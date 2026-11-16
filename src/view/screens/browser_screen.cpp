/*
 * SPDX-License-Identifier: MIT
 */

#include "browser_screen.h"

#include "asset_manager.h"
#include "theme.h"
#include "ui_const.h"

#include <algorithm>

namespace screen {
namespace {

constexpr int32_t kRowHeight = 23;
constexpr int32_t kListTop = 1;
constexpr int32_t kListHeight = kRowHeight * static_cast<int32_t>(BrowserScreen::kRows);
constexpr int32_t kScrollWidth = 3;

const char* icon_for(gallery::Kind kind) {
    switch (kind) {
        case gallery::Kind::Folder:
            return view::ICON_FOLDER;
        case gallery::Kind::Video:
            return view::ICON_VIDEO;
        default:
            return view::ICON_IMAGE;
    }
}

} // namespace

BrowserScreen::BrowserScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets)
    : BaseScreen(view_model, assets) {
    init();
}

void BrowserScreen::build_content(lv_obj_t* content) {
    const lv_font_t* text_font = assets().load_standard_font(14, app::StandardFontWeight::Regular);
    if (!text_font) {
        text_font = &lv_font_montserrat_14;
    }
    const lv_font_t* icon_font = assets().load_font("Phosphor-Fill.ttf", 16);

    for (size_t i = 0; i < rows_.size(); ++i) {
        auto& row = rows_[i];
        row.box = lv_obj_create(content);
        lv_obj_remove_style_all(row.box);
        lv_obj_set_size(row.box, view::kScreenWidth - 8 - kScrollWidth, kRowHeight - 1);
        lv_obj_set_pos(row.box, 3, kListTop + static_cast<int32_t>(i) * kRowHeight);
        lv_obj_set_style_radius(row.box, 4, 0);
        lv_obj_clear_flag(row.box, LV_OBJ_FLAG_SCROLLABLE);

        row.icon = lv_label_create(row.box);
        lv_obj_set_style_text_font(row.icon, icon_font ? icon_font : &lv_font_montserrat_14, 0);
        lv_obj_align(row.icon, LV_ALIGN_LEFT_MID, 6, 0);

        row.name = lv_label_create(row.box);
        lv_obj_set_style_text_font(row.name, text_font, 0);
        lv_label_set_long_mode(row.name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(row.name, view::kScreenWidth - 8 - kScrollWidth - 36);
        lv_obj_set_height(row.name, lv_font_get_line_height(text_font));
        lv_obj_align(row.name, LV_ALIGN_LEFT_MID, 30, 0);
    }

    empty_label_ = lv_label_create(content);
    lv_label_set_text(empty_label_, "No photos or videos here");
    lv_obj_set_style_text_font(empty_label_, text_font, 0);
    lv_obj_center(empty_label_);

    scroll_thumb_ = lv_obj_create(content);
    lv_obj_remove_style_all(scroll_thumb_);
    lv_obj_set_style_radius(scroll_thumb_, 2, 0);
    lv_obj_set_style_bg_opa(scroll_thumb_, LV_OPA_COVER, 0);

    // Al añadir cada observador LVGL lo llama una vez: eso hace el primer pintado.
    lv_subject_add_observer_obj(view_model().listing_subject(), refresh_observer, content, this);
    lv_subject_add_observer_obj(view_model().dark_mode_subject(), refresh_observer, content, this);
}

void BrowserScreen::refresh() {
    const auto& session = view_model().session();
    const auto& entries = session.entries();
    const auto colors = view::palette(view_model().is_dark_mode());
    const size_t selected = session.selected();

    // La ventana se mueve lo justo para que lo señalado siga a la vista.
    if (selected < top_) {
        top_ = selected;
    }
    else if (selected >= top_ + kRows) {
        top_ = selected + 1 - kRows;
    }
    if (entries.size() <= kRows) {
        top_ = 0;
    }
    else {
        top_ = std::min(top_, entries.size() - kRows);
    }

    for (size_t i = 0; i < rows_.size(); ++i) {
        auto& row = rows_[i];
        const size_t index = top_ + i;
        if (index >= entries.size()) {
            lv_obj_add_flag(row.box, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row.box, LV_OBJ_FLAG_HIDDEN);

        const auto& entry = entries[index];
        const bool is_selected = index == selected;
        lv_label_set_text(row.icon, icon_for(entry.kind));
        lv_label_set_text(row.name, entry.name.c_str());

        lv_obj_set_style_bg_color(row.box, colors.primary, 0);
        lv_obj_set_style_bg_opa(row.box, is_selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        // Sobre el color de selección el texto va con el color de fondo, que contrasta
        // con él en los dos temas.
        lv_obj_set_style_text_color(row.name, is_selected ? colors.background : colors.text, 0);
        lv_color_t icon_color = colors.info;
        if (entry.kind == gallery::Kind::Folder) {
            icon_color = colors.warning;
        }
        else if (entry.kind == gallery::Kind::Video) {
            icon_color = colors.error;
        }
        lv_obj_set_style_text_color(row.icon, is_selected ? colors.background : icon_color, 0);
    }

    if (entries.empty()) {
        lv_obj_remove_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(empty_label_, colors.text_disabled, 0);
    }
    else {
        lv_obj_add_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
    }

    if (entries.size() > kRows) {
        const auto total = static_cast<int32_t>(entries.size());
        const int32_t height = std::max<int32_t>(10, kListHeight * static_cast<int32_t>(kRows) / total);
        const int32_t travel = kListHeight - height;
        const int32_t y = travel * static_cast<int32_t>(top_) / (total - static_cast<int32_t>(kRows));
        lv_obj_remove_flag(scroll_thumb_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(scroll_thumb_, kScrollWidth, height);
        lv_obj_set_pos(scroll_thumb_, view::kScreenWidth - kScrollWidth - 2, kListTop + y);
        lv_obj_set_style_bg_color(scroll_thumb_, colors.border, 0);
    }
    else {
        lv_obj_add_flag(scroll_thumb_, LV_OBJ_FLAG_HIDDEN);
    }
}

void BrowserScreen::refresh_observer(lv_observer_t* observer, lv_subject_t*) {
    auto* screen = static_cast<BrowserScreen*>(lv_observer_get_user_data(observer));
    if (screen) {
        screen->refresh();
    }
}

} // namespace screen
