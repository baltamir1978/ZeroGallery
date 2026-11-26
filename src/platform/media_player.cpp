/*
 * SPDX-License-Identifier: MIT
 */

#include "media_player.h"
#include "fps_counters.h"

#include "audio_route.h"
#include "ffmpeg_process.h"
#include "gallery.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

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

// Fotogramas por segundo: los del propio vídeo con 30 de tope (gallery::screen_video_fps;
// hasta la 0.7.0, 15 fijos; en la 0.7.1 de pruebas, 30 fijos, que en un vídeo de 25 repetía
// 5 fotogramas de cada 25 y daba tirones). Medido en el aparato (25-nov-2026):
// - El panel SPI se refresca como mucho unas 37 veces por segundo por /dev/fb0
//   (mipi_dbi_fb_dirty contado con ftrace escribiendo sin parar): 30 cabe.
// - Lo caro es decodificar, no cuántos fotogramas salen: un 1080p a 30 fps gasta 126 % de CPU
//   frente al 117 % a 15 (720p: 89 % frente a 79 %).
// - Por software y no con el decodificador por hardware: para reducir a 320×170 hay que leer
//   sus fotogramas con la CPU desde memoria sin caché y sale peor (1080p: 160 %; 720p: 152 %).
constexpr size_t kFrameBytes = kCanvasPixels * sizeof(uint16_t);

