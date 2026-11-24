/*
 * SPDX-License-Identifier: MIT
 *
 * Lo que hace falta para mandar una foto o un vídeo a una tele por DLNA (UPnP AV), sin red
 * ni hilos: leer las respuestas de las teles, escribir las órdenes SOAP, decidir qué
 * cabeceras lleva el fichero que se sirve y qué hay que convertir antes. Va aparte para
 * probarlo en el PC (tests/dlna_test.cpp); la red está en platform/dlna_caster.
 *
 * Estos dos ficheros (y dlna_caster) son iguales en ZeroGallery y en el fork de Music: no
 * dependen de nada de la app, y un arreglo en uno se copia tal cual al otro.
 *
 * Cómo funciona DLNA, en corto: la tele (un «renderer») no recibe el fichero, recibe una
 * URL. Se la encuentra por SSDP (un UDP a multicast), se lee su descripción (XML por HTTP)
 * para saber a qué dirección mandarle las órdenes del servicio AVTransport, y con
 * SetAVTransportURI + Play se le dice «reproduce esto». La tele baja el fichero de un
 * servidor HTTP que pone la app y lo decodifica ella.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace dlna {

// ---------------------------------------------------------------- descubrir teles

/** Lo que se busca por SSDP. Cada marca contesta a unas: se preguntan todas. */
extern const char* const kSearchTargets[3];

/** Petición M-SEARCH para `search_target`. */
std::string msearch_request(std::string_view search_target);

/**
 * Respuesta a un M-SEARCH (o anuncio NOTIFY): la URL de la descripción (LOCATION) y el
 * identificador del aparato (USN). `false` si no es una respuesta válida o no trae LOCATION.
 */
bool parse_ssdp_response(std::string_view message, std::string& location, std::string& usn);

struct Url {
    std::string host;
    int port{80};
    std::string path{"/"}; // con la consulta, si la hay
};

/** Sólo `http://host[:puerto][/ruta]`, que es lo que usan las teles. */
bool parse_url(std::string_view text, Url& url);

/** URL absoluta de `reference` (absoluta, desde la raíz o relativa) vista desde `base`. */
std::string resolve_url(std::string_view base, std::string_view reference);

/** Una tele (o lo que sea que reproduce) con lo que hace falta para mandarle órdenes. */
struct Renderer {
    std::string name;         // friendlyName: «[LG] webOS TV OLED55C1», «[TV] Samsung Q60…»
    std::string manufacturer;
    std::string model;
    std::string udn;          // identificador único: sirve para no repetirla en la lista
    std::string av_transport; // URL de control del servicio AVTransport, ya absoluta
    std::string connection_manager; // URL de control de ConnectionManager (puede faltar)
    std::string rendering_control;  // URL de control de RenderingControl: el volumen (puede faltar)
    // Google Cast (Chromecast, Google TV, Nest Hub): no es DLNA, pero se elige en la misma
    // lista. Con cast_port > 0 lo de arriba no cuenta (salvo name, model y udn).
    std::string cast_host;
    int cast_port{0};
    bool is_cast() const { return cast_port > 0; }
};

/**
 * Lee la descripción de un aparato UPnP. `false` si no tiene el servicio AVTransport, es
 * decir, si no es algo a lo que se pueda mandar un vídeo (un router, un NAS…).
 * `location` es la URL de la que se ha bajado, para resolver las relativas.
 */
bool parse_description(std::string_view xml, std::string_view location, Renderer& renderer);

// ---------------------------------------------------------------- XML

std::string xml_escape(std::string_view text);
std::string xml_unescape(std::string_view text);

/**
 * Texto del primer elemento `name` (sin mirar el prefijo de espacio de nombres:
 * `<u:errorCode>` vale para `errorCode`), ya sin escapar. `false` si no está.
 */
bool xml_value(std::string_view xml, std::string_view name, std::string& value);

// ---------------------------------------------------------------- qué se manda

enum class MediaClass {
    Photo,
    Video,
    Audio,
};

/** Tipo MIME por la extensión; vacío si no se conoce. */
std::string mime_type(std::string_view file_name);

/**
 * Tipo MIME con el que se anuncia el fichero a una tele concreta. `sink` es lo que ella
 * dice que acepta (GetProtocolInfo: protocolInfo separados por comas; vacío si no lo dijo).
 * Si su lista no trae el tipo exacto pero sí uno del mismo contenedor, se usa ese: un MOV
 * de iPhone es un MP4 por dentro y LG o Samsung lo reproducen como `video/mp4`, pero
 * rechazan `video/quicktime` sin mirarlo.
 */
std::string choose_mime(std::string_view file_name, std::string_view sink);

/** Lo que interesa de un JPEG sin decodificarlo. */
struct JpegInfo {
    int width{0};
    int height{0};
    int components{3};  // 1 gris, 3 color; 4 es CMYK, que muchas teles no enseñan
    bool progressive{false};
    int orientation{1}; // EXIF, 1..8
};

/** Orientación EXIF (1..8) del contenido de un segmento APP1 («Exif\0\0…»); 1 si no hay. */
int exif_orientation(const uint8_t* data, size_t size);

/** Lee la cabecera de un JPEG (basta con el principio del fichero). `false` si no lo es. */
bool parse_jpeg(const uint8_t* data, size_t size, JpegInfo& info);

/**
 * Si la foto se puede mandar tal cual o hay que convertirla antes a un JPEG corriente.
 * Lo único que enseñan todas las teles es JPEG, y no cualquiera: sólo en orden
 * secuencial (no progresivo), hasta 4096 px de lado (el mayor perfil DLNA, JPEG_LRG) y
 * sin depender de la orientación EXIF, que unas respetan y otras no. Todo lo demás (PNG,
 * HEIC, WebP, GIF…) se convierte. `jpeg` es null si el fichero no es JPEG.
 */
