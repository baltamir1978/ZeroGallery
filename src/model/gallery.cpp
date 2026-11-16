/*
 * SPDX-License-Identifier: MIT
 */

#include "gallery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <system_error>

namespace gallery {
namespace {

// Códigos de tecla de LVGL (lv_key_t). Se repiten aquí para que este fichero no dependa
// de LVGL y se pueda probar solo; tests/gallery_test.cpp no los comprueba contra lvgl.h,
// pero base_viewmodel.cpp sí (static_assert).
constexpr uint32_t kKeyUp = 17;
constexpr uint32_t kKeyDown = 18;
constexpr uint32_t kKeyRight = 19;
constexpr uint32_t kKeyLeft = 20;
constexpr uint32_t kKeyEsc = 27;
constexpr uint32_t kKeyBackspace = 8;
constexpr uint32_t kKeyEnter = 10;
constexpr uint32_t kKeyNext = 9;
constexpr uint32_t kKeyPrev = 11;
constexpr uint32_t kKeyHome = 2;
constexpr uint32_t kKeyEnd = 3;

// Los JPEG los abre libjpeg; todo lo demás, el ffmpeg del sistema. Las listas son lo que
// decodifica el ffmpeg de Debian: si un fichero concreto no se puede abrir, el visor lo
// dice y se pasa al siguiente.
constexpr std::string_view kImageExtensions[] = {
    "jpg", "jpeg", "jpe", "jfif", "png",  "apng", "gif", "bmp", "dib", "webp", "heic", "heif",
    "avif", "tif", "tiff", "jxl", "jp2",  "j2k",  "tga", "ico", "svg", "ppm",  "pgm",
    "pbm",  "pnm", "pam",  "qoi", "pcx",  "dds",  "exr", "hdr", "xbm", "xpm",  "sgi",  "dpx",
};

constexpr std::string_view kVideoExtensions[] = {
    "mp4", "m4v", "mov", "mkv",  "webm", "avi",  "mpg", "mpeg", "mpe", "m2v", "vob",  "3gp",
    "3g2", "ts",  "mts", "m2ts", "wmv",  "asf",  "flv", "f4v",  "ogv", "ogm", "rm",   "rmvb",
    "divx", "mxf", "dv", "y4m",  "nut",  "mjpeg", "h264", "h265", "hevc", "gifv",
};

// Formatos de imagen que pueden traer animación.
constexpr std::string_view kAnimatedExtensions[] = {"gif", "apng"};

std::string lower_extension(std::string_view file_name) {
    const auto dot = file_name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == file_name.size()) {
        return {};
    }
    std::string extension(file_name.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return extension;
}

template <size_t Size>
bool contains(const std::string_view (&list)[Size], const std::string& extension) {
    return std::find(std::begin(list), std::end(list), extension) != std::end(list);
}

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool is_digit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

uint16_t read16(const uint8_t* p, bool little_endian) {
    return little_endian ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                         : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read32(const uint8_t* p, bool little_endian) {
    return little_endian
               ? static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24)
               : (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                     (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

} // namespace

Action action_for_key(uint32_t key) {
    switch (key) {
        case kKeyUp:
        case 'f':
        case 'F':
            return Action::Up;
        case kKeyDown:
        case 'x':
        case 'X':
            return Action::Down;
        case kKeyLeft:
        case kKeyBackspace:
        case 'z':
        case 'Z':
            return Action::Left;
        case kKeyRight:
        case 'c':
        case 'C':
            return Action::Right;
        // Coma y punto (con «<» y «>» pintados): en la lista, una página; en el visor,
        // el fichero anterior o el siguiente, también en un vídeo, donde las flechas saltan.
        case kKeyPrev:
        case ',':
        case '<':
            return Action::PageUp;
        case kKeyNext:
        case '.':
        case '>':
            return Action::PageDown;
        case kKeyHome:
            return Action::First;
        case kKeyEnd:
            return Action::Last;
        case kKeyEnter:
        case '\r':
        case ' ':
            return Action::Select;
        case kKeyEsc:
            return Action::Back;
        case 't':
        case 'T':
            return Action::ToggleTheme;
        default:
            return Action::None;
    }
}

bool classify(std::string_view file_name, Kind& kind) {
    const auto extension = lower_extension(file_name);
    if (extension.empty()) {
        return false;
    }
    if (contains(kImageExtensions, extension)) {
        kind = Kind::Image;
        return true;
    }
    if (contains(kVideoExtensions, extension)) {
        kind = Kind::Video;
        return true;
    }
    return false;
}

bool is_animated_format(std::string_view file_name) {
    return contains(kAnimatedExtensions, lower_extension(file_name));
}

bool is_jpeg(std::string_view file_name) {
    const auto extension = lower_extension(file_name);
    return extension == "jpg" || extension == "jpeg" || extension == "jpe" || extension == "jfif";
}

bool natural_less(std::string_view a, std::string_view b) {
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            // Se comparan los números enteros, no dígito a dígito: sin ceros a la
            // izquierda, el más largo es el mayor.
            size_t start_a = i;
            size_t start_b = j;
            while (start_a < a.size() && a[start_a] == '0') ++start_a;
            while (start_b < b.size() && b[start_b] == '0') ++start_b;
            size_t end_a = start_a;
            size_t end_b = start_b;
            while (end_a < a.size() && is_digit(a[end_a])) ++end_a;
            while (end_b < b.size() && is_digit(b[end_b])) ++end_b;

            const auto digits_a = a.substr(start_a, end_a - start_a);
            const auto digits_b = b.substr(start_b, end_b - start_b);
            if (digits_a.size() != digits_b.size()) {
                return digits_a.size() < digits_b.size();
            }
            if (digits_a != digits_b) {
                return digits_a < digits_b;
            }
            i = end_a;
            j = end_b;
            continue;
        }

        const char ca = lower(a[i]);
        const char cb = lower(b[j]);
        if (ca != cb) {
            return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb);
        }
        ++i;
        ++j;
    }

