/*
 * SPDX-License-Identifier: MIT
 */

#include "hdmi_player.h"

#include "audio_route.h"
#include "dlna.h"
#include "ffmpeg_process.h"
#include "gallery.h"
#include "hdmi.h"
#include "kms_display.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <vector>

namespace platform {
namespace {

// Por encima de 720p se salta el filtro antibloques de H.264 (como en la pantalla del
// aparato): a 1080p es lo que deja margen para ir a velocidad real.
constexpr long long kSkipLoopFilterPixels = 1280LL * 720;

std::string seconds_text(double seconds) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", seconds);
    return text;
}

std::string size_text(int width, int height) {
    return std::to_string(width) + ":" + std::to_string(height);
}

/** Orientación EXIF de un JPEG (1 si no es JPEG o no la lleva), del principio del fichero. */
int jpeg_orientation(const std::string& path) {
    if (!gallery::is_jpeg(path)) {
        return 1;
    }
    std::ifstream file(path, std::ios::binary);
    std::vector<char> head(256 * 1024);
    file.read(head.data(), static_cast<std::streamsize>(head.size()));
    dlna::JpegInfo info;
    if (!dlna::parse_jpeg(reinterpret_cast<const uint8_t*>(head.data()), static_cast<size_t>(file.gcount()), info)) {
        return 1;
    }
    return info.orientation;
}

/**
 * La foto al tamaño de la pantalla, con bandas negras. Un JPEG se endereza aquí según su
 * EXIF (-noautorotate, como al mandarlo a la tele); el resto (HEIC de móvil) lo endereza
 * ffmpeg solo.
 */
std::vector<std::string> still_arguments(const std::string& path, int orientation, int width, int height) {
    std::string filter = dlna::orientation_filter(orientation);
    if (!filter.empty()) {
        filter += ",";
    }
    filter += "scale=" + size_text(width, height) +
              ":force_original_aspect_ratio=decrease:force_divisible_by=2:flags=bicubic,"
              "pad=" + size_text(width, height) + ":(ow-iw)/2:(oh-ih)/2:black,format=yuv420p";
    std::vector<std::string> arguments = {"-nostdin", "-hide_banner", "-loglevel", "error"};
    if (orientation != 1) {
        arguments.push_back("-noautorotate");
    }
    // Sin -map: un HEIC de móvil es una rejilla de teselas que ffmpeg sólo recompone si elige él.
    arguments.insert(arguments.end(), {"-i", path, "-frames:v", "1", "-vf", filter, "-f", "rawvideo", "pipe:1"});
    return arguments;
}

// Sin el primer fotograma en este tiempo, el decodificador por hardware se da por colgado
// (con poca memoria de GPU tardaba 28 s en fallar) y se sigue por software.
constexpr auto kHardwareFirstFrame = std::chrono::seconds(6);

/** Memoria de la GPU en MB según `vcgencmd`; -1 si no se puede preguntar. */
int gpu_memory_mb() {
    FfmpegProcess vcgencmd;
    if (!vcgencmd.start({"get_mem", "gpu"}, "vcgencmd")) {
        return -1;
    }
    char text[64] = {};
    size_t filled = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (filled < sizeof(text) - 1 && std::chrono::steady_clock::now() < deadline) {
        const long count = vcgencmd.read_some(reinterpret_cast<uint8_t*>(text) + filled, sizeof(text) - 1 - filled, 100);
        if (count < 0) {
            break;
        }
        filled += static_cast<size_t>(count);
    }
    return hdmi::parse_gpu_mem(text);
}

/**
 * Si está el decodificador H.264 por hardware: el dispositivo (`bcm2835-codec-decode`) y
 * memoria de GPU para él. Con 32 MB el dispositivo aparece pero no funciona, así que también
 * se pregunta la memoria (si no se puede, vale con el dispositivo: el tiempo máximo del primer
 * fotograma lo cubre). Se mira una vez: cambiar gpu_mem pide reiniciar.
 * `ZEROGALLERY_HWDEC=0` lo desactiva.
 */
bool hardware_decoder_available() {
    static const bool available = [] {
        if (std::string(env_or("ZEROGALLERY_HWDEC", "1")) == "0") {
            return false;
        }
        bool device = false;
        for (int i = 0; i < 64 && !device; ++i) {
            std::ifstream name("/sys/class/video4linux/video" + std::to_string(i) + "/name");
            std::string text;
            device = std::getline(name, text) && text.find("codec-decode") != std::string::npos;
        }
        if (!device) {
            return false;
        }
        const int memory = gpu_memory_mb();
        return memory < 0 || memory >= hdmi::kHardwareDecoderGpuMem;
    }();
    return available;
}

std::string video_filter(int width, int height, double fps, double source_fps) {
    std::string filter;
    if (fps > 0.0 && source_fps > fps + 0.5) {
        filter = "fps=" + std::to_string(static_cast<int>(fps)) + ",";
    }
    return filter + "scale=" + size_text(width, height) + ":flags=bilinear,format=yuv420p";
}

} // namespace

