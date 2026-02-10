#pragma once

#include <string>
#include "tracking_altai.h"

void recotrackdata_writer(
    std::string data_repository,
    std::string run_name,
    std::string track_data_repository,
    std::string track_run_name,
    int max_frames = 10000000,
    bool force_recodata_rebuild = false,
    bool force_lightdata_rebuild = false);
