/*
 * SPDX-License-Identifier: MIT
 *
 * Pruebas de la parte de DLNA que no toca la red. Las descripciones de tele de
 * tests/fixtures son reales (una LG webOS de 2018), con los identificadores cambiados.
 */

#include "dlna.h"

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

void test_ssdp() {
    std::string location;
    std::string usn;
    // Tal cual contesta la LG (cabeceras con mayúsculas y minúsculas mezcladas).
    const std::string reply = "HTTP/1.1 200 OK\r\n"
                              "Location: http://192.168.1.20:1243/\r\n"
                              "Server: Linux/i686 UPnP/1,0 DLNADOC/1.50 LGE WebOS TV/Version 0.9\r\n"
                              "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
                              "USN: uuid:1::urn:schemas-upnp-org:device:MediaRenderer:1\r\n\r\n";
    CHECK(dlna::parse_ssdp_response(reply, location, usn));
    CHECK(location == "http://192.168.1.20:1243/");
    CHECK(usn == "uuid:1::urn:schemas-upnp-org:device:MediaRenderer:1");

    CHECK(dlna::parse_ssdp_response("NOTIFY * HTTP/1.1\r\nLOCATION: http://10.0.0.2:9197/dmr\r\nNTS: ssdp:alive\r\n\r\n",
                                    location, usn));
    CHECK(location == "http://10.0.0.2:9197/dmr");
    CHECK(!dlna::parse_ssdp_response("NOTIFY * HTTP/1.1\r\nLOCATION: http://x/\r\nNTS: ssdp:byebye\r\n\r\n",
                                     location, usn));
    CHECK(!dlna::parse_ssdp_response("HTTP/1.1 200 OK\r\nST: x\r\n\r\n", location, usn));
    CHECK(!dlna::parse_ssdp_response("M-SEARCH * HTTP/1.1\r\nLOCATION: http://x/\r\n\r\n", location, usn));

    const std::string search = dlna::msearch_request("upnp:rootdevice");
    CHECK(search.rfind("M-SEARCH * HTTP/1.1\r\n", 0) == 0);
    CHECK(search.find("\r\nST: upnp:rootdevice\r\n") != std::string::npos);
    CHECK(search.find("MAN: \"ssdp:discover\"") != std::string::npos);
    CHECK(search.size() > 4 && search.compare(search.size() - 4, 4, "\r\n\r\n") == 0);
}

void test_urls() {
    dlna::Url url;
    CHECK(dlna::parse_url("http://192.168.1.20:1243/AVTransport/control.xml", url));
    CHECK(url.host == "192.168.1.20" && url.port == 1243 && url.path == "/AVTransport/control.xml");
    CHECK(dlna::parse_url("HTTP://tv", url));
    CHECK(url.host == "tv" && url.port == 80 && url.path == "/");
    CHECK(!dlna::parse_url("https://tv/", url));
    CHECK(!dlna::parse_url("http://tv:0/", url));
    CHECK(!dlna::parse_url("http://:80/", url));

    CHECK(dlna::resolve_url("http://10.0.0.2:9197/dmr", "/upnp/control/AVTransport1") ==
          "http://10.0.0.2:9197/upnp/control/AVTransport1");
    CHECK(dlna::resolve_url("http://10.0.0.2:52323/dmr.xml", "upnp/control/AVTransport") ==
          "http://10.0.0.2:52323/upnp/control/AVTransport");
    CHECK(dlna::resolve_url("http://10.0.0.2/a/b/desc.xml", "ctl") == "http://10.0.0.2:80/a/b/ctl");
    CHECK(dlna::resolve_url("http://10.0.0.2:1/", "http://10.0.0.3:2/x") == "http://10.0.0.3:2/x");
}

