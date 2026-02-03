#pragma once

#include <vector>
#include <cstdint>
#include <algorithm> // for std::find_if

#include "alcor_finedata.h"
#include "triggers.h"
#include "config_reader.h"

// Data structure
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

// Main class
class alcor_lightdata : public alcor_finedata
{
private:
    alcor_lightdata_struct lightdata;

public:
    // Constructors
    alcor_lightdata() = default;
    alcor_lightdata(const alcor_lightdata_struct &data_struct);

    // Getters
    alcor_lightdata_struct get_lightdata() const;
    std::vector<alcor_finedata_struct> get_timing_hits() const;
    std::vector<alcor_finedata_struct> get_tracking_hits() const;
    std::vector<alcor_finedata_struct> get_cherenkov_hits() const;
    std::vector<trigger_struct> get_triggers() const;

    alcor_lightdata_struct &get_lightdata_link();
    std::vector<alcor_finedata_struct> &get_timing_hits_link();
    std::vector<alcor_finedata_struct> &get_tracking_hits_link();
    std::vector<alcor_finedata_struct> &get_cherenkov_hits_link();
    std::vector<trigger_struct> &get_triggers_link();

    // Setters
    void set_lightdata(alcor_lightdata_struct v);
    void set_timing_hits(std::vector<alcor_finedata_struct> v);
    void set_tracking_hits(std::vector<alcor_finedata_struct> v);
    void set_cherenkov_hits(std::vector<alcor_finedata_struct> v);
    void set_trigger(std::vector<trigger_struct> v);

    void set_lightdata_link(alcor_lightdata_struct &v);
    void set_timing_hits_link(std::vector<alcor_finedata_struct> &v);
    void set_tracking_hits_link(std::vector<alcor_finedata_struct> &v);
    void set_cherenkov_hits_link(std::vector<alcor_finedata_struct> &v);
    void set_trigger_link(std::vector<trigger_struct> &v);

    // Utilities
    uint16_t get_trigger_time(uint8_t trigger_index);
};


