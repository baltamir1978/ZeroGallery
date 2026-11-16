/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "base_screen.h"
#include "media_player.h"

#include <memory>
#include <vector>

namespace screen {

/**
 * Foto o vídeo a pantalla completa. Qué se enseña lo dice la selección de la sesión;
 * esta pantalla sólo pide al reproductor que lo cargue y pinta lo que va llegando.
 */
class ViewerScreen : public BaseScreen {
public:
    ViewerScreen(viewmodel::BaseViewModel& view_model, app::AssetManager& assets);
    ~ViewerScreen() override;

protected:
    void build_content(lv_obj_t* content) override;

private:
    void load_selected();
    void poll();
    void show_status(const char* icon, const char* text);
    void hide_status();

    static void media_observer(lv_observer_t* observer, lv_subject_t* subject);
    static void playback_observer(lv_observer_t* observer, lv_subject_t* subject);
    static void poll_timer_cb(lv_timer_t* timer);

    std::unique_ptr<platform::MediaPlayer> player_;
    std::vector<uint16_t> canvas_;
    lv_image_dsc_t canvas_dsc_{};
    lv_obj_t* image_{nullptr};
    lv_obj_t* caption_{nullptr};
    lv_obj_t* status_box_{nullptr};
    lv_obj_t* status_icon_{nullptr};
    lv_obj_t* status_text_{nullptr};
    lv_timer_t* poll_timer_{nullptr};
    platform::MediaState shown_state_{platform::MediaState::Idle};
    bool is_video_{false};
    bool loading_notice_{false};
    int32_t playback_seen_{0};
    uint32_t loading_since_{0};
    uint32_t caption_since_{0};
};

} // namespace screen