    if (a.size() - i != b.size() - j) {
        return a.size() - i < b.size() - j;
    }
    // Iguales salvo mayúsculas o ceros: un orden cualquiera pero estable.
    return a < b;
}

std::vector<Entry> list_directory(const std::filesystem::path& directory) {
    std::vector<Entry> entries;
    std::error_code error;
    std::filesystem::directory_iterator it(
        directory, std::filesystem::directory_options::skip_permission_denied, error);
    if (error) {
        return entries;
    }

    for (const std::filesystem::directory_iterator end; it != end; it.increment(error)) {
        if (error) {
            break;
        }
        const auto name = it->path().filename().string();
        if (name.empty() || name.front() == '.') {
            continue;
        }

        std::error_code status_error;
        // is_directory sigue los enlaces: una carpeta enlazada también se puede abrir.
        if (it->is_directory(status_error)) {
            entries.push_back({name, Kind::Folder});
            continue;
        }
        Kind kind{};
        if (!status_error && it->is_regular_file(status_error) && classify(name, kind)) {
            entries.push_back({name, kind});
        }
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        const bool folder_a = a.kind == Kind::Folder;
        const bool folder_b = b.kind == Kind::Folder;
        if (folder_a != folder_b) {
            return folder_a;
        }
        return natural_less(a.name, b.name);
    });
    return entries;
}

Rect fit_rect(int image_width, int image_height, int screen_width, int screen_height) {
    if (image_width <= 0 || image_height <= 0 || screen_width <= 0 || screen_height <= 0) {
        return {0, 0, 0, 0};
    }

    // Se compara en enteros (producto cruzado) para no depender del redondeo de float.
    int width = screen_width;
    int height = screen_height;
    const long long by_width = static_cast<long long>(image_width) * screen_height;
    const long long by_height = static_cast<long long>(image_height) * screen_width;
    if (by_width > by_height) {
        height = static_cast<int>((static_cast<long long>(image_height) * screen_width + image_width / 2) /
                                  image_width);
    }
    else {
        width = static_cast<int>((static_cast<long long>(image_width) * screen_height + image_height / 2) /
                                 image_height);
    }
    width = std::clamp(width, 1, screen_width);
    height = std::clamp(height, 1, screen_height);
    return {(screen_width - width) / 2, (screen_height - height) / 2, width, height};
}

