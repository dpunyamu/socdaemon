#ifndef HINTMONITOR_H
#define HINTMONITOR_H

#include <string>
#include <functional>

/**
 * @file HintMonitor.h
 * @brief Generic base class for monitoring integer-valued hints or signals.
 *
 * This header declares HintMonitor, an abstract base class that provides a
 * minimal interface and common behavior for monitor implementations that
 * observe a named hint and notify observers when its integer value changes.
 *
 * Responsibilities:
 *  - Store a stable name for the monitored hint.
 *  - Provide an overridable initialization hook.
 *  - Provide an abstract monitoring loop to be implemented by derived classes.
 *  - Provide a callback mechanism to notify consumers when the value changes.
 *
 * Notes on usage:
 *  - Derived classes should implement monitorLoop(), which typically runs in a
 *    dedicated thread and calls onValueChanged(...) when a change is detected.
 *  - setChangeAlertCallback(...) installs a callback that will be invoked from
 *    onValueChanged(). The callback's lifetime must outlive the calls coming
 *    from the monitor (i.e., ensure synchronization if monitor runs on another thread).
 *  - Copy and assignment are disabled to avoid accidental duplication of monitors.
 */

/**
 * @class HintMonitor
 * @brief Base class for concrete monitors that detect integer value changes.
 *
 * The class is intentionally lightweight and header-only friendly. Derived
 * classes implement platform- or application-specific polling/event logic in
 * monitorLoop(). When a change is detected, derived classes should call
 * onValueChanged(previous, current) to trigger the installed callback.
 */
class HintMonitor {
private:
    // Name that identifies the monitored hint. Immutable after construction.
    std::string hintName_;

public:
    /**
     * @brief Construct a monitor for a hint with the given name.
     * @param name A human- and machine-readable identifier for the hint being monitored.
     *
     * The name is stored by value and can be retrieved by name().
     */
    HintMonitor(const std::string& name) : hintName_(name) {}

    /**
     * @brief Virtual default destructor to allow polymorphic deletion.
     *
     * Derived classes may override if they need custom teardown, but the
     * default is sufficient for most cases.
     */
    virtual ~HintMonitor() = default;

    // Disable copy construction and copy assignment to avoid multiple owners
    // of a monitor, which could cause race conditions or duplicate threads.
    HintMonitor(const HintMonitor&) = delete;
    HintMonitor& operator=(const HintMonitor&) = delete;

    /**
     * @brief Optional initialization step invoked before monitoring begins.
     * @return int Status code: 0 for success, non-zero for failure.
     *
     * Override in derived classes to perform any initialization that may fail,
     * such as opening descriptors, registering with system services, etc.
     * The default implementation does nothing and returns success (0).
     */
    virtual int init() { return 0; }

    /**
     * @brief Core monitoring routine that derived classes must implement.
     *
     * monitorLoop() should contain the logic that detects changes in the
     * monitored value. It is typically run on a dedicated thread. When a
     * change is found, the derived class should call onValueChanged(previous, current).
     *
     * Implementations are free to block, poll, or subscribe to events as
     * appropriate for the platform.
     */
    virtual void monitorLoop() = 0;

    /**
     * @brief Install a callback to be notified on value changes.
     * @param cb A callable accepting (const std::string& hintName, int oldValue, int newValue).
     *
     * The callback will be invoked by onValueChanged(...) when a change is
     * detected. The monitor does not make any assumptions about thread affinity
     * of the callback; callers are responsible for thread-safety and lifetime
     * of any captured state inside the callback.
     *
     * The callback is stored by value. Passing an empty std::function clears it.
     */
    void setChangeAlertCallback(
        std::function<void(const std::string& hintName, int oldValue, int newValue)> cb)
    {
        alertCallback_ = std::move(cb);
    }

    /**
     * @brief Return the name of the monitored hint.
     * @return const std::string& Reference to the stored hint name.
     */
    const std::string& name() const { return hintName_; }

protected:
    /**
     * @brief Invoked by derived classes when the observed value changes.
     * @param previous_value The value observed before the change.
     * @param current_value The new observed value.
     *
     * The default implementation invokes the installed alertCallback_, if any,
     * passing the hint name and the old/new values. Override this method to
     * implement extra behavior on change while still optionally calling the
     * base implementation to notify the callback:
     *
     * Example:
     *   void onValueChanged(int prev, int cur) override {
     *       // custom behavior...
     *       HintMonitor::onValueChanged(prev, cur); // notify callback
     *   }
     *
     * Thread-safety: this method will be called from whatever context the
     * derived class calls it from (often a monitoring thread). The base
     * implementation does not perform synchronization; ensure that any shared
     * state accessed here is protected appropriately.
     */
    virtual void onValueChanged(int previous_value, int current_value)
    {
        if (alertCallback_) {
            alertCallback_(hintName_, previous_value, current_value);
        }
    }

    // Stored change notification callback. May be empty.
    std::function<void(const std::string& hintName, int oldValue, int newValue)> alertCallback_;
};

#endif // HINTMONITOR_H