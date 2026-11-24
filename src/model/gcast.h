/*
 * SPDX-License-Identifier: MIT
 *
 * Google Cast (Chromecast, Google TV, Nest Hub…) sin red ni hilos: buscar por mDNS, el
 * sobre protobuf de cada mensaje, un JSON mínimo y las peticiones al reproductor de Google
 * («Default Media Receiver»). Va aparte para probarlo en el PC (tests/gcast_test.cpp); la
 * conexión (TLS al puerto 8009) está en platform/google_cast.
 *
 * Cómo funciona Cast, en corto: el aparato se anuncia por mDNS como _googlecast._tcp. Se
 * abre TLS al 8009 (certificado propio, no se comprueba) y se hablan mensajes CastMessage
 * (protobuf) con 4 bytes de longitud delante; dentro va JSON. Se lanza la app CC1AD845 (el
 * reproductor de Google), se le conecta y se le manda LOAD con la URL del fichero, que el
 * aparato se baja del mismo servidor HTTP que usa DLNA.
 *
 * Igual que dlna.*, es el mismo fichero en ZeroGallery y en el fork de Music.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gcast {

// ---------------------------------------------------------------- mDNS

constexpr const char* kMdnsGroup = "224.0.0.251";
constexpr int kMdnsPort = 5353;
constexpr int kCastPort = 8009;

/**
 * Pregunta mDNS por _googlecast._tcp.local. Se manda desde un puerto cualquiera (no el 5353):
 * así el aparato contesta directamente a quien pregunta («legacy unicast») y no hace falta
 * unirse al grupo multicast.
 */
std::string mdns_query();

struct Device {
    std::string name;  // fn: «Salón»
    std::string model; // md: «Google Nest Hub», «Chromecast HD»…
    std::string id;    // id: único por aparato
    std::string host;  // IP
    int port{kCastPort};
};

/**
 * Lee una respuesta mDNS y añade a `devices` los aparatos Cast que trae. `source_ip` es de
 * dónde ha llegado, por si la respuesta no trae la dirección (registro A).
 */
bool parse_mdns_response(const uint8_t* data, size_t size, const std::string& source_ip, std::vector<Device>& devices);

// ---------------------------------------------------------------- CastMessage

constexpr const char* kSenderId = "sender-0";
constexpr const char* kReceiverId = "receiver-0";
constexpr const char* kNsConnection = "urn:x-cast:com.google.cast.tp.connection";
constexpr const char* kNsHeartbeat = "urn:x-cast:com.google.cast.tp.heartbeat";
constexpr const char* kNsReceiver = "urn:x-cast:com.google.cast.receiver";
constexpr const char* kNsMedia = "urn:x-cast:com.google.cast.media";
/** El reproductor de Google: fotos, vídeo y audio desde una URL, sin registrar una app. */
constexpr const char* kDefaultMediaReceiver = "CC1AD845";

struct Message {
    std::string source;
    std::string destination;
    std::string name_space;
    std::string payload; // JSON
};

/** El mensaje en protobuf, con sus 4 bytes de longitud (big endian) delante. */
std::string encode_message(const Message& message);

/** Lee un mensaje protobuf (sin los 4 bytes de longitud). `false` si no se entiende. */
bool decode_message(const uint8_t* data, size_t size, Message& message);

// ---------------------------------------------------------------- JSON

/** Un valor JSON. Lo justo para leer lo que contestan los aparatos. */
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type{Type::Null};
    bool boolean{false};
    double number{0};
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    /** Miembro de un objeto; un Null si no está (o si esto no es un objeto). */
    const Json& operator[](const std::string& key) const;
    /** Elemento de una lista; un Null si no está. */
    const Json& at(size_t index) const;
    bool is_null() const { return type == Type::Null; }
    std::string str(const std::string& fallback = {}) const;
    double num(double fallback = 0) const;
};

/** `false` si el texto no es JSON válido. */
bool parse_json(std::string_view text, Json& value);

/** Texto entre comillas y escapado para meterlo en JSON. */
std::string json_string(std::string_view text);

// ---------------------------------------------------------------- peticiones

/** Cuerpo `{"type":…,"requestId":…}` más los campos que se den (JSON sin llaves). */
std::string request(std::string_view type, int request_id, std::string_view fields = {});

enum class MediaKind {
    Photo,
    Video,
    Audio,
};

struct LoadInfo {
    std::string url;
    std::string mime;
    MediaKind kind{MediaKind::Video};
    std::string title;
    std::string artist;
    std::string album;
    std::string cover_url;
    double start{0}; // segundos
};

/** Campos de LOAD (para `request`): el fichero, sus metadatos y que empiece solo. */
std::string load_fields(const std::string& session_id, const LoadInfo& info);

/**
 * Tipo con el que se le anuncia un fichero al reproductor de Google. Lo que no está en su
 * lista no lo abre: un MOV es un MP4 por dentro y así sí.
 */
std::string cast_mime(std::string_view file_name);

/** Lo que interesa de RECEIVER_STATUS. */
struct ReceiverStatus {
    std::string app_id;       // la app que está abierta; vacío si ninguna
    std::string session_id;
    std::string transport_id; // a quién mandar los mensajes de esa app
    double volume{-1};        // 0..1; -1 si no lo dice
    bool muted{false};
};

/** `false` si `payload` no es un RECEIVER_STATUS. */
bool parse_receiver_status(const Json& payload, ReceiverStatus& status);

/** Lo que interesa de MEDIA_STATUS. */
struct MediaStatus {
    long media_session_id{0};
    std::string player_state; // IDLE, PLAYING, PAUSED, BUFFERING
    std::string idle_reason;  // FINISHED, CANCELLED, INTERRUPTED, ERROR (con IDLE)
    double current_time{0};
    double duration{0};
};

/** `false` si `payload` no es un MEDIA_STATUS o no trae ningún estado. */
bool parse_media_status(const Json& payload, MediaStatus& status);

} // namespace gcast
