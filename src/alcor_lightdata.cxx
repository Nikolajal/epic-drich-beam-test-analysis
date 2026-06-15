#include "alcor_lightdata.h"

void AlcorLightdataStruct::clear()
{
    trigger_hits.clear();
    trigger_hits.shrink_to_fit();
    timing_hits.clear();
    timing_hits.shrink_to_fit();
    tracking_hits.clear();
    tracking_hits.shrink_to_fit();
    cherenkov_hits.clear();
    cherenkov_hits.shrink_to_fit();
    ring1_cx = ring1_cy = ring1_radius = 0.f;
    ring2_cx = ring2_cy = ring2_radius = 0.f;
}

std::optional<float> AlcorLightdata::get_trigger_time(uint8_t trigger_index)
{
    auto it = std::find_if(
        lightdata.trigger_hits.begin(),
        lightdata.trigger_hits.end(),
        [trigger_index](const TriggerEvent &t)
        { return t.index == trigger_index; });

    if (it != lightdata.trigger_hits.end())
        return it->fine_time;
    return std::nullopt;
}
