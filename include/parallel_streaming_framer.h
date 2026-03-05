#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
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
    parallel_streaming_framer(std::vector<std::string> filenames, uint16_t frame_size = _FRAME_SIZE_);

    /**
     * @brief Construct a framer with trigger configuration.
     * @param filenames           Paths to the raw ALCOR data files to stream.
     * @param trigger_config_file Path to the trigger configuration file.
     * @param frame_size          Number of clock cycles per frame (default: @ref _FRAME_SIZE_).
     */
    parallel_streaming_framer(std::vector<std::string> filenames, std::string trigger_config_file, uint16_t frame_size = _FRAME_SIZE_);

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
     * @return The framed @ref alcor_spilldata produced from the stream.
     */
    void process(alcor_data_streamer &current_stream, int _frame_size);

private:
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

    /** @brief Accumulated framed data for the current spill. */
    alcor_spilldata spilldata;

    /** @brief One data streamer per input file. */
    std::vector<alcor_data_streamer> data_streams;

    /// @}

    /// @name Configuration
    /// @{

    /** @brief Ordered list of trigger configurations loaded from file. */
    std::vector<trigger_config_struct> triggers;

    /** @brief Fast lookup map from channel ID to trigger configuration. */
    std::unordered_map<uint8_t, trigger_config_struct> triggers_map;

    /** @brief Readout configuration (channel masking, thresholds, etc.). */
    readout_config_list readout_config;

    /** @brief Frame size in clock cycles used during processing. */
    uint16_t _frame_size;

    /// @}
};