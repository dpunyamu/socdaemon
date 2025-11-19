// Add missing include for class definition
#include "GpuRc6Monitor.h"

void GpuRc6Monitor::stop() {
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        shouldExit_ = true;
        paused_ = false;
    }
    pauseCv_.notify_one();
    GPULOGI("GpuRc6Monitor: Stop requested, exiting monitor loop");
}
// -----------------------------------------------------------------------------
// GpuRc6Monitor.cpp
//
// This file implements the GpuRc6Monitor base class, which provides a generic
// mechanism for monitoring a single sysfs entry on Linux-based systems.
// The class is designed to be inherited by other classes that require custom
// behavior when a sysfs value changes. It uses polling and file operations to
// detect changes in the sysfs entry and calls a virtual callback when a change
// is detected.
//
// Usage:
//   - Inherit from GpuRc6Monitor and override onValueChanged() for custom logic.
//   - Call monitorLoop() to start monitoring in a thread or task.
//
// Author: (Your Name)
// -----------------------------------------------------------------------------

#include "GpuRc6Monitor.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <poll.h>
#include <chrono>
#include <thread>

/**
 * @brief Constructor for WltMonitor.
 * @param name Name for the watcher instance.
 * @param sysfsPath Path to the sysfs file to monitor.
 * @param pollTimeoutMs Polling timeout in milliseconds.
 */
GpuRc6Monitor::GpuRc6Monitor(const std::string& name, const std::string& sysfsPath, int pollTimeoutMs)
    : HintMonitor(name), sysfs_path_(sysfsPath), poll_timeout_ms_(pollTimeoutMs) {
   GPULOGD("GpuRc6Monitor: Initializing '%s' for '%s' with poll timeout %dms",
              name.c_str(), sysfs_path_.c_str(), poll_timeout_ms_);
}

