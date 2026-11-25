/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <string>
#include <string_view>

/**
 * Por dónde suena el vídeo: el altavoz del Cardputer, los cascos o la tele por HDMI. Lo que
 * se decide sin tocar el hardware; el cambio de verdad está en platform/audio_route.*.
 */
namespace audio {

enum class Output {
    Speaker,
    Headphones,
    Hdmi,
};

/** Para el rótulo: «Speaker», «Headphones», «HDMI». */
const char* output_name(Output output);

/**
 * La siguiente salida al pulsar A: Speaker → Headphones → HDMI → Speaker…, saltando las que
 * no hay (cascos sin enchufar, HDMI sin pantalla que acepte sonido). Si `current` ya no está
 * (han quitado los cascos), se sigue desde ahí igualmente.
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

} // namespace audio
