/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

/**
 * La salida HDMI del Cardputer Zero manejada directamente por DRM/KMS (la tarjeta `vc4`;
 * la pantalla interna es otra tarjeta, la del panel SPI, y no se toca).
 *
 * Al abrir, se pone el modo que elige hdmi::pick_mode y se apaga el plano de fondo (la GPU
 * rellena de negro sola); cada fotograma (YUV 4:2:0, como lo saca ffmpeg) va a un plano
 * superpuesto que la GPU escala y centra en la pantalla. Tres búferes y un hilo que los
 * cambia en cada refresco: sin medio fotograma a la vista y sin frenar al reproductor.
 *
 * Escalar es cosa de la GPU: un vídeo de 720p se ve a pantalla completa sin que la CPU amplíe
 * nada, y un 1080p del decodificador por hardware se reduce sin leerlo con la CPU. El escalador
 * no depende de `gpu_mem` (funciona también con 16).
 *
 * Sólo ioctls del kernel (cabeceras `drm/drm_mode.h` de linux-libc-dev), sin libdrm: no
 * añade dependencias al .deb. Mientras está abierta, la app es la dueña (DRM master) de
 * esa tarjeta: si hay un escritorio en la salida HDMI, falla al abrir y lo dice.
 */
class KmsDisplay {
public:
    KmsDisplay() = default;
    ~KmsDisplay();

    KmsDisplay(const KmsDisplay&) = delete;
    KmsDisplay& operator=(const KmsDisplay&) = delete;

    /**
     * Tarjeta (`/dev/dri/cardN`) que tiene algo enchufado en un conector HDMI, o vacío.
     * Se mira en /sys, sin abrir nada: es lo bastante barato para llamarlo a menudo.
     * `ZEROGALLERY_HDMI_SYSFS` cambia /sys/class/drm (pruebas).
     */
    static std::string connected_card();

    /** Enciende la salida con la pantalla en negro. `error` dice por qué no, en inglés. */
    bool open(std::string& error);
    void close();
    bool is_open() const { return fd_ >= 0; }

    int width() const { return width_; }
    int height() const { return height_; }

    /**
     * Pinta un fotograma YUV 4:2:0 plano (I420: Y, luego U y V a medio tamaño) de
     * `width`×`height` (pares), encajado con su proporción y centrado. Se copia a un búfer
     * libre y se deja para el siguiente refresco; vuelve sin esperarlo, salvo con `wait`
     * (una foto: así se sabe si ha salido). `false` si el último cambio falló.
     */
    bool show(const uint8_t* frame, int width, int height, bool wait = false);

    /** Quita la imagen (queda la pantalla en negro). */
    void blank();

private:
    struct Buffer {
        uint32_t handle{0};
        uint32_t fb{0};
        uint32_t pitch{0};
        size_t size{0};
        uint8_t* map{nullptr};
        uint32_t offsets[3]{};
    };

    bool create_buffer(Buffer& buffer, int width, int height, bool yuv);
    void destroy_buffer(Buffer& buffer);
    void drop_background();
    void present_loop();
    /** Espera a que no quede ningún cambio pendiente ni en marcha. */
    void wait_idle();

    int fd_{-1};
    uint32_t crtc_{0};
    uint32_t connector_{0};
    uint32_t plane_{0};
    int width_{0};
    int height_{0};
    Buffer background_;
    std::array<Buffer, 3> frames_;
    int frame_width_{0};
    int frame_height_{0};
    bool plane_on_{false};

    // Cambio de fotograma en su hilo (todo con present_mutex_).
    std::mutex present_mutex_;
    std::condition_variable present_wake_;
    std::thread presenter_;
    bool present_quit_{false};
    int pending_{-1};   // búfer listo esperando turno
    int in_flight_{-1}; // búfer que se está poniendo (SETPLANE en marcha)
    int shown_{-1};     // búfer en pantalla
    uint64_t queued_{0};
    uint64_t presented_{0};
    bool present_failed_{false};
};

} // namespace platform
