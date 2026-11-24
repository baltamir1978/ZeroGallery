/*
 * SPDX-License-Identifier: MIT
 */

#include "dlna_caster.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace platform {
namespace {

// Cada cuánto se vuelve a preguntar por teles con la lista abierta: hay teles que tardan
// en contestar la primera vez (o están saliendo del reposo).
constexpr int kSearchPeriodMs = 3000;
constexpr int kDescriptionTimeoutMs = 2500;
// Cada cuánto se pregunta a la tele cómo va un vídeo.
constexpr int kPollPeriodMs = 1000;
// Tras mandar algo, la tele puede decir STOPPED un momento antes de empezar.
constexpr uint32_t kStartGraceMs = 5000;
// Fotos convertidas: hasta 4K, que es lo que tiene la mejor tele; más sólo pesa.
constexpr const char* kPhotoScale =
    "scale=w='min(3840,iw)':h='min(2160,ih)':force_original_aspect_ratio=decrease:force_divisible_by=2";
constexpr int kConvertTimeoutMs = 60000;
// Ficheros que se siguen sirviendo después de mandar otro: la tele puede estar acabando
// de pedir el anterior.
constexpr size_t kServedKept = 6; // con su carátula, cada canción ocupa dos
// Con la siguiente dada, lo que la tele puede estar parada al cambiar antes de que se dé
// por hecho que no ha sabido.
constexpr uint32_t kNextGraceMs = 4000;
// SetAVTransportURI y Play no contestan hasta que la tele ha abierto el fichero: en la LG,
// tres o cuatro segundos para una foto.
constexpr int kPlayTimeoutMs = 20000;

bool debug_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("ZEROGALLERY_DLNA_DEBUG");
        return value && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

void debug(const char* format, ...) {
    if (!debug_enabled()) {
        return;
    }
    std::va_list args;
    va_start(args, format);
    std::fprintf(stderr, "[dlna] ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

uint32_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string random_token() {
    unsigned char bytes[12] = {};
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    urandom.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    static const char* hex = "0123456789abcdef";
    std::string token;
    for (const unsigned char b : bytes) {
        token += hex[b >> 4];
        token += hex[b & 15];
    }
    return token;
}

bool send_all(int fd, const char* data, size_t size) {
    while (size > 0) {
        const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

/** Conexión TCP con un tiempo máximo. -1 si no se puede. */
int connect_with_timeout(const dlna::Url& url, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &result) != 0 || !result) {
        return -1;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        freeaddrinfo(result);
        return -1;
    }
    int rc = ::connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    if (rc < 0) {
        pollfd pfd{fd, POLLOUT, 0};
        if (::poll(&pfd, 1, timeout_ms) <= 0) {
            ::close(fd);
            return -1;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error != 0) {
            ::close(fd);
            return -1;
        }
    }
    // De vuelta a bloqueante, con tiempos máximos para leer y escribir.
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

/**
 * Petición HTTP/1.1 con «Connection: close»: manda, lee hasta que la otra parte cierra y
 * devuelve el estado y el cuerpo. `false` si no hubo respuesta.
 */
bool http_request(const std::string& url_text,
                  const std::string& method,
                  const std::string& extra_headers,
                  const std::string& body,
                  int timeout_ms,
                  int& status,
                  std::string& response_body) {
    dlna::Url url;
    if (!dlna::parse_url(url_text, url)) {
        return false;
    }
    const int fd = connect_with_timeout(url, timeout_ms);
    if (fd < 0) {
        return false;
    }
    std::string request = method + " " + url.path + " HTTP/1.1\r\n";
    request += "Host: " + url.host + ":" + std::to_string(url.port) + "\r\n";
    request += "User-Agent: Linux/6 UPnP/1.0 DLNADOC/1.50 ZeroGallery/1\r\n";
    request += "Connection: close\r\n";
    request += extra_headers;
    if (!body.empty() || method == "POST") {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;
    if (!send_all(fd, request.data(), request.size())) {
        ::close(fd);
        return false;
    }
    std::string response;
    char buffer[8192];
    const uint32_t deadline = now_ms() + static_cast<uint32_t>(timeout_ms);
    while (response.size() < (2u << 20)) {
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got > 0) {
            response.append(buffer, static_cast<size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR && now_ms() < deadline) {
            continue;
        }
        break;
    }
    ::close(fd);
    return dlna::parse_http_response(response, status, response_body);
}

/** IP de esta máquina por la que se llega a `host` (la que la tele podrá alcanzar). */
std::string local_address_towards(const std::string& host, int port) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(static_cast<uint16_t>(port));
    std::string result;
    // connect() de UDP no manda nada: sólo elige la ruta, y con ella la IP de salida.
    if (inet_pton(AF_INET, host.c_str(), &remote.sin_addr) == 1 &&
        ::connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
            result = text;
        }
    }
    ::close(fd);
    return result;
}

std::string temp_directory() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string base = runtime && runtime[0] ? runtime : "/tmp";
    std::string dir = base + "/zerogallery-tv-" + std::to_string(getuid());
    ::mkdir(dir.c_str(), 0700);
    return dir;
}

bool sleep_unless(const std::function<bool()>& cancelled, int ms) {
    const uint32_t end = now_ms() + static_cast<uint32_t>(ms);
    while (now_ms() < end) {
        if (cancelled()) {
            return false;
        }
        usleep(20000);
    }
    return true;
}

} // namespace

DlnaCaster::DlnaCaster() : temp_dir_(temp_directory()) {
    // sendfile() no admite MSG_NOSIGNAL: si la tele cierra a medias (lo hace al saltar o al
    // pedir otro trozo), sin esto SIGPIPE mataría la app. Con SIG_IGN sólo da EPIPE.
    std::signal(SIGPIPE, SIG_IGN);
    worker_ = std::thread(&DlnaCaster::worker_loop, this);
}

DlnaCaster::~DlnaCaster() {
    stop_discovery();
    if (discovery_.joinable()) {
        discovery_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    // Que la tele no se quede esperando un fichero que ya nadie sirve.
    if (google_) {
        if (active_) {
            google_->stop_app();
        }
        google_->close();
    }
    if (active_ && !current_.tv.av_transport.empty()) {
        std::string response;
        int upnp_error = 0;
        soap(current_.tv.av_transport, dlna::kAvTransportService, "Stop", dlna::instance_arguments(), response,
             upnp_error, 1500);
    }
    shutdown_server();
    clear_served();
    std::error_code ignored;
    std::filesystem::remove_all(temp_dir_, ignored);
}

// ---------------------------------------------------------------- buscar teles

void DlnaCaster::start_discovery() {
    std::lock_guard<std::mutex> lock(renderers_mutex_);
    discovering_ = true;
    if (discovery_alive_) {
        return; // el hilo sigue: al ver la marca otra vez, sigue buscando
    }
    if (discovery_.joinable()) {
        discovery_.join(); // ya ha acabado: no espera
    }
    discovery_alive_ = true;
    discovery_ = std::thread(&DlnaCaster::discovery_loop, this);
}

void DlnaCaster::stop_discovery() {
    // Sin esperar al hilo: puede estar leyendo una descripción y eso, en el hilo de la
    // interfaz, congelaría el teclado. Lo recoge el siguiente start o el destructor.
    discovering_ = false;
}

bool DlnaCaster::searching() const {
    return discovering_;
}

std::vector<dlna::Renderer> DlnaCaster::renderers() const {
    std::lock_guard<std::mutex> lock(renderers_mutex_);
    return renderers_;
}

uint32_t DlnaCaster::renderers_version() const {
    return renderers_version_;
}

void DlnaCaster::discovery_loop() {
    std::set<std::string> fetched;
    const auto add_from = [&](const std::string& location) {
        if (!fetched.insert(location).second) {
            return;
        }
        int status = 0;
        std::string body;
        if (!http_request(location, "GET", "", "", kDescriptionTimeoutMs, status, body) || status != 200) {
            debug("descripción sin respuesta: %s", location.c_str());
            fetched.erase(location); // se vuelve a intentar en la siguiente ronda
            return;
        }
        dlna::Renderer tv;
        if (!dlna::parse_description(body, location, tv)) {
            debug("no es un renderer: %s", location.c_str());
            return;
        }
        std::lock_guard<std::mutex> lock(renderers_mutex_);
        const bool known = std::any_of(renderers_.begin(), renderers_.end(),
                                       [&](const dlna::Renderer& r) { return r.udn == tv.udn; });
        if (!known) {
            debug("tele: %s (%s) %s", tv.name.c_str(), tv.manufacturer.c_str(), tv.av_transport.c_str());
            renderers_.push_back(tv);
            ++renderers_version_;
        }
    };

    if (const char* manual = std::getenv("ZEROGALLERY_TV"); manual && manual[0]) {
        add_from(manual);
    }

    // Aparatos Google Cast: se añaden (o se ponen al día, si cambian de IP) por su id.
    std::vector<gcast::Device> cast_devices;
    const auto add_cast = [&](const gcast::Device& device) {
        dlna::Renderer tv;
        tv.name = device.model.empty() ? device.name : device.name + " (" + device.model + ")";
        tv.manufacturer = "Google";
        tv.model = device.model;
        tv.udn = "cast:" + device.id;
        tv.cast_host = device.host;
        tv.cast_port = device.port;
        std::lock_guard<std::mutex> lock(renderers_mutex_);
        const auto known = std::find_if(renderers_.begin(), renderers_.end(),
                                        [&](const dlna::Renderer& r) { return r.udn == tv.udn; });
        if (known == renderers_.end()) {
            debug("Cast: %s en %s:%d", tv.name.c_str(), tv.cast_host.c_str(), tv.cast_port);
            renderers_.push_back(tv);
            ++renderers_version_;
        }
        else if (known->cast_host != tv.cast_host || known->cast_port != tv.cast_port) {
            *known = tv;
            ++renderers_version_;
        }
    };
    // ZEROGALLERY_CAST=ip[:puerto] añade un Chromecast a mano (redes sin multicast, pruebas).
    if (const char* manual = std::getenv("ZEROGALLERY_CAST"); manual && manual[0]) {
        gcast::Device device;
        std::string text = manual;
        const auto colon = text.find(':');
        device.host = text.substr(0, colon);
        device.port = colon == std::string::npos ? gcast::kCastPort : std::atoi(text.c_str() + colon + 1);
        device.name = "Chromecast " + device.host;
        device.id = text;
        add_cast(device);
    }

    // Un socket por interfaz con multicast (wlan0, eth0…): así la búsqueda sale por todas
    // y no sólo por la de la ruta por defecto. Tailscale no tiene multicast y no cuenta.
    std::vector<int> sockets;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (auto* it = interfaces; it; it = it->ifa_next) {
            if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) {
                continue;
            }
            if (!(it->ifa_flags & IFF_UP) || !(it->ifa_flags & IFF_MULTICAST) || (it->ifa_flags & IFF_LOOPBACK)) {
                continue;
            }
            const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            if (fd < 0) {
                continue;
            }
            auto address = *reinterpret_cast<sockaddr_in*>(it->ifa_addr);
            address.sin_port = 0;
            const unsigned char ttl = 2;
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
            setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &address.sin_addr, sizeof(address.sin_addr));
            if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
                ::close(fd);
                continue;
            }
            debug("buscando por %s", it->ifa_name);
            sockets.push_back(fd);
        }
        freeifaddrs(interfaces);
    }
    if (sockets.empty()) {
        const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            sockets.push_back(fd);
        }
    }

    sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &group.sin_addr);
    sockaddr_in mdns_group{};
    mdns_group.sin_family = AF_INET;
    mdns_group.sin_port = htons(gcast::kMdnsPort);
    inet_pton(AF_INET, gcast::kMdnsGroup, &mdns_group.sin_addr);

    while (true) {
        {
            std::lock_guard<std::mutex> lock(renderers_mutex_);
            if (!discovering_) {
                discovery_alive_ = false;
                break;
            }
        }
        for (const int fd : sockets) {
            for (const char* target : dlna::kSearchTargets) {
                const std::string request = dlna::msearch_request(target);
                ::sendto(fd, request.data(), request.size(), 0, reinterpret_cast<sockaddr*>(&group), sizeof(group));
            }
            // mDNS desde este mismo puerto (no el 5353): los aparatos contestan aquí mismo.
            const std::string query = gcast::mdns_query();
            ::sendto(fd, query.data(), query.size(), 0, reinterpret_cast<sockaddr*>(&mdns_group), sizeof(mdns_group));
        }
        const uint32_t round_end = now_ms() + kSearchPeriodMs;
        while (discovering_ && now_ms() < round_end) {
            std::vector<pollfd> fds;
            for (const int fd : sockets) {
                fds.push_back({fd, POLLIN, 0});
            }
            if (::poll(fds.data(), fds.size(), 200) <= 0) {
                continue;
            }
            for (const auto& pfd : fds) {
                if (!(pfd.revents & POLLIN)) {
                    continue;
                }
                char buffer[9000];
                sockaddr_in from{};
                socklen_t from_length = sizeof(from);
                const ssize_t got = ::recvfrom(pfd.fd, buffer, sizeof(buffer) - 1, 0,
                                               reinterpret_cast<sockaddr*>(&from), &from_length);
                if (got <= 0) {
                    continue;
                }
                std::string location;
                std::string usn;
                char from_ip[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof(from_ip));
                cast_devices.clear();
                if (gcast::parse_mdns_response(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(got),
                                               from_ip, cast_devices)) {
                    for (const auto& device : cast_devices) {
                        add_cast(device);
                    }
                    continue;
                }
                if (dlna::parse_ssdp_response(std::string_view(buffer, static_cast<size_t>(got)), location, usn)) {
                    add_from(location);
                }
            }
        }
    }
    for (const int fd : sockets) {
        ::close(fd);
    }
}