int exif_orientation(const uint8_t* data, size_t size) {
    constexpr uint8_t kHeader[6] = {'E', 'x', 'i', 'f', 0, 0};
    if (!data || size < 14 || !std::equal(kHeader, kHeader + 6, data)) {
        return 1;
    }

    const uint8_t* tiff = data + 6;
    const size_t tiff_size = size - 6;
    bool little_endian = false;
    if (tiff[0] == 'I' && tiff[1] == 'I') {
        little_endian = true;
    }
    else if (!(tiff[0] == 'M' && tiff[1] == 'M')) {
        return 1;
    }
    if (read16(tiff + 2, little_endian) != 42) {
        return 1;
    }

    const uint32_t ifd = read32(tiff + 4, little_endian);
    if (ifd > tiff_size || tiff_size - ifd < 2) {
        return 1;
    }
    const uint16_t count = read16(tiff + ifd, little_endian);
    for (uint16_t i = 0; i < count; ++i) {
        const size_t offset = static_cast<size_t>(ifd) + 2 + static_cast<size_t>(i) * 12;
        if (offset + 12 > tiff_size) {
            break;
        }
        if (read16(tiff + offset, little_endian) == 0x0112) {
            const uint16_t value = read16(tiff + offset + 8, little_endian);
            return value >= 1 && value <= 8 ? value : 1;
        }
    }
    return 1;
}

bool orientation_swaps_axes(int orientation) {
    return orientation >= 5 && orientation <= 8;
}

void oriented_to_stored(int orientation, int width, int height, int x, int y, int& stored_x, int& stored_y) {
    switch (orientation) {
        case 2: // espejo horizontal
            stored_x = width - 1 - x;
            stored_y = y;
            break;
        case 3: // 180°
            stored_x = width - 1 - x;
            stored_y = height - 1 - y;
            break;
        case 4: // espejo vertical
            stored_x = x;
            stored_y = height - 1 - y;
            break;
        case 5: // traspuesta
            stored_x = y;
            stored_y = x;
            break;
        case 6: // hay que girarla 90° a la derecha para verla bien
            stored_x = y;
            stored_y = height - 1 - x;
            break;
        case 7: // traspuesta por la otra diagonal
            stored_x = width - 1 - y;
            stored_y = height - 1 - x;
            break;
        case 8: // hay que girarla 90° a la izquierda
            stored_x = width - 1 - y;
            stored_y = x;
            break;
        default:
            stored_x = x;
            stored_y = y;
            break;
    }
}

int rotation_from_gravity(double sensor_x, double sensor_y, double sensor_z, int current) {
    // Ejes de la pantalla a partir de los del sensor, como los da la app oficial Compass
    // (`mapBmi270ToScreen`): derecha = Y del sensor, arriba = -X. Parado, el acelerómetro
    // marca hacia arriba (la fuerza que lo sostiene), así que (right, top) dice hacia qué
    // borde de la pantalla queda el techo.
    const double right = sensor_y;
    const double top = -sensor_x;
    const double in_plane = std::hypot(right, top);
    const double total = std::hypot(in_plane, sensor_z);
    // Menos de unos 25° levantado de la mesa: no hay un «arriba» del que fiarse.
    constexpr double kMinimumInPlane = 0.42;
    if (!(total > 0.0) || in_plane < kMinimumInPlane * total) {
        return current;
    }

    // Grados desde el borde de arriba de la pantalla hasta el techo, en el sentido del reloj.
    constexpr double kDegreesPerRadian = 57.29577951308232;
    const double angle = std::atan2(right, top) * kDegreesPerRadian;
    double from_current = std::fmod(angle - current * 90.0, 360.0);
    if (from_current > 180.0) {
        from_current -= 360.0;
    }
    else if (from_current < -180.0) {
        from_current += 360.0;
    }
    // El cambio justo sería a los 45°; se piden 60° para que cerca de la diagonal no vaya
    // y venga con el pulso.
    constexpr double kSwitchDegrees = 60.0;
    if (std::fabs(from_current) <= kSwitchDegrees) {
        return current;
    }
    const int turns = static_cast<int>(std::lround(angle / 90.0));
    return ((turns % 4) + 4) % 4;
}

int seek_seconds_for(Action action) {
    switch (action) {
        case Action::Right:
            return 10;
        case Action::Left:
            return -10;
        case Action::Up:
            return 30;
        case Action::Down:
            return -30;
        default:
            return 0;
    }
}

