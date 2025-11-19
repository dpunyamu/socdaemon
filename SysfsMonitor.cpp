// -----------------------------------------------------------------------------
// SysfsMonitor.cpp
//
// This file implements the SysfsMonitor base class, which provides a generic
// mechanism for monitoring a single sysfs entry on Linux-based systems.
// The class is designed to be inherited by other classes that require custom
// behavior when a sysfs value changes. It uses polling and file operations to
// detect changes in the sysfs entry and calls a virtual callback when a change
// is detected.
//
// Usage:
//   - Inherit from SysfsMonitor and override onValueChanged() for custom logic.
//   - Call monitorLoop() to start monitoring in a thread or task.
//
// Author: (Your Name)
// -----------------------------------------------------------------------------

#include "SysfsMonitor.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <poll.h>
#include <chrono>
#include <thread>

/**
 * @brief Constructor for SysfsMonitor.
 * @param name Name for the monitor instance.
 * @param sysfsPath Path to the sysfs file to monitor.
 * @param pollTimeoutMs Polling timeout in milliseconds.
 */
SysfsMonitor::SysfsMonitor(const std::string& name, const std::string& sysfsPath, int pollTimeoutMs)
    : name_(name), sysfs_path_(sysfsPath), poll_timeout_ms_(pollTimeoutMs) {
    SYSFSLOGI("SysfsMonitor: Initializing '%s' for '%s' with poll timeout %dms",
              name_.c_str(), sysfs_path_.c_str(), poll_timeout_ms_);
}

/**
 * @brief Reads the sysfs value once (convenience for initial read).
 * @param value_out Output string for the value read.
 * @return true if read was successful, false otherwise.
 */
bool SysfsMonitor::readValueOnce(std::string& value_out) {
    int fd = open(sysfs_path_.c_str(), O_RDONLY);

    char buffer[kSysfsReadBufferSize];
    ssize_t bytes_read = read(fd, buffer, kSysfsReadBufferSize - 1);
    close(fd);

    if (bytes_read < 0) {
        SYSFSLOGE("SysfsMonitor: Could not read from '%s': %s",
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
bool SysfsMonitor::readValue(int fd, std::string& value_out) {
    char buffer[kSysfsReadBufferSize];
    ssize_t bytes_read = read(fd, buffer, kSysfsReadBufferSize - 1);

    if (bytes_read < 0) {
        SYSFSLOGE("SysfsMonitor: Could not read from '%s': %s",
                  sysfs_path_.c_str(), std::strerror(errno));
        return false;
    } else {
        buffer[bytes_read] = '\0';
        buffer[strcspn(buffer, "\n")] = '\0';
        value_out = buffer;
    }
    SYSFSLOGI("SysfsMonitor read_sysfs_value byte=%zd", bytes_read);
    return true;
}

/**
 * @brief Main monitoring loop. Calls onValueChanged when value changes.
 *        Intended to be run in a thread.
 */
void SysfsMonitor::monitorLoop()
{
    SYSFSLOGI("SysfsMonitor: Starting monitoring loop for '%s'", sysfs_path_.c_str());

    int fd;
    struct pollfd pfd;
    std::string current_value;
    std::string previous_value;
    std::string temp_value;

    // Initial read of the sysfs value
    readValueOnce(current_value);

    SYSFSLOGI("SysfsMonitor: Initial value of '%s' is '%s'", sysfs_path_.c_str(), current_value.c_str());

    // Polling loop to monitor for changes
    while (true) {
        fd = open(sysfs_path_.c_str(), O_RDONLY);
        if (fd < 0) {
            SYSFSLOGE("SysfsMonitor: Could not open '%s' for reading: %s",
                      sysfs_path_.c_str(), std::strerror(errno));
            return;
        }

        readValue(fd, current_value);

        if (current_value != previous_value) {
            SYSFSLOGI("SysfsMonitor: previous_value '%s', current value '%s' changed.", previous_value.c_str(), current_value.c_str());
            onValueChanged(previous_value, current_value);
            previous_value = current_value;
        }

        pfd.fd = fd;
        pfd.events = POLLPRI | POLLERR;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, poll_timeout_ms_);

        if (ret < 0) {
            SYSFSLOGE("SysfsMonitor: poll() failed for '%s': %s", sysfs_path_.c_str(), std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            SYSFSLOGI("SysfsMonitor poll event=%d: pdf.revents=%d", ret, pfd.revents);
            if (pfd.revents == (POLLPRI | POLLERR)) {
                readValue(fd, temp_value);
                // No action here, value will be checked on next loop
            } else {
                SYSFSLOGI("SysfsMonitor: Poll event on '%s', but value '%s' unchanged.",
                          sysfs_path_.c_str(), current_value.c_str());
            }
        }
        close(fd);
    }
}