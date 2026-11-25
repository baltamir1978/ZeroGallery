/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace platform {

/**
 * La salida HDMI del Cardputer Zero manejada directamente por DRM/KMS (la tarjeta `vc4`;
 * la pantalla interna es otra tarjeta, la del panel SPI, y no se toca).
 *
 * Al abrir, se pone el modo que elige hdmi::pick_mode con un fondo negro; cada fotograma
 * (YUV 4:2:0, como lo saca ffmpeg) va a un plano superpuesto que la GPU escala y centra
 * en la pantalla. Así un vídeo de 720p se ve a pantalla completa sin que la CPU amplíe
 * nada, y con `gpu_mem=16` también: el escalador de la GPU no depende de esa memoria (el
 * decodificador por hardware sí, y por eso aquí no se usa).
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
     * `width`×`height` (pares), encajado con su proporción y centrado. Se escribe en el
     * búfer que no se está viendo y luego se cambia: sin medio fotograma a la vista.
     */
    bool show(const uint8_t* frame, int width, int height);

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

    int fd_{-1};
    uint32_t crtc_{0};
    uint32_t connector_{0};
    uint32_t plane_{0};
    int width_{0};
    int height_{0};
    Buffer background_;
    std::array<Buffer, 2> frames_;
    int frame_width_{0};
    int frame_height_{0};
    int next_{0};
    bool plane_on_{false};
};

} // namespace platform
