#include "forte/eetmonitor.h"
#include "forte/fetmonitor.h" // one-directional: EET → FET only
#include "forte/eetconfig.h"
#include "forte/util/devlog.h"
#include <filesystem>
#include <fstream>

// ─────────────────────────────────────────────────────────────────────────────
// startMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::startMeasurement(TStringId paFBId) {
  if (forte::eet::isMonitoringExcluded(paFBId))
    return;
  std::lock_guard<std::mutex> lock(mMutex);
  // Only record start if no measurement is already in progress.
  // Overwriting would corrupt the timestamp for FBs that receive re-entrant
  // calls (e.g. FBs that fire multiple output events per input event).
  if (mStartTimes.find(paFBId) == mStartTimes.end()) {
    mStartTimes[paFBId] = Clock::now();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// endMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::endMeasurement(TStringId paFBId) {
  if (forte::eet::isMonitoringExcluded(paFBId))
    return;

  const auto endTime = Clock::now();

  std::lock_guard<std::mutex> lock(mMutex);

  auto startIt = mStartTimes.find(paFBId);
  if (startIt == mStartTimes.end())
    return;

  const long long durationNs = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startIt->second).count();

  mStartTimes.erase(startIt);

  if (durationNs <= 0)
    return;

  auto &samples = mSamples[paFBId];

  if (samples.size() >= MAX_SAMPLES)
    samples.erase(samples.begin());

  Sample sample;
  sample.durationNs = durationNs;
  sample.timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime.time_since_epoch());

  // -------------------- FET STATE --------------------
  auto it = mFETActivated.find(paFBId);
  const bool fet = (it != mFETActivated.end() && it->second);
  sample.fetActive = fet;

  // -------------------- DEADLINE MISS --------------------
  sample.deadlineMiss = false;
  if (fet) {
    const long long deadlineNs = getConfiguredDeadline(paFBId);
    if (deadlineNs > 0) {
      sample.deadlineMiss = (durationNs > deadlineNs);
    }
  }

  // -------------------- PHASE --------------------
  auto sampleCount = samples.size();

  if (!fet) {
    sample.phase = ExecutionPhase::WARMUP;
  } else if (sampleCount < WARMUP_SAMPLES + 200) {
    sample.phase = ExecutionPhase::TRANSITION;
  } else {
    sample.phase = ExecutionPhase::FET_ACTIVE;
  }

  samples.push_back(sample);

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
  if (durations.empty())
    return 0.0;
  double sum = 0.0;
  for (long long d : durations)
    sum += static_cast<double>(d);
  return sum / static_cast<double>(durations.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// getStdDev
// ─────────────────────────────────────────────────────────────────────────────

double CEETMonitor::getStdDev(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if (durations.size() < 2)
    return 0.0;
  double sum = 0.0;
  for (long long d : durations)
    sum += static_cast<double>(d);
  const double mean = sum / static_cast<double>(durations.size());
  double variance = 0.0;
  for (long long d : durations) {
    const double diff = static_cast<double>(d) - mean;
    variance += diff * diff;
  }
  variance /= static_cast<double>(durations.size() - 1); // Bessel correction
  return std::sqrt(variance);
}

// ─────────────────────────────────────────────────────────────────────────────
// get90thPercentile
// ─────────────────────────────────────────────────────────────────────────────

long long CEETMonitor::get90thPercentile(TStringId paFBId) const {
  auto durations = getDurationsCopy(paFBId); // copy — we sort it
  if (durations.empty())
    return 0;
  std::sort(durations.begin(), durations.end());
  // Nearest-rank: ceil(0.9 * N) gives the 1-based rank.
  const size_t idx = static_cast<size_t>(std::ceil(0.9 * static_cast<double>(durations.size()))) - 1;
  return durations[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
// getMax
// ─────────────────────────────────────────────────────────────────────────────

long long CEETMonitor::getMax(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if (durations.empty())
    return 0;
  return *std::max_element(durations.begin(), durations.end());
}

// ─────────────────────────────────────────────────────────────────────────────
// getSampleCount
// ─────────────────────────────────────────────────────────────────────────────

size_t CEETMonitor::getSampleCount(TStringId paFBId) const {
  std::lock_guard<std::mutex> lock(mMutex);
  auto it = mSamples.find(paFBId);
  return (it != mSamples.end()) ? it->second.size() : 0u;
}

// ─────────────────────────────────────────────────────────────────────────────
// getDeadlineSuggestion
// ─────────────────────────────────────────────────────────────────────────────

long long CEETMonitor::getDeadlineSuggestion(TStringId paFBId, DeadlineStrategy strategy) const {
  switch (strategy) {
    case DeadlineStrategy::MAX: return getMax(paFBId);
    case DeadlineStrategy::P90:
      return static_cast<long long>(static_cast<double>(get90thPercentile(paFBId)) * mDeadlineMultiplier);
      // return get90thPercentile(paFBId);
    case DeadlineStrategy::MEAN_PLUS_3SIG: {
      const double mean = getMean(paFBId);
      const double sig = getStdDev(paFBId);
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
    if (activatedIt != mFETActivated.end() && activatedIt->second)
      return;

    auto samplesIt = mSamples.find(paFBId);
    if (samplesIt == mSamples.end() || samplesIt->second.size() < WARMUP_SAMPLES)
      return;

    mFETActivated[paFBId] = true;
  }

  const long long deadlineNs = getDeadlineSuggestion(paFBId, strategy);

  if (deadlineNs <= 0)
    return;

  setConfiguredDeadline(paFBId, deadlineNs);

  CFETMonitor::getInstance().registerFB(paFBId, std::chrono::nanoseconds(deadlineNs),
                                        [](TStringId paId) { DEVLOG_ERROR("FET deadline missed: %s\n", paId); });

  DEVLOG_INFO("EET-FET: activated deadline %lldns for '%s'\n", deadlineNs, paFBId);
}

// ─────────────────────────────────────────────────────────────────────────────
// clearData / clearAllData
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::clearData(TStringId paFBId) {
  std::lock_guard<std::mutex> lock(mMutex);
  mSamples.erase(paFBId);
  mStartTimes.erase(paFBId);
  mConfiguredDeadlines.erase(paFBId);
  mFETActivated.erase(paFBId);
}

void CEETMonitor::clearAllData() {
  std::lock_guard<std::mutex> lock(mMutex);
  mSamples.clear();
  mStartTimes.clear();
  mConfiguredDeadlines.clear();
  mFETActivated.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// exportCSV
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::exportCSV(TStringId paFBId, const std::string &paFileName) const {
  std::ofstream file(paFileName);
  if (!file.is_open())
    return;
  file << "sample,duration_ns\n";
  const auto data = getDurations(paFBId);
  for (size_t i = 0; i < data.size(); ++i) {
    file << i << "," << data[i] << "\n";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// exportAllCSV
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::exportAllCSV(const std::string &dir) const {

  std::map<TStringId, std::vector<Sample>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    snapshot = mSamples;
  }

  std::filesystem::create_directories(dir);

  for (const auto &[fbId, samples] : snapshot) {

    const std::string fileName = dir + "/" + std::string(fbId) + ".csv";

    std::ofstream file(fileName);
    if (!file.is_open())
      continue;

    file << "timestamp_ns,duration_ns,fet_active,deadline_miss,phase\n";

    for (const auto &s : samples) {
      file << s.timestamp.count() << "," << s.durationNs << "," << (s.fetActive ? 1 : 0) << ","
           << (s.deadlineMiss ? 1 : 0) << "," << static_cast<int>(s.phase) << "\n";
    }

    DEVLOG_INFO("EETMonitor: exported %zu samples for '%s'\n", samples.size(), fbId);
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

void CEETMonitor::startPeriodicExport(const std::string &paDirectory,
                                      std::chrono::seconds paInterval,
                                      size_t paTargetSamples) {
  if (mExportRunning.exchange(true))
    return;

  mExportThread = std::thread([this, paDirectory, paInterval, paTargetSamples]() {
    auto lastExport = std::chrono::steady_clock::now();
    const auto pollInterval = std::chrono::milliseconds(500);

    while (mExportRunning) {
      std::this_thread::sleep_for(pollInterval);
      if (!mExportRunning)
        break;

      // Check target — lock, check, release immediately.
      if (paTargetSamples > 0) {
        bool allDone = false;
        {
          std::lock_guard<std::mutex> lock(mMutex); // ← lock
          allDone = !mSamples.empty();
          for (const auto &[id, d] : mSamples) {
            if (d.size() < paTargetSamples) {
              allDone = false;
              break;
            }
          }
        } // ← lock released here

        if (allDone) {
          exportAllCSV(paDirectory); // ← called WITHOUT holding lock
          DEVLOG_INFO("EETMonitor: target of %zu samples reached -> stopping.\n", paTargetSamples);
          mExportRunning = false;
          break;
        }
      }

      // Periodic export — also called without holding lock.
      auto now = std::chrono::steady_clock::now();
      if (now - lastExport >= paInterval) {
        exportAllCSV(paDirectory);
        lastExport = now;
      }
    }
  });
}

void CEETMonitor::stopPeriodicExport() {
  mExportRunning = false;
  if (mExportThread.joinable()) {
    mExportThread.join();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// getDurationsCopy  (private helper)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<long long> CEETMonitor::getDurationsCopy(TStringId paFBId) const {
  std::lock_guard<std::mutex> lock(mMutex);

  std::vector<long long> result;

  auto it = mSamples.find(paFBId);
  if (it == mSamples.end())
    return {};

  result.reserve(it->second.size());

  for (const auto &s : it->second) {
    result.push_back(s.durationNs);
  }

  return result;
}
