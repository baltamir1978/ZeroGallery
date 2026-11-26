/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace platform {

/** Código de salida del hijo cuando no se ha podido ejecutar ffmpeg (no está instalado). */
constexpr int kExitExecFailed = 127;

/**
 * Un `ffmpeg` (o `ffprobe`) en marcha que escribe por una tubería. Lo usan la pantalla del
 * aparato (MediaPlayer) y la salida HDMI (HdmiPlayer).
 */
class FfmpegProcess {
public:
    FfmpegProcess() = default;
    ~FfmpegProcess() { stop(); }

    FfmpegProcess(const FfmpegProcess&) = delete;
    FfmpegProcess& operator=(const FfmpegProcess&) = delete;

    /**
     * `tool` es «ffmpeg», «ffprobe» u otro programa: `ZEROGALLERY_FFMPEG` sólo cambia el
     * primero. `stdin_fd`: lo que el hijo lee por la entrada (si no, /dev/null). `fd3`: se le
     * pasa como descriptor 3 (para «pipe:3» de ffmpeg: el sonido para el HDMI).
     */
    bool start(const std::vector<std::string>& arguments, const char* tool = "ffmpeg", int stdin_fd = -1,
               int fd3 = -1);

    /**
     * Lee hasta `size` bytes esperando como mucho `timeout_ms`.
     * Devuelve los leídos, 0 si no ha llegado nada todavía y -1 si ffmpeg ha terminado.
     */
    long read_some(uint8_t* buffer, size_t size, int timeout_ms);

    /** Lo mata (si sigue) y lo recoge. */
    void stop();

    /** Recoge el código de salida cuando ffmpeg ha terminado por su cuenta. */
    int finish();

private:
    pid_t pid_{-1};
    int fd_{-1};
    int exit_code_{-1};
};

/** Valor de una variable de entorno, o `fallback` si no está o está vacía. */
const char* env_or(const char* name, const char* fallback);

/**
 * Por dónde sacar el sonido («pulse», «alsa» o vacío para ninguno). El Cardputer Zero lleva
 * PipeWire (con su servidor compatible con PulseAudio), que es quien decide si suena el
 * altavoz, los auriculares o un altavoz Bluetooth y quien aplica el volumen del sistema: si
 * está, se le entrega a él y no se elige salida aquí. ALSA a pelo queda para sistemas sin
 * PipeWire. `ZEROGALLERY_AUDIO=none|pulse|alsa` lo fuerza.
 */
std::string audio_output();

/**
 * Ejecuta `ffprobe` con `arguments` y devuelve lo que escribe (hasta 4 KB y 5 s); vacío si
 * falla.
 */
std::string probe_text(const std::vector<std::string>& arguments);

/** Lo mismo con otro programa (`tool`, buscado en el PATH): `pactl` para el volumen. */
std::string command_text(const char* tool, const std::vector<std::string>& arguments);

} // namespace platform
