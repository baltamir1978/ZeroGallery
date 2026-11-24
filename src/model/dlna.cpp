/*
 * SPDX-License-Identifier: MIT
 */

#include "dlna.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace dlna {
namespace {

char lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string to_lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), lower);
    return result;
}

bool starts_with_nocase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (lower(text[i]) != lower(prefix[i])) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::string extension_of(std::string_view file_name) {
    const auto dot = file_name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 == file_name.size()) {
        return {};
    }
    return to_lower(file_name.substr(dot + 1));
}

/**
 * Cabeceras «Nombre: valor» desde `text` (tras la primera línea) hasta la línea en blanco,
 * con los nombres en minúsculas. Devuelve dónde acaba la cabecera (tras la línea en blanco).
 */
size_t parse_headers(std::string_view text, size_t from, std::map<std::string, std::string>& headers) {
    size_t pos = from;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        pos = end + 1;
        if (line.empty()) {
            return pos;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        headers[to_lower(trim(line.substr(0, colon)))] = std::string(trim(line.substr(colon + 1)));
    }
    return text.size();
}

/** Primera línea y resto: `false` si no hay ni una línea completa. */
bool split_first_line(std::string_view text, std::string_view& first, size_t& rest) {
    const auto end = text.find('\n');
    if (end == std::string_view::npos) {
        return false;
    }
    first = text.substr(0, end);
    if (!first.empty() && first.back() == '\r') {
        first.remove_suffix(1);
    }
    rest = end + 1;
    return true;
}

/**
 * Busca desde `from` el siguiente elemento llamado `name` (con o sin prefijo). Deja en
 * `content_begin`/`content_end` su contenido y en `after` dónde sigue el documento.
 */
bool find_element(std::string_view xml,
                  std::string_view name,
                  size_t from,
                  size_t& start,
                  size_t& content_begin,
                  size_t& content_end,
                  size_t& after) {
    size_t pos = from;
    while ((pos = xml.find('<', pos)) != std::string_view::npos) {
        size_t name_begin = pos + 1;
        if (name_begin < xml.size() && (xml[name_begin] == '/' || xml[name_begin] == '?' || xml[name_begin] == '!')) {
            ++pos;
            continue;
        }
        size_t name_end = name_begin;
        while (name_end < xml.size() && !std::isspace(static_cast<unsigned char>(xml[name_end])) &&
               xml[name_end] != '>' && xml[name_end] != '/') {
            ++name_end;
        }
        std::string_view tag = xml.substr(name_begin, name_end - name_begin);
        const std::string_view full_tag = tag;
        if (const auto colon = tag.find(':'); colon != std::string_view::npos) {
            tag.remove_prefix(colon + 1);
        }
        if (tag != name) {
            ++pos;
            continue;
        }
        const auto close = xml.find('>', name_end);
        if (close == std::string_view::npos) {
            return false;
        }
        start = pos;
        if (xml[close - 1] == '/') { // <nombre/>
            content_begin = content_end = after = close + 1;
            return true;
        }
        content_begin = close + 1;
        const std::string end_tag = "</" + std::string(full_tag);
        // «</service» también empieza «</serviceType>»: tiene que seguir «>» o un espacio.
        size_t end = content_begin;
        while ((end = xml.find(end_tag, end)) != std::string_view::npos) {
            const size_t next = end + end_tag.size();
            if (next < xml.size() && (xml[next] == '>' || std::isspace(static_cast<unsigned char>(xml[next])))) {
                break;
            }
            ++end;
        }
        if (end == std::string_view::npos) {
            return false;
        }
        content_end = end;
        const auto end_close = xml.find('>', end);
        after = end_close == std::string_view::npos ? xml.size() : end_close + 1;
        return true;
    }
    return false;
}

/** Valor del último elemento `name` que empieza antes de `limit`. */
std::string last_value_before(std::string_view xml, std::string_view name, size_t limit) {
    std::string value;
    size_t from = 0;
    size_t start = 0;
    size_t begin = 0;
    size_t end = 0;
    size_t after = 0;
    while (find_element(xml, name, from, start, begin, end, after) && start < limit) {
        value = xml_unescape(trim(xml.substr(begin, end - begin)));
        from = after;
    }
    return value;
}

