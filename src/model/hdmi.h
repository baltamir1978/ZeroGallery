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
 * tele 4K) el mayor que no pase de 1920×1200, mejor a 50-60 Hz que a 24-30 y nunca
 * entrelazado. 1920×1200 a 60 Hz es lo más que saca el HDMI de la CM0 (el de la Pi 3:
 * hasta unos 162 MHz de reloj de píxel). -1 si no hay ninguno que valga.
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
    std::string codec; // «h264», «hevc»…
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

/**
 * Si el vídeo se puede decodificar con el decodificador por hardware de la CM0
 * (`h264_v4l2m2m`, que sólo existe con `gpu_mem` de 32 MB o más): sólo H.264 y hasta
 * 1920×1080, y **sin girar**. HEVC no lo decodifica: ese chip no lo tiene.
 *
 * Lo de girar es porque el decodificador deja los fotogramas en memoria sin caché: leerlos con
 * la CPU para girarlos o reducirlos es lentísimo (un 1080p reducido a 1024×576 va a 0,4 veces
 * la velocidad real; medido). Por eso, con hardware, el fotograma va a la pantalla a su tamaño
 * y lo reduce la GPU (`hardware_size`), y un vídeo de móvil grabado de pie (que lleva el giro
 * en los metadatos) va por software.
 */
bool hardware_decodable(const VideoInfo& info);

/**
 * Hasta cuántos fotogramas por segundo se piden. Lo que limita es la tubería entre ffmpeg y
 * la app (3 MB por fotograma a 1080p): con el decodificador por hardware, un 1080p60 se queda
 * en unos 52 fps (medido), así que 60 sólo hasta 720p; por software, 30 siempre.
 */
double max_fps(bool hardware, int width, int height);

/** Tamaño de los fotogramas con el decodificador por hardware: el del vídeo, par. */
Size hardware_size(const VideoInfo& info);

/** Fotogramas por segundo de un «30000/1001» o «25/1»; 0 si no se entiende. */
double parse_rate(std::string_view text);

/** Si el contenido de `/sys/class/drm/cardN-HDMI-A-1/status` dice que hay algo enchufado. */
bool status_connected(std::string_view text);

} // namespace hdmi
