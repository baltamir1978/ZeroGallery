/*
 * SPDX-License-Identifier: MIT
 */

#include "orientation_sensor.h"

#include "gallery.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace platform {
namespace {

// Cinco lecturas por segundo: de sobra para girar un aparato con la mano.
constexpr auto kPeriod = std::chrono::milliseconds(200);
// Lecturas seguidas que tienen que coincidir antes de girar: una sacudida no cuenta.
constexpr int kStableReadings = 2;

bool read_number(const std::string& path, double& value) {
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return false;
    }
    const bool ok = std::fscanf(file, "%lf", &value) == 1;
    std::fclose(file);
    return ok;
}

bool read_gravity(const std::string& device, double& x, double& y, double& z) {
    return read_number(device + "/in_accel_x_raw", x) && read_number(device + "/in_accel_y_raw", y) &&
           read_number(device + "/in_accel_z_raw", z);
}

/** Primer dispositivo IIO con acelerómetro: se busca por lo que sabe hacer, no por su número. */
std::string find_accelerometer() {
    const char* forced = std::getenv("ZEROGALLERY_IIO");
    const std::filesystem::path root = forced && forced[0] != '\0' ? forced : "/sys/bus/iio/devices";
    std::error_code error;
    for (std::filesystem::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const std::string device = it->path().string();
        double x = 0;
        double y = 0;
        double z = 0;
        if (read_gravity(device, x, y, z)) {
            return device;
        }
    }
    return {};
}

} // namespace

OrientationSensor::OrientationSensor() {
    if (const char* fixed = std::getenv("ZEROGALLERY_ROTATE"); fixed && fixed[0] >= '0' && fixed[0] <= '3') {
        turns_ = fixed[0] - '0';
        return;
    }
    device_ = find_accelerometer();
    if (device_.empty()) {
        return;
    }
    // La primera lectura, ya: si se abre el visor con el aparato girado, que la primera
    // foto salga girada y no dé la vuelta medio segundo después.
    double x = 0;
    double y = 0;
    double z = 0;
    if (read_gravity(device_, x, y, z)) {
        turns_ = gallery::rotation_from_gravity(x, y, z, 0);
    }
    worker_ = std::thread([this] { run(); });
}

OrientationSensor::~OrientationSensor() {
    if (!worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

int OrientationSensor::turns() const {
    return turns_;
}

void OrientationSensor::run() {
    int candidate = turns_;
    int repeats = 0;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!wake_.wait_for(lock, kPeriod, [this] { return quit_; })) {
        lock.unlock();
        double x = 0;
        double y = 0;
        double z = 0;
        if (read_gravity(device_, x, y, z)) {
            const int current = turns_;
            const int wanted = gallery::rotation_from_gravity(x, y, z, current);
            if (wanted == current) {
                repeats = 0;
            }
            else if (wanted != candidate) {
                candidate = wanted;
                repeats = 1;
            }
            else if (++repeats >= kStableReadings) {
                turns_ = wanted;
                repeats = 0;
            }
        }
        lock.lock();
    }
}

} // namespace platform
