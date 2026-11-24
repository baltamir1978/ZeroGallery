/*
 * SPDX-License-Identifier: MIT
 */

#include "gcast.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gcast {
namespace {

// ---------------------------------------------------------------- DNS

uint16_t read16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

/**
 * Nombre DNS desde `pos` (con punteros de compresión), en minúsculas y con puntos. Deja
 * `pos` detrás del nombre. `false` si está mal formado.
 */
bool read_name(const uint8_t* data, size_t size, size_t& pos, std::string& name) {
    name.clear();
    size_t cursor = pos;
    bool jumped = false;
    for (int guard = 0; guard < 128; ++guard) {
        if (cursor >= size) {
            return false;
        }
        const uint8_t length = data[cursor];
        if (length == 0) {
            if (!jumped) {
                pos = cursor + 1;
            }
            return true;
        }
        if ((length & 0xC0) == 0xC0) {
            if (cursor + 1 >= size) {
                return false;
            }
            const size_t target = static_cast<size_t>(((length & 0x3F) << 8) | data[cursor + 1]);
            if (!jumped) {
                pos = cursor + 2;
            }
            jumped = true;
            cursor = target;
            continue;
        }
        if (cursor + 1 + length > size) {
            return false;
        }
        if (!name.empty()) {
            name += '.';
        }
        name.append(reinterpret_cast<const char*>(data + cursor + 1), length);
        cursor += 1 + length;
    }
    return false;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return text;
}

// ---------------------------------------------------------------- protobuf

void put_varint(std::string& out, uint64_t value) {
    while (value >= 0x80) {
        out += static_cast<char>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    out += static_cast<char>(value);
}

void put_string(std::string& out, int field, const std::string& value) {
    put_varint(out, static_cast<uint64_t>(field << 3 | 2));
    put_varint(out, value.size());
    out += value;
}

bool get_varint(const uint8_t* data, size_t size, size_t& pos, uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (pos >= size) {
            return false;
        }
        const uint8_t byte = data[pos++];
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- JSON

struct JsonParser {
    std::string_view text;
    size_t pos{0};

    void skip() {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
            ++pos;
        }
    }

    bool literal(std::string_view word) {
        if (text.substr(pos, word.size()) != word) {
            return false;
        }
        pos += word.size();
        return true;
    }

    static void put_utf8(std::string& out, unsigned long code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        }
        else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(unsigned long& code) {
        if (pos + 4 > text.size()) {
            return false;
        }
        const std::string digits(text.substr(pos, 4));
        char* end = nullptr;
        code = std::strtoul(digits.c_str(), &end, 16);
        if (!end || *end != '\0') {
            return false;
        }
        pos += 4;
        return true;
    }

    bool string(std::string& out) {
        if (pos >= text.size() || text[pos] != '"') {
            return false;
        }
        ++pos;
        out.clear();
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= text.size()) {
                return false;
            }
            const char e = text[pos++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned long code = 0;
                    if (!hex4(code)) {
                        return false;
                    }
                    // Pareja suplente (emoji y demás fuera del plano básico).
                    if (code >= 0xD800 && code < 0xDC00 && text.substr(pos, 2) == "\\u") {
                        pos += 2;
                        unsigned long low = 0;
                        if (!hex4(low)) {
                            return false;
                        }
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    put_utf8(out, code);
                    break;
                }
                default:
                    return false;
            }
        }
        return false;
    }

