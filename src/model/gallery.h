/*
 * SPDX-License-Identifier: MIT
 *
 * Lógica de la galería sin LVGL ni hardware: qué se enseña de una carpeta, en qué orden,
 * por dónde se navega y cómo encaja una imagen en la pantalla. Va aparte para poder probarla
 * en el PC (tests/gallery_test.cpp).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gallery {

enum class Kind {
    Folder,
    Image,
    Video,
};

struct Entry {
    std::string name;
    Kind kind;
};

/** Lo que el usuario quiere hacer, ya separado de la tecla concreta. */
enum class Action {
    None,
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    First,
    Last,
    Select,
    Back,
    ToggleTheme,
};

/**
 * Traduce una tecla (código de LVGL o carácter) a una acción.
 *
 * En el Cardputer Zero las flechas están serigrafiadas en F, Z, X y C y sólo salen como
 * flechas con Fn: aquí esas letras valen igual sin Fn, que es como se usa con una mano.
 */
Action action_for_key(uint32_t key);

/**
 * Segundos que salta un vídeo con cada flecha: derecha e izquierda, 10 adelante y atrás;
 * arriba y abajo, 30 adelante y atrás. 0 si la acción no es una flecha.
 */
int seek_seconds_for(Action action);

/** Tipo según la extensión; `false` si no es ni foto ni vídeo. */
bool classify(std::string_view file_name, Kind& kind);

/** Imagen que puede estar animada (GIF, APNG): el visor la reproduce en bucle. */
bool is_animated_format(std::string_view file_name);

/** JPEG por la extensión: los únicos que se abren sin ffmpeg. */
bool is_jpeg(std::string_view file_name);

/** Orden «natural» sin distinguir mayúsculas: IMG_2 va antes que IMG_10. */
bool natural_less(std::string_view a, std::string_view b);

/**
 * Carpetas primero y luego fotos y vídeos, sin ocultos y en orden natural.
 * Una carpeta que no se puede leer se devuelve vacía: no es motivo para cerrar la app.
 */
std::vector<Entry> list_directory(const std::filesystem::path& directory);

struct Rect {
    int x;
    int y;
    int width;
    int height;
};

/** Mayor rectángulo centrado con la proporción de la imagen que cabe en la pantalla. */
Rect fit_rect(int image_width, int image_height, int screen_width, int screen_height);

/**
 * Orientación EXIF (1..8) a partir del contenido del segmento APP1 de un JPEG
 * (lo que va tras la longitud, empezando por «Exif\0\0»). 1 si no hay o no se entiende.
 */
int exif_orientation(const uint8_t* data, size_t size);

/** Con las orientaciones 5 a 8 la imagen se ve girada 90°: ancho y alto se intercambian. */
bool orientation_swaps_axes(int orientation);

/**
 * Píxel del JPEG tal como está guardado que corresponde al píxel (x, y) de la imagen ya
 * enderezada. `width` y `height` son los de la imagen guardada.
 */
void oriented_to_stored(int orientation, int width, int height, int x, int y, int& stored_x, int& stored_y);

/**
 * Cómo hay que pintar para que la imagen se vea derecha con el aparato girado: cuartos de
 * vuelta (0 a 3) en el sentido del reloj. 0 es el aparato como se usa con el teclado, 1 con
 * su lado derecho hacia arriba, 2 boca abajo y 3 con el lado izquierdo hacia arriba.
 *
 * `sensor_*` es la lectura del acelerómetro (BMI270) en sus propios ejes y en cualquier
 * unidad: sólo cuentan las proporciones. `current` es el giro que hay ahora, y se conserva
 * mientras la lectura no diga otra cosa con claridad: con el aparato tumbado (la gravedad
 * no señala ningún borde) o a medio camino entre dos posiciones, para que no baile.
 */
int rotation_from_gravity(double sensor_x, double sensor_y, double sensor_z, int current);

/** Con 1 y 3 cuartos de vuelta la imagen se pinta en vertical: 170×320 en vez de 320×170. */
bool rotation_swaps_axes(int turns);

/**
 * Gira una imagen ya compuesta para como se va a ver (`screen_width`×`screen_height`, o al
 * revés si el giro intercambia los ejes) y la deja en `screen`, que es la pantalla tal como
 * está montada. Los dos son RGB565 fila a fila.
 */
void rotate_to_screen(const uint16_t* upright, int turns, int screen_width, int screen_height, uint16_t* screen);

/**
 * Por dónde va el usuario: carpeta actual, su contenido y el elemento señalado.
 * El visor no tiene estado propio: pasar de foto es mover la selección al siguiente
 * elemento que no sea carpeta, así al volver a la lista se está donde se dejó.
 */
class Session {
public:
    explicit Session(std::filesystem::path start);

    const std::filesystem::path& directory() const;
    const std::vector<Entry>& entries() const;
    size_t selected() const;
    bool empty() const;

    /** Nombre para el título: el de la carpeta, o «/» en la raíz. */
    std::string title() const;

    void move(long delta);
    void select_first();
    void select_last();

    const Entry* selected_entry() const;
    std::filesystem::path selected_path() const;

    /** Entra en la carpeta señalada. `false` si lo señalado no es una carpeta. */
    bool enter_selected_folder();

    /** Sube a la carpeta madre y deja señalada la carpeta de la que se viene. */
    bool go_up();

    /** Mueve la selección a la foto o vídeo anterior o siguiente; `false` si no hay más. */
    bool step_media(int direction);

    /** Posición (desde 1) de lo señalado entre las fotos y vídeos, y cuántos hay. */
    void media_position(size_t& index, size_t& count) const;

private:
    void load(const std::filesystem::path& directory, const std::string& select_name);

    std::filesystem::path directory_;
    std::vector<Entry> entries_;
    size_t selected_{0};
};

} // namespace gallery
