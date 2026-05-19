#include "forte/fetmonitor.h"
#include "forte/util/devlog.h"

#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// registerFB
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::registerFB(TStringId paFBId,
                              std::chrono::nanoseconds paDeadline,
                              FETErrorCallback paCallback) {
  std::lock_guard<std::mutex> lock(mMutex);
  auto& state    = mStates[paFBId];
  state.deadline = paDeadline;
  state.callback = paCallback;
  // Preserve active/startTime if a measurement is already in progress.
}

// ─────────────────────────────────────────────────────────────────────────────
// startMeasurement
// Called from receiveInputEvent, before executeEvent.
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::startMeasurement(TStringId paFBId) {
  // Capture time before the lock to minimise hot-path overhead.
  const auto now = Clock::now();

  std::lock_guard<std::mutex> lock(mMutex);
  auto it = mStates.find(paFBId);
  if(it == mStates.end()) {
    return;  // Not registered — nothing to do.
  }
  it->second.startTime = now;
  it->second.active    = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// waitUntilDeadline
//
// Called from sendOutputEvent, AFTER writeOutputData and EET.endMeasurement,
// BEFORE triggerEvent.
//
//   elapsed <= deadline  → sleep(deadline − elapsed); return true
//   elapsed >  deadline  → fire callback; return false
//   not registered       → return true  (unmonitored FBs unaffected)
//   not active           → return true  (no matching startMeasurement)
// ─────────────────────────────────────────────────────────────────────────────

bool CFETMonitor::waitUntilDeadline(TStringId paFBId) {
  // Capture end time before the lock — keeps elapsed measurement accurate.
  const auto endTime = Clock::now();

  FBState stateCopy;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mStates.find(paFBId);
    if(it == mStates.end()) {
      return true;  // Not registered.
    }

    FBState& state = it->second;
    if(!state.active) {
      return true;  // No matching startMeasurement, or already consumed.
    }

    stateCopy = state;
    // Mark inactive now — prevents double-sleep when sendOutputEvent is
    // called for multiple output events within the same receiveInputEvent.
    state.active = false;
  }

  // All time values in nanoseconds.
  const auto elapsed =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      endTime - stateCopy.startTime);

  if(elapsed > stateCopy.deadline) {
    DEVLOG_ERROR(
      "FETMonitor: deadline missed for FB '%s': "
      "elapsed=%lldns  deadline=%lldns  overshoot=%lldns\n",
      paFBId,
      static_cast<long long>(elapsed.count()),
      static_cast<long long>(stateCopy.deadline.count()),
      static_cast<long long>((elapsed - stateCopy.deadline).count()));

    if(stateCopy.callback) {
      stateCopy.callback(paFBId);
    }
    return false;
  }

  // Normal path: pad remaining time so triggerEvent always fires at
  // exactly startTime + deadline — deterministic downstream timing.
  const auto remaining = stateCopy.deadline - elapsed;

  DEVLOG_INFO(
    "FETMonitor: '%s' elapsed=%lldns deadline=%lldns sleeping=%lldns\n",
    paFBId,
    static_cast<long long>(elapsed.count()),
    static_cast<long long>(stateCopy.deadline.count()),
    static_cast<long long>(remaining.count()));

  std::this_thread::sleep_for(remaining);
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// unregisterFB
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::unregisterFB(TStringId paFBId) {
  std::lock_guard<std::mutex> lock(mMutex);
  mStates.erase(paFBId);
}

// ─────────────────────────────────────────────────────────────────────────────
// clearAll
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::clearAll() {
  std::lock_guard<std::mutex> lock(mMutex);
  mStates.clear();
}