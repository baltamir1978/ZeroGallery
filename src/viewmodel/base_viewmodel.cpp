/*
 * SPDX-License-Identifier: MIT
 */

#include "base_viewmodel.h"

#include <utility>

namespace viewmodel {
namespace {

// gallery.cpp repite estos códigos para no depender de LVGL; si LVGL los cambiara,
// que falle aquí al compilar y no en el Cardputer.
static_assert(LV_KEY_UP == 17 && LV_KEY_DOWN == 18 && LV_KEY_RIGHT == 19 && LV_KEY_LEFT == 20);
static_assert(LV_KEY_ESC == 27 && LV_KEY_BACKSPACE == 8 && LV_KEY_ENTER == 10);
static_assert(LV_KEY_NEXT == 9 && LV_KEY_PREV == 11 && LV_KEY_HOME == 2 && LV_KEY_END == 3);

// Filas que caben en la lista: es lo que avanza una página.
constexpr long kPageRows = 6;

} // namespace

BaseViewModel::BaseViewModel(std::filesystem::path start_directory)
    : session_(std::move(start_directory)),
      start_directory_(session_.directory()),
      title_subject_(session_.title().c_str()),
      dark_mode_subject_(true),
      current_page_subject_(static_cast<int>(model::AppPage::Browser)),
      quit_requested_subject_(false),
      listing_subject_(0),
      media_subject_(0),
      playback_subject_(0),
      seek_subject_(0) {}

BaseViewModel::~BaseViewModel() = default;

lv_subject_t* BaseViewModel::title_subject() {
    return title_subject_.native();
}

lv_subject_t* BaseViewModel::dark_mode_subject() {
    return dark_mode_subject_.native();
}

lv_subject_t* BaseViewModel::current_page_subject() {
    return current_page_subject_.native();
}

lv_subject_t* BaseViewModel::quit_requested_subject() {
    return quit_requested_subject_.native();
}

lv_subject_t* BaseViewModel::listing_subject() {
    return listing_subject_.native();
}

lv_subject_t* BaseViewModel::media_subject() {
    return media_subject_.native();
}

lv_subject_t* BaseViewModel::playback_subject() {
    return playback_subject_.native();
}

lv_subject_t* BaseViewModel::seek_subject() {
    return seek_subject_.native();
}

int BaseViewModel::seek_seconds() const {
    return seek_seconds_;
}

const gallery::Session& BaseViewModel::session() const {
    return session_;
}

bool BaseViewModel::is_dark_mode() const {
    return dark_mode_subject_.value();
}

void BaseViewModel::set_dark_mode(bool enabled) {
    dark_mode_subject_.set(enabled);
}

void BaseViewModel::toggle_dark_mode() {
    dark_mode_subject_.toggle();
}

model::AppPage BaseViewModel::current_page() const {
    return page_;
}

bool BaseViewModel::handle_key(uint32_t key) {
    const auto action = gallery::action_for_key(key);
    if (action == gallery::Action::None || action == gallery::Action::Back) {
        return false;
    }
    if (action == gallery::Action::ToggleTheme) {
        toggle_dark_mode();
        return true;
    }
    return page_ == model::AppPage::Viewer ? handle_viewer(action) : handle_browser(action);
}

bool BaseViewModel::handle_browser(gallery::Action action) {
    switch (action) {
        case gallery::Action::Up:
            session_.move(-1);
            break;
        case gallery::Action::Down:
            session_.move(1);
            break;
        case gallery::Action::PageUp:
            session_.move(-kPageRows);
            break;
        case gallery::Action::PageDown:
            session_.move(kPageRows);
            break;
        case gallery::Action::First:
            session_.select_first();
            break;
        case gallery::Action::Last:
            session_.select_last();
            break;
        case gallery::Action::Left:
            if (!session_.go_up()) {
                return true;
            }
            break;
        case gallery::Action::Right:
        case gallery::Action::Select: {
            const auto* entry = session_.selected_entry();
            if (!entry) {
                return true;
            }
            if (entry->kind != gallery::Kind::Folder) {
                show_page(model::AppPage::Viewer);
                media_subject_.set(media_subject_.value() + 1);
                return true;
            }
            session_.enter_selected_folder();
            break;
        }
        default:
            return false;
    }
    publish_listing();
    return true;
}

bool BaseViewModel::handle_viewer(gallery::Action action) {
    // En un vídeo las flechas saltan dentro de él; de fichero se cambia con coma y punto.
    // Un GIF no cuenta: se repite solo y no tiene por dónde saltar.
    const auto* entry = session_.selected_entry();
    const int seek = gallery::seek_seconds_for(action);
    if (seek != 0 && entry && entry->kind == gallery::Kind::Video) {
        seek_seconds_ = seek;
        seek_subject_.set(seek_subject_.value() + 1);
        return true;
    }

    switch (action) {
        case gallery::Action::Left:
        case gallery::Action::Up:
        case gallery::Action::PageUp:
            if (session_.step_media(-1)) {
                media_subject_.set(media_subject_.value() + 1);
            }
            return true;
        case gallery::Action::Right:
        case gallery::Action::Down:
        case gallery::Action::PageDown:
            if (session_.step_media(1)) {
                media_subject_.set(media_subject_.value() + 1);
            }
            return true;
        case gallery::Action::Select:
            playback_subject_.set(playback_subject_.value() + 1);
            return true;
        default:
            return false;
    }
}

bool BaseViewModel::back() {
    if (page_ == model::AppPage::Viewer) {
        // La lista se vuelve a crear al cambiar de pantalla y lee la selección de la
        // sesión, que ya apunta a lo último que se vio.
        show_page(model::AppPage::Browser);
        return true;
    }
    if (session_.directory() == start_directory_ || !session_.go_up()) {
        return false;
    }
    publish_listing();
    return true;
}

void BaseViewModel::request_quit() {
    quit_requested_subject_.set(true);
}

void BaseViewModel::show_page(model::AppPage page) {
    page_ = page;
    current_page_subject_.set(static_cast<int>(page));
}

void BaseViewModel::publish_listing() {
    title_subject_.set(session_.title().c_str());
    listing_subject_.set(listing_subject_.value() + 1);
}

} // namespace viewmodel