uint16_t rgb565(unsigned r, unsigned g, unsigned b) {
    return static_cast<uint16_t>(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
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
bool decode_jpeg(std::FILE* file,
                 int canvas_width,
                 int canvas_height,
                 std::vector<uint8_t>& pixels,
                 int& width,
                 int& height,
                 int& orientation) {
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
    const auto fit = gallery::fit_rect(shown_width, shown_height, canvas_width, canvas_height);
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

/**
 * Endereza según EXIF, encaja en el lienzo promediando píxeles y deja el resto en negro.
 * El lienzo es la pantalla tal como se va a ver: 320×170, o 170×320 con el aparato de lado.
 */
void render_to_canvas(const std::vector<uint8_t>& pixels,
                      int width,
                      int height,
                      int orientation,
                      int canvas_width,
                      int canvas_height,
                      std::vector<uint16_t>& canvas) {
    canvas.assign(kCanvasPixels, 0);
    const bool swap = gallery::orientation_swaps_axes(orientation);
    const int shown_width = swap ? height : width;
    const int shown_height = swap ? width : height;
    const auto fit = gallery::fit_rect(shown_width, shown_height, canvas_width, canvas_height);

    for (int y = 0; y < fit.height; ++y) {
        const int y0 = static_cast<int>(static_cast<long long>(y) * shown_height / fit.height);
        const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(y + 1) * shown_height / fit.height));
        auto* out = canvas.data() + static_cast<size_t>(fit.y + y) * canvas_width + fit.x;
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

bool load_jpeg(const std::string& path, int canvas_width, int canvas_height, std::vector<uint16_t>& canvas) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int orientation = 1;
    const bool ok = decode_jpeg(file, canvas_width, canvas_height, pixels, width, height, orientation);
    std::fclose(file);
    if (ok) {
        render_to_canvas(pixels, width, height, orientation, canvas_width, canvas_height, canvas);
    }
    return ok;
}

// ---------------------------------------------------------------- ffmpeg

/** Ancho del lienzo tal como se va a ver: con el aparato de lado, el alto de la pantalla. */
int upright_width(int turns) {
    return gallery::rotation_swaps_axes(turns) ? kCanvasHeight : kCanvasWidth;
}

int upright_height(int turns) {
    return gallery::rotation_swaps_axes(turns) ? kCanvasWidth : kCanvasHeight;
}

/**
 * ffmpeg encaja en el lienzo tal como se va a ver y el giro a la pantalla se hace aquí
 * (`to_canvas`): son 54 400 píxeles por fotograma y así fotos y vídeos giran igual.
 */
std::string canvas_filter(bool video, int turns, double fps = gallery::kMaxScreenFps) {
    const int width = upright_width(turns);
    const int height = upright_height(turns);
    char rate[32] = "";
    if (video) {
        std::snprintf(rate, sizeof(rate), "fps=%.3f,", fps);
    }
    char filter[256];
    std::snprintf(filter,
                  sizeof(filter),
                  "%sscale=%d:%d:force_original_aspect_ratio=decrease:flags=%s,"
                  "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:black",
                  rate,
                  width,
                  height,
                  video ? "fast_bilinear" : "area",
                  width,
                  height);
    return filter;
}

std::vector<std::string> still_arguments(const std::string& path, int turns) {
    // Sin -map: en un HEIC de móvil la imagen es una rejilla de teselas y ffmpeg sólo la
    // recompone si se le deja elegir a él.
    return {"-nostdin", "-hide_banner", "-loglevel", "error", "-i", path, "-frames:v", "1",
            "-vf", canvas_filter(false, turns), "-pix_fmt", "rgb565le", "-f", "rawvideo", "pipe:1"};
}

/** Un solo fotograma de un vídeo, sin sonido. */
std::vector<std::string> video_frame_arguments(const std::string& path, double seconds, int turns) {
    char position[32];
    std::snprintf(position, sizeof(position), "%.3f", seconds);
    return {"-nostdin", "-hide_banner", "-loglevel", "error", "-skip_loop_filter", "all", "-ss", position,
            "-i", path, "-an", "-frames:v", "1", "-vf", canvas_filter(false, turns), "-pix_fmt", "rgb565le",
            "-f", "rawvideo", "pipe:1"};
}

/**
 * `audio`: formato de salida de ffmpeg («pulse» o «alsa»), o vacío para no sacar sonido.
 * `loop`: repetir sin fin (animaciones).
 */
std::vector<std::string> video_arguments(const std::string& path,
                                         double position,
                                         const std::vector<std::string>& audio,
                                         bool loop,
                                         int turns,
                                         double fps) {
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
    arguments.insert(arguments.end(), {"-i", path, "-an", "-vf", canvas_filter(true, turns, fps), "-pix_fmt", "rgb565le",
                                       "-f", "rawvideo", "pipe:1"});
    // Segunda salida del mismo proceso (altavoz, cascos o HDMI, ver audio_route.h): al salir
    // las dos del mismo reloj, imagen y sonido van a la par sin sincronizar nada aquí.
    arguments.insert(arguments.end(), audio.begin(), audio.end());
    return arguments;
}

/** Del lienzo tal como se va a ver a la pantalla tal como está montada. */
void to_canvas(const std::vector<uint16_t>& upright, int turns, std::vector<uint16_t>& canvas) {
    canvas.resize(kCanvasPixels);
    gallery::rotate_to_screen(upright.data(), turns, kCanvasWidth, kCanvasHeight, canvas.data());
}

uint8_t* bytes_of(std::vector<uint16_t>& pixels) {
    return reinterpret_cast<uint8_t*>(pixels.data());
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
        seek_ = 0.0;
        position_ = 0.0;
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

void MediaPlayer::set_rotation(int turns) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        rotation_ = ((turns % 4) + 4) % 4;
    }
    wake_.notify_all();
}

void MediaPlayer::seek(double seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_.path.empty() || request_.kind != MediaKind::Video) {
            return;
        }
        seek_ += seconds;
    }
    wake_.notify_all();
}

double MediaPlayer::position() const {
    return position_;
}

bool MediaPlayer::take_frame(uint16_t* destination) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frame_fresh_) {
        return false;
    }
    std::memcpy(destination, frame_.data(), kFrameBytes);
    frame_fresh_ = false;
    fps_counters().taken++;
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
    fps_counters().published++;
}

void MediaPlayer::fail(uint64_t generation, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (request_.generation != generation) {
        return;
    }
    error_ = message;
    state_ = MediaState::Error;
}

MediaPlayer::Event MediaPlayer::wait_for_event(uint64_t generation,
                                               uint64_t& toggles_seen,
                                               int& turns,
                                               double& seek) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] {
        return request_.generation != generation || toggles_ != toggles_seen || rotation_ != turns ||
               seek_ != 0.0;
    });
    if (request_.generation != generation) {
        return Event::Superseded;
    }
    if (toggles_ != toggles_seen) {
        toggles_seen = toggles_;
        return Event::Toggled;
    }
    if (seek_ != 0.0) {
        seek = seek_;
        seek_ = 0.0;
        return Event::Sought;
    }
    turns = rotation_;
    return Event::Rotated;
}

