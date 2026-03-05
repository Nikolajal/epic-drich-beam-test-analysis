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
    int requested_n_threads = -1);
