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

using TStringId = const char *;

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
 */
class CEETMonitor {
  public:
    /*! \brief Strategy used to compute the deadline suggestion from collected samples.
     *
     *  MAX             — observed worst case (WCET)
     *  P90             — 90th percentile, ignores outliers
     *  MEAN_PLUS_3SIG  — mean + 3*stddev statistically ~99.7% coverage
     */
    enum class DeadlineStrategy { MAX, P90, MEAN_PLUS_3SIG };

    /*! \brief Access the singleton instance. */
    static CEETMonitor &getInstance() {
      static CEETMonitor instance;
      return instance;
    }

    CEETMonitor(const CEETMonitor &) = delete;
    CEETMonitor &operator=(const CEETMonitor &) = delete;

    /*! \brief Maximum number of samples stored per FB in the sliding window.
     *
     * Controls how many samples are retained in memory, once the MAX_SAMPLES
     * is exceded, the oldest samples start being discarded
     *  Used by FORTE_EET_MONITORING.
     */
    static constexpr size_t MAX_SAMPLES = 5000;

    /*! \brief Number of samples collected before FET deadline is derived and activated.
     * Controls when FET is activated
     * Used by FORTE_EET_MONITORING + FORTE_FET_ENFORCEMENT.
     * */
    static constexpr size_t WARMUP_SAMPLES = 2000;

    /*! \brief Deadline derivation strategy applied at FET activation.
     *
     * Can be overridden before startDevice() in forteinstance.cpp.
     */
    DeadlineStrategy mDefaultStrategy{DeadlineStrategy::P90};

    /*! \brief Multiplier applied to the derived deadline (e.g. P90 × 1.2).
     *
     *  Adds headroom above the measured percentile.
     */
    double mDeadlineMultiplier = 1.2;

    /*! \brief Execution phase of a recorded sample. */
    enum class ExecutionPhase {
      WARMUP, ///< Collected before FET activation (no enforcement)
      FET_ACTIVE, ///< Collected after FET activation (enforcement active)
      ENFORCED ///< Collected after waitUntilDeadline (includes FET sleep)
    };

    /*! \brief Single EET measurement sample with metadata. */
    struct Sample {
        long long durationNs; ///< Measured execution time in nanoseconds
        long long timestampNs; ///< Wall-clock timestamp at end of measurement
        long long deadlineNs; ///< Registered FET deadline at time of measurement (0 if not yet activated)
        bool fetActive; ///< True if FET was active when this sample was recorded
        bool deadlineMiss; ///< True if durationNs exceeded deadlineNs
        ExecutionPhase phase; ///< Warmup, enforcement-active, or enforced (post-sleep)
    };

    /*! \brief Start timing for a Function Block's execution.
     *
     * Called when a triggering input event arrives (receiveInputEvent).
     * \param paFBId The FB's instance name ID.
     */
    void startMeasurement(TStringId paFBId);

    /*! \brief End timing for a Function Block's after enforcement.
     *
     * Called when the FB produces an output event (sendOutputEvent).
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

    /*! \brief Compute a deadline suggestion from collected samples.
     *
     * Returns 0 if there is not enough data yet.
     *
     * \param paFBId    The FB's instance name ID.
     * \param strategy  Which statistical method to use (see DeadlineStrategy).
     * \return Suggested deadline in nanoseconds.
     */
    long long getDeadlineSuggestion(TStringId paFBId, DeadlineStrategy strategy) const;

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
    void activateFET(TStringId paFBId, DeadlineStrategy strategy = DeadlineStrategy::MEAN_PLUS_3SIG);

    // #ifdef FORTE_EET_EVALUATION
    /*! \brief Export duration data for a specific FB to a CSV file.
     *
     * \param paFBId     The FB's instance name ID.
     * \param paFileName Output file path.
     */
    void exportCSV(TStringId paFBId, const std::string &paFileName) const;

    /*! \brief Export duration data for all FBs to individual CSV files.
     *
     * Creates one file per FB named <fbId>.csv inside paDirectory.
     * The directory is created if it does not exist.
     *
     * \param paDirectory Output directory path.
     */
    void exportAllCSV(const std::string &paDirectory) const;

    /*! \brief Starts a background thread that periodically exports EET and enforced samples to CSV.
     *
     * Stops automatically when paTargetSamples is reached by all FBs,
     * or when stopPeriodicExport() is called. Only active under FORTE_EET_EVALUATION.
     *
     *  \param paDirectory          Output directory for raw EET samples (eet_results).
     *  \param paDirectoryEnforced  Output directory for enforced samples (eet_results_enforced).
     *  \param paInterval           Interval between periodic exports (safety net).
     *  \param paTargetSamples      Stop when all FBs reach this count (0 = run indefinitely). */
    void startPeriodicExport(const std::string &paDirectory,
                             const std::string &paDirectoryEnforced,
                             std::chrono::seconds paInterval,
                             size_t paTargetSamples = 0);

    /*! \brief Stops the periodic export thread and waits for it to finish. */
    void stopPeriodicExport();
    // #endif // FORTE_EET_EVALUATION

  private:
    CEETMonitor() = default;

    /*! \brief Retrieve a copy of the duration vector for paFBId under lock.
     *
     * Caller must NOT already hold mMutex.
     * Returns an empty vector if paFBId is not found.
     */
    std::vector<long long> getDurationsCopy(TStringId paFBId) const;

    /*! \brief Monotonic high-resolution clock used for all EET timestamps. */
    using Clock = std::chrono::high_resolution_clock;

    /*! \brief Protects all mutable state against concurrent access from
     *  the ECET thread and the periodic export thread. (mSamples, mStartTimes)
     */
    mutable std::mutex mMutex;

    /*! \brief Per-FB sliding window of EET samples (raw algorithm time).
     *
     * Capped at MAX_SAMPLES. Includes phase and deadline metadata per sample.
     */
    std::map<TStringId, std::vector<Sample>> mSamples;

    /*! \brief Per-FB real-time (wall-clock) start timestamp for the measurement in progress.
     *  Set by startMeasurement(), erased by endMeasurement(). */
    std::map<TStringId, Clock::time_point> mStartTimes;

    /*! \brief Per-FB activation flag. True once activateFET() has registered
     *  this FB with CFETMonitor. Prevents re-registration on subsequent events. */
    struct FETState {
        bool active = false;
        long long deadlineNs = 0;
    };
    std::map<TStringId, FETState> mFETActivated;

    /*! \brief Per-FB total sample count since device start, unbounded by MAX_SAMPLES.
     * Used as the warmup counter unlike mSamples.size() this never shrinks
     * when the sliding window evicts old entries. */
    std::map<TStringId, size_t> mWarmupCount;

    // #ifdef FORTE_EET_EVALUATION
    /*! \brief Background thread that periodically writes CSVs to disk. */
    std::thread mExportThread;

    /*! \brief True while the export thread is running. Set to false to
     *  request graceful shutdown; stopPeriodicExport() blocks until exit. */
    std::atomic<bool> mExportRunning{false};
    // #endif
};

#endif // _EETMONITOR_H_
