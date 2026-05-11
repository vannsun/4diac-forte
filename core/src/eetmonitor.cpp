#include "forte/eetmonitor.h"

// When integrating into FORTE, add: DEFINE_SINGLETON(CEETMonitor)

// ─────────────────────────────────────────────────────────────────────────────
// startMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::startMeasurement(TStringId paFBId) {
  std::lock_guard<std::mutex> lock(mMutex);
  // Overwrite any existing in-progress timestamp. This handles the case where
  // a previous endMeasurement was never called (e.g. an event was dropped).
  mStartTimes[paFBId] = std::chrono::high_resolution_clock::now();
}

// ─────────────────────────────────────────────────────────────────────────────
// endMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::endMeasurement(TStringId paFBId) {
  // Capture end time before acquiring the lock to minimise measurement error.
  const auto endTime = std::chrono::high_resolution_clock::now();

  std::lock_guard<std::mutex> lock(mMutex);

  // Guard: ignore calls with no matching startMeasurement.
  auto startIt = mStartTimes.find(paFBId);
  if(startIt == mStartTimes.end()) {
    return;
  }

  const long long durationNs =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      endTime - startIt->second).count();

  // Remove the in-progress entry now that we have the duration.
  mStartTimes.erase(startIt);

  // Append to the histogram, enforcing the sliding-window cap.
  auto& durations = mDurations[paFBId];
  if(durations.size() >= MAX_SAMPLES) {
    durations.erase(durations.begin());
  }
  durations.push_back(durationNs);
}

// ─────────────────────────────────────────────────────────────────────────────
// getDurations
// ─────────────────────────────────────────────────────────────────────────────

std::vector<long long> CEETMonitor::getDurations(TStringId paFBId) const {
  return getDurationsCopy(paFBId);
}

// ─────────────────────────────────────────────────────────────────────────────
// getMean
// ─────────────────────────────────────────────────────────────────────────────

double CEETMonitor::getMean(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if(durations.empty()) {
    return 0.0;
  }
  double sum = 0.0;
  for(long long d : durations) {
    sum += static_cast<double>(d);
  }
  return sum / static_cast<double>(durations.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// getStdDev
// ─────────────────────────────────────────────────────────────────────────────

double CEETMonitor::getStdDev(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if(durations.size() < 2) {
    return 0.0;
  }

  // Compute mean first.
  double sum = 0.0;
  for(long long d : durations) {
    sum += static_cast<double>(d);
  }
  const double mean = sum / static_cast<double>(durations.size());

  // Bessel-corrected sample variance (N-1 denominator).
  double variance = 0.0;
  for(long long d : durations) {
    const double diff = static_cast<double>(d) - mean;
    variance += diff * diff;
  }
  variance /= static_cast<double>(durations.size() - 1);

  return std::sqrt(variance);
}

// ─────────────────────────────────────────────────────────────────────────────
// get90thPercentile
// ─────────────────────────────────────────────────────────────────────────────

long long CEETMonitor::get90thPercentile(TStringId paFBId) const {
  auto durations = getDurationsCopy(paFBId);  // intentional copy — we sort it
  if(durations.empty()) {
    return 0;
  }

  std::sort(durations.begin(), durations.end());

  // Nearest-rank method: ceil(0.9 * N) gives the 1-based rank.
  const size_t idx = static_cast<size_t>(
    std::ceil(0.9 * static_cast<double>(durations.size()))) - 1;

  return durations[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// getMax
// ─────────────────────────────────────────────────────────────────────────────

long long CEETMonitor::getMax(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if(durations.empty()) {
    return 0;
  }
  return *std::max_element(durations.begin(), durations.end());
}

// ─────────────────────────────────────────────────────────────────────────────
// getSampleCount
// ─────────────────────────────────────────────────────────────────────────────

size_t CEETMonitor::getSampleCount(TStringId paFBId) const {
  std::lock_guard<std::mutex> lock(mMutex);
  auto it = mDurations.find(paFBId);
  return (it != mDurations.end()) ? it->second.size() : 0u;
}

// ─────────────────────────────────────────────────────────────────────────────
// clearData
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::clearData(TStringId paFBId) {
  std::lock_guard<std::mutex> lock(mMutex);
  mDurations.erase(paFBId);
  mStartTimes.erase(paFBId);
}

// ─────────────────────────────────────────────────────────────────────────────
// clearAllData
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::clearAllData() {
  std::lock_guard<std::mutex> lock(mMutex);
  mDurations.clear();
  mStartTimes.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// getDurationsCopy  (private helper)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<long long> CEETMonitor::getDurationsCopy(TStringId paFBId) const {
  std::lock_guard<std::mutex> lock(mMutex);
  auto it = mDurations.find(paFBId);
  if(it == mDurations.end()) {
    return {};
  }
  return it->second;  // copy
}