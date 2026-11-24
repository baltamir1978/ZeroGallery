/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "dlna.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace platform {

enum class CastState {
    Off,      // no se está mandando nada
    Sending,  // preparando el fichero o esperando a que la tele lo coja
    Showing,  // foto en la tele
    Playing,  // vídeo en marcha en la tele
    Paused,
    Stopped,  // el vídeo acabó o lo pararon desde la tele
    Error,
};

/**
 * Manda fotos y vídeos a una tele por DLNA. Todo pasa en hilos propios: la interfaz pide
 * (`cast`, `toggle_pause`, `seek`) y mira cómo va (`state`, `position`).
 *
 * - Buscar teles: M-SEARCH por SSDP cada pocos segundos mientras la lista esté abierta,
 *   por cada interfaz con multicast, y lectura de la descripción de lo que conteste.
 * - Servir el fichero: un servidor HTTP mínimo que sólo da los ficheros que se han mandado
 *   (con una ruta imposible de adivinar), con trozos (Range) y las cabeceras DLNA que piden
 *   las teles. Sólo existe mientras se está mandando algo.
 * - Fotos que no son un JPEG corriente: se convierten antes con ffmpeg (ver
 *   dlna::photo_needs_conversion). Los vídeos van tal cual: decodifica la tele.
 *
 * ZEROGALLERY_TV=<url de la descripción> añade una tele a mano, para redes sin multicast
 * (Tailscale) o pruebas. ZEROGALLERY_DLNA_DEBUG=1 cuenta lo que pasa por stderr.
 */
class DlnaCaster {
public:
    DlnaCaster();
    ~DlnaCaster();

    DlnaCaster(const DlnaCaster&) = delete;
    DlnaCaster& operator=(const DlnaCaster&) = delete;

    /** Empieza a buscar teles (y sigue buscando hasta `stop_discovery`). */
    void start_discovery();
    void stop_discovery();
    bool searching() const;
    /** Teles encontradas, en el orden en que contestaron. */
    std::vector<dlna::Renderer> renderers() const;
    /** Cambia cada vez que cambia la lista. */
    uint32_t renderers_version() const;

    /** Manda `path` a `tv`. Lo que se estuviera mandando se abandona. */
    void cast(const dlna::Renderer& tv, const std::string& path, bool video);
    /** Vídeo: pausa, reanuda o, si había acabado, vuelve a empezar. */
    void toggle_pause();
    /** Salta en el vídeo; varios saltos seguidos se suman. */
    void seek(double seconds);
    /** Deja de mandar: para la tele y cierra el servidor de ficheros. */
    void stop();

    /** Si se está mandando algo a una tele (aunque haya fallado lo último). */
    bool active() const;
    std::string tv_name() const;
    std::string tv_udn() const;
    CastState state() const;
    std::string error() const;
    double position() const;
    double duration() const;
    /** Aviso pasajero (un salto que la tele no quiso hacer…); vacío si no hay. */
    std::string take_notice();

private:
    struct Job {
        uint64_t generation{0};
        dlna::Renderer tv;
        std::string path;
        bool video{false};
    };

    struct Served {
        std::string path;
        std::string mime;
        std::string features;
        bool temporary{false};
    };

    struct Connection {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
        int fd{-1};
    };

    void discovery_loop();
    void worker_loop();
    void server_loop();
    void serve_connection(int fd, std::shared_ptr<std::atomic<bool>> done);

    bool ensure_server();
    void shutdown_server();
    std::string add_served(Served served);
    void clear_served();

    void do_cast(const Job& job);
    void do_toggle();
    void do_seek(double delta);
    void do_stop();
    void poll_tv();
    bool superseded(uint64_t generation) const;
    void set_state(CastState state);
    void fail(uint64_t generation, const std::string& message);
    bool soap(const std::string& control_url,
              const char* service,
              const char* action,
              const std::string& arguments,
              std::string& response,
              int& upnp_error,
              int timeout_ms = 4000);
    /** JPEG corriente a partir de cualquier foto; `false` si ffmpeg no puede. */
    bool convert_photo(uint64_t generation, const std::string& path, int orientation, const std::string& out);

    // Lista de teles
    mutable std::mutex renderers_mutex_;
    std::vector<dlna::Renderer> renderers_;
    std::atomic<uint32_t> renderers_version_{0};
    std::atomic<bool> discovering_{false};
    bool discovery_alive_{false}; // con renderers_mutex_
    std::thread discovery_;

    // Lo que se manda
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Job wanted_;
    Job current_;
    uint64_t current_generation_{0};
    int toggles_{0};
    double seek_{0.0};
    bool stop_requested_{false};
    bool quit_{false};
    bool active_{false};
    std::string error_;
    std::string notice_;
    std::atomic<CastState> state_{CastState::Off};
    std::atomic<double> position_{0.0};
    std::atomic<double> duration_{0.0};
    uint32_t cast_started_ms_{0};
    int poll_failures_{0};
    std::string sink_udn_;
    std::string sink_;
    std::thread worker_;

    // Servidor de ficheros
    std::mutex served_mutex_;
    std::map<std::string, Served> served_;
    std::vector<std::string> served_order_;
    std::string temp_dir_;
    int listen_fd_{-1};
    int port_{0};
    std::atomic<bool> server_quit_{false};
    std::thread server_;
    std::mutex connections_mutex_;
    std::vector<Connection> connections_;
};

} // namespace platform
