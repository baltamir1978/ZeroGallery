#pragma once

#include <algorithm>
#include <chrono>
#include <thread>

namespace platform {

/**
 * Entrega los fotogramas de un vídeo a su hora. ffmpeg los escribe a rachas (dos o tres
 * seguidos, entre trozos de sonido) y quien los muestra se queda con el último que haya
 * llegado: el visor de la pantalla del aparato lo recoge cada pocos ms y el HDMI cambia de
 * búfer en cada refresco de la tele, así que de cada racha se perdían todos menos uno
 * (medido el 26-nov-2026: de 25 por segundo se veían 11 en el aparato y 14 por HDMI). Los
 * adelantados esperan aquí; la tubería de ffmpeg hace de colchón.
 *
 * Uno por cada ffmpeg (al pausar, saltar o girar se crea otro): el primer fotograma marca
 * la hora de los demás.
 */
class FramePacer {
public:
    explicit FramePacer(double fps)
        : period_(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(1.0 / std::max(fps, 1.0)))) {}

    /** Espera, si hace falta, a que le toque al fotograma que se acaba de leer. */
    void wait() {
        const auto now = std::chrono::steady_clock::now();
        if (!started_ || now > next_ + 5 * period_) {
            // Al empezar, o si se ha quedado atrás: desde ahora, sin correr para recuperar.
            started_ = true;
            next_ = now;
        }
        else if (now < next_) {
            std::this_thread::sleep_until(std::min(next_, now + 4 * period_));
        }
        next_ += period_;
    }

private:
    std::chrono::steady_clock::duration period_;
    std::chrono::steady_clock::time_point next_{};
    bool started_ = false;
};

} // namespace platform
