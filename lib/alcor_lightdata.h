#pragma once

#include <vector>
#include <cstdint>

#include "alcor_finedata.h"
#include "triggers.h"
#include "config_reader.h"

struct alcor_lightdata_struct
{
    std::vector<trigger_struct> trigger_hits;
    std::vector<alcor_finedata_struct> timing_hits;
    std::vector<alcor_finedata_struct> tracking_hits;
    std::vector<alcor_finedata_struct> cherenkov_hits;

    void clear()
    {
        trigger_hits.clear();
        trigger_hits.shrink_to_fit();
        timing_hits.clear();
        timing_hits.shrink_to_fit();
        tracking_hits.clear();
        tracking_hits.shrink_to_fit();
        cherenkov_hits.clear();
        cherenkov_hits.shrink_to_fit();
    }
};

class alcor_lightdata : public alcor_finedata
{
private:
    // Data structure
    alcor_lightdata_struct lightdata;

public:
    //  Constructors
    alcor_lightdata() = default;
    alcor_lightdata(const alcor_lightdata_struct &data_struct)
        : lightdata(data_struct) {};

    //  Getters
    //  --- Copied value
    alcor_lightdata_struct get_lightdata() const { return lightdata; }
    std::vector<alcor_finedata_struct> get_timing_hits() const { return lightdata.timing_hits; }
    std::vector<alcor_finedata_struct> get_tracking_hits() const { return lightdata.tracking_hits; }
    std::vector<alcor_finedata_struct> get_cherenkov_hits() const { return lightdata.cherenkov_hits; }
    std::vector<trigger_struct> get_triggers() const { return lightdata.trigger_hits; }
    //  --- Linked reference
    alcor_lightdata_struct &get_lightdata_link() { return lightdata; }
    std::vector<alcor_finedata_struct> &get_timing_hits_link() { return lightdata.timing_hits; }
    std::vector<alcor_finedata_struct> &get_tracking_hits_link() { return lightdata.tracking_hits; }
    std::vector<alcor_finedata_struct> &get_cherenkov_hits_link() { return lightdata.cherenkov_hits; }
    std::vector<trigger_struct> &get_triggers_link() { return lightdata.trigger_hits; }

    //   Setters
    //  --- Copied value
    void set_lightdata(alcor_lightdata_struct v) { lightdata = v; }
    void set_timing_hits(std::vector<alcor_finedata_struct> v) { lightdata.timing_hits = v; }
    void set_tracking_hits(std::vector<alcor_finedata_struct> v) { lightdata.tracking_hits = v; }
    void set_cherenkov_hits(std::vector<alcor_finedata_struct> v) { lightdata.cherenkov_hits = v; }
    void set_trigger(std::vector<trigger_struct> v) { lightdata.trigger_hits = v; }
    //  --- Linked reference
    void set_lightdata_link(alcor_lightdata_struct &v) { lightdata = v; }
    void set_timing_hits_link(std::vector<alcor_finedata_struct> &v) { lightdata.timing_hits = v; }
    void set_tracking_hits_link(std::vector<alcor_finedata_struct> &v) { lightdata.tracking_hits = v; }
    void set_cherenkov_hits_link(std::vector<alcor_finedata_struct> &v) { lightdata.cherenkov_hits = v; }
    void set_trigger_link(std::vector<trigger_struct> &v) { lightdata.trigger_hits = v; }

    //  Utilities
    //  --- Triggers search
    uint16_t get_trigger_time(uint8_t trigger_index);
};

uint16_t alcor_lightdata::get_trigger_time(uint8_t trigger_index)
{
    //  Check the trigger fired
    auto it = std::find_if(lightdata.trigger_hits.begin(), lightdata.trigger_hits.end(),
                           [trigger_index](const trigger_struct &t)
                           { return t.index == trigger_index; });

    //   TODO: understand and fix
    if (it != lightdata.trigger_hits.end())
        return it->coarse;
    else
        return 65535;
}

#ifdef __ROOTCLING__
#pragma link C++ struct trigger_struct + ;
#pragma link C++ struct alcor_lightdata_struct + ;
#pragma link C++ class std::vector < trigger_struct> + ;
#pragma link C++ class std::vector < alcor_finedata_struct> + ;
#endif