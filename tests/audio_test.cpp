/*
 * SPDX-License-Identifier: MIT
 *
 * Pruebas de la elección de la salida de sonido.
 */

#include "audio_output.h"

#include <cstdio>
#include <string>
#include <vector>

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

// Como lo escribe `pactl --format=json list sinks` (PulseAudio 17 sobre pipewire-pulse),
// recortado: el códec del Cardputer, una tarjeta USB y unos cascos Bluetooth. Los puertos
// también tienen «name» y «description», y no deben confundirse con los de la salida.
const char* kSinksJson = R"([{"index":45,"state":"SUSPENDED","name":"alsa_output.platform-soc_sound.stereo-fallback","description":"ES8389Audio Estéreo","driver":"PipeWire","sample_specification":"s32le 2ch 48000Hz","channel_map":"front-left,front-right","owner_module":4294967295,"mute":false,"volume":{"front-left":{"value":19660,"value_percent":"30%","db":"-31.37 dB"},"front-right":{"value":19660,"value_percent":"30%","db":"-31.37 dB"}},"balance":0.0,"base_volume":{"value":65536,"value_percent":"100%","db":"0.00 dB"},"monitor_source":"alsa_output.platform-soc_sound.stereo-fallback.monitor","latency":{"actual":0.0,"configured":0.0},"flags":["HARDWARE","HW_MUTE_CTRL","HW_VOLUME_CTRL","DECIBEL_VOLUME","LATENCY"],"properties":{"alsa.card":"1","alsa.card_name":"ES8389Audio","device.api":"alsa","device.bus":"platform","node.name":"alsa_output.platform-soc_sound.stereo-fallback"},"ports":[{"name":"analog-output-speaker","description":"Speakers","type":"Speaker","priority":10000,"availability_group":"","availability":"availability unknown"}],"active_port":"analog-output-speaker","formats":["pcm"]},{"index":71,"state":"IDLE","name":"alsa_output.usb-C-Media_Electronics_Inc._USB_Audio_Device-00.analog-stereo","description":"USB Audio Device Estéreo analógico","driver":"PipeWire","mute":false,"properties":{"alsa.card":"2","device.api":"alsa","device.bus":"usb","device.vendor.name":"C-Media Electronics Inc."},"ports":[{"name":"analog-output-headphones","description":"Headphones","type":"Headphones"}],"active_port":"analog-output-headphones","formats":["pcm"]},{"index":80,"state":"RUNNING","name":"bluez_output.AA_BB_CC_DD_EE_FF.1","description":"Cascos \"Sony\" \u00e9","driver":"PipeWire","mute":false,"properties":{"api.bluez5.address":"AA:BB:CC:DD:EE:FF","device.api":"bluez5","bluez5.profile":"a2dp-sink"},"ports":[],"active_port":null,"formats":["pcm"]}])";

void test_sinks() {
    const auto sinks = audio::sinks_from_pactl_json(kSinksJson);
    CHECK(sinks.size() == 3);
    if (sinks.size() == 3) {
        CHECK(sinks[0].name == "alsa_output.platform-soc_sound.stereo-fallback");
        CHECK(sinks[0].description == "ES8389Audio Estéreo");
        CHECK(!sinks[0].external);
        CHECK(sinks[1].external); // device.bus = usb
        CHECK(sinks[1].description == "USB Audio Device Estéreo analógico");
        CHECK(sinks[2].external); // device.api = bluez5, sin device.bus
        CHECK(sinks[2].name == "bluez_output.AA_BB_CC_DD_EE_FF.1");
        CHECK(sinks[2].description == "Cascos \"Sony\" \xC3\xA9"); // escapes y \u00e9
    }
    CHECK(audio::internal_sink(sinks) == "alsa_output.platform-soc_sound.stereo-fallback");
    CHECK(audio::sinks_from_pactl_json("").empty());
    CHECK(audio::sinks_from_pactl_json("[]").empty());
    CHECK(audio::sinks_from_pactl_json("Connection failure: Connection refused").empty());
    // Roto a medias: lo leído hasta ahí.
    const std::string cut = std::string(kSinksJson).substr(0, std::string(kSinksJson).find("{\"index\":80"));
    CHECK(audio::sinks_from_pactl_json(cut).size() == 2);
    // Sin el códec: no hay salida propia.
    std::vector<audio::Sink> only_external{{"bluez_output.X.1", "BT", true, false}};
    CHECK(audio::internal_sink(only_external).empty());
    std::vector<audio::Sink> with_hdmi{{"alsa_output.platform-hdmi.stereo", "HDMI", false, true}};
    CHECK(audio::internal_sink(with_hdmi).empty());
}

