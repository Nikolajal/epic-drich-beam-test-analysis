#pragma once

/**
 * @file lightdata_writer.h
 * @brief Top-level entry points for building and writing lightdata ROOT files.
 *
 * Provides:
 * - @ref lightdata_writer — full pipeline from raw ALCOR data to a lightdata TTree.
 * - @ref run_streaming_trigger — per-frame online streaming-trigger evaluation.
 *
 * @todo Consolidate writer/reader entry points into a single class hierarchy.
 */

#include <string>
#include <vector>
#include "alcor_spilldata.h"

/**
 * @brief Build a lightdata ROOT file from raw ALCOR data for a given run.
 *
 * Opens all per-FIFO ROOT files under @p data_repository/@p run_name, frames
 * them spill-by-spill using @ref ParallelStreamingFramer, applies the
 * trigger and readout configurations, and writes one lightdata TTree entry per
 * spill.  If an up-to-date lightdata file already exists and
 * @p force_lightdata_rebuild is @c false the function returns immediately.
 *
 * @param data_repository          Root directory that contains the run folder.
 * @param run_name                 Run identifier (sub-directory name).
 * @param max_spill                Maximum number of spills to process (default 1000).
 * @param force_lightdata_rebuild  If @c true, overwrite any existing lightdata file.
 * @param requested_n_threads      Number of parallel threads; -1 = auto-detect.
 * @param trigger_setup_file       Path to the trigger TOML configuration.
 * @param readout_config_file      Path to the readout TOML configuration.
 * @param mapping_config_file      Path to the Mapping TOML calibration file.
 * @param fine_calibration_config_file
 *                                 Path to the fine-time calibration file; empty
 *                                 string disables fine-time correction.
 * @param framer_conf_file         Path to the framer TOML configuration; missing
 *                                 keys fall back to compile-time macro defaults.
 */
void lightdata_writer(
    const std::string &data_repository,
    const std::string &run_name,
    int max_spill = 1000,
    bool force_lightdata_rebuild = false,
    int requested_n_threads = -1,
    std::string trigger_setup_file = "conf/trigger_conf.toml",
    std::string readout_config_file = "conf/readout_config.toml",
    std::string mapping_config_file = "conf/mapping_conf.toml",
    std::string fine_calibration_config_file = "",
    std::string framer_conf_file = "conf/framer_conf.toml");

/**
 * @brief Evaluate the streaming trigger for a single frame and accumulate diagnostics.
 *
 * Scans the Cherenkov hits in @p current_spill for the given @p frame_id
 * using a sliding time window of width @p time_window_ns and a Hit-count
 * @p threshold.  Carry-over hits from the previous frame are provided via
 * @p carry_over_hits and updated in-place for the next call.
 *
 * @param current_spill              Spill data for the current frame.
 * @param frame_id                   Index of the frame to evaluate.
 * @param time_window_ns             Width of the sliding trigger window [ns].
 * @param threshold                  Minimum Hit count to fire the trigger.
 * @param carry_over_hits            Hits that spill into the next frame (in/out).
 * @param h_delta_t_leading_edge     Diagnostic histogram: Δt to leading edge.
 * @param h_delta_t_half_centroid    Diagnostic histogram: Δt to half-centroid.
 * @param h_delta_t_half_center_left  Diagnostic histogram: Δt left half-centre.
 * @param h_delta_t_half_center_right Diagnostic histogram: Δt right half-centre.
 * @param h_sigma_vs_nhits           2D diagnostic: σ vs. Hit count.
 * @param h_median_vs_window         2D diagnostic: median vs. window size.
 * @param h_tdc_step_sizes           Diagnostic histogram: TDC step sizes.
 * @param h_tdc_zero_times           Diagnostic histogram: TDC zero times.
 * @param h_tdc_zero_cluster_size    Diagnostic histogram: TDC zero cluster size.
 * @param frame_length_ns            Frame duration in nanoseconds; used to shift
 *                                   carry-over Hit times at frame boundaries.
 *                                   Required (no default) — must be derived from
 *                                   the active @ref FramerConfigStruct so that
 *                                   changes in the TOML config propagate here.
 * @return @c true if the trigger fired for this frame.
 */
bool run_streaming_trigger(AlcorSpilldata &current_spill,
                           int frame_id,
                           const float time_window_ns,
                           const int threshold,
                           std::vector<std::pair<int, float>> &carry_over_hits,
                           TH1F *h_delta_t_leading_edge,
                           TH1F *h_delta_t_half_centroid,
                           TH1F *h_delta_t_half_center_left,
                           TH1F *h_delta_t_half_center_right,
                           TH2F *h_sigma_vs_nhits,
                           TH2F *h_median_vs_window,
                           TH1F *h_tdc_step_sizes,
                           TH1F *h_tdc_zero_times,
                           TH1F *h_tdc_zero_cluster_size,
                           float frame_length_ns);
