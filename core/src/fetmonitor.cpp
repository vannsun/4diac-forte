#include "fetmonitor.h"

// When integrating into FORTE, add: DEFINE_SINGLETON(CFETMonitor)

// ─────────────────────────────────────────────────────────────────────────────
// Destructor — join all running timer threads cleanly
// ─────────────────────────────────────────────────────────────────────────────

CFETMonitor::~CFETMonitor() {
  // Signal all timer threads to wake up and exit, then join them.
  {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto& [id, state] : mStates) {
      // Advance every generation so every live timer sees a cancellation.
      state.generation++;
      state.inProgress = false;
    }
  }
  mCV.notify_all();

  for (auto& [id, thread] : mTimerThreads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// registerFB
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::registerFB(TStringId paFBId,
                              std::chrono::nanoseconds paDeadline,
                              FETErrorCallback paCallback) {
  std::lock_guard<std::mutex> lock(mMutex);
  auto& state = mStates[paFBId];
  state.deadline = paDeadline;
  state.callback = std::move(paCallback);
  // Leave generation and inProgress as-is in case a measurement is ongoing.
}

// ─────────────────────────────────────────────────────────────────────────────
// unregisterFB
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::unregisterFB(TStringId paFBId) {
  std::thread threadToJoin;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    auto stateIt = mStates.find(paFBId);
    if (stateIt == mStates.end()) {
      return;
    }
    // Cancel any in-progress measurement by advancing the generation.
    stateIt->second.generation++;
    stateIt->second.inProgress = false;
    mStates.erase(stateIt);

    auto threadIt = mTimerThreads.find(paFBId);
    if (threadIt != mTimerThreads.end()) {
      threadToJoin = std::move(threadIt->second);
      mTimerThreads.erase(threadIt);
    }
  }
  mCV.notify_all();

  // Join outside the lock to avoid deadlock.
  if (threadToJoin.joinable()) {
    threadToJoin.join();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// startMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::startMeasurement(TStringId paFBId) {
  std::thread oldThread;
  {
    std::lock_guard<std::mutex> lock(mMutex);

    auto it = mStates.find(paFBId);
    if (it == mStates.end()) {
      return; // FB not registered — silently ignore
    }

    auto& state = it->second;

    // If a previous measurement is still in progress, cancel it by advancing
    // the generation before starting a new one.
    if (state.inProgress) {
      state.generation++;
      state.inProgress = false;
      mCV.notify_all();

      // Collect old thread to join outside the lock.
      auto threadIt = mTimerThreads.find(paFBId);
      if (threadIt != mTimerThreads.end()) {
        oldThread = std::move(threadIt->second);
        mTimerThreads.erase(threadIt);
      }
    }

    state.inProgress = true;
    state.generation++;
    const uint64_t capturedGeneration = state.generation;
    const auto deadline = state.deadline;
    const auto callback = state.callback;

    launchTimerThread(paFBId, capturedGeneration, deadline, callback);
  }

  // Join the old thread outside the lock.
  if (oldThread.joinable()) {
    oldThread.join();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// endMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::endMeasurement(TStringId paFBId) {
  std::thread oldThread;
  {
    std::lock_guard<std::mutex> lock(mMutex);

    auto it = mStates.find(paFBId);
    if (it == mStates.end() || !it->second.inProgress) {
      return; // not registered or no measurement in progress — safe no-op
    }

    // Cancel the timer by advancing the generation and clearing inProgress.
    // The timer thread will wake on mCV, see the generation mismatch, and exit
    // without firing the error callback.
    it->second.generation++;
    it->second.inProgress = false;

    auto threadIt = mTimerThreads.find(paFBId);
    if (threadIt != mTimerThreads.end()) {
      oldThread = std::move(threadIt->second);
      mTimerThreads.erase(threadIt);
    }
  }
  // Signal the timer thread to wake and check cancellation.
  mCV.notify_all();

  // Join outside the lock — this is safe because the timer thread only holds
  // the lock briefly to check the generation; it does not call back into
  // endMeasurement.
  if (oldThread.joinable()) {
    oldThread.join();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// isRegistered
// ─────────────────────────────────────────────────────────────────────────────

bool CFETMonitor::isRegistered(TStringId paFBId) const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mStates.find(paFBId) != mStates.end();
}

// ─────────────────────────────────────────────────────────────────────────────
// launchTimerThread  (private)
// ─────────────────────────────────────────────────────────────────────────────

void CFETMonitor::launchTimerThread(TStringId paFBId,
                                     uint64_t paGeneration,
                                     std::chrono::nanoseconds paDeadline,
                                     FETErrorCallback paCallback) {
  // Must be called with mMutex held.
  // Captures paFBId, paGeneration, paDeadline, paCallback by value so the
  // thread owns its own copy and is not affected by later registerFB calls.
  mTimerThreads[paFBId] = std::thread([this, paFBId, paGeneration, paDeadline, paCallback]() {
    std::unique_lock<std::mutex> lock(mMutex);

    // Wait until either:
    //   (a) the deadline expires (timeout), or
    //   (b) the generation changes (endMeasurement or unregisterFB cancelled us)
    const bool timedOut = !mCV.wait_for(lock, paDeadline, [this, paFBId, paGeneration]() {
      auto it = mStates.find(paFBId);
      // Wake early if the FB was unregistered or generation advanced (cancelled).
      return it == mStates.end() || it->second.generation != paGeneration;
    });

    if (timedOut) {
      // Double-check: only fire the error if this generation is still the
      // active one (guards against a race where endMeasurement arrived at
      // exactly the same moment as the timeout).
      auto it = mStates.find(paFBId);
      if (it != mStates.end() && it->second.generation == paGeneration) {
        it->second.inProgress = false;
        lock.unlock();
        // Fire the error callback outside the lock so the handler can safely
        // call back into the monitor (e.g. to unregister the FB).
        if (paCallback) {
          paCallback(paFBId);
        }
      }
    }
    // If not timed out: cancelled by endMeasurement — exit silently.
  });
}