uint16_t read16(const uint8_t* p, bool little_endian) {
    return little_endian ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read32(const uint8_t* p, bool little_endian) {
    return little_endian ? static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24)
                         : (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/** Tipos MIME que las teles aceptan en lugar de uno dado: mismo contenedor, otro nombre. */
std::vector<std::string_view> mime_alternatives(std::string_view mime) {
    if (mime == "video/quicktime" || mime == "video/3gpp" || mime == "video/3gpp2" || mime == "video/x-m4v") {
        return {"video/mp4"};
    }
    if (mime == "video/x-msvideo") {
        return {"video/avi", "video/x-avi", "video/x-ms-avi", "video/divx"};
    }
    if (mime == "video/mp2t") {
        return {"video/vnd.dlna.mpeg-tts", "video/mp2ts", "video/mts", "video/mpeg"};
    }
    if (mime == "video/x-matroska") {
        return {"video/x-mkv", "video/mkv"};
    }
    if (mime == "video/mpeg") {
        return {"video/mpeg2", "video/x-mpeg"};
    }
    if (mime == "video/x-ms-wmv") {
        return {"video/x-ms-asf"};
    }
    if (mime == "video/x-flv") {
        return {"video/flv"};
    }
    if (mime == "audio/flac") {
        return {"audio/x-flac"};
    }
    if (mime == "audio/mp4") {
        return {"audio/x-m4a", "audio/m4a", "audio/aac"};
    }
    if (mime == "audio/aac") {
        return {"audio/vnd.dlna.adts", "audio/x-aac", "audio/mp4"};
    }
    if (mime == "audio/ogg") {
        return {"audio/x-ogg", "application/ogg"};
    }
    if (mime == "audio/wav") {
        return {"audio/x-wav", "audio/wave"};
    }
    if (mime == "audio/x-ms-wma") {
        return {"audio/wma"};
    }
    if (mime == "audio/x-aiff") {
        return {"audio/aiff"};
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------- descubrir teles

// MediaRenderer es lo que se busca; algunas sólo contestan si se pregunta por el servicio
// (AVTransport) y otras sólo a rootdevice. Lo que no tenga AVTransport se descarta al leer
// la descripción.
const char* const kSearchTargets[3] = {
    "urn:schemas-upnp-org:device:MediaRenderer:1",
    "urn:schemas-upnp-org:service:AVTransport:1",
    "upnp:rootdevice",
};

std::string msearch_request(std::string_view search_target) {
    std::string request = "M-SEARCH * HTTP/1.1\r\n"
                          "HOST: 239.255.255.250:1900\r\n"
                          "MAN: \"ssdp:discover\"\r\n"
                          "MX: 2\r\n"
                          "ST: ";
    request += search_target;
    request += "\r\nUSER-AGENT: Linux/6 UPnP/1.0 ZeroGallery/1\r\n\r\n";
    return request;
}

bool parse_ssdp_response(std::string_view message, std::string& location, std::string& usn) {
    std::string_view first;
    size_t rest = 0;
    if (!split_first_line(message, first, rest)) {
        return false;
    }
    const bool reply = starts_with_nocase(first, "HTTP/1.") && first.find(" 200") != std::string_view::npos;
    const bool notify = starts_with_nocase(first, "NOTIFY ");
    if (!reply && !notify) {
        return false;
    }
    std::map<std::string, std::string> headers;
    parse_headers(message, rest, headers);
    if (notify) {
        // Un anuncio de que se va (ssdp:byebye) no lleva a ningún sitio.
        const auto nts = headers.find("nts");
        if (nts != headers.end() && nts->second.find("byebye") != std::string::npos) {
            return false;
        }
    }
    const auto found = headers.find("location");
    if (found == headers.end() || found->second.empty()) {
        return false;
    }
    location = found->second;
    const auto id = headers.find("usn");
    usn = id == headers.end() ? std::string() : id->second;
    return true;
}

bool parse_url(std::string_view text, Url& url) {
    text = trim(text);
    if (!starts_with_nocase(text, "http://")) {
        return false;
    }
    text.remove_prefix(7);
    const auto slash = text.find('/');
    std::string_view authority = text.substr(0, slash);
    url.path = slash == std::string_view::npos ? "/" : std::string(text.substr(slash));
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        authority.remove_prefix(at + 1);
    }
    const auto colon = authority.rfind(':');
    url.port = 80;
    if (colon != std::string_view::npos) {
        const std::string port(authority.substr(colon + 1));
        char* end = nullptr;
        const long value = std::strtol(port.c_str(), &end, 10);
        if (port.empty() || *end != '\0' || value <= 0 || value > 65535) {
            return false;
        }
        url.port = static_cast<int>(value);
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) {
        return false;
    }
    url.host = std::string(authority);
    return true;
}

std::string resolve_url(std::string_view base, std::string_view reference) {
    reference = trim(reference);
    if (starts_with_nocase(reference, "http://") || starts_with_nocase(reference, "https://")) {
        return std::string(reference);
    }
    Url parsed;
    if (!parse_url(base, parsed)) {
        return std::string(reference);
    }
    std::string origin = "http://" + parsed.host + ":" + std::to_string(parsed.port);
    if (reference.empty()) {
        return origin + parsed.path;
    }
    if (reference.front() == '/') {
        return origin + std::string(reference);
    }
    std::string directory = parsed.path.substr(0, parsed.path.find('?'));
    directory = directory.substr(0, directory.rfind('/') + 1);
    if (directory.empty()) {
        directory = "/";
    }
    return origin + directory + std::string(reference);
}

bool parse_description(std::string_view xml, std::string_view location, Renderer& renderer) {
    std::string base(location);
    std::string url_base;
    if (xml_value(xml, "URLBase", url_base) && !url_base.empty()) {
        base = url_base;
    }

    renderer = Renderer{};
    size_t from = 0;
    size_t start = 0;
    size_t begin = 0;
    size_t end = 0;
    size_t after = 0;
    size_t av_start = std::string_view::npos;
    while (find_element(xml, "service", from, start, begin, end, after)) {
        const std::string_view service = xml.substr(begin, end - begin);
        std::string type;
        std::string control;
        if (xml_value(service, "serviceType", type) && xml_value(service, "controlURL", control)) {
            if (type.find(":service:AVTransport:") != std::string::npos && renderer.av_transport.empty()) {
                renderer.av_transport = resolve_url(base, control);
                av_start = start;
            }
            else if (type.find(":service:RenderingControl:") != std::string::npos) {
                if (renderer.rendering_control.empty() || av_start == std::string_view::npos) {
                    renderer.rendering_control = resolve_url(base, control);
                }
            }
            else if (type.find(":service:ConnectionManager:") != std::string::npos) {
                // El de la misma tele: el primero que haya después de su AVTransport, o el
                // último de antes si va delante.
                if (renderer.connection_manager.empty() || av_start == std::string_view::npos) {
                    renderer.connection_manager = resolve_url(base, control);
                }
            }
        }
        from = after;
    }
    if (renderer.av_transport.empty()) {
        return false;
    }
    // El nombre es el del aparato que contiene el servicio: el último que se ha abierto
    // antes de él (una descripción puede traer aparatos dentro de otros).
    renderer.name = last_value_before(xml, "friendlyName", av_start);
    renderer.manufacturer = last_value_before(xml, "manufacturer", av_start);
    renderer.model = last_value_before(xml, "modelName", av_start);
    renderer.udn = last_value_before(xml, "UDN", av_start);
    if (renderer.name.empty()) {
        renderer.name = renderer.model.empty() ? std::string(location) : renderer.model;
    }
    if (renderer.udn.empty()) {
        renderer.udn = renderer.av_transport;
    }
    return true;
}

// ---------------------------------------------------------------- XML

std::string xml_escape(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 16);
    for (const char c : text) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c; break;
        }
    }
    return result;
}

