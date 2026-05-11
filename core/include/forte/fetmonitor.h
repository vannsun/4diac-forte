#ifndef _FETMONITOR_H_
#define _FETMONITOR_H_

#include <chrono>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <functional>
#include <cstdint>
#include <atomic>

/*! \brief Portable alias — replace with CStringDictionary::TStringId on FORTE integration. */
using TStringId = const char*;

/*! \brief Callback type invoked when a deadline is missed.
 *
 * Receives the FB instance name ID so the handler knows which FB violated.
 * On FORTE integration this callback should post an ERROR EVENT to the Safety FB
 * via FORTE's thread-safe event queue.
 */
using FETErrorCallback = std::function<void(TStringId paFBId)>;

/*! \ingroup CORE \brief Singleton monitor for Forced Execution Time (FET) deadline enforcement.
 *
 * For each monitored FB:
 *   - startMeasurement() is called when an input event arrives
 *   - endMeasurement() is called when the output event is placed on the queue
 *   - If the deadline elapses before endMeasurement(), the registered error
 *     callback fires on a dedicated timer thread
 *
 * Design decisions:
 *   - No artificial waiting: if the FB finishes before its deadline, the output
 *     event fires immediately and the remaining budget is discarded.
 *   - Error handling stays in IEC 61499: the callback posts an event to FORTE's
 *     own queue rather than calling back into the scheduler directly.
 *   - Thread safety: a single mutex protects all shared state. The condition
 *     variable allows the timer thread to be cancelled atomically when the FB
 *     finishes on time.
 *
 * FORTE integration notes:
 *   - Replace TStringId with CStringDictionary::TStringId
 *   - Replace singleton implementation with DECLARE_SINGLETON / DEFINE_SINGLETON
 *   - In the error callback, call the Safety FB's receiveInputEvent via the ECET
 *
 * Configuration:
 *   - Call registerFB() to enable monitoring for a specific FB and set its deadline
 *   - Call unregisterFB() to disable monitoring for a specific FB
 *   - Unregistered FBs are silently ignored by startMeasurement / endMeasurement
 */
class CFETMonitor {
public:
  /*! \brief Access the singleton instance. */
  static CFETMonitor& getInstance() {
    static CFETMonitor instance;
    return instance;
  }

  CFETMonitor(const CFETMonitor&) = delete;
  CFETMonitor& operator=(const CFETMonitor&) = delete;

  ~CFETMonitor();

  /*! \brief Register an FB for FET monitoring.
   *
   * Must be called before startMeasurement / endMeasurement for this FB.
   * If the FB is already registered its configuration is updated.
   *
   * \param paFBId      The FB's instance name ID.
   * \param paDeadline  Maximum allowed execution time.
   * \param paCallback  Called on the timer thread if the deadline is missed.
   *                    Must be thread-safe. On FORTE integration, post an event
   *                    to the Safety FB via FORTE's thread-safe queue here.
   */
  void registerFB(TStringId paFBId,
                  std::chrono::nanoseconds paDeadline,
                  FETErrorCallback paCallback);

  /*! \brief Unregister an FB — monitoring is silently disabled for it.
   *
   * Safe to call even if no measurement is in progress for this FB.
   * \param paFBId The FB's instance name ID.
   */
  void unregisterFB(TStringId paFBId);

  /*! \brief Start the deadline countdown for a Function Block.
   *
   * Call this when a triggering input event arrives (e.g., in receiveInputEvent).
   * Silently ignored if the FB is not registered.
   *
   * \param paFBId The FB's instance name ID.
   */
  void startMeasurement(TStringId paFBId);

  /*! \brief Signal that the FB has produced its output event — cancel the timer.
   *
   * Call this when the FB places its output event on the queue (e.g., in
   * sendOutputEvent). Silently ignored if the FB is not registered or no
   * measurement is in progress.
   * No artificial waiting: if called before the deadline, execution continues
   * immediately and the remaining budget is discarded.
   *
   * \param paFBId The FB's instance name ID.
   */
  void endMeasurement(TStringId paFBId);

  /*! \brief Check whether an FB is currently registered for FET monitoring.
   *
   * \param paFBId The FB's instance name ID.
   * \return true if registered.
   */
  bool isRegistered(TStringId paFBId) const;

private:
  CFETMonitor() = default;

  /*! \brief Per-FB state held by the monitor. */
  struct FBMonitorState {
    std::chrono::nanoseconds deadline{0};
    FETErrorCallback callback;

    // Set to true when a measurement is in progress.
    // The timer thread reads this under mMutex.
    bool inProgress{false};

    // Incremented each time startMeasurement is called for this FB.
    // The timer thread captures the generation at start; if endMeasurement
    // increments it before the deadline, the timer knows it was cancelled.
    uint64_t generation{0};
  };

  // Protects mStates and mTimerThreads.
  mutable std::mutex mMutex;

  // Signalled by endMeasurement to wake the timer thread early (cancelled).
  std::condition_variable mCV;

  // Per-FB configuration and runtime state.
  std::map<TStringId, FBMonitorState> mStates;

  // Per-FB active timer thread. Joined/replaced on each startMeasurement.
  std::map<TStringId, std::thread> mTimerThreads;

  /*! \brief Internal: launch the timer thread for a given FB.
   *
   * Captures the current generation so it can detect cancellation.
   * Must be called with mMutex held.
   */
  void launchTimerThread(TStringId paFBId, uint64_t paGeneration,
                         std::chrono::nanoseconds paDeadline,
                         FETErrorCallback paCallback);
};

#endif // _FETMONITOR_H_