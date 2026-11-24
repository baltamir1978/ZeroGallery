/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "app_page.h"
#include "dlna_caster.h"
#include "gallery.h"
#include "subjects.h"

#include "lvgl.h"

#include <filesystem>
#include <memory>
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
    bool casting() const;
    bool picker_open() const;
    /** Tele señalada en la lista (índice en `caster()->renderers()`). */
    size_t picker_selection() const;

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
    int seek_seconds_{0};
    std::unique_ptr<platform::DlnaCaster> caster_;
    dlna::Renderer tv_;
    bool picker_open_{false};
    size_t picker_selection_{0};
};

} // namespace viewmodel