void test_lg_description() {
    const std::string xml = read_fixture("lg_webos_dmr.xml");
    CHECK(!xml.empty());
    dlna::Renderer tv;
    CHECK(dlna::parse_description(xml, "http://192.168.1.20:1243/", tv));
    CHECK(tv.name == "[LG] webOS TV UK6500PLA");
    CHECK(tv.manufacturer == "LG Electronics.");
    CHECK(tv.udn == "uuid:00000000-0000-0000-0000-000000000001");
    CHECK(tv.av_transport ==
          "http://192.168.1.20:1243/AVTransport/00000000-0000-0000-0000-000000000001/control.xml");
    CHECK(tv.connection_manager ==
          "http://192.168.1.20:1243/ConnectionManager/00000000-0000-0000-0000-000000000001/control.xml");
    CHECK(tv.rendering_control ==
          "http://192.168.1.20:1243/RenderingControl/00000000-0000-0000-0000-000000000001/control.xml");
}

void test_nested_description() {
    // Como las Samsung y Sony: un aparato raíz sin AVTransport con el renderer dentro, y
    // un URLBase; nombre con entidades.
    const std::string xml =
        "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        "<URLBase>http://10.0.0.9:7676/</URLBase>"
        "<device><deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>"
        "<friendlyName>Root</friendlyName><UDN>uuid:root</UDN>"
        "<serviceList><service><serviceType>urn:x:service:Other:1</serviceType>"
        "<controlURL>/other</controlURL></service></serviceList>"
        "<deviceList><device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
        "<friendlyName>[TV] Sala &amp; cocina</friendlyName><manufacturer>Samsung</manufacturer>"
        "<modelName>QE55</modelName><UDN>uuid:tv</UDN><serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType>"
        "<controlURL>RenderingControl/ctl</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
        "<controlURL>ConnectionManager/ctl</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
        "<controlURL>AVTransport/ctl</controlURL></service>"
        "</serviceList></device></deviceList></device></root>";
    dlna::Renderer tv;
    CHECK(dlna::parse_description(xml, "http://10.0.0.9:9197/dmr", tv));
    CHECK(tv.name == "[TV] Sala & cocina");
    CHECK(tv.manufacturer == "Samsung");
    CHECK(tv.model == "QE55");
    CHECK(tv.udn == "uuid:tv");
    CHECK(tv.av_transport == "http://10.0.0.9:7676/AVTransport/ctl");
    CHECK(tv.connection_manager == "http://10.0.0.9:7676/ConnectionManager/ctl");
    CHECK(tv.rendering_control == "http://10.0.0.9:7676/RenderingControl/ctl");

    // Un router: sin AVTransport no es una tele.
    CHECK(!dlna::parse_description("<root><device><friendlyName>Router</friendlyName><serviceList><service>"
                                   "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                                   "<controlURL>/ctl</controlURL></service></serviceList></device></root>",
                                   "http://192.168.1.1:5000/rootDesc.xml", tv));
}

void test_xml() {
    CHECK(dlna::xml_escape("a<b>&\"c'") == "a&lt;b&gt;&amp;&quot;c&apos;");
    CHECK(dlna::xml_unescape("a&lt;b&gt;&amp;&quot;&apos;&#65;&#x42;&#xF1;") == "a<b>&\"'AB\xC3\xB1");
    CHECK(dlna::xml_unescape("R&D &unknown; &") == "R&D &unknown; &");
    std::string value;
    CHECK(dlna::xml_value("<s:Body><u:Resp><errorCode> 714 </errorCode></u:Resp>", "errorCode", value));
    CHECK(value == "714");
    CHECK(dlna::xml_value("<a><u:RelTime>0:01:02</u:RelTime></a>", "RelTime", value));
    CHECK(value == "0:01:02");
    CHECK(dlna::xml_value("<a><Empty/></a>", "Empty", value) && value.empty());
    CHECK(!dlna::xml_value("<a><RelTimeX>1</RelTimeX></a>", "RelTime", value));
}