// ---------------------------------------------------------------- órdenes

void DlnaCaster::cast(const dlna::Renderer& tv, const std::string& path, bool video) {
    CastMedia media;
    media.path = path;
    media.media_class = video ? dlna::MediaClass::Video : dlna::MediaClass::Photo;
    cast(tv, media);
}

void DlnaCaster::cast(const dlna::Renderer& tv, const CastMedia& media) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wanted_.tv.udn != tv.udn) {
            // Otra tele: su volumen se pregunta de nuevo.
            volume_ = -1;
            muted_ = false;
            volume_dirty_ = mute_dirty_ = false;
            volume_wanted_ = !tv.rendering_control.empty() || tv.is_cast();
        }
        wanted_.generation += 1;
        wanted_.tv = tv;
        wanted_.media = media;
        stop_requested_ = false;
        toggles_ = 0;
        pause_requested_ = false;
        seek_ = 0;
        finished_ = false;
        advanced_ = false;
        next_dirty_ = false;
        next_wanted_ = CastMedia{};
        next_url_.clear();
        active_ = true;
        error_.clear();
    }
    state_ = CastState::Sending;
    position_ = 0;
    duration_ = 0;
    wake_.notify_all();
}

void DlnaCaster::toggle_pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++toggles_;
    }
    wake_.notify_all();
}

