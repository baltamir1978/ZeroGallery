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
    // Intervalo entre fotogramas recogidos: el más largo del segundo (ms) y cuántos pasan de
    // 1,5 veces lo que tocaría (`late`, con `period_ms` puesto por el reproductor).
    std::atomic<unsigned> max_gap_ms{0};
    std::atomic<unsigned> late{0};
    std::atomic<unsigned> period_ms{40};
    std::atomic<long long> last_take_ms{0};

    /** Lo llama el visor con cada fotograma que recoge. */
    void note_take(long long now_ms) {
        taken++;
        const long long last = last_take_ms.exchange(now_ms);
        if (last > 0) {
            const auto gap = static_cast<unsigned>(now_ms - last);
            if (gap > max_gap_ms) {
                max_gap_ms = gap;
            }
            if (gap * 2 > period_ms * 3) {
                late++;
            }
        }
    }
};

inline FpsCounters& fps_counters() {
    static FpsCounters counters;
    return counters;
}

} // namespace platform
