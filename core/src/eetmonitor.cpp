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

#include <filesystem>
#include <fstream>
#include "forte/eetconfig.h"
#include "forte/eetmonitor.h"
#include "forte/util/devlog.h"

static const char *deadlineStrategyToString(CEETMonitor::DeadlineStrategy strategy) {
  switch (strategy) {
    case CEETMonitor::DeadlineStrategy::MAX: return "MAX";
    case CEETMonitor::DeadlineStrategy::P90: return "P90";
    case CEETMonitor::DeadlineStrategy::MEAN_PLUS_3SIG: return "MEAN_PLUS_3SIG";
    default: return "UNKNOWN";
  }
}

void CEETMonitor::startMeasurement(TStringId paFBId) {
  if (forte::eet::isMonitoringExcluded(paFBId))
    return;
  std::lock_guard<std::mutex> lock(mMutex);

  const auto now = Clock::now();
  mStartTimes[paFBId] = now;
}

void CEETMonitor::endMeasurement(TStringId paFBId) {
  if (forte::eet::isMonitoringExcluded(paFBId))
    return;
  const auto endTime = Clock::now();

  bool shouldActivate = false;
  Sample s;
  {
    std::lock_guard<std::mutex> lock(mMutex);

    auto startIt = mStartTimes.find(paFBId);
    if (startIt == mStartTimes.end())
      return;

    const long long durationNs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startIt->second).count();

    // Read FET state before deciding whether to erase start time.
    auto fetIt = mFETActivated.find(paFBId);
    const bool fetActive = (fetIt != mFETActivated.end() && fetIt->second.active);
    const long long deadlineNs = fetActive ? fetIt->second.deadlineNs : 0;

    // Only erase when FET is NOT active.
    // the same start time to measure execution + padding duration.
    if (!fetActive) {
      mStartTimes.erase(startIt);
    }

    if (durationNs <= 0)
      return;

    const size_t warmupCount = ++mWarmupCount[paFBId];

    const ExecutionPhase phase = fetActive ? ExecutionPhase::FET_ACTIVE : ExecutionPhase::WARMUP;

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

double CEETMonitor::getMean(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if (durations.empty())
    return 0.0;
  double sum = 0.0;
  for (long long d : durations)
    sum += static_cast<double>(d);
  return sum / static_cast<double>(durations.size());
}

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

long long CEETMonitor::get90thPercentile(TStringId paFBId) const {
  auto durations = getDurationsCopy(paFBId); // copy — we sort it
  if (durations.empty())
    return 0;
  std::sort(durations.begin(), durations.end());
  // Nearest-rank: ceil(0.9 * N) gives the 1-based rank.
  const size_t idx = static_cast<size_t>(std::ceil(0.9 * static_cast<double>(durations.size()))) - 1;
  return durations[idx];
}

long long CEETMonitor::getMax(TStringId paFBId) const {
  const auto durations = getDurationsCopy(paFBId);
  if (durations.empty())
    return 0;
  return *std::max_element(durations.begin(), durations.end());
}

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

void CEETMonitor::activateFET(TStringId paFBId, DeadlineStrategy strategy) {
  {
    std::lock_guard<std::mutex> lock(mMutex);

    // Already activated — do not re-register.
    auto activatedIt = mFETActivated.find(paFBId);
    if (activatedIt != mFETActivated.end() && activatedIt->second.active)
      return;

    // Not enough warmup samples yet.
    auto countIt = mWarmupCount.find(paFBId);
    if (countIt == mWarmupCount.end() || countIt->second < WARMUP_SAMPLES)
      return;

    // Mark as active.
    mFETActivated[paFBId].active = true;
  }

  // Compute deadline outside lock — stat helpers take their own lock.
  const long long deadlineNs = getDeadlineSuggestion(paFBId, strategy);
  if (deadlineNs <= 0)
    return;

  // Store deadline in FETState — single map, no mConfiguredDeadlines needed.
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mFETActivated[paFBId].deadlineNs = deadlineNs;
  }

  DEVLOG_INFO("EET-FET: activated deadline %lldns for '%s' after %zu warmup samples using strategy %s\n", deadlineNs,
              paFBId, static_cast<size_t>(WARMUP_SAMPLES), deadlineStrategyToString(strategy));
}

// #ifdef FORTE_EET_EVALUATION
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

void CEETMonitor::startPeriodicExport(const std::string &paDirectory,
                                      const std::string &paDirectoryEnforced,
                                      std::chrono::seconds paInterval,
                                      size_t paTargetSamples) {
  if (mExportRunning.exchange(true))
    return;

  mExportThread = std::thread([this, paDirectory, paDirectoryEnforced, paInterval, paTargetSamples]() {
    auto lastExport = std::chrono::steady_clock::now();

    // Makes the thread wake up every 500ms (0,5s) to check whether the target has been reached
    // or the periodic export interval has elapsed, instead of sleeping for the full interval at once.
    const auto pollInterval = std::chrono::milliseconds(500);

    while (mExportRunning) {
      std::this_thread::sleep_for(pollInterval);
      if (!mExportRunning)
        break;

      const auto now = std::chrono::steady_clock::now();

      // ── Target check ────────────────────────────────────────────────────
      if (paTargetSamples > 0) {
        bool allDone = false;
        size_t minCount = SIZE_MAX;
        {
          std::lock_guard<std::mutex> lock(mMutex);
          if (!mSamples.empty()) {
            allDone = true;
            for (const auto &[id, s] : mSamples) {
              minCount = std::min(minCount, s.size());
              if (s.size() < paTargetSamples) {
                allDone = false;
              }
            }
          }
        }

        // Progress log every 30 seconds.
        if (now - lastExport >= std::chrono::seconds(30)) {
          DEVLOG_INFO("EETMonitor: progress - min samples across FBs: %zu / %zu\n", minCount == SIZE_MAX ? 0 : minCount,
                      paTargetSamples);
        }

        if (allDone) {
          exportAllCSV(paDirectory);
          DEVLOG_INFO("EETMonitor: target of %zu samples reached — stopping.\n", paTargetSamples);
          mExportRunning = false;
          break;
        }
      }

      // ── Periodic safety-net export ───────────────────────────────────────
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

// #endif // FORTE_EET_EVALUATION

/** Private **/

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