    bool value(Json& out, int depth) {
        if (depth > 64) {
            return false;
        }
        skip();
        if (pos >= text.size()) {
            return false;
        }
        const char c = text[pos];
        if (c == '{') {
            ++pos;
            out.type = Json::Type::Object;
            skip();
            if (pos < text.size() && text[pos] == '}') {
                ++pos;
                return true;
            }
            while (true) {
                skip();
                std::string key;
                if (!string(key)) {
                    return false;
                }
                skip();
                if (pos >= text.size() || text[pos] != ':') {
                    return false;
                }
                ++pos;
                Json member;
                if (!value(member, depth + 1)) {
                    return false;
                }
                out.object[key] = std::move(member);
                skip();
                if (pos < text.size() && text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < text.size() && text[pos] == '}') {
                    ++pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++pos;
            out.type = Json::Type::Array;
            skip();
            if (pos < text.size() && text[pos] == ']') {
                ++pos;
                return true;
            }
            while (true) {
                Json element;
                if (!value(element, depth + 1)) {
                    return false;
                }
                out.array.push_back(std::move(element));
                skip();
                if (pos < text.size() && text[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < text.size() && text[pos] == ']') {
                    ++pos;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.type = Json::Type::String;
            return string(out.string);
        }
        if (literal("true")) {
            out.type = Json::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.type = Json::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (literal("null")) {
            out.type = Json::Type::Null;
            return true;
        }
        // Número
        const size_t start = pos;
        while (pos < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[pos])) || text[pos] == '-' || text[pos] == '+' ||
                text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E')) {
            ++pos;
        }
        if (pos == start) {
            return false;
        }
        const std::string number(text.substr(start, pos - start));
        char* end = nullptr;
        out.type = Json::Type::Number;
        out.number = std::strtod(number.c_str(), &end);
        return end && *end == '\0';
    }
};

const Json& null_json() {
    static const Json null;
    return null;
}

std::string extension_of(std::string_view file_name) {
    const auto dot = file_name.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    return lower(std::string(file_name.substr(dot + 1)));
}

std::string number_text(double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.3f", value);
    return text;
}

} // namespace

// ---------------------------------------------------------------- mDNS

std::string mdns_query() {
    std::string query("\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00", 12); // id 0, una pregunta
    for (const char* label : {"_googlecast", "_tcp", "local"}) {
        query += static_cast<char>(std::string_view(label).size());
        query += label;
    }
    query += std::string("\x00\x00\x0c\x00\x01", 5); // PTR, IN
    return query;
}

bool parse_mdns_response(const uint8_t* data, size_t size, const std::string& source_ip, std::vector<Device>& devices) {
    if (size < 12 || !(data[2] & 0x80)) { // no es una respuesta
        return false;
    }
    const uint16_t questions = read16(data + 4);
    const uint16_t records = static_cast<uint16_t>(read16(data + 6) + read16(data + 8) + read16(data + 10));
    size_t pos = 12;
    std::string name;
    for (uint16_t i = 0; i < questions; ++i) {
        if (!read_name(data, size, pos, name) || pos + 4 > size) {
            return false;
        }
        pos += 4;
    }

    struct Instance {
        std::map<std::string, std::string> txt;
        std::string target;
        int port{0};
    };
    std::map<std::string, Instance> instances; // por nombre de la instancia
    std::map<std::string, std::string> addresses; // nombre de máquina → IP

    for (uint16_t i = 0; i < records; ++i) {
        if (!read_name(data, size, pos, name) || pos + 10 > size) {
            return false;
        }
        const uint16_t type = read16(data + pos);
        const uint16_t length = read16(data + pos + 8);
        pos += 10;
        if (pos + length > size) {
            return false;
        }
        const size_t rdata = pos;
        pos += length;
        const std::string key = lower(name);
        if (type == 12) { // PTR: nombre de servicio → instancia
            size_t cursor = rdata;
            std::string instance;
            if (key == "_googlecast._tcp.local" && read_name(data, size, cursor, instance)) {
                instances[lower(instance)];
            }
        }
        else if (type == 33 && length >= 6) { // SRV
            Instance& instance = instances[key];
            instance.port = read16(data + rdata + 4);
            size_t cursor = rdata + 6;
            read_name(data, size, cursor, instance.target);
            instance.target = lower(instance.target);
        }
        else if (type == 16) { // TXT: cadenas «clave=valor» con su longitud delante
            Instance& instance = instances[key];
            size_t cursor = rdata;
            while (cursor < rdata + length) {
                const uint8_t item = data[cursor];
                if (cursor + 1 + item > rdata + length) {
                    break;
                }
                const std::string entry(reinterpret_cast<const char*>(data + cursor + 1), item);
                const auto equals = entry.find('=');
                if (equals != std::string::npos) {
                    instance.txt[lower(entry.substr(0, equals))] = entry.substr(equals + 1);
                }
                cursor += 1 + item;
            }
        }
        else if (type == 1 && length == 4) { // A
            char ip[16];
            std::snprintf(ip, sizeof(ip), "%u.%u.%u.%u", data[rdata], data[rdata + 1], data[rdata + 2],
                          data[rdata + 3]);
            addresses[key] = ip;
        }
    }

    bool found = false;
    for (const auto& [instance_name, instance] : instances) {
        if (instance_name.find("_googlecast._tcp") == std::string::npos) {
            continue;
        }
        Device device;
        const auto get = [&](const char* k) {
            const auto it = instance.txt.find(k);
            return it == instance.txt.end() ? std::string() : it->second;
        };
        device.name = get("fn");
        device.model = get("md");
        device.id = get("id");
        if (device.id.empty()) {
            device.id = instance_name;
        }
        if (device.name.empty()) {
            device.name = device.model.empty() ? std::string("Chromecast") : device.model;
        }
        device.port = instance.port > 0 ? instance.port : kCastPort;
        const auto address = addresses.find(instance.target);
        device.host = address != addresses.end() ? address->second : source_ip;
        if (device.host.empty()) {
            continue;
        }
        const auto same = std::find_if(devices.begin(), devices.end(), [&](const Device& d) { return d.id == device.id; });
        if (same == devices.end()) {
            devices.push_back(device);
        }
        else {
            *same = device;
        }
        found = true;
    }
    return found;
}

// ---------------------------------------------------------------- CastMessage

std::string encode_message(const Message& message) {
    std::string body;
    put_varint(body, 1 << 3 | 0); // protocol_version
    put_varint(body, 0);          // CASTV2_1_0
    put_string(body, 2, message.source);
    put_string(body, 3, message.destination);
    put_string(body, 4, message.name_space);
    put_varint(body, 5 << 3 | 0); // payload_type
    put_varint(body, 0);          // STRING
    put_string(body, 6, message.payload);
    std::string framed;
    const uint32_t length = static_cast<uint32_t>(body.size());
    framed += static_cast<char>(length >> 24);
    framed += static_cast<char>(length >> 16);
    framed += static_cast<char>(length >> 8);
    framed += static_cast<char>(length);
    return framed + body;
}

bool decode_message(const uint8_t* data, size_t size, Message& message) {
    message = Message{};
    size_t pos = 0;
    while (pos < size) {
        uint64_t key = 0;
        if (!get_varint(data, size, pos, key)) {
            return false;
        }
        const int field = static_cast<int>(key >> 3);
        const int wire = static_cast<int>(key & 7);
        if (wire == 0) {
            uint64_t ignored = 0;
            if (!get_varint(data, size, pos, ignored)) {
                return false;
            }
        }
        else if (wire == 2) {
            uint64_t length = 0;
            if (!get_varint(data, size, pos, length) || pos + length > size) {
                return false;
            }
            const std::string value(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(length));
            pos += static_cast<size_t>(length);
            switch (field) {
                case 2: message.source = value; break;
                case 3: message.destination = value; break;
                case 4: message.name_space = value; break;
                case 6: message.payload = value; break;
                default: break; // payload_binary y lo que se añada
            }
        }
        else {
            return false;
        }
    }
    return !message.name_space.empty();
}

// ---------------------------------------------------------------- JSON

const Json& Json::operator[](const std::string& key) const {
    if (type != Type::Object) {
        return null_json();
    }
    const auto it = object.find(key);
    return it == object.end() ? null_json() : it->second;
}

const Json& Json::at(size_t index) const {
    return type == Type::Array && index < array.size() ? array[index] : null_json();
}

std::string Json::str(const std::string& fallback) const {
    return type == Type::String ? string : fallback;
}

double Json::num(double fallback) const {
    return type == Type::Number ? number : fallback;
}

bool parse_json(std::string_view text, Json& value) {
    JsonParser parser{text};
    value = Json{};
    if (!parser.value(value, 0)) {
        return false;
    }
    parser.skip();
    return parser.pos == text.size();
}

std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
                    out += escaped;
                }
                else {
                    out += c; // UTF-8 tal cual: JSON lo admite
                }
        }
    }
    return out + "\"";
}