void DlnaCaster::pause() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pause_requested_ = true;
    }
    wake_.notify_all();
}

void DlnaCaster::set_next(const CastMedia& media) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Cast no tiene «la siguiente» como DLNA: se manda al acabar (tarda poco).
        if (!active_ || wanted_.tv.udn == next_unsupported_udn_ || wanted_.tv.is_cast()) {
            return;
        }
        next_wanted_ = media;
        next_dirty_ = true;
    }
    wake_.notify_all();
}

bool DlnaCaster::take_advanced() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool advanced = advanced_;
    advanced_ = false;
    return advanced;
}

bool DlnaCaster::take_finished() {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool finished = finished_;
    finished_ = false;
    return finished;
}

int DlnaCaster::volume() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return volume_;
}

bool DlnaCaster::muted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return muted_;
}

int DlnaCaster::change_volume(int delta) {
    int volume = -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (volume_ < 0) {
            volume_wanted_ = !wanted_.tv.rendering_control.empty() || wanted_.tv.is_cast();
        }
        else {
            volume_ = std::clamp(volume_ + delta, 0, 100);
            volume_dirty_ = true;
            if (muted_ && delta > 0) {
                muted_ = false; // subir el volumen quita el silencio, como en la tele
                mute_dirty_ = true;
            }
            volume = volume_;
        }
    }
    wake_.notify_all();
    return volume;
}

bool DlnaCaster::toggle_mute() {
    bool muted = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        muted_ = !muted_;
        mute_dirty_ = true;
        muted = muted_;
    }
    wake_.notify_all();
    return muted;
}

void DlnaCaster::seek(double seconds) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seek_ += seconds;
    }
    wake_.notify_all();
}

void DlnaCaster::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        stop_requested_ = true;
        active_ = false;
        wanted_.generation += 1; // abandona lo que se esté preparando
        wanted_.media = CastMedia{};
    }
    state_ = CastState::Off;
    wake_.notify_all();
}

bool DlnaCaster::active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

std::string DlnaCaster::tv_name() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wanted_.tv.name;
}

std::string DlnaCaster::tv_udn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wanted_.tv.udn;
}

CastState DlnaCaster::state() const {
    return state_;
}

std::string DlnaCaster::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

double DlnaCaster::position() const {
    return position_;
}

double DlnaCaster::duration() const {
    return duration_;
}

std::string DlnaCaster::take_notice() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string notice;
    notice.swap(notice_);
    return notice;
}

bool DlnaCaster::superseded(uint64_t generation) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quit_ || wanted_.generation != generation;
}

void DlnaCaster::set_state(CastState state) {
    state_ = state;
}

void DlnaCaster::fail(uint64_t generation, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (wanted_.generation != generation) {
        return;
    }
    debug("error: %s", message.c_str());
    error_ = message;
    state_ = CastState::Error;
}

bool DlnaCaster::soap(const std::string& control_url,
                      const char* service,
                      const char* action,
                      const std::string& arguments,
                      std::string& response,
                      int& upnp_error,
                      int timeout_ms) {
    const std::string body = dlna::soap_body(service, action, arguments);
    const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPACTION: " +
                                dlna::soap_action_header(service, action) + "\r\n";
    int status = 0;
    upnp_error = 0;
    if (!http_request(control_url, "POST", headers, body, timeout_ms, status, response)) {
        debug("%s: sin respuesta", action);
        upnp_error = -1;
        return false;
    }
    if (status != 200) {
        upnp_error = dlna::soap_error_code(response);
        if (upnp_error == 0) {
            upnp_error = status;
        }
        debug("%s: HTTP %d, error %d", action, status, upnp_error);
        return false;
    }
    return true;
}