HdmiPlayer::HdmiPlayer() {
    worker_ = std::thread([this] { run(); });
}

HdmiPlayer::~HdmiPlayer() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
        ++request_.generation;
    }
    wake_.notify_all();
    worker_.join();
}

bool HdmiPlayer::available() {
    return !KmsDisplay::connected_card().empty();
}

void HdmiPlayer::open(const std::string& path, MediaKind kind) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++request_.generation;
        request_.path = path;
        request_.kind = kind;
        seek_ = 0.0;
        position_ = 0.0;
        duration_ = 0.0;
        error_.clear();
        state_ = path.empty() ? MediaState::Idle : MediaState::Loading;
    }
    wake_.notify_all();
}

void HdmiPlayer::toggle_pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++toggles_;
    }
    wake_.notify_all();
}

void HdmiPlayer::seek(double seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_.path.empty() || request_.kind != MediaKind::Video) {
            return;
        }
        seek_ += seconds;
    }
    wake_.notify_all();
}

double HdmiPlayer::position() const {
    return position_;
}

double HdmiPlayer::duration() const {
    return duration_;
}

MediaState HdmiPlayer::state() const {
    return state_;
}

std::string HdmiPlayer::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::string HdmiPlayer::name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return name_;
}

bool HdmiPlayer::superseded(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    return request_.generation != generation;
}

void HdmiPlayer::set_state(uint64_t generation, MediaState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request_.generation == generation) {
        state_ = state;
    }
}

void HdmiPlayer::fail(uint64_t generation, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request_.generation != generation) {
        return;
    }
    error_ = message;
    state_ = MediaState::Error;
}

HdmiPlayer::Event HdmiPlayer::wait_for_event(uint64_t generation, uint64_t& toggles_seen, double& seek) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return request_.generation != generation || toggles_ != toggles_seen || seek_ != 0.0; });
    if (request_.generation != generation) {
        return Event::Superseded;
    }
    if (toggles_ != toggles_seen) {
        toggles_seen = toggles_;
        return Event::Toggled;
    }
    seek = seek_;
    seek_ = 0.0;
    return Event::Sought;
}

void HdmiPlayer::run() {
    // La pantalla sólo la toca este hilo: se crea aquí y se apaga al salir de él.
    KmsDisplay screen;
    display_ = &screen;
    uint64_t handled = 0;
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || request_.generation != handled; });
            if (quit_) {
                break;
            }
            request = request_;
            handled = request.generation;
        }
        if (request.path.empty()) {
            screen.blank();
            continue;
        }
        if (!ensure_display(request.generation)) {
            continue;
        }
        if (request.kind == MediaKind::Still) {
            show_still(request);
        }
        else {
            play_video(request);
        }
    }
    screen.close();
    display_ = nullptr;
}