// ---------------------------------------------------------------- peticiones

std::string request(std::string_view type, int request_id, std::string_view fields) {
    std::string out = "{\"type\":" + json_string(type) + ",\"requestId\":" + std::to_string(request_id);
    if (!fields.empty()) {
        out += ",";
        out += fields;
    }
    return out + "}";
}

std::string load_fields(const std::string& session_id, const LoadInfo& info) {
    // metadataType: 0 genérico (vídeo), 3 música, 4 foto. Con música el aparato enseña la
    // carátula y los nombres.
    int metadata_type = 0;
    if (info.kind == MediaKind::Audio) {
        metadata_type = 3;
    }
    else if (info.kind == MediaKind::Photo) {
        metadata_type = 4;
    }
    std::string metadata = "{\"metadataType\":" + std::to_string(metadata_type);
    if (!info.title.empty()) {
        metadata += ",\"title\":" + json_string(info.title);
    }
    if (!info.artist.empty()) {
        metadata += ",\"artist\":" + json_string(info.artist);
    }
    if (!info.album.empty()) {
        metadata += ",\"albumName\":" + json_string(info.album);
    }
    if (!info.cover_url.empty()) {
        metadata += ",\"images\":[{\"url\":" + json_string(info.cover_url) + "}]";
    }
    metadata += "}";
    std::string fields = "\"sessionId\":" + json_string(session_id) + ",\"media\":{\"contentId\":" +
                         json_string(info.url) + ",\"contentType\":" + json_string(info.mime) +
                         ",\"streamType\":\"BUFFERED\",\"metadata\":" + metadata + "},\"autoplay\":true";
    if (info.start > 0) {
        fields += ",\"currentTime\":" + number_text(info.start);
    }
    return fields;
}