void DlnaCaster::worker_loop() {
    uint32_t last_poll = 0;
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto pending = [&] {
            return quit_ || stop_requested_ || wanted_.generation != current_generation_ || toggles_ > 0 ||
                   pause_requested_ || seek_ != 0 ||
                   (active_ && (volume_dirty_ || mute_dirty_ || volume_wanted_)) ||
                   (next_dirty_ && (state_ == CastState::Playing || state_ == CastState::Paused));
        };
        // Sin nada en la tele no hay nada que vigilar: se duerme hasta que se pida algo.
        if (active_) {
            wake_.wait_for(lock, std::chrono::milliseconds(200), pending);
        }
        else {
            wake_.wait(lock, pending);
        }
        if (quit_) {
            return;
        }
        if (stop_requested_) {
            stop_requested_ = false;
            current_generation_ = wanted_.generation;
            lock.unlock();
            do_stop();
            continue;
        }
        if (wanted_.generation != current_generation_) {
            Job job = wanted_;
            current_generation_ = job.generation;
            lock.unlock();
            if (!job.media.path.empty()) {
                current_ = job;
                do_cast(job);
                last_poll = now_ms();
            }
            continue;
        }
        if (toggles_ > 0) {
            const int toggles = toggles_;
            toggles_ = 0;
            lock.unlock();
            // Dos pulsaciones seguidas se anulan.
            if (toggles % 2 == 1) {
                do_toggle();
            }
            continue;
        }
        if (pause_requested_) {
            pause_requested_ = false;
            lock.unlock();
            if (state_ == CastState::Playing) {
                do_toggle();
            }
            continue;
        }
        if (volume_dirty_ || mute_dirty_ || volume_wanted_) {
            lock.unlock();
            do_volume();
            continue;
        }
        if (next_dirty_ && (state_ == CastState::Playing || state_ == CastState::Paused)) {
            lock.unlock();
            do_set_next();
            continue;
        }
        if (seek_ != 0) {
            const double delta = seek_;
            seek_ = 0;
            lock.unlock();
            do_seek(delta);
            continue;
        }
        const bool active = active_;
        lock.unlock();
        if (active && now_ms() - last_poll >= static_cast<uint32_t>(kPollPeriodMs)) {
            last_poll = now_ms();
            poll_tv();
        }
    }
}

