#include "HintManager.h" // Include your header file first

// Definition of the HintManager constructor
HintManager::HintManager() {
    // Attempt to connect to the Power HAL Extension service.
    const std::string kInstance = std::string(IPower::descriptor) + "/default";
    ndk::SpAIBinder power_binder = ndk::SpAIBinder(AServiceManager_getService(kInstance.c_str()));
    ndk::SpAIBinder ext_power_binder;

    if (power_binder.get() == nullptr) {
        HMLOGE("HintManager: Cannot get Power Hal Binder for instance '%s'", kInstance.c_str());
        return; // Connection failed
    }

    // Try to get the extension interface from the main IPower binder.
    if (STATUS_OK != AIBinder_getExtension(power_binder.get(), ext_power_binder.getR()) ||
        ext_power_binder.get() == nullptr) {
        HMLOGE("HintManager: Cannot get Power Hal Extension Binder from main HAL.");
        return; // Connection failed
    }

    // Convert the AIBinder to the AIDL interface shared_ptr.
    power_ext_hal_ = IPowerExt::fromBinder(ext_power_binder);
    if (power_ext_hal_ == nullptr) {
        HMLOGE("HintManager: Cannot get Power Hal Extension AIDL interface.");
        return; // Connection failed
    }

    HMLOGI("HintManager: Successfully connected to Power HAL Extension.");
}

bool HintManager::isPowerHalConnected() const
{
    return power_ext_hal_ != nullptr;
}
 

// Definition of setMode() member function
bool HintManager::sendHint(const std::string &type, const bool &enable) {
    if (!isPowerHalConnected()) {
        HMLOGE("Not connected to Power HAL Extension. Cannot set mode '%s'.", type.c_str());
        return false;
    }

    // Call the AIDL interface method. Check if the transaction was successful.
    if (!power_ext_hal_->setMode(type, enable).isOk()) {
        HMLOGE("Fail to send hint. Mode: '%s' enabled: %d", type.c_str(), enable);
        return false;
    } else {
        HMLOGI("Successfully send hint. Mode: '%s' enabled: %d", type.c_str(), enable);
        return true;
    }
}