int GpuRc6Monitor::init() {
    // Enable workload_hint if not already enabled
    const char* gpurc6_path = "/sys/class/drm/card0/device/tile0/gt0/gtidle/idle_residency_ms";
    int fd = open(gpurc6_path, O_RDONLY);
    if (fd >= 0) {
        char buf[16] = {0};
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        if (len < 0) {
           GPULOGE("SocDaemon: Failed to read from %s: %s", gpurc6_path, std::strerror(errno));
            close(fd);
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Reads the sysfs value once (convenience for initial read).
 * @param value_out Output string for the value read.
 * @return true if read was successful, false otherwise.
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
 */
 
bool GpuRc6Monitor::readValueOnce(std::string& value_out) {
    int fd = open(sysfs_path_.c_str(), O_RDONLY);

    char buffer[kSysfsReadBufferSize];
    ssize_t bytes_read = read(fd, buffer, kSysfsReadBufferSize - 1);
    close(fd);

    if (bytes_read < 0) {
       GPULOGE("GpuRc6Monitor: Could not read from '%s': %s",
                  sysfs_path_.c_str(), std::strerror(errno));
        return false;
    }

    buffer[bytes_read] = '\0';
    buffer[strcspn(buffer, "\n")] = '\0';
    value_out = buffer;
    return true;
}

/**
 * @brief Reads the sysfs value from an open file descriptor.
 * @param fd Open file descriptor for the sysfs file.
 * @param value_out Output string for the value read.
 * @return true if read was successful, false otherwise.
 */
bool GpuRc6Monitor::readValue(int fd, std::string& value_out) {
    char buffer[kSysfsReadBufferSize];
    ssize_t bytes_read = read(fd, buffer, kSysfsReadBufferSize - 1);

    if (bytes_read < 0) {
       GPULOGE("GpuRc6Monitor: Could not read from '%s': %s",
                  sysfs_path_.c_str(), std::strerror(errno));
        return false;
    } else {
        buffer[bytes_read] = '\0';
        buffer[strcspn(buffer, "\n")] = '\0';
        value_out = buffer;
    }
   GPULOGD("GpuRc6Monitor read_sysfs_value byte=%zd", bytes_read);
    return true;
}

/**
 * @brief Main monitoring loop. Calls onValueChanged when value changes.
 *        Intended to be run in a thread.
 */
void GpuRc6Monitor::monitorLoop()
{
   GPULOGD("GpuRc6Monitor: Starting monitoring loop for '%s'", sysfs_path_.c_str());

     int fd;
     struct pollfd pfd;
     std::string current_value;
     std::string previous_value;
     std::string temp_value;
     int gfxMode = 0;
     // Initial read of the sysfs value
     readValueOnce(current_value);

     GPULOGD("GpuRc6Monitor: Initial value of '%s' is '%s'", sysfs_path_.c_str(), current_value.c_str());

    // Polling loop to monitor for changes
    std::unique_lock<std::mutex> lock(pauseMutex_);
    while (!shouldExit_) {
        pauseCv_.wait(lock, [this]{ return !paused_ || shouldExit_; });
        if (shouldExit_) break;
        // Only poll when not paused
        lock.unlock();
        fd = open(sysfs_path_.c_str(), O_RDONLY);
        if (fd < 0) {
            GPULOGE("GpuRc6Monitor: Could not open '%s' for reading: %s",
                   sysfs_path_.c_str(), std::strerror(errno));
            lock.lock();
            continue;
        }

        readValue(fd, current_value);

        if (current_value != previous_value) {
            int prev_val = 0, curr_val = 0;
            char* endptr_prev = nullptr;
            prev_val = std::strtol(previous_value.c_str(), &endptr_prev, 10);
            if (endptr_prev == previous_value.c_str() || *endptr_prev != '\0') {
                GPULOGE("GpuRc6Monitor: Failed to convert previous_value '%s' to int", previous_value.c_str());
            }
            char* endptr_curr = nullptr;
            curr_val = std::strtol(current_value.c_str(), &endptr_curr, 10);
            if (endptr_curr == current_value.c_str() || *endptr_curr != '\0') {
                GPULOGE("GpuRc6Monitor: Failed to convert current_value '%s' to int", current_value.c_str());
            }
            unsigned long long delta_idle = 0;
            if (curr_val >= prev_val) {
                delta_idle = static_cast<unsigned long long>(curr_val - prev_val);
            }
            double percent = (double)delta_idle * 100.0 / 1000.0;
            double idle_percent = (percent > 100.0 ? 100.0 : percent);

            if (idle_percent <= kGpuHighLoadPercent) {
                GPULOGI("GpuRc6Monitor: High GPU load detected, GPU is %f%% idle", idle_percent);
                gfxMode = 1; // High load mode
                onValueChanged(static_cast<int>(idle_percent), gfxMode);
            }
            else {
                gfxMode = 0; // Normal load mode
                onValueChanged(static_cast<int>(idle_percent), gfxMode);
            }
            previous_value = current_value;
        }

        pfd.fd = fd;
        pfd.events = POLLPRI | POLLERR;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, poll_timeout_ms_);

        if (ret < 0) {
            GPULOGE("GpuRc6Monitor: poll() failed for '%s': %s", sysfs_path_.c_str(), std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        close(fd);
        lock.lock();
    }

}

void GpuRc6Monitor::onValueChanged(int previous_value, int current_value) {
     //GPULOGI("GpuRc6Monitor: GfxMode changed idle_res_percent %d gfxmode %d", previous_value, current_value);
     HintMonitor::onValueChanged(previous_value, current_value);
}

void GpuRc6Monitor::pause() {
    std::lock_guard<std::mutex> lock(pauseMutex_);
    paused_ = true;
    pauseCv_.notify_one();
    GPULOGI("GpuRc6Monitor: Paused sysfs polling (notified thread)");
}

void GpuRc6Monitor::resume() {
    {
        std::lock_guard<std::mutex> lock(pauseMutex_);
        paused_ = false;
    }
    pauseCv_.notify_one();
    GPULOGI("GpuRc6Monitor: Resumed sysfs polling (notified thread)");
}