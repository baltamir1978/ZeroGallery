/*
 * SPDX-License-Identifier: MIT
 */

#include "media_player.h"

#include "gallery.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csetjmp>
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

// jpeglib.h no trae sus propios extern "C" en todas las versiones.
extern "C" {
#include <jpeglib.h>
}

namespace platform {
namespace {

// 15 fotogramas por segundo: en 320×170 se ve fluido y deja a la CM0 (cuatro núcleos a
// 1 GHz, sin decodificación por hardware aquí) margen para vídeos de móvil a 1080p.
constexpr int kVideoFps = 15;
constexpr size_t kFrameBytes = kCanvasPixels * sizeof(uint16_t);
constexpr int kExitExecFailed = 127;

uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

const char* env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' ? value : fallback;
}

// ---------------------------------------------------------------- JPEG

struct JpegError {
    jpeg_error_mgr base;
    std::jmp_buf jump;
};

[[noreturn]] void jpeg_error_exit(j_common_ptr info) {
    auto* error = reinterpret_cast<JpegError*>(info->err);
    std::longjmp(error->jump, 1);
}

void jpeg_silent(j_common_ptr, int) {}

/**
 * Decodifica el JPEG ya reducido (1/2, 1/4 o 1/8, que libjpeg hace casi gratis en la DCT)
 * hasta lo justo para cubrir la pantalla. `pixels` es RGB de 8 bits, tal como está
 * guardado, sin enderezar.
 *
 * Con setjmp no puede haber objetos con destructor entre medias: por eso el vector viene
 * de fuera y aquí sólo hay punteros y enteros.
 */
bool decode_jpeg(std::FILE* file, std::vector<uint8_t>& pixels, int& width, int& height, int& orientation) {
    jpeg_decompress_struct info{};
    JpegError error{};
    info.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    error.base.emit_message = jpeg_silent;

    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&info);
        return false;
    }

    jpeg_create_decompress(&info);
    jpeg_stdio_src(&info, file);
    jpeg_save_markers(&info, JPEG_APP0 + 1, 0xFFFF);
    jpeg_read_header(&info, TRUE);

    orientation = 1;
    for (auto* marker = info.marker_list; marker; marker = marker->next) {
        if (marker->marker == JPEG_APP0 + 1) {
            const int found = gallery::exif_orientation(marker->data, marker->data_length);
            if (found != 1) {
                orientation = found;
                break;
            }
        }
    }

    const bool swap = gallery::orientation_swaps_axes(orientation);
    const int shown_width = swap ? static_cast<int>(info.image_height) : static_cast<int>(info.image_width);
    const int shown_height = swap ? static_cast<int>(info.image_width) : static_cast<int>(info.image_height);
    const auto fit = gallery::fit_rect(shown_width, shown_height, kCanvasWidth, kCanvasHeight);
    const int need_width = swap ? fit.height : fit.width;
    const int need_height = swap ? fit.width : fit.height;

    int denominator = 8;
    while (denominator > 1 &&
           (static_cast<int>(info.image_width) / denominator < need_width ||
            static_cast<int>(info.image_height) / denominator < need_height)) {
        denominator /= 2;
    }
    info.scale_num = 1;
    info.scale_denom = static_cast<unsigned>(denominator);
    info.out_color_space = JCS_RGB;
    info.dct_method = JDCT_IFAST;
    info.do_fancy_upsampling = FALSE;

    jpeg_start_decompress(&info);
    width = static_cast<int>(info.output_width);
    height = static_cast<int>(info.output_height);
    if (info.output_components != 3 || width <= 0 || height <= 0) {
        jpeg_destroy_decompress(&info);
        return false;
    }

    pixels.resize(static_cast<size_t>(width) * height * 3);
    while (info.output_scanline < info.output_height) {
        JSAMPROW row = pixels.data() + static_cast<size_t>(info.output_scanline) * width * 3;
        jpeg_read_scanlines(&info, &row, 1);
    }
    jpeg_finish_decompress(&info);
    jpeg_destroy_decompress(&info);
    return true;
}

