/*
 * SPDX-License-Identifier: MIT
 *
 * Pruebas de lo que se decide para la salida HDMI sin tocar el hardware.
 */

#include "hdmi.h"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::printf("FALLA %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

bool near(double a, double b) {
    return std::fabs(a - b) < 0.001;
}

void test_pick_mode() {
    using hdmi::Mode;
    // Tele 1080p corriente: su preferido.
    CHECK(hdmi::pick_mode({{1280, 720, 60}, {1920, 1080, 60, true}, {1920, 1080, 50}}) == 1);
    // Tele 4K: el preferido no vale; el mayor hasta 1080p, a 60 mejor que a 30 o 24.
    CHECK(hdmi::pick_mode({{3840, 2160, 30, true}, {1920, 1080, 24}, {1920, 1080, 60}, {1920, 1080, 30}, {1280, 720, 60}}) == 2);
    // 1080i no.
    CHECK(hdmi::pick_mode({{3840, 2160, 60, true}, {1920, 1080, 60, false, true}, {1280, 720, 60}}) == 2);
    // Sin EDID (conector forzado): los de serie, ninguno preferido; el mayor.
    CHECK(hdmi::pick_mode({{1024, 768, 60}, {800, 600, 60}, {640, 480, 60}}) == 0);
    CHECK(hdmi::pick_mode({}) == -1);
    CHECK(hdmi::pick_mode({{3840, 2160, 60, true}}) == -1);
}

void test_video_size() {
    // Cabe: tal cual, que amplíe la GPU.
    auto size = hdmi::video_size(1280, 720, 1920, 1080);
    CHECK(size.width == 1280 && size.height == 720);
    // Igual que la pantalla.
    size = hdmi::video_size(1920, 1080, 1920, 1080);
    CHECK(size.width == 1920 && size.height == 1080);
    // 4K en 1080p.
    size = hdmi::video_size(3840, 2160, 1920, 1080);
    CHECK(size.width == 1920 && size.height == 1080);
    // De pie (1080×1920 ya girado) en una 1080p: por el alto.
    size = hdmi::video_size(1080, 1920, 1920, 1080);
    CHECK(size.height == 1080 && size.width == 608);
    // 1080p en una pantalla de 1024×768: por el ancho, y par.
    size = hdmi::video_size(1920, 1080, 1024, 768);
    CHECK(size.width == 1024 && size.height == 576);
    // Medidas impares de vídeos raros: pares.
    size = hdmi::video_size(853, 481, 1920, 1080);
    CHECK(size.width == 852 && size.height == 480);
    // Sin saber el tamaño: la pantalla entera (ffmpeg encaja con bandas).
    size = hdmi::video_size(0, 0, 1920, 1080);
    CHECK(size.width == 1920 && size.height == 1080);
}

void test_parse_probe() {
    // Salida real de ffprobe en el Cardputer con un vídeo de 1080p.
    auto info = hdmi::parse_probe("width=1920\nheight=1080\nr_frame_rate=25/1\navg_frame_rate=25/1\n"
                                  "duration=182.360000\n");
    CHECK(info.width == 1920 && info.height == 1080);
    CHECK(near(info.fps, 25.0));
    CHECK(near(info.duration, 182.36));
    CHECK(info.rotation == 0);

    // Móvil de pie: girado -90, de cadencia variable.
    info = hdmi::parse_probe("width=1920\r\nheight=1080\r\nr_frame_rate=120/1\r\navg_frame_rate=30000/1001\r\n"
                             "duration=12.5\r\nrotation=-90\r\n");
    CHECK(info.width == 1080 && info.height == 1920);
    CHECK(near(info.fps, 29.97));
    CHECK(info.rotation == -90);

    // 180°: no cambia el ancho.
    info = hdmi::parse_probe("width=640\nheight=360\nrotation=180\navg_frame_rate=0/0\nr_frame_rate=30/1\n");
    CHECK(info.width == 640 && info.height == 360);
    CHECK(near(info.fps, 30.0));

    // Lo que no se sabe se queda en 0.
    info = hdmi::parse_probe("duration=N/A\n");
    CHECK(info.width == 0 && info.fps == 0.0 && info.duration == 0.0);

    CHECK(near(hdmi::parse_rate("30000/1001"), 29.97));
    CHECK(hdmi::parse_rate("0/0") == 0.0);
    CHECK(near(hdmi::parse_rate("24"), 24.0));
}

void test_status() {
    CHECK(hdmi::status_connected("connected\n"));
    CHECK(!hdmi::status_connected("disconnected\n"));
    CHECK(!hdmi::status_connected("unknown"));
    CHECK(!hdmi::status_connected(""));
}

} // namespace

int main() {
    test_pick_mode();
    test_video_size();
    test_parse_probe();
    test_status();
    if (failures) {
        std::printf("%d fallos\n", failures);
        return 1;
    }
    std::printf("hdmi_test: todo bien\n");
    return 0;
}
