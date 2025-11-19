#pragma once

// SysfsMonitor.h
// -----------------------------------------------------------------------------
// This header defines the SysfsMonitor base class, which provides a generic
// interface for monitoring a single sysfs entry on Linux-based systems.
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

#include <string>
#include <android/log.h>
#include <functional>

// Logging macros for SysfsMonitor
#define SYSFS_MONITOR_LOG_TAG "SysfsMonitor"
#define SYSFSLOGI(...) __android_log_print(ANDROID_LOG_INFO, SYSFS_MONITOR_LOG_TAG, __VA_ARGS__)
#define SYSFSLOGE(...) __android_log_print(ANDROID_LOG_ERROR, SYSFS_MONITOR_LOG_TAG, __VA_ARGS__)

/**
 * @brief Generic base class for monitoring a single sysfs entry.
 *        Inherit and override onValueChanged() for custom behavior.
 *
 * This class provides methods to read the value of a sysfs file and a polling
 * loop to detect changes. When a change is detected, the onValueChanged()
 * callback is invoked. The class is intended to be used as a base class.
 */
class SysfsMonitor {
public:
    /**
     * @brief Constructor.
     * @param name Unique name for this monitor.
     * @param sysfsPath Path to the sysfs file to monitor.
     * @param pollTimeoutMs Polling timeout in milliseconds.
     */
    SysfsMonitor(const std::string& name, const std::string& sysfsPath, int pollTimeoutMs);

    // Disable copy and assignment
    SysfsMonitor(const SysfsMonitor&) = delete;
    SysfsMonitor& operator=(const SysfsMonitor&) = delete;

    virtual ~SysfsMonitor() = default;

    /**
     * @brief Reads the sysfs value once (convenience for initial read).
     * @param value_out Output string for the value read.
     * @return true if read was successful, false otherwise.
     */
    bool readValueOnce(std::string& value_out);

    /**
     * @brief Reads the sysfs value from an open file descriptor.
     * @param fd Open file descriptor for the sysfs file.
     * @param value_out Output string for the value read.
     * @return true if read was successful, false otherwise.
     */
    bool readValue(int fd, std::string& value_out);

    /**
     * @brief Main monitoring loop. Calls onValueChanged when value changes.
     *        Intended to be run in a thread.
     */
    virtual void monitorLoop();

    void setAlertCallback(std::function<void(const std::string& name, const std::string& oldValue, const std::string& newValue)> cb) {
        alertCallback_ = std::move(cb);
    }

    const std::string& name() const { return name_; }

protected:
    /**
     * @brief Called when the sysfs value changes; override in derived classes.
     *
     * @param previous_value The value before the change.
     * @param current_value The value after the change.
     */
    void onValueChanged(const std::string& previous_value, const std::string& current_value) {
        if (alertCallback_) {
            alertCallback_(name_, previous_value, current_value);
        }
    }

private:
    std::string name_;         // Unique name for this monitor
    std::string sysfs_path_;   ///< Path to the sysfs file being monitored.
    int poll_timeout_ms_;      ///< Polling timeout in milliseconds.

    static constexpr size_t kSysfsReadBufferSize = 16;

    std::function<void(const std::string& name, const std::string& oldValue, const std::string& newValue)> alertCallback_;
};

