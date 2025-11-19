// -----------------------------------------------------------------------------
// WltMonitor.cpp
//
// This file implements the WltMonitor base class, which provides a generic
// mechanism for monitoring a single sysfs entry on Linux-based systems.
// The class is designed to be inherited by other classes that require custom
// behavior when a sysfs value changes. It uses polling and file operations to
// detect changes in the sysfs entry and calls a virtual callback when a change
// is detected.
//
// Usage:
//   - Inherit from WltMonitor and override onValueChanged() for custom logic.
//   - Call monitorLoop() to start monitoring in a thread or task.
//
// Author: (Your Name)
// -----------------------------------------------------------------------------

#include "WltMonitor.h"
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
WltMonitor::WltMonitor(const std::string& name, const std::string& sysfsPath, int pollTimeoutMs, int notificationDelay)
    : HintMonitor(name), sysfs_path_(sysfsPath), poll_timeout_ms_(pollTimeoutMs), notificationDelay_(notificationDelay) {
   WLTLOGD("WltMonitor: Initializing '%s' for '%s' with poll timeout %dms and notificationDelay %d",
              name.c_str(), sysfs_path_.c_str(), poll_timeout_ms_, notificationDelay_);
}

int WltMonitor::init() {
    // Enable workload_hint if not already enabled
    const char* enable_path = "/sys/devices/pci0000:00/0000:00:04.0/workload_hint/workload_hint_enable";
    int fd = open(enable_path, O_RDWR);
    if (fd >= 0) {
        char buf[16] = {0};
        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        if (len < 0) {
           WLTLOGE("SocDaemon: Failed to read from %s: %s", enable_path, std::strerror(errno));
            close(fd);
            return -1;
        }
        if (len > 0 && buf[0] == '0') {
            // Not enabled, so enable it
            if (lseek(fd, 0, SEEK_SET) == -1) {
               WLTLOGE("SocDaemon: Failed to seek %s: %s", enable_path, std::strerror(errno));
                close(fd);
                return -1;
            }
            if (write(fd, "1\n", 2) != 2) {
               WLTLOGE("SocDaemon: Failed to write enable to %s: %s", enable_path, std::strerror(errno));
                close(fd);
                return -1;
            } else {
               WLTLOGD("SocDaemon: Enabled workload_hint via %s", enable_path);
            }
        }
        close(fd);
    } else {
       WLTLOGE("SocDaemon: Failed to open %s: %s", enable_path, std::strerror(errno));
        return -1;
    }

    // If notificationDelay_ is set, write it to the sysfs path
    if (notificationDelay_ >= 0) {
        const char* delay_path = "/sys/devices/pci0000:00/0000:00:04.0/workload_hint/notification_delay_ms";
        int fd_delay = open(delay_path, O_WRONLY);
        if (fd_delay >= 0) {
            std::string delayStr = std::to_string(notificationDelay_) + "\n";
            ssize_t written = write(fd_delay, delayStr.c_str(), delayStr.size());
            if (written != static_cast<ssize_t>(delayStr.size())) {
               WLTLOGE("SocDaemon: Failed to write notificationDelay to %s: %s", enable_path, std::strerror(errno));
                close(fd_delay);
                return -1;
            } else {
               WLTLOGD("SocDaemon: Set notificationDelay %d to %s", notificationDelay_, enable_path);
            }
            close(fd_delay);
        } else {
           WLTLOGE("SocDaemon: Failed to open %s for notificationDelay: %s", enable_path, std::strerror(errno));
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Reads the sysfs value once (convenience for initial read).
 * @param value_out Output string for the value read.
 * @return true if read was successful, false otherwise.
 */
bool WltMonitor::readValueOnce(std::string& value_out) {
    int fd = open(sysfs_path_.c_str(), O_RDONLY);

    char buffer[kSysfsReadBufferSize];
    ssize_t bytes_read = read(fd, buffer, kSysfsReadBufferSize - 1);
    close(fd);

    if (bytes_read < 0) {
       WLTLOGE("WltMonitor: Could not read from '%s': %s",
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
bool WltMonitor::readValue(int fd, std::string& value_out) {
    char buffer[kSysfsReadBufferSize];
    ssize_t bytes_read = read(fd, buffer, kSysfsReadBufferSize - 1);

    if (bytes_read < 0) {
       WLTLOGE("WltMonitor: Could not read from '%s': %s",
                  sysfs_path_.c_str(), std::strerror(errno));
        return false;
    } else {
        buffer[bytes_read] = '\0';
        buffer[strcspn(buffer, "\n")] = '\0';
        value_out = buffer;
    }
   WLTLOGD("WltMonitor read_sysfs_value byte=%zd", bytes_read);
    return true;
}

/**
 * @brief Main monitoring loop. Calls onValueChanged when value changes.
 *        Intended to be run in a thread.
 */
void WltMonitor::monitorLoop()
{
   WLTLOGD("WltMonitor: Starting monitoring loop for '%s'", sysfs_path_.c_str());

    int fd;
    struct pollfd pfd;
    std::string current_value;
    std::string previous_value;
    std::string temp_value;

    // Initial read of the sysfs value
    readValueOnce(current_value);

   WLTLOGD("WltMonitor: Initial value of '%s' is '%s'", sysfs_path_.c_str(), current_value.c_str());

    // Polling loop to monitor for changes
    while (true) {
        fd = open(sysfs_path_.c_str(), O_RDONLY);
        if (fd < 0) {
           WLTLOGE("WltMonitor: Could not open '%s' for reading: %s",
                      sysfs_path_.c_str(), std::strerror(errno));
            return;
        }

        readValue(fd, current_value);

        if (current_value != previous_value) {
           WLTLOGD("WltMonitor: previous_value '%s', current value '%s' changed.", previous_value.c_str(), current_value.c_str());
            int prev_val = 0, curr_val = 0;
            char* endptr_prev = nullptr;
            prev_val = std::strtol(previous_value.c_str(), &endptr_prev, 10);
            if (endptr_prev == previous_value.c_str() || *endptr_prev != '\0') {
               WLTLOGE("WltMonitor: Failed to convert previous_value '%s' to int", previous_value.c_str());
            }
            char* endptr_curr = nullptr;
            curr_val = std::strtol(current_value.c_str(), &endptr_curr, 10);
            if (endptr_curr == current_value.c_str() || *endptr_curr != '\0') {
               WLTLOGE("WltMonitor: Failed to convert current_value '%s' to int", current_value.c_str());
            }
            onValueChanged(prev_val, curr_val);
            previous_value = current_value;
        }

        pfd.fd = fd;
        pfd.events = POLLPRI | POLLERR;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, poll_timeout_ms_);

        if (ret < 0) {
           WLTLOGE("WltMonitor: poll() failed for '%s': %s", sysfs_path_.c_str(), std::strerror(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
           WLTLOGD("WltMonitor poll event=%d: pdf.revents=%d", ret, pfd.revents);
            if (pfd.revents == (POLLPRI | POLLERR)) {
                readValue(fd, temp_value);
                // No action here, value will be checked on next loop
            } else {
               WLTLOGD("WltMonitor: Poll event on '%s', but value '%s' unchanged.",
                          sysfs_path_.c_str(), current_value.c_str());
            }
        }
        close(fd);
    }
}
