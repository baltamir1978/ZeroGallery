/*
 * SPDX-License-Identifier: MIT
 */

#include "audio_output.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

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
        case Output::External:
            return false; // los aparatos los recorre next_choice
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
        case Output::External:
            return Output::Speaker;
    }
    return Output::Speaker;
}

/**
 * Lo justo de JSON para leer la lista de pactl: cadenas (con sus escapes, `\u` incluido) y
 * saltar cualquier otro valor. Sin librería: sólo hacen falta tres campos.
 */
class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    bool consume(char c) {
        skip_space();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool peek(char c) {
        skip_space();
        return pos_ < text_.size() && text_[pos_] == c;
    }

    bool read_string(std::string& out) {
        out.clear();
        if (!consume('"')) {
            return false;
        }
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            const char e = text_[pos_++];
            switch (e) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) {
                        return false;
                    }
                    const unsigned code = static_cast<unsigned>(
                        std::strtoul(std::string(text_.substr(pos_, 4)).c_str(), nullptr, 16));
                    pos_ += 4;
                    append_utf8(out, code);
                    break;
                }
                default: out.push_back(e); break; // \" \\ \/
            }
        }
        return false;
    }

    /** Salta un valor cualquiera (cadena, número, objeto, lista, true/false/null). */
    bool skip_value() {
        skip_space();
        if (pos_ >= text_.size()) {
            return false;
        }
        const char c = text_[pos_];
        if (c == '"') {
            std::string ignored;
            return read_string(ignored);
        }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++pos_;
            if (consume(close)) {
                return true;
            }
            do {
                if (c == '{') {
                    std::string key;
                    if (!read_string(key) || !consume(':')) {
                        return false;
                    }
                }
                if (!skip_value()) {
                    return false;
                }
            } while (consume(','));
            return consume(close);
        }
        // Número o palabra: hasta el siguiente separador.
        const size_t start = pos_;
        while (pos_ < text_.size() && std::strchr(",}] \t\r\n", text_[pos_]) == nullptr) {
            ++pos_;
        }
        return pos_ > start;
    }

private:
    void skip_space() {
        while (pos_ < text_.size() && std::strchr(" \t\r\n", text_[pos_]) != nullptr) {
            ++pos_;
        }
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        }
        else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::string_view text_;
    size_t pos_ = 0;
};

/** Lee un objeto de la lista de pactl; `false` si el JSON está roto. */
bool read_sink(JsonReader& json, Sink& sink) {
    if (!json.consume('{')) {
        return false;
    }
    if (json.consume('}')) {
        return true;
    }
    do {
        std::string key;
        if (!json.read_string(key) || !json.consume(':')) {
            return false;
        }
        if ((key == "name" || key == "description") && json.peek('"')) {
            if (!json.read_string(key == "name" ? sink.name : sink.description)) {
                return false;
            }
        }
        else if (key == "properties" && json.peek('{')) {
            json.consume('{');
            if (json.consume('}')) {
                continue;
            }
            do {
                std::string property;
                if (!json.read_string(property) || !json.consume(':')) {
                    return false;
                }
                if ((property == "device.bus" || property == "device.api") && json.peek('"')) {
                    std::string value;
                    if (!json.read_string(value)) {
                        return false;
                    }
                    if (value == "usb" || value == "bluetooth" || value == "bluez5") {
                        sink.external = true;
                    }
                    if (property == "device.api" && value == "alsa") {
                        sink.alsa = true;
                    }
                }
                else if (!json.skip_value()) {
                    return false;
                }
            } while (json.consume(','));
            if (!json.consume('}')) {
                return false;
            }
        }
        else if (!json.skip_value()) {
            return false;
        }
    } while (json.consume(','));
    return json.consume('}');
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
        case Output::External:
            return "External";
    }
    return "";
}

std::vector<Sink> sinks_from_pactl_json(std::string_view text) {
    std::vector<Sink> sinks;
    JsonReader json(text);
    if (!json.consume('[') || json.consume(']')) {
        return sinks;
    }
    do {
        Sink sink;
        if (!read_sink(json, sink)) {
            break;
        }
        if (!sink.name.empty()) {
            if (sink.description.empty()) {
                sink.description = sink.name;
            }
            sinks.push_back(std::move(sink));
        }
    } while (json.consume(','));
    return sinks;
}

std::string internal_sink(const std::vector<Sink>& sinks) {
    for (const auto& sink : sinks) {
        if (sink.alsa && !sink.external && sink.name.find("hdmi") == std::string::npos) {
            return sink.name;
        }
    }
    return {};
}

std::string choice_name(const Choice& choice) {
    if (choice.output == Output::External) {
        return choice.label.empty() ? choice.sink : choice.label;
    }
    return output_name(choice.output);
}

Choice next_choice(const Choice& current, bool headphones, bool hdmi, const std::vector<Sink>& sinks) {
    std::vector<const Sink*> externals;
    for (const auto& sink : sinks) {
        if (sink.external) {
            externals.push_back(&sink);
        }
    }
    auto external = [](const Sink& sink) {
        return Choice{Output::External, sink.name, sink.description};
    };
    if (current.output == Output::External) {
        // El siguiente aparato tras el elegido; tras el último (o si ya no está), el altavoz.
        for (size_t i = 0; i < externals.size(); ++i) {
            if (externals[i]->name == current.sink) {
                return i + 1 < externals.size() ? external(*externals[i + 1]) : Choice{};
            }
        }
        return Choice{};
    }
    // Altavoz → cascos → HDMI como siempre; donde la vuelta volvería al altavoz, antes los
    // aparatos.
    Output candidate = after(current.output);
    while (candidate != Output::Speaker && !available(candidate, headphones, hdmi)) {
        candidate = after(candidate);
    }
    if (candidate == Output::Speaker && !externals.empty()) {
        return external(*externals.front());
    }
    return Choice{candidate, {}, {}};
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

int volume_from_pactl(std::string_view text) {
    const auto percent = text.find('%');
    if (percent == std::string_view::npos) {
        return -1;
    }
    std::size_t start = percent;
    while (start > 0 && text[start - 1] >= '0' && text[start - 1] <= '9') {
        --start;
    }
    if (start == percent) {
        return -1;
    }
    return std::atoi(std::string(text.substr(start, percent - start)).c_str());
}

int stepped_volume(int current, int delta) {
    return std::clamp(current + delta, 0, 100);
}

bool muted_from_pactl(std::string_view text) {
    return text.find("Mute: yes") != std::string_view::npos;
}

} // namespace audio