std::string xml_unescape(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            result += text[i];
            continue;
        }
        const auto semicolon = text.find(';', i);
        if (semicolon == std::string_view::npos || semicolon - i > 10) {
            result += '&';
            continue;
        }
        const std::string_view entity = text.substr(i + 1, semicolon - i - 1);
        unsigned long code = 0;
        bool known = true;
        if (entity == "amp") code = '&';
        else if (entity == "lt") code = '<';
        else if (entity == "gt") code = '>';
        else if (entity == "quot") code = '"';
        else if (entity == "apos") code = '\'';
        else if (!entity.empty() && entity[0] == '#') {
            const std::string number(entity.substr(1));
            char* end = nullptr;
            code = (!number.empty() && (number[0] == 'x' || number[0] == 'X'))
                       ? std::strtoul(number.c_str() + 1, &end, 16)
                       : std::strtoul(number.c_str(), &end, 10);
            known = end && *end == '\0' && code > 0 && code <= 0x10FFFF;
        }
        else {
            known = false;
        }
        if (!known) {
            result += '&';
            continue;
        }
        // A UTF-8.
        if (code < 0x80) {
            result += static_cast<char>(code);
        }
        else if (code < 0x800) {
            result += static_cast<char>(0xC0 | (code >> 6));
            result += static_cast<char>(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000) {
            result += static_cast<char>(0xE0 | (code >> 12));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code & 0x3F));
        }
        else {
            result += static_cast<char>(0xF0 | (code >> 18));
            result += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code & 0x3F));
        }
        i = semicolon;
    }
    return result;
}