bool HdmiPlayer::ensure_display(uint64_t generation) {
    if (display_->is_open()) {
        return true;
    }
    std::string error;
    if (!display_->open(error)) {
        fail(generation, error);
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    name_ = "HDMI " + std::to_string(display_->width()) + "x" + std::to_string(display_->height());
    return true;
}

/** Lee un fotograma entero; `false` si ffmpeg acaba antes, tarda más de medio minuto o se pide otra cosa. */
static bool read_frame(FfmpegProcess& ffmpeg, std::vector<uint8_t>& frame, size_t& filled,
                       const std::function<bool()>& abandoned) {
    filled = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (filled < frame.size()) {
        if (abandoned() || std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        const long count = ffmpeg.read_some(frame.data() + filled, frame.size() - filled, 100);
        if (count < 0) {
            return false;
        }
        filled += static_cast<size_t>(count);
    }
    return true;
}

void HdmiPlayer::show_still(const Request& request) {
    // Pares: YUV 4:2:0 (una pantalla de 1366 de ancho pierde una columna).
    const int width = display_->width() & ~1;
    const int height = display_->height() & ~1;
    FfmpegProcess ffmpeg;
    if (!ffmpeg.start(still_arguments(request.path, jpeg_orientation(request.path), width, height))) {
        fail(request.generation, "Cannot run ffmpeg");
        return;
    }
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 3 / 2);
    size_t filled = 0;
    if (!read_frame(ffmpeg, frame, filled, [&] { return superseded(request.generation); })) {
        if (superseded(request.generation)) {
            return;
        }
        const bool missing = filled == 0 && ffmpeg.finish() == kExitExecFailed;
        fail(request.generation, missing ? "ffmpeg is not installed" : "Cannot open this image");
        return;
    }
    if (superseded(request.generation)) {
        return;
    }
    if (!display_->show(frame.data(), width, height, true)) {
        fail(request.generation, "HDMI: cannot show the image");
        return;
    }
    set_state(request.generation, MediaState::Still);
}

bool HdmiPlayer::show_video_frame(const Request& request, double seconds, int width, int height) {
    FfmpegProcess ffmpeg;
    if (!ffmpeg.start({"-nostdin", "-hide_banner", "-loglevel", "error", "-ss", seconds_text(seconds), "-i",
                       request.path, "-an", "-frames:v", "1", "-vf", video_filter(width, height, 0, 0), "-f",
                       "rawvideo", "pipe:1"})) {
        return false;
    }
    std::vector<uint8_t> frame(static_cast<size_t>(width) * height * 3 / 2);
    size_t filled = 0;
    if (!read_frame(ffmpeg, frame, filled, [&] { return superseded(request.generation); })) {
        return false;
    }
    return display_->show(frame.data(), width, height, true);
}

void HdmiPlayer::play_video(const Request& request) {
    const bool loop = request.kind == MediaKind::Animation;
    bool audio_off = loop;
    uint32_t audio_seen = audio_generation();

    const auto info = hdmi::parse_probe(probe_text(
        {"-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=codec_name,width,height,r_frame_rate,avg_frame_rate:stream_side_data=rotation:format=duration", "-of",
         "default=nw=1", request.path}));
    // Con el decodificador por hardware la CPU sólo mueve los fotogramas (75 % de un núcleo
    // con un 1080p30, frente a casi dos por software). Si falla con un vídeo, se sigue por
    // software.
    bool hardware = hardware_decoder_available() && hdmi::hardware_decodable(info);
    const auto fps_for = [&](bool with_hardware) {
        const double limit = hdmi::max_fps(with_hardware, info.width, info.height);
        return info.fps > 0.0 ? std::min(info.fps, limit) : 25.0;
    };
    // Con hardware, a su tamaño (reducir con la CPU sería lentísimo: lo hace la GPU); por
    // software, reducido para que quepa, que cuesta menos que pasar el fotograma entero.
    const auto size_for = [&](bool with_hardware) {
        return with_hardware ? hdmi::hardware_size(info)
                             : hdmi::video_size(info.width, info.height, display_->width(), display_->height());
    };
    double fps = fps_for(hardware);
    hdmi::Size size = size_for(hardware);
    const bool skip_loop_filter = static_cast<long long>(info.width) * info.height > kSkipLoopFilterPixels;
    if (!loop) {
        duration_ = info.duration;
    }
    size_t frame_bytes = static_cast<size_t>(size.width) * size.height * 3 / 2;
    std::vector<uint8_t> frame(frame_bytes);

    uint64_t toggles_seen = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        toggles_seen = toggles_;
    }
    // Ni antes del principio ni, hacia delante, a menos de un segundo del final.
    const auto landing = [&](double from, double seek) {
        double to = std::max(0.0, from + seek);
        if (seek > 0.0 && info.duration > 0.0) {
            to = std::min(to, std::max(from, info.duration - 1.0));
        }
        return to;
    };

    double position = 0.0;
    for (;;) {
        std::vector<std::string> arguments = {"-nostdin", "-hide_banner", "-loglevel", "error"};
        if (hardware) {
            arguments.insert(arguments.end(), {"-c:v", "h264_v4l2m2m"});
        }
        else if (skip_loop_filter) {
            arguments.insert(arguments.end(), {"-skip_loop_filter", "all"});
        }
        arguments.push_back("-re");
        if (loop) {
            arguments.insert(arguments.end(), {"-stream_loop", "-1"});
        }
        if (position > 0.05) {
            arguments.insert(arguments.end(), {"-ss", seconds_text(position)});
        }
        arguments.insert(arguments.end(), {"-i", request.path, "-an", "-vf",
                                           video_filter(size.width, size.height, fps, info.fps), "-f", "rawvideo",
                                           "pipe:1"});
        // Del mismo proceso, como en la pantalla del aparato: imagen y sonido a la par.
        audio_seen = audio_generation();
        HdmiAudioPipe hdmi_audio;
        const bool to_hdmi = !audio_off && current_audio_output() == audio::Output::Hdmi;
        if (to_hdmi && !hdmi_audio.start()) {
            audio_off = true;
        }
        const auto audio = audio_off ? std::vector<std::string>{} : audio_arguments();
        arguments.insert(arguments.end(), audio.begin(), audio.end());

        FfmpegProcess ffmpeg;
        const bool launched = ffmpeg.start(arguments, "ffmpeg", -1, audio_off ? -1 : hdmi_audio.ffmpeg_fd());
        hdmi_audio.release_ffmpeg_end();
        if (!launched) {
            fail(request.generation, "Cannot run ffmpeg");
            return;
        }

        size_t filled = 0;
        long frames = 0;
        bool paused = false;
        bool stalled = false;
        bool restart = false;
        double sought = 0.0;
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            if (superseded(request.generation)) {
                return;
            }
            if (hardware && frames == 0 && std::chrono::steady_clock::now() - started > kHardwareFirstFrame) {
                stalled = true;
                break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (toggles_ != toggles_seen) {
                    toggles_seen = toggles_;
                    paused = true;
                }
                if (seek_ != 0.0) {
                    sought = seek_;
                    seek_ = 0.0;
                }
            }
            if (audio_generation() != audio_seen) {
                // Otra salida de sonido: se relanza desde donde iba.
                audio_seen = audio_generation();
                audio_off = loop;
                restart = true;
            }
            if (paused || sought != 0.0 || restart) {
                break;
            }
            const long count = ffmpeg.read_some(frame.data() + filled, frame_bytes - filled, 100);
            if (count < 0) {
                break;
            }
            filled += static_cast<size_t>(count);
            if (filled == frame_bytes) {
                filled = 0;
                ++frames;
                display_->show(frame.data(), size.width, size.height);
                if (frames == 1) {
                    set_state(request.generation, MediaState::Playing);
                }
                position_ = position + static_cast<double>(frames) / fps;
            }
        }

        if (stalled) {
            // El decodificador por hardware no da nada: por software desde donde iba.
            ffmpeg.stop();
            hardware = false;
            fps = fps_for(false);
            size = size_for(false);
            frame_bytes = static_cast<size_t>(size.width) * size.height * 3 / 2;
            frame.assign(frame_bytes, 0);
            continue;
        }
        position = loop ? 0.0 : position + static_cast<double>(frames) / fps;
        if (sought != 0.0) {
            position = landing(position, sought);
        }
        position_ = position;

        if ((sought != 0.0 || restart) && !paused) {
            ffmpeg.stop();
            continue;
        }

        bool ended = false;
        if (!paused) {
            const int exit_code = ffmpeg.finish();
            if (exit_code == kExitExecFailed) {
                fail(request.generation, "ffmpeg is not installed");
                return;
            }
            if (exit_code != 0 && frames == 0 && hardware) {
                // El decodificador por hardware no ha podido con este vídeo: por software.
                hardware = false;
                fps = fps_for(false);
                size = size_for(false);
                frame_bytes = static_cast<size_t>(size.width) * size.height * 3 / 2;
                frame.assign(frame_bytes, 0);
                continue;
            }
            if (exit_code != 0 && !audio_off) {
                // Sin pista de sonido, o la salida de sonido no se deja abrir: sólo imagen.
                audio_off = true;
                continue;
            }
            if (frames == 0 && position <= 0.0) {
                fail(request.generation, "Cannot play this video");
                return;
            }
            ended = true;
        }
        ffmpeg.stop();

        // Parado: en la pantalla se queda el último fotograma (el plano no se toca).
        set_state(request.generation, ended ? MediaState::Ended : MediaState::Paused);
        double at = ended ? std::max(0.0, position - 1.0 / fps) : position;
        bool resume = false;
        while (!resume) {
            double seek = 0.0;
            const Event event = wait_for_event(request.generation, toggles_seen, seek);
            if (event == Event::Superseded) {
                return;
            }
            if (event == Event::Toggled) {
                // Reanudar, o volver a empezar si había acabado.
                position = ended ? 0.0 : at;
                resume = true;
                break;
            }
            if (ended) {
                if (seek < 0.0) {
                    position = std::max(0.0, at + seek);
                    resume = true;
                }
                continue;
            }
            at = landing(at, seek);
            position_ = at;
            show_video_frame(request, at, size.width, size.height);
        }
        position_ = position;
        set_state(request.generation, MediaState::Loading);
    }
}

} // namespace platform
