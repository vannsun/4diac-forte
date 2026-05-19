#ifndef _EETMONITOR_H_
#define _EETMONITOR_H_

#include <chrono>
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <thread>
#include <atomic>

/*! \brief Portable alias for FORTE's CStringDictionary::TStringId.
 *
 * In standalone builds this is const char* (FORTE interns all strings so
 * pointer equality is identity equality).
 * When integrating into FORTE, replace with CStringDictionary::TStringId.
 */
using TStringId = const char*;

/*! \ingroup CORE \brief Singleton class for monitoring Estimated Execution Time (EET) of Function Blocks.
 *
 * Records timestamps when input events arrive and output events are produced,
 * computes durations, builds histograms, and provides statistical analysis.
 *
 * Thread safety: all public methods are protected by an internal mutex.
 * Safe to call from concurrent FB execution threads.
 *
 * Memory: each FB accumulates up to MAX_SAMPLES durations in a sliding window.
 * Older samples are dropped once the cap is reached.
 *
 * FORTE integration notes:
 *   - Replace TStringId with CStringDictionary::TStringId
 *   - Replace singleton implementation with DECLARE_SINGLETON(CEETMonitor)
 *   - Add DEFINE_SINGLETON(CEETMonitor) in eetmonitor.cpp
 *   - Add #include <forte_config.h>, <stringdict.h>, "utils/singlet.h"
 */
class CEETMonitor {
public:

  /*! \brief Strategy used to compute the deadline suggestion from collected samples.
   *
   *  MAX             — strictest: never allow above observed worst case
   *  P90             — 90th percentile: ignores outliers, good general default
   *  MEAN_PLUS_3SIG  — mean + 3*stddev: statistically principled (~99.7% coverage)
   */
  enum class DeadlineStrategy {
    MAX,
    P90,
    MEAN_PLUS_3SIG
  };

  /*! \brief Access the singleton instance. */
  static CEETMonitor& getInstance() {
    static CEETMonitor instance;
    return instance;
  }

  CEETMonitor(const CEETMonitor&) = delete;
  CEETMonitor& operator=(const CEETMonitor&) = delete;

  /*! \brief Maximum number of duration samples stored per FB (sliding window). */
  static constexpr size_t MAX_SAMPLES = 5000;

  /*! \brief Number of samples collected before activateFET() considers data stable. */
  //static constexpr size_t WARMUP_SAMPLES = 1000;
  // In eetmonitor.h:
#ifdef NDEBUG
  static constexpr size_t WARMUP_SAMPLES = 100;
#else
  static constexpr size_t WARMUP_SAMPLES = 10;   //10: fast activation in debug
#endif

  /*! \brief Start timing for a Function Block's execution.
   *
   * Call this when a triggering input event arrives (e.g., in receiveInputEvent).
   * If a measurement for this FB is already in progress it is silently overwritten,
   * which handles the case where a previous endMeasurement was never called.
   *
   * \param paFBId The FB's instance name ID.
   */
  void startMeasurement(TStringId paFBId);

  /*! \brief End timing for a Function Block's execution.
   *
   * Call this when the FB produces an output event (e.g., in sendOutputEvent).
   * Computes the duration and appends it to the histogram for paFBId.
   * If no matching startMeasurement exists for paFBId this call is a safe no-op.
   *
   * \param paFBId The FB's instance name ID.
   */
  void endMeasurement(TStringId paFBId);

  /*! \brief Get the stored duration samples (nanoseconds) for a FB.
   *
   * Returns a snapshot copy so the caller is not affected by concurrent updates.
   * Returns an empty vector if paFBId has no recorded data.
   *
   * \param paFBId The FB's instance name ID.
   * \return Copy of the duration vector in insertion order.
   */
  std::vector<long long> getDurations(TStringId paFBId) const;

  /*! \brief Compute the mean execution time for a FB.
   *
   * \param paFBId The FB's instance name ID.
   * \return Mean in nanoseconds, or 0.0 if no data.
   */
  double getMean(TStringId paFBId) const;

  /*! \brief Compute the standard deviation of execution times for a FB.
   *
   * Uses the sample standard deviation (Bessel's correction, N-1 denominator).
   *
   * \param paFBId The FB's instance name ID.
   * \return Standard deviation in nanoseconds, or 0.0 if fewer than 2 samples.
   */
  double getStdDev(TStringId paFBId) const;

  /*! \brief Compute the 90th percentile execution time for a FB.
   *
   * Works on a sorted copy of the duration data; insertion order is preserved.
   *
   * \param paFBId The FB's instance name ID.
   * \return 90th percentile in nanoseconds, or 0 if no data.
   */
  long long get90thPercentile(TStringId paFBId) const;