bool xml_value(std::string_view xml, std::string_view name, std::string& value) {
    size_t start = 0;
    size_t begin = 0;
    size_t end = 0;
    size_t after = 0;
    if (!find_element(xml, name, 0, start, begin, end, after)) {
        return false;
    }
    value = xml_unescape(trim(xml.substr(begin, end - begin)));
    return true;
}

// ---------------------------------------------------------------- qué se manda

std::string mime_type(std::string_view file_name) {
    const std::string extension = extension_of(file_name);
    struct Pair {
        std::string_view extension;
        std::string_view mime;
    };
    static constexpr Pair kTypes[] = {
        {"jpg", "image/jpeg"},        {"jpeg", "image/jpeg"},      {"jpe", "image/jpeg"},
        {"jfif", "image/jpeg"},       {"png", "image/png"},        {"gif", "image/gif"},
        {"bmp", "image/bmp"},         {"mp4", "video/mp4"},        {"m4v", "video/x-m4v"},
        {"mov", "video/quicktime"},   {"mkv", "video/x-matroska"}, {"webm", "video/webm"},
        {"avi", "video/x-msvideo"},   {"divx", "video/x-msvideo"}, {"mpg", "video/mpeg"},
        {"mpeg", "video/mpeg"},       {"mpe", "video/mpeg"},       {"m2v", "video/mpeg"},
        {"vob", "video/mpeg"},        {"ts", "video/mp2t"},        {"mts", "video/mp2t"},
        {"m2ts", "video/mp2t"},       {"wmv", "video/x-ms-wmv"},   {"asf", "video/x-ms-asf"},
        {"flv", "video/x-flv"},       {"f4v", "video/mp4"},        {"3gp", "video/3gpp"},
        {"3g2", "video/3gpp2"},       {"ogv", "video/ogg"},        {"ogm", "video/ogg"},
        {"mp3", "audio/mpeg"},        {"flac", "audio/flac"},      {"m4a", "audio/mp4"},
        {"m4b", "audio/mp4"},         {"aac", "audio/aac"},        {"ogg", "audio/ogg"},
        {"oga", "audio/ogg"},         {"opus", "audio/ogg"},       {"wav", "audio/wav"},
        {"wave", "audio/wav"},        {"wma", "audio/x-ms-wma"},   {"aif", "audio/x-aiff"},
        {"aiff", "audio/x-aiff"},     {"ape", "audio/x-ape"},      {"wv", "audio/x-wavpack"},
        {"mpc", "audio/x-musepack"},
    };
    for (const auto& pair : kTypes) {
        if (pair.extension == extension) {
            return std::string(pair.mime);
        }
    }
    return {};
}

