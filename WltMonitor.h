#pragma once

#include <string>
#include <functional>
#include <android/log.h>

#include "HintMonitor.h"

// Logging macros for wltMonitor
#define WLT_MONITOR_LOG_TAG "SocDaemon_wltMonitor"
#define WLTLOGD(...) __android_log_print(ANDROID_LOG_DEBUG, WLT_MONITOR_LOG_TAG, __VA_ARGS__)
#define WLTLOGI(...) __android_log_print(ANDROID_LOG_INFO, WLT_MONITOR_LOG_TAG, __VA_ARGS__)
#define WLTLOGE(...) __android_log_print(ANDROID_LOG_ERROR, WLT_MONITOR_LOG_TAG, __VA_ARGS__)

/**
 * @brief Monitor WLT (workload Type) hints by reading sysfs files.
 */
class WltMonitor : public HintMonitor {
public:
    WltMonitor(const std::string& name, const std::string& sysfsPath, int pollTimeoutMs, int notificationDelay = -1);

    virtual ~WltMonitor() = default;

    bool readValueOnce(std::string& value_out);
    bool readValue(int fd, std::string& value_out);

    void monitorLoop() override;
    int init();

private:
    std::string sysfs_path_;
    int poll_timeout_ms_;
    int notificationDelay_ = -1;
    static constexpr size_t kSysfsReadBufferSize = 16;
};