bool rotation_swaps_axes(int turns) {
    return (turns & 1) != 0;
}

void rotate_to_screen(const uint16_t* upright, int turns, int screen_width, int screen_height, uint16_t* screen) {
    const size_t pixels = static_cast<size_t>(screen_width) * screen_height;
    switch (((turns % 4) + 4) % 4) {
        case 1: {
            // La imagen mide screen_height de ancho: su borde de arriba va al borde derecho.
            const int width = screen_height;
            const int height = screen_width;
            for (int y = 0; y < screen_height; ++y) {
                auto* out = screen + static_cast<size_t>(y) * screen_width;
                for (int x = 0; x < screen_width; ++x) {
                    out[x] = upright[static_cast<size_t>(height - 1 - x) * width + y];
                }
            }
            break;
        }
        case 2:
            for (size_t i = 0; i < pixels; ++i) {
                screen[i] = upright[pixels - 1 - i];
            }
            break;
        case 3: {
            // Su borde de arriba va al borde izquierdo.
            const int width = screen_height;
            for (int y = 0; y < screen_height; ++y) {
                auto* out = screen + static_cast<size_t>(y) * screen_width;
                for (int x = 0; x < screen_width; ++x) {
                    out[x] = upright[static_cast<size_t>(x) * width + (width - 1 - y)];
                }
            }
            break;
        }
        default:
            std::copy(upright, upright + pixels, screen);
            break;
    }
}

Session::Session(std::filesystem::path start) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(start, error);
    if (error || !std::filesystem::is_directory(absolute, error)) {
        absolute = "/";
    }
    load(absolute.lexically_normal(), {});
}

const std::filesystem::path& Session::directory() const {
    return directory_;
}

const std::vector<Entry>& Session::entries() const {
    return entries_;
}

size_t Session::selected() const {
    return selected_;
}

bool Session::empty() const {
    return entries_.empty();
}

std::string Session::title() const {
    const auto name = directory_.filename().string();
    return name.empty() ? "/" : name;
}

void Session::move(long delta) {
    if (entries_.empty()) {
        return;
    }
    const long last = static_cast<long>(entries_.size()) - 1;
    selected_ = static_cast<size_t>(std::clamp(static_cast<long>(selected_) + delta, 0L, last));
}

void Session::select_first() {
    selected_ = 0;
}

void Session::select_last() {
    selected_ = entries_.empty() ? 0 : entries_.size() - 1;
}

const Entry* Session::selected_entry() const {
    return selected_ < entries_.size() ? &entries_[selected_] : nullptr;
}

std::filesystem::path Session::selected_path() const {
    const auto* entry = selected_entry();
    return entry ? directory_ / entry->name : std::filesystem::path{};
}

bool Session::enter_selected_folder() {
    const auto* entry = selected_entry();
    if (!entry || entry->kind != Kind::Folder) {
        return false;
    }
    load(directory_ / entry->name, {});
    return true;
}

bool Session::go_up() {
    if (!directory_.has_relative_path()) {
        return false;
    }
    const auto from = directory_.filename().string();
    load(directory_.parent_path(), from);
    return true;
}

bool Session::step_media(int direction) {
    if (direction == 0 || entries_.empty()) {
        return false;
    }
    const long step = direction > 0 ? 1 : -1;
    for (long i = static_cast<long>(selected_) + step; i >= 0 && i < static_cast<long>(entries_.size());
         i += step) {
        if (entries_[static_cast<size_t>(i)].kind != Kind::Folder) {
            selected_ = static_cast<size_t>(i);
            return true;
        }
    }
    return false;
}

void Session::media_position(size_t& index, size_t& count) const {
    index = 0;
    count = 0;
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].kind == Kind::Folder) {
            continue;
        }
        ++count;
        if (i == selected_) {
            index = count;
        }
    }
}

void Session::load(const std::filesystem::path& directory, const std::string& select_name) {
    directory_ = directory;
    entries_ = list_directory(directory_);
    selected_ = 0;
    if (!select_name.empty()) {
        const auto it = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const Entry& entry) { return entry.name == select_name; });
        if (it != entries_.end()) {
            selected_ = static_cast<size_t>(it - entries_.begin());
        }
    }
}

} // namespace gallery