bool DlnaCaster::convert_photo(uint64_t generation, const std::string& path, int orientation, const std::string& out) {
    std::string filter;
    const std::string rotate = dlna::orientation_filter(orientation);
    if (!rotate.empty()) {
        filter = rotate + ",";
    }
    filter += kPhotoScale;
    filter += ",format=yuvj420p";

    std::vector<std::string> args = {"ffmpeg", "-v", "error", "-nostdin", "-y"};
    if (dlna::mime_type(path) == "image/jpeg") {
        // El giro lo pone el filtro, según el EXIF que ya se ha leído.
        args.push_back("-noautorotate");
    }
    args.insert(args.end(), {"-i", path, "-frames:v", "1", "-vf", filter, "-q:v", "3", "-f", "image2", out});
    std::vector<char*> argv;
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        return false;
    }
    if (pid == 0) {
#ifdef __linux__
        prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            if (!debug_enabled()) {
                dup2(null_fd, STDERR_FILENO);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    const uint32_t deadline = now_ms() + kConvertTimeoutMs;
    int status = 0;
    while (true) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            break;
        }
        if (done < 0 && errno != EINTR) {
            return false;
        }
        if (superseded(generation) || now_ms() > deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            return false;
        }
        usleep(20000);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool DlnaCaster::prepare(const Job& job,
                         std::string& url,
                         std::string& metadata,
                         std::string* mime_out,
                         dlna::ItemInfo* item_out) {
    const uint64_t generation = job.generation;
    const CastMedia& media = job.media;
    const std::string name = std::filesystem::path(media.path).filename().string();
    Served served;
    served.path = media.path;
    std::string url_name = dlna::url_safe_name(name);
    std::string profile;
    const dlna::MediaClass media_class = media.media_class;

    if (job.tv.is_cast() && media_class != dlna::MediaClass::Photo) {
        // El reproductor de Google tiene su propia lista de tipos (un MOV va como MP4).
        served.mime = gcast::cast_mime(name);
        if (served.mime.empty()) {
            served.mime = dlna::choose_mime(name, "");
        }
    }
    else if (media_class == dlna::MediaClass::Video) {
        served.mime = dlna::choose_mime(name, sink_);
    }
    else if (media_class == dlna::MediaClass::Audio) {
        served.mime = dlna::choose_mime(name, sink_);
        profile = dlna::audio_profile(served.mime);
    }
    else {
        std::vector<uint8_t> head(512 * 1024);
        std::ifstream file(media.path, std::ios::binary);
        file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(file.gcount()));
        dlna::JpegInfo info;
        const bool jpeg = dlna::parse_jpeg(head.data(), head.size(), info);
        if (dlna::photo_needs_conversion(jpeg ? &info : nullptr)) {
            const std::string out = temp_dir_ + "/" + std::to_string(generation) + ".jpg";
            debug("convirtiendo a JPEG (%s)", jpeg ? "JPEG raro" : "otro formato");
            if (!convert_photo(generation, media.path, jpeg ? info.orientation : 1, out)) {
                if (!superseded(generation)) {
                    fail(generation, "Can't convert this photo");
                }
                ::unlink(out.c_str());
                return false;
            }
            served.path = out;
            served.temporary = true;
            url_name = dlna::url_safe_name(std::filesystem::path(name).stem().string() + ".jpg");
            head.assign(512 * 1024, 0);
            std::ifstream converted(out, std::ios::binary);
            converted.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
            head.resize(static_cast<size_t>(converted.gcount()));
            if (dlna::parse_jpeg(head.data(), head.size(), info)) {
                profile = dlna::jpeg_profile(info.width, info.height);
            }
        }
        else {
            profile = dlna::jpeg_profile(info.width, info.height);
        }
        served.mime = "image/jpeg";
    }
    if (superseded(generation)) {
        if (served.temporary) {
            ::unlink(served.path.c_str());
        }
        return false;
    }
    served.features = dlna::content_features(media_class, profile);

    struct stat st{};
    const uint64_t size = ::stat(served.path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;

    if (!ensure_server()) {
        fail(generation, "Can't start the file server");
        return false;
    }
    dlna::Url tv_url;
    if (job.tv.is_cast()) {
        tv_url.host = job.tv.cast_host;
        tv_url.port = job.tv.cast_port;
    }
    else {
        dlna::parse_url(job.tv.av_transport, tv_url);
    }
    const std::string address = local_address_towards(tv_url.host, tv_url.port);
    if (address.empty()) {
        fail(generation, "No network to reach the TV");
        return false;
    }
    const std::string origin = "http://" + address + ":" + std::to_string(port_);
    dlna::ItemInfo item;
    item.title = media.title.empty() ? std::filesystem::path(name).stem().string() : media.title;
    item.artist = media.artist;
    item.album = media.album;
    if (!media.cover_path.empty() && ::access(media.cover_path.c_str(), R_OK) == 0) {
        // La carátula va por el mismo servidor; la tele la pide aparte.
        Served cover;
        cover.path = media.cover_path;
        cover.mime = dlna::mime_type(media.cover_path);
        if (cover.mime.empty()) {
            cover.mime = "image/jpeg";
        }
        cover.features = dlna::content_features(dlna::MediaClass::Photo, "");
        const std::string cover_name = dlna::url_safe_name(std::filesystem::path(media.cover_path).filename().string());
        item.cover_url = origin + "/m/" + add_served(std::move(cover)) + "/" + cover_name;
    }
    const std::string mime = served.mime;
    if (mime_out) {
        *mime_out = mime;
    }
    if (item_out) {
        *item_out = item;
    }
    const std::string token = add_served(std::move(served));
    url = origin + "/m/" + token + "/" + url_name;
    metadata = dlna::didl_metadata(item, url, mime, media_class, profile, size);
    debug("URL %s (%s, %s)", url.c_str(), mime.c_str(), profile.c_str());
    return true;
}

void DlnaCaster::do_cast(const Job& job) {
    const uint64_t generation = job.generation;
    set_state(CastState::Sending);
    position_ = 0;
    duration_ = 0;
    cast_started_ms_ = now_ms();
    poll_failures_ = 0;
    const CastMedia& media = job.media;
    debug("mandando %s a %s", media.path.c_str(), job.tv.name.c_str());

    if (job.tv.is_cast()) {
        cast_google(job);
        return;
    }

    // Qué tipos dice aceptar esta tele: se pregunta una vez por tele.
    if (sink_udn_ != job.tv.udn) {
        sink_.clear();
        sink_udn_ = job.tv.udn;
        if (!job.tv.connection_manager.empty()) {
            std::string response;
            int upnp_error = 0;
            if (soap(job.tv.connection_manager, dlna::kConnectionManagerService, "GetProtocolInfo",
                     "", response, upnp_error)) {
                dlna::xml_value(response, "Sink", sink_);
            }
        }
    }
    if (superseded(generation)) {
        return;
    }

    std::string url;
    std::string metadata;
    if (!prepare(job, url, metadata)) {
        return;
    }

    std::string response;
    int upnp_error = 0;
    bool ok = soap(job.tv.av_transport, dlna::kAvTransportService, "SetAVTransportURI",
                   dlna::set_uri_arguments(url, metadata), response, upnp_error, kPlayTimeoutMs);
    if (!ok && upnp_error != -1 && upnp_error != 714 && !superseded(generation)) {
        // Hay teles (Samsung) que no cambian de fichero sin pararlas antes.
        std::string ignored;
        int stop_error = 0;
        soap(job.tv.av_transport, dlna::kAvTransportService, "Stop", dlna::instance_arguments(), ignored, stop_error);
        if (!sleep_unless([&] { return superseded(generation); }, 400)) {
            return;
        }
        ok = soap(job.tv.av_transport, dlna::kAvTransportService, "SetAVTransportURI",
                  dlna::set_uri_arguments(url, metadata), response, upnp_error, kPlayTimeoutMs);
    }
    if (superseded(generation)) {
        return;
    }
    if (!ok) {
        fail(generation, upnp_error == -1 ? "The TV isn't answering" : dlna::soap_error_text(upnp_error));
        return;
    }

    // Play justo después de SetAVTransportURI falla en algunas (701: aún cargando): se
    // insiste un poco.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t asked = now_ms();
        ok = soap(job.tv.av_transport, dlna::kAvTransportService, "Play", dlna::play_arguments(), response,
                  upnp_error, kPlayTimeoutMs);
        debug("Play: %s en %u ms", ok ? "bien" : "mal", now_ms() - asked);
        if (ok || upnp_error == -1 || superseded(generation)) {
            break;
        }
        if (!sleep_unless([&] { return superseded(generation); }, 500)) {
            return;
        }
    }
    if (superseded(generation)) {
        return;
    }
    if (!ok) {
        fail(generation, upnp_error == -1 ? "The TV isn't answering" : dlna::soap_error_text(upnp_error));
        return;
    }
    debug("en la tele");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reached_playing_ = false;
        if (media.duration > 0) {
            duration_ = media.duration;
        }
    }
    set_state(job.playable() ? CastState::Playing : CastState::Showing);
}

void DlnaCaster::cast_google(const Job& job) {
    const uint64_t generation = job.generation;
    if (!google_) {
        google_ = std::make_unique<GoogleCast>();
    }
    if (!google_->connect(job.tv.cast_host, job.tv.cast_port)) {
        fail(generation, "Can't reach the Chromecast");
        return;
    }
    std::string error;
    if (!google_->ensure_media_app(error)) {
        fail(generation, error);
        return;
    }
    if (superseded(generation)) {
        return;
    }
    std::string url;
    std::string metadata;
    std::string mime;
    dlna::ItemInfo item;
    if (!prepare(job, url, metadata, &mime, &item)) {
        return;
    }
    gcast::LoadInfo info;
    info.url = url;
    info.mime = mime;
    switch (job.media.media_class) {
        case dlna::MediaClass::Photo: info.kind = gcast::MediaKind::Photo; break;
        case dlna::MediaClass::Video: info.kind = gcast::MediaKind::Video; break;
        case dlna::MediaClass::Audio: info.kind = gcast::MediaKind::Audio; break;
    }
    info.title = item.title;
    info.artist = item.artist;
    info.album = item.album;
    info.cover_url = item.cover_url;
    debug("LOAD %s (%s) en %s", url.c_str(), mime.c_str(), job.tv.name.c_str());
    if (!google_->load(info, error)) {
        if (!superseded(generation)) {
            fail(generation, error);
        }
        return;
    }
    if (superseded(generation)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reached_playing_ = false;
        if (job.media.duration > 0) {
            duration_ = job.media.duration;
        }
    }
    debug("en el Chromecast");
    set_state(job.playable() ? CastState::Playing : CastState::Showing);
}

