/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platform {

constexpr int kCanvasWidth = 320;
constexpr int kCanvasHeight = 170;
constexpr size_t kCanvasPixels = static_cast<size_t>(kCanvasWidth) * kCanvasHeight;

enum class MediaKind {
    Still,
    Video,
    Animation, // GIF y similares: sin sonido y en bucle
};

enum class MediaState {
    Idle,
    Loading,
    Still,   // foto en pantalla
    Playing,
    Paused,
    Ended,   // el vídeo ha acabado; se queda el último fotograma
    Error,
};

/**
 * Carga fotos y reproduce vídeos en un hilo aparte y deja el resultado como fotogramas
 * RGB565 de pantalla completa (320×170, con bandas negras si hace falta).
 *
 * Todo pasa fuera del hilo de LVGL porque en la CM0 abrir un JPEG de 12 Mpx tarda décimas
 * de segundo y eso, en el hilo de la interfaz, deja el teclado sin responder. La interfaz
 * sólo pide (`open`) y recoge (`take_frame`); pedir otra cosa abandona lo anterior sin
 * esperar a que termine.
 *
 * Los JPEG se abren con libjpeg, reducidos ya al decodificar. Lo demás (PNG, HEIC, WebP…
 * y todos los vídeos) lo hace `ffmpeg` como proceso aparte: así la app no enlaza media
 * docena de librerías de códecs y un fichero roto no la tira.
 */
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    void open(const std::string& path, MediaKind kind);
    void close();

    /**
     * Vídeo: pausa, reanuda o, si había acabado, vuelve a empezar. Una animación se
     * congela y al reanudar empieza de nuevo. En fotos no hace nada.
     */
    void toggle_pause();

    /** Copia el último fotograma en `destination` (kCanvasPixels) si hay uno nuevo. */
    bool take_frame(uint16_t* destination);

    MediaState state() const;
    std::string error() const;

private:
    struct Request {
        uint64_t generation{0};
        std::string path;
        MediaKind kind{MediaKind::Still};
    };

    void run();
    void show_still(const Request& request);
    void play_video(const Request& request);
    bool superseded(uint64_t generation);
    void publish(const std::vector<uint16_t>& frame, uint64_t generation, MediaState state);
    void fail(uint64_t generation, const std::string& message);
    /** Espera a que se pulse pausa (`true`) o a que se pida otra cosa (`false`). */
    bool wait_for_toggle(uint64_t generation, uint64_t& toggles_seen);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Request request_;
    bool quit_{false};
    uint64_t toggles_{0};
    std::vector<uint16_t> frame_;
    bool frame_fresh_{false};
    std::string error_;
    std::atomic<MediaState> state_{MediaState::Idle};
    std::thread worker_;
};

} // namespace platform
