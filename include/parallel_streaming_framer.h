#pragma once

/**
 * @file ParallelStreamingFramer.h
 * @brief Multi-threaded, streaming Hit-framer that pulls hits from the
 *        per-FIFO @ref AlcorDataStreamer instances and groups them into
 *        fixed-width readout frames (default: 1024 clock cycles).
 *
 * The framer is the workhorse of the lightdata pipeline: it merges hits from
 * up to 192 streams, applies trigger gating, and emits one frame per spill
 * with the category bookkeeping handed off to @c lightdata_writer.
 *
 * @par Threading
 * The framer holds three mutexes.  The canonical acquisition order — the
 * only order that avoids deadlock — is:
 *   1. @c frame_mutexes_access  (controls the per-stream frame map)
 *   2. @c triggers_map_mutex    (controls the in-flight trigger queue)
 *   3. @c spilldata_masks_mutex (controls dead-channel / participation masks)
 * Lower-numbered mutexes are acquired first; release happens in reverse.
 * Worker threads only ever take @c frame_mutexes_access; the writer thread
 * is the only consumer that may need #2 and #3.
 */

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include "TFile.h"
#include "TTree.h"
#include "TH1.h"
#include "TH2.h"
#include "TSystem.h"
#include <filesystem>
#include "alcor_spilldata.h"
#include "alcor_data_streamer.h"
#include "utility/config_reader.h"
#include <mist/logger/progress_bar.h>
#include <mist/logger/multi_progress_bar.h>

/// @defgroup FramerConfig Framer Configuration Constants
/// @{

/** @brief Number of clock cycles per frame */
#define BTANA_FRAME_SIZE 1024

/** @brief Frame duration in nanoseconds (3.125ns per clock cycle at 320MHz) */
#define BTANA_FRAME_LENGTH_NS (BTANA_FRAME_SIZE * 3.125)

/** @brief Number of initial frames reserved for trigger synchronization */
#define BTANA_FIRST_FRAMES_TRIGGER 5000

/** @brief Afterpulse suppression deadtime in clock cycles (~200ns) */
#define BTANA_AFTERPULSE_DEADTIME 64

/** @brief Window for secondary-trigger detection in clock cycles (~625ns) */
#define BTANA_TRIGGER_SECONDARY_WINDOW 200

/// @}

/**
 * @class ParallelStreamingFramer
 * @brief Frames raw ALCOR data streams from multiple input files in parallel.
 *
 * This class manages multiple @ref AlcorDataStreamer instances, one per input file,
 * and processes them concurrently to produce framed spill data. It supports optional
 * trigger and readout configuration and afterpulse suppression.
 *
 * A per-stream, per-spill rollover-offset correction table can be built via
 * @ref resolve_rollover_offsets() before the first call to @ref next_spill().
 * The correction is applied uniformly to both ALCOR hits and trigger hits during
 * @ref process() and realigns streams whose gRollover endpoint lags the run-wide
 * maximum for a given spill.
 *
 * @note Frame size and timing constants are defined via macros and should eventually
 *       be moved to a config file with a proper pass-down mechanism to @ref AlcorSpilldata.
 */
class ParallelStreamingFramer
{
public:
    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------

    /** @brief Default constructor. */
    ParallelStreamingFramer() = default;

    /** @brief Destructor.  Histograms are RAII-owned (RootHist members); no manual delete. */
    ~ParallelStreamingFramer() = default;

    /**
     * @brief Construct a framer from a list of input files.
     * @param filenames  Paths to the raw ALCOR data files to stream.
     * @param frame_size Number of clock cycles per frame (default: @ref BTANA_FRAME_SIZE).
     */
    ParallelStreamingFramer(std::vector<std::string> filenames, uint16_t frame_size = BTANA_FRAME_SIZE)
        : ParallelStreamingFramer(filenames, "", "", frame_size) {}

    /**
     * @brief Construct a framer with trigger configuration.
     * @param filenames           Paths to the raw ALCOR data files to stream.
     * @param trigger_config_file Path to the trigger configuration file.
     * @param frame_size          Number of clock cycles per frame (default: @ref BTANA_FRAME_SIZE).
     */
    ParallelStreamingFramer(std::vector<std::string> filenames, std::string trigger_config_file, uint16_t frame_size = BTANA_FRAME_SIZE)
        : ParallelStreamingFramer(filenames, trigger_config_file, "", frame_size) {}

    /**
     * @brief Construct a framer with trigger and readout configuration.
     * @param filenames            Paths to the raw ALCOR data files to stream.
     * @param trigger_config_file  Path to the trigger configuration file.
     * @param readout_config_file  Path to the readout configuration file.
     * @param frame_size           Number of clock cycles per frame (default: @ref BTANA_FRAME_SIZE).
     */
    ParallelStreamingFramer(std::vector<std::string> filenames, std::string trigger_config_file, std::string readout_config_file, uint16_t frame_size = BTANA_FRAME_SIZE);

