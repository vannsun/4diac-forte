#include "forte/eetmonitor.h"
#include "forte/fetmonitor.h"   // one-directional: EET → FET only
#include "forte/eetconfig.h"
#include "forte/util/devlog.h"
#include <filesystem>
#include <fstream>

// ─────────────────────────────────────────────────────────────────────────────
// startMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::startMeasurement(TStringId paFBId) {
  if(forte::eet::isMonitoringExcluded(paFBId)) return;
  std::lock_guard<std::mutex> lock(mMutex);
  // Overwrite any existing in-progress timestamp. Handles the case where
  // a previous endMeasurement was never called (e.g. an event was dropped).
  mStartTimes[paFBId] = Clock::now();
}

// ─────────────────────────────────────────────────────────────────────────────
// endMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::endMeasurement(TStringId paFBId) {
  if(forte::eet::isMonitoringExcluded(paFBId)) return;
  // Capture end time before the lock to minimise measurement error.
  const auto endTime = Clock::now();

  {
    std::lock_guard<std::mutex> lock(mMutex);

    auto startIt = mStartTimes.find(paFBId);
    if(startIt == mStartTimes.end()) {
      return;  // No matching startMeasurement.
    }

    const long long durationNs =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        endTime - startIt->second).count();

    mStartTimes.erase(startIt);

    auto& durations = mDurations[paFBId];
    if(durations.size() >= MAX_SAMPLES) {
      durations.erase(durations.begin());
    }
    durations.push_back(durationNs);
  }

  // Attempt FET activation — no-op until WARMUP_SAMPLES are collected,
  // and no-op on all subsequent calls once activated.
  // Called outside the lock because activateFET takes its own lock
  // and then calls CFETMonitor which has its own lock.
  activateFET(paFBId, mDefaultStrategy);
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
  if(durations.empty()) return 0.0;
  double sum = 0.0;
  for(long long d : durations) sum += static_cast<double>(d);
  return sum / static_cast<double>(durations.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// getStdDev
// ─────────────────────────────────────────────────────────────────────────────

double CEETMonitor::getStdDev(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if(durations.size() < 2) return 0.0;
  double sum = 0.0;
  for(long long d : durations) sum += static_cast<double>(d);
  const double mean = sum / static_cast<double>(durations.size());
  double variance = 0.0;
  for(long long d : durations) {
    const double diff = static_cast<double>(d) - mean;
    variance += diff * diff;
  }
  variance /= static_cast<double>(durations.size() - 1);  // Bessel correction
  return std::sqrt(variance);
}

// ─────────────────────────────────────────────────────────────────────────────
// get90thPercentile
// ─────────────────────────────────────────────────────────────────────────────

long long CEETMonitor::get90thPercentile(TStringId paFBId) const {
  auto durations = getDurationsCopy(paFBId);  // copy — we sort it
  if(durations.empty()) return 0;
  std::sort(durations.begin(), durations.end());
  // Nearest-rank: ceil(0.9 * N) gives the 1-based rank.
  const size_t idx = static_cast<size_t>(
    std::ceil(0.9 * static_cast<double>(durations.size()))) - 1;
  return durations[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// getMax
// ─────────────────────────────────────────────────────────────────────────────

long long CEETMonitor::getMax(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if(durations.empty()) return 0;
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
// getDeadlineSuggestion
// ─────────────────────────────────────────────────────────────────────────────

long long CEETMonitor::getDeadlineSuggestion(TStringId paFBId,
                                              DeadlineStrategy strategy) const {
  switch(strategy) {
    case DeadlineStrategy::MAX:
      return getMax(paFBId);
    case DeadlineStrategy::P90:
      return get90thPercentile(paFBId);
    case DeadlineStrategy::MEAN_PLUS_3SIG: {
      const double mean = getMean(paFBId);
      const double sig  = getStdDev(paFBId);
      return static_cast<long long>(mean + 3.0 * sig);
    }
  }
  return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// setConfiguredDeadline / getConfiguredDeadline
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::setConfiguredDeadline(TStringId paFBId, long long paDeadlineNs) {
  std::lock_guard<std::mutex> lock(mMutex);
  mConfiguredDeadlines[paFBId] = paDeadlineNs;
}

long long CEETMonitor::getConfiguredDeadline(TStringId paFBId) const {
  std::lock_guard<std::mutex> lock(mMutex);
  auto it = mConfiguredDeadlines.find(paFBId);
  return (it != mConfiguredDeadlines.end()) ? it->second : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// activateFET
//
// Called from endMeasurement after every sample. No-op until WARMUP_SAMPLES
// are collected. No-op on all subsequent calls once activated for this FB.
//
// On first activation:
//   1. Compute deadline via mDefaultStrategy
//   2. Store in mConfiguredDeadlines (for CSV export and logging)
//   3. Register with CFETMonitor — enforcement starts on the next event
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::activateFET(TStringId paFBId, DeadlineStrategy strategy) {

  {
    std::lock_guard<std::mutex> lock(mMutex);

    auto activatedIt = mFETActivated.find(paFBId);
    if(activatedIt != mFETActivated.end() && activatedIt->second) {
      return;
    }

    auto durIt = mDurations.find(paFBId);
    if(durIt == mDurations.end() || durIt->second.size() < WARMUP_SAMPLES) {
      return;
    }

    mFETActivated[paFBId] = true;
  }

  // Compute deadline outside the lock — stat helpers take their own lock.
  const long long deadlineNs = getDeadlineSuggestion(paFBId, strategy);
  if(deadlineNs <= 0) {
    return;
  }

  // Store for logging and CSV export.
  setConfiguredDeadline(paFBId, deadlineNs);

  // Register with FET — enforcement starts from the next receiveInputEvent.
  CFETMonitor::getInstance().registerFB(
    paFBId,
    std::chrono::nanoseconds(deadlineNs),
    [](TStringId paId) {
      DEVLOG_ERROR("FET deadline missed: %s\n", paId);
    }
  );

  DEVLOG_INFO("EET→FET: activated deadline %lldns for '%s' after %zu warmup samples\n",
              deadlineNs, paFBId, static_cast<size_t>(WARMUP_SAMPLES));
}

// ─────────────────────────────────────────────────────────────────────────────
// clearData / clearAllData
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::clearData(TStringId paFBId) {
  std::lock_guard<std::mutex> lock(mMutex);
  mDurations.erase(paFBId);
  mStartTimes.erase(paFBId);
  mConfiguredDeadlines.erase(paFBId);
  mFETActivated.erase(paFBId);
}

void CEETMonitor::clearAllData() {
  std::lock_guard<std::mutex> lock(mMutex);
  mDurations.clear();
  mStartTimes.clear();
  mConfiguredDeadlines.clear();
  mFETActivated.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// exportCSV
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::exportCSV(TStringId paFBId,
                             const std::string& paFileName) const {
  std::ofstream file(paFileName);
  if(!file.is_open()) return;
  file << "sample,duration_ns\n";
  const auto data = getDurations(paFBId);
  for(size_t i = 0; i < data.size(); ++i) {
    file << i << "," << data[i] << "\n";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// exportAllCSV
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::exportAllCSV(const std::string& paDirectory) const {
  // Match the exact type of mDurations from eetmonitor.h
  std::map<TStringId, std::vector<long long>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    snapshot = mDurations;
  }

  std::filesystem::create_directories(paDirectory);
  for(const auto& [fbId, durations] : snapshot) {
    const std::string filename =
      paDirectory + "/" + std::string(fbId) + ".csv";
    std::ofstream file(filename);
    if(!file.is_open()) continue;
    file << "execution_ns\n";
    for(const auto& d : durations) {
      file << d << "\n";
    }
    DEVLOG_INFO("EETMonitor: exported %zu samples for '%s'\n",
                durations.size(), fbId);
  }
}
// ─────────────────────────────────────────────────────────────────────────────
// startPeriodicExport / stopPeriodicExport
// ─────────────────────────────────────────────────────────────────────────────

/* void CEETMonitor::startPeriodicExport(const std::string& paDirectory,
                                       std::chrono::seconds paInterval) {
  if(mExportRunning.exchange(true)) {
    return;  // Already running — guard against double-start.
  }
  mExportThread = std::thread([this, paDirectory, paInterval]() {
    while(mExportRunning) {
      std::this_thread::sleep_for(paInterval);
      if(mExportRunning) {  // Re-check after waking — may have been stopped.
        exportAllCSV(paDirectory);
      }
    }
  });
} */

void CEETMonitor::startPeriodicExport(const std::string& paDirectory,
                                       std::chrono::seconds paInterval,
                                       size_t paTargetSamples) {
  if(mExportRunning.exchange(true)) return;

  mExportThread = std::thread([this, paDirectory, paInterval, paTargetSamples]() {
    auto lastExport = std::chrono::steady_clock::now();
    const auto pollInterval = std::chrono::milliseconds(500);

    while(mExportRunning) {
      std::this_thread::sleep_for(pollInterval);
      if(!mExportRunning) break;

      // Check target on every poll tick.
      if(paTargetSamples > 0) {
        std::lock_guard<std::mutex> lock(mMutex);
        bool allDone = !mDurations.empty();
        for(const auto& [id, d] : mDurations) {
          if(d.size() < paTargetSamples) { allDone = false; break; }
        }
        if(allDone) {
          exportAllCSV(paDirectory);
          DEVLOG_INFO("EETMonitor: target of %zu samples reached — stopping.\n",
                      paTargetSamples);
          mExportRunning = false;
          break;
        }
      }

      // Periodic export on the original interval.
      auto now = std::chrono::steady_clock::now();
      if(now - lastExport >= paInterval) {
        exportAllCSV(paDirectory);
        lastExport = now;
      }
    }
  });
}

void CEETMonitor::stopPeriodicExport() {
  mExportRunning = false;
  if(mExportThread.joinable()) {
    mExportThread.join();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// getDurationsCopy  (private helper)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<long long> CEETMonitor::getDurationsCopy(TStringId paFBId) const {
  std::lock_guard<std::mutex> lock(mMutex);
  auto it = mDurations.find(paFBId);
  if(it == mDurations.end()) return {};
  return it->second;
}