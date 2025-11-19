// Add missing destructor implementation to resolve vtable error
GpuLoadMonitor::~GpuLoadMonitor() {
    stop();
}
// GpuLoadMonitor.cpp
#include "GpuLoadMonitor.h"

static std::mutex g_gpuEmaMutex;
static double g_gpuBusyValue = -1.0; // negative => not yet initialized
static double g_gpuBusyValuePrev = -1.0; // previous EMA value
//static std::chrono::steady_clock::time_point g_gpuEmaLastTs = std::chrono::steady_clock::now();
// Time constant (tau) in seconds: larger => slower smoothing. Tune as needed.
//static constexpr double kGpuEmaTimeConstantSec = 1.5;

void GpuLoadMonitor::monitorLoop() {
    SYSMON_ALOGI("GpuLoadMonitor: Thread started");

    samplerRunning_.store(true);
    while (samplerRunning_.load()) {
        // If paused, wait until restarted or stopped
        {
            std::unique_lock<std::mutex> lk(pauseMutex_);
            pauseCv_.wait(lk, [this] {
                return !samplerPaused_.load() || !samplerRunning_.load();
            });
            if (!samplerRunning_.load())
                break;
        }

        // Perform GPU idle residency read and update samples
        SYSMON_ALOGI("GpuLoadMonitor: Periodic GPU load check");

        if (getGpuLoad() > kGpuLoadHighThreshold) {
            SYSMON_ALOGI("GpuLoadMonitor: High GPU load detected more than %.1f", kGpuLoadHighThreshold);
            onValueChanged(g_gpuBusyValuePrev, g_gpuBusyValue);
        }

        // Sleep for up to 1s but wake early if paused/stopped
        {
            std::unique_lock<std::mutex> lk(pauseMutex_);
            pauseCv_.wait_for(lk, samplerInterval_, [this]() {
                return samplerPaused_.load() || !samplerRunning_.load();
            });
        }
    }
    samplerRunning_.store(false);
    SYSMON_ALOGI("GpuLoadMonitor: sampler thread exiting");
}

double GpuLoadMonitor::getGpuLoad() {

    // Backup previous sample
    lastSample_ = currentSample_;
    SYSMON_ALOGD("GpuLoadMonitor: backed up last sample: idle_residency_ms=%llu",
                 lastSample_.idle_residency_ms);

    FILE* f = fopen("/sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms", "r");
    if (!f) {
        SYSMON_ALOGE("GpuLoadMonitor: failed to open /sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms");
        return -1.0;
    }

    // Read the residency value (should be a single integer in ms)
    unsigned long long idleResidencyMs = 0;
    if (fscanf(f, "%llu", &idleResidencyMs) != 1) {
        SYSMON_ALOGE("GpuLoadMonitor: failed to parse idle_residency_ms value");
        fclose(f);
        return -1.0;
    };
    fclose(f);
    if (currentSample_.idle_residency_ms > lastSample_.idle_residency_ms) {
        delta_idle_residency = currentSample_.idle_residency_ms - lastSample_.idle_residency_ms;
    } else {
        delta_idle_residency = 0;
    }
    double gpu_idle_percent = (double)delta_idle_residency * 100.0 / (double)1000;
    double gpu_busy_percent = 100 - (gpu_idle_percent > 100.0 ? 100.0 : gpu_idle_percent);
    // Update current sample
    currentSample_.idle_residency_ms = idleResidencyMs;
    SYSMON_ALOGI("GpuLoadMonitor: Read idle_residency_ms = %llu", idleResidencyMs);

    // You may want to compute and return a load value here, e.g., busy percent
    // For now, just return the raw value
    return gpu_busy_percent;
}
