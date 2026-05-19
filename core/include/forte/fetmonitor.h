#ifndef _FETMONITOR_H_
#define _FETMONITOR_H_

#include <chrono>
#include <functional>
#include <mutex>
#include <unordered_map>

using TStringId = const char*;
using FETErrorCallback = std::function<void(TStringId)>;
// ─────────────────────────────────────────────────────────────────────────────
// CFETMonitor — Fixed Execution Time monitor
//
// Responsibilities:
//   - Hold a registered deadline per FB (set by CEETMonitor::activateFET)
//   - On startMeasurement: record wall-clock start (called from receiveInputEvent)
//   - On waitUntilDeadline: pad or detect violation (called from sendOutputEvent,
//     after EET.endMeasurement, before triggerEvent)
//
// CFETMonitor knows nothing about CEETMonitor. Deadlines flow one way:
//   CEETMonitor::activateFET → CFETMonitor::registerFB
//
// Thread safety: all public methods are mutex-protected.
// ─────────────────────────────────────────────────────────────────────────────
 
class CFETMonitor {
public:
  static CFETMonitor& getInstance() {
    static CFETMonitor inst;
    return inst;
  }
 
  // Register or update a deadline for an FB.
  // Called by CEETMonitor::activateFET once enough EET samples exist.
  void registerFB(TStringId paFBId,
                  std::chrono::nanoseconds paDeadline,
                  FETErrorCallback paCallback = nullptr);
 
  // Called at the start of receiveInputEvent, before executeEvent.
  void startMeasurement(TStringId paFBId);
 
  // Called in sendOutputEvent, AFTER writeOutputData and EET.endMeasurement,
  // BEFORE triggerEvent.
  // Returns true  → within deadline, sleep applied, proceed normally.
  // Returns false → deadline exceeded, callback fired, suppress triggerEvent.
  bool waitUntilDeadline(TStringId paFBId);
 
  void unregisterFB(TStringId paFBId);
  void clearAll();
 
private:
  CFETMonitor() = default;
  CFETMonitor(const CFETMonitor&) = delete;
  CFETMonitor& operator=(const CFETMonitor&) = delete;
 
  using Clock     = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
 
  struct FBState {
    std::chrono::nanoseconds deadline{0};
    FETErrorCallback         callback;
    TimePoint                startTime{};
    bool                     active{false};
  };
 
  mutable std::mutex                           mMutex;
  std::unordered_map<TStringId, FBState>       mStates;
};
#endif // _FETMONITOR_H_