/*
 * SPDX-License-Identifier: MIT
 */

#include "kms_display.h"

#include "ffmpeg_process.h"
#include "gallery.h"
#include "hdmi.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__
#include <drm/drm.h>
#include <drm/drm_mode.h>
#define ZG_HAVE_KMS 1
#endif

namespace platform {
namespace {

#ifdef ZG_HAVE_KMS
constexpr uint32_t kFormatXrgb8888 = 0x34325258; // «XR24»
constexpr uint32_t kFormatYuv420 = 0x32315559;   // «YU12», I420

uint64_t user_pointer(const void* pointer) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer));
}

int drm_ioctl(int fd, unsigned long request, void* argument) {
    int result = 0;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && (errno == EINTR || errno == EAGAIN));
    return result;
}
#endif

std::string read_small(const std::string& path) {
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

} // namespace

std::string KmsDisplay::connected_card() {
    const std::string root = env_or("ZEROGALLERY_HDMI_SYSFS", "/sys/class/drm");
    DIR* directory = opendir(root.c_str());
    if (!directory) {
        return {};
    }
    std::string card;
    while (dirent* entry = readdir(directory)) {
        // «card0-HDMI-A-1»: la tarjeta es lo de antes del primer guion.
        const std::string name = entry->d_name;
        const auto dash = name.find('-');
        if (name.rfind("card", 0) != 0 || dash == std::string::npos || name.find("-HDMI-", dash) != dash) {
            continue;
        }
        if (hdmi::status_connected(read_small(root + "/" + name + "/status"))) {
            card = "/dev/dri/" + name.substr(0, dash);
            break;
        }
    }
    closedir(directory);
    return card;
}

KmsDisplay::~KmsDisplay() {
    close();
}

#ifdef ZG_HAVE_KMS