std::string choose_mime(std::string_view file_name, std::string_view sink) {
    std::string mime = mime_type(file_name);
    if (mime.empty()) {
        // Sin nombre conocido, que la tele mire el contenido: algunas lo hacen.
        mime = "application/octet-stream";
    }
    if (sink.empty()) {
        return mime;
    }
    std::vector<std::string> accepted;
    size_t pos = 0;
    while (pos <= sink.size()) {
        size_t comma = sink.find(',', pos);
        if (comma == std::string_view::npos) {
            comma = sink.size();
        }
        const std::string_view entry = trim(sink.substr(pos, comma - pos));
        // protocolo:red:tipo:extra
        const auto first = entry.find(':');
        const auto second = first == std::string_view::npos ? first : entry.find(':', first + 1);
        const auto third = second == std::string_view::npos ? second : entry.find(':', second + 1);
        if (third != std::string_view::npos) {
            accepted.push_back(to_lower(entry.substr(second + 1, third - second - 1)));
        }
        pos = comma + 1;
    }
    const auto accepts = [&](std::string_view type) {
        return std::find(accepted.begin(), accepted.end(), type) != accepted.end();
    };
    if (accepts(mime)) {
        return mime;
    }
    for (const auto alternative : mime_alternatives(mime)) {
        if (accepts(alternative)) {
            return std::string(alternative);
        }
    }
    // MOV, M4V y 3GP sin nada parecido en la lista: como MP4 tienen más posibilidades.
    if (mime == "video/quicktime" || mime == "video/x-m4v" || mime == "video/3gpp" || mime == "video/3gpp2") {
        return "video/mp4";
    }
    return mime;
}

int exif_orientation(const uint8_t* data, size_t size) {
    static constexpr uint8_t kHeader[6] = {'E', 'x', 'i', 'f', 0, 0};
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

bool parse_jpeg(const uint8_t* data, size_t size, JpegInfo& info) {
    info = JpegInfo{};
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }
    size_t pos = 2;
    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) {
            return false;
        }
        const uint8_t marker = data[pos + 1];
        if (marker == 0xFF) { // relleno
            ++pos;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) { // fin o datos sin haber visto el tamaño
            return false;
        }
        const size_t length = (static_cast<size_t>(data[pos + 2]) << 8) | data[pos + 3];
        if (length < 2) {
            return false;
        }
        if (marker == 0xE1 && info.orientation == 1 && pos + 2 + length <= size) {
            info.orientation = exif_orientation(data + pos + 4, length - 2);
        }
        const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (sof) {
            if (pos + 10 > size) {
                return false;
            }
            info.height = (data[pos + 5] << 8) | data[pos + 6];
            info.width = (data[pos + 7] << 8) | data[pos + 8];
            info.components = data[pos + 9];
            info.progressive = marker == 0xC2 || marker == 0xC6 || marker == 0xCA || marker == 0xCE;
            return info.width > 0 && info.height > 0;
        }
        pos += 2 + length;
    }
    return false;
}

bool photo_needs_conversion(const JpegInfo* jpeg) {
    if (!jpeg) {
        return true;
    }
    return jpeg->progressive || jpeg->orientation != 1 || jpeg->components == 4 ||
           jpeg->width > 4096 || jpeg->height > 4096;
}

std::string orientation_filter(int orientation) {
    switch (orientation) {
        case 2: return "hflip";
        case 3: return "hflip,vflip";
        case 4: return "vflip";
        case 5: return "transpose=0"; // trasponer
        case 6: return "transpose=1"; // 90° en el sentido del reloj
        case 7: return "transpose=3"; // trasponer por la otra diagonal
        case 8: return "transpose=2"; // 90° al revés
        default: return {};
    }
}

