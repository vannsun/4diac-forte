#ifndef _FETMONITOR_H_
#define _FETMONITOR_H_

#include <chrono>
#include <functional>
#include <mutex>
#include <unordered_map>

using TStringId = const char *;
using FETErrorCallback = std::function<void(TStringId)>;

/*! \ingroup CORE \brief Singleton monitor for Forced Execution Time (FET) deadline enforcement.
 *
 * After CEETMonitor::activateFET() registers an FB, FET enforces a maximum
 * response time from input event arrival to output event placement.
 *
 * On each execution:
 *   - startMeasurementAt() records the start timestamp (shared with EET)
 *   - waitUntilDeadline() computes elapsed time and either sleeps for the
 *     remaining budget or fires the error callback on a deadline miss
 *
 * The ECET thread is blocked during the sleep - this is intentional and
 * produces deterministic output event timing clustered at the deadline value.
 *
 * Known limitation: blocking the ECET thread introduces overhead for all
 * FBs in the chain. A non-blocking timer-thread design would avoid this
 * at the cost of losing the clustering behavior.
 *
 */
class CFETMonitor {
  public:
    /*! \brief Monotonic clock FET measurements. */
    using Clock = std::chrono::steady_clock;
    /*! \brief Timestamp for FET measurements. */
    using TimePoint = Clock::time_point;

    /*! \brief Access the singleton instance. */
    static CFETMonitor &getInstance() {
      static CFETMonitor instance;
      return instance;
    }

    /*! \brief Register an FB for deadline enforcement.
     *
     * Called by CEETMonitor::activateFET() after warmup completes.
     * Sets the deadline, sleep target, and error callback for this FB.
     *
     * \param paFBId        FB instance name ID.
     * \param paDeadline    Maximum allowed execution time — deadline missed threshold.
     * \param paSleepTarget Duration to sleep to in waitUntilDeadline(). Must be
     *                      less than paDeadline to leave headroom for OS timer
     *                      overshoot. Computed by CEETMonitor::getSleepTarget().
     * \param paCallback    Called on the timer thread if the deadline is missed.
     */
    void registerFB(TStringId paFBId,
                    std::chrono::nanoseconds paDeadline,
                    std::chrono::nanoseconds paSleepTarget,
                    FETErrorCallback paCallback);

    /*! \brief Record execution start timestamp for a monitored FB.
     *
     * Called from receiveInputEvent with the same timestamp as EET
     * so both monitors share a consistent start time.
     * Silently ignored if FB is not registered.
     */
    void startMeasurementAt(TStringId paFBId, TimePoint paStartTime);

    /*! \brief Check deadline and enforce it by sleeping if needed.
     *
     * Called from sendOutputEvent after writeOutputData.
     *
     * Computes elapsed = now - startTime.
     *
     * If elapsed < deadline: sleeps for remaining time, returns true.
     *
     * If elapsed > deadline: fires error callback, returns false.
     *
     * Returns enforced duration in ns (> 0), (0) on FB not registered or no active session, or (-1) on deadline missed.
     *
     * Returning -1 suppresses triggerEvent - output event is not fired.
     */
    long long waitUntilDeadline(TStringId paFBId);

    /*! \brief Unregister an FB - enforcement silently disabled. */
    void unregisterFB(TStringId paFBId);

    /*! \brief Check whether an FB is currently registered. */
    bool isRegistered(TStringId paFBId) const;

  private:
    CFETMonitor() = default;
    CFETMonitor(const CFETMonitor &) = delete;
    CFETMonitor &operator=(const CFETMonitor &) = delete;

    /*! \brief Per-FB enforcement state. */
    struct FBState {
        std::chrono::nanoseconds deadline{0}; //!< Maximum allowed execution time.
        std::chrono::nanoseconds sleepTarget{0}; //!< Actual sleep target without deadline multiplier.
        FETErrorCallback callback; //!< Called when deadline is missed.
        TimePoint startTime{}; //!< Recorded by startMeasurementAt().
        bool active{false}; //!< True while a measurement is in progress.
    };

    /*! \brief Protects mStates for concurrent access from ECET threads. */
    mutable std::mutex mMutex;

    /*! \brief Per-FB enforcement state keyed by interned instance name pointer. */
    std::unordered_map<TStringId, FBState> mStates;
};

#endif // _FETMONITOR_H_
