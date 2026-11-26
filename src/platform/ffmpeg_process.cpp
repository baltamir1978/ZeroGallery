/*
 * SPDX-License-Identifier: MIT
 */

#include "ffmpeg_process.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace platform {

const char* env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' ? value : fallback;
}

bool FfmpegProcess::start(const std::vector<std::string>& arguments, const char* tool, int stdin_fd, int fd3) {
    stop();
    int ends[2];
    if (pipe2(ends, O_CLOEXEC) != 0) {
        return false;
    }

    // argv se prepara antes del fork: entre fork y exec, en un proceso con hilos,
    // no se puede reservar memoria.
    const std::string program = std::strcmp(tool, "ffmpeg") == 0 ? env_or("ZEROGALLERY_FFMPEG", "ffmpeg") : tool;
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& argument : arguments) {
        argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t parent = getpid();
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(ends[0]);
        ::close(ends[1]);
        return false;
    }
    if (pid == 0) {
#ifdef __linux__
        // Si la galería muere, ffmpeg no debe quedarse sonando de fondo.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) {
            _exit(kExitExecFailed);
        }
#endif
        if (fd3 >= 0) {
            // dup2 a otro número quita el O_CLOEXEC; si ya es el 3, hay que quitarlo a mano.
            if (fd3 == 3) {
                fcntl(3, F_SETFD, 0);
            }
            else {
                dup2(fd3, 3);
            }
        }
        dup2(ends[1], STDOUT_FILENO);
        const int null = ::open("/dev/null", O_RDWR);
        if (stdin_fd >= 0) {
            dup2(stdin_fd, STDIN_FILENO);
        }
        else if (null >= 0) {
            dup2(null, STDIN_FILENO);
        }
        if (null >= 0) {
            dup2(null, STDERR_FILENO);
        }
        // Lo demás que tenga abierto la galería no es cosa de ffmpeg: el framebuffer que abre
        // LVGL sin O_CLOEXEC le llegaba abierto (dos veces).
        close_range(fd3 >= 0 ? 4 : 3, ~0U, 0);
        execvp(argv[0], argv.data());
        _exit(kExitExecFailed);
    }

    ::close(ends[1]);
    pid_ = pid;
    fd_ = ends[0];
    exit_code_ = -1;
    return true;
}

long FfmpegProcess::read_some(uint8_t* buffer, size_t size, int timeout_ms) {
    if (fd_ < 0) {
        return -1;
    }
    pollfd waiting{fd_, POLLIN, 0};
    const int ready = poll(&waiting, 1, timeout_ms);
    if (ready == 0 || (ready < 0 && errno == EINTR)) {
        return 0;
    }
    if (ready < 0) {
        return -1;
    }
    const auto count = ::read(fd_, buffer, size);
    if (count > 0) {
        return static_cast<long>(count);
    }
    if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
        return 0;
    }
    return -1;
}

void FfmpegProcess::stop() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (pid_ > 0) {
        // SIGKILL y no SIGTERM: no hay nada que ffmpeg tenga que guardar y así el
        // cambio de vídeo es inmediato.
        kill(pid_, SIGKILL);
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        pid_ = -1;
    }
}

int FfmpegProcess::finish() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (pid_ > 0) {
        int status = 0;
        while (waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        pid_ = -1;
    }
    return exit_code_;
}

namespace {

/** Hay alguna tarjeta de sonido. Sin ninguna, ni se intenta sacar audio por ALSA. */
bool has_sound_card() {
    std::FILE* cards = std::fopen("/proc/asound/cards", "r");
    if (!cards) {
        return false;
    }
    char line[128];
    bool found = false;
    while (std::fgets(line, sizeof(line), cards)) {
        if (std::strchr(line, '[')) { // «0 [Headphones ]: ...»
            found = true;
            break;
        }
    }
    std::fclose(cards);
    return found;
}

} // namespace

std::string audio_output() {
    const std::string forced = env_or("ZEROGALLERY_AUDIO", "");
    if (forced == "none") {
        return {};
    }
    if (forced == "pulse" || forced == "alsa") {
        return forced;
    }

    std::string runtime = env_or("XDG_RUNTIME_DIR", "");
    if (runtime.empty()) {
        runtime = "/run/user/" + std::to_string(getuid());
    }
    if (access((runtime + "/pulse/native").c_str(), F_OK) == 0) {
        return "pulse";
    }
    return has_sound_card() ? "alsa" : "";
}

std::string probe_text(const std::vector<std::string>& arguments) {
    return command_text("ffprobe", arguments);
}

std::string command_text(const char* tool, const std::vector<std::string>& arguments) {
    FfmpegProcess probe;
    if (!probe.start(arguments, tool)) {
        return {};
    }
    std::string text;
    char chunk[512];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (text.size() < 4096 && std::chrono::steady_clock::now() < deadline) {
        const long count = probe.read_some(reinterpret_cast<uint8_t*>(chunk), sizeof(chunk), 100);
        if (count < 0) {
            break;
        }
        text.append(chunk, static_cast<size_t>(count));
    }
    return text;
}

} // namespace platform
