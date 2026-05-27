#pragma once

/**
 * @file lightdata_writer.h
 * @brief Top-level entry points for building and writing lightdata ROOT files.
 *
 * Provides:
 * - @ref lightdata_writer — full pipeline from raw ALCOR data to a lightdata TTree.
 *
 * @todo Consolidate writer/reader entry points into a single class hierarchy.
 */

#include <string>

/**
 * @brief Build a lightdata ROOT file from raw ALCOR data for a given run.
 *
 * Opens all per-FIFO ROOT files under @p data_repository/@p run_name, frames
 * them spill-by-spill using @ref ParallelStreamingFramer, applies the
 * trigger and readout configurations, and writes one lightdata TTree entry per
 * spill.  If an up-to-date lightdata file already exists and
 * @p force_rebuild is @c false the function returns immediately.
 *
 * Lightdata has no writer upstream of it (its dependency is the raw
 * ALCOR data on disk, which is never regenerated), so there is no
 * `force_upstream` parameter — the cascade rule documented on the
 * other writers terminates here.
 *
 * @param data_repository          Root directory that contains the run folder.
 * @param run_name                 Run identifier (sub-directory name).
 * @param max_spill                Maximum number of spills to process (default 1000).
 * @param force_rebuild            If @c true, overwrite any existing lightdata file.
 * @param requested_n_threads      Number of parallel threads; -1 = auto-detect.
 * @param trigger_setup_file       Path to the trigger TOML configuration.
 * @param readout_config_file      Path to the readout TOML configuration.
 * @param mapping_config_file      Path to the Mapping TOML calibration file.
 * @param fine_calibration_config_file
 *                                 Path to the fine-time calibration file; empty
 *                                 string disables fine-time correction.
 * @param framer_conf_file         Path to the framer TOML configuration; missing
 *                                 keys fall back to compile-time macro defaults.
 * @param streaming_conf_file      Path to the software-trigger-pipeline TOML
 *                                 configuration (sections `[streaming_trigger]`
 *                                 and `[streaming_hough]`).  See
 *                                 `include/triggers/streaming/DISCUSSION.md`
 *                                 and `conf/streaming.toml`.
 */
void lightdata_writer(
    const std::string &data_repository,
    const std::string &run_name,
    int max_spill = 1000,
    bool force_rebuild = false,
    int requested_n_threads = -1,
    std::string trigger_setup_file = "conf/trigger_conf.toml",
    std::string readout_config_file = "conf/readout_config.toml",
    std::string mapping_config_file = "conf/mapping_conf.toml",
    std::string fine_calibration_config_file = "",
    std::string framer_conf_file = "conf/framer_conf.toml",
    std::string streaming_conf_file = "conf/streaming.toml");
