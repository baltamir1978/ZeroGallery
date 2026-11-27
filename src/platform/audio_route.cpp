/*
 * SPDX-License-Identifier: MIT
 */

#include "audio_route.h"

#include "ffmpeg_process.h"
#include "kms_display.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __linux__
#include <sound/asound.h>
#define ZG_HAVE_ALSA_CTL 1
#endif

namespace platform {
namespace {

// Nombres de las tarjetas en el Cardputer Zero (/proc/asound/<nombre> -> cardN).
constexpr const char* kCodecCard = "ES8389Audio";
constexpr const char* kHdmiCard = "vc4hdmi";
constexpr const char* kHdmiPcm = "hdmi:CARD=vc4hdmi,DEV=0";

std::atomic<audio::Output> g_output{audio::Output::Speaker};
std::atomic<uint32_t> g_generation{0};

// Los nombres de PipeWire: el aparato elegido (con External) y la salida del códec. Los leen
// los hilos de los reproductores al lanzar ffmpeg.
std::mutex g_sinks_mutex;
audio::Choice g_choice;
std::string g_internal_sink;

/** Hasta dónde se lee la lista de pactl: unos 3 KB por salida. */
constexpr size_t kSinksMaxBytes = 256 * 1024;

/** Las salidas de PipeWire; vacío si no hay PipeWire (o se ha forzado ALSA o nada). */
std::vector<audio::Sink> list_sinks() {
    if (audio_output() != "pulse") {
        return {};
    }
    return audio::sinks_from_pactl_json(
        command_text("pactl", {"--format=json", "list", "sinks"}, kSinksMaxBytes));
}

/** A qué salida de PipeWire va lo elegido; vacío: la de por defecto. */
std::string pulse_device(const audio::Choice& choice, const std::string& internal) {
    switch (choice.output) {
        case audio::Output::External:
            return choice.sink;
        case audio::Output::Speaker:
        case audio::Output::Headphones:
            return internal;
        case audio::Output::Hdmi:
            return {};
    }
    return {};
}

/** Lo que ffmpeg tiene que saber: si cambia, hay que relanzarlo. */
std::string ffmpeg_target(const audio::Choice& choice) {
    if (choice.output == audio::Output::Hdmi) {
        return "\x01hdmi";
    }
    return choice.output == audio::Output::External ? choice.sink : std::string();
}

std::string read_text(const std::string& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/** «/proc/asound/ES8389Audio» apunta a «card1»: el número de la tarjeta, o -1. */
int card_number(const char* id) {
    char target[64] = {};
    const std::string link = std::string("/proc/asound/") + id;
    const ssize_t length = readlink(link.c_str(), target, sizeof(target) - 1);
    if (length <= 4 || std::strncmp(target, "card", 4) != 0) {
        return -1;
    }
    return std::atoi(target + 4);
}

#ifdef ZG_HAVE_ALSA_CTL

/** Abre /dev/snd/controlCN de la tarjeta `id`; -1 si no está. */
int open_control(const char* id) {
    const int card = card_number(id);
    if (card < 0) {
        return -1;
    }
    return ::open(("/dev/snd/controlC" + std::to_string(card)).c_str(), O_RDWR | O_CLOEXEC);
}

snd_ctl_elem_value control_value(int iface, const char* name) {
    snd_ctl_elem_value value{};
    value.id.iface = static_cast<unsigned int>(iface);
    std::strncpy(reinterpret_cast<char*>(value.id.name), name, sizeof(value.id.name) - 1);
    return value;
}

bool read_switch(const char* card, int iface, const char* name, bool& on) {
    const int fd = open_control(card);
    if (fd < 0) {
        return false;
    }
    auto value = control_value(iface, name);
    const bool ok = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &value) == 0;
    ::close(fd);
    if (ok) {
        on = value.value.integer.value[0] != 0;
    }
    return ok;
}

bool write_switch(const char* card, int iface, const char* name, bool on) {
    const int fd = open_control(card);
    if (fd < 0) {
        return false;
    }
    auto value = control_value(iface, name);
    value.value.integer.value[0] = on ? 1 : 0;
    const bool ok = ioctl(fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) >= 0;
    ::close(fd);
    return ok;
}

bool set_speaker(bool on) {
    return write_switch(kCodecCard, SNDRV_CTL_ELEM_IFACE_MIXER, "Speaker Switch", on);
}

#else

bool read_switch(const char*, int, const char*, bool&) {
    return false;
}
bool set_speaker(bool) {
    return false;
}
constexpr int SNDRV_CTL_ELEM_IFACE_CARD = 0;

#endif

bool speaker_touched = false;

} // namespace

AudioRoute::AudioRoute() {
    const std::string internal = audio::internal_sink(list_sinks());
    std::lock_guard<std::mutex> lock(g_sinks_mutex);
    g_internal_sink = internal;
    g_choice = audio::Choice{audio::device_default(headphones_plugged()), {}, {}};
    g_output = g_choice.output;
}

AudioRoute::~AudioRoute() {
    if (speaker_touched) {
        set_speaker(!headphones_plugged());
        speaker_touched = false;
    }
    if (g_output == audio::Output::Hdmi || g_output == audio::Output::External) {
        ++g_generation;
    }
    std::lock_guard<std::mutex> lock(g_sinks_mutex);
    g_choice = audio::Choice{};
    g_output = audio::Output::Speaker;
}

namespace {

/** La salida de PipeWire a la que van las teclas de volumen: la elegida, o la de por defecto. */
std::string volume_sink() {
    std::lock_guard<std::mutex> lock(g_sinks_mutex);
    const std::string device = pulse_device(g_choice, g_internal_sink);
    return device.empty() ? "@DEFAULT_SINK@" : device;
}

AudioRoute::Volume read_volume(const std::string& sink) {
    AudioRoute::Volume volume;
    volume.percent = audio::volume_from_pactl(command_text("pactl", {"get-sink-volume", sink}));
    volume.muted = audio::muted_from_pactl(command_text("pactl", {"get-sink-mute", sink}));
    return volume;
}

} // namespace

AudioRoute::Volume AudioRoute::change_volume(int delta) {
    const std::string sink = volume_sink();
    Volume volume = read_volume(sink);
    if (volume.percent < 0) {
        return volume;
    }
    volume.percent = audio::stepped_volume(volume.percent, delta);
    command_text("pactl", {"set-sink-volume", sink, std::to_string(volume.percent) + "%"});
    if (delta > 0 && volume.muted) {
        command_text("pactl", {"set-sink-mute", sink, "0"});
        volume.muted = false;
    }
    return volume;
}

AudioRoute::Volume AudioRoute::toggle_mute() {
    const std::string sink = volume_sink();
    Volume volume = read_volume(sink);
    if (volume.percent < 0) {
        return volume;
    }
    volume.muted = !volume.muted;
    command_text("pactl", {"set-sink-mute", sink, volume.muted ? "1" : "0"});
    return volume;
}

bool AudioRoute::headphones_plugged() {
    bool plugged = false;
    return read_switch(kCodecCard, SNDRV_CTL_ELEM_IFACE_CARD, "Headphones Jack", plugged) && plugged;
}

bool AudioRoute::hdmi_audio_available() {
    const int card = card_number(kHdmiCard);
    if (card < 0 || KmsDisplay::connected_card().empty() || access("/usr/bin/aplay", X_OK) != 0) {
        return false;
    }
    return audio::eld_accepts_audio(read_text("/proc/asound/card" + std::to_string(card) + "/eld#0"));
}

audio::Output AudioRoute::output() const {
    return g_output;
}

audio::Choice AudioRoute::choice() const {
    std::lock_guard<std::mutex> lock(g_sinks_mutex);
    return g_choice;
}

audio::Output AudioRoute::set(audio::Output output) {
    return set(audio::Choice{output, {}, {}});
}

audio::Output AudioRoute::set(const audio::Choice& wanted) {
    audio::Choice choice = wanted;
    const bool headphones = headphones_plugged();
    if ((choice.output == audio::Output::Headphones && !headphones) ||
        (choice.output == audio::Output::Hdmi && !hdmi_audio_available()) ||
        (choice.output == audio::Output::External && choice.sink.empty())) {
        choice = audio::Choice{audio::device_default(headphones), {}, {}};
    }
    if (choice.output != audio::Output::External) {
        choice.sink.clear();
        choice.label.clear();
    }
    // Altavoz o cascos: el interruptor del altavoz. Con el HDMI o un aparato no se toca: lo
    // que el Cardputer tenga puesto vuelve tal cual al dejarlo.
    if (choice.output == audio::Output::Speaker) {
        speaker_touched = set_speaker(true) || speaker_touched;
    }
    else if (choice.output == audio::Output::Headphones) {
        speaker_touched = set_speaker(false) || speaker_touched;
    }
    std::lock_guard<std::mutex> lock(g_sinks_mutex);
    const bool relaunch = ffmpeg_target(g_choice) != ffmpeg_target(choice);
    g_choice = choice;
    g_output = choice.output;
    if (relaunch) {
        ++g_generation;
    }
    return choice.output;
}

audio::Output AudioRoute::cycle() {
    // La lista se pide en cada pulsación: los aparatos Bluetooth van y vienen.
    const auto sinks = list_sinks();
    const std::string internal = audio::internal_sink(sinks);
    audio::Choice current;
    {
        std::lock_guard<std::mutex> lock(g_sinks_mutex);
        if (!internal.empty()) {
            g_internal_sink = internal;
        }
        current = g_choice;
    }
    return set(audio::next_choice(current, headphones_plugged(), hdmi_audio_available(), sinks));
}

audio::Output current_audio_output() {
    return g_output;
}

uint32_t audio_generation() {
    return g_generation;
}

HdmiAudioPipe::~HdmiAudioPipe() {
    release_ffmpeg_end();
    if (read_ >= 0) {
        ::close(read_);
    }
    aplay_.stop();
}

bool HdmiAudioPipe::start() {
    int ends[2];
    if (pipe2(ends, O_CLOEXEC) != 0) {
        return false;
    }
    read_ = ends[0];
    write_ = ends[1];
#ifdef F_SETPIPE_SZ
    // 4 KB son 21 ms de sonido: lo que se adelanta el sonido a la imagen, poco.
    fcntl(write_, F_SETPIPE_SZ, 4096);
#endif
    // 48 kHz, que acepta cualquier tele; la conversión a IEC958 la hace el PCM «hdmi».
    if (!aplay_.start({"-q", "-D", kHdmiPcm, "-t", "raw", "-f", "S16_LE", "-r", "48000", "-c", "2",
                       "--period-size=512", "--buffer-size=2048"},
                      "aplay", read_)) {
        return false;
    }
    ::close(read_);
    read_ = -1;
    return true;
}

void HdmiAudioPipe::release_ffmpeg_end() {
    if (write_ >= 0) {
        ::close(write_);
        write_ = -1;
    }
}

std::vector<std::string> audio_arguments() {
    if (g_output == audio::Output::Hdmi) {
        // En crudo por el descriptor 3, para el aplay de HdmiAudioPipe.
        return {"-vn", "-ac", "2", "-ar", "48000", "-f", "s16le", "pipe:3"};
    }
    const std::string output = audio_output();
    if (output.empty()) {
        return {};
    }
    if (output != "pulse") {
        return {"-vn", "-ac", "2", "-f", output, "default"};
    }
    std::string device;
    {
        std::lock_guard<std::mutex> lock(g_sinks_mutex);
        device = pulse_device(g_choice, g_internal_sink);
    }
    // Lo último es el nombre con que aparece en el mezclador; `-device`, la salida de PipeWire.
    if (device.empty()) {
        return {"-vn", "-ac", "2", "-f", "pulse", "Gallery"};
    }
    return {"-vn", "-ac", "2", "-f", "pulse", "-device", device, "Gallery"};
}

} // namespace platform
