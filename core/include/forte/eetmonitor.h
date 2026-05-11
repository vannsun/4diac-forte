#ifndef _EETMONITOR_H_
#define _EETMONITOR_H_

#include <chrono>
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <cstdint>

/*! \brief Portable alias for FORTE's CStringDictionary::TStringId.
 *
 * In standalone builds this is a plain uint32_t.
 * When integrating into FORTE, replace this block with:
 *
 *   #include <forte_config.h>
 *   #include <stringdict.h>
 *   #include "utils/singlet.h"
 *
 * and replace TStringId with CStringDictionary::TStringId throughout,
 * and replace the singleton implementation with DECLARE_SINGLETON(CEETMonitor).
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
 */
class CEETMonitor {
public:
  /*! \brief Access the singleton instance. */
  static CEETMonitor& getInstance() {
    static CEETMonitor instance;
    return instance;
  }

  CEETMonitor(const CEETMonitor&) = delete;
  CEETMonitor& operator=(const CEETMonitor&) = delete;

  /*! \brief Maximum number of duration samples stored per FB (sliding window). */
  static constexpr size_t MAX_SAMPLES = 10000;

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

private:
  CEETMonitor() = default;

  /*! \brief Retrieve a copy of the duration vector for paFBId under lock.
   *
   * Caller must NOT already hold mMutex.
   * Returns an empty vector if paFBId is not found.
   */
  std::vector<long long> getDurationsCopy(TStringId paFBId) const;

  // Protects mDurations and mStartTimes for concurrent access.
  mutable std::mutex mMutex;

  // Per-FB list of completed execution durations in nanoseconds (insertion order).
  // Capped at MAX_SAMPLES via a sliding window.
  std::map<TStringId, std::vector<long long>> mDurations;

  // Per-FB start timestamp for the measurement currently in progress.
  // Entry is erased by endMeasurement once the duration has been recorded.
  std::map<TStringId, std::chrono::high_resolution_clock::time_point> mStartTimes;
};

#endif // _EETMONITOR_H_