std::string jpeg_profile(int width, int height) {
    if (width <= 640 && height <= 480) {
        return "JPEG_SM";
    }
    if (width <= 1024 && height <= 768) {
        return "JPEG_MED";
    }
    if (width <= 4096 && height <= 4096) {
        return "JPEG_LRG";
    }
    return {};
}

std::string audio_profile(std::string_view mime) {
    return mime == "audio/mpeg" ? "MP3" : "";
}

std::string content_features(MediaClass media_class, std::string_view profile) {
    // FLAGS: compatible con DLNA 1.5, pausa de conexión, transferencia en segundo plano y,
    // según el tipo, interactiva (fotos) o en directo (vídeo). OP=01: admite pedir trozos.
    std::string features;
    if (!profile.empty()) {
        features += "DLNA.ORG_PN=";
        features += profile;
        features += ';';
    }
    features += "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=";
    features += media_class == MediaClass::Photo ? "00F00000000000000000000000000000"
                                                 : "01700000000000000000000000000000";
    return features;
}

std::string protocol_info(std::string_view mime, MediaClass media_class, std::string_view profile) {
    return "http-get:*:" + std::string(mime) + ":" + content_features(media_class, profile);
}

std::string didl_metadata(std::string_view title,
                          std::string_view url,
                          std::string_view mime,
                          MediaClass media_class,
                          std::string_view profile,
                          uint64_t size) {
    ItemInfo item;
    item.title = std::string(title);
    return didl_metadata(item, url, mime, media_class, profile, size);
}

std::string didl_metadata(const ItemInfo& item,
                          std::string_view url,
                          std::string_view mime,
                          MediaClass media_class,
                          std::string_view profile,
                          uint64_t size) {
    std::string didl =
        "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" "
        "xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\">"
        "<item id=\"1\" parentID=\"0\" restricted=\"1\"><dc:title>";
    didl += xml_escape(item.title.empty() ? std::string_view("Untitled") : std::string_view(item.title));
    didl += "</dc:title>";
    if (!item.artist.empty()) {
        // dc:creator lo leen unas y upnp:artist otras.
        didl += "<dc:creator>" + xml_escape(item.artist) + "</dc:creator>";
        didl += "<upnp:artist>" + xml_escape(item.artist) + "</upnp:artist>";
    }
    if (!item.album.empty()) {
        didl += "<upnp:album>" + xml_escape(item.album) + "</upnp:album>";
    }
    if (!item.cover_url.empty()) {
        didl += "<upnp:albumArtURI>" + xml_escape(item.cover_url) + "</upnp:albumArtURI>";
    }
    didl += "<upnp:class>";
    switch (media_class) {
        case MediaClass::Photo: didl += "object.item.imageItem.photo"; break;
        case MediaClass::Video: didl += "object.item.videoItem"; break;
        case MediaClass::Audio: didl += "object.item.audioItem.musicTrack"; break;
    }
    didl += "</upnp:class><res protocolInfo=\"";
    didl += xml_escape(protocol_info(mime, media_class, profile));
    didl += "\"";
    if (size > 0) {
        didl += " size=\"" + std::to_string(size) + "\"";
    }
    didl += ">";
    didl += xml_escape(url);
    didl += "</res></item></DIDL-Lite>";
    return didl;
}

std::string url_safe_name(std::string_view file_name) {
    std::string result;
    for (const char c : file_name) {
        const bool safe = std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_';
        if (safe && static_cast<unsigned char>(c) < 0x80) {
            result += c;
        }
        else if (result.empty() || result.back() != '_') {
            result += '_';
        }
    }
    // Lo largo se recorta por delante: la extensión, que es lo que miran, se queda.
    constexpr size_t kMaxLength = 64;
    if (result.size() > kMaxLength) {
        result = result.substr(result.size() - kMaxLength);
    }
    if (result.empty() || result.front() == '.') {
        result = "media" + result;
    }
    return result;
}

// ---------------------------------------------------------------- órdenes SOAP

std::string soap_body(std::string_view service, std::string_view action, std::string_view arguments_xml) {
    std::string body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                       "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                       "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:";
    body += action;
    body += " xmlns:u=\"";
    body += service;
    body += "\">";
    body += arguments_xml;
    body += "</u:";
    body += action;
    body += "></s:Body></s:Envelope>\r\n";
    return body;
}

