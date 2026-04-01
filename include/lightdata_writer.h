#pragma once
#include <string>
#include <vector>
#include "alcor_spilldata.h"

/**
 * @todo Make a single file with (class?) writers/readers
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
    std::string fine_calibration_config_file = "");

/**
 * 
 */
bool run_streaming_trigger(alcor_spilldata &current_spill,
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
                           TH1F *h_tdc_zero_cluster_size);