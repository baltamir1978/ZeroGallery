/*
 * SPDX-License-Identifier: MIT
 *
 * Pruebas de la lógica de la galería. Sin dependencias: un ejecutable que devuelve 1 si
 * algo falla, para pasarlo con ctest.
 */

#include "gallery.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
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

namespace fs = std::filesystem;

void touch(const fs::path& path) {
    std::ofstream(path) << "x";
}

std::vector<std::string> names(const std::vector<gallery::Entry>& entries) {
    std::vector<std::string> result;
    for (const auto& entry : entries) {
        result.push_back(entry.name);
    }
    return result;
}

/** APP1 mínimo con sólo la etiqueta de orientación. */
std::vector<uint8_t> exif(bool little_endian, uint16_t orientation) {
    std::vector<uint8_t> data = {'E', 'x', 'i', 'f', 0, 0};
    const auto put16 = [&](uint16_t value) {
        if (little_endian) {
            data.push_back(static_cast<uint8_t>(value & 0xFF));
            data.push_back(static_cast<uint8_t>(value >> 8));
        }
        else {
            data.push_back(static_cast<uint8_t>(value >> 8));
            data.push_back(static_cast<uint8_t>(value & 0xFF));
        }
    };
    const auto put32 = [&](uint32_t value) {
        if (little_endian) {
            put16(static_cast<uint16_t>(value & 0xFFFF));
            put16(static_cast<uint16_t>(value >> 16));
        }
        else {
            put16(static_cast<uint16_t>(value >> 16));
            put16(static_cast<uint16_t>(value & 0xFFFF));
        }
    };
    data.push_back(little_endian ? 'I' : 'M');
    data.push_back(little_endian ? 'I' : 'M');
    put16(42);
    put32(8);      // la IFD empieza justo detrás de la cabecera
    put16(2);      // dos etiquetas
    put16(0x010F); // fabricante: una cualquiera antes de la que se busca
    put16(2);
    put32(1);
    put32(0);
    put16(0x0112); // orientación
    put16(3);
    put32(1);
    put16(orientation);
    put16(0);
    put32(0);
    return data;
}

void test_classify() {
    gallery::Kind kind{};
    CHECK(gallery::classify("foto.JPG", kind) && kind == gallery::Kind::Image);
    CHECK(gallery::classify("a.b.heic", kind) && kind == gallery::Kind::Image);
    CHECK(gallery::classify("clip.Mp4", kind) && kind == gallery::Kind::Video);
    CHECK(gallery::classify("viaje.mov", kind) && kind == gallery::Kind::Video);
    CHECK(gallery::classify("anim.GIF", kind) && kind == gallery::Kind::Image);
    CHECK(gallery::classify("dibujo.svg", kind) && kind == gallery::Kind::Image);
    CHECK(gallery::classify("camara.MTS", kind) && kind == gallery::Kind::Video);
    CHECK(gallery::classify("peli.m2ts", kind) && kind == gallery::Kind::Video);
    CHECK(gallery::is_animated_format("anim.GIF") && gallery::is_animated_format("a.apng"));
    CHECK(!gallery::is_animated_format("foto.png") && !gallery::is_animated_format("gif"));
    CHECK(gallery::is_jpeg("a.JPEG") && gallery::is_jpeg("b.jfif") && !gallery::is_jpeg("c.png"));
    CHECK(!gallery::classify("notas.txt", kind));
    CHECK(!gallery::classify("sin_extension", kind));
    CHECK(!gallery::classify(".jpg", kind)); // oculto sin nombre, no una foto
    CHECK(!gallery::classify("raro.", kind));
}

void test_natural_order() {
    CHECK(gallery::natural_less("IMG_2.jpg", "IMG_10.jpg"));
    CHECK(!gallery::natural_less("IMG_10.jpg", "IMG_2.jpg"));
    CHECK(gallery::natural_less("a", "B"));
    CHECK(gallery::natural_less("B", "c"));
    CHECK(gallery::natural_less("foto", "foto2"));
    CHECK(gallery::natural_less("2004 Warner", "2005 Windsor"));
    CHECK(gallery::natural_less("x9", "x0010"));
    CHECK(!gallery::natural_less("igual", "igual"));
    // Números más largos que un entero de 64 bits no deben desbordar nada.
    CHECK(gallery::natural_less("9", "100000000000000000000000000000"));
}

