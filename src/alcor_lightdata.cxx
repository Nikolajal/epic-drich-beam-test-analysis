#include "alcor_lightdata.h"

// Constructors
alcor_lightdata::alcor_lightdata(const alcor_lightdata_struct &data_struct)
    : lightdata(data_struct) {}

// Getters
alcor_lightdata_struct alcor_lightdata::get_lightdata() const { return lightdata; }
std::vector<alcor_finedata_struct> alcor_lightdata::get_timing_hits() const { return lightdata.timing_hits; }
std::vector<alcor_finedata_struct> alcor_lightdata::get_tracking_hits() const { return lightdata.tracking_hits; }
std::vector<alcor_finedata_struct> alcor_lightdata::get_cherenkov_hits() const { return lightdata.cherenkov_hits; }
std::vector<trigger_struct> alcor_lightdata::get_triggers() const { return lightdata.trigger_hits; }

alcor_lightdata_struct &alcor_lightdata::get_lightdata_link() { return lightdata; }
std::vector<alcor_finedata_struct> &alcor_lightdata::get_timing_hits_link() { return lightdata.timing_hits; }
std::vector<alcor_finedata_struct> &alcor_lightdata::get_tracking_hits_link() { return lightdata.tracking_hits; }
std::vector<alcor_finedata_struct> &alcor_lightdata::get_cherenkov_hits_link() { return lightdata.cherenkov_hits; }
std::vector<trigger_struct> &alcor_lightdata::get_triggers_link() { return lightdata.trigger_hits; }

// Setters
void alcor_lightdata::set_lightdata(alcor_lightdata_struct v) { lightdata = v; }
void alcor_lightdata::set_timing_hits(std::vector<alcor_finedata_struct> v) { lightdata.timing_hits = v; }
void alcor_lightdata::set_tracking_hits(std::vector<alcor_finedata_struct> v) { lightdata.tracking_hits = v; }
void alcor_lightdata::set_cherenkov_hits(std::vector<alcor_finedata_struct> v) { lightdata.cherenkov_hits = v; }
void alcor_lightdata::set_trigger(std::vector<trigger_struct> v) { lightdata.trigger_hits = v; }

void alcor_lightdata::set_lightdata_link(alcor_lightdata_struct &v) { lightdata = v; }
void alcor_lightdata::set_timing_hits_link(std::vector<alcor_finedata_struct> &v) { lightdata.timing_hits = v; }
void alcor_lightdata::set_tracking_hits_link(std::vector<alcor_finedata_struct> &v) { lightdata.tracking_hits = v; }
void alcor_lightdata::set_cherenkov_hits_link(std::vector<alcor_finedata_struct> &v) { lightdata.cherenkov_hits = v; }
void alcor_lightdata::set_trigger_link(std::vector<trigger_struct> &v) { lightdata.trigger_hits = v; }

// Utility
uint16_t alcor_lightdata::get_trigger_time(uint8_t trigger_index)
{
    auto it = std::find_if(lightdata.trigger_hits.begin(), lightdata.trigger_hits.end(),
                           [trigger_index](const trigger_struct &t)
                           { return t.index == trigger_index; });

    if (it != lightdata.trigger_hits.end())
        return it->coarse;
    else
        return 65535;
}
