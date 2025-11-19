/**
 * @file HfiMonitor.cpp
 * @brief Implementation of the HfiMonitor class for monitoring thermal events via netlink sockets.
 *
 * This file contains the implementation of the HfiMonitor class, which is responsible for
 * initializing a netlink socket, subscribing to thermal event notifications, and processing
 * received messages related to CPU thermal capabilities. The monitor listens for events
 * such as CPU capability changes and updates the power mode accordingly.
 *
 * Key Functions:
 * - HfiMonitor::init(): Initializes the netlink socket, connects to the generic netlink family,
 *   subscribes to the thermal event multicast group, and sets up the message callback.
 * - HfiMonitor::monitorLoop(): Runs a blocking loop to receive and dispatch netlink messages
 *   to the registered callback handler.
 * - HfiMonitor::process_message(): Processes incoming netlink messages, parses thermal event
 *   attributes, and updates the efficient power mode if necessary.
 *
 * Error Handling:
 * - Each step of the socket initialization and message handling includes error checking and
 *   appropriate resource cleanup to ensure robustness.
 *
 * Dependencies:
 * - Linux kernel headers for netlink and thermal events.
 * - External logging macros/functions (e.g., HFILOGE, LOG).
 * - PowerHalService for updating power modes.
 *
 * Thread Safety:
 * - The monitorLoop() function is designed to run in a dedicated thread, blocking on message
 *   reception and dispatching events as they arrive.
 *
 * Usage:
 * 1. Create an instance of HfiMonitor.
 * 2. Call init() to set up the netlink socket and event subscription.
 * 3. Start monitorLoop() in a separate thread to begin processing events.
 *
 * Note:
 * - The process_message() function currently focuses on handling CPU capability change events,
 *   but can be extended to support additional thermal event types as needed.
 */
#include <linux/netlink.h>
#include <linux/thermal.h>

#include "HfiMonitor.h"

int HfiMonitor::init()
{
    // Allocate a new netlink socket for communication with the kernel
    hfi_sock_ = nl_socket_alloc();
    if (!hfi_sock_) {
        HFILOGE("nl_socket_alloc failed");
        return -1;
    }

    // Connect the socket to the generic netlink family
    if (genl_connect(hfi_sock_) < 0) {
        HFILOGE("genl_connect failed: hfi_sock_");
        nl_socket_free(hfi_sock_);
        hfi_sock_ = nullptr;
        return -1;
    }

    // Resolve the multicast group ID for thermal events (e.g., "thermal_event")
    int group_id = genl_ctrl_resolve_grp(hfi_sock_, THERMAL_GENL_FAMILY_NAME, THERMAL_GENL_EVENT_GROUP_NAME);
    if (group_id < 0) {
        HFILOGE("Failed to get multicast ID %s for hfi_monitor", strerror(-group_id));
        nl_socket_free(hfi_sock_);
        hfi_sock_ = nullptr;
        return -1;
    }

    // Subscribe the socket to the resolved multicast group to receive thermal events
    if (nl_socket_add_membership(hfi_sock_, group_id) < 0) {
        HFILOGE("Failed to add netlink socket membership: %s", THERMAL_GENL_EVENT_GROUP_NAME);
        nl_socket_free(hfi_sock_);
        hfi_sock_ = nullptr;
        return -1;
    }

    // Register a custom callback handler for incoming netlink messages
    // NL_CB_MSG_IN: Callback type for incoming messages
    // NL_CB_CUSTOM: Use a user-defined callback function
    // event_handler: The callback function to handle messages
    // this: Pass the current HfiMonitor instance as user data
    nl_socket_modify_cb(hfi_sock_, NL_CB_MSG_IN, NL_CB_CUSTOM, event_handler, this);

    // Disable sequence number checking for multicast/event sockets
    nl_socket_disable_seq_check(hfi_sock_);

    return 0; // Return 0 on successful initialization
}

void HfiMonitor::monitorLoop() {

     while (true) {
            // This function blocks, receives messages, and dispatches them to our callback
            int res = nl_recvmsgs_default(hfi_sock_);
            if (res < 0) {
                 // An error like -NLE_INTR (Interrupted system call) is not fatal.
                 // You might want more sophisticated error handling here.
                 HFILOGE("Failed to receive netlink messages: %s", nl_geterror(res));
            }
        }

}

void HfiMonitor::process_message(struct nl_msg *msg) {
    struct nlmsghdr *nlh = nlmsg_hdr(msg);
    struct genlmsghdr *glh = genlmsg_hdr(nlh);
    struct nlattr *attrs[THERMAL_GENL_ATTR_MAX + 1];

    genlmsg_parse(nlh, 0, attrs, THERMAL_GENL_ATTR_MAX, NULL);

    switch (glh->cmd) {
        case THERMAL_GENL_EVENT_CPU_CAPABILITY_CHANGE : {
            struct nlattr *cpu_cap;
            int j = 0;
            int index = 0;
            int cpu = 0;
            int perf = 0;
            int eff = 0;

            HFILOGE("THERMAL_GENL_EVENT_CPU_CAPABILITY_CHANGE cmd");

            nla_for_each_nested(cpu_cap, attrs[THERMAL_GENL_ATTR_CPU_CAPABILITY], j) {
                if (cpu_cap == NULL) {
                    HFILOGE("THERMAL_GENL_EVENT_CPU_CAPABILITY_CHANGE : nla_for_each_nested failed");
                    continue;
                }

                switch (index) {
                    case 0:
                        cpu = nla_get_u32(cpu_cap);
                        break;
                    case 1:
                        perf = nla_get_u32(cpu_cap) >> 2; // scale down to original [0-255] range
                        break;
                    case 2:
                        eff = nla_get_u32(cpu_cap) >> 2; // scale down to original [0-255] range
                        break;
                    default:
                        break;
                }
                ++index;
                if (index == 3) {
                    index = 0;
                }
            }
            // Track EFFICIENT_POWER_MODE globally and call setMode only on change
            HFILOGI("JHS cpu=%d perf=%d eff=%d", cpu, perf, eff);
                     
            if (efficient_power != eff) {
                onValueChanged(efficient_power, eff);
                efficient_power = eff;
            }

            break;
        }
        // ... (all other cases unchanged) ...
        default:
            HFILOGE("JHS Unknown genlink event command:%x", glh->cmd);
    }
}
