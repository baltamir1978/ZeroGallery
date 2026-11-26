#pragma once

#include <atomic>

namespace platform {

/**
 * Contadores para ver dónde se pierden fotogramas de vídeo (ZEROGALLERY_FPS_LOG=1 los escribe
 * cada segundo en el registro): los que llegan de ffmpeg, los que recoge el visor y los que
 * LVGL acaba mandando a la pantalla.
 */
struct FpsCounters {
    std::atomic<unsigned> published{0};
    std::atomic<unsigned> taken{0};
    std::atomic<unsigned> refreshed{0};
    std::atomic<unsigned> polls{0};
};

inline FpsCounters& fps_counters() {
    static FpsCounters counters;
    return counters;
}

} // namespace platform