std::string cast_mime(std::string_view file_name) {
    const std::string extension = extension_of(file_name);
    struct Pair {
        const char* extension;
        const char* mime;
    };
    // Lo que abre el reproductor de Google (developers.google.com/cast/docs/media).
    static constexpr Pair kTypes[] = {
        {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"}, {"jpe", "image/jpeg"},  {"jfif", "image/jpeg"},
        {"png", "image/png"},   {"gif", "image/gif"},   {"webp", "image/webp"}, {"bmp", "image/bmp"},
        {"mp4", "video/mp4"},   {"m4v", "video/mp4"},   {"mov", "video/mp4"},   {"3gp", "video/mp4"},
        {"webm", "video/webm"}, {"mkv", "video/x-matroska"},
        {"mp3", "audio/mpeg"},  {"m4a", "audio/mp4"},   {"m4b", "audio/mp4"},   {"aac", "audio/aac"},
        {"flac", "audio/flac"}, {"ogg", "audio/ogg"},   {"oga", "audio/ogg"},   {"opus", "audio/ogg"},
        {"wav", "audio/wav"},   {"wave", "audio/wav"},
    };
    for (const auto& pair : kTypes) {
        if (extension == pair.extension) {
            return pair.mime;
        }
    }
    return {};
}

bool parse_receiver_status(const Json& payload, ReceiverStatus& status) {
    if (payload["type"].str() != "RECEIVER_STATUS") {
        return false;
    }
    status = ReceiverStatus{};
    const Json& body = payload["status"];
    const Json& app = body["applications"].at(0);
    status.app_id = app["appId"].str();
    status.session_id = app["sessionId"].str();
    status.transport_id = app["transportId"].str();
    const Json& volume = body["volume"];
    status.volume = volume["level"].num(-1);
    status.muted = volume["muted"].type == Json::Type::Bool && volume["muted"].boolean;
    return true;
}

bool parse_media_status(const Json& payload, MediaStatus& status) {
    if (payload["type"].str() != "MEDIA_STATUS") {
        return false;
    }
    const Json& first = payload["status"].at(0);
    if (first.is_null()) {
        return false;
    }
    status = MediaStatus{};
    status.media_session_id = static_cast<long>(first["mediaSessionId"].num(0));
    status.player_state = first["playerState"].str();
    status.idle_reason = first["idleReason"].str();
    status.current_time = first["currentTime"].num(0);
    status.duration = first["media"]["duration"].num(0);
    return true;
}

} // namespace gcast
