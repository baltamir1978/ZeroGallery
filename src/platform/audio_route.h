/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "audio_output.h"
#include "ffmpeg_process.h"

#include <cstdint>
#include <string>
#include <vector>

namespace platform {

/**
 * Por dónde suena el vídeo (tecla A): altavoz, cascos o HDMI.
 *
 * En el Cardputer Zero altavoz y cascos salen del mismo códec (ES8389): el amplificador del
 * altavoz se alimenta del canal izquierdo de los cascos, y el controlador de M5Stack lo apaga
 * solo al enchufar unos (`m5stack,hp-mute-pins = "Speaker"`). Elegir entre los dos es encender
 * o apagar su control «Speaker Switch»; con «Speaker» y los cascos puestos suenan los dos (los
 * cascos no tienen interruptor propio). Se hace con los ioctls de control de ALSA del kernel
 * (`sound/asound.h`), sin alsa-lib ni amixer.
 *
 * El HDMI es otra tarjeta (`vc4hdmi`). PipeWire no sabe abrirla (sólo acepta el formato
 * IEC958 y su perfil «Pro Audio» no lo convierte: «no format found»); el PCM de ALSA
 * `hdmi:CARD=vc4hdmi,DEV=0` sí lo convierte, pero la salida ALSA de ffmpeg pide siempre el
 * periodo mínimo, que ahí es de 2 muestras, y el sonido sale a trompicones. Así que ffmpeg
 * entrega el sonido en crudo por un descriptor propio («pipe:3», HdmiAudioPipe) y un `aplay`
 * lo toca con periodos de 512 (búfer de 2048: 43 ms; la tubería, 4 KB: 21 ms; así la imagen
 * y el sonido no se separan). El volumen, con el mando de la tele.
 *
 * Los aparatos USB y Bluetooth son salidas de PipeWire («sinks»): se listan con `pactl` y
 * ffmpeg suena por el elegido con `-device`, sin tocar la salida por defecto del sistema (al
 * salir de la app todo queda como estaba). Altavoz y cascos van también con `-device`, a la
 * salida del códec: si no, al conectar unos cascos Bluetooth (que WirePlumber pone por
 * defecto) «Speaker» sonaría por ellos.
 *
 * La salida elegida es una sola para toda la app (los dos reproductores la leen al lanzar
 * ffmpeg) y `generation()` cambia cuando hay que relanzarlo (de o hacia el HDMI o un aparato).
 */
class AudioRoute {
public:
    AudioRoute();
    /** Deja el altavoz como lo tendría el controlador: apagado si hay cascos. */
    ~AudioRoute();

    AudioRoute(const AudioRoute&) = delete;
    AudioRoute& operator=(const AudioRoute&) = delete;

    static bool headphones_plugged();
    /** Hay una pantalla HDMI conectada que acepta sonido. */
    static bool hdmi_audio_available();

    audio::Output output() const;
    /** Lo elegido, con el aparato si es USB o Bluetooth. */
    audio::Choice choice() const;
    /**
     * Pone `output` (si no está disponible, la del aparato). Devuelve la que queda. Para un
     * aparato USB o Bluetooth, `set(Choice)`.
     */
    audio::Output set(audio::Output output);
    audio::Output set(const audio::Choice& choice);
    /** La siguiente disponible (tecla A). */
    audio::Output cycle();

    /** El volumen del sistema; `percent` -1 si no hay PipeWire. */
    struct Volume {
        int percent = -1;
        bool muted = false;
    };
    /**
     * Sube o baja el volumen de PipeWire de la salida elegida (la del códec con el altavoz o
     * los cascos, la del aparato USB o Bluetooth) `delta` puntos, sin pasar de 100 %, y quita
     * el silencio al subir. Devuelve cómo queda. Al HDMI no le afecta: va por `aplay`.
     */
    static Volume change_volume(int delta);
    /** Pone o quita el silencio. */
    static Volume toggle_mute();
};

/**
 * Lo que lleva el sonido de ffmpeg al HDMI: una tubería pequeña y un `aplay` leyéndola.
 * Vive lo mismo que el ffmpeg al que acompaña: al matarlo, aplay acaba lo que tenga y sale.
 */
class HdmiAudioPipe {
public:
    HdmiAudioPipe() = default;
    ~HdmiAudioPipe();

    HdmiAudioPipe(const HdmiAudioPipe&) = delete;
    HdmiAudioPipe& operator=(const HdmiAudioPipe&) = delete;

    /** Crea la tubería y lanza aplay. `false` si no se puede (sin aplay, sin HDMI…). */
    bool start();
    /** El extremo que se le da a ffmpeg como descriptor 3. */
    int ffmpeg_fd() const { return write_; }
    /** Tras lanzar ffmpeg: cierra aquí su extremo, para que aplay vea el final cuando muera. */
    void release_ffmpeg_end();

private:
    FfmpegProcess aplay_;
    int read_{-1};
    int write_{-1};
};

/** La salida de sonido elegida ahora (la que leen los reproductores). */
audio::Output current_audio_output();

/** Cambia cada vez que hay que relanzar ffmpeg para que el sonido salga por otro sitio. */
uint32_t audio_generation();

/**
 * Argumentos de ffmpeg para la salida de sonido elegida (una segunda salida del mismo
 * proceso, para que imagen y sonido vayan a la par). Vacío: sin sonido.
 */
std::vector<std::string> audio_arguments();

} // namespace platform