    /**
     * @brief Construct a framer with full framer configuration.
     *
     * Preferred over the @c frame_size overload — all four timing constants
     * (@c frame_size, @c first_frames_trigger, @c afterpulse_deadtime,
     * @c trigger_secondary_window) are taken from the struct rather than the
     * compile-time macro defaults.
     *
     * @param filenames            Paths to the raw ALCOR data files to stream.
     * @param trigger_config_file  Path to the trigger configuration file.
     * @param readout_config_file  Path to the readout configuration file.
     * @param framer_cfg           Framer configuration (see @ref FramerConfigStruct).
     */
    ParallelStreamingFramer(std::vector<std::string> filenames, std::string trigger_config_file, std::string readout_config_file, FramerConfigStruct framer_cfg);

    // -------------------------------------------------------------------------
    // Getters
    // -------------------------------------------------------------------------

    /**
     * @brief Returns a reference to the current spill data.
     *
     * The previous by-value `get_spilldata()` overload was removed when
     * @ref AlcorSpilldata became non-copyable  All
     * callers should hold a reference / `unique_ptr` to the owning wrapper.
     *
     * @return Reference to the internal @ref AlcorSpilldata object.
     */
    AlcorSpilldata &get_spilldata_link() { return spilldata; }

    /**
     * @brief Returns the number of triggers registered from the configuration file.
     *        Counts both device-mode and channel-mode entries.
     * @return Total registered triggers.
     */
    int get_registered_triggers() { return static_cast<int>(trigger_config.size()); }

    /// @return Const reference to the parsed trigger configuration (both maps + ordered list).
    const TriggerConfigSet &get_trigger_config() const { return trigger_config; }

    /**
     * @brief Returns the fine tune distribution per tdc index.
     * @return fine tune distribution per tdc index.
     */
    TH2F *get_fine_tune_distribution() { return h2_fine_tune_distribution.get(); }

    /**
     * @brief Returns the same-channel Δt distribution used to validate the
     *        afterpulse QA windows.
     *
     * One entry per Hit (after the channel's first Hit), Δt = current
     * global cc − previous-same-channel global cc.  Range 0..32768 cc
     * (one full rollover period), 1024 bins → 32 cc/bin — covers both
     * the near afterpulse window (default 1–64 cc) and the far
     * sideband (default 256–319 cc) plus the entire DCR plateau out
     * to one rollover.
     */
    TH1F *get_afterpulse_dt_distribution() { return h_afterpulse_dt.get(); }

    /**
     * @brief Returns the per-stream, per-spill rollover correction table.
     * @return Const reference to the correction table, indexed as @c [stream][spill].
     *         Entries are clock-cycle offsets; empty/zero entries indicate no correction.
     */
    const std::vector<std::vector<uint64_t>> &get_rollover_correction_table() const { return rollover_correction_per_stream_and_spill; }

    /**
     * @brief Read-only access to the loaded readout configuration.
     *
     * Use to query which device/chip pairs are assigned to each sub-detector
     * role (timing / tracking / cherenkov) without re-parsing the TOML.
     * Added for (timing chip IDs are no longer hardcoded
     * in @c lightdata_writer.cxx).
     */
    const ReadoutConfigList &get_readout_config() const { return readout_config; }

    /**
     * @brief Returns the filenames of the data streams in index order.
     * @return Filename list parallel to the internal stream vector.
     */
    std::vector<std::string> get_stream_filenames() const;

    // -------------------------------------------------------------------------
    // Setters
    // -------------------------------------------------------------------------

    // set_spilldata*() removed: AlcorSpilldata is non-copyable and non-movable;
    // replace any future "replace the spill" use case with
    // `framer.get_spilldata_link().clear()` + repopulate-in-place.

    /**
     * @brief Sets the number of parallel processing threads.
     * @param v Requested number of threads.
     */
    void set_parallel_cores(uint16_t v) { n_threads_requested = v; }

    /**
     * @brief Provide a QA configuration so the framer can tag
     *        @c HitmaskAfterpulseNear / @c HitmaskAfterpulseFar per Hit.
     *
     * The framer computes Δt to the previous same-channel Hit during
     * @ref process and consults this struct to decide whether the Hit
     * lands in the QA "near" or "far" same-channel window.  Default-
     * constructed @c QaConfigStruct gives the legacy behaviour
     * (windows that match the historic hard-coded values).
     *
     * Must be set before the first @ref next_spill call to take effect.
     */
    void set_qa_config(QaConfigStruct v) { _qa_cfg = v; }

    /** @brief Returns the currently active QA configuration. */
    const QaConfigStruct &get_qa_config() const { return _qa_cfg; }

