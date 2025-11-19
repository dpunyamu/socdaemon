#pragma once

#include <string>
#include <functional>
#include <android/log.h>

#include "HintMonitor.h"

// Logging macros for wltMonitor
#define GPU_MONITOR_LOG_TAG "SocDaemon_GPUMonitor"
#define GPULOGD(...) __android_log_print(ANDROID_LOG_DEBUG, GPU_MONITOR_LOG_TAG, __VA_ARGS__)
#define GPULOGI(...) __android_log_print(ANDROID_LOG_INFO, GPU_MONITOR_LOG_TAG, __VA_ARGS__)
#define GPULOGE(...) __android_log_print(ANDROID_LOG_ERROR, GPU_MONITOR_LOG_TAG, __VA_ARGS__)

/**
 * @brief Monitor GPU RC6 residency by reading sysfs entry.
 */
class GpuRc6Monitor : public HintMonitor {
public:
    GpuRc6Monitor(const std::string& name, const std::string& sysfsPath, int pollTimeoutMs);

    virtual ~GpuRc6Monitor() = default;
    bool readValueOnce(std::string& value_out);
    bool readValue(int fd, std::string& value_out);
    void onValueChanged(int previous_value, int current_value) override;
    void monitorLoop() override;
    int init();
    void stop();
    void pause();
    void resume();

private:
    std::string sysfs_path_;
    int poll_timeout_ms_;
    static constexpr size_t kSysfsReadBufferSize = 32;
    static constexpr int kGpuHighLoadPercent = 40; // Example threshold percentage
    std::mutex pauseMutex_;
    std::condition_variable pauseCv_;
    bool paused_ = false;
    bool shouldExit_ = false;
};