void MediaPlayer::run() {
    uint64_t handled = 0;
    Request request;
    // Giro con que se pintó la foto que hay en pantalla; -1 si lo que hay no es una foto
    // (el vídeo se ocupa él de su giro mientras dura).
    int still_turns = -1;
    for (;;) {
        int turns = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [&] {
                return quit_ || request_.generation != handled || (still_turns >= 0 && rotation_ != still_turns);
            });
            if (quit_) {
                return;
            }
            // Si no ha cambiado lo pedido es que se ha girado el aparato: la misma foto otra vez.
            request = request_;
            handled = request.generation;
            turns = rotation_;
        }
        still_turns = -1;
        if (request.path.empty()) {
            continue;
        }
        if (request.kind == MediaKind::Still) {
            show_still(request, turns);
            still_turns = turns;
        }
        else {
            play_video(request);
        }
    }
}

/**
 * Lee de ffmpeg un fotograma entero. `false` si no llega (ffmpeg acaba antes, tarda más de
 * medio minuto) o si mientras tanto se pide otra cosa.
 */
static bool read_frame(FfmpegProcess& ffmpeg, std::vector<uint16_t>& upright, size_t& filled, const std::function<bool()>& abandoned) {
    upright.resize(kCanvasPixels);
    filled = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (filled < kFrameBytes) {
        if (abandoned() || std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        const long count = ffmpeg.read_some(bytes_of(upright) + filled, kFrameBytes - filled, 100);
        if (count < 0) {
            return false;
        }
        filled += static_cast<size_t>(count);
    }
    return true;
}

void MediaPlayer::show_still(const Request& request, int turns) {
    std::vector<uint16_t> upright;
    std::vector<uint16_t> canvas;
    if (gallery::is_jpeg(request.path) &&
        load_jpeg(request.path, upright_width(turns), upright_height(turns), upright)) {
        to_canvas(upright, turns, canvas);
        publish(canvas, request.generation, MediaState::Still);
        return;
    }

    // Lo que libjpeg no abre (otros formatos, o un JPEG en CMYK) se le deja a ffmpeg.
    FfmpegProcess ffmpeg;
    if (!ffmpeg.start(still_arguments(request.path, turns))) {
        fail(request.generation, "Cannot run ffmpeg");
        return;
    }
    size_t filled = 0;
    if (!read_frame(ffmpeg, upright, filled, [&] { return superseded(request.generation); })) {
        if (superseded(request.generation)) {
            return;
        }
        const bool missing = filled == 0 && ffmpeg.finish() == kExitExecFailed;
        fail(request.generation, missing ? "ffmpeg is not installed" : "Cannot open this image");
        return;
    }
    to_canvas(upright, turns, canvas);
    publish(canvas, request.generation, MediaState::Still);
}

bool MediaPlayer::show_video_frame(const Request& request, double seconds, int turns, MediaState state) {
    FfmpegProcess ffmpeg;
    if (!ffmpeg.start(video_frame_arguments(request.path, seconds, turns))) {
        return false;
    }
    std::vector<uint16_t> upright;
    size_t filled = 0;
    if (!read_frame(ffmpeg, upright, filled, [&] { return superseded(request.generation); })) {
        return false;
    }
    std::vector<uint16_t> canvas;
    to_canvas(upright, turns, canvas);
    publish(canvas, request.generation, state);
    return true;
}

/**
 * Lo que dura el vídeo en segundos, preguntado a `ffprobe` (viene con ffmpeg); 0 si no lo
 * dice. Hace falta para que saltar adelante no se pase del final: un `-ss` más allá no da
 * ningún fotograma y la cuenta se quedaría en un sitio que no existe.
 */
static double video_duration(const std::string& path) {
    const std::string text = probe_text({"-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path});
    const double seconds = std::atof(text.c_str());
    return seconds > 0.0 ? seconds : 0.0;
}

/**
 * A cuántos fotogramas por segundo sacarlo en la pantalla: la media del vídeo (`avg_frame_rate`;
 * en los de móvil, de velocidad variable, `r_frame_rate` puede salir absurdo) y si no la da,
 * `r_frame_rate`; con eso, gallery::screen_video_fps.
 */
static double video_fps(const std::string& path) {
    const std::string text = probe_text({"-v", "error", "-select_streams", "v:0", "-show_entries",
                                         "stream=avg_frame_rate,r_frame_rate", "-of", "default=nw=1", path});
    double average = 0.0, nominal = 0.0;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(start, end - start);
        if (line.rfind("avg_frame_rate=", 0) == 0) {
            average = gallery::parse_frame_rate(line.substr(15));
        } else if (line.rfind("r_frame_rate=", 0) == 0) {
            nominal = gallery::parse_frame_rate(line.substr(13));
        }
        start = end + 1;
    }
    return gallery::screen_video_fps(average > 0.0 ? average : nominal);
}

void MediaPlayer::play_video(const Request& request) {
    const bool loop = request.kind == MediaKind::Animation;
    const double fps = video_fps(request.path);
    // Sin sonido si es una animación o si con sonido ha fallado (hasta que se cambie de salida).
    bool audio_off = loop;
    uint32_t audio_seen = audio_generation();

    uint64_t toggles_seen = 0;
    int turns = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        toggles_seen = toggles_;
        turns = rotation_;
    }

    double position = 0.0;
    // Sólo se pregunta la primera vez que se salta adelante: -1 es «sin preguntar».
    double duration = -1.0;
    // Dónde se cae al saltar `seek` desde `from`: ni antes del principio ni, hacia delante,
    // más allá de un segundo antes del final (que se vea acabar, no un fotograma parado).
    const auto landing = [&](double from, double seek) {
        double to = std::max(0.0, from + seek);
        if (seek > 0.0) {
            if (duration < 0.0) {
                duration = video_duration(request.path);
            }
            if (duration > 0.0) {
                to = std::min(to, std::max(from, duration - 1.0));
            }
        }
        return to;
    };
    std::vector<uint16_t> upright(kCanvasPixels);
    std::vector<uint16_t> canvas;

    const auto show_stopped = [&](MediaState state, double at) {
        // Justo al final puede no quedar fotograma que sacar: se prueba un poco antes.
        if (!show_video_frame(request, at, turns, state)) {
            show_video_frame(request, std::max(0.0, at - 1.0), turns, state);
        }
    };

    enum class Stopped {
        Superseded, // se ha pedido otra cosa
        Toggled,    // pausa: seguir (o repetir, si había acabado)
        Resume,     // acabado y se ha saltado atrás: seguir desde `at`
    };

    // Parado (en pausa o acabado) se espera aquí. `at` es el fotograma que hay en pantalla:
    // si se gira el aparato se le pide a ffmpeg ese mismo, ya girado, y si se salta, el de
    // donde se cae, y `at` se queda ahí.
    const auto wait_stopped = [&](MediaState state, double& at) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (request_.generation == request.generation) {
                state_ = state;
            }
        }
        for (;;) {
            double seek = 0.0;
            const Event event = wait_for_event(request.generation, toggles_seen, turns, seek);
            if (event == Event::Superseded) {
                return Stopped::Superseded;
            }
            if (event == Event::Toggled) {
                return Stopped::Toggled;
            }
            if (event == Event::Sought) {
                if (state == MediaState::Ended) {
                    // Más allá del final no hay nada; hacia atrás, se vuelve a ver desde ahí.
                    if (seek < 0.0) {
                        at = std::max(0.0, at + seek);
                        return Stopped::Resume;
                    }
                    continue;
                }
                at = landing(at, seek);
                position_ = at;
            }
            show_stopped(state, at);
        }
    };

    // Cada vuelta es un ffmpeg: al empezar, al reanudar tras una pausa (desde donde se
    // quedó), al girar el aparato y al repetir. Pausar es parar el proceso: con -re,
    // congelarlo y soltarlo después lo haría correr para recuperar el tiempo perdido. Y el
    // giro cambia el tamaño al que ffmpeg encaja la imagen, que va en sus argumentos.
    for (;;) {
        audio_seen = audio_generation();
        HdmiAudioPipe hdmi_audio;
        const bool to_hdmi = !audio_off && current_audio_output() == audio::Output::Hdmi;
        if (to_hdmi && !hdmi_audio.start()) {
            audio_off = true;
        }
        const auto audio = audio_off ? std::vector<std::string>{} : audio_arguments();
        FfmpegProcess ffmpeg;
        const bool launched = ffmpeg.start(video_arguments(request.path, position, audio, loop, turns, fps), "ffmpeg", -1,
                                          audio_off ? -1 : hdmi_audio.ffmpeg_fd());
        hdmi_audio.release_ffmpeg_end();
        if (!launched) {
            fail(request.generation, "Cannot run ffmpeg");
            return;
        }

        size_t filled = 0;
        long frames = 0;
        std::chrono::steady_clock::time_point next_due{};
        bool paused = false;
        bool rotated = false;
        double sought = 0.0;
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
                if (rotation_ != turns) {
                    turns = rotation_;
                    rotated = true;
                }
                if (audio_generation() != audio_seen) {
                    // Otra salida de sonido: se relanza desde donde iba, como al girar.
                    audio_seen = audio_generation();
                    audio_off = loop;
                    rotated = true;
                }
                if (seek_ != 0.0) {
                    sought = seek_;
                    seek_ = 0.0;
                }
            }
            if (paused || rotated || sought != 0.0) {
                break;
            }
            const long count = ffmpeg.read_some(bytes_of(upright) + filled, kFrameBytes - filled, 100);
            if (count < 0) {
                break;
            }
            filled += static_cast<size_t>(count);
            if (filled == kFrameBytes) {
                filled = 0;
                ++frames;
                to_canvas(upright, turns, canvas);
                // A su hora: ffmpeg los escribe a rachas (dos o tres seguidos, entre trozos de
                // sonido) y el visor sólo recoge el último cada 15 ms, así que de cada racha se
                // perdían todos menos uno (medido el 26-nov-2026: llegaban 25 por segundo y se
                // pintaban 11). Los adelantados esperan aquí; la tubería hace de colchón.
                const auto now = std::chrono::steady_clock::now();
                const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1.0 / fps));
                if (frames == 1 || now > next_due + 5 * period) {
                    next_due = now; // al empezar, o si se ha quedado atrás: no correr para recuperar
                }
                else if (now < next_due) {
                    std::this_thread::sleep_until(std::min(next_due, now + 4 * period));
                }
                next_due += period;
                publish(canvas, request.generation, MediaState::Playing);
                position_ = position + static_cast<double>(frames) / fps;
            }
        }

        // Una animación no sabe por dónde iba (se repite): al reanudar empieza otra vez.
        position = loop ? 0.0 : position + static_cast<double>(frames) / fps;
        if (sought != 0.0) {
            // Saltar es lo mismo que reanudar, pero desde otro sitio: otro ffmpeg con -ss.
            position = landing(position, sought);
        }
        position_ = position;

        if (paused) {
            ffmpeg.stop();
            if (rotated || sought != 0.0) {
                // Pausa a la vez que giro o salto: el fotograma que toca, no el que había.
                show_stopped(MediaState::Paused, position);
            }
            if (wait_stopped(MediaState::Paused, position) == Stopped::Superseded) {
                return;
            }
            continue;
        }
        if (rotated || sought != 0.0) {
            ffmpeg.stop();
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
            audio_off = true;
            continue;
        }
        if (frames == 0 && position <= 0.0) {
            fail(request.generation, "Cannot play this video");
            return;
        }

        // El último fotograma que se vio es el anterior a donde se quedó la cuenta.
        double at = std::max(0.0, position - 1.0 / fps);
        const Stopped next = wait_stopped(MediaState::Ended, at);
        if (next == Stopped::Superseded) {
            return;
        }
        position = next == Stopped::Resume ? at : 0.0;
        position_ = position;
    }
}

} // namespace platform