void test_keys() {
    using gallery::Action;
    // Flechas de verdad (con Fn) y las mismas teclas sin Fn.
    CHECK(gallery::action_for_key(17) == Action::Up);
    CHECK(gallery::action_for_key('f') == Action::Up);
    CHECK(gallery::action_for_key(18) == Action::Down);
    CHECK(gallery::action_for_key('x') == Action::Down);
    CHECK(gallery::action_for_key(20) == Action::Left);
    CHECK(gallery::action_for_key('z') == Action::Left);
    CHECK(gallery::action_for_key(19) == Action::Right);
    CHECK(gallery::action_for_key('C') == Action::Right);
    CHECK(gallery::action_for_key(10) == Action::Select);
    CHECK(gallery::action_for_key(' ') == Action::Select);
    CHECK(gallery::action_for_key(27) == Action::Back);
    CHECK(gallery::action_for_key('q') == Action::None);
    CHECK(gallery::action_for_key(',') == Action::PageUp);
    CHECK(gallery::action_for_key('.') == Action::PageDown);
    // En un vídeo las flechas saltan: 10 s a los lados, 30 s arriba y abajo.
    CHECK(gallery::seek_seconds_for(gallery::action_for_key('c')) == 10);
    CHECK(gallery::seek_seconds_for(gallery::action_for_key('z')) == -10);
    CHECK(gallery::seek_seconds_for(gallery::action_for_key('f')) == 30);
    CHECK(gallery::seek_seconds_for(gallery::action_for_key('x')) == -30);
    CHECK(gallery::seek_seconds_for(Action::Select) == 0);
}

void test_fit() {
    auto r = gallery::fit_rect(4000, 3000, 320, 170); // apaisada 4:3: manda el alto
    CHECK(r.height == 170 && r.width == 227 && r.y == 0 && r.x == 46);
    r = gallery::fit_rect(3000, 4000, 320, 170); // vertical
    CHECK(r.height == 170 && r.width == 128 && r.x == 96);
    r = gallery::fit_rect(8000, 1000, 320, 170); // panorámica: manda el ancho
    CHECK(r.width == 320 && r.height == 40 && r.x == 0 && r.y == 65);
    r = gallery::fit_rect(320, 170, 320, 170);
    CHECK(r.x == 0 && r.y == 0 && r.width == 320 && r.height == 170);
    r = gallery::fit_rect(100000, 1, 320, 170); // nunca menos de un píxel
    CHECK(r.width == 320 && r.height == 1);
    r = gallery::fit_rect(0, 10, 320, 170);
    CHECK(r.width == 0 && r.height == 0);
}

void test_exif() {
    for (const bool little_endian : {true, false}) {
        for (uint16_t value = 1; value <= 8; ++value) {
            const auto data = exif(little_endian, value);
            CHECK(gallery::exif_orientation(data.data(), data.size()) == value);
        }
        const auto invalid = exif(little_endian, 9);
        CHECK(gallery::exif_orientation(invalid.data(), invalid.size()) == 1);
    }

    auto data = exif(true, 6);
    CHECK(gallery::exif_orientation(data.data(), 10) == 1);              // cortado
    CHECK(gallery::exif_orientation(data.data(), data.size() - 8) == 1); // sin la etiqueta entera
    CHECK(gallery::exif_orientation(nullptr, 0) == 1);
    data[0] = 'X';
    CHECK(gallery::exif_orientation(data.data(), data.size()) == 1);

    // Una IFD que dice empezar fuera del bloque no debe leerse.
    data = exif(true, 6);
    data[10] = 0xFF;
    data[11] = 0xFF;
    data[12] = 0xFF;
    data[13] = 0x7F;
    CHECK(gallery::exif_orientation(data.data(), data.size()) == 1);
}

