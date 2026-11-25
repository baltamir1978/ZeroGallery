/*
 * SPDX-License-Identifier: MIT
 *
 * Pruebas de la elección de la salida de sonido.
 */

#include "audio_output.h"

#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FALLA %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

using audio::Output;

void test_next_output() {
    // Todo disponible: la vuelta entera.
    CHECK(audio::next_output(Output::Speaker, true, true) == Output::Headphones);
    CHECK(audio::next_output(Output::Headphones, true, true) == Output::Hdmi);
    CHECK(audio::next_output(Output::Hdmi, true, true) == Output::Speaker);
    // Sin cascos: altavoz ↔ HDMI.
    CHECK(audio::next_output(Output::Speaker, false, true) == Output::Hdmi);
    CHECK(audio::next_output(Output::Hdmi, false, true) == Output::Speaker);
    // Sin HDMI: altavoz ↔ cascos.
    CHECK(audio::next_output(Output::Speaker, true, false) == Output::Headphones);
    CHECK(audio::next_output(Output::Headphones, true, false) == Output::Speaker);
    // Nada más que el altavoz: se queda.
    CHECK(audio::next_output(Output::Speaker, false, false) == Output::Speaker);
    // Estaba en cascos y los han quitado: sigue al HDMI o vuelve al altavoz.
    CHECK(audio::next_output(Output::Headphones, false, true) == Output::Hdmi);
    CHECK(audio::next_output(Output::Headphones, false, false) == Output::Speaker);
    // Estaba en HDMI y la han desenchufado.
    CHECK(audio::next_output(Output::Hdmi, true, false) == Output::Speaker);
}

void test_default_and_names() {
    CHECK(audio::device_default(true) == Output::Headphones);
    CHECK(audio::device_default(false) == Output::Speaker);
    CHECK(std::string(audio::output_name(Output::Hdmi)) == "HDMI");
}

void test_eld() {
    // /proc/asound/card0/eld#0 real en el Cardputer, con una tele simulada (EDID con sonido).
    const char* with_audio = "monitor_name\t\tZG 1920x1200\nconnection_type\t\tHDMI\n"
                             "speakers\t\t[0x1] FL/FR\nsad_count\t\t1\nsad0_coding_type\t[0x1] LPCM\n";
    CHECK(audio::eld_accepts_audio(with_audio));
    // Monitor sin sonido (DVI): sin formatos.
    CHECK(!audio::eld_accepts_audio("monitor_name\t\tMonitor\nsad_count\t\t0\n"));
    CHECK(!audio::eld_accepts_audio(""));
    // «sad_count_x» no es «sad_count».
    CHECK(!audio::eld_accepts_audio("sad_count_x 2\n"));
}

} // namespace

int main() {
    test_next_output();
    test_default_and_names();
    test_eld();
    if (failures) {
        std::printf("%d fallos\n", failures);
        return 1;
    }
    std::printf("audio_test: todo bien\n");
    return 0;
}
