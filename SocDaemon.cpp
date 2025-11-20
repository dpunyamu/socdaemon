#include "SocDaemon.h"
#include "GpuRc6Monitor.h"

SocDaemon::SocDaemon(bool sendHint, bool sendGfxHint, const std::string& socHint, int notificationDelay) noexcept
    : sendHint_(sendHint), sendGfxHint_(sendGfxHint), socHint_(socHint), notificationDelay_(notificationDelay) {
    startDebounceThreadOnce();
}

void SocDaemon::start() {
    ALOGI("SocDaemon: V0.89 Main daemon process starting...");
    
    if (!socHint_.empty()) {
        ALOGI("SocDaemon: socHint is set to %s", socHint_.c_str());
    }

    // Only add WltMonitor if socHint_ is "wlt" or "swlt"
    if (socHint_ == "wlt" || socHint_ == "swlt") {
        auto wltMonitorPtr = std::make_unique<WltMonitor>(
            "WltMonitor",
            "/sys/devices/pci0000:00/0000:00:04.0/workload_hint/workload_type_index",
            -1,
            notificationDelay_);

        if (wltMonitorPtr->init() < 0) {
            ALOGE("SocDaemon: WltMonitor initialization failed, not adding to monitors_.");
        } else {
            monitors_.push_back(std::move(wltMonitorPtr));
        }

    } else if (socHint_ == "hfi") {
        auto hfiMonitorPtr = std::make_unique<HfiMonitor>("HfiMonitor");

        if (hfiMonitorPtr->init() < 0) {
            ALOGE("SocDaemon: HfiMonitor initialization failed, not adding to monitors_.");
        } else {
            monitors_.push_back(std::move(hfiMonitorPtr));
        }
    }

    auto gpuMonitorPtr = std::make_unique<GpuRc6Monitor>(
        "GpuRc6Monitor",
        "/sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms",
        1000 // poll timeout in ms
    );
    
    gpuMonitorPtr->pause(); // Start in paused state for WLT Idle/Btl
    if (gpuMonitorPtr->init() < 0) {
            ALOGE("SocDaemon: GpuRc6Monitor initialization failed, not adding to monitors_.");
        } else {
            monitors_.push_back(std::move(gpuMonitorPtr));
            gpuRc6MonitorPtr_ = static_cast<GpuRc6Monitor*>(monitors_.back().get()); // non-owning pointer
        }


    // Add SysLoadMonitor
    auto localSysLoad = std::make_unique<SysLoadMonitor>("SysLoadMonitor");
    if (localSysLoad->init() < 0) {
        ALOGE("SocDaemon: SysLoadMonitor initialization failed, not adding to monitors_.");
    } else {
        SysLoadMonitor* rawPtr = localSysLoad.get();
        monitors_.push_back(std::move(localSysLoad)); // monitors_ now owns the SysLoadMonitor
        sysLoadMonitorPtr_ = rawPtr; // non-owning pointer for fast access without RTTI
        ALOGI("SocDaemon: SysLoadMonitor initialized and added to monitors_.");
    }

    // Register callback for each monitor
    for (auto& monitor : monitors_) {
        monitor->setChangeAlertCallback([this](const std::string& name, int oldValue, int newValue) {
            this->handleChangeAlert(name, oldValue, newValue);
        });
    }

    // Create and launch each monitoring thread
    threads_.resize(monitors_.size());
    for (size_t i = 0; i < monitors_.size(); ++i) {
        if (pthread_create(&threads_[i], NULL, SocDaemon::monitorSysfsWrapper, monitors_[i].get()) != 0) {
            ALOGE("SocDaemon: Failed to create thread %zu: %s", i, std::strerror(errno));
        }
    }

    // Keep the main daemon process alive indefinitely.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
}

void SocDaemon::startDebounceThreadOnce() {
    static bool started = false;
    if (!started) {
        debounceThread_ = std::thread(&SocDaemon::debounceThreadFunc, this);
        debounceThread_.detach();
        started = true;
    }
}

