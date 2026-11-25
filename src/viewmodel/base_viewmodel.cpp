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
// Cada cuánto se mira si han enchufado o desenchufado el HDMI con la lista abierta.
constexpr uint32_t kHdmiCheckMs = 1000;

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
      seek_subject_(0),
      cast_subject_(0) {}

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

lv_subject_t* BaseViewModel::cast_subject() {
    return cast_subject_.native();
}

platform::DlnaCaster* BaseViewModel::caster() const {
    return caster_.get();
}

bool BaseViewModel::casting() const {
    return hdmi_ || (caster_ && caster_->active());
}

bool BaseViewModel::picker_open() const {
    return picker_open_;
}

size_t BaseViewModel::picker_selection() const {
    return picker_selection_;
}

bool BaseViewModel::hdmi_listed() {
    if (!hdmi_checked_once_ || lv_tick_elaps(hdmi_checked_) >= kHdmiCheckMs) {
        hdmi_checked_once_ = true;
        hdmi_checked_ = lv_tick_get();
        hdmi_listed_ = platform::HdmiPlayer::available();
    }
    return hdmi_listed_;
}

std::vector<std::string> BaseViewModel::picker_names() {
    std::vector<std::string> names;
    if (hdmi_listed()) {
        names.emplace_back("HDMI");
    }
    if (caster_) {
        for (const auto& renderer : caster_->renderers()) {
            names.push_back(renderer.name);
        }
    }
    return names;
}

uint32_t BaseViewModel::picker_version() {
    const uint32_t tvs = caster_ ? caster_->renderers_version() : 0;
    return tvs * 2 + (hdmi_listed() ? 1 : 0);
}

namespace {

platform::CastState cast_state_of(platform::MediaState state) {
    switch (state) {
        case platform::MediaState::Still:
            return platform::CastState::Showing;
        case platform::MediaState::Playing:
            return platform::CastState::Playing;
        case platform::MediaState::Paused:
            return platform::CastState::Paused;
        case platform::MediaState::Ended:
            return platform::CastState::Stopped;
        case platform::MediaState::Error:
            return platform::CastState::Error;
        default:
            return platform::CastState::Sending;
    }
}

} // namespace

std::string BaseViewModel::output_name() const {
    if (hdmi_) {
        return hdmi_->name();
    }
    return caster_ ? caster_->tv_name() : std::string{};
}

platform::CastState BaseViewModel::output_state() const {
    if (hdmi_) {
        return cast_state_of(hdmi_->state());
    }
    return caster_ ? caster_->state() : platform::CastState::Off;
}

std::string BaseViewModel::output_error() const {
    if (hdmi_) {
        return hdmi_->error();
    }
    return caster_ ? caster_->error() : std::string{};
}

double BaseViewModel::output_position() const {
    if (hdmi_) {
        return hdmi_->position();
    }
    return caster_ ? caster_->position() : 0.0;
}

double BaseViewModel::output_duration() const {
    if (hdmi_) {
        return hdmi_->duration();
    }
    return caster_ ? caster_->duration() : 0.0;
}