bool KmsDisplay::open(std::string& error) {
    close();
    const std::string card = connected_card();
    if (card.empty()) {
        error = "Nothing plugged into HDMI";
        return false;
    }
    fd_ = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        error = errno == EACCES ? "No permission for HDMI (video group?)" : "Cannot open HDMI";
        return false;
    }

    drm_mode_card_res resources{};
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETRESOURCES, &resources) != 0) {
        error = "HDMI: no display controller";
        close();
        return false;
    }
    std::vector<uint32_t> connectors(resources.count_connectors);
    std::vector<uint32_t> crtcs(resources.count_crtcs);
    std::vector<uint32_t> encoders(resources.count_encoders);
    drm_mode_card_res listed{};
    listed.count_connectors = resources.count_connectors;
    listed.count_crtcs = resources.count_crtcs;
    listed.count_encoders = resources.count_encoders;
    listed.connector_id_ptr = user_pointer(connectors.data());
    listed.crtc_id_ptr = user_pointer(crtcs.data());
    listed.encoder_id_ptr = user_pointer(encoders.data());
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETRESOURCES, &listed) != 0) {
        error = "HDMI: no display controller";
        close();
        return false;
    }

    drm_mode_modeinfo chosen{};
    uint32_t crtc_index = 0;
    for (uint32_t id : connectors) {
        // Primero se pregunta cuántos modos y codificadores hay, y luego se piden.
        drm_mode_get_connector probe{};
        probe.connector_id = id;
        if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETCONNECTOR, &probe) != 0 || probe.connector_type != DRM_MODE_CONNECTOR_HDMIA ||
            probe.connection != 1 || probe.count_modes == 0 || probe.count_encoders == 0) {
            continue;
        }
        std::vector<drm_mode_modeinfo> modes(probe.count_modes);
        std::vector<uint32_t> connector_encoders(probe.count_encoders);
        drm_mode_get_connector full{};
        full.connector_id = id;
        full.count_modes = probe.count_modes;
        full.count_encoders = probe.count_encoders;
        full.modes_ptr = user_pointer(modes.data());
        full.encoders_ptr = user_pointer(connector_encoders.data());
        if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETCONNECTOR, &full) != 0) {
            continue;
        }
        modes.resize(std::min<size_t>(modes.size(), full.count_modes));

        std::vector<hdmi::Mode> offered;
        for (const auto& mode : modes) {
            offered.push_back({mode.hdisplay, mode.vdisplay, static_cast<int>(mode.vrefresh),
                               (mode.type & DRM_MODE_TYPE_PREFERRED) != 0, (mode.flags & DRM_MODE_FLAG_INTERLACE) != 0});
        }
        const int pick = hdmi::pick_mode(offered);
        if (pick < 0) {
            continue;
        }

        drm_mode_get_encoder encoder{};
        encoder.encoder_id = connector_encoders[0];
        if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0) {
            continue;
        }
        for (uint32_t i = 0; i < crtcs.size(); ++i) {
            if (encoder.possible_crtcs & (1U << i)) {
                crtc_ = crtcs[i];
                crtc_index = i;
                break;
            }
        }
        if (crtc_ == 0) {
            continue;
        }
        connector_ = id;
        chosen = modes[static_cast<size_t>(pick)];
        break;
    }
    if (connector_ == 0) {
        error = "HDMI: the screen offers no usable mode";
        close();
        return false;
    }
    width_ = chosen.hdisplay;
    height_ = chosen.vdisplay;

    // Un plano libre de esa CRTC que acepte YUV 4:2:0 (en la vc4 lo aceptan casi todos).
    drm_mode_get_plane_res plane_count{};
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_count) == 0 && plane_count.count_planes > 0) {
        std::vector<uint32_t> planes(plane_count.count_planes);
        drm_mode_get_plane_res plane_list{};
        plane_list.count_planes = plane_count.count_planes;
        plane_list.plane_id_ptr = user_pointer(planes.data());
        if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETPLANERESOURCES, &plane_list) == 0) {
            for (uint32_t id : planes) {
                drm_mode_get_plane plane{};
                plane.plane_id = id;
                if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETPLANE, &plane) != 0 || plane.crtc_id != 0 ||
                    !(plane.possible_crtcs & (1U << crtc_index)) || plane.count_format_types == 0) {
                    continue;
                }
                std::vector<uint32_t> formats(plane.count_format_types);
                drm_mode_get_plane with_formats{};
                with_formats.plane_id = id;
                with_formats.count_format_types = plane.count_format_types;
                with_formats.format_type_ptr = user_pointer(formats.data());
                if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETPLANE, &with_formats) != 0) {
                    continue;
                }
                bool yuv = false;
                for (uint32_t format : formats) {
                    yuv = yuv || format == kFormatYuv420;
                }
                if (yuv) {
                    plane_ = id;
                    break;
                }
            }
        }
    }
    if (plane_ == 0) {
        error = "HDMI: no video plane";
        close();
        return false;
    }

    if (!create_buffer(background_, width_, height_, false)) {
        error = "HDMI: out of video memory";
        close();
        return false;
    }
    std::memset(background_.map, 0, background_.size);

    drm_mode_crtc set{};
    set.crtc_id = crtc_;
    set.fb_id = background_.fb;
    set.set_connectors_ptr = user_pointer(&connector_);
    set.count_connectors = 1;
    set.mode_valid = 1;
    set.mode = chosen;
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_SETCRTC, &set) != 0) {
        // EACCES: otro (un escritorio en el HDMI) es el dueño de la tarjeta.
        error = errno == EACCES || errno == EPERM ? "HDMI is in use (desktop running?)" : "HDMI: cannot set the mode";
        close();
        return false;
    }
    drop_background();

    present_quit_ = false;
    presenter_ = std::thread([this] { present_loop(); });
    return true;
}

