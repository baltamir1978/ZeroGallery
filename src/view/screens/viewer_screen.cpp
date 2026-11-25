/*
 * SPDX-License-Identifier: MIT
 */

#include "viewer_screen.h"

#include "asset_manager.h"
#include "theme.h"
#include "ui_const.h"

#include <algorithm>
#include <string>

namespace screen {
namespace {

constexpr uint32_t kPollPeriodMs = 15;
// El rótulo con el nombre se quita solo para no tapar la foto.
constexpr uint32_t kCaptionMs = 2500;
// «Loading» sólo si tarda: en las fotos que abren al momento sería un parpadeo.
constexpr uint32_t kLoadingNoticeMs = 350;
// Separación del rótulo con el nombre a los bordes.
constexpr int32_t kCaptionMargin = 4;
// Tras mandar una foto, el aviso con el nombre de la tele se queda un momento.
constexpr uint32_t kCastShownMs = 1800;
// Sin teles al cabo de este rato, la lista explica qué puede pasar.
constexpr uint32_t kNoTvHintMs = 8000;
constexpr size_t kPickerRows = 4;

std::string clock_text(double seconds) {
    const long total = seconds > 0 ? static_cast<long>(seconds) : 0;
    char text[32];
    if (total >= 3600) {
        lv_snprintf(text, sizeof(text), "%ld:%02ld:%02ld", total / 3600, total / 60 % 60, total % 60);
    }
    else {
        lv_snprintf(text, sizeof(text), "%ld:%02ld", total / 60, total % 60);
    }
    return text;
}

} // namespace

ViewerScreen::ViewerScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets),
      player_(std::make_unique<platform::MediaPlayer>()),
      orientation_(std::make_unique<platform::OrientationSensor>()),
      turns_(orientation_->turns()),
      canvas_(platform::kCanvasPixels, 0) {
    // Antes de cargar nada: que la primera foto ya salga para como está el aparato.
    player_->set_rotation(turns_);
    init(true);
}

ViewerScreen::~ViewerScreen() {
    if (poll_timer_) {
        lv_timer_delete(poll_timer_);
        poll_timer_ = nullptr;
    }
    orientation_.reset();
    // Antes de que se borren los objetos: para ffmpeg y con él el sonido.
    player_.reset();
}

void ViewerScreen::build_content(lv_obj_t* content) {
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    // Un rótulo girado asoma su caja sin girar por fuera de la pantalla: que no cuente
    // como contenido por el que desplazarse.
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    canvas_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    canvas_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    canvas_dsc_.header.w = platform::kCanvasWidth;
    canvas_dsc_.header.h = platform::kCanvasHeight;
    canvas_dsc_.header.stride = platform::kCanvasWidth * sizeof(uint16_t);
    canvas_dsc_.data_size = static_cast<uint32_t>(canvas_.size() * sizeof(uint16_t));
    canvas_dsc_.data = reinterpret_cast<const uint8_t*>(canvas_.data());

    image_ = lv_image_create(content);
    lv_image_set_src(image_, &canvas_dsc_);
    lv_obj_set_pos(image_, 0, 0);

    const lv_font_t* text_font = assets().load_standard_font(12, app::StandardFontWeight::Regular);
    if (!text_font) {
        text_font = &lv_font_montserrat_12;
    }

    caption_ = lv_label_create(content);
    lv_obj_set_style_text_font(caption_, text_font, 0);
    lv_obj_set_style_text_color(caption_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(caption_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(caption_, LV_OPA_60, 0);
    lv_obj_set_style_radius(caption_, 4, 0);
    lv_obj_set_style_pad_hor(caption_, 6, 0);
    lv_obj_set_style_pad_ver(caption_, 2, 0);
    lv_label_set_long_mode(caption_, LV_LABEL_LONG_DOT);
    lv_obj_set_height(caption_, lv_font_get_line_height(text_font) + 4);
    lv_obj_set_style_transform_pivot_x(caption_, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(caption_, lv_pct(50), 0);

    status_box_ = lv_obj_create(content);
    lv_obj_remove_style_all(status_box_);
    lv_obj_set_size(status_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_transform_pivot_x(status_box_, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(status_box_, lv_pct(50), 0);
    lv_obj_set_style_bg_color(status_box_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(status_box_, LV_OPA_60, 0);
    lv_obj_set_style_radius(status_box_, 8, 0);
    lv_obj_set_style_pad_hor(status_box_, 12, 0);
    lv_obj_set_style_pad_ver(status_box_, 8, 0);
    lv_obj_set_style_pad_row(status_box_, 2, 0);
    lv_obj_set_flex_flow(status_box_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(status_box_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(status_box_, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t* icon_font = assets().load_font("Phosphor-Fill.ttf", 30);
    status_icon_ = lv_label_create(status_box_);
    lv_obj_set_style_text_font(status_icon_, icon_font ? icon_font : &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(status_icon_, lv_color_white(), 0);

    status_text_ = lv_label_create(status_box_);
    lv_obj_set_style_text_font(status_text_, text_font, 0);
    lv_obj_set_style_text_color(status_text_, lv_color_white(), 0);
    hide_status();

    build_picker(content);

    playback_seen_ = lv_subject_get_int(view_model().playback_subject());
    lv_subject_add_observer_obj(view_model().playback_subject(), playback_observer, content, this);
    seek_seen_ = lv_subject_get_int(view_model().seek_subject());
    lv_subject_add_observer_obj(view_model().seek_subject(), seek_observer, content, this);
    lv_subject_add_observer_obj(view_model().cast_subject(), cast_observer, content, this);
    // LVGL llama al observador al añadirlo: esa primera llamada carga lo señalado.
    lv_subject_add_observer_obj(view_model().media_subject(), media_observer, content, this);

    poll_timer_ = lv_timer_create(poll_timer_cb, kPollPeriodMs, this);
}

void ViewerScreen::load_selected() {
    const auto& session = view_model().session();
    const auto* entry = session.selected_entry();
    if (!entry || entry->kind == gallery::Kind::Folder) {
        return;
    }

    auto kind = platform::MediaKind::Still;
    if (entry->kind == gallery::Kind::Video) {
        kind = platform::MediaKind::Video;
    }
    else if (gallery::is_animated_format(entry->name)) {
        kind = platform::MediaKind::Animation;
    }
    is_video_ = kind != platform::MediaKind::Still;
    tv_video_ = view_model().casting() && entry->kind == gallery::Kind::Video;
    if (tv_video_) {
        // En la tele: aquí sólo el aviso de cómo va, sin gastar en decodificarlo.
        player_->close();
        std::fill(canvas_.begin(), canvas_.end(), uint16_t{0});
        lv_obj_invalidate(image_);
        shown_state_ = platform::MediaState::Idle;
    }
    else {
        player_->open(session.selected_path().string(), kind);
        shown_state_ = platform::MediaState::Loading;
    }
    loading_since_ = lv_tick_get();
    loading_notice_ = false;
    cast_text_.clear();
    cast_seen_ = platform::CastState::Off;
    hide_status();

    size_t index = 0;
    size_t count = 0;
    session.media_position(index, count);
    const std::string caption = (view_model().casting() ? "TV  " : "") + std::to_string(index) + "/" +
                                std::to_string(count) + "  " + entry->name;
    lv_label_set_text(caption_, caption.c_str());
    lv_obj_remove_flag(caption_, LV_OBJ_FLAG_HIDDEN);
    caption_since_ = lv_tick_get();
    seek_caption_ = false;
    place_overlays();
}

void ViewerScreen::place_overlays() {
    // Ancho de la pantalla tal como se ve ahora: de lado, los 170 del alto.
    const bool sideways = gallery::rotation_swaps_axes(turns_);
    const int32_t seen_width = sideways ? view::kScreenHeight : view::kScreenWidth;
    const int32_t rotation = turns_ * 900; // décimas de grado

    lv_obj_set_style_max_width(status_box_, seen_width - 24, 0);
    lv_obj_set_style_transform_rotation(status_box_, rotation, 0);
    lv_obj_center(status_box_);

    // El rótulo va arriba a la izquierda de lo que se ve. LVGL gira cada objeto sobre su
    // centro sin moverlo, así que se calcula dónde cae ese centro en la pantalla real.
    lv_obj_set_style_max_width(caption_, seen_width - 2 * kCaptionMargin, 0);
    lv_obj_set_style_transform_rotation(caption_, rotation, 0);
    lv_obj_update_layout(caption_);
    const int32_t along = kCaptionMargin + lv_obj_get_width(caption_) / 2;  // desde la izquierda de lo que se ve
    const int32_t down = kCaptionMargin + lv_obj_get_height(caption_) / 2;  // desde arriba de lo que se ve
    int32_t x = along;
    int32_t y = down;
    switch (turns_) {
        case 1: // arriba es el borde derecho de la pantalla
            x = view::kScreenWidth - down;
            y = along;
            break;
        case 2:
            x = view::kScreenWidth - along;
            y = view::kScreenHeight - down;
            break;
        case 3: // arriba es el borde izquierdo
            x = down;
            y = view::kScreenHeight - along;
            break;
        default:
            break;
    }
    lv_obj_align(caption_, LV_ALIGN_CENTER, x - view::kScreenWidth / 2, y - view::kScreenHeight / 2);
}

void ViewerScreen::poll() {
    if (const int turns = orientation_->turns(); turns != turns_) {
        turns_ = turns;
        player_->set_rotation(turns_);
        place_overlays();
    }

    if (player_->take_frame(canvas_.data())) {
        lv_obj_invalidate(image_);
    }

    if (!lv_obj_has_flag(caption_, LV_OBJ_FLAG_HIDDEN) && lv_tick_elaps(caption_since_) >= kCaptionMs) {
        lv_obj_add_flag(caption_, LV_OBJ_FLAG_HIDDEN);
        seek_caption_ = false;
    }
    if (seek_caption_) {
        // El salto lo atiende el hilo del reproductor: el rótulo sigue a donde haya caído.
        const long second = static_cast<long>(player_->position());
        if (second != seek_caption_second_) {
            seek_caption_second_ = second;
            char text[32];
            if (second >= 3600) {
                lv_snprintf(text, sizeof(text), "%ld:%02ld:%02ld", second / 3600, second / 60 % 60, second % 60);
            }
            else {
                lv_snprintf(text, sizeof(text), "%ld:%02ld", second / 60, second % 60);
            }
            lv_label_set_text(caption_, text);
            place_overlays();
        }
    }

    if (picker_was_open_) {
        update_picker();
    }
    if (poll_cast()) {
        return;
    }

    const auto state = player_->state();
    if (state == platform::MediaState::Loading) {
        if (!loading_notice_ && lv_tick_elaps(loading_since_) >= kLoadingNoticeMs) {
            loading_notice_ = true;
            show_status(nullptr, "Loading...");
        }
        shown_state_ = state;
        return;
    }
    if (state == shown_state_) {
        return;
    }
    shown_state_ = state;
    loading_notice_ = false;

    switch (state) {
        case platform::MediaState::Paused:
            show_status(view::ICON_PAUSE, nullptr);
            break;
        case platform::MediaState::Ended:
            show_status(view::ICON_PLAY, nullptr);
            break;
        case platform::MediaState::Error:
            // Fuera la foto anterior: debajo del aviso parecería que es la que ha fallado.
            std::fill(canvas_.begin(), canvas_.end(), uint16_t{0});
            lv_obj_invalidate(image_);
            show_status(view::ICON_WARNING, player_->error().c_str());
            break;
        default:
            hide_status();
            break;
    }
}

bool ViewerScreen::poll_cast() {
    auto& vm = view_model();
    if (!vm.casting()) {
        if (cast_status_) {
            cast_status_ = false;
            cast_text_.clear();
            hide_status();
            // Que el aviso del reproductor de aquí se vuelva a poner si hace falta.
            shown_state_ = platform::MediaState::Idle;
        }
        return false;
    }

    if (const std::string notice = vm.take_output_notice(); !notice.empty()) {
        lv_label_set_text(caption_, notice.c_str());
        lv_obj_remove_flag(caption_, LV_OBJ_FLAG_HIDDEN);
        caption_since_ = lv_tick_get();
        seek_caption_ = false;
        place_overlays();
    }

    const auto state = vm.output_state();
    if (state != cast_seen_) {
        cast_seen_ = state;
        cast_state_since_ = lv_tick_get();
    }
    const uint32_t since = lv_tick_elaps(cast_state_since_);
    const std::string tv = vm.output_name();
    const char* icon = view::ICON_TELEVISION;
    std::string text;
    switch (state) {
        case platform::CastState::Sending:
            // Una foto suele llegar en un par de segundos: el aviso, sólo si tarda.
            if (tv_video_ || since >= kLoadingNoticeMs) {
                text = tv.empty() ? "Sending to TV..." : "Sending to " + tv + "...";
            }
            break;
        case platform::CastState::Showing:
            if (since < kCastShownMs) {
                text = tv;
            }
            break;
        case platform::CastState::Playing:
        case platform::CastState::Paused:
            if (tv_video_) {
                if (state == platform::CastState::Paused) {
                    icon = view::ICON_PAUSE;
                }
                text = tv + "\n" + clock_text(vm.output_position());
                if (vm.output_duration() > 0) {
                    text += " / " + clock_text(vm.output_duration());
                }
            }
            break;
        case platform::CastState::Stopped:
            if (tv_video_) {
                icon = view::ICON_PLAY;
                text = "Ended on " + tv;
            }
            break;
        case platform::CastState::Error:
            icon = view::ICON_WARNING;
            text = vm.output_error();
            break;
        default:
            break;
    }

    if (text.empty()) {
        if (cast_status_ && !cast_text_.empty()) {
            hide_status();
            cast_text_.clear();
        }
    }
    else if (text != cast_text_ || icon != cast_icon_) {
        cast_text_ = text;
        cast_icon_ = icon;
        show_status(icon, text.c_str());
        place_overlays();
    }
    cast_status_ = true;

    // Una foto se sigue viendo aquí: su «Loading» y sus errores siguen valiendo, pero
    // sólo cuando la tele no tiene nada que decir.
    return !cast_text_.empty() || tv_video_;
}

void ViewerScreen::build_picker(lv_obj_t* content) {
    const lv_font_t* text_font = assets().load_standard_font(12, app::StandardFontWeight::Regular);
    if (!text_font) {
        text_font = &lv_font_montserrat_12;
    }
    const lv_font_t* small_font = assets().load_standard_font(10);

    picker_ = lv_obj_create(content);
    lv_obj_remove_style_all(picker_);
    lv_obj_set_size(picker_, 280, 132);
    lv_obj_center(picker_);
    lv_obj_set_style_bg_opa(picker_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(picker_, 6, 0);
    lv_obj_set_style_border_width(picker_, 1, 0);
    lv_obj_set_style_pad_all(picker_, 8, 0);
    lv_obj_remove_flag(picker_, LV_OBJ_FLAG_SCROLLABLE);

    picker_title_ = lv_label_create(picker_);
    lv_obj_set_style_text_font(picker_title_, text_font, 0);
    lv_obj_align(picker_title_, LV_ALIGN_TOP_LEFT, 0, 0);

    for (size_t i = 0; i < picker_rows_.size(); ++i) {
        auto* row = lv_label_create(picker_);
        lv_obj_set_style_text_font(row, text_font, 0);
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
        lv_obj_set_size(row, LV_PCT(100), 20);
        lv_obj_set_style_pad_hor(row, 6, 0);
        lv_obj_set_style_pad_top(row, 2, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 20 + static_cast<int32_t>(i) * 21);
        picker_rows_[i] = row;
    }

    picker_hint_ = lv_label_create(picker_);
    lv_obj_set_style_text_font(picker_hint_, small_font ? small_font : &lv_font_montserrat_10, 0);
    lv_label_set_long_mode(picker_hint_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(picker_hint_, LV_PCT(100));
    lv_obj_align(picker_hint_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_flag(picker_, LV_OBJ_FLAG_HIDDEN);
}

void ViewerScreen::update_picker() {
    const bool open = view_model().picker_open();
    if (!open) {
        if (picker_was_open_) {
            lv_obj_add_flag(picker_, LV_OBJ_FLAG_HIDDEN);
            picker_was_open_ = false;
        }
        return;
    }
    if (!picker_was_open_) {
        picker_was_open_ = true;
        picker_opened_ = lv_tick_get();
        picker_version_seen_ = view_model().picker_version() + 1; // que se pinte
        picker_empty_hint_ = false;
        lv_obj_remove_flag(picker_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(picker_);
    }
    const auto colors = view::palette(view_model().is_dark_mode());
    lv_obj_set_style_bg_color(picker_, colors.surface, 0);
    lv_obj_set_style_border_color(picker_, colors.border, 0);
    lv_obj_set_style_text_color(picker_title_, colors.text, 0);
    lv_obj_set_style_text_color(picker_hint_, colors.text_disabled, 0);

    const auto renderers = view_model().picker_names();
    const uint32_t version = view_model().picker_version();
    const bool empty_hint = renderers.empty() && lv_tick_elaps(picker_opened_) >= kNoTvHintMs;
    // Se repinta sólo si cambia algo: esto se llama en cada vuelta del temporizador.
    const int32_t selection = static_cast<int32_t>(view_model().picker_selection());
    const int32_t dark = view_model().is_dark_mode() ? 1 : 0;
    if (picker_version_seen_ == version && picker_drawn_selection_ == selection && picker_drawn_dark_ == dark &&
        picker_empty_hint_ == empty_hint) {
        return;
    }
    picker_version_seen_ = version;
    picker_drawn_selection_ = selection;
    picker_drawn_dark_ = dark;
    picker_empty_hint_ = empty_hint;

    lv_label_set_text(picker_title_, renderers.empty() ? "Looking for TVs..." : "Send to TV");
    // La ventana de filas sigue a la selección.
    const size_t first = renderers.size() <= kPickerRows || static_cast<size_t>(selection) < kPickerRows
                             ? 0
                             : static_cast<size_t>(selection) + 1 - kPickerRows;
    for (size_t i = 0; i < picker_rows_.size(); ++i) {
        auto* row = picker_rows_[i];
        const size_t index = first + i;
        if (index >= renderers.size()) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(row, renderers[index].c_str());
        const bool selected = static_cast<int32_t>(index) == selection;
        lv_obj_set_style_bg_color(row, colors.primary, 0);
        lv_obj_set_style_bg_opa(row, selected ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(row, selected ? colors.primary : colors.text, 0);
    }
    if (empty_hint) {
        lv_label_set_text(picker_hint_, "None yet. Is the TV on and on this Wi-Fi? (Samsung, Sony, Hisense: allow "
                                        "the device or turn on sharing / renderer)");
    }
    else {
        lv_label_set_text(picker_hint_, renderers.empty() ? "ESC cancel" : "ENTER send   F/X choose   ESC cancel");
    }
}

void ViewerScreen::cast_observer(lv_observer_t* observer, lv_subject_t*) {
    auto* screen = static_cast<ViewerScreen*>(lv_observer_get_user_data(observer));
    if (screen && screen->picker_) {
        if (screen->view_model().picker_open() && !screen->picker_was_open_) {
            screen->update_picker();
        }
        else if (screen->picker_was_open_) {
            screen->update_picker();
        }
    }
}

void ViewerScreen::show_status(const char* icon, const char* text) {
    if (icon) {
        lv_label_set_text(status_icon_, icon);
        lv_obj_remove_flag(status_icon_, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(status_icon_, LV_OBJ_FLAG_HIDDEN);
    }
    if (text && text[0] != '\0') {
        lv_label_set_text(status_text_, text);
        lv_obj_remove_flag(status_text_, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        lv_obj_add_flag(status_text_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(status_box_, LV_OBJ_FLAG_HIDDEN);
}

void ViewerScreen::hide_status() {
    lv_obj_add_flag(status_box_, LV_OBJ_FLAG_HIDDEN);
}

void ViewerScreen::media_observer(lv_observer_t* observer, lv_subject_t*) {
    auto* screen = static_cast<ViewerScreen*>(lv_observer_get_user_data(observer));
    if (screen) {
        screen->load_selected();
    }
}

void ViewerScreen::playback_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* screen = static_cast<ViewerScreen*>(lv_observer_get_user_data(observer));
    if (!screen) {
        return;
    }
    // La llamada que LVGL hace al registrar el observador no es una pulsación.
    const int32_t value = lv_subject_get_int(subject);
    if (value == screen->playback_seen_) {
        return;
    }
    screen->playback_seen_ = value;
    if (screen->is_video_) {
        screen->player_->toggle_pause();
    }
}

void ViewerScreen::seek_observer(lv_observer_t* observer, lv_subject_t* subject) {
    auto* screen = static_cast<ViewerScreen*>(lv_observer_get_user_data(observer));
    if (!screen) {
        return;
    }
    const int32_t value = lv_subject_get_int(subject);
    if (value == screen->seek_seen_) {
        return;
    }
    screen->seek_seen_ = value;
    screen->player_->seek(screen->view_model().seek_seconds());
    screen->seek_caption_ = true;
    screen->seek_caption_second_ = -1;
    lv_obj_remove_flag(screen->caption_, LV_OBJ_FLAG_HIDDEN);
    screen->caption_since_ = lv_tick_get();
}

void ViewerScreen::poll_timer_cb(lv_timer_t* timer) {
    auto* screen = static_cast<ViewerScreen*>(lv_timer_get_user_data(timer));
    if (screen) {
        screen->poll();
    }
}

} // namespace screen