void SocDaemon::debounceThreadFunc() {
    using clock = std::chrono::steady_clock;
    std::unique_lock<std::mutex> lock(debounceMutex_);
    while (true) {
        // Wait until any timer is requested or thread is marked started
        debounceCv_.wait(lock, [this] {
            return ccEntryDebounceActive_.load() || ccExitDebounceActive_.load() ||debounceThreadStarted_.load();
        });

        // Reset thread-started marker
       debounceThreadStarted_ = false;

        // Process timers while any are active
        while (ccEntryDebounceActive_.load() || ccExitDebounceActive_.load()) {
            if (ccEntryDebounceActive_.load()) {
                // Start waiting for the entry debounce period from now.
                auto deadline = clock::now() + kCCEntryDebounceMs;

                // Wait until deadline or until notified (cancel/restart)
                while (ccEntryDebounceActive_.load()) {
                    if (debounceCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                        break; // timer expired
                    }
                    // woken up, re-evaluate whether the timer is still active or cancelled
                }

                ccEntryDebounceActive_ = false;

                if (!ccEntryDebounceCancelled_.load()) {
                    // perform long work without holding mutex
                    lock.unlock();
                    // add to private members
                    double currentSysCpuLoad = getSysCpuLoad();
                    ALOGI("SocDaemon: Open : EntryDebounceTimer Expired. SysCpuLoad=%f", currentSysCpuLoad);
                    //AR: Erin to make 0.5 value as configuration.
                    if (currentSysCpuLoad < 25.0) {
                        CCGlobalState prev = CCGlobalState_.exchange(CCGlobalState::CoreContainment);
                        if (prev != CCGlobalState::CoreContainment) {
                            sendHintIfAllowed(1, "EntryDebounceTimerExpired");
                        } else {
                            ALOGI("SocDaemon: Already in CoreContainment state, no transition needed");
                        }
                    } else {
                        ALOGI("SocDaemon: System load is high. Remain in MONITOR state");
                    }

                    lock.lock();
                } else {
                    ccEntryDebounceCancelled_ = false;
                }
            }

            if (ccExitDebounceActive_.load()) {
                ALOGI("SocDaemon: ExitDebounceTimer Started");

                std::chrono::milliseconds exitDelay = ccExitDebounceMs_;
                auto deadline = clock::now() + exitDelay;

                // Wait until deadline or until notified (cancel/restart)
                while (ccExitDebounceActive_.load()) {
                    if (debounceCv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                        break; // timer expired
                    }
                    // woken up, re-evaluate whether the timer is still active or cancelled
                }

                ccExitDebounceActive_ = false;
                if (!ccExitDebounceCancelled_.load()) {
                    lock.unlock();
                    if (CCGlobalState_.load() == CCGlobalState::CoreContainment) {
                        double currentSysCpuLoad = getSysCpuLoad();
                        double slope = currentSysCpuLoad - latestSysCpuLoadCC_;
                        ALOGI("SocDaemon: CC : ExitDebounceTimer Expired with SysCpuLoad=%f latestSysCpuLoadCC_=%f slope=%f",
                                currentSysCpuLoad, latestSysCpuLoadCC_, slope);

                        if (slope > kSysloadSlopeThreshold) {
                            CCGlobalState prev = CCGlobalState_.exchange(CCGlobalState::Open);
                            if (prev != CCGlobalState::Open) {
                                sendHintIfAllowed(0, "ExitDebounceTimerExpired");
                            } else {
                                ALOGI("SocDaemon: Already in Open after exit debounce (no action)");
                            }
                        } else {
                            ALOGI("SocDaemon: SysLoad has not increased. Restart ExitDebounceTimer");
                            startCCExitDebounceTimer(std::chrono::milliseconds(5000));
                        }

                    } else {
                        ALOGI("SocDaemon: Exit debounce expired but not in CoreContainment (no action)");
                    }
                    lock.lock();
                } else {
                    ccExitDebounceCancelled_ = false;
                }
            }
        } // inner while for active timers
    }     // outer while true
}

// Debounce control helpers
void SocDaemon::startCCEntryDebounceTimer() noexcept {
    {
        std::lock_guard<std::mutex> lock(debounceMutex_);
        ccEntryDebounceStartTime_ = std::chrono::steady_clock::now();
        ccEntryDebounceActive_ = true;
        ccEntryDebounceCancelled_ = false;
       debounceThreadStarted_ = true;
    }
    debounceCv_.notify_one();
}