void KmsDisplay::drop_background() {
    // El fondo negro sólo hacía falta para poner el modo (SETCRTC pide un búfer). Apagado, la
    // GPU rellena de negro lo que no tapa la foto o el vídeo sin leer nada de memoria: a
    // 1920×1200 y 60 Hz son 550 MB/s menos, y sin eso la GPU no acepta una foto a pantalla
    // completa encima (SETPLANE da ENOSPC: su cuenta de ancho de banda no da para los dos).
    // Para tocar el plano primario hay que pedir los «universal planes».
    drm_set_client_cap cap{};
    cap.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES;
    cap.value = 1;
    if (drm_ioctl(fd_, DRM_IOCTL_SET_CLIENT_CAP, &cap) != 0) {
        return;
    }
    drm_mode_get_plane_res count{};
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETPLANERESOURCES, &count) != 0 || count.count_planes == 0) {
        return;
    }
    std::vector<uint32_t> planes(count.count_planes);
    drm_mode_get_plane_res list{};
    list.count_planes = count.count_planes;
    list.plane_id_ptr = user_pointer(planes.data());
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETPLANERESOURCES, &list) != 0) {
        return;
    }
    for (uint32_t id : planes) {
        drm_mode_get_plane plane{};
        plane.plane_id = id;
        if (drm_ioctl(fd_, DRM_IOCTL_MODE_GETPLANE, &plane) != 0 || plane.fb_id != background_.fb) {
            continue;
        }
        drm_mode_set_plane off{};
        off.plane_id = id;
        if (drm_ioctl(fd_, DRM_IOCTL_MODE_SETPLANE, &off) == 0) {
            destroy_buffer(background_);
        }
        return;
    }
}

bool KmsDisplay::create_buffer(Buffer& buffer, int width, int height, bool yuv) {
    drm_mode_create_dumb create{};
    create.width = static_cast<uint32_t>(width);
    // YUV 4:2:0 en un solo búfer de 8 bits: Y entero y debajo U y V a medio ancho y medio alto.
    create.height = static_cast<uint32_t>(yuv ? height * 3 / 2 : height);
    create.bpp = yuv ? 8 : 32;
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        return false;
    }
    buffer.handle = create.handle;
    buffer.pitch = create.pitch;
    buffer.size = create.size;

    drm_mode_fb_cmd2 framebuffer{};
    framebuffer.width = static_cast<uint32_t>(width);
    framebuffer.height = static_cast<uint32_t>(height);
    framebuffer.pixel_format = yuv ? kFormatYuv420 : kFormatXrgb8888;
    framebuffer.handles[0] = create.handle;
    framebuffer.pitches[0] = create.pitch;
    if (yuv) {
        framebuffer.handles[1] = framebuffer.handles[2] = create.handle;
        framebuffer.pitches[1] = framebuffer.pitches[2] = create.pitch / 2;
        framebuffer.offsets[1] = create.pitch * static_cast<uint32_t>(height);
        framebuffer.offsets[2] = framebuffer.offsets[1] + create.pitch / 2 * static_cast<uint32_t>(height / 2);
    }
    for (int i = 0; i < 3; ++i) {
        buffer.offsets[i] = framebuffer.offsets[i];
    }
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_ADDFB2, &framebuffer) != 0) {
        destroy_buffer(buffer);
        return false;
    }
    buffer.fb = framebuffer.fb_id;

    drm_mode_map_dumb map{};
    map.handle = create.handle;
    if (drm_ioctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        destroy_buffer(buffer);
        return false;
    }
    void* pixels = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, static_cast<off_t>(map.offset));
    if (pixels == MAP_FAILED) {
        destroy_buffer(buffer);
        return false;
    }
    buffer.map = static_cast<uint8_t*>(pixels);
    return true;
}