void test_mime() {
    CHECK(dlna::mime_type("IMG_1.JPG") == "image/jpeg");
    CHECK(dlna::mime_type("a.mov") == "video/quicktime");
    CHECK(dlna::mime_type("a.mkv") == "video/x-matroska");
    CHECK(dlna::mime_type("a.xyz").empty());

    const std::string lg = read_fixture("lg_webos_sink.txt");
    CHECK(!lg.empty());
    // La LG no dice video/quicktime: un MOV se le anuncia como MP4.
    CHECK(dlna::choose_mime("IMG_0001.MOV", lg) == "video/mp4");
    CHECK(dlna::choose_mime("a.mp4", lg) == "video/mp4");
    CHECK(dlna::choose_mime("a.mkv", lg) == "video/x-matroska");
    CHECK(dlna::choose_mime("a.avi", lg) == "video/x-msvideo");
    CHECK(dlna::choose_mime("a.ts", lg) == "video/mp2t");
    CHECK(dlna::choose_mime("a.jpg", lg) == "image/jpeg");
    // Sin lista se deja el propio, salvo para lo que no se sabe.
    CHECK(dlna::choose_mime("a.mov", "") == "video/quicktime");
    CHECK(dlna::choose_mime("a.xyz", "") == "application/octet-stream");
    // Con otra lista, el equivalente que la tele sí dice.
    CHECK(dlna::choose_mime("a.avi", "http-get:*:video/avi:*,http-get:*:video/mp4:*") == "video/avi");
    CHECK(dlna::choose_mime("a.mkv", "http-get:*:video/x-mkv:*") == "video/x-mkv");
    CHECK(dlna::choose_mime("a.3gp", "http-get:*:image/jpeg:*") == "video/mp4");
}

/** JPEG mínimo: SOI, APP1 opcional y SOF del tipo pedido. */
std::vector<uint8_t> jpeg(uint8_t sof, int width, int height, int components, int orientation) {
    std::vector<uint8_t> data = {0xFF, 0xD8};
    if (orientation != 0) {
        const std::vector<uint8_t> exif = {'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8,
                                           0,   1,   0x01, 0x12, 0, 3, 0, 0, 0, 1,
                                           0,   static_cast<uint8_t>(orientation), 0, 0, 0, 0, 0, 0};
        const size_t length = exif.size() + 2;
        data.insert(data.end(), {0xFF, 0xE1, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)});
        data.insert(data.end(), exif.begin(), exif.end());
    }
    data.insert(data.end(), {0xFF, 0xDB, 0x00, 0x04, 0x00, 0x00}); // DQT de relleno
    data.insert(data.end(), {0xFF, sof, 0x00, 0x11, 0x08, static_cast<uint8_t>(height >> 8),
                             static_cast<uint8_t>(height), static_cast<uint8_t>(width >> 8),
                             static_cast<uint8_t>(width), static_cast<uint8_t>(components)});
    data.insert(data.end(), 9, 0);
    return data;
}

void test_jpeg() {
    dlna::JpegInfo info;
    auto data = jpeg(0xC0, 4032, 3024, 3, 0);
    CHECK(dlna::parse_jpeg(data.data(), data.size(), info));
    CHECK(info.width == 4032 && info.height == 3024 && !info.progressive && info.orientation == 1);
    CHECK(!dlna::photo_needs_conversion(&info));

    data = jpeg(0xC0, 4032, 3024, 3, 6);
    CHECK(dlna::parse_jpeg(data.data(), data.size(), info));
    CHECK(info.orientation == 6);
    CHECK(dlna::photo_needs_conversion(&info));

    data = jpeg(0xC2, 800, 600, 3, 0);
    CHECK(dlna::parse_jpeg(data.data(), data.size(), info));
    CHECK(info.progressive && dlna::photo_needs_conversion(&info));

    data = jpeg(0xC0, 8160, 6120, 3, 0); // 50 Mpx: más que JPEG_LRG
    CHECK(dlna::parse_jpeg(data.data(), data.size(), info));
    CHECK(dlna::photo_needs_conversion(&info));

    data = jpeg(0xC0, 800, 600, 4, 0); // CMYK
    CHECK(dlna::parse_jpeg(data.data(), data.size(), info));
    CHECK(dlna::photo_needs_conversion(&info));

    CHECK(dlna::photo_needs_conversion(nullptr));
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 0, 0, 0, 0};
    CHECK(!dlna::parse_jpeg(png, sizeof(png), info));
    data = jpeg(0xC0, 800, 600, 3, 0);
    CHECK(!dlna::parse_jpeg(data.data(), 8, info)); // cortado antes del SOF

    CHECK(dlna::jpeg_profile(640, 480) == "JPEG_SM");
    CHECK(dlna::jpeg_profile(1024, 768) == "JPEG_MED");
    CHECK(dlna::jpeg_profile(4032, 3024) == "JPEG_LRG");
    CHECK(dlna::jpeg_profile(4097, 100).empty());

    CHECK(dlna::orientation_filter(1).empty());
    CHECK(dlna::orientation_filter(6) == "transpose=1");
    CHECK(dlna::orientation_filter(8) == "transpose=2");
    CHECK(dlna::orientation_filter(3) == "hflip,vflip");
}

