#pragma once

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include "HintManager.h"
#include "HintMonitor.h"
#include "SysLoadMonitor.h"
#include "WltMonitor.h"
#include "HfiMonitor.h"
#include "GpuRc6Monitor.h"

// Logging helpers (avoid leaking macro LOG_TAG into other translation units)
inline constexpr char kLogTag[] = "SocDaemon";
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, kLogTag, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

class SocDaemon {
public:
    // Constructor / main entry
    SocDaemon(bool sendHint, const std::string& socHint, int notificationDelay) noexcept;
    ~SocDaemon() = default;

    // Start monitoring; may spawn threads.
    void start();

private:
    enum class CCGlobalState : int { Open = 0, CoreContainment = 1 };

    enum class WltType : int {
        Idle    = 0,
        Btl     = 1,
        Sustain = 2,
        Bursty  = 3,
    };

    // Debounce thread and related utilities
    void startDebounceThreadOnce();
    void debounceThreadFunc();

    // Debounce control helpers
    void startCCEntryDebounceTimer() noexcept;
    void stopCCEntryDebounceTimer() noexcept;
    bool isCCEntryDebounceTimerRunning() const noexcept;

    void startCCExitDebounceTimer(std::chrono::milliseconds timeout =
                                      std::chrono::milliseconds(1000)) noexcept;
    void stopCCExitDebounceTimer() noexcept;
    bool isCCExitDebounceTimerRunning() const noexcept;

    // Monitor callbacks and helpers
    void handleChangeAlert(const std::string& name, int oldValue, int newValue);
    double getSysCpuLoad() const noexcept;
    double getLatestSysCpuLoad() const noexcept;
    void sendHintIfAllowed(int value, const char* reason);

    // Static wrapper for pthreads
    static void* monitorSysfsWrapper(void* arg) noexcept;

    // --- Private data members ---

    // Public-facing manager (owned)
    HintManager hintManager;

    // Monitors and their threads
    std::vector<std::unique_ptr<HintMonitor>> monitors_;
    SysLoadMonitor* sysLoadMonitorPtr_ = nullptr; // non-owning
    GpuRc6Monitor* gpuRc6MonitorPtr_ = nullptr; // non-owning
    pthread_t gpuMonitorThread_ = 0;
    bool gpuMonitorThreadRunning_ = false;
    std::vector<pthread_t> threads_;

    // Configuration/state
    bool sendHint_;
    std::string socHint_;
    int notificationDelay_;
    bool efficientMode_ = false;

    // Global state for the daemon: Open (normal monitoring) or CoreContainment (consolidated)
    std::atomic<CCGlobalState> CCGlobalState_{CCGlobalState::Open};

    // Single debounce thread + shared CV/mutex handling both entry and exit timers
    mutable std::mutex debounceMutex_;
    std::condition_variable debounceCv_;
    std::thread debounceThread_;
    std::atomic<bool> debounceThreadStarted_{false};

    // Entry debounce (10s) variables
    std::atomic<bool> ccEntryDebounceActive_{false};
    std::atomic<bool> ccEntryDebounceCancelled_{false};
    std::chrono::steady_clock::time_point ccEntryDebounceStartTime_{};
    static constexpr std::chrono::milliseconds kCCEntryDebounceMs{10000};

    // Exit debounce (1s) variables
    std::atomic<bool> ccExitDebounceActive_{false};
    std::atomic<bool> ccExitDebounceCancelled_{false};
    // Exit debounce duration (mutable; default 1000ms)
    std::chrono::milliseconds ccExitDebounceMs_{1000};

    // System load thresholds (tunable) to get out of CC.
    static constexpr double kSysloadSlopeThreshold = 5.0;
    double latestSysCpuLoadCC_{0.0};

    // Disable copy/move to avoid accidental duplication of threads and resources
    SocDaemon(const SocDaemon&) = delete;
    SocDaemon& operator=(const SocDaemon&) = delete;
    SocDaemon(SocDaemon&&) = delete;
    SocDaemon& operator=(SocDaemon&&) = delete;
};