std::string soap_action_header(std::string_view service, std::string_view action) {
    return "\"" + std::string(service) + "#" + std::string(action) + "\"";
}

std::string set_uri_arguments(std::string_view url, std::string_view metadata) {
    return "<InstanceID>0</InstanceID><CurrentURI>" + xml_escape(url) + "</CurrentURI><CurrentURIMetaData>" +
           xml_escape(metadata) + "</CurrentURIMetaData>";
}

std::string set_next_uri_arguments(std::string_view url, std::string_view metadata) {
    return "<InstanceID>0</InstanceID><NextURI>" + xml_escape(url) + "</NextURI><NextURIMetaData>" +
           xml_escape(metadata) + "</NextURIMetaData>";
}

std::string channel_arguments() {
    return "<InstanceID>0</InstanceID><Channel>Master</Channel>";
}

std::string set_volume_arguments(int volume) {
    volume = std::clamp(volume, 0, 100);
    return channel_arguments() + "<DesiredVolume>" + std::to_string(volume) + "</DesiredVolume>";
}

std::string set_mute_arguments(bool muted) {
    return channel_arguments() + "<DesiredMute>" + (muted ? "1" : "0") + "</DesiredMute>";
}

std::string play_arguments() {
    return "<InstanceID>0</InstanceID><Speed>1</Speed>";
}

std::string instance_arguments() {
    return "<InstanceID>0</InstanceID>";
}

std::string seek_arguments(std::string_view unit, double seconds) {
    return "<InstanceID>0</InstanceID><Unit>" + std::string(unit) + "</Unit><Target>" + format_hms(seconds) +
           "</Target>";
}

int soap_error_code(std::string_view response) {
    std::string code;
    if (!xml_value(response, "errorCode", code)) {
        return 0;
    }
    return std::atoi(code.c_str());
}

std::string soap_error_text(int code) {
    switch (code) {
        case 701: return "The TV isn't ready";
        case 702: return "Nothing to play on the TV";
        case 703: return "The TV couldn't read the file";
        case 704: return "The TV can't play this format";
        case 705: return "The TV is busy";
        case 707: return "The TV won't play this file";
        case 710:
        case 711: return "The TV can't seek here";
        case 714: return "The TV doesn't support this type";
        case 715: return "The TV is using the file";
        case 716: return "The TV couldn't get the file";
        default: return "TV error " + std::to_string(code);
    }
}

TransportState parse_transport_state(std::string_view value) {
    value = trim(value);
    if (value == "PLAYING") return TransportState::Playing;
    if (value == "PAUSED_PLAYBACK" || value == "PAUSED_RECORDING") return TransportState::Paused;
    if (value == "STOPPED") return TransportState::Stopped;
    if (value == "TRANSITIONING") return TransportState::Transitioning;
    if (value == "NO_MEDIA_PRESENT") return TransportState::NoMedia;
    return TransportState::Unknown;
}

bool parse_hms(std::string_view text, double& seconds) {
    text = trim(text);
    if (text.empty() || text == "NOT_IMPLEMENTED") {
        return false;
    }
    const std::string copy(text);
    unsigned hours = 0;
    unsigned minutes = 0;
    double secs = 0;
    int consumed = 0;
    if (std::sscanf(copy.c_str(), "%u:%u:%lf%n", &hours, &minutes, &secs, &consumed) != 3 ||
        consumed != static_cast<int>(copy.size()) || minutes > 59 || secs < 0 || secs >= 60) {
        return false;
    }
    seconds = hours * 3600.0 + minutes * 60.0 + secs;
    return true;
}

std::string format_hms(double seconds) {
    const long total = seconds > 0 ? static_cast<long>(std::lround(seconds)) : 0;
    char text[32];
    std::snprintf(text, sizeof(text), "%02ld:%02ld:%02ld", total / 3600, total / 60 % 60, total % 60);
    return text;
}