void test_orientation_mapping() {
    const int width = 4;
    const int height = 3;
    for (int orientation = 1; orientation <= 8; ++orientation) {
        const bool swap = gallery::orientation_swaps_axes(orientation);
        CHECK(swap == (orientation >= 5));
        const int shown_width = swap ? height : width;
        const int shown_height = swap ? width : height;
        // Cada píxel enderezado sale de un píxel guardado distinto y dentro de la imagen.
        std::vector<int> used(static_cast<size_t>(width * height), 0);
        for (int y = 0; y < shown_height; ++y) {
            for (int x = 0; x < shown_width; ++x) {
                int sx = -1;
                int sy = -1;
                gallery::oriented_to_stored(orientation, width, height, x, y, sx, sy);
                CHECK(sx >= 0 && sx < width && sy >= 0 && sy < height);
                if (sx >= 0 && sx < width && sy >= 0 && sy < height) {
                    ++used[static_cast<size_t>(sy * width + sx)];
                }
            }
        }
        for (const int count : used) {
            CHECK(count == 1);
        }
    }

    int sx = 0;
    int sy = 0;
    // Orientación 6 (móvil en vertical): la esquina de arriba a la izquierda de lo que se
    // ve es la de abajo a la izquierda de lo guardado.
    gallery::oriented_to_stored(6, width, height, 0, 0, sx, sy);
    CHECK(sx == 0 && sy == height - 1);
    gallery::oriented_to_stored(8, width, height, 0, 0, sx, sy);
    CHECK(sx == width - 1 && sy == 0);
    gallery::oriented_to_stored(3, width, height, 0, 0, sx, sy);
    CHECK(sx == width - 1 && sy == height - 1);
}

void test_rotation_from_gravity() {
    // Lecturas reales del BMI270 del aparato (en crudo): de pie, como se usa, marca -X.
    CHECK(gallery::rotation_from_gravity(-3792, 1432, 52, 0) == 0);
    CHECK(gallery::rotation_from_gravity(-4096, 0, 0, 2) == 0);
    // Lado derecho hacia arriba, boca abajo y lado izquierdo hacia arriba.
    CHECK(gallery::rotation_from_gravity(0, 4096, 0, 0) == 1);
    CHECK(gallery::rotation_from_gravity(4096, 0, 0, 0) == 2);
    CHECK(gallery::rotation_from_gravity(0, -4096, 0, 0) == 3);
    // Tumbado en la mesa no se sabe: se queda como estaba.
    CHECK(gallery::rotation_from_gravity(-300, 200, 4096, 3) == 3);
    CHECK(gallery::rotation_from_gravity(0, 0, 0, 1) == 1);
    // En la diagonal (45°) no cambia; hay que pasar de 60°, desde cualquiera de los dos lados.
    CHECK(gallery::rotation_from_gravity(-2896, 2896, 0, 0) == 0);
    CHECK(gallery::rotation_from_gravity(-2896, 2896, 0, 1) == 1);
    CHECK(gallery::rotation_from_gravity(-1401, 3849, 0, 0) == 1); // 70°
    CHECK(gallery::rotation_from_gravity(-3849, 1401, 0, 1) == 0); // 20°
    // De 3 a 0 pasando por el salto de -180°/180° del ángulo.
    CHECK(gallery::rotation_from_gravity(3849, -1401, 0, 3) == 2);
    CHECK(gallery::rotation_from_gravity(3849, 1401, 0, 3) == 2);
}

void test_rotate_to_screen() {
    const int width = 4;
    const int height = 3;
    CHECK(!gallery::rotation_swaps_axes(0) && gallery::rotation_swaps_axes(1));
    CHECK(!gallery::rotation_swaps_axes(2) && gallery::rotation_swaps_axes(3));

    for (int turns = 0; turns < 4; ++turns) {
        const bool swap = gallery::rotation_swaps_axes(turns);
        const int upright_width = swap ? height : width;
        const int upright_height = swap ? width : height;
        std::vector<uint16_t> upright(static_cast<size_t>(width * height));
        for (size_t i = 0; i < upright.size(); ++i) {
            upright[i] = static_cast<uint16_t>(i + 1);
        }
        std::vector<uint16_t> screen(upright.size(), 0);
        gallery::rotate_to_screen(upright.data(), turns, width, height, screen.data());

        const auto at = [&](int x, int y) { return screen[static_cast<size_t>(y * width + x)]; };
        const uint16_t top_left = upright[0];
        const uint16_t top_right = upright[static_cast<size_t>(upright_width - 1)];
        const uint16_t bottom_left = upright[static_cast<size_t>((upright_height - 1) * upright_width)];
        // La esquina de arriba a la izquierda de la imagen va girando por la pantalla en
        // el sentido del reloj, y el borde de arriba con ella.
        switch (turns) {
            case 0:
                CHECK(at(0, 0) == top_left && at(width - 1, 0) == top_right);
                break;
            case 1:
                CHECK(at(width - 1, 0) == top_left && at(width - 1, height - 1) == top_right);
                CHECK(at(0, 0) == bottom_left);
                break;
            case 2:
                CHECK(at(width - 1, height - 1) == top_left && at(0, height - 1) == top_right);
                break;
            default:
                CHECK(at(0, height - 1) == top_left && at(0, 0) == top_right);
                CHECK(at(width - 1, height - 1) == bottom_left);
                break;
        }
        // No se pierde ni se repite ningún píxel.
        std::vector<int> seen(upright.size() + 1, 0);
        for (const uint16_t value : screen) {
            CHECK(value >= 1 && value <= upright.size());
            if (value >= 1 && value <= upright.size()) {
                ++seen[value];
            }
        }
        for (size_t value = 1; value <= upright.size(); ++value) {
            CHECK(seen[value] == 1);
        }
    }
}