void SocDaemon::stopCCEntryDebounceTimer() noexcept {
ALOGI("SocDaemon: EntryDebounceTimer Stopped");
{
    std::lock_guard<std::mutex> lock(debounceMutex_);
        ccEntryDebounceActive_ = false;
        ccEntryDebounceCancelled_ = true;
    }
    debounceCv_.notify_one();
}

bool SocDaemon::isCCEntryDebounceTimerRunning() const noexcept {
    return ccEntryDebounceActive_.load();
}

void SocDaemon::startCCExitDebounceTimer(std::chrono::milliseconds timeout) noexcept {
    {
        std::lock_guard<std::mutex> lock(debounceMutex_);
        ccExitDebounceActive_ = true;
        ccExitDebounceCancelled_ = false;
        debounceThreadStarted_ = true;
        ccExitDebounceMs_ = timeout;
    }
    debounceCv_.notify_one();
}

void SocDaemon::stopCCExitDebounceTimer() noexcept {
ALOGI("SocDaemon: ExitDebounceTimer Stopped");
{
    std::lock_guard<std::mutex> lock(debounceMutex_);
        ccExitDebounceActive_ = false;
        ccExitDebounceCancelled_ = true;
    }
    debounceCv_.notify_one();
}

bool SocDaemon::isCCExitDebounceTimerRunning() const noexcept {
    return ccExitDebounceActive_.load();
}

