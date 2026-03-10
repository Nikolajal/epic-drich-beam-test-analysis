#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <variant>
#include "TFile.h"
#include "TTree.h"
#include "TH1.h"
#include "TH2.h"
#include "TSystem.h"
#include <filesystem>
#include "alcor_spilldata.h"
#include "alcor_data_streamer.h"

/// @defgroup FramerConfig Framer Configuration Constants
/// @{

/** @brief Number of clock cycles per frame */
#define _FRAME_SIZE_ 1024

/** @brief Frame duration in nanoseconds (3.125ns per clock cycle at 320MHz) */
#define _FRAME_LENGTH_NS_ (_FRAME_SIZE_ * 3.125)

/** @brief Number of initial frames reserved for trigger synchronization */
#define _FIRST_FRAMES_TRIGGER_ 2500

/** @brief Afterpulse suppression deadtime in clock cycles (~200ns) */
#define _AFTERPULSE_DEADTIME_ 64

/// @}

/**
 * @class parallel_streaming_framer
 * @brief Frames raw ALCOR data streams from multiple input files in parallel.
 *
 * This class manages multiple @ref alcor_data_streamer instances, one per input file,
 * and processes them concurrently to produce framed spill data. It supports optional
 * trigger and readout configuration and afterpulse suppression.
 *
 * ### Progress reporting
 * The framer can optionally report its progress through a `mist::logger` bar.
 * Either a standalone `progress_bar` or a `subtask_progress_bar` from a
 * `multi_progress_bar` group can be assigned via `assign_bar()`. The framer
 * holds a non-owning pointer — the caller is responsible for keeping the bar
 * alive for the duration of processing.
 *
 * @code{.cpp}
 * // Option A — standalone bar
 * mist::logger::progress_bar bar;
 * framer.assign_bar(bar);
 *
 * // Option B — subtask inside a multi_progress_bar group
 * mist::logger::multi_progress_bar mb;
 * auto& s = mb.add_subtask("Streaming");
 * framer.assign_bar(s);   // mb (and s) must outlive framer
 *
 * framer.next_spill();    // bar updates automatically during processing
 * framer.clear_bar();     // detach when done (optional)
 * @endcode
 *
 * @note Frame size and timing constants are defined via macros and should eventually
 *       be moved to a config file with a proper pass-down mechanism to @ref alcor_spilldata.
 */
class parallel_streaming_framer
{
public:
    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------

    /** @brief Default constructor. */
    parallel_streaming_framer() = default;

    /**
     * @brief Construct a framer from a list of input files.
     * @param filenames  Paths to the raw ALCOR data files to stream.
     * @param frame_size Number of clock cycles per frame (default: @ref _FRAME_SIZE_).
     */
    parallel_streaming_framer(std::vector<std::string> filenames, uint16_t frame_size = _FRAME_SIZE_)
        : parallel_streaming_framer(filenames, "", "", frame_size) {}

    /**
     * @brief Construct a framer with trigger configuration.
     * @param filenames           Paths to the raw ALCOR data files to stream.
     * @param trigger_config_file Path to the trigger configuration file.
     * @param frame_size          Number of clock cycles per frame (default: @ref _FRAME_SIZE_).
     */
    parallel_streaming_framer(std::vector<std::string> filenames, std::string trigger_config_file, uint16_t frame_size = _FRAME_SIZE_)
        : parallel_streaming_framer(filenames, trigger_config_file, "", frame_size) {}

    /**
     * @brief Construct a framer with trigger and readout configuration.
     * @param filenames            Paths to the raw ALCOR data files to stream.
     * @param trigger_config_file  Path to the trigger configuration file.
     * @param readout_config_file  Path to the readout configuration file.
     * @param frame_size           Number of clock cycles per frame (default: @ref _FRAME_SIZE_).
     */
    parallel_streaming_framer(std::vector<std::string> filenames, std::string trigger_config_file, std::string readout_config_file, uint16_t frame_size = _FRAME_SIZE_);

    // -------------------------------------------------------------------------
    // Getters
    // -------------------------------------------------------------------------

    /**
     * @brief Returns a copy of the current spill data.
     * @return The accumulated @ref alcor_spilldata for the current spill.
     */
    alcor_spilldata get_spilldata() const;

    /**
     * @brief Returns a reference to the current spill data.
     * @return Reference to the internal @ref alcor_spilldata object.
     */
    alcor_spilldata &get_spilldata_link();

    /**
     * @brief Returns the number of triggers registered from the configuration file.
     * @return Number of triggers registered from the configuration file.
     * @todo Trigger number should be assigned by program, and checked to avoid using reserved numbers > should be [0-100[
     */
    int get_registered_triggers();