void test_session() {
    const fs::path root = fs::temp_directory_path() / ("zerogallery_test_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "Viaje" / "Dentro");
    fs::create_directories(root / "album");
    fs::create_directories(root / ".oculta");
    touch(root / "IMG_10.jpg");
    touch(root / "IMG_2.jpg");
    touch(root / "clip.mp4");
    touch(root / "notas.txt");
    touch(root / ".escondida.jpg");
    touch(root / "Viaje" / "a.png");

    {
        const auto listed = names(gallery::list_directory(root));
        const std::vector<std::string> expected = {"album", "Viaje", "clip.mp4", "IMG_2.jpg", "IMG_10.jpg"};
        CHECK(listed == expected);
    }
    CHECK(gallery::list_directory(root / "no_existe").empty());

    gallery::Session session(root);
    CHECK(session.directory() == root);
    CHECK(session.title() == root.filename().string());
    CHECK(session.selected() == 0);

    session.move(-5);
    CHECK(session.selected() == 0);
    session.move(100);
    CHECK(session.selected() == 4);
    session.select_first();

    // Lo señalado es una carpeta: no hay foto que enseñar, pero sí siguiente.
    size_t index = 9;
    size_t count = 9;
    session.media_position(index, count);
    CHECK(index == 0 && count == 3);
    CHECK(session.step_media(1));
    CHECK(session.selected_entry()->name == "clip.mp4");
    session.media_position(index, count);
    CHECK(index == 1 && count == 3);
    CHECK(!session.step_media(-1)); // antes sólo hay carpetas
    CHECK(session.step_media(1) && session.step_media(1));
    CHECK(session.selected_entry()->name == "IMG_10.jpg");
    CHECK(!session.step_media(1));
    CHECK(session.selected_path() == root / "IMG_10.jpg");
    CHECK(!session.enter_selected_folder()); // una foto no es una carpeta

    session.select_first();
    session.move(1);
    CHECK(session.selected_entry()->name == "Viaje");
    CHECK(session.enter_selected_folder());
    CHECK(session.directory() == root / "Viaje");
    CHECK(session.title() == "Viaje");
    CHECK(names(session.entries()) == (std::vector<std::string>{"Dentro", "a.png"}));

    // Al subir se queda señalada la carpeta de la que se viene.
    CHECK(session.go_up());
    CHECK(session.directory() == root);
    CHECK(session.selected_entry()->name == "Viaje");

    // Carpeta vacía: nada señalado y nada se rompe.
    session.select_first();
    CHECK(session.enter_selected_folder());
    CHECK(session.empty());
    CHECK(session.selected_entry() == nullptr);
    CHECK(session.selected_path().empty());
    session.move(1);
    CHECK(!session.step_media(1));
    CHECK(!session.enter_selected_folder());

    // Hasta la raíz y ni un paso más.
    int guard = 0;
    while (session.go_up() && ++guard < 100) {
    }
    CHECK(session.directory() == "/");
    CHECK(session.title() == "/");
    CHECK(!session.go_up());

    // Si la carpeta inicial no existe se empieza en la raíz en vez de fallar.
    gallery::Session lost(root / "no_existe");
    CHECK(lost.directory() == "/");

    fs::remove_all(root);
}

} // namespace

int main() {
    test_classify();
    test_natural_order();
    test_keys();
    test_fit();
    test_exif();
    test_orientation_mapping();
    test_rotation_from_gravity();
    test_rotate_to_screen();
    test_session();

    if (failures) {
        std::printf("%d comprobaciones fallidas\n", failures);
        return 1;
    }
    std::printf("gallery_test: todo bien\n");
    return 0;
}