void DlnaCaster::poll_google(const Job& job) {
    if (!google_ || !google_->connected()) {
        fail(job.generation, "Lost the Chromecast");
        return;
    }
    google_->request_media_status();
    if (superseded(job.generation)) {
        return;
    }
    const auto media = google_->media_status();
    std::lock_guard<std::mutex> lock(mutex_);
    if (media.player_state == "PLAYING") {
        reached_playing_ = true;
        set_state(CastState::Playing);
    }
    else if (media.player_state == "PAUSED") {
        set_state(CastState::Paused);
    }
    else if (media.player_state == "IDLE" && state_ != CastState::Stopped) {
        if (media.idle_reason == "ERROR") {
            error_ = "The Chromecast can't play this";
            state_ = CastState::Error;
            return;
        }
        // FINISHED es que ha llegado al final; CANCELLED o INTERRUPTED, que lo han parado
        // (desde el mando, otra app…), y entonces no toca la siguiente.
        if (reached_playing_ || now_ms() - cast_started_ms_ > kStartGraceMs) {
            if (media.idle_reason == "FINISHED" && reached_playing_) {
                finished_ = true;
                debug("ha acabado");
            }
            set_state(CastState::Stopped);
        }
    }
    position_ = google_->position();
    if (media.duration > 0) {
        duration_ = media.duration;
    }
}

void DlnaCaster::poll_tv() {
    Job job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = current_;
    }
    const auto state = state_.load();
    if (!job.playable() || state == CastState::Error || state == CastState::Off) {
        return;
    }
    if (job.tv.is_cast()) {
        poll_google(job);
        return;
    }
    std::string response;
    int upnp_error = 0;
    if (!soap(job.tv.av_transport, dlna::kAvTransportService, "GetTransportInfo", dlna::instance_arguments(),
              response, upnp_error, 2500)) {
        if (++poll_failures_ >= 4) {
            fail(job.generation, "Lost the TV");
        }
        return;
    }
    poll_failures_ = 0;
    std::string value;
    dlna::xml_value(response, "CurrentTransportState", value);
    const auto transport = dlna::parse_transport_state(value);
    if (superseded(job.generation)) {
        return;
    }
    switch (transport) {
        case dlna::TransportState::Playing: {
            std::lock_guard<std::mutex> lock(mutex_);
            reached_playing_ = true;
            stopped_since_ = 0;
            set_state(CastState::Playing);
            break;
        }
        case dlna::TransportState::Paused:
            set_state(CastState::Paused);
            break;
        case dlna::TransportState::Stopped:
        case dlna::TransportState::NoMedia: {
            // Al empezar, alguna dice STOPPED antes de ponerse a cargar.
            std::lock_guard<std::mutex> lock(mutex_);
            if (!next_url_.empty() && reached_playing_) {
                // Con la siguiente dada, la tele pasa un momento por STOPPED al cambiar:
                // sólo si se queda parada es que no ha sabido encadenar.
                if (stopped_since_ == 0) {
                    stopped_since_ = now_ms();
                }
                if (now_ms() - stopped_since_ < kNextGraceMs) {
                    break;
                }
                debug("la tele no ha pasado sola a la siguiente");
                next_url_.clear();
            }
            if ((reached_playing_ || now_ms() - cast_started_ms_ > kStartGraceMs) && state != CastState::Stopped) {
                // Lo que se sabe de antes de pararse (esta vuelta aún no ha preguntado la
                // posición, que al parar muchas ponen a cero) dice si ha llegado al final.
                if (reached_playing_ && dlna::reached_end(position_, duration_)) {
                    finished_ = true;
                    debug("ha acabado");
                }
                set_state(CastState::Stopped);
            }
            break;
        }
        default:
            break;
    }

    if (soap(job.tv.av_transport, dlna::kAvTransportService, "GetPositionInfo", dlna::instance_arguments(),
             response, upnp_error, 2500)) {
        // ¿Ha pasado sola a la siguiente? Lo dice el fichero que está tocando.
        std::string uri;
        if (dlna::xml_value(response, "TrackURI", uri)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!next_url_.empty() && uri == next_url_ && wanted_.generation == job.generation) {
                debug("la tele ha pasado sola a %s", next_media_.path.c_str());
                wanted_.media = next_media_;
                current_.media = next_media_;
                next_url_.clear();
                advanced_ = true;
                finished_ = false;
                reached_playing_ = true;
                stopped_since_ = 0;
                cast_started_ms_ = now_ms();
                duration_ = next_media_.duration;
                set_state(CastState::Playing);
            }
        }
        double seconds = 0;
        if (dlna::xml_value(response, "RelTime", value) && dlna::parse_hms(value, seconds)) {
            position_ = seconds;
        }
        if (dlna::xml_value(response, "TrackDuration", value) && dlna::parse_hms(value, seconds) && seconds > 0) {
            duration_ = seconds;
        }
    }
}

void DlnaCaster::do_set_next() {
    Job job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job.generation = wanted_.generation;
        job.tv = wanted_.tv;
        job.media = next_wanted_;
        next_dirty_ = false;
    }
    if (job.media.path.empty() || !job.playable()) {
        return;
    }
    std::string url;
    std::string metadata;
    if (!prepare(job, url, metadata)) {
        return;
    }
    std::string response;
    int upnp_error = 0;
    const bool ok = soap(job.tv.av_transport, dlna::kAvTransportService, "SetNextAVTransportURI",
                         dlna::set_next_uri_arguments(url, metadata), response, upnp_error, kPlayTimeoutMs);
    std::lock_guard<std::mutex> lock(mutex_);
    if (wanted_.generation != job.generation) {
        return;
    }
    if (ok) {
        debug("siguiente dada: %s", job.media.path.c_str());
        next_url_ = url;
        next_media_ = job.media;
    }
    else if (upnp_error != -1) {
        // No sabe (401, 602…): no se vuelve a intentar con esta tele.
        debug("la tele no admite SetNextAVTransportURI (%d)", upnp_error);
        next_unsupported_udn_ = job.tv.udn;
    }
}

void DlnaCaster::do_toggle() {
    Job job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = current_;
        if (!active_) {
            return;
        }
    }
    if (!job.playable()) {
        return;
    }
    std::string response;
    int upnp_error = 0;
    const auto state = state_.load();
    const bool google = job.tv.is_cast() && google_ && google_->connected();
    if (state == CastState::Playing) {
        if (google ? google_->pause()
                   : soap(job.tv.av_transport, dlna::kAvTransportService, "Pause", dlna::instance_arguments(),
                          response, upnp_error)) {
            set_state(CastState::Paused);
        }
        return;
    }
    if (state == CastState::Paused) {
        if (google ? google_->play()
                   : soap(job.tv.av_transport, dlna::kAvTransportService, "Play", dlna::play_arguments(), response,
                          upnp_error)) {
            set_state(CastState::Playing);
        }
        return;
    }
    if (state == CastState::Stopped || state == CastState::Error) {
        // Acabado: unas lo repiten con Play; otras ya han olvidado el fichero y hay que
        // mandarlo de nuevo, que vale para todas.
        cast(job.tv, job.media);
    }
}

void DlnaCaster::do_seek(double delta) {
    Job job;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = current_;
        if (!active_) {
            return;
        }
    }
    if (!job.playable()) {
        return;
    }
    if (job.tv.is_cast()) {
        if (!google_ || !google_->connected()) {
            return;
        }
        const double target = dlna::seek_target(google_->position(), delta, duration_);
        if (google_->seek(target)) {
            position_ = target;
        }
        else {
            std::lock_guard<std::mutex> lock(mutex_);
            notice_ = "The Chromecast can't seek here";
        }
        return;
    }
    std::string response;
    int upnp_error = 0;
    double position = position_;
    double duration = duration_;
    std::string value;
    if (soap(job.tv.av_transport, dlna::kAvTransportService, "GetPositionInfo", dlna::instance_arguments(), response,
             upnp_error, 2500)) {
        double seconds = 0;
        if (dlna::xml_value(response, "RelTime", value) && dlna::parse_hms(value, seconds)) {
            position = seconds;
        }
        if (dlna::xml_value(response, "TrackDuration", value) && dlna::parse_hms(value, seconds) && seconds > 0) {
            duration = seconds;
        }
    }
    const double target = dlna::seek_target(position, delta, duration);
    bool ok = soap(job.tv.av_transport, dlna::kAvTransportService, "Seek", dlna::seek_arguments("REL_TIME", target),
                   response, upnp_error);
    if (!ok && upnp_error == 710) {
        ok = soap(job.tv.av_transport, dlna::kAvTransportService, "Seek", dlna::seek_arguments("ABS_TIME", target),
                  response, upnp_error);
    }
    if (ok) {
        position_ = target;
        duration_ = duration;
    }
    else {
        std::lock_guard<std::mutex> lock(mutex_);
        notice_ = upnp_error == -1 ? "The TV isn't answering" : dlna::soap_error_text(upnp_error);
    }
}

void DlnaCaster::do_volume() {
    Job job;
    int volume = -1;
    bool muted = false;
    bool set_volume = false;
    bool set_mute = false;
    bool get = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job = wanted_;
        volume = volume_;
        muted = muted_;
        set_volume = volume_dirty_;
        set_mute = mute_dirty_;
        get = volume_wanted_;
        volume_dirty_ = mute_dirty_ = volume_wanted_ = false;
    }
    if (job.tv.is_cast()) {
        // Volumen del aparato Cast (0..1 en su lado).
        if (!google_ || !google_->connected()) {
            return;
        }
        if (set_mute) {
            google_->set_muted(muted);
        }
        if (set_volume && volume >= 0) {
            google_->set_volume(volume / 100.0);
        }
        if (get && google_->request_receiver_status()) {
            const auto status = google_->receiver_status();
            std::lock_guard<std::mutex> lock(mutex_);
            if (status.volume >= 0 && !volume_dirty_) {
                volume_ = static_cast<int>(status.volume * 100 + 0.5);
            }
            if (!mute_dirty_) {
                muted_ = status.muted;
            }
        }
        return;
    }
    const std::string& url = job.tv.rendering_control;
    if (url.empty()) {
        return;
    }
    std::string response;
    int upnp_error = 0;
    if (set_mute) {
        soap(url, dlna::kRenderingControlService, "SetMute", dlna::set_mute_arguments(muted), response, upnp_error,
             2500);
    }
    if (set_volume && volume >= 0) {
        soap(url, dlna::kRenderingControlService, "SetVolume", dlna::set_volume_arguments(volume), response,
             upnp_error, 2500);
    }
    if (get) {
        std::string value;
        if (soap(url, dlna::kRenderingControlService, "GetVolume", dlna::channel_arguments(), response, upnp_error,
                 2500) &&
            dlna::xml_value(response, "CurrentVolume", value)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!volume_dirty_) {
                volume_ = std::clamp(std::atoi(value.c_str()), 0, 100);
            }
        }
        if (soap(url, dlna::kRenderingControlService, "GetMute", dlna::channel_arguments(), response, upnp_error,
                 2500) &&
            dlna::xml_value(response, "CurrentMute", value)) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!mute_dirty_) {
                muted_ = value == "1" || value == "true";
            }
        }
        debug("volumen de la tele: %d%s", volume_, muted_ ? " (silencio)" : "");
    }
}

void DlnaCaster::do_stop() {
    Job job = current_;
    current_ = Job{};
    if (google_) {
        google_->stop_app();
        google_->close();
    }
    if (!job.tv.av_transport.empty()) {
        std::string response;
        int upnp_error = 0;
        soap(job.tv.av_transport, dlna::kAvTransportService, "Stop", dlna::instance_arguments(), response, upnp_error,
             2500);
    }
    shutdown_server();
    clear_served();
    set_state(CastState::Off);
}

// ---------------------------------------------------------------- servidor de ficheros

bool DlnaCaster::ensure_server() {
    if (listen_fd_ >= 0) {
        return true;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = 0; // el que dé el sistema
    socklen_t length = sizeof(address);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(fd, 16) < 0 ||
        getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        ::close(fd);
        return false;
    }
    listen_fd_ = fd;
    port_ = ntohs(address.sin_port);
    server_quit_ = false;
    server_ = std::thread(&DlnaCaster::server_loop, this);
    debug("servidor en el puerto %d", port_);
    return true;
}

void DlnaCaster::shutdown_server() {
    if (listen_fd_ < 0) {
        return;
    }
    server_quit_ = true;
    ::shutdown(listen_fd_, SHUT_RDWR);
    if (server_.joinable()) {
        server_.join();
    }
    ::close(listen_fd_);
    listen_fd_ = -1;
    port_ = 0;
    std::vector<Connection> connections;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections.swap(connections_);
    }
    for (auto& connection : connections) {
        ::shutdown(connection.fd, SHUT_RDWR);
    }
    for (auto& connection : connections) {
        if (connection.thread.joinable()) {
            connection.thread.join();
        }
        ::close(connection.fd);
    }
}

std::string DlnaCaster::add_served(Served served) {
    const std::string token = random_token();
    std::lock_guard<std::mutex> lock(served_mutex_);
    served_[token] = std::move(served);
    served_order_.push_back(token);
    while (served_order_.size() > kServedKept) {
        const auto old = served_.find(served_order_.front());
        if (old != served_.end()) {
            if (old->second.temporary) {
                ::unlink(old->second.path.c_str());
            }
            served_.erase(old);
        }
        served_order_.erase(served_order_.begin());
    }
    return token;
}

void DlnaCaster::clear_served() {
    std::lock_guard<std::mutex> lock(served_mutex_);
    for (const auto& [token, served] : served_) {
        if (served.temporary) {
            ::unlink(served.path.c_str());
        }
    }
    served_.clear();
    served_order_.clear();
}

void DlnaCaster::server_loop() {
    while (!server_quit_) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 250);
        // Las conexiones que ya han acabado se recogen aquí.
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            for (auto it = connections_.begin(); it != connections_.end();) {
                if (it->done->load()) {
                    if (it->thread.joinable()) {
                        it->thread.join();
                    }
                    ::close(it->fd);
                    it = connections_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }
        if (ready <= 0 || !(pfd.revents & POLLIN) || server_quit_) {
            continue;
        }
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        std::lock_guard<std::mutex> lock(connections_mutex_);
        if (connections_.size() >= 16) { // una tele abre dos o tres; más es raro
            ::close(fd);
            continue;
        }
        timeval tv{15, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        Connection connection;
        connection.fd = fd;
        connection.done = std::make_shared<std::atomic<bool>>(false);
        connection.thread = std::thread(&DlnaCaster::serve_connection, this, fd, connection.done);
        connections_.push_back(std::move(connection));
    }
}

void DlnaCaster::serve_connection(int fd, std::shared_ptr<std::atomic<bool>> done) {
    // Una petición por conexión («Connection: close»): es lo más sencillo y todas las
    // teles lo aceptan. El descriptor lo cierra quien recoge el hilo.
    std::string header;
    char buffer[4096];
    while (header.find("\r\n\r\n") == std::string::npos && header.size() < 16384) {
        const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0) {
            done->store(true);
            return;
        }
        header.append(buffer, static_cast<size_t>(got));
    }
    const auto reply = [&](const char* status) {
        std::string text = std::string("HTTP/1.1 ") + status + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(fd, text.data(), text.size());
    };

    dlna::HttpRequest request;
    if (!dlna::parse_http_request(header, request)) {
        reply("400 Bad Request");
        done->store(true);
        return;
    }
    debug("%s %s%s%s", request.method.c_str(), request.path.c_str(),
          request.headers.count("range") ? " Range: " : "",
          request.headers.count("range") ? request.headers["range"].c_str() : "");

    // /m/<token>/<nombre>: el nombre no cuenta, sólo está para que la tele vea la extensión.
    Served served;
    bool found = false;
    if (request.path.rfind("/m/", 0) == 0) {
        const auto slash = request.path.find('/', 3);
        const std::string token = request.path.substr(3, slash == std::string::npos ? std::string::npos : slash - 3);
        std::lock_guard<std::mutex> lock(served_mutex_);
        const auto it = served_.find(token);
        if (it != served_.end()) {
            served = it->second;
            found = true;
        }
    }
    if (request.method != "GET" && request.method != "HEAD") {
        reply("501 Not Implemented");
        done->store(true);
        return;
    }
    const int file = found ? ::open(served.path.c_str(), O_RDONLY | O_CLOEXEC) : -1;
    struct stat st{};
    if (file < 0 || fstat(file, &st) < 0) {
        if (file >= 0) {
            ::close(file);
        }
        reply("404 Not Found");
        done->store(true);
        return;
    }
    const uint64_t size = static_cast<uint64_t>(st.st_size);
    uint64_t first = 0;
    uint64_t last = size == 0 ? 0 : size - 1;
    bool partial = false;
    if (const auto range = request.headers.find("range"); range != request.headers.end()) {
        if (!dlna::parse_range(range->second, size, first, last)) {
            std::string text = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                               std::to_string(size) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(fd, text.data(), text.size());
            ::close(file);
            done->store(true);
            return;
        }
        partial = true;
    }
    const uint64_t length = size == 0 ? 0 : last - first + 1;
    const bool photo = served.mime.rfind("image/", 0) == 0;
    std::string text = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    text += "Content-Type: " + served.mime + "\r\n";
    text += "Content-Length: " + std::to_string(length) + "\r\n";
    if (partial) {
        text += "Content-Range: bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
                std::to_string(size) + "\r\n";
    }
    text += "Accept-Ranges: bytes\r\n";
    // La transferencia que pide la tele, si la pide; si no, la que toca al tipo.
    std::string transfer = photo ? "Interactive" : "Streaming";
    if (const auto mode = request.headers.find("transfermode.dlna.org"); mode != request.headers.end()) {
        transfer = mode->second;
    }
    text += "transferMode.dlna.org: " + transfer + "\r\n";
    text += "contentFeatures.dlna.org: " + served.features + "\r\n";
    text += "Server: Linux/6 UPnP/1.0 DLNADOC/1.50 ZeroGallery/1\r\n";
    text += "Cache-Control: no-cache\r\n";
    text += "Connection: close\r\n\r\n";
    if (!send_all(fd, text.data(), text.size()) || request.method == "HEAD") {
        ::close(file);
        done->store(true);
        return;
    }
    off_t offset = static_cast<off_t>(first);
    uint64_t remaining = length;
    while (remaining > 0 && !server_quit_) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, 1u << 20));
        const ssize_t sent = ::sendfile(fd, file, &offset, chunk);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            break; // la tele ha cerrado (pasa siempre que salta o cambia de fichero)
        }
        remaining -= static_cast<uint64_t>(sent);
    }
    ::close(file);
    done->store(true);
}

} // namespace platform
