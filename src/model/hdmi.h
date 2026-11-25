/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

/**
 * Lo que se decide sin tocar el hardware para sacar fotos y vídeos por la salida HDMI del
 * Cardputer Zero: qué modo de pantalla poner, a qué tamaño pedirle los fotogramas a ffmpeg
 * y dónde colocarlos en la pantalla. La parte que habla con el kernel (DRM/KMS) está en
 * platform/kms_display.*.
 */
namespace hdmi {

/** Un modo que anuncia la pantalla (de su EDID, o los de serie si no hay). */
struct Mode {
    int width{0};
    int height{0};
    int refresh{0}; // Hz
    bool preferred{false};
    bool interlaced{false};
};

/**
 * El modo que se pone: el preferido de la pantalla si la CM0 puede con él, y si no (una
 * tele 4K) el mayor que no pase de 1920×1080, mejor a 50-60 Hz que a 24-30 y nunca
 * entrelazado. La CM0 escala por hardware, pero decodifica por software: más de 1080p no
 * aportaría nada. -1 si no hay ninguno que valga.
 */
int pick_mode(const std::vector<Mode>& modes);

struct Size {
    int width{0};
    int height{0};
};

/**
 * Tamaño al que ffmpeg entrega los fotogramas de un vídeo de `width`×`height` (ya girado
 * según sus metadatos) para una pantalla de `screen_width`×`screen_height`: el suyo si
 * cabe (el plano de la GPU lo amplía gratis), y si no, reducido con su proporción hasta
 * caber. Siempre par: en YUV 4:2:0 el color va a medio tamaño.
 */
Size video_size(int width, int height, int screen_width, int screen_height);

/** Lo que interesa de un vídeo, de `ffprobe -show_entries stream=...:stream_side_data=rotation`. */
struct VideoInfo {
    int width{0};
    int height{0};
    int rotation{0}; // grados, como los da ffprobe (90, -90, 180…)
    double fps{0};
    double duration{0};
};

/**
 * Lee la salida de ffprobe con `-of default=nw=0` (líneas clave=valor). Un vídeo de móvil
 * grabado de pie lleva rotation=-90 o 90: entonces ancho y alto se intercambian, porque
 * ffmpeg lo endereza solo al decodificar.
 */
VideoInfo parse_probe(std::string_view text);

/** Fotogramas por segundo de un «30000/1001» o «25/1»; 0 si no se entiende. */
double parse_rate(std::string_view text);

/** Si el contenido de `/sys/class/drm/cardN-HDMI-A-1/status` dice que hay algo enchufado. */
bool status_connected(std::string_view text);

} // namespace hdmi
