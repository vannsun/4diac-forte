/*******************************************************************************
 * Copyright (c) 2026 Carl von Ossietzky Oldenburg University, OFFIS
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0
 *
 * Contributors:
 *    Vannessa Cañon Pasquel - Initial implementation
 *******************************************************************************/

#include "forte/fetmonitor.h"
#include "forte/util/devlog.h"

#include <thread>

void CFETMonitor::registerFB(TStringId paFBId, std::chrono::nanoseconds paDeadline, FETErrorCallback paCallback) {
  std::lock_guard<std::mutex> lock(mMutex);
  auto &state = mStates[paFBId];
  state.deadline = paDeadline;
  state.callback = paCallback;
}

void CFETMonitor::startMeasurementAt(TStringId paFBId, TimePoint paStartTime) {
  std::lock_guard<std::mutex> lock(mMutex);
  auto it = mStates.find(paFBId);
  if (it == mStates.end()) {
    return; // Not registered
  }

  it->second.startTime = paStartTime;
  it->second.active = true;
}

long long CFETMonitor::waitUntilDeadline(TStringId paFBId) {
  const auto endTime = Clock::now();

  FBState stateCopy;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    auto it = mStates.find(paFBId);
    if (it == mStates.end())
      return true; // FB not registered.
    FBState &state = it->second;
    if (!state.active)
      return true; // No active timing session.
    stateCopy = state;
    state.active = false; // Measurement completed
  } // Lock is released.

  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - stateCopy.startTime);

  if (elapsed > stateCopy.deadline) {
    DEVLOG_ERROR("FETMonitor: deadline missed for FB '%s': "
                 "elapsed=%lldns  deadline=%lldns  overshoot=%lldns\n",
                 paFBId, static_cast<long long>(elapsed.count()), static_cast<long long>(stateCopy.deadline.count()),
                 static_cast<long long>((elapsed - stateCopy.deadline).count()));

    if (stateCopy.callback)
      stateCopy.callback(paFBId);
    return false;
  }

  const auto remaining = stateCopy.deadline - elapsed;
  std::this_thread::sleep_for(remaining);

  const auto enforcedEndTime = Clock::now();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(enforcedEndTime - stateCopy.startTime).count();
}

void CFETMonitor::unregisterFB(TStringId paFBId) {
  std::lock_guard<std::mutex> lock(mMutex);
  mStates.erase(paFBId);
}