void KmsDisplay::destroy_buffer(Buffer& buffer) {
    if (buffer.map) {
        munmap(buffer.map, buffer.size);
    }
    if (fd_ >= 0 && buffer.fb) {
        uint32_t id = buffer.fb;
        drm_ioctl(fd_, DRM_IOCTL_MODE_RMFB, &id);
    }
    if (fd_ >= 0 && buffer.handle) {
        drm_mode_destroy_dumb destroy{};
        destroy.handle = buffer.handle;
        drm_ioctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    buffer = Buffer{};
}

bool KmsDisplay::show(const uint8_t* frame, int width, int height, bool wait) {
    if (fd_ < 0 || width < 2 || height < 2 || (width & 1) || (height & 1)) {
        return false;
    }
    if (width != frame_width_ || height != frame_height_) {
        // Otro tamaño (otro vídeo, o una foto): búferes nuevos. El plano deja de mirar los
        // viejos antes de borrarlos.
        blank();
        for (auto& buffer : frames_) {
            destroy_buffer(buffer);
        }
        frame_width_ = frame_height_ = 0;
        for (auto& buffer : frames_) {
            // La memoria de vídeo (CMA) la presta Linux a la caché de disco mientras no se usa
            // y la recupera moviendo páginas; justo tras arrancar, con el disco ocupado, eso
            // puede fallar un momento («Failed to allocate from GEM DMA helper»). Se reintenta.
            bool created = false;
            for (int attempt = 0; attempt < 5 && !created; ++attempt) {
                created = create_buffer(buffer, width, height, true);
                if (!created) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }
            if (!created) {
                for (auto& created : frames_) {
                    destroy_buffer(created);
                }
                return false;
            }
        }
        frame_width_ = width;
        frame_height_ = height;
    }

    // Un búfer que no esté en pantalla ni cambiándose. Con tres siempre hay uno, salvo que el
    // tercero espere turno: entonces ese fotograma se tira y se escribe encima (si llegan más
    // deprisa de lo que refresca la pantalla, se ve el último, sin retrasarse).
    int index = -1;
    {
        std::lock_guard<std::mutex> lock(present_mutex_);
        for (int i = 0; i < static_cast<int>(frames_.size()); ++i) {
            if (i != shown_ && i != in_flight_ && i != pending_) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            index = pending_;
            pending_ = -1;
        }
    }

    Buffer& buffer = frames_[static_cast<size_t>(index)];
    const size_t luma = static_cast<size_t>(width) * height;
    const size_t chroma = luma / 4;
    const int half_width = width / 2;
    const int half_height = height / 2;
    if (buffer.pitch == static_cast<uint32_t>(width)) {
        // Mismo trazado que el de ffmpeg: de una vez.
        std::memcpy(buffer.map, frame, luma + 2 * chroma);
    }
    else {
        for (int y = 0; y < height; ++y) {
            std::memcpy(buffer.map + static_cast<size_t>(y) * buffer.pitch, frame + static_cast<size_t>(y) * width,
                        static_cast<size_t>(width));
        }
        const uint32_t half_pitch = buffer.pitch / 2;
        for (int plane = 0; plane < 2; ++plane) {
            const uint8_t* source = frame + luma + plane * chroma;
            uint8_t* target = buffer.map + buffer.offsets[1 + plane];
            for (int y = 0; y < half_height; ++y) {
                std::memcpy(target + static_cast<size_t>(y) * half_pitch, source + static_cast<size_t>(y) * half_width,
                            static_cast<size_t>(half_width));
            }
        }
    }

    std::unique_lock<std::mutex> lock(present_mutex_);
    pending_ = index;
    const uint64_t ticket = ++queued_;
    present_wake_.notify_all();
    if (!wait) {
        return !present_failed_;
    }
    // Una foto: se espera a que esté en pantalla, para poder decir si ha fallado.
    present_wake_.wait(lock, [&] { return presented_ >= ticket || present_quit_; });
    return !present_failed_;
}

void KmsDisplay::present_loop() {
    std::unique_lock<std::mutex> lock(present_mutex_);
    for (;;) {
        present_wake_.wait(lock, [&] { return present_quit_ || pending_ >= 0; });
        if (present_quit_) {
            return;
        }
        const int index = pending_;
        const uint64_t ticket = queued_;
        pending_ = -1;
        in_flight_ = index;
        const uint32_t fb = frames_[static_cast<size_t>(index)].fb;
        const int width = frame_width_;
        const int height = frame_height_;
        lock.unlock();

        // SETPLANE espera al siguiente refresco de la pantalla (medido: 8,5 ms de media, como
        // mucho uno entero): así no se ve medio fotograma, y por eso va en este hilo y no en
        // el del reproductor, que mientras tanto ya está copiando el siguiente.
        const auto fit = gallery::fit_rect(width, height, width_, height_);
        drm_mode_set_plane set{};
        set.plane_id = plane_;
        set.crtc_id = crtc_;
        set.fb_id = fb;
        set.crtc_x = fit.x;
        set.crtc_y = fit.y;
        set.crtc_w = static_cast<uint32_t>(fit.width);
        set.crtc_h = static_cast<uint32_t>(fit.height);
        // Origen en 16.16.
        set.src_w = static_cast<uint32_t>(width) << 16;
        set.src_h = static_cast<uint32_t>(height) << 16;
        const bool ok = drm_ioctl(fd_, DRM_IOCTL_MODE_SETPLANE, &set) == 0;

        lock.lock();
        in_flight_ = -1;
        if (ok) {
            shown_ = index;
            plane_on_ = true;
        }
        present_failed_ = !ok;
        presented_ = std::max(presented_, ticket);
        present_wake_.notify_all();
    }
}

void KmsDisplay::wait_idle() {
    std::unique_lock<std::mutex> lock(present_mutex_);
    present_wake_.wait(lock, [&] { return present_quit_ || (pending_ < 0 && in_flight_ < 0); });
}

void KmsDisplay::blank() {
    if (fd_ < 0) {
        return;
    }
    wait_idle();
    std::lock_guard<std::mutex> lock(present_mutex_);
    if (!plane_on_) {
        return;
    }
    drm_mode_set_plane set{};
    set.plane_id = plane_;
    drm_ioctl(fd_, DRM_IOCTL_MODE_SETPLANE, &set);
    plane_on_ = false;
    shown_ = -1;
}

void KmsDisplay::close() {
    if (presenter_.joinable()) {
        blank();
        {
            std::lock_guard<std::mutex> lock(present_mutex_);
            present_quit_ = true;
        }
        present_wake_.notify_all();
        presenter_.join();
    }
    if (fd_ >= 0) {
        // Apagar la salida. Al cerrar, la consola del sistema (fbdev) la vuelve a coger, en
        // negro: eso es cosa del kernel, no de la app.
        if (crtc_) {
            drm_mode_crtc off{};
            off.crtc_id = crtc_;
            drm_ioctl(fd_, DRM_IOCTL_MODE_SETCRTC, &off);
        }
        for (auto& buffer : frames_) {
            destroy_buffer(buffer);
        }
        destroy_buffer(background_);
        ::close(fd_);
    }
    fd_ = -1;
    crtc_ = connector_ = plane_ = 0;
    width_ = height_ = 0;
    frame_width_ = frame_height_ = 0;
    plane_on_ = false;
    pending_ = in_flight_ = shown_ = -1;
    queued_ = presented_ = 0;
    present_failed_ = false;
}

#else // sin KMS (no es Linux): nunca hay HDMI

bool KmsDisplay::open(std::string& error) {
    error = "No HDMI here";
    return false;
}
bool KmsDisplay::create_buffer(Buffer&, int, int, bool) {
    return false;
}
void KmsDisplay::destroy_buffer(Buffer&) {}
bool KmsDisplay::show(const uint8_t*, int, int, bool) {
    return false;
}
void KmsDisplay::blank() {}
void KmsDisplay::close() {}
void KmsDisplay::drop_background() {}
void KmsDisplay::present_loop() {}
void KmsDisplay::wait_idle() {}

#endif

} // namespace platform