void test_metadata() {
    CHECK(dlna::content_features(dlna::MediaClass::Photo, "JPEG_LRG") ==
          "DLNA.ORG_PN=JPEG_LRG;DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=00F00000000000000000000000000000");
    CHECK(dlna::content_features(dlna::MediaClass::Video, "") ==
          "DLNA.ORG_OP=01;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=01700000000000000000000000000000");
    const std::string didl = dlna::didl_metadata("Playa & sol", "http://10.0.0.5:8000/m/ab/a.jpg?x=1&y=2",
                                                 "image/jpeg", dlna::MediaClass::Photo, "JPEG_LRG", 1234);
    CHECK(didl.find("<dc:title>Playa &amp; sol</dc:title>") != std::string::npos);
    CHECK(didl.find("<upnp:class>object.item.imageItem.photo</upnp:class>") != std::string::npos);
    CHECK(didl.find("protocolInfo=\"http-get:*:image/jpeg:DLNA.ORG_PN=JPEG_LRG;") != std::string::npos);
    CHECK(didl.find("size=\"1234\"") != std::string::npos);
    CHECK(didl.find(">http://10.0.0.5:8000/m/ab/a.jpg?x=1&amp;y=2</res>") != std::string::npos);

    CHECK(dlna::url_safe_name("Vacaciones año 2024 (1).MOV") == "Vacaciones_a_o_2024_1_.MOV");
    CHECK(dlna::url_safe_name(".hidden") == "media.hidden");
    CHECK(dlna::url_safe_name("ñ") == "_");
    const std::string longest = dlna::url_safe_name(std::string(100, 'a') + ".mp4");
    CHECK(longest.size() == 64 && longest.compare(longest.size() - 4, 4, ".mp4") == 0);
}

