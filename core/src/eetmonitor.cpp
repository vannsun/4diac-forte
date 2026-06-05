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

  const auto now = Clock::now();
  mStartTimes[paFBId] = now; // always record for EET

  // Only record enforced start if FET is already active.
  // Before activation, onEnforced is null so endMeasurementEnforced
  // would never be called — recording here just creates stale entries.
  auto fetIt = mFETActivated.find(paFBId);
  const bool fetActive = (fetIt != mFETActivated.end() && fetIt->second);
  if (fetActive) {
    mStartTimesEnforced[paFBId] = now;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// endMeasurement
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::endMeasurement(TStringId paFBId) {
  if (forte::eet::isMonitoringExcluded(paFBId))
    return;
  const auto endTime = Clock::now();

  bool shouldActivate = false;
  {
    std::lock_guard<std::mutex> lock(mMutex);

    auto startIt = mStartTimes.find(paFBId);
    if (startIt == mStartTimes.end())
      return;

    const long long durationNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startIt->second).count();

    // Read FET state before deciding whether to erase start time.
    auto fetIt = mFETActivated.find(paFBId);
    const bool fetActive = (fetIt != mFETActivated.end() && fetIt->second);

    // Only erase when FET is NOT active.
    // the same start time to measure execution + padding duration.
    if (!fetActive) {
      mStartTimes.erase(startIt);
    }

    if (durationNs <= 0)
      return;

    const size_t warmupCount = ++mWarmupCount[paFBId];

    long long deadlineNs = 0;
    auto dlIt = mConfiguredDeadlines.find(paFBId);
    if (dlIt != mConfiguredDeadlines.end()) {
      deadlineNs = dlIt->second;
    }

    const ExecutionPhase phase = fetActive ? ExecutionPhase::FET_ACTIVE : ExecutionPhase::WARMUP;

    Sample s;
    s.durationNs = durationNs;
    s.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime.time_since_epoch()).count();
    s.deadlineNs = deadlineNs;
    s.fetActive = fetActive;
    s.deadlineMiss = fetActive && deadlineNs > 0 && durationNs > deadlineNs;
    s.phase = phase;

    auto &samples = mSamples[paFBId];
    if (samples.size() >= MAX_SAMPLES) {
      samples.erase(samples.begin());
    }
    samples.push_back(s);

    if (!fetActive) {
      shouldActivate = (warmupCount >= WARMUP_SAMPLES);
    }
  }

  if (shouldActivate) {
    activateFET(paFBId, mDefaultStrategy);
  }
}

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

void CEETMonitor::endMeasurementEnforced(TStringId paFBId) {
  if(forte::eet::isMonitoringExcluded(paFBId)) return;

  const auto endTime = Clock::now();
  std::lock_guard<std::mutex> lock(mMutex);

  auto startIt = mStartTimesEnforced.find(paFBId);
  if(startIt == mStartTimesEnforced.end()) return;

  const long long durationNs =
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      endTime - startIt->second).count();

  mStartTimesEnforced.erase(startIt);

  if(durationNs <= 0) return;

  // Sanity check — enforced duration cannot exceed deadline by more than
  // a reasonable margin. Stale timestamps produce multi-second values.
  // Max plausible enforced duration = 10 × deadline.
  auto dlIt = mConfiguredDeadlines.find(paFBId);
  if(dlIt != mConfiguredDeadlines.end() && dlIt->second > 0) {
    if(durationNs > dlIt->second * 10) {
      DEVLOG_WARNING("EETMonitor: discarding stale enforced sample for '%s': "
                     "%lldns >> deadline %lldns\n",
                     paFBId, durationNs, dlIt->second);
      return;
    }
  }

  // Read FET state directly — no nested lock.
  auto fetIt = mFETActivated.find(paFBId);
  const bool fetActive = (fetIt != mFETActivated.end() && fetIt->second);

  long long deadlineNs = 0;
  if(dlIt != mConfiguredDeadlines.end()) {
    deadlineNs = dlIt->second;
  }

  Sample s;
  s.durationNs   = durationNs;
  s.timestampNs  = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     endTime.time_since_epoch()).count();
  s.deadlineNs   = deadlineNs;
  s.fetActive    = fetActive;
  s.deadlineMiss = fetActive && deadlineNs > 0 && durationNs > deadlineNs;
  s.phase        = fetActive ? ExecutionPhase::FET_ACTIVE
                             : ExecutionPhase::WARMUP;

  auto& samples = mSamplesEnforced[paFBId];
  if(samples.size() >= MAX_SAMPLES) {
    samples.erase(samples.begin());
  }
  samples.push_back(s);
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

  // CFETMonitor::getInstance().registerFB(paFBId, std::chrono::nanoseconds(deadlineNs),
  //                                      [](TStringId paId) { DEVLOG_ERROR("FET deadline missed: %s\n", paId); });

  CFETMonitor::getInstance().registerFB(
      paFBId, std::chrono::nanoseconds(deadlineNs),
      [](TStringId paId) { DEVLOG_ERROR("FET deadline missed: %s\n", paId); },
      [paFBId]() {
        // Called after waitUntilDeadline completes (sleep or violation)
        CEETMonitor::getInstance().endMeasurementEnforced(paFBId);
      });

  DEVLOG_INFO("EET-FET: activated deadline %lldns for '%s'\n", deadlineNs, paFBId);
}