// Monitor callbacks
void SocDaemon::handleChangeAlert(const std::string& name, int oldValue, int newValue) {

    if (name == "WltMonitor") {
        ALOGI("SocDaemon: New WLT=%d", newValue);
        // ERIN TO DO -- WHAT ARE WE GOING TO DO with sysload here

            if (socHint_ == "wlt") {
                WltType newWLT = static_cast<WltType>(newValue & 0x3);
                WltType oldWLT = static_cast<WltType>(oldValue & 0x3);
                /* // Continue GPU monitoring for Sustain/Bursty, pause only for Idle/Btl
                if ((oldWLT == WltType::Idle || oldWLT == WltType::Btl) &&
                    (newWLT == WltType::Sustain || newWLT == WltType::Bursty)) {
                        ALOGD("DEEPIKA SocDaemon: WLT changed from IDLE/BTL to SUSTAIN/BURSTY");
                    if (!gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                        // Start thread if not running
                        ALOGD("DEEPIKA SocDaemon: Starting GpuRc6Monitor thread for WLT Sustain/Bursty");
                        if (pthread_create(&gpuMonitorThread_, NULL, SocDaemon::monitorSysfsWrapper, gpuRc6MonitorPtr_) == 0) {
                            gpuMonitorThreadRunning_ = true;
                            ALOGI("DEEPIKA SocDaemon: Started GpuRc6Monitor thread for WLT Sustain/Bursty");
                            // Always resume after thread start in case monitor was paused
                            gpuRc6MonitorPtr_->resume();
                            ALOGI("DEEPIKA SocDaemon: Resumed GpuRc6Monitor polling for WLT Sustain/Bursty (after thread start)");
                        } else {
                            ALOGE("DEEPIKA SocDaemon: Failed to start GpuRc6Monitor thread: %s", std::strerror(errno));
                        }
                    } else if (gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                        // Resume polling if thread is already running
                        gpuRc6MonitorPtr_->resume();
                        ALOGI("DEEPIKA SocDaemon: Resumed GpuRc6Monitor polling for WLT Sustain/Bursty");
                    }
                } else if (newWLT == WltType::Idle || newWLT == WltType::Btl) {
                    // Pause GPU monitor polling if running
                    if (gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                        gpuRc6MonitorPtr_->pause();
                        ALOGI("SocDaemon: Paused GpuRc6Monitor polling for WLT Idle/Btl");
                    }
                } */
                

                if (CCGlobalState_.load() == CCGlobalState::CoreContainment) {
                    // We're in CoreContainment

                    if ((oldWLT == WltType::Idle || oldWLT == WltType::Btl) &&
                        (newWLT == WltType::Sustain || newWLT == WltType::Bursty)) {
                        ALOGI("SocDaemon: CC : WLT changed from IDLE/BTL to SUSTAIN/BURSTY. Resetting latestSysCpuLoadCC_");
                        latestSysCpuLoadCC_ = getLatestSysCpuLoad();
                        if (!gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                        // Start thread if not running
                            if (pthread_create(&gpuMonitorThread_, NULL, SocDaemon::monitorSysfsWrapper, gpuRc6MonitorPtr_) == 0) {
                                gpuMonitorThreadRunning_ = true;
                                ALOGI("DEEPIKA SocDaemon: Started GpuRc6Monitor thread for WLT Sustain/Bursty");
                                // Always resume after thread start in case monitor was paused
                                gpuRc6MonitorPtr_->resume();
                                ALOGI("DEEPIKA SocDaemon: Resumed GpuRc6Monitor polling for WLT Sustain/Bursty (after thread start)");
                            } else {
                                ALOGE("DEEPIKA SocDaemon: Failed to start GpuRc6Monitor thread: %s", std::strerror(errno));
                            }
                        } else if (gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                        // Resume polling if thread is already running
                        gpuRc6MonitorPtr_->resume();
                        ALOGI("DEEPIKA SocDaemon: Resumed GpuRc6Monitor polling for WLT Sustain/Bursty");
                        }
                    }

                    // Update the sysLoad value.
                    getSysCpuLoad();

                    switch (newWLT) {
                        case WltType::Idle:
                        case WltType::Btl:
                            // Idle/BTL -> ensure exit debounce is stopped
                            if (gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                                gpuRc6MonitorPtr_->pause();
                                ALOGI("SocDaemon: Paused GpuRc6Monitor polling for WLT Idle/Btl");
                                }
                            if (isCCExitDebounceTimerRunning()) {
                                ALOGI("SocDaemon: CC_ExitDT : WLT_IDLE/BTL. Cancel ExitDebounceTimer");
                                stopCCExitDebounceTimer();
                            } else {
                                ALOGI("SocDaemon: CC : WLT_IDLE/BTL : No Action");
                            }
                            break;
                        case WltType::Sustain:
                        case WltType::Bursty:
                            if (!isCCExitDebounceTimerRunning()) {
                                ALOGI("SocDaemon: CC : WLT_SUSTAIN/BURSTY. Start ExitDebounceTimer");
                                startCCExitDebounceTimer();
                            }
                            if (gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                                gpuRc6MonitorPtr_->resume();
                                ALOGI("SocDaemon: Resumed GpuRc6Monitor polling for WLT Sustain/Bursty");
                            }
                            break;
                        default:
                            ALOGD("SocDaemon: CC : Unknown WLT state %d", newWLT);
                            break;
                    }

                } else {
                    // Not in CoreContainment
                    // If an entry debounce is running and we see sustain, cancel the entry debounce
                    switch (newWLT) {
                        case WltType::Idle:
                        case WltType::Btl:
                            if ((CCGlobalState_.load() == CCGlobalState::Open) && !isCCEntryDebounceTimerRunning()) {
                                ALOGI("SocDaemon: Open : WLT_IDLE/BTL : EntryDebounceTimer Started");
                                startCCEntryDebounceTimer();
                            } else {
                                ALOGI("SocDaemon: Open : WLT_IDLE/BTL : EntryDebounceTimer already running or not in Open");
                            }
                            if (gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                                gpuRc6MonitorPtr_->pause();
                                ALOGI("SocDaemon: Paused GpuRc6Monitor polling for WLT Idle/Btl");
                            }
                            break;
                        case WltType::Sustain:
                            if (isCCEntryDebounceTimerRunning()) {
                                ALOGI("SocDaemon: Open_EntryDT : WLT_SUSTAIN : Cancel EntryDebounceTimer");
                                stopCCEntryDebounceTimer();
                            } else {
                                ALOGI("SocDaemon: Open : WLT_SUSTAIN : No EntryDebounceTimer running");
                            }
                            if (gpuMonitorThreadRunning_ && gpuRc6MonitorPtr_) {
                                gpuRc6MonitorPtr_->resume();
                                ALOGI("SocDaemon: Resumed GpuRc6Monitor polling for WLT Sustain/Bursty");
                            }
                            break;
                        case WltType::Bursty:
                            ALOGI("SocDaemon: Open : WLT_BURSTY (no action)");
                            break;
                        default:
                            ALOGD("SocDaemon: Open : Unknown WLT state %d", newWLT);
                            break;
                    }
                }
            } else if (socHint_ == "swlt") {
                if (!(newValue & (1 << 4))) {
                    sendHintIfAllowed(0, "SWLT is Performance");
                } else {
                    sendHintIfAllowed(1, "SWLT is Power");
                }
            }
        }

        if (name == "HfiMonitor") {
            if (newValue == 255) {
                sendHintIfAllowed(1, "HFI Efficient Power Mode is 255");
            } else {
                sendHintIfAllowed(0, "HFI Efficient Power Mode is not 255");
            }
        }

        if (name == "SysLoadMonitor") {
            // SysLoadMonitor change alert: newValue is the smoothed CPU load percentage
            double cpuLoad = static_cast<double>(newValue);
            ALOGI("SocDaemon: SysLoadMonitor ALERT: CPU load changed to %f", cpuLoad);
            // If in CoreContainment and CPU load rises above high threshold, start exit debounce
            CCGlobalState prev = CCGlobalState_.exchange(CCGlobalState::Open);
            if (prev != CCGlobalState::CoreContainment) {
                sendHintIfAllowed(0, "HighSysLoadInCC");
            }
        }

        if (name == "GpuRc6Monitor") {
            // GpuRc6Monitor change alert: newValue is the gfxMode (0=normal, 1=high load)
            //ALOGI("SocDaemon: GpuRc6Monitor ALERT: GfxMode changed to %d", newValue);
            if (newValue == 1) {
                ALOGI("SocDaemon: GpuRc6Monitor ALERT: GfxMode changed to %d, High GPU load detected", newValue);
                sendGfxHintIfAllowed(1, "High GPU load detected");
            } else {
                ALOGI("SocDaemon: GpuRc6Monitor ALERT: GfxMode changed to %d, Low GPU load detected", newValue);
                sendGfxHintIfAllowed(0, "Low GPU load detected");
            }
        }
    }