/** Endereza según EXIF, encaja en la pantalla promediando píxeles y deja el resto en negro. */
void render_to_canvas(const std::vector<uint8_t>& pixels,
                      int width,
                      int height,
                      int orientation,
                      std::vector<uint16_t>& canvas) {
    canvas.assign(kCanvasPixels, 0);
    const bool swap = gallery::orientation_swaps_axes(orientation);
    const int shown_width = swap ? height : width;
    const int shown_height = swap ? width : height;
    const auto fit = gallery::fit_rect(shown_width, shown_height, kCanvasWidth, kCanvasHeight);

    for (int y = 0; y < fit.height; ++y) {
        const int y0 = static_cast<int>(static_cast<long long>(y) * shown_height / fit.height);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(y + 1) * shown_height / fit.height));
        auto* out = canvas.data() + static_cast<size_t>(fit.y + y) * kCanvasWidth + fit.x;
        for (int x = 0; x < fit.width; ++x) {
            const int x0 = static_cast<int>(static_cast<long long>(x) * shown_width / fit.width);
            const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<long long>(x + 1) * shown_width / fit.width));
            unsigned r = 0;
            unsigned g = 0;
            unsigned b = 0;
            unsigned count = 0;
            for (int sy = y0; sy < y1 && sy < shown_height; ++sy) {
                for (int sx = x0; sx < x1 && sx < shown_width; ++sx) {
                    int stored_x = 0;
                    int stored_y = 0;
                    gallery::oriented_to_stored(orientation, width, height, sx, sy, stored_x, stored_y);
                    const auto* pixel = pixels.data() + (static_cast<size_t>(stored_y) * width + stored_x) * 3;
                    r += pixel[0];
                    g += pixel[1];
                    b += pixel[2];
                    ++count;
                }
            }
            if (count) {
                out[x] = rgb565(r / count, g / count, b / count);
            }
        }
    }
}

bool load_jpeg(const std::string& path, std::vector<uint16_t>& canvas) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int orientation = 1;
    const bool ok = decode_jpeg(file, pixels, width, height, orientation);
    std::fclose(file);
    if (ok) {
        render_to_canvas(pixels, width, height, orientation, canvas);
    }
    return ok;
}

// ---------------------------------------------------------------- ffmpeg

/** Un `ffmpeg` en marcha que escribe fotogramas en crudo por una tubería. */
class Ffmpeg {
public:
    Ffmpeg() = default;
    ~Ffmpeg() { stop(); }

    Ffmpeg(const Ffmpeg&) = delete;
    Ffmpeg& operator=(const Ffmpeg&) = delete;

    bool start(const std::vector<std::string>& arguments) {
        stop();
        int ends[2];
        if (pipe2(ends, O_CLOEXEC) != 0) {
            return false;
        }

        // argv se prepara antes del fork: entre fork y exec, en un proceso con hilos,
        // no se puede reservar memoria.
        const std::string program = env_or("ZEROGALLERY_FFMPEG", "ffmpeg");
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
            dup2(ends[1], STDOUT_FILENO);
            const int null = ::open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, STDIN_FILENO);
                dup2(null, STDERR_FILENO);
            }
            execvp(argv[0], argv.data());
            _exit(kExitExecFailed);
        }

        ::close(ends[1]);
        pid_ = pid;
        fd_ = ends[0];
        exit_code_ = -1;
        return true;
    }

    /**
     * Lee hasta `size` bytes esperando como mucho `timeout_ms`.
     * Devuelve los leídos, 0 si no ha llegado nada todavía y -1 si ffmpeg ha terminado.
     */
    long read_some(uint8_t* buffer, size_t size, int timeout_ms) {
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

    void stop() {
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

    /** Recoge el código de salida cuando ffmpeg ha terminado por su cuenta. */
    int finish() {
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

private:
    pid_t pid_{-1};
    int fd_{-1};
    int exit_code_{-1};
};

std::string canvas_filter(bool video) {
    char filter[256];
    std::snprintf(filter,
                  sizeof(filter),
                  "%s%sscale=%d:%d:force_original_aspect_ratio=decrease:flags=%s,"
                  "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black",
                  video ? "fps=" : "",
                  video ? (std::to_string(kVideoFps) + ",").c_str() : "",
                  kCanvasWidth,
                  kCanvasHeight,
                  video ? "fast_bilinear" : "area",
                  kCanvasWidth,
                  kCanvasHeight);
    return filter;
}

std::vector<std::string> still_arguments(const std::string& path) {
    // Sin -map: en un HEIC de móvil la imagen es una rejilla de teselas y ffmpeg sólo la
    // recompone si se le deja elegir a él.
    return {"-nostdin", "-hide_banner", "-loglevel", "error", "-i", path, "-frames:v", "1",
            "-vf", canvas_filter(false), "-pix_fmt", "rgb565le", "-f", "rawvideo", "pipe:1"};
}

/**
 * `audio`: formato de salida de ffmpeg («pulse» o «alsa»), o vacío para no sacar sonido.
 * `loop`: repetir sin fin (animaciones).
 */
std::vector<std::string> video_arguments(const std::string& path,
                                         double position,
                                         const std::string& audio,
                                         bool loop) {
    std::vector<std::string> arguments = {"-nostdin", "-hide_banner", "-loglevel", "error",
                                          // A 320×170 no se nota y ahorra un buen pico de CPU en H.264.
                                          "-skip_loop_filter", "all",
                                          // -re: a velocidad real; sin él ffmpeg decodifica todo lo deprisa que puede.
                                          "-re"};
    if (loop) {
        arguments.insert(arguments.end(), {"-stream_loop", "-1"});
    }
    if (position > 0.05) {
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.3f", position);
        arguments.insert(arguments.end(), {"-ss", seconds});
    }
    arguments.insert(arguments.end(), {"-i", path, "-an", "-vf", canvas_filter(true), "-pix_fmt", "rgb565le",
                                       "-f", "rawvideo", "pipe:1"});
    if (!audio.empty()) {
        // Segunda salida del mismo proceso: al salir las dos del mismo reloj, imagen y
        // sonido van a la par sin tener que sincronizar nada aquí.
        // En «pulse» lo último es el nombre con que aparece en el mezclador; en «alsa»,
        // el dispositivo.
        arguments.insert(arguments.end(),
                         {"-vn", "-ac", "2", "-f", audio, audio == "pulse" ? "Gallery" : "default"});
    }
    return arguments;
}

void to_canvas(const std::vector<uint8_t>& bytes, std::vector<uint16_t>& canvas) {
    canvas.resize(kCanvasPixels);
    std::memcpy(canvas.data(), bytes.data(), kFrameBytes);
}

} // namespace

MediaPlayer::MediaPlayer() : frame_(kCanvasPixels, 0) {
    // Lanzada desde un servicio puede faltar XDG_RUNTIME_DIR, y sin él ffmpeg no encuentra
    // el servidor de sonido. Se pone aquí, antes de que exista el hilo.
    const std::string runtime = "/run/user/" + std::to_string(getuid());
    if (access(runtime.c_str(), F_OK) == 0) {
        setenv("XDG_RUNTIME_DIR", runtime.c_str(), 0);
    }
    worker_ = std::thread([this] { run(); });
}

MediaPlayer::~MediaPlayer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        ++request_.generation;
    }
    wake_.notify_all();
    worker_.join();
}

void MediaPlayer::open(const std::string& path, MediaKind kind) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++request_.generation;
        request_.path = path;
        request_.kind = kind;
        frame_fresh_ = false;
        error_.clear();
        state_ = path.empty() ? MediaState::Idle : MediaState::Loading;
    }
    wake_.notify_all();
}

void MediaPlayer::close() {
    open({}, MediaKind::Still);
}

void MediaPlayer::toggle_pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++toggles_;
    }
    wake_.notify_all();
}

bool MediaPlayer::take_frame(uint16_t* destination) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_fresh_) {
        return false;
    }
    std::memcpy(destination, frame_.data(), kFrameBytes);
    frame_fresh_ = false;
    return true;
}

MediaState MediaPlayer::state() const {
    return state_;
}

std::string MediaPlayer::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

bool MediaPlayer::superseded(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return request_.generation != generation;
}

void MediaPlayer::publish(const std::vector<uint16_t>& frame, uint64_t generation, MediaState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request_.generation != generation) {
        return; // llegó tarde: ya se ha pedido otra cosa
    }
    frame_ = frame;
    frame_fresh_ = true;
    state_ = state;
}

void MediaPlayer::fail(uint64_t generation, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request_.generation != generation) {
        return;
    }
    error_ = message;
    state_ = MediaState::Error;
}

bool MediaPlayer::wait_for_toggle(uint64_t generation, uint64_t& toggles_seen) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return request_.generation != generation || toggles_ != toggles_seen; });
    if (request_.generation != generation) {
        return false;
    }
    toggles_seen = toggles_;
    return true;
}

void MediaPlayer::run() {
    uint64_t handled = 0;
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || request_.generation != handled; });
            if (quit_) {
                return;
            }
            request = request_;
            handled = request.generation;
        }
        if (request.path.empty()) {
            continue;
        }
        if (request.kind == MediaKind::Still) {
            show_still(request);
        }
        else {
            play_video(request);
        }
    }
}

void MediaPlayer::show_still(const Request& request) {
    std::vector<uint16_t> canvas;
    if (gallery::is_jpeg(request.path) && load_jpeg(request.path, canvas)) {
        publish(canvas, request.generation, MediaState::Still);
        return;
    }

    // Lo que libjpeg no abre (otros formatos, o un JPEG en CMYK) se le deja a ffmpeg.
    Ffmpeg ffmpeg;
    if (!ffmpeg.start(still_arguments(request.path))) {
        fail(request.generation, "Cannot run ffmpeg");
        return;
    }
    std::vector<uint8_t> bytes(kFrameBytes);
    size_t filled = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (filled < kFrameBytes) {
        if (superseded(request.generation)) {
            return;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            break;
        }
        const long count = ffmpeg.read_some(bytes.data() + filled, kFrameBytes - filled, 100);
        if (count < 0) {
            break;
        }
        filled += static_cast<size_t>(count);
    }
    if (filled < kFrameBytes) {
        const bool missing = filled == 0 && ffmpeg.finish() == kExitExecFailed;
        fail(request.generation, missing ? "ffmpeg is not installed" : "Cannot open this image");
        return;
    }
    to_canvas(bytes, canvas);
    publish(canvas, request.generation, MediaState::Still);
}

/** Hay alguna tarjeta de sonido. Sin ninguna, ni se intenta sacar audio por ALSA. */
static bool has_sound_card() {
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

/**
 * Por dónde sacar el sonido. El Cardputer Zero lleva PipeWire (con su servidor compatible
 * con PulseAudio), que es quien decide si suena el altavoz, los auriculares o un altavoz
 * Bluetooth y quien aplica el volumen del sistema: si está, se le entrega a él y no se
 * elige salida aquí. ALSA a pelo queda para sistemas sin PipeWire.
 * `ZEROGALLERY_AUDIO=none|pulse|alsa` lo fuerza.
 */
static std::string audio_output() {
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

void MediaPlayer::play_video(const Request& request) {
    const bool loop = request.kind == MediaKind::Animation;
    std::string audio = loop ? std::string{} : audio_output();

    uint64_t toggles_seen = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        toggles_seen = toggles_;
    }

    double position = 0.0;
    std::vector<uint8_t> bytes(kFrameBytes);
    std::vector<uint16_t> canvas;

    // Cada vuelta es un ffmpeg: al empezar, al reanudar tras una pausa (desde donde se
    // quedó) y al repetir. Pausar es parar el proceso: con -re, congelarlo y soltarlo
    // después lo haría correr para recuperar el tiempo perdido.
    for (;;) {
        Ffmpeg ffmpeg;
        if (!ffmpeg.start(video_arguments(request.path, position, audio, loop))) {
            fail(request.generation, "Cannot run ffmpeg");
            return;
        }

        size_t filled = 0;
        long frames = 0;
        bool paused = false;
        for (;;) {
            if (superseded(request.generation)) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (toggles_ != toggles_seen) {
                    toggles_seen = toggles_;
                    paused = true;
                }
            }
            if (paused) {
                break;
            }
            const long count = ffmpeg.read_some(bytes.data() + filled, kFrameBytes - filled, 100);
            if (count < 0) {
                break;
            }
            filled += static_cast<size_t>(count);
            if (filled == kFrameBytes) {
                filled = 0;
                ++frames;
                to_canvas(bytes, canvas);
                publish(canvas, request.generation, MediaState::Playing);
            }
        }

        // Una animación no sabe por dónde iba (se repite): al reanudar empieza otra vez.
        position = loop ? 0.0 : position + static_cast<double>(frames) / kVideoFps;

        if (paused) {
            ffmpeg.stop();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (request_.generation == request.generation) {
                    state_ = MediaState::Paused;
                }
            }
            if (!wait_for_toggle(request.generation, toggles_seen)) {
                return;
            }
            continue;
        }

        const int exit_code = ffmpeg.finish();
        if (exit_code == kExitExecFailed) {
            fail(request.generation, "ffmpeg is not installed");
            return;
        }
        if (exit_code != 0 && !audio.empty()) {
            // Con sonido ha fallado: lo normal es que el vídeo no tenga pista de audio o
            // que la salida de sonido no se deje abrir (a veces tras los primeros
            // fotogramas). Se sigue sólo con la imagen desde donde iba.
            audio.clear();
            continue;
        }
        if (frames == 0 && position <= 0.0) {
            fail(request.generation, "Cannot play this video");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (request_.generation == request.generation) {
                state_ = MediaState::Ended;
            }
        }
        if (!wait_for_toggle(request.generation, toggles_seen)) {
            return;
        }
        position = 0.0;
    }
}

} // namespace platform
