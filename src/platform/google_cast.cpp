/*
 * SPDX-License-Identifier: MIT
 */

#include "google_cast.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace platform {
namespace {

// Lo que llega de libssl. Se declara a mano para no necesitar sus cabeceras al compilar.
struct SslApi {
    void* library{nullptr};
    const void* (*TLS_client_method)(){nullptr};
    void* (*SSL_CTX_new)(const void*){nullptr};
    void (*SSL_CTX_free)(void*){nullptr};
    void* (*SSL_new)(void*){nullptr};
    void (*SSL_free)(void*){nullptr};
    int (*SSL_set_fd)(void*, int){nullptr};
    int (*SSL_connect)(void*){nullptr};
    int (*SSL_read)(void*, void*, int){nullptr};
    int (*SSL_write)(void*, const void*, int){nullptr};
    int (*SSL_pending)(const void*){nullptr};
    int (*SSL_shutdown)(void*){nullptr};
    int (*SSL_get_error)(const void*, int){nullptr};

    bool ok() const { return library != nullptr; }
};

constexpr int kSslErrorWantRead = 2;
constexpr int kSslErrorWantWrite = 3;
constexpr int kHeartbeatMs = 5000;
// Sin noticias del aparato en este tiempo (ni PONG), la conexión se da por perdida.
constexpr int kSilenceMs = 20000;

const SslApi& ssl_api() {
    static const SslApi api = [] {
        SslApi a;
        for (const char* name : {"libssl.so.3", "libssl.so"}) {
            a.library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (a.library) {
                break;
            }
        }
        if (!a.library) {
            return a;
        }
        bool complete = true;
        const auto bind = [&](auto& fn, const char* symbol) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(a.library, symbol));
            complete = complete && fn != nullptr;
        };
        bind(a.TLS_client_method, "TLS_client_method");
        bind(a.SSL_CTX_new, "SSL_CTX_new");
        bind(a.SSL_CTX_free, "SSL_CTX_free");
        bind(a.SSL_new, "SSL_new");
        bind(a.SSL_free, "SSL_free");
        bind(a.SSL_set_fd, "SSL_set_fd");
        bind(a.SSL_connect, "SSL_connect");
        bind(a.SSL_read, "SSL_read");
        bind(a.SSL_write, "SSL_write");
        bind(a.SSL_pending, "SSL_pending");
        bind(a.SSL_shutdown, "SSL_shutdown");
        bind(a.SSL_get_error, "SSL_get_error");
        if (!complete) {
            dlclose(a.library);
            a = SslApi{};
        }
        return a;
    }();
    return api;
}

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
    std::fprintf(stderr, "[cast] ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

double now_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

int connect_tcp(const std::string& host, int port, int timeout_ms) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0 || !result) {
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
        int error = 0;
        socklen_t length = sizeof(error);
        if (::poll(&pfd, 1, timeout_ms) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
            error != 0) {
            ::close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

} // namespace

GoogleCast::~GoogleCast() {
    close();
}

bool GoogleCast::connected() const {
    return running_;
}

bool GoogleCast::connect(const std::string& host, int port) {
    if (running_ && host == host_ && port == port_) {
        return true;
    }
    close();
    const SslApi& api = ssl_api();
    if (!api.ok()) {
        debug("no hay libssl: sin Cast");
        return false;
    }
    fd_ = connect_tcp(host, port, 4000);
    if (fd_ < 0) {
        debug("no conecta con %s:%d", host.c_str(), port);
        return false;
    }
    ctx_ = api.SSL_CTX_new(api.TLS_client_method());
    ssl_ = ctx_ ? api.SSL_new(ctx_) : nullptr;
    if (!ssl_ || api.SSL_set_fd(ssl_, fd_) != 1 || api.SSL_connect(ssl_) != 1) {
        debug("TLS falla con %s", host.c_str());
        close();
        return false;
    }
    host_ = host;
    port_ = port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        outgoing_.clear();
        replies_.clear();
        transport_id_.clear();
        session_id_.clear();
        receiver_ = gcast::ReceiverStatus{};
        media_ = gcast::MediaStatus{};
    }
    running_ = true;
    io_ = std::thread(&GoogleCast::io_loop, this);
    send(gcast::kReceiverId, gcast::kNsConnection, "{\"type\":\"CONNECT\"}");
    debug("conectado con %s", host.c_str());
    return true;
}

void GoogleCast::close() {
    running_ = false;
    if (io_.joinable()) {
        io_.join();
    }
    const SslApi& api = ssl_api();
    if (ssl_) {
        api.SSL_shutdown(ssl_);
        api.SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_) {
        api.SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    replied_.notify_all();
}

bool GoogleCast::send(const std::string& destination, const char* name_space, const std::string& payload) {
    if (!running_) {
        return false;
    }
    gcast::Message message;
    message.source = gcast::kSenderId;
    message.destination = destination;
    message.name_space = name_space;
    message.payload = payload;
    std::lock_guard<std::mutex> lock(mutex_);
    outgoing_.push_back(gcast::encode_message(message));
    return true;
}

bool GoogleCast::call(const std::string& destination,
                      const char* name_space,
                      const char* type,
                      const std::string& fields,
                      gcast::Json& reply,
                      int timeout_ms) {
    int id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_request_++;
    }
    if (!send(destination, name_space, gcast::request(type, id, fields))) {
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const bool got = replied_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                       [&] { return !running_ || replies_.count(id) > 0; });
    if (!got || !replies_.count(id)) {
        debug("%s: sin respuesta", type);
        return false;
    }
    reply = std::move(replies_[id]);
    replies_.erase(id);
    return true;
}