bool reached_end(double position, double duration) {
    // La tele se pregunta una vez por segundo: lo último que dijo puede ir unos segundos por
    // detrás del final.
    return duration <= 0 || position >= duration - 5;
}

double seek_target(double position, double delta, double duration) {
    double target = position + delta;
    if (duration > 0 && target > duration - 1) {
        target = duration - 1;
    }
    return target < 0 ? 0 : target;
}

// ---------------------------------------------------------------- servidor HTTP

bool parse_http_request(std::string_view text, HttpRequest& request) {
    std::string_view first;
    size_t rest = 0;
    if (!split_first_line(text, first, rest)) {
        return false;
    }
    const auto space1 = first.find(' ');
    const auto space2 = space1 == std::string_view::npos ? space1 : first.find(' ', space1 + 1);
    if (space2 == std::string_view::npos || !starts_with_nocase(first.substr(space2 + 1), "HTTP/")) {
        return false;
    }
    request.method = std::string(first.substr(0, space1));
    request.path = std::string(first.substr(space1 + 1, space2 - space1 - 1));
    request.headers.clear();
    parse_headers(text, rest, request.headers);
    return !request.method.empty() && !request.path.empty();
}

bool parse_range(std::string_view header, uint64_t size, uint64_t& first, uint64_t& last) {
    header = trim(header);
    if (!starts_with_nocase(header, "bytes=") || size == 0) {
        return false;
    }
    header.remove_prefix(6);
    // Varios trozos no los pide ninguna tele: se atiende el primero.
    header = trim(header.substr(0, header.find(',')));
    const auto dash = header.find('-');
    if (dash == std::string_view::npos) {
        return false;
    }
    const std::string start(trim(header.substr(0, dash)));
    const std::string end(trim(header.substr(dash + 1)));
    const auto number = [](const std::string& text, uint64_t& value) {
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return false;
        }
        value = std::strtoull(text.c_str(), nullptr, 10);
        return true;
    };
    if (start.empty()) { // los últimos N bytes
        uint64_t count = 0;
        if (!number(end, count) || count == 0) {
            return false;
        }
        first = count >= size ? 0 : size - count;
        last = size - 1;
        return true;
    }
    if (!number(start, first) || first >= size) {
        return false;
    }
    if (end.empty()) {
        last = size - 1;
        return true;
    }
    if (!number(end, last) || last < first) {
        return false;
    }
    last = std::min(last, size - 1);
    return true;
}

bool parse_http_response(std::string_view text, int& status, std::string& body) {
    std::string_view first;
    size_t rest = 0;
    if (!split_first_line(text, first, rest) || !starts_with_nocase(first, "HTTP/")) {
        return false;
    }
    const auto space = first.find(' ');
    if (space == std::string_view::npos) {
        return false;
    }
    status = std::atoi(std::string(first.substr(space + 1, 3)).c_str());
    std::map<std::string, std::string> headers;
    const size_t body_start = parse_headers(text, rest, headers);
    std::string_view raw = body_start < text.size() ? text.substr(body_start) : std::string_view();

    const auto encoding = headers.find("transfer-encoding");
    if (encoding != headers.end() && to_lower(encoding->second).find("chunked") != std::string::npos) {
        body.clear();
        size_t pos = 0;
        while (pos < raw.size()) {
            const auto line_end = raw.find("\r\n", pos);
            if (line_end == std::string_view::npos) {
                break;
            }
            const std::string size_text(raw.substr(pos, line_end - pos));
            const unsigned long chunk = std::strtoul(size_text.c_str(), nullptr, 16);
            pos = line_end + 2;
            if (chunk == 0) {
                break;
            }
            body.append(raw.substr(pos, std::min<size_t>(chunk, raw.size() - pos)));
            pos += chunk + 2;
        }
        return true;
    }
    const auto length = headers.find("content-length");
    if (length != headers.end()) {
        const size_t count = std::strtoul(length->second.c_str(), nullptr, 10);
        raw = raw.substr(0, std::min(count, raw.size()));
    }
    body = std::string(raw);
    return true;
}

} // namespace dlna