  /*! \brief Get the maximum execution time recorded for a FB.
   *
   * \param paFBId The FB's instance name ID.
   * \return Maximum in nanoseconds, or 0 if no data.
   */
  long long getMax(TStringId paFBId) const;

  /*! \brief Return the number of completed measurements stored for a FB.
   *
   * \param paFBId The FB's instance name ID.
   * \return Sample count, or 0 if paFBId is unknown.
   */
  size_t getSampleCount(TStringId paFBId) const;

  /*! \brief Clear all stored data for a specific FB.
   *
   * Removes both completed durations and any in-progress start timestamp.
   *
   * \param paFBId The FB's instance name ID.
   */
  void clearData(TStringId paFBId);

  /*! \brief Clear all stored data for all FBs. */
  void clearAllData();

  /*! \brief Export duration data for a specific FB to a CSV file.
   *
   * \param paFBId     The FB's instance name ID.
   * \param paFileName Output file path.
   */
  void exportCSV(TStringId paFBId, const std::string& paFileName) const;

  /*! \brief Export duration data for all FBs to individual CSV files.
   *
   * Creates one file per FB named <fbId>.csv inside paDirectory.
   * The directory is created if it does not exist.
   *
   * \param paDirectory Output directory path.
   */
  void exportAllCSV(const std::string& paDirectory) const;

  /*! \brief Compute a deadline suggestion from collected samples.
   *
   * Returns 0 if there is not enough data yet.
   *
   * \param paFBId    The FB's instance name ID.
   * \param strategy  Which statistical method to use (see DeadlineStrategy).
   * \return Suggested deadline in nanoseconds.
   */
  long long getDeadlineSuggestion(TStringId paFBId,
                                  DeadlineStrategy strategy) const;

  /*! \brief Store a configured deadline for a FB (for logging / export).
   *
   * Called automatically by activateFET(). Can also be called manually.
   *
   * \param paFBId       The FB's instance name ID.
   * \param paDeadlineNs Deadline in nanoseconds.
   */
  void setConfiguredDeadline(TStringId paFBId, long long paDeadlineNs);

  /*! \brief Retrieve the stored configured deadline for a FB.
   *
   * \param paFBId The FB's instance name ID.
   * \return Deadline in nanoseconds, or 0 if not set.
   */
  long long getConfiguredDeadline(TStringId paFBId) const;

  /*! \brief Compute deadline from EET data and register the FB with CFETMonitor.
   *
   * Called automatically from receiveInputEvent after WARMUP_SAMPLES have been
   * collected. Can also be called manually at any time after warmup.
   *
   * The deadline is computed using the chosen strategy, stored via
   * setConfiguredDeadline(), and passed directly to CFETMonitor::registerFB().
   * From this point forward FET enforces the deadline for this FB.
   *
   * No-op if fewer than WARMUP_SAMPLES have been collected.
   *
   * \param paFBId    The FB's instance name ID.
   * \param strategy  Which statistical method to use. Default: MEAN_PLUS_3SIG.
   */
  void activateFET(TStringId paFBId,
                   DeadlineStrategy strategy = DeadlineStrategy::MEAN_PLUS_3SIG);


  void startPeriodicExport(const std::string& paDirectory,
                                       std::chrono::seconds paInterval,
                                       size_t paTargetSamples = 0);

  void stopPeriodicExport();

private:
  CEETMonitor() = default;

  /*! \brief Retrieve a copy of the duration vector for paFBId under lock.
   *
   * Caller must NOT already hold mMutex.
   * Returns an empty vector if paFBId is not found.
   */
  std::vector<long long> getDurationsCopy(TStringId paFBId) const;

  using Clock = std::chrono::high_resolution_clock;

  // Protects mDurations, mStartTimes and mConfiguredDeadlines for concurrent access.
  mutable std::mutex mMutex;

  // Per-FB list of completed execution durations in nanoseconds (insertion order).
  // Capped at MAX_SAMPLES via a sliding window.
  std::map<TStringId, std::vector<long long>> mDurations;

  // Per-FB start timestamp for the measurement currently in progress.
  // Entry is erased by endMeasurement once the duration has been recorded.
  std::map<TStringId, Clock::time_point> mStartTimes;

  // Per-FB deadline computed by activateFET() and stored for logging/export.
  std::unordered_map<TStringId, long long> mConfiguredDeadlines;

  // Per-FB flag: true once activateFET() has been called for this FB.
  // Prevents re-registration on every subsequent event after warmup.
  std::unordered_map<TStringId, bool> mFETActivated;

  std::thread  mExportThread;
  std::atomic<bool> mExportRunning{false};

  DeadlineStrategy mDefaultStrategy{DeadlineStrategy::MEAN_PLUS_3SIG};
};

#endif // _EETMONITOR_H_