double SocDaemon::getSysCpuLoad() const noexcept {
    // Use the non-owning pointer to the SysLoadMonitor (set during construction) to avoid RTTI/dynamic_cast.
    if (sysLoadMonitorPtr_) {
        return sysLoadMonitorPtr_->getSysCpuLoad();
    }
    return -1.0;
}

double SocDaemon::getLatestSysCpuLoad() const noexcept {
    // Use the non-owning pointer to the SysLoadMonitor (set during construction) to avoid RTTI/dynamic_cast.
    if (sysLoadMonitorPtr_) {
        return sysLoadMonitorPtr_->getLatestSysCpuLoad();
    }
    return -1.0;
}

void SocDaemon::sendHintIfAllowed(int value, const char* reason) {
    if (value != efficientMode_) {
        if (sendHint_) {
            hintManager.sendHint("EFFICIENT_POWER", value);
                ALOGI("SocDaemon: Send EFFICIENT_POWER: %d due to %s", value, reason);
            } else {
                ALOGI("SocDaemon: %s but not sending due to sendHint=false", reason);
            }
            efficientMode_ = value;

            if (efficientMode_) {
                if (sysLoadMonitorPtr_) sysLoadMonitorPtr_->restart();
            } else {
                if (sysLoadMonitorPtr_) sysLoadMonitorPtr_->pause();
            }
        } else {
            ALOGD("SocDaemon: Hint value unchanged (%d), not sending: %s", value, reason);
    }
}

void SocDaemon::sendGfxHintIfAllowed(int value, const char* reason) {
    if (value != gfxMode_) {
        if (sendGfxHint_) {
            hintManager.sendHint("GFX_MODE", value);
                ALOGI("SocDaemon: Send GFX_MODE: %d due to %s", value, reason);
            } else {
                ALOGI("SocDaemon: %s but not sending due to sendGfxHint=false", reason);
            }
            gfxMode_ = value;
        } else {
            ALOGD("SocDaemon: GFX Hint value unchanged (%d), not sending: %s", value, reason);
    }
}

// Static wrapper for pthreads_
void* SocDaemon::monitorSysfsWrapper(void* arg) noexcept {
    HintMonitor* monitor = static_cast<HintMonitor*>(arg);
    monitor->monitorLoop();
    return nullptr;
}