bool photo_needs_conversion(const JpegInfo* jpeg);

/**
 * Filtro de ffmpeg para enderezar según la orientación EXIF (vacío para la 1). ffmpeg se
 * lanza con -noautorotate para que no gire dos veces.
 */
std::string orientation_filter(int orientation);

/**
 * Perfil DLNA de un JPEG según su tamaño (JPEG_SM, _MED, _LRG); vacío si es más grande.
 * Hay teles (Samsung) que lo miran.
 */
std::string jpeg_profile(int width, int height);

/** Perfil DLNA de un audio por su tipo: «MP3» para MP3, vacío para lo demás. */
std::string audio_profile(std::string_view mime);

/**
 * Valor de la cabecera contentFeatures.dlna.org (y cuarto campo del protocolInfo): que se
 * puede pedir por trozos (OP=01) y cómo se transfiere. `profile` puede ir vacío.
 */
std::string content_features(MediaClass media_class, std::string_view profile);

/** protocolInfo del recurso: `http-get:*:<mime>:<contentFeatures>`. */
std::string protocol_info(std::string_view mime, MediaClass media_class, std::string_view profile);

/** Lo que la tele enseña del elemento, además del fichero. */
struct ItemInfo {
    std::string title;
    std::string artist;    // música
    std::string album;     // música
    std::string cover_url; // carátula, ya servida; vacía si no hay
};

/**
 * Metadatos DIDL-Lite del elemento. Hay teles que sin ellos no reproducen nada (LG,
 * Samsung) aunque el estándar diga que son opcionales.
 */
std::string didl_metadata(const ItemInfo& item,
                          std::string_view url,
                          std::string_view mime,
                          MediaClass media_class,
                          std::string_view profile,
                          uint64_t size);
std::string didl_metadata(std::string_view title,
                          std::string_view url,
                          std::string_view mime,
                          MediaClass media_class,
                          std::string_view profile,
                          uint64_t size);

/** Nombre que va en la URL: sólo ASCII sin espacios (a alguna tele le molesta lo demás). */
std::string url_safe_name(std::string_view file_name);

// ---------------------------------------------------------------- órdenes SOAP

constexpr const char* kAvTransportService = "urn:schemas-upnp-org:service:AVTransport:1";

/** Cuerpo SOAP de `action` del servicio `service`, con sus argumentos ya en XML. */
std::string soap_body(std::string_view service, std::string_view action, std::string_view arguments_xml);

/** Cabecera SOAPACTION (con sus comillas, que alguna tele exige). */
std::string soap_action_header(std::string_view service, std::string_view action);

std::string set_uri_arguments(std::string_view url, std::string_view metadata);
/** SetNextAVTransportURI: la siguiente, para que la tele la enlace sin hueco (si sabe). */
std::string set_next_uri_arguments(std::string_view url, std::string_view metadata);
std::string play_arguments();
/** Para Pause, Stop, GetTransportInfo y GetPositionInfo, que sólo llevan InstanceID. */
std::string instance_arguments();
/** GetProtocolInfo es del servicio ConnectionManager, no del AVTransport. */
constexpr const char* kConnectionManagerService = "urn:schemas-upnp-org:service:ConnectionManager:1";
/** El volumen es del servicio RenderingControl. */
constexpr const char* kRenderingControlService = "urn:schemas-upnp-org:service:RenderingControl:1";
/** GetVolume y GetMute: InstanceID y Channel=Master. */
std::string channel_arguments();
std::string set_volume_arguments(int volume);
std::string set_mute_arguments(bool muted);
/** `unit` es REL_TIME (lo normal) o ABS_TIME (lo que entienden algunas). */
std::string seek_arguments(std::string_view unit, double seconds);

/** Código de error UPnP de una respuesta de fallo (`<errorCode>`), o 0 si no lo trae. */
int soap_error_code(std::string_view response);

/** Explicación corta, en inglés como la interfaz, de un error UPnP. */
std::string soap_error_text(int code);

/** Estado del transporte de la tele (GetTransportInfo). */
enum class TransportState {
    Unknown,
    Stopped,
    Playing,
    Paused,
    Transitioning,
    NoMedia,
};

TransportState parse_transport_state(std::string_view value);

/** «H:MM:SS» que usan las teles; también con milésimas o con horas de dos cifras. */
bool parse_hms(std::string_view text, double& seconds);
std::string format_hms(double seconds);

/**
 * A dónde saltar: `position` más `delta`, sin bajar de 0 ni pasar de un segundo antes del
 * final (si se conoce la duración, que unas teles no dicen).
 */
double seek_target(double position, double delta, double duration);

/**
 * Si un STOPPED de la tele es que la canción o el vídeo ha llegado al final (y toca el
 * siguiente) o que lo han parado con el mando. Lo último que se supo: por dónde iba y cuánto
 * dura (0 si no se sabe). Sin duración no se puede saber y se da por acabado.
 */
bool reached_end(double position, double duration);

// ---------------------------------------------------------------- servidor HTTP

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers; // nombres en minúsculas
};

/** Cabecera de una petición HTTP (hasta la línea en blanco). */
bool parse_http_request(std::string_view text, HttpRequest& request);

/**
 * Trozo pedido con `Range: bytes=…` de un fichero de `size` bytes: `first` y `last`
 * incluidos. `false` si no se puede servir (416).
 */
bool parse_range(std::string_view header, uint64_t size, uint64_t& first, uint64_t& last);

/** Estado y cuerpo de una respuesta HTTP; deshace el «chunked» si viene así. */
bool parse_http_response(std::string_view text, int& status, std::string& body);

} // namespace dlna