void test_soap() {
    const std::string body = dlna::soap_body(dlna::kAvTransportService, "SetAVTransportURI",
                                             dlna::set_uri_arguments("http://h/a?b&c", "<DIDL-Lite>x</DIDL-Lite>"));
    CHECK(body.find("<u:SetAVTransportURI xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">") !=
          std::string::npos);
    CHECK(body.find("<CurrentURI>http://h/a?b&amp;c</CurrentURI>") != std::string::npos);
    // Los metadatos son XML dentro de XML: van escapados.
    CHECK(body.find("<CurrentURIMetaData>&lt;DIDL-Lite&gt;x&lt;/DIDL-Lite&gt;</CurrentURIMetaData>") !=
          std::string::npos);
    CHECK(body.find("</u:SetAVTransportURI></s:Body></s:Envelope>") != std::string::npos);
    CHECK(dlna::soap_action_header(dlna::kAvTransportService, "Play") ==
          "\"urn:schemas-upnp-org:service:AVTransport:1#Play\"");
    CHECK(dlna::seek_arguments("REL_TIME", 3725.4) ==
          "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>01:02:05</Target>");

    const std::string fault =
        "<s:Envelope><s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>"
        "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>716</errorCode>"
        "<errorDescription>Resource not found</errorDescription></UPnPError></detail></s:Fault></s:Body>"
        "</s:Envelope>";
    CHECK(dlna::soap_error_code(fault) == 716);
    CHECK(dlna::soap_error_code("<ok/>") == 0);
    CHECK(dlna::soap_error_text(714) == "The TV doesn't support this type");
    CHECK(dlna::soap_error_text(999) == "TV error 999");

    CHECK(dlna::parse_transport_state("NO_MEDIA_PRESENT") == dlna::TransportState::NoMedia);
    CHECK(dlna::parse_transport_state("PAUSED_PLAYBACK") == dlna::TransportState::Paused);
    CHECK(dlna::parse_transport_state(" PLAYING ") == dlna::TransportState::Playing);
    CHECK(dlna::parse_transport_state("CUSTOM") == dlna::TransportState::Unknown);

    double seconds = 0;
    CHECK(dlna::parse_hms("0:01:02", seconds) && seconds == 62);
    CHECK(dlna::parse_hms("01:00:00.500", seconds) && seconds == 3600.5);
    CHECK(dlna::parse_hms("123:00:00", seconds) && seconds == 123 * 3600);
    CHECK(!dlna::parse_hms("NOT_IMPLEMENTED", seconds));
    CHECK(!dlna::parse_hms("1:2", seconds));
    CHECK(!dlna::parse_hms("0:61:00", seconds));
    CHECK(dlna::format_hms(0) == "00:00:00");
    CHECK(dlna::format_hms(-5) == "00:00:00");
    CHECK(dlna::format_hms(59.6) == "00:01:00");

    CHECK(dlna::seek_target(100, 10, 0) == 110);
    CHECK(dlna::seek_target(5, -10, 60) == 0);
    CHECK(dlna::seek_target(55, 30, 60) == 59);
}

void test_audio() {
    CHECK(dlna::mime_type("01 - Song.MP3") == "audio/mpeg");
    CHECK(dlna::mime_type("a.flac") == "audio/flac");
    CHECK(dlna::mime_type("a.m4a") == "audio/mp4");
    CHECK(dlna::mime_type("a.opus") == "audio/ogg");
    const std::string lg = read_fixture("lg_webos_sink.txt");
    // Lo que la LG dice aceptar: MP3 y M4A tal cual, FLAC y OGG con su otro nombre.
    CHECK(dlna::choose_mime("a.mp3", lg) == "audio/mpeg");
    CHECK(dlna::choose_mime("a.m4a", lg) == "audio/mp4");
    CHECK(dlna::choose_mime("a.flac", lg) == "audio/flac");
    CHECK(dlna::choose_mime("a.ogg", lg) == "audio/x-ogg");
    CHECK(dlna::choose_mime("a.wav", lg) == "audio/wav");
    CHECK(dlna::choose_mime("a.flac", "http-get:*:audio/x-flac:*") == "audio/x-flac");
    CHECK(dlna::audio_profile("audio/mpeg") == "MP3");
    CHECK(dlna::audio_profile("audio/flac").empty());

    dlna::ItemInfo item;
    item.title = "Clair de Lune";
    item.artist = "Debussy & co";
    item.album = "Suite";
    item.cover_url = "http://10.0.0.5:8000/m/cc/cover.jpg";
    const std::string didl = dlna::didl_metadata(item, "http://10.0.0.5:8000/m/aa/a.mp3", "audio/mpeg",
                                                 dlna::MediaClass::Audio, "MP3", 99);
    CHECK(didl.find("<upnp:class>object.item.audioItem.musicTrack</upnp:class>") != std::string::npos);
    CHECK(didl.find("<dc:creator>Debussy &amp; co</dc:creator>") != std::string::npos);
    CHECK(didl.find("<upnp:artist>Debussy &amp; co</upnp:artist>") != std::string::npos);
    CHECK(didl.find("<upnp:album>Suite</upnp:album>") != std::string::npos);
    CHECK(didl.find("<upnp:albumArtURI>http://10.0.0.5:8000/m/cc/cover.jpg</upnp:albumArtURI>") != std::string::npos);
    CHECK(didl.find("protocolInfo=\"http-get:*:audio/mpeg:DLNA.ORG_PN=MP3;DLNA.ORG_OP=01;") != std::string::npos);
    // Sin artista ni carátula, esas etiquetas no salen.
    const std::string bare = dlna::didl_metadata("x", "http://h/x.mp4", "video/mp4", dlna::MediaClass::Video, "", 0);
    CHECK(bare.find("upnp:artist") == std::string::npos && bare.find("albumArtURI") == std::string::npos);
    CHECK(bare.find("size=") == std::string::npos);

    // Volumen
    CHECK(dlna::set_volume_arguments(130) ==
          "<InstanceID>0</InstanceID><Channel>Master</Channel><DesiredVolume>100</DesiredVolume>");
    CHECK(dlna::set_mute_arguments(true).find("<DesiredMute>1</DesiredMute>") != std::string::npos);
    CHECK(dlna::set_next_uri_arguments("http://h/a&b", "<x/>") ==
          "<InstanceID>0</InstanceID><NextURI>http://h/a&amp;b</NextURI><NextURIMetaData>&lt;x/&gt;</NextURIMetaData>");

    // ¿Ha acabado o la han parado?
    CHECK(dlna::reached_end(178, 180));
    CHECK(!dlna::reached_end(60, 180));
    CHECK(dlna::reached_end(0, 0)); // sin duración no se sabe: se da por acabada
}

void test_http() {
    dlna::HttpRequest request;
    CHECK(dlna::parse_http_request("GET /m/abc/a.mp4 HTTP/1.1\r\nHost: x\r\nRange: bytes=0-\r\n"
                                   "getcontentFeatures.dlna.org: 1\r\n\r\n",
                                   request));
    CHECK(request.method == "GET" && request.path == "/m/abc/a.mp4");
    CHECK(request.headers["range"] == "bytes=0-");
    CHECK(request.headers["getcontentfeatures.dlna.org"] == "1");
    CHECK(!dlna::parse_http_request("GARBAGE\r\n\r\n", request));

    uint64_t first = 0;
    uint64_t last = 0;
    CHECK(dlna::parse_range("bytes=0-", 1000, first, last) && first == 0 && last == 999);
    CHECK(dlna::parse_range("bytes=100-199", 1000, first, last) && first == 100 && last == 199);
    CHECK(dlna::parse_range("bytes=900-5000", 1000, first, last) && first == 900 && last == 999);
    CHECK(dlna::parse_range("bytes=-100", 1000, first, last) && first == 900 && last == 999);
    CHECK(dlna::parse_range("bytes=-5000", 1000, first, last) && first == 0 && last == 999);
    CHECK(dlna::parse_range("bytes=10-20, 30-40", 1000, first, last) && first == 10 && last == 20);
    CHECK(!dlna::parse_range("bytes=1000-", 1000, first, last));
    CHECK(!dlna::parse_range("bytes=20-10", 1000, first, last));
    CHECK(!dlna::parse_range("items=0-1", 1000, first, last));
    CHECK(!dlna::parse_range("bytes=x-", 1000, first, last));

    int status = 0;
    std::string body;
    CHECK(dlna::parse_http_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello world", status, body));
    CHECK(status == 200 && body == "hello");
    CHECK(dlna::parse_http_response("HTTP/1.1 500 Internal Server Error\r\nTransfer-Encoding: chunked\r\n\r\n"
                                    "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n",
                                    status, body));
    CHECK(status == 500 && body == "hello world");
    CHECK(!dlna::parse_http_response("nope", status, body));
}

} // namespace

int main() {
    test_ssdp();
    test_urls();
    test_lg_description();
    test_nested_description();
    test_xml();
    test_mime();
    test_jpeg();
    test_metadata();
    test_soap();
    test_audio();
    test_http();
    if (failures == 0) {
        std::printf("dlna_test: todo bien\n");
    }
    return failures == 0 ? 0 : 1;
}
