/*
 * SPDX-License-Identifier: MIT
 */

#include "audio_route.h"

#include "ffmpeg_process.h"
#include "kms_display.h"

#include <atomic>
#include <cstring>
#include <fstream>
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
    g_output = audio::device_default(headphones_plugged());
}

AudioRoute::~AudioRoute() {
    if (speaker_touched) {
        set_speaker(!headphones_plugged());
        speaker_touched = false;
    }
    if (g_output == audio::Output::Hdmi) {
        ++g_generation;
    }
    g_output = audio::Output::Speaker;
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

audio::Output AudioRoute::set(audio::Output output) {
    const bool headphones = headphones_plugged();
    if ((output == audio::Output::Headphones && !headphones) ||
        (output == audio::Output::Hdmi && !hdmi_audio_available())) {
        output = audio::device_default(headphones);
    }
    const audio::Output before = g_output;
    // Altavoz o cascos: el interruptor del altavoz. Con el HDMI no se toca: lo que el
    // Cardputer tenga puesto vuelve tal cual al dejarlo.
    if (output == audio::Output::Speaker) {
        speaker_touched = set_speaker(true) || speaker_touched;
    }
    else if (output == audio::Output::Headphones) {
        speaker_touched = set_speaker(false) || speaker_touched;
    }
    g_output = output;
    if ((before == audio::Output::Hdmi) != (output == audio::Output::Hdmi)) {
        ++g_generation;
    }
    return output;
}

audio::Output AudioRoute::cycle() {
    return set(audio::next_output(g_output, headphones_plugged(), hdmi_audio_available()));
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
    // En «pulse» lo último es el nombre con que aparece en el mezclador; en «alsa», el dispositivo.
    return {"-vn", "-ac", "2", "-f", output, output == "pulse" ? "Gallery" : "default"};
}

} // namespace platform
