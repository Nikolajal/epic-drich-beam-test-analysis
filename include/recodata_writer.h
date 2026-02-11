#pragma once
#include <string>

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill = 1000,
    bool force_recodata_rebuild = false,
    bool force_lightdata_rebuild = false,
    std::string mapping_conf = "conf/mapping_conf.2025.toml");
