/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "app_page.h"
#include "gallery.h"
#include "subjects.h"

#include "lvgl.h"

#include <filesystem>

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
    int seek_seconds_{0};
};

} // namespace viewmodel
