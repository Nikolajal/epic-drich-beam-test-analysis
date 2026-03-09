#pragma once
#include <string>

/**
 * @todo Make a single file with (class?) writers/readers
 * */

void lightdata_writer(
    const std::string &data_repository,
    const std::string &run_name,
    int max_spill = 1000,
    bool force_lightdata_rebuild = false,
    int requested_n_threads = -1,
    std::string trigger_setup_file = "conf/trigger_conf.toml",
    std::string readout_config_file = "conf/readout_config.txt",
    std::string mapping_config_file = "conf/mapping_conf.toml");
