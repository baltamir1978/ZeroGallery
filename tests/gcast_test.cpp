/*
 * SPDX-License-Identifier: MIT
 *
 * Pruebas de la parte de Google Cast que no toca la red. La respuesta mDNS de
 * tests/fixtures es real (un Google Nest Hub), con los identificadores cambiados.
 */

#include "gcast.h"

#include <cstdio>
#include <fstream>
#include <sstream>
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

std::string read_fixture(const char* name) {
    std::ifstream file(std::string(FIXTURES_DIR) + "/" + name, std::ios::binary);
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

const uint8_t* bytes(const std::string& s) {
    return reinterpret_cast<const uint8_t*>(s.data());
}

void test_mdns() {
    const std::string query = gcast::mdns_query();
    CHECK(query.size() == 12 + 24 + 4); // cabecera, nombre y tipo + clase
    CHECK(query[5] == 1); // una pregunta
    CHECK(query.find("_googlecast") != std::string::npos);

    const std::string reply = read_fixture("nest_hub_mdns.bin");
    CHECK(reply.size() == 422);
    std::vector<gcast::Device> devices;
    CHECK(gcast::parse_mdns_response(bytes(reply), reply.size(), "192.168.0.10", devices));
    CHECK(devices.size() == 1);
    if (!devices.empty()) {
        const auto& hub = devices[0];
        CHECK(hub.name == "Sal\xC3\xB3n");
        CHECK(hub.model == "Google Nest Hub");
        CHECK(hub.id == "0123456789abcdef0123456789abcdef");
        CHECK(hub.port == 8009);
        CHECK(hub.host == "192.168.0.10");
    }
    // Repetida, no se duplica.
    CHECK(gcast::parse_mdns_response(bytes(reply), reply.size(), "192.168.0.10", devices));
    CHECK(devices.size() == 1);
    // Una pregunta (no una respuesta) no vale.
    CHECK(!gcast::parse_mdns_response(bytes(query), query.size(), "x", devices));
    // Cortada, tampoco (y no se sale del búfer).
    CHECK(!gcast::parse_mdns_response(bytes(reply), 100, "x", devices));
}

void test_message() {
    gcast::Message message;
    message.source = gcast::kSenderId;
    message.destination = gcast::kReceiverId;
    message.name_space = gcast::kNsConnection;
    message.payload = "{\"type\":\"CONNECT\"}";
    const std::string framed = gcast::encode_message(message);
    const uint32_t length = (static_cast<uint8_t>(framed[0]) << 24) | (static_cast<uint8_t>(framed[1]) << 16) |
                            (static_cast<uint8_t>(framed[2]) << 8) | static_cast<uint8_t>(framed[3]);
    CHECK(length == framed.size() - 4);
    // Lo que manda pychromecast para lo mismo, byte a byte.
    const std::string expected = std::string("\x08\x00\x12\x08sender-0\x1a\x0areceiver-0\x22\x28", 26) +
                                 "urn:x-cast:com.google.cast.tp.connection" + std::string("\x28\x00\x32\x12", 4) +
                                 "{\"type\":\"CONNECT\"}";
    CHECK(framed.substr(4) == expected);

    gcast::Message back;
    CHECK(gcast::decode_message(bytes(framed) + 4, framed.size() - 4, back));
    CHECK(back.source == "sender-0" && back.destination == "receiver-0");
    CHECK(back.name_space == gcast::kNsConnection && back.payload == message.payload);
    // Un payload largo (más de 127 bytes: longitud en dos bytes)
    message.payload = std::string(300, 'x');
    const std::string big = gcast::encode_message(message);
    CHECK(gcast::decode_message(bytes(big) + 4, big.size() - 4, back) && back.payload.size() == 300);
    CHECK(!gcast::decode_message(bytes(big) + 4, 20, back));
}

void test_json() {
    gcast::Json value;
    CHECK(gcast::parse_json(R"( {"a":1.5,"b":[true,false,null],"c":{"d":"x\"yé😀"}} )", value));
    CHECK(value["a"].num() == 1.5);
    CHECK(value["b"].at(0).boolean && value["b"].at(1).type == gcast::Json::Type::Bool);
    CHECK(value["b"].at(2).is_null() && value["b"].at(9).is_null());
    CHECK(value["c"]["d"].str() == "x\"y\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(value["missing"]["deeper"].is_null());
    CHECK(!gcast::parse_json("{\"a\":}", value));
    CHECK(!gcast::parse_json("[1,2", value));
    CHECK(!gcast::parse_json("{} x", value));
    CHECK(gcast::json_string("a\"b\\c\n\x01\xC3\xB1") == "\"a\\\"b\\\\c\\n\\u0001\xC3\xB1\"");
}

void test_requests() {
    CHECK(gcast::request("GET_STATUS", 3) == "{\"type\":\"GET_STATUS\",\"requestId\":3}");
    CHECK(gcast::request("LAUNCH", 1, "\"appId\":\"CC1AD845\"") ==
          "{\"type\":\"LAUNCH\",\"requestId\":1,\"appId\":\"CC1AD845\"}");

    gcast::LoadInfo info;
    info.url = "http://192.168.0.2:4000/m/ab/song.mp3";
    info.mime = "audio/mpeg";
    info.kind = gcast::MediaKind::Audio;
    info.title = "Mírate";
    info.artist = "Antonio \"Orozco\"";
    info.cover_url = "http://192.168.0.2:4000/m/cd/cover.jpg";
    info.start = 12.5;
    const std::string load = gcast::request("LOAD", 5, gcast::load_fields("sess-1", info));
    gcast::Json parsed;
    CHECK(gcast::parse_json(load, parsed));
    CHECK(parsed["type"].str() == "LOAD" && parsed["requestId"].num() == 5);
    CHECK(parsed["sessionId"].str() == "sess-1");
    CHECK(parsed["media"]["contentId"].str() == info.url);
    CHECK(parsed["media"]["contentType"].str() == "audio/mpeg");
    CHECK(parsed["media"]["streamType"].str() == "BUFFERED");
    CHECK(parsed["media"]["metadata"]["metadataType"].num() == 3);
    CHECK(parsed["media"]["metadata"]["title"].str() == "Mírate");
    CHECK(parsed["media"]["metadata"]["artist"].str() == "Antonio \"Orozco\"");
    CHECK(parsed["media"]["metadata"]["images"].at(0)["url"].str() == info.cover_url);
    CHECK(parsed["autoplay"].boolean && parsed["currentTime"].num() == 12.5);

    CHECK(gcast::cast_mime("IMG.MOV") == "video/mp4");
    CHECK(gcast::cast_mime("a.flac") == "audio/flac");
    CHECK(gcast::cast_mime("a.heic").empty());
}

void test_status() {
    gcast::Json payload;
    // Como lo manda un Nest Hub con el reproductor de Google abierto.
    CHECK(gcast::parse_json(
        R"({"requestId":2,"status":{"applications":[{"appId":"CC1AD845","displayName":"Default Media Receiver",)"
        R"("isIdleScreen":false,"namespaces":[{"name":"urn:x-cast:com.google.cast.media"}],)"
        R"("sessionId":"7e1a-session","statusText":"","transportId":"web-5"}],)"
        R"("volume":{"controlType":"master","level":0.25,"muted":false,"stepInterval":0.05}},"type":"RECEIVER_STATUS"})",
        payload));
    gcast::ReceiverStatus receiver;
    CHECK(gcast::parse_receiver_status(payload, receiver));
    CHECK(receiver.app_id == "CC1AD845" && receiver.session_id == "7e1a-session" && receiver.transport_id == "web-5");
    CHECK(receiver.volume == 0.25 && !receiver.muted);

    CHECK(gcast::parse_json(R"({"type":"RECEIVER_STATUS","status":{"volume":{"level":1,"muted":true}}})", payload));
    CHECK(gcast::parse_receiver_status(payload, receiver));
    CHECK(receiver.app_id.empty() && receiver.muted && receiver.volume == 1);

    CHECK(gcast::parse_json(
        R"({"type":"MEDIA_STATUS","requestId":0,"status":[{"mediaSessionId":1,"playbackRate":1,)"
        R"("playerState":"PLAYING","currentTime":63.2,"media":{"contentId":"x","duration":217.5}}]})",
        payload));
    gcast::MediaStatus media;
    CHECK(gcast::parse_media_status(payload, media));
    CHECK(media.media_session_id == 1 && media.player_state == "PLAYING");
    CHECK(media.current_time == 63.2 && media.duration == 217.5 && media.idle_reason.empty());

    CHECK(gcast::parse_json(
        R"({"type":"MEDIA_STATUS","status":[{"mediaSessionId":1,"playerState":"IDLE","idleReason":"FINISHED"}]})",
        payload));
    CHECK(gcast::parse_media_status(payload, media));
    CHECK(media.player_state == "IDLE" && media.idle_reason == "FINISHED");
    // Sin ningún estado (nada cargado): no cuenta.
    CHECK(gcast::parse_json(R"({"type":"MEDIA_STATUS","status":[]})", payload));
    CHECK(!gcast::parse_media_status(payload, media));
    CHECK(!gcast::parse_receiver_status(payload, receiver));
}

} // namespace

int main() {
    test_mdns();
    test_message();
    test_json();
    test_requests();
    test_status();
    if (failures == 0) {
        std::printf("gcast_test: todo bien\n");
    }
    return failures == 0 ? 0 : 1;
}
