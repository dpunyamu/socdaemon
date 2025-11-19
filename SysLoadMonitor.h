#ifndef SYSLOADMONITOR_H
#define SYSLOADMONITOR_H

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <android/log.h>

// Exponential-moving-average helpers (handle irregular sampling intervals)
#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <fstream>

// Default sampler interval is 3 seconds. Constructor can override per-instance.
static constexpr std::chrono::milliseconds g_samplerIntervalDefault{3000};


#define SYS_MON_LOG_TAG "SocDaemon"
#define SYSMON_ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  SYS_MON_LOG_TAG, __VA_ARGS__)
#define SYSMON_ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, SYS_MON_LOG_TAG, __VA_ARGS__)
#define SYSMON_ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, SYS_MON_LOG_TAG, __VA_ARGS__)

#include "HintMonitor.h"

// SysLoadMonitor: cleaned up to assume an external thread will call monitorLoop()
// (SoCDaemon will create the pthread; do not create or manage std::thread here)

class SysLoadMonitor : public HintMonitor {
public:
    SysLoadMonitor(const std::string& name,
                   std::chrono::milliseconds interval = g_samplerIntervalDefault)
        : HintMonitor(name),
          samplerRunning_(false),
          samplerPaused_(false),
          samplerInterval_(interval) {}

    ~SysLoadMonitor() { stop(); }

    // monitorLoop() is executed by an external thread (SoCDaemon). Do NOT spawn a thread here.
    void monitorLoop() override;

    // Initialize internal flags only; do not create a thread.
    int init() {
        bool expected = false;
        if (samplerRunning_.compare_exchange_strong(expected, true)) {
            std::lock_guard<std::mutex> lk(pauseMutex_);
            samplerPaused_.store(true); // start in paused state
            SYSMON_ALOGI("SysLoadMonitor: initialized (external thread expected)");
        }
        return 0;
    }

    // Stop sampling and wake any waiters.
    void stop() {
        samplerRunning_.store(false);
        {
            std::lock_guard<std::mutex> lk(pauseMutex_);
            samplerPaused_.store(false);
        }
        pauseCv_.notify_all();
        SYSMON_ALOGI("SysLoadMonitor: stop requested");
    }

    // Pause sampling
    void pause() {
        samplerPaused_.store(true);
        pauseCv_.notify_all();
        SYSMON_ALOGI("SysLoadMonitor: Pause periodic CPU load checks");
    }

    // Resume sampling
    void restart() {
        samplerPaused_.store(false);
        pauseCv_.notify_all();
        SYSMON_ALOGI("SysLoadMonitor: Resume periodic CPU load checks");
    }

    // Accessors
    double getSysCpuLoad();
    double getSysCpuLoadOld();
    double getLatestSysCpuLoad() const;
    std::vector<double> getEachCpuLoad();
    double getSysCpuLoadDetails();

private:
    struct SystemLoadSample {
        unsigned long long totalTime = 0;
        unsigned long long idleTime  = 0;
    };

    // Control flags (no internal std::thread anymore)
    std::atomic<bool> samplerRunning_;
    std::atomic<bool> samplerPaused_;
    mutable std::mutex pauseMutex_;
    std::condition_variable pauseCv_;

    // Recent samples
    SystemLoadSample lastSample_;
    SystemLoadSample currentSample_;
    std::chrono::milliseconds samplerInterval_;

    static constexpr double kSysloadHighThreshold = 25.0;
};
#endif // SYSLOADMONITOR_H