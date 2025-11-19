#pragma once

#include <string>
#include <functional>
#include <android/log.h>
#include <linux/genetlink.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>

#include "HintMonitor.h"
// Logging macros for HfiMonitor
#define HFI_MONITOR_LOG_TAG "SocDaemon_HfiMonitor"
#define HFILOGI(...) __android_log_print(ANDROID_LOG_INFO, HFI_MONITOR_LOG_TAG, __VA_ARGS__)
#define HFILOGE(...) __android_log_print(ANDROID_LOG_ERROR, HFI_MONITOR_LOG_TAG, __VA_ARGS__)

/**
 * @brief Monitors HFI
 */
class HfiMonitor : public HintMonitor {
public:
    HfiMonitor(const std::string& hintName) : HintMonitor(hintName) {};

    virtual ~HfiMonitor() = default;

    void monitorLoop() override;
    
    int init();

private:
    struct nl_sock *hfi_sock_;
    int efficient_power = 0; // Default efficient power mode

    static int event_handler(struct nl_msg *msg, void *arg) {
         // Cast the 'arg' back to our class instance
        HfiMonitor* self = static_cast<HfiMonitor*>(arg);
        self->process_message(msg);
        return 0;
    }
    
    void process_message(struct nl_msg *msg);
};