    // -------------------------------------------------------------------------
    // Progress bar assignment
    // -------------------------------------------------------------------------

    /**
     * @brief Assign a standalone progress bar for reporting streaming progress.
     *
     * The framer holds a non-owning pointer — @p bar must remain alive for
     * the entire duration of processing. Replaces any previously assigned bar.
     *
     * @param bar  A `ProgressBar` instance owned by the caller.
     */
    void assign_bar(mist::logger::ProgressBar &bar) { assigned_bar_ = &bar; }

    /**
     * @brief Assign a subtask bar from a `MultiProgressBar` group.
     *
     * Use this when the framer is one of several concurrent tasks displayed
     * together. The parent `MultiProgressBar` (and therefore this subtask
     * handle) must outlive the framer. Replaces any previously assigned bar.
     *
     * @param bar  A `SubtaskProgressBar` handle returned by
     *             `MultiProgressBar::add_subtask()`.
     */
    void assign_bar(mist::logger::SubtaskProgressBar &bar) { assigned_bar_ = &bar; }

    /**
     * @brief Detach the currently assigned bar without finishing it.
     *
     * After this call the framer reports no progress. The bar itself is
     * unaffected — the caller retains full ownership.
     */
    void clear_bar() { assigned_bar_ = std::monostate{}; }

    // -------------------------------------------------------------------------
    // Rollover-offset resolution
    // -------------------------------------------------------------------------

    /**
     * @brief Build the per-stream, per-spill rollover-offset correction table.
     *
     * For each spill index shared across streams, the reference rollover count
     * is taken as the maximum of @c gRollover values across all valid streams.
     * Streams lagging the reference missed one or more rollover ticks; the
     * correction applied during framing adds back the missing ticks, expressed
     * in clock cycles.
     *
     * Must be called after construction and before the first @ref next_spill().
     * Safe to call when no correction is needed — the resulting table is all
     * zeros and framing behaves as if the method had never been called.
     *
     * @note Correction is always non-negative under the max-reference convention:
     *       streams ahead of the reference are treated as the reference itself.
     */
    void resolve_rollover_offsets();

    // -------------------------------------------------------------------------
    // I/O Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Advances all data streams to the next spill.
     * @return @c true if a next spill is available, @c false if all streams are exhausted.
     */
    bool next_spill();

    /**
     * @brief Per-worker-thread scratch space (frame map + QA histograms).
     *
     * Each `std::async` worker in @ref next_spill owns one of these.  Two
     * dominant per-Hit contention points are addressed by isolating them per
     * worker:
     *
     *  1. **`frame_map`** — a private `std::map<uint32_t, AlcorLightdataStruct>`
     *     that the worker writes to without acquiring @c frame_mutexes_access.
     *     The shared `frame_mutexes_access` lock used to serialise every Hit's
     *     `frame_and_lightdata[]` insertion across all 16 worker threads; that
     *     was the dominant bottleneck even after the QA-fill move.  At spill
     *     end @ref next_spill merges every worker's `frame_map` into the master
     *     `spilldata.frame_and_lightdata` via @ref merge_lightdata.
     *  2. **`h2_fine_tune` / `h_afterpulse`** — clones of the master QA
     *     histograms, also filled lock-free by the worker and merged into the
     *     master via `TH1::Add` at spill end (then freed).
     *
     * `spilldata_masks_mutex` is still used for `is_start_spill()` paths
     * (rare; once per stream per spill).
     */
    struct WorkerQA
    {
        TH2F *h2_fine_tune = nullptr;                       ///< Per-thread clone of @ref h2_fine_tune_distribution.
        TH1F *h_afterpulse = nullptr;                       ///< Per-thread clone of @ref h_afterpulse_dt.
        std::map<uint32_t, AlcorLightdataStruct> frame_map; ///< Per-worker frame buffer; merged into master at spill end.
    };

    /**
     * @brief Processes a single data stream for the current spill.
     *
     * Applies the per-stream, per-spill rollover correction (if any) before
     * computing frame indices, so both ALCOR hits and trigger hits land in
     * the correct frame regardless of missed rollover ticks.
     *
     * @param stream_index Index into @ref data_streams of the stream to process.
     * @param qa           Per-worker scratch space (frame map + QA histograms).
     *                     **Required** for parallel callers — workers write
     *                     their frames into @c qa->frame_map without holding
     *                     @c frame_mutexes_access.  May be @c nullptr only for
     *                     a hypothetical single-threaded test driver; in that
     *                     case the function falls back to direct writes into
     *                     @c spilldata.frame_and_lightdata under the lock.
     *
     * Frame size is read directly from the @c _frame_size member; the
     * previous `int _frame_size` parameter was an unused override (it
     * just shadowed the member) — dropped.
     */
    void process(size_t stream_index, WorkerQA *qa = nullptr);

private:
    // -------------------------------------------------------------------------
    // Internal progress helpers
    // -------------------------------------------------------------------------

