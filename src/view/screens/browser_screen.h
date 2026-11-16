/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"

#include <array>
#include <cstddef>

namespace screen {

/**
 * Lista de la carpeta actual: subcarpetas primero y luego fotos y vídeos.
 *
 * Sólo existen las filas que caben en pantalla y se rellenan con el tramo visible: una
 * carpeta con miles de fotos no crea miles de objetos de LVGL.
 */
class BrowserScreen : public BaseScreen {
public:
    static constexpr size_t kRows = 6;

    BrowserScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);

protected:
    void build_content(lv_obj_t* content) override;

private:
    struct Row {
        lv_obj_t* box{nullptr};
        lv_obj_t* icon{nullptr};
        lv_obj_t* name{nullptr};
    };

    void refresh();
    static void refresh_observer(lv_observer_t* observer, lv_subject_t* subject);

    std::array<Row, kRows> rows_{};
    lv_obj_t* empty_label_{nullptr};
    lv_obj_t* scroll_thumb_{nullptr};
    size_t top_{0};
};

} // namespace screen
