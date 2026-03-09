#pragma once
#include <string>

#define _CROSS_TALK_DEADTIME_      124  ///< Cross-talk veto window (clock cycles, ~200 ns)
#define _TRIGGER_MIN_SEPARATION_    16  ///< Minimum separation between two valid same-type triggers (cc, ~25 ns)
#define _EDGE_REJECTION_NS_         25  ///< Edge rejection guard window (ns)

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int         max_spill              = 1000,
    bool        force_recodata_rebuild = false,
    bool        force_lightdata_rebuild= false,
    std::string mapping_conf           = "conf/mapping_conf.2025.toml",
    std::string trigger_conf           = "conf/trigger_conf.toml");