    /**
     * @brief Update whichever bar is currently assigned, if any.
     *
     * Uses `std::visit` over the variant — the monostate arm is a no-op,
     * so callers never need to check whether a bar is assigned before calling this.
     *
     * Thread-safe: both `ProgressBar` and `SubtaskProgressBar` from MIST's
     * `<mist/logger>` are internally mutex-guarded (verified against MIST
     * `dev`: each bar owns a `std::mutex` taken on every state-mutating
     * method).  Concurrent calls from worker threads are safe.
     *
     */
    void _update_bar(int64_t current, int64_t total);

    /// @name Threading
    /// @{

    /** @brief Number of parallel threads requested by the user. */
    uint16_t n_threads_requested;

    /**
     * @name Mutex lock-ordering contract
     *
     * When more than one of these mutexes must be held simultaneously, always
     * acquire them in the following canonical order to prevent deadlock:
     *
     *   1. @c frame_mutexes_access
     *   2. @c triggers_map_mutex
     *   3. @c spilldata_masks_mutex
     *
     * In practice the three mutexes protect disjoint data structures, so it is
     * rare to hold more than one at a time.  The ordering is documented here as
     * a safety invariant for future contributors.
     * @{
     */

    /** @brief Protects creation of new per-frame mutex entries in
     *         @c frame_mutexes.  Acquired before the frame mutex itself. */
    std::mutex frame_mutexes_access;

    /** @brief Protects writes to @ref unknown_trigger_devices_seen. */
    std::mutex triggers_map_mutex;

    /** @brief Protects creation of new spill participants and dead-channel masks. */
    std::mutex spilldata_masks_mutex;

    /// @}

    /// @}

    /// @name Streaming & Spill Data
    /// @{

    /** @brief Index of the spill currently being processed. */
    int32_t _current_spill;

    /**
     * @brief Per-stream spill advancement counters.
     * Maps stream identifier to the index of the next spill to be read.
     */
    std::map<std::string, uint32_t> _next_spill;

    /**
     * @brief Non-owning handle to an optional progress bar.
     *
     * Holds one of:
     *  - `std::monostate`                       — no bar assigned (default)
     *  - `mist::logger::ProgressBar*`          — standalone bar
     *  - `mist::logger::SubtaskProgressBar*`  — subtask handle
     *
     * The pointed-to object is owned by the caller and must outlive the framer.
     */
    std::variant<std::monostate,
                 mist::logger::ProgressBar *,
                 mist::logger::SubtaskProgressBar *>
        assigned_bar_;

    /** @brief Accumulated framed data for the current spill. */
    AlcorSpilldata spilldata;

    /** @brief One data streamer per input file. */
    std::vector<AlcorDataStreamer> data_streams;

    /// @}

    /// @name Rollover-offset correction
    /// @{

    /**
     * @brief Per-stream, per-spill rollover correction in clock cycles.
     *
     * Indexed as @c [stream_index][spill_index]; the stored value is the number
     * of clock cycles to add to the coarse global time of every Hit or trigger
     * from that stream during that spill. Populated by @ref resolve_rollover_offsets().
     * Empty or zero entries disable correction (the common case).
     */
    std::vector<std::vector<uint64_t>> rollover_correction_per_stream_and_spill;

    /// @}

    /// @name Configuration
    /// @{

    /** @brief Parsed trigger configuration — device-mode map, channel-mode map, ordered list. */
    TriggerConfigSet trigger_config;

    /** @brief Tracks device IDs already logged as "tagged trigger from unknown device" (deduplication). */
    std::unordered_set<uint16_t> unknown_trigger_devices_seen;

    /** @brief Readout configuration (channel masking, thresholds, etc.). */
    ReadoutConfigList readout_config;

    /** @brief Frame size in clock cycles used during processing. */
    uint16_t _frame_size;

    /** @brief QA windows used to tag afterpulse near/far bits during @ref process. */
    QaConfigStruct _qa_cfg;

    /** @brief Number of start-of-spill frames pre-filled as noise-measurement frames. */
    int _first_frames_trigger;

    /** @brief Afterpulse suppression deadtime (clock cycles). */
    uint16_t _afterpulse_deadtime;

    /** @brief Secondary-trigger detection window (clock cycles). */
    uint16_t _trigger_secondary_window;

    /// @}

    /// @name Histograms
    /// @{

    /** @brief Fine tune distribution per tdc index — RAII-owned, no manual delete. */
    RootHist<TH2F> h2_fine_tune_distribution;

    /** @brief Same-channel Δt distribution (cc) — diagnostic for the afterpulse QA windows.  RAII-owned. */
    RootHist<TH1F> h_afterpulse_dt;

    /// @}
};