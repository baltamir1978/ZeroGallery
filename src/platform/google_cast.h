/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "gcast.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

/**
 * Conexión con un aparato Google Cast: TLS al puerto 8009 y un hilo que lee y escribe los
 * mensajes (OpenSSL no deja leer y escribir a la vez desde dos hilos en la misma conexión),
 * contesta a los PING y guarda el último estado del aparato y del reproductor.
 *
 * TLS va con el libssl del sistema cargado al vuelo (dlopen): así ni la compilación ni el
 * .deb necesitan OpenSSL, y si no estuviera, Cast simplemente no funciona y DLNA sí. El
 * certificado del aparato es propio y no se comprueba (es lo que hacen todos los clientes).
 *
 * Se usa desde un solo hilo (el del DlnaCaster): las llamadas esperan la respuesta.
 * Mismo fichero en ZeroGallery y en el fork de Music.
 */
class GoogleCast {
public:
    GoogleCast() = default;
    ~GoogleCast();

    GoogleCast(const GoogleCast&) = delete;
    GoogleCast& operator=(const GoogleCast&) = delete;

    /** Conecta (o sigue conectado si ya lo estaba con ese aparato). */
    bool connect(const std::string& host, int port);
    bool connected() const;
    void close();
    const std::string& host() const { return host_; }

    /**
     * Deja abierto el reproductor de Google y conectado a él. `false` si no se abre (otra
     * app que no deja, aparato ocupado…).
     */
    bool ensure_media_app(std::string& error);

    /** Manda LOAD; espera a que el aparato diga que lo tiene (o que no puede). */
    bool load(const gcast::LoadInfo& info, std::string& error);

    // Órdenes al reproductor; esperan la respuesta (MEDIA_STATUS).
    bool play();
    bool pause();
    bool seek(double seconds);
    bool request_media_status();
    /** Cierra el reproductor en el aparato (vuelve a su pantalla de reposo). */
    void stop_app();

    bool set_volume(double level);
    bool set_muted(bool muted);
    bool request_receiver_status();

    /** Último estado conocido. `media_version` cambia con cada MEDIA_STATUS que llega. */
    gcast::MediaStatus media_status() const;
    uint64_t media_version() const;
    gcast::ReceiverStatus receiver_status() const;
    /** Posición estimada: la última conocida más lo que ha pasado si está sonando. */
    double position() const;

private:
    bool send(const std::string& destination, const char* name_space, const std::string& payload);
    /** Manda y espera la respuesta con ese requestId; `false` si no llega a tiempo. */
    bool call(const std::string& destination,
              const char* name_space,
              const char* type,
              const std::string& fields,
              gcast::Json& reply,
              int timeout_ms = 8000);
    void io_loop();
    void handle(const gcast::Message& message);

    std::string host_;
    int port_{0};
    int fd_{-1};
    void* ssl_{nullptr};
    void* ctx_{nullptr};
    std::thread io_;
    std::atomic<bool> running_{false};

    mutable std::mutex mutex_;
    std::condition_variable replied_;
    std::deque<std::string> outgoing_;
    std::map<int, gcast::Json> replies_;
    int next_request_{1};
    std::string transport_id_;   // reproductor al que se está conectado
    std::string session_id_;
    gcast::ReceiverStatus receiver_;
    gcast::MediaStatus media_;
    uint64_t media_version_{0};
    double media_time_at_{0};    // cuándo llegó media_.current_time (reloj monótono)
};

} // namespace platform
