/*
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace platform {

/**
 * Hacia dónde está girado el aparato, según su acelerómetro (BMI270, que el núcleo enseña
 * por IIO en `/sys/bus/iio/devices`). Lo lee un hilo aparte unas pocas veces por segundo:
 * cada lectura es una conversación por I2C y no tiene por qué esperar el hilo de LVGL.
 *
 * Sólo existe mientras está el visor en pantalla: fuera de él no se lee nada. Si no hay
 * acelerómetro (el simulador, otro aparato) se queda en 0 y no arranca el hilo.
 * `ZEROGALLERY_IIO` cambia la carpeta donde se busca, para probarlo con ficheros
 * corrientes, y `ZEROGALLERY_ROTATE=0|1|2|3` fija el giro sin mirar el sensor.
 */
class OrientationSensor {
public:
    OrientationSensor();
    ~OrientationSensor();

    OrientationSensor(const OrientationSensor&) = delete;
    OrientationSensor& operator=(const OrientationSensor&) = delete;

    /** Cuartos de vuelta en el sentido del reloj, como `gallery::rotation_from_gravity`. */
    int turns() const;

private:
    void run();

    std::string device_;
    std::atomic<int> turns_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_{false};
    std::thread worker_;
};

} // namespace platform
