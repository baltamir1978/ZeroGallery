/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "app_page.h"
#include "audio_route.h"
#include "dlna_caster.h"
#include "hdmi_player.h"
#include "gallery.h"
#include "subjects.h"

#include "lvgl.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace viewmodel {

/**
 * Estado de la app tal como lo ven las pantallas. La navegación en sí es de
 * gallery::Session; aquí se decide qué hace cada tecla según la pantalla y se avisa a las
 * vistas con «subjects» de LVGL. Los tres contadores (listing, media, playback) no llevan
 * dato: sólo cambian de valor para decir «vuelve a mirar la sesión».
 */
class BaseViewModel {
public:
    explicit BaseViewModel(std::filesystem::path start_directory);
    ~BaseViewModel();

    BaseViewModel(const BaseViewModel&) = delete;
    BaseViewModel& operator=(const BaseViewModel&) = delete;

    lv_subject_t* title_subject();
    lv_subject_t* dark_mode_subject();
    lv_subject_t* current_page_subject();
    lv_subject_t* quit_requested_subject();
    /** Cambia cuando cambia la carpeta o el elemento señalado en la lista. */
    lv_subject_t* listing_subject();
    /** Cambia cuando el visor tiene que enseñar otra foto o vídeo. */
    lv_subject_t* media_subject();
    /** Cambia cuando se pide pausar o reanudar el vídeo. */
    lv_subject_t* playback_subject();
    /** Cambia cuando se pide saltar en el vídeo; cuánto, lo dice `seek_seconds()`. */
    lv_subject_t* seek_subject();
    /** Segundos del último salto pedido: positivo adelante, negativo atrás. */
    int seek_seconds() const;
    /**
     * Cambia cuando se abre o se cierra la lista de teles, se mueve su selección o se
     * empieza o se deja de mandar a la tele.
     */
    lv_subject_t* cast_subject();

    /** Lo que manda a la tele; null hasta la primera vez que se pulsa V. */
    platform::DlnaCaster* caster() const;
    /** Si se está mandando a una tele o al HDMI. */
    bool casting() const;
    bool picker_open() const;
    /** Fila señalada en la lista (índice en `picker_names()`). */
    size_t picker_selection() const;
    /** Lo que sale en la lista: «HDMI» primero si hay algo enchufado, y luego las teles. */
    std::vector<std::string> picker_names();
    /** Cambia cada vez que cambia la lista. */
    uint32_t picker_version();

    /** Cambia cuando se cambia por dónde suena (tecla A, o al mandar al HDMI y dejarlo). */
    lv_subject_t* audio_subject();
    /** Por dónde suena ahora, para el rótulo: «Speaker», «Headphones», «HDMI» o el nombre del aparato USB o Bluetooth. */
    std::string audio_output_name() const;

    /** Cambia con cada pulsación de las teclas de volumen o de silencio (en el visor). */
    lv_subject_t* volume_subject();
    /**
     * Cómo ha quedado tras la última: `percent` y `muted` para la barra; si `percent` es -1,
     * no se ha podido cambiar y `notice` dice por qué (por el HDMI o en la tele, el mando).
     */
    struct VolumeState {
        int percent = -1;
        bool muted = false;
        std::string notice;
    };
    const VolumeState& volume_state() const;

    // Cómo va lo que se está mandando, venga de una tele o del HDMI (para el visor).
    std::string output_name() const;
    platform::CastState output_state() const;
    std::string output_error() const;
    double output_position() const;
    double output_duration() const;
    /** Aviso pasajero; vacío si no hay. */
    std::string take_output_notice();

    const gallery::Session& session() const;

    bool is_dark_mode() const;
    void set_dark_mode(bool enabled);
    void toggle_dark_mode();

    model::AppPage current_page() const;

    /** Tecla que no es ESC. Devuelve si se ha usado. */
    bool handle_key(uint32_t key);

    /**
     * Un paso atrás (ESC corto): del visor a la lista y de una carpeta a su madre.
     * `false` cuando ya no hay adónde volver: se está en la carpeta inicial o en «/».
     */
    bool back();

    void request_quit();

private:
    bool handle_browser(gallery::Action action);
    bool handle_viewer(gallery::Action action);
    bool handle_picker(gallery::Action action);
    void open_picker();
    void close_picker();
    void stop_casting();
    /** Pone la salida de sonido y avisa al visor si cambia. */
    void set_audio(audio::Output output);
    /** Si hay que poner «HDMI» en la lista (se mira /sys como mucho una vez por segundo). */
    bool hdmi_listed();
    /** Manda a la tele lo señalado, si se está mandando y es una foto o un vídeo. */
    void cast_selected();
    /** El visor tiene que enseñar otra cosa (y la tele también, si se está mandando). */
    void publish_media();
    void show_page(model::AppPage page);
    void publish_listing();

    gallery::Session session_;
    std::filesystem::path start_directory_;
    model::AppPage page_{model::AppPage::Browser};
    reactive::StringSubject<128> title_subject_;
    reactive::BoolSubject dark_mode_subject_;
    reactive::IntSubject current_page_subject_;
    reactive::BoolSubject quit_requested_subject_;
    reactive::IntSubject listing_subject_;
    reactive::IntSubject media_subject_;
    reactive::IntSubject playback_subject_;
    reactive::IntSubject seek_subject_;
    reactive::IntSubject cast_subject_;
    reactive::IntSubject audio_subject_;
    reactive::IntSubject volume_subject_;
    VolumeState volume_;
    platform::AudioRoute audio_;
    int seek_seconds_{0};
    std::unique_ptr<platform::DlnaCaster> caster_;
    dlna::Renderer tv_;
    // Salida HDMI: existe mientras se está mandando a ella.
    std::unique_ptr<platform::HdmiPlayer> hdmi_;
    bool hdmi_chosen_{false}; // lo último elegido fue el HDMI (sale señalado al abrir la lista)
    bool hdmi_listed_{false};
    uint32_t hdmi_checked_{0};
    bool hdmi_checked_once_{false};
    bool picker_open_{false};
    size_t picker_selection_{0};
};

} // namespace viewmodel