// La de verdad del Cardputer Zero (27-nov-2026, `pactl --format=json list sinks`), con la
// pantalla recién apagada: la salida muda de ZeroPower va primero y no es la del códec.
void test_real_cardputer_sinks() {
    std::FILE* file = std::fopen(FIXTURES_DIR "/pactl-sinks-cardputer.json", "rb");
    CHECK(file != nullptr);
    if (!file) {
        return;
    }
    std::string json;
    char chunk[4096];
    size_t count;
    while ((count = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        json.append(chunk, count);
    }
    std::fclose(file);
    const auto sinks = audio::sinks_from_pactl_json(json);
    CHECK(sinks.size() == 2);
    if (sinks.size() == 2) {
        CHECK(sinks[0].name == "zeropower_null");
        CHECK(!sinks[0].alsa && !sinks[0].external);
        CHECK(sinks[1].description == "Built-in Audio Stereo");
        CHECK(sinks[1].alsa && !sinks[1].external);
    }
    CHECK(audio::internal_sink(sinks) == "alsa_output.platform-sound.stereo-fallback");
    // Ni la muda ni el códec son aparatos para la tecla A.
    CHECK(audio::next_choice(audio::Choice{}, false, false, sinks) == audio::Choice{});
}

void test_next_choice() {
    using audio::Choice;
    const auto sinks = audio::sinks_from_pactl_json(kSinksJson);
    const Choice speaker{};
    const Choice headphones{Output::Headphones, {}, {}};
    const Choice hdmi{Output::Hdmi, {}, {}};
    const Choice usb{Output::External, sinks[1].name, sinks[1].description};
    const Choice bt{Output::External, sinks[2].name, sinks[2].description};
    // Todo: altavoz → cascos → HDMI → USB → Bluetooth → altavoz (el códec no sale: es el altavoz).
    CHECK(audio::next_choice(speaker, true, true, sinks) == headphones);
    CHECK(audio::next_choice(headphones, true, true, sinks) == hdmi);
    CHECK(audio::next_choice(hdmi, true, true, sinks) == usb);
    CHECK(audio::next_choice(usb, true, true, sinks) == bt);
    CHECK(audio::next_choice(bt, true, true, sinks) == speaker);
    // Sólo el altavoz y los aparatos.
    CHECK(audio::next_choice(speaker, false, false, sinks) == usb);
    CHECK(audio::next_choice(headphones, false, false, sinks) == usb);
    // El rótulo: el nombre del aparato.
    CHECK(audio::choice_name(audio::next_choice(usb, false, false, sinks)) == sinks[2].description);
    CHECK(audio::choice_name(speaker) == "Speaker");
    // Se ha desconectado el elegido: al altavoz.
    const Choice gone{Output::External, "bluez_output.11_22.1", "Old"};
    CHECK(audio::next_choice(gone, true, true, sinks) == speaker);
    // Sin aparatos, como antes.
    const std::vector<audio::Sink> none;
    CHECK(audio::next_choice(speaker, true, false, none) == headphones);
    CHECK(audio::next_choice(speaker, false, false, none) == speaker);
    CHECK(audio::next_choice(hdmi, false, true, none) == speaker);
}

} // namespace

void test_volume() {
    CHECK(audio::volume_from_pactl(
              "Volume: front-left: 19660 /  30% / -31.37 dB,   front-right: 19660 /  30% / -31.37 dB\n"
              "        balance 0.00\n") == 30);
    CHECK(audio::volume_from_pactl("Volume: mono: 65536 / 100% / 0.00 dB") == 100);
    CHECK(audio::volume_from_pactl("Volume: front-left: 0 /   0% / -inf dB") == 0);
    CHECK(audio::volume_from_pactl("") == -1);
    CHECK(audio::volume_from_pactl("Connection failure: Connection refused") == -1);
    CHECK(audio::stepped_volume(30, audio::kVolumeStep) == 35);
    CHECK(audio::stepped_volume(30, -audio::kVolumeStep) == 25);
    CHECK(audio::stepped_volume(98, 5) == 100); // pactl deja pasar de 100 %: aquí no
    CHECK(audio::stepped_volume(3, -5) == 0);
    CHECK(audio::stepped_volume(120, 5) == 100);
    CHECK(audio::muted_from_pactl("Mute: yes\n"));
    CHECK(!audio::muted_from_pactl("Mute: no\n"));
    CHECK(!audio::muted_from_pactl(""));
}

int main() {
    test_next_output();
    test_sinks();
    test_real_cardputer_sinks();
    test_next_choice();
    test_volume();
    test_default_and_names();
    test_eld();
    if (failures) {
        std::printf("%d fallos\n", failures);
        return 1;
    }
    std::printf("audio_test: todo bien\n");
    return 0;
}
