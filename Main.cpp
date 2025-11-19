
/**
 * Main program entry for the SocDaemon binary.
 *
 * This executable parses command-line arguments, applies validation and defaults,
 * constructs a SocDaemon instance, and starts its main processing loop.
 *
 */
#include <iostream>
#include <cstring>
#include <algorithm>
#include "SocDaemon.h"

int main(int argc, char* argv[]) {

    bool sendHint = false;
    std::string socHint;
    int notificationDelay = -1; // Default: not set

     // Parse command line arguments for --sendHint, --sochint, --notification-delay, and --help
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--sendHint") {
            if (i + 1 < argc) {
                std::string value = argv[i + 1];
                if (value == "true") {
                    sendHint = true;
                    ALOGI("--sendHint set to true");
                } else if (value == "false") {
                    sendHint = false;
                    ALOGI("--sendHint set to false");
                } else {
                    std::cout << "Invalid value for --sendHint: " << value << ". Use true or false." << std::endl;
                    exit(1);
                }
                ++i; // Skip the value
            } else {
                std::cout << "--sendHint requires a value (true or false)" << std::endl;
                exit(1);
            }
        } else if (arg == "--socHint") {
            if (i + 1 < argc) {
                socHint = argv[i + 1];
                if (socHint == "wlt" || socHint == "swlt" || socHint == "hfi") {
                    ALOGI("--sochint set to %s", socHint.c_str());
                } else {
                    std::cout << "Invalid value for --sochint: " << socHint.c_str() << std::endl;
                    socHint.clear();
                    exit(1);
                }
                ++i; // Skip the value
            } else {
                std::cout << "--socHint requires a value" << std::endl;
                exit(1);
            }
        } else if (arg == "--notification-delay") {
            if (i + 1 < argc) {
                std::string delayStr = argv[i + 1];
                bool valid = !delayStr.empty() && std::all_of(delayStr.begin(), delayStr.end(), ::isdigit);
                if (valid) {
                    notificationDelay = std::stoi(delayStr);
                    if (notificationDelay < 0) {
                        std::cout << "--notification-delay must be non-negative" << std::endl;
                        exit(1);
                    }
                    ALOGI("--notification-delay set to %d", notificationDelay);
                } else {
                    std::cout << "Invalid value for --notification-delay: " << delayStr << std::endl;
                    exit(1);
                }
                ++i; // Skip the value
            } else {
                std::cout << "--notification-delay requires a value" << std::endl;
                exit(1);
            }
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [--sendHint <true|false>] [--sochint <wlt|swlt|hfi>] [--notification-delay <ms>] [--help]\n";
            std::cout << "  --sendHint <true|false>         : Specify whether to send power hints to PowerHal (default: false)\n";
            std::cout << "  --sochint <value>               : Set SoC hint type. Allowed values: wlt, swlt, hfi\n";
            std::cout << "  --notification-delay <ms>       : Notification delay in milliseconds (only valid with wlt or swlt)\n";
            std::cout << "  --help, -h                      : Show this help message\n";
            exit(1);
        } else {
            std::cout << "Usage: " << argv[0] << " [--sendHint <true|false>] [--sochint <wlt|swlt|hfi>] [--notification-delay <ms>] [--help]\n";
            exit(1);
        }
    }

    // Validate --notification-delay usage
    if (notificationDelay >= 0 && !(socHint == "wlt" || socHint == "swlt")) {
        std::cout << "--notification-delay is only valid with --sochint wlt or swlt\n";
        exit(1);
    }

    if (socHint.empty()) {
        socHint = "wlt";
        ALOGI("--sochint not given, defaulting to %s", socHint.c_str());
    }

    SocDaemon daemon(sendHint, socHint, notificationDelay);
    daemon.start();
    return 0;
}