std::string BaseViewModel::take_output_notice() {
    if (hdmi_) {
        return {};
    }
    return caster_ ? caster_->take_notice() : std::string{};
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
    if (picker_open_) {
        return handle_picker(action);
    }
    if (action == gallery::Action::Cast) {
        // En la lista de carpetas la V sólo sirve para dejar de mandar: a qué tele se
        // elige con una foto delante, que es cuando se sabe qué se va a ver.
        if (casting()) {
            stop_casting();
            return true;
        }
        if (page_ != model::AppPage::Viewer) {
            return false;
        }
        open_picker();
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
                publish_media();
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
    const bool video = entry && entry->kind == gallery::Kind::Video;
    const int seek = gallery::seek_seconds_for(action);
    // Con el vídeo en la tele (o en el HDMI), saltar y pausar es cosa de allí.
    if (video && casting()) {
        if (seek != 0) {
            if (hdmi_) {
                hdmi_->seek(seek);
            }
            else {
                caster_->seek(seek);
            }
            return true;
        }
        if (action == gallery::Action::Select) {
            if (hdmi_) {
                hdmi_->toggle_pause();
            }
            else {
                caster_->toggle_pause();
            }
            return true;
        }
    }
    if (seek != 0 && video) {
        seek_seconds_ = seek;
        seek_subject_.set(seek_subject_.value() + 1);
        return true;
    }

    switch (action) {
        case gallery::Action::Left:
        case gallery::Action::Up:
        case gallery::Action::PageUp:
            if (session_.step_media(-1)) {
                publish_media();
            }
            return true;
        case gallery::Action::Right:
        case gallery::Action::Down:
        case gallery::Action::PageDown:
            if (session_.step_media(1)) {
                publish_media();
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
    if (picker_open_) {
        close_picker();
        return true;
    }
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

bool BaseViewModel::handle_picker(gallery::Action action) {
    const size_t count = picker_names().size();
    switch (action) {
        case gallery::Action::Up:
            if (picker_selection_ > 0) {
                --picker_selection_;
                cast_subject_.set(cast_subject_.value() + 1);
            }
            return true;
        case gallery::Action::Down:
            if (picker_selection_ + 1 < count) {
                ++picker_selection_;
                cast_subject_.set(cast_subject_.value() + 1);
            }
            return true;
        case gallery::Action::Select:
        case gallery::Action::Right: {
            if (picker_selection_ >= count) {
                return true;
            }
            const bool hdmi = hdmi_listed();
            if (hdmi && picker_selection_ == 0) {
                // Al HDMI: lo que se mandara a una tele se deja.
                if (caster_ && caster_->active()) {
                    caster_->stop();
                }
                if (!hdmi_) {
                    hdmi_ = std::make_unique<platform::HdmiPlayer>();
                }
                hdmi_chosen_ = true;
            }
            else {
                const auto renderers = caster_ ? caster_->renderers() : std::vector<dlna::Renderer>{};
                const size_t index = picker_selection_ - (hdmi ? 1 : 0);
                if (index >= renderers.size()) {
                    return true;
                }
                tv_ = renderers[index];
                hdmi_.reset(); // apaga la salida HDMI
                hdmi_chosen_ = false;
            }
            close_picker();
            cast_selected();
            // El visor vuelve a mirar: un vídeo ya no se reproduce aquí sino en la tele.
            media_subject_.set(media_subject_.value() + 1);
            return true;
        }
        case gallery::Action::Cast:
            close_picker();
            return true;
        default:
            // Con la lista abierta el resto de teclas no hace nada: que no cambie la foto
            // por detrás sin que se vea.
            return true;
    }
}

void BaseViewModel::open_picker() {
    if (!caster_) {
        caster_ = std::make_unique<platform::DlnaCaster>();
    }
    caster_->start_discovery();
    // Lo último usado sale ya señalado, si sigue en la lista.
    hdmi_checked_once_ = false;
    const size_t first_tv = hdmi_listed() ? 1 : 0;
    picker_selection_ = 0;
    if (!hdmi_chosen_ || first_tv == 0) {
        const auto renderers = caster_->renderers();
        for (size_t i = 0; i < renderers.size(); ++i) {
            if (renderers[i].udn == tv_.udn) {
                picker_selection_ = first_tv + i;
            }
        }
    }
    picker_open_ = true;
    cast_subject_.set(cast_subject_.value() + 1);
}

void BaseViewModel::close_picker() {
    picker_open_ = false;
    if (caster_) {
        caster_->stop_discovery();
    }
    cast_subject_.set(cast_subject_.value() + 1);
}

void BaseViewModel::stop_casting() {
    if (caster_) {
        caster_->stop();
    }
    hdmi_.reset();
    cast_subject_.set(cast_subject_.value() + 1);
    // Un vídeo que estaba en la tele vuelve a verse aquí.
    if (page_ == model::AppPage::Viewer) {
        media_subject_.set(media_subject_.value() + 1);
    }
}

void BaseViewModel::cast_selected() {
    const auto* entry = session_.selected_entry();
    if (!entry || entry->kind == gallery::Kind::Folder) {
        return;
    }
    if (hdmi_) {
        auto kind = platform::MediaKind::Still;
        if (entry->kind == gallery::Kind::Video) {
            kind = platform::MediaKind::Video;
        }
        else if (gallery::is_animated_format(entry->name)) {
            kind = platform::MediaKind::Animation;
        }
        hdmi_->open(session_.selected_path().string(), kind);
        cast_subject_.set(cast_subject_.value() + 1);
        return;
    }
    if (!caster_ || tv_.av_transport.empty()) {
        return;
    }
    caster_->cast(tv_, session_.selected_path().string(), entry->kind == gallery::Kind::Video);
    cast_subject_.set(cast_subject_.value() + 1);
}

void BaseViewModel::publish_media() {
    if (casting()) {
        cast_selected();
    }
    media_subject_.set(media_subject_.value() + 1);
}

void BaseViewModel::publish_listing() {
    title_subject_.set(session_.title().c_str());
    listing_subject_.set(listing_subject_.value() + 1);
}

} // namespace viewmodel
