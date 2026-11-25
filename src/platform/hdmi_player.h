/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "media_player.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

class KmsDisplay;

/**
 * Fotos y vídeos a resolución completa por la salida HDMI (KmsDisplay), en un hilo propio
 * como MediaPlayer: la interfaz pide (`open`, `toggle_pause`, `seek`) y mira (`state`,
 * `position`).
 *
 * Todo lo decodifica ffmpeg por software: con `gpu_mem=16` (lo que deja ZeroPower) la CM0
 * no tiene decodificador por hardware, pero un H.264 de 1080p le sale a 1,6 veces la
 * velocidad real (medido). Los fotogramas llegan en YUV 4:2:0 al tamaño del vídeo (o
 * reducidos si no caben en la pantalla) y la GPU los amplía. Las fotos, al tamaño de la
 * pantalla. El sonido, como en la pantalla del aparato (PipeWire, que lo mandará adonde
 * esté elegido).
 */
class HdmiPlayer {
public:
    HdmiPlayer();
    ~HdmiPlayer();

    HdmiPlayer(const HdmiPlayer&) = delete;
    HdmiPlayer& operator=(const HdmiPlayer&) = delete;

    /** Si hay algo enchufado al HDMI (barato: sólo mira /sys). */
    static bool available();

    /** Enseña `path`; con la salida apagada, la enciende. Vacío: pantalla en negro. */
    void open(const std::string& path, MediaKind kind);
    void toggle_pause();
    void seek(double seconds);

    double position() const;
    /** Lo que dura el vídeo (0 si no se sabe o es una foto). */
    double duration() const;
    MediaState state() const;
    std::string error() const;
    /** Para los avisos: «HDMI 1920x1080» con el modo puesto, o «HDMI». */
    std::string name() const;

private:
    struct Request {
        uint64_t generation{0};
        std::string path;
        MediaKind kind{MediaKind::Still};
    };

    enum class Event {
        Superseded,
        Toggled,
        Sought,
    };

    void run();
    bool ensure_display(uint64_t generation);
    void show_still(const Request& request);
    void play_video(const Request& request);
    bool show_video_frame(const Request& request, double seconds, int width, int height);
    bool superseded(uint64_t generation);
    void set_state(uint64_t generation, MediaState state);
    void fail(uint64_t generation, const std::string& message);
    Event wait_for_event(uint64_t generation, uint64_t& toggles_seen, double& seek);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Request request_;
    bool quit_{false};
    uint64_t toggles_{0};
    double seek_{0.0};
    std::string error_;
    std::string name_{"HDMI"};
    std::atomic<double> position_{0.0};
    std::atomic<double> duration_{0.0};
    std::atomic<MediaState> state_{MediaState::Idle};
    KmsDisplay* display_{nullptr}; // sólo desde worker_
    std::thread worker_;
};

} // namespace platform
