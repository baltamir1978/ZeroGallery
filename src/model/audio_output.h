/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

/**
 * Por dónde suena el vídeo: el altavoz del Cardputer, los cascos, la tele por HDMI o un
 * aparato USB o Bluetooth que PipeWire tenga (una tarjeta de sonido, unos cascos, un
 * altavoz). Lo que se decide sin tocar el hardware; el cambio de verdad está en
 * platform/audio_route.*.
 */
namespace audio {

enum class Output {
    Speaker,
    Headphones,
    Hdmi,
    /** Un aparato USB o Bluetooth: cuál, en `Choice::sink`. */
    External,
};

/** Para el rótulo: «Speaker», «Headphones», «HDMI», «External». */
const char* output_name(Output output);

/** Una salida de PipeWire, de `pactl --format=json list sinks`. */
struct Sink {
    /** El nombre interno, el que se le da a ffmpeg («bluez_output.AA_BB_CC_DD_EE_FF.1»). */
    std::string name;
    /** El que se enseña («WH-1000XM4»). */
    std::string description;
    /** USB o Bluetooth (`device.bus` o `device.api` = «bluez5»). */
    bool external = false;
    /**
     * Una tarjeta de ALSA (`device.api` = «alsa»). Las salidas «mudas» (`module-null-sink`,
     * como la que crea ZeroPower para callar al lanzador con la pantalla apagada) no la tienen.
     */
    bool alsa = false;
};

/**
 * Las salidas que lista `pactl --format=json list sinks`. Sólo mira los campos `name`,
 * `description` y, dentro de `properties`, `device.bus` y `device.api`; lo demás se salta
 * (también los `name` de los puertos, que van más adentro). Con un JSON roto devuelve lo que
 * haya leído hasta ahí.
 */
std::vector<Sink> sinks_from_pactl_json(std::string_view json);

/**
 * La salida de PipeWire del propio Cardputer (la del códec, por la que suenan el altavoz y
 * los cascos): la primera tarjeta de ALSA que no es USB ni HDMI. Vacío si no hay.
 */
std::string internal_sink(const std::vector<Sink>& sinks);

/** Lo elegido con A: la salida y, si es un aparato USB o Bluetooth, cuál. */
struct Choice {
    Output output = Output::Speaker;
    /** Sólo con `External`: el nombre interno del aparato. */
    std::string sink;
    /** Sólo con `External`: el que se enseña. */
    std::string label;

    bool operator==(const Choice& other) const {
        return output == other.output && sink == other.sink;
    }
};

/** Para el rótulo: el nombre del aparato si es USB o Bluetooth, si no `output_name`. */
std::string choice_name(const Choice& choice);

/**
 * La siguiente al pulsar A: Speaker → Headphones → HDMI → cada aparato de `externals` (sólo
 * los que son `external`, en su orden) → Speaker…, saltando lo que no hay. Si el aparato
 * elegido ya no está (se ha desconectado), se vuelve al altavoz.
 */
Choice next_choice(const Choice& current, bool headphones, bool hdmi, const std::vector<Sink>& sinks);

/**
 * La siguiente salida al pulsar A sin aparatos USB ni Bluetooth: Speaker → Headphones → HDMI
 * → Speaker…, saltando las que no hay (cascos sin enchufar, HDMI sin pantalla que acepte
 * sonido). Si `current` ya no está (han quitado los cascos), se sigue desde ahí igualmente.
 */
Output next_output(Output current, bool headphones, bool hdmi);

/** La de siempre del Cardputer: los cascos si están puestos, si no el altavoz. */
Output device_default(bool headphones);

/**
 * Si el contenido de `/proc/asound/<tarjeta vc4hdmi>/eld#0` dice que la pantalla acepta
 * sonido: al menos un formato (`sad_count` ≥ 1). Una pantalla DVI o un monitor sin altavoces
 * no lo declaran, y entonces el controlador no deja abrir el sonido. Ojo: el fichero conserva
 * la última pantalla al desenchufarla, así que hay que mirar además que haya una conectada.
 */
bool eld_accepts_audio(std::string_view eld);

/** Cuánto sube o baja el volumen cada pulsación de las teclas de volumen. */
constexpr int kVolumeStep = 5;

/**
 * El volumen en % de lo que escribe `pactl get-sink-volume` («Volume: front-left: 19660 /  30%
 * / -31.37 dB, …»): el del primer canal. -1 si no lo trae.
 */
int volume_from_pactl(std::string_view text);

/** El volumen tras una pulsación: `current` + `delta`, sin pasar de 0 ni de 100. */
int stepped_volume(int current, int delta);

/** Si `pactl get-sink-mute` dice que está en silencio («Mute: yes»). */
bool muted_from_pactl(std::string_view text);

} // namespace audio
