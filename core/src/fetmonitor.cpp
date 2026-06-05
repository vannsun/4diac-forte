#include "forte/fetmonitor.h"
#include "forte/util/devlog.h"

#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
// registerFB
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::registerFB(TStringId paFBId,
                             std::chrono::nanoseconds paDeadline,
                             FETErrorCallback paCallback,
                             std::function<void()> paOnEnforced) {
  std::lock_guard<std::mutex> lock(mMutex);
  auto &state = mStates[paFBId];
  state.deadline = paDeadline;
  state.callback = paCallback;
  state.onEnforced = paOnEnforced;
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
  if (it == mStates.end()) {
    return; // Not registered — nothing to do.
  }
  it->second.startTime = now;
  it->second.active = true;
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
  const auto endTime = Clock::now();

  FBState stateCopy;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mStates.find(paFBId);
    if (it == mStates.end())
      return true;
    FBState &state = it->second;
    if (!state.active)
      return true;
    stateCopy = state;
    state.active = false;
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - stateCopy.startTime);

  if (elapsed > stateCopy.deadline) {
    DEVLOG_ERROR("FETMonitor: deadline missed for FB '%s': "
                 "elapsed=%lldns  deadline=%lldns  overshoot=%lldns\n",
                 paFBId, static_cast<long long>(elapsed.count()), static_cast<long long>(stateCopy.deadline.count()),
                 static_cast<long long>((elapsed - stateCopy.deadline).count()));

    if (stateCopy.callback)
      stateCopy.callback(paFBId);
    if (stateCopy.onEnforced)
      stateCopy.onEnforced(); // ← record violation
    return false;
  }

  const auto remaining = stateCopy.deadline - elapsed;
  std::this_thread::sleep_for(remaining);

  if (stateCopy.onEnforced)
    stateCopy.onEnforced(); // ← record after sleep
  return true;
} // ─────────────────────────────────────────────────────────────────────────────
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
