#ifndef HINT_MANAGER_H
#define HINT_MANAGER_H

#include <aidl/android/hardware/power/IPower.h>
#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>
#include <android/binder_manager.h>
#include <android/binder_status.h>

#include <memory>   // For std::shared_ptr
#include <string>   // For std::string
#include <utility>  // For std::move

// Required for Android logging
#include <android/log.h>

// Define a tag for HintManager's log messages
#define HINT_LOG_TAG "SocDameon_HintManger"
#define HMLOGI(...) __android_log_print(ANDROID_LOG_INFO, HINT_LOG_TAG, __VA_ARGS__)
#define HMLOGE(...) __android_log_print(ANDROID_LOG_ERROR, HINT_LOG_TAG, __VA_ARGS__)

using ::aidl::android::hardware::power::IPower;
using ::aidl::google::hardware::power::extension::pixel::IPowerExt;

/**
 * @brief Manages connection to the Power HAL Extension and sends power hints.
 * This class handles establishing and maintaining a connection to the IPowerExt
 * AIDL service, allowing the daemon to send specific power modes.
 */
class HintManager {
public:
    /**
     * @brief Constructor for HintManager. Attempts to connect to the Power HAL Extension.
     * The connection status can be checked using isConnected().
     */
    HintManager(); // Declare the default constructor here

    /**
     * @brief Checks if the HintManager is successfully connected to the Power HAL Extension.
     * @return true if connected, false otherwise.
     */
    bool isPowerHalConnected() const;

    /**
     * @brief Sends a power mode hint to the Power HAL Extension.
     * @param type The type of power mode to set (e.g., "CPU_BOOST", "INTERACTION").
     * @param enable True to enable the mode, false to disable.
     * @return true if the mode was successfully set, false otherwise.
     */
    bool sendHint(const std::string &type, const bool &enable);

private:
    // Shared pointer to the IPowerExt AIDL interface.
    // This will manage the lifecycle of the connection.
    std::shared_ptr<IPowerExt> power_ext_hal_;
};

#endif // HINT_MANAGER_H