void GoogleCast::io_loop() {
    const SslApi& api = ssl_api();
    std::string buffer;
    double last_heard = now_seconds();
    double last_ping = now_seconds();
    char chunk[16384];
    while (running_) {
        // Escribir lo pendiente
        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending.swap(outgoing_);
        }
        for (const auto& data : pending) {
            if (api.SSL_write(ssl_, data.data(), static_cast<int>(data.size())) <= 0) {
                debug("escritura fallida");
                running_ = false;
                break;
            }
        }
        const double now = now_seconds();
        if ((now - last_ping) * 1000 > kHeartbeatMs) {
            last_ping = now;
            gcast::Message ping{gcast::kSenderId, gcast::kReceiverId, gcast::kNsHeartbeat, "{\"type\":\"PING\"}"};
            const std::string data = gcast::encode_message(ping);
            api.SSL_write(ssl_, data.data(), static_cast<int>(data.size()));
        }
        if ((now - last_heard) * 1000 > kSilenceMs) {
            debug("el aparato no contesta");
            running_ = false;
            break;
        }

        // Leer lo que haya (TLS puede tener datos ya descifrados aunque el socket esté vacío)
        if (api.SSL_pending(ssl_) <= 0) {
            pollfd pfd{fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0) {
                continue;
            }
        }
        const int got = api.SSL_read(ssl_, chunk, sizeof(chunk));
        if (got <= 0) {
            const int error = api.SSL_get_error(ssl_, got);
            if (error == kSslErrorWantRead || error == kSslErrorWantWrite) {
                continue;
            }
            debug("conexión cerrada");
            running_ = false;
            break;
        }
        last_heard = now_seconds();
        buffer.append(chunk, static_cast<size_t>(got));
        while (buffer.size() >= 4) {
            const uint32_t length = (static_cast<uint8_t>(buffer[0]) << 24) | (static_cast<uint8_t>(buffer[1]) << 16) |
                                    (static_cast<uint8_t>(buffer[2]) << 8) | static_cast<uint8_t>(buffer[3]);
            if (length > (1u << 20)) {
                running_ = false;
                break;
            }
            if (buffer.size() < 4 + length) {
                break;
            }
            gcast::Message message;
            if (gcast::decode_message(reinterpret_cast<const uint8_t*>(buffer.data()) + 4, length, message)) {
                handle(message);
            }
            buffer.erase(0, 4 + length);
        }
    }
    replied_.notify_all();
}

void GoogleCast::handle(const gcast::Message& message) {
    gcast::Json payload;
    if (!gcast::parse_json(message.payload, payload)) {
        return;
    }
    const std::string type = payload["type"].str();
    if (message.name_space == gcast::kNsHeartbeat) {
        if (type == "PING") {
            send(message.source, gcast::kNsHeartbeat, "{\"type\":\"PONG\"}");
        }
        return;
    }
    if (message.name_space == gcast::kNsConnection && type == "CLOSE") {
        // El reproductor se ha cerrado (otra app, o desde el mando).
        std::lock_guard<std::mutex> lock(mutex_);
        if (message.source == transport_id_) {
            transport_id_.clear();
            session_id_.clear();
            media_.player_state = "IDLE";
            media_.idle_reason = "CANCELLED";
            ++media_version_;
        }
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    gcast::ReceiverStatus receiver;
    if (gcast::parse_receiver_status(payload, receiver)) {
        receiver_ = receiver;
        if (!transport_id_.empty() && receiver.transport_id != transport_id_) {
            // Ya no está el reproductor al que se estaba conectado.
            transport_id_.clear();
            session_id_.clear();
            media_.player_state = "IDLE";
            media_.idle_reason = "CANCELLED";
            ++media_version_;
        }
    }
    gcast::MediaStatus media;
    if (gcast::parse_media_status(payload, media)) {
        media_ = media;
        media_time_at_ = now_seconds();
        ++media_version_;
    }
    const int id = static_cast<int>(payload["requestId"].num(0));
    if (id > 0) {
        replies_[id] = std::move(payload);
        replied_.notify_all();
    }
}

bool GoogleCast::ensure_media_app(std::string& error) {
    gcast::Json reply;
    if (!call(gcast::kReceiverId, gcast::kNsReceiver, "GET_STATUS", "", reply)) {
        error = "The Chromecast isn't answering";
        return false;
    }
    gcast::ReceiverStatus status;
    gcast::parse_receiver_status(reply, status);
    if (status.app_id != gcast::kDefaultMediaReceiver) {
        if (!call(gcast::kReceiverId, gcast::kNsReceiver, "LAUNCH",
                  std::string("\"appId\":\"") + gcast::kDefaultMediaReceiver + "\"", reply, 20000)) {
            error = "The Chromecast didn't open the player";
            return false;
        }
        if (reply["type"].str() == "LAUNCH_ERROR") {
            error = "The Chromecast refused: " + reply["reason"].str("unknown");
            return false;
        }
        gcast::parse_receiver_status(reply, status);
        if (status.app_id != gcast::kDefaultMediaReceiver || status.transport_id.empty()) {
            error = "The Chromecast didn't open the player";
            return false;
        }
    }
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fresh = transport_id_ != status.transport_id;
        transport_id_ = status.transport_id;
        session_id_ = status.session_id;
    }
    if (fresh) {
        send(status.transport_id, gcast::kNsConnection, "{\"type\":\"CONNECT\"}");
    }
    return true;
}

bool GoogleCast::load(const gcast::LoadInfo& info, std::string& error) {
    std::string transport;
    std::string session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_id_;
        session = session_id_;
    }
    gcast::Json reply;
    // Hasta que el aparato se ha bajado el principio del fichero no contesta.
    if (!call(transport, gcast::kNsMedia, "LOAD", gcast::load_fields(session, info), reply, 30000)) {
        error = "The Chromecast isn't answering";
        return false;
    }
    const std::string type = reply["type"].str();
    if (type == "MEDIA_STATUS") {
        return true;
    }
    debug("LOAD: %s %s", type.c_str(), reply["detailedErrorCode"].is_null() ? "" : "(detalle)");
    error = type == "LOAD_FAILED" ? "The Chromecast can't play this" : "The Chromecast refused it";
    return false;
}

bool GoogleCast::play() {
    std::string transport;
    long id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_id_;
        id = media_.media_session_id;
    }
    gcast::Json reply;
    return !transport.empty() &&
           call(transport, gcast::kNsMedia, "PLAY", "\"mediaSessionId\":" + std::to_string(id), reply);
}

bool GoogleCast::pause() {
    std::string transport;
    long id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_id_;
        id = media_.media_session_id;
    }
    gcast::Json reply;
    return !transport.empty() &&
           call(transport, gcast::kNsMedia, "PAUSE", "\"mediaSessionId\":" + std::to_string(id), reply);
}

bool GoogleCast::seek(double seconds) {
    std::string transport;
    long id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_id_;
        id = media_.media_session_id;
    }
    char time[32];
    std::snprintf(time, sizeof(time), "%.3f", seconds);
    gcast::Json reply;
    return !transport.empty() && call(transport, gcast::kNsMedia, "SEEK",
                                      "\"mediaSessionId\":" + std::to_string(id) + ",\"currentTime\":" + time, reply);
}

bool GoogleCast::request_media_status() {
    std::string transport;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        transport = transport_id_;
    }
    gcast::Json reply;
    return !transport.empty() && call(transport, gcast::kNsMedia, "GET_STATUS", "", reply, 3000);
}

void GoogleCast::stop_app() {
    std::string session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        session = session_id_;
    }
    if (session.empty() || !running_) {
        return;
    }
    gcast::Json reply;
    call(gcast::kReceiverId, gcast::kNsReceiver, "STOP", "\"sessionId\":" + gcast::json_string(session), reply, 3000);
    std::lock_guard<std::mutex> lock(mutex_);
    transport_id_.clear();
    session_id_.clear();
}

bool GoogleCast::set_volume(double level) {
    char value[32];
    std::snprintf(value, sizeof(value), "%.2f", level < 0 ? 0 : (level > 1 ? 1 : level));
    gcast::Json reply;
    return call(gcast::kReceiverId, gcast::kNsReceiver, "SET_VOLUME", std::string("\"volume\":{\"level\":") + value + "}",
                reply, 3000);
}

bool GoogleCast::set_muted(bool muted) {
    gcast::Json reply;
    return call(gcast::kReceiverId, gcast::kNsReceiver, "SET_VOLUME",
                std::string("\"volume\":{\"muted\":") + (muted ? "true" : "false") + "}", reply, 3000);
}

bool GoogleCast::request_receiver_status() {
    gcast::Json reply;
    return call(gcast::kReceiverId, gcast::kNsReceiver, "GET_STATUS", "", reply, 3000);
}

gcast::MediaStatus GoogleCast::media_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return media_;
}

uint64_t GoogleCast::media_version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return media_version_;
}

gcast::ReceiverStatus GoogleCast::receiver_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receiver_;
}

double GoogleCast::position() const {
    std::lock_guard<std::mutex> lock(mutex_);
    double position = media_.current_time;
    if (media_.player_state == "PLAYING") {
        position += now_seconds() - media_time_at_;
    }
    if (media_.duration > 0 && position > media_.duration) {
        position = media_.duration;
    }
    return position;
}

} // namespace platform