// ─────────────────────────────────────────────────────────────────────────────
// clearData / clearAllData
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::clearData(TStringId paFBId) {
  std::lock_guard<std::mutex> lock(mMutex);
  mSamples.erase(paFBId);
  mSamplesEnforced.erase(paFBId);
  mStartTimes.erase(paFBId);
  mStartTimesEnforced.erase(paFBId);
  mConfiguredDeadlines.erase(paFBId);
  mFETActivated.erase(paFBId);
  mWarmupCount.erase(paFBId);
}

void CEETMonitor::clearAllData() {
  std::lock_guard<std::mutex> lock(mMutex);
  mSamples.clear();
  mSamplesEnforced.clear();
  mStartTimes.clear();
  mStartTimesEnforced.clear();
  mConfiguredDeadlines.clear();
  mFETActivated.clear();
  mWarmupCount.clear();
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

void CEETMonitor::exportAllCSV(const std::string &paDirectory) const {
  std::map<TStringId, std::vector<Sample>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    snapshot = mSamples;
  }

  std::filesystem::create_directories(paDirectory);
  for (const auto &[fbId, samples] : snapshot) {
    const std::string filename = paDirectory + "/" + std::string(fbId) + ".csv";
    std::ofstream file(filename);
    if (!file.is_open())
      continue;

    file << "timestamp_ns,execution_ns,deadline_ns,deadline_miss,fet_active,phase\n";
    for (const auto &s : samples) {
      file << s.timestampNs << "," << s.durationNs << "," << s.deadlineNs << "," << (s.deadlineMiss ? 1 : 0) << ","
           << (s.fetActive ? 1 : 0) << "," << static_cast<int>(s.phase) << "\n";
    }
    DEVLOG_INFO("EETMonitor: exported %zu samples for '%s'\n", samples.size(), fbId);
  }
}
// ─────────────────────────────────────────────────────────────────────────────
// exportAllCSVEnforced
// ─────────────────────────────────────────────────────────────────────────────
void CEETMonitor::exportAllCSVEnforced(const std::string &paDirectory) const {
  std::map<TStringId, std::vector<Sample>> snapshot;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    snapshot = mSamplesEnforced;
  }

  std::filesystem::create_directories(paDirectory);
  for (const auto &[fbId, samples] : snapshot) {
    const std::string filename = paDirectory + "/" + std::string(fbId) + ".csv";
    std::ofstream file(filename);
    if (!file.is_open())
      continue;
    file << "timestamp_ns,execution_ns,deadline_ns,deadline_miss,fet_active,phase\n";
    for (const auto &s : samples) {
      file << s.timestampNs << "," << s.durationNs << "," << s.deadlineNs << "," << (s.deadlineMiss ? 1 : 0) << ","
           << (s.fetActive ? 1 : 0) << "," << static_cast<int>(s.phase) << "\n";
    }
    DEVLOG_INFO("EETMonitor: exported %zu enforced samples for '%s'\n", samples.size(), fbId);
  }
}
// ─────────────────────────────────────────────────────────────────────────────
// startPeriodicExport / stopPeriodicExport
// ─────────────────────────────────────────────────────────────────────────────

void CEETMonitor::startPeriodicExport(const std::string &paDirectory,
                                      const std::string &paDirectoryEnforced,
                                      std::chrono::seconds paInterval,
                                      size_t paTargetSamples) {
  if (mExportRunning.exchange(true))
    return;

  mExportThread = std::thread([this, paDirectory, paDirectoryEnforced, paInterval, paTargetSamples]() {
    auto lastExport = std::chrono::steady_clock::now();
    const auto pollInterval = std::chrono::milliseconds(500);

    while (mExportRunning) {
      std::this_thread::sleep_for(pollInterval);
      if (!mExportRunning)
        break;

      if (paTargetSamples > 0) {
        bool allDone = false;
        {
          std::lock_guard<std::mutex> lock(mMutex);
          allDone = !mSamples.empty();
          for (const auto &[id, s] : mSamples) {
            if (s.size() < paTargetSamples) {
              allDone = false;
              break;
            }
          }
        }
        if (allDone) {
          exportAllCSV(paDirectory);
          exportAllCSVEnforced(paDirectoryEnforced);
          DEVLOG_INFO("EETMonitor: target of %zu samples reached -> stopping.\n", paTargetSamples);
          mExportRunning = false;
          break;
        }
      }

      auto now = std::chrono::steady_clock::now();
      if (now - lastExport >= paInterval) {
        exportAllCSV(paDirectory);
        exportAllCSVEnforced(paDirectoryEnforced);
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
  auto it = mSamples.find(paFBId);
  if (it == mSamples.end())
    return {};
  std::vector<long long> result;
  result.reserve(it->second.size());
  for (const auto &s : it->second) {
    result.push_back(s.durationNs);
  }
  return result;
}
