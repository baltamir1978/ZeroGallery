/*
 * SPDX-License-Identifier: MIT
 */

#include "audio_output.h"

#include <cstdlib>

namespace audio {
namespace {

bool available(Output output, bool headphones, bool hdmi) {
    switch (output) {
        case Output::Speaker:
            return true;
        case Output::Headphones:
            return headphones;
        case Output::Hdmi:
            return hdmi;
    }
    return false;
}

Output after(Output output) {
    switch (output) {
        case Output::Speaker:
            return Output::Headphones;
        case Output::Headphones:
            return Output::Hdmi;
        case Output::Hdmi:
            return Output::Speaker;
    }
    return Output::Speaker;
}

/** Valor de `key` en una línea «key<espacios>valor» del ELD; vacío si no está. */
std::string_view eld_value(std::string_view eld, std::string_view key) {
    size_t start = 0;
    while (start < eld.size()) {
        size_t end = eld.find('\n', start);
        if (end == std::string_view::npos) {
            end = eld.size();
        }
        std::string_view line = eld.substr(start, end - start);
        start = end + 1;
        if (line.substr(0, key.size()) != key || line.size() == key.size() ||
            (line[key.size()] != ' ' && line[key.size()] != '\t')) {
            continue;
        }
        line.remove_prefix(key.size());
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.remove_suffix(1);
        }
        return line;
    }
    return {};
}

} // namespace

const char* output_name(Output output) {
    switch (output) {
        case Output::Speaker:
            return "Speaker";
        case Output::Headphones:
            return "Headphones";
        case Output::Hdmi:
            return "HDMI";
    }
    return "";
}

Output next_output(Output current, bool headphones, bool hdmi) {
    Output candidate = after(current);
    // Como mucho dos saltos: el altavoz siempre está.
    while (!available(candidate, headphones, hdmi)) {
        candidate = after(candidate);
    }
    return candidate;
}

Output device_default(bool headphones) {
    return headphones ? Output::Headphones : Output::Speaker;
}

bool eld_accepts_audio(std::string_view eld) {
    const std::string count(eld_value(eld, "sad_count"));
    return !count.empty() && std::atoi(count.c_str()) >= 1;
}

} // namespace audio
