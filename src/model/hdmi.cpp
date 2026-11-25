/*
 * SPDX-License-Identifier: MIT
 */

#include "hdmi.h"

#include <algorithm>
#include <cstdlib>
#include <string>

namespace hdmi {
namespace {

constexpr int kMaxWidth = 1920;
constexpr int kMaxHeight = 1200;

bool fits(const Mode& mode) {
    return mode.width > 0 && mode.height > 0 && mode.width <= kMaxWidth && mode.height <= kMaxHeight &&
           !mode.interlaced;
}

int even_down(int value) {
    return std::max(2, value & ~1);
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

int pick_mode(const std::vector<Mode>& modes) {
    for (size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].preferred && fits(modes[i])) {
            return static_cast<int>(i);
        }
    }
    int best = -1;
    const auto score = [](const Mode& mode) {
        // Primero el tamaño; a igual tamaño, 50-60 Hz antes que 24-30 (un vídeo a 25 fps se
        // ve igual y la interfaz de la tele, si la hay, no parpadea), y luego lo más alto.
        const long long pixels = static_cast<long long>(mode.width) * mode.height;
        const int smooth = mode.refresh >= 50 && mode.refresh <= 60 ? 1 : 0;
        return pixels * 1000 + smooth * 100 + std::min(mode.refresh, 99);
    };
    for (size_t i = 0; i < modes.size(); ++i) {
        if (!fits(modes[i])) {
            continue;
        }
        if (best < 0 || score(modes[i]) > score(modes[static_cast<size_t>(best)])) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

Size video_size(int width, int height, int screen_width, int screen_height) {
    if (width <= 0 || height <= 0 || screen_width <= 0 || screen_height <= 0) {
        return {even_down(screen_width), even_down(screen_height)};
    }
    if (width <= screen_width && height <= screen_height) {
        return {even_down(width), even_down(height)};
    }
    // Producto cruzado en enteros, como gallery::fit_rect.
    const long long by_width = static_cast<long long>(width) * screen_height;
    const long long by_height = static_cast<long long>(height) * screen_width;
    int out_width = screen_width;
    int out_height = screen_height;
    if (by_width > by_height) {
        out_height = static_cast<int>((static_cast<long long>(height) * screen_width + width / 2) / width);
    }
    else {
        out_width = static_cast<int>((static_cast<long long>(width) * screen_height + height / 2) / height);
    }
    return {even_down(out_width), even_down(out_height)};
}

double parse_rate(std::string_view text) {
    text = trim(text);
    const auto slash = text.find('/');
    const std::string top(text.substr(0, slash));
    const double numerator = std::atof(top.c_str());
    double denominator = 1.0;
    if (slash != std::string_view::npos) {
        const std::string bottom(text.substr(slash + 1));
        denominator = std::atof(bottom.c_str());
    }
    if (numerator <= 0.0 || denominator <= 0.0) {
        return 0.0;
    }
    return numerator / denominator;
}

VideoInfo parse_probe(std::string_view text) {
    VideoInfo info;
    double average_rate = 0.0;
    double real_rate = 0.0;
    while (!text.empty()) {
        const auto end = text.find('\n');
        const std::string_view line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);

        const auto equal = line.find('=');
        if (equal == std::string_view::npos) {
            continue;
        }
        const std::string_view key = line.substr(0, equal);
        const std::string value(line.substr(equal + 1));
        // ffprobe da el primer valor del flujo y, si se le pide, también el del formato:
        // se queda el primero que tenga sentido.
        if (key == "codec_name" && info.codec.empty()) {
            info.codec = value;
        }
        else if (key == "width" && info.width == 0) {
            info.width = std::atoi(value.c_str());
        }
        else if (key == "height" && info.height == 0) {
            info.height = std::atoi(value.c_str());
        }
        else if (key == "rotation" && info.rotation == 0) {
            info.rotation = std::atoi(value.c_str());
        }
        else if (key == "avg_frame_rate" && average_rate == 0.0) {
            average_rate = parse_rate(value);
        }
        else if (key == "r_frame_rate" && real_rate == 0.0) {
            real_rate = parse_rate(value);
        }
        else if (key == "duration" && info.duration <= 0.0) {
            info.duration = std::max(0.0, std::atof(value.c_str()));
        }
    }
    // avg_frame_rate es lo que de verdad saca ffmpeg con un vídeo de móvil de cadencia
    // variable; r_frame_rate, si no hay otra cosa. Algo absurdo (más de 120) no vale.
    info.fps = average_rate > 0.0 && average_rate <= 120.0 ? average_rate
               : real_rate > 0.0 && real_rate <= 120.0    ? real_rate
                                                          : 0.0;
    const int quarter = ((info.rotation % 360) + 360) % 360;
    if (quarter == 90 || quarter == 270) {
        std::swap(info.width, info.height);
    }
    return info;
}

bool hardware_decodable(const VideoInfo& info) {
    const int longest = std::max(info.width, info.height);
    const int shortest = std::min(info.width, info.height);
    const int quarter = ((info.rotation % 360) + 360) % 360;
    return info.codec == "h264" && quarter == 0 && shortest > 0 && longest <= 1920 && shortest <= 1088;
}

Size hardware_size(const VideoInfo& info) {
    return {even_down(info.width), even_down(info.height)};
}

double max_fps(bool hardware, int width, int height) {
    return hardware && static_cast<long long>(width) * height <= 1280LL * 720 ? 60.0 : 30.0;
}

bool status_connected(std::string_view text) {
    return trim(text) == "connected";
}

} // namespace hdmi