    /**
     * @brief Returns the fine tune distribution per tdc index.
     * @return fine tune distribution per tdc index.
     */
    TH2F *get_fine_tune_distribution();

    // -------------------------------------------------------------------------
    // Setters
    // -------------------------------------------------------------------------

    /**
     * @brief Sets the spill data by value.
     * @param v The @ref alcor_spilldata object to assign.
     */
    void set_spilldata(alcor_spilldata v);

    /**
     * @brief Sets the spill data by reference.
     * @param v Reference to the @ref alcor_spilldata object to assign.
     */
    void set_spilldata_link(alcor_spilldata &v);

    /**
     * @brief Sets the number of parallel processing threads.
     * @param v Requested number of threads.
     */
    void set_parallel_cores(uint16_t v);

    // -------------------------------------------------------------------------
    // Progress bar assignment
    // -------------------------------------------------------------------------

    /**
     * @brief Assign a standalone progress bar for reporting streaming progress.
     *
     * The framer holds a non-owning pointer — @p bar must remain alive for
     * the entire duration of processing. Replaces any previously assigned bar.
     *
     * @param bar  A `progress_bar` instance owned by the caller.
     */
    void assign_bar(mist::logger::progress_bar& bar);

    /**
     * @brief Assign a subtask bar from a `multi_progress_bar` group.
     *
     * Use this when the framer is one of several concurrent tasks displayed
     * together. The parent `multi_progress_bar` (and therefore this subtask
     * handle) must outlive the framer. Replaces any previously assigned bar.
     *
     * @param bar  A `subtask_progress_bar` handle returned by
     *             `multi_progress_bar::add_subtask()`.
     */
    void assign_bar(mist::logger::subtask_progress_bar& bar);

    /**
     * @brief Detach the currently assigned bar without finishing it.
     *
     * After this call the framer reports no progress. The bar itself is
     * unaffected — the caller retains full ownership.
     */
    void clear_bar();

    // -------------------------------------------------------------------------
    // I/O Operations
    // -------------------------------------------------------------------------

    /**
     * @brief Advances all data streams to the next spill.
     * @return @c true if a next spill is available, @c false if all streams are exhausted.
     */
    bool next_spill();

    /**
     * @brief Processes a single data stream and returns the framed spill data.
     * @param current_stream Reference to the @ref alcor_data_streamer to process.
     * @param _frame_size    Number of clock cycles per frame.
     */
    void process(alcor_data_streamer &current_stream, int _frame_size);

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
     * Thread-safe: both `progress_bar` and `subtask_progress_bar` are internally
     * mutex-guarded, so concurrent calls from worker threads are safe.
     */
    void _update_bar(int64_t current, int64_t total);

    /// @name Threading
    /// @{

    /** @brief Number of parallel threads requested by the user. */
    uint16_t n_threads_requested;

    /** @brief Protects creation of new mutex entries. */
    std::mutex frame_mutexes_access;

    /** @brief Protects creation of new entries for unknown triggers. */
    std::mutex triggers_map_mutex;

    /** @brief Protects creation of new participants and dead masks. */
    std::mutex spilldata_masks_mutex;

    /// @}

    /// @name Streaming & Spill Data
    /// @{

    /** @brief Index of the spill currently being processed. */
    uint8_t _current_spill;

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
     *  - `mist::logger::progress_bar*`          — standalone bar
     *  - `mist::logger::subtask_progress_bar*`  — subtask handle
     *
     * The pointed-to object is owned by the caller and must outlive the framer.
     */
    std::variant<std::monostate,
                 mist::logger::progress_bar*,
                 mist::logger::subtask_progress_bar*> assigned_bar_;

    /** @brief Accumulated framed data for the current spill. */
    alcor_spilldata spilldata;

    /** @brief One data streamer per input file. */
    std::vector<alcor_data_streamer> data_streams;

    /// @}

    /// @name Configuration
    /// @{

    /** @brief Ordered list of trigger configurations loaded from file. */
    std::vector<trigger_config> triggers;

    /** @brief Fast lookup map from channel ID to trigger configuration. */
    std::unordered_map<uint8_t, trigger_config> triggers_map;

    /** @brief Readout configuration (channel masking, thresholds, etc.). */
    readout_config_list readout_config;

    /** @brief Frame size in clock cycles used during processing. */
    uint16_t _frame_size;

    /// @}

    /// @name Histograms
    /// @{

    /** @brief Fine tune distribution per tdc index. */
    TH2F *h2_fine_tune_distribution;

    /// @}
};