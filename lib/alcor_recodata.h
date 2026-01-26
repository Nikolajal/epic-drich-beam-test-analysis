#pragma once

#include <iostream>
#include "TTree.h"
#include "triggers.h"
#include "alcor_spilldata.h"

enum hit_mask
{
    _HITMASK_ring_tag = 1,
    _HITMASK_part_lane = 30,
    _HITMASK_dead_lane = 31
};

struct alcor_recodata_struct
{
    float hit_x;
    float hit_y;
    float hit_t;
    //  4 * global_index + tdc
    uint32_t channel;
    float time;
    float fine_tune;
    uint32_t hit_mask;

    alcor_recodata_struct() = default;
};

// idea: weighted enytry in the R histo (r-phi shape of the square sensor)

class alcor_recodata
{
private:
    //  Add spill info
    std::vector<trigger_struct> triggers;
    std::vector<trigger_struct> *triggers_ptr = &triggers;
    std::vector<alcor_recodata_struct> recodata;
    std::vector<alcor_recodata_struct> *recodata_ptr = &recodata;

public:
    //  Constructors
    alcor_recodata() = default;
    explicit alcor_recodata(const std::vector<alcor_recodata_struct> &d) : recodata(d) {}

    //  Getters
    //  --- Copied value
    std::vector<alcor_recodata_struct> get() const { return recodata; }
    alcor_recodata_struct get(int i) const { return recodata[i]; }
    float get_hit_x(int i) const { return recodata[i].hit_x; }
    float get_hit_x_rnd(int i) const { return recodata[i].hit_x + (_rnd_(_global_gen_) * 3.0 - 1.5); }
    float get_hit_y(int i) const { return recodata[i].hit_y; }
    float get_hit_y_rnd(int i) const { return recodata[i].hit_y + (_rnd_(_global_gen_) * 3.0 - 1.5); }
    float get_hit_t(int i) const { return recodata[i].hit_t; }
    uint32_t get_channel(int i) const { return recodata[i].channel; }
    float get_hit_time(int i) const { return recodata[i].time; }
    uint32_t get_hit_mask(int i) const { return recodata[i].hit_mask; }
    std::vector<trigger_struct> get_triggers() const { return triggers; }
    //  --- Linked reference
    alcor_recodata_struct &get_link(int i) { return recodata[i]; }
    std::vector<trigger_struct> &get_triggers_link() { return triggers; }

    // --- Derived, getters to combinationa of class variables
    int get_hit_tdc(int i) const { return recodata[i].channel % 4; }
    int get_device(int i) const { return 192 + (recodata[i].channel / 1024); }
    int get_fifo(int i) const { return (recodata[i].channel % 1024) / 32; }
    int get_chip(int i) const { return get_fifo(i) / 4; };
    int get_eo_channel(int i) const { return (recodata[i].channel % 1024) % 32 + 32 * (get_chip(i) % 2); }
    int get_column(int i) const { return ((recodata[i].channel % 1024) % 32) / 4; };
    int get_pixel(int i) const { return ((recodata[i].channel % 1024) % 32) % 4; };
    int get_calib_index(int i) const { return get_hit_tdc(i) + 4 * get_eo_channel(i) + 128 * get_chip(i); };
    int get_device_index(int i) const { return get_eo_channel(i) + 64 * (get_chip(i) / 2); };
    int get_global_index(int i) const { return recodata[i].channel; };
    float get_hit_r(int i) const { return sqrt(get_hit_x(i) * get_hit_x(i) + get_hit_y(i) * get_hit_y(i)); }
    float get_hit_r(int i, std::array<float, 2> v) const { return sqrt((get_hit_x(i) - v[0]) * (get_hit_x(i) - v[0]) + (get_hit_y(i) - v[1]) * (get_hit_y(i) - v[1])); }
    float get_hit_r_rnd(int i, std::array<float, 2> v) const { return sqrt((get_hit_x(i) + (_rnd_(_global_gen_) * 3.0 - 1.5) - v[0]) * (get_hit_x(i) + (_rnd_(_global_gen_) * 3.0 - 1.5) - v[0]) + (get_hit_y(i) + (_rnd_(_global_gen_) * 3.0 - 1.5) - v[1]) * (get_hit_y(i) + (_rnd_(_global_gen_) * 3.0 - 1.5) - v[1])); }
    float get_hit_phi(int i) const { return atan2(get_hit_y(i), get_hit_x(i)); }
    float get_hit_phi(int i, std::array<float, 2> v) const { return atan2(get_hit_y(i) - v[1], get_hit_x(i) - v[0]); }
    float get_hit_phi_rnd(int i, std::array<float, 2> v) const { return atan2(get_hit_y(i) + (_rnd_(_global_gen_) * 3.0 - 1.5) - v[1], get_hit_x(i) + (_rnd_(_global_gen_) * 3.0 - 1.5) - v[0]); }

    //  Setters
    //  --- Copied value
    void set_recodata(std::vector<alcor_recodata_struct> v) { recodata = v; }
    void set_hit_x(int i, float v) { recodata[i].hit_x = v; }
    void set_hit_y(int i, float v) { recodata[i].hit_y = v; }
    void set_hit_t(int i, float v) { recodata[i].hit_t = v; }
    void set_channel(int i, uint32_t v) { recodata[i].channel = v; }
    void set_hit_mask(int i, uint32_t v) { recodata[i].hit_mask = v; }
    void add_hit_mask(int i, uint32_t v) { recodata[i].hit_mask += v; }
    void set_time(int i, float v) { recodata[i].time = v; }
    void set_triggers(const std::vector<trigger_struct> &v) { triggers = v; }
    //  --- Linked reference
    void set_recodata_link(std::vector<alcor_recodata_struct> &v) { recodata = v; }
    void set_triggers_link(std::vector<trigger_struct> &v) { triggers = v; }

    //  I/O utilities
    void add_trigger(uint8_t index, uint16_t coarse);
    void add_trigger(trigger_struct hit) { triggers.push_back(hit); }
    void add_hit(float hit_x, float hit_y, float hit_t, uint32_t channel, float time, uint32_t mask = 0);
    void add_hit(alcor_recodata_struct hit) { recodata.push_back(hit); }
    void clear()
    {
        recodata.clear();
        triggers.clear();
        recodata.shrink_to_fit();
        triggers.shrink_to_fit();
    }
    void link_to_tree(TTree *input_tree);
    void write_to_tree(TTree *output_tree);
    void find_rings(float_t distance_length_cut, float_t distance_time_cut);
    // void prepare_tree_fill();
    // void get_entry();
};

void alcor_recodata::add_trigger(uint8_t index, uint16_t coarse)
{
    trigger_struct new_trigger(index, coarse);
    triggers.push_back(new_trigger);
}

void alcor_recodata::add_hit(float hit_x, float hit_y, float hit_t, uint32_t channel, float time, uint32_t hit_mask)
{
    alcor_recodata_struct new_hit;
    new_hit.hit_x = hit_x;
    new_hit.hit_y = hit_y;
    new_hit.hit_t = hit_t;
    new_hit.channel = channel;
    new_hit.time = time;
    new_hit.hit_mask = hit_mask;
    recodata.push_back(new_hit);
}

void alcor_recodata::link_to_tree(TTree *input_tree)
{
    if (!input_tree)
        return;
    input_tree->SetBranchAddress("recodata", &recodata_ptr);
    input_tree->SetBranchAddress("triggers", &triggers_ptr);
}

void alcor_recodata::write_to_tree(TTree *output_tree)
{
    if (!output_tree)
        return;
    output_tree->Branch("recodata", &recodata);
    output_tree->Branch("triggers", &triggers);
}

void alcor_recodata::find_rings(float_t distance_length_cut, float_t distance_time_cut)
{
    auto i_index = -1;
    std::map<int, std::map<int, std::vector<int>>> proximity_hit_list;
    for (auto current_hit : recodata)
    {
        i_index++;
        for (auto j_index = i_index + 1; j_index < recodata.size(); j_index++)
        {
            if (get_device(i_index) != get_device(j_index))
                continue;
            auto distance_length = sqrt((get_hit_r(j_index) - get_hit_r(i_index)) * (get_hit_r(j_index) - get_hit_r(i_index)));
            auto distance_time = sqrt((get_hit_t(i_index) - get_hit_t(j_index)) * (get_hit_t(i_index) - get_hit_t(j_index)));
            if ((distance_length < distance_length_cut) && (distance_time < distance_time_cut))
                proximity_hit_list[get_device(i_index)][i_index].push_back(j_index);
        }
    }
    for (auto [current_device, proximity_hit_list_device] : proximity_hit_list)
    {
        auto selected_main_index = -1;
        auto selected_size = -1;
        for (auto [main_index, current_index_list] : proximity_hit_list_device)
        {
            if (current_index_list.size() > selected_size)
                continue;
            selected_main_index = main_index;
            selected_size = current_index_list.size();
        }
        add_hit_mask(selected_main_index, encode_bit(_HITMASK_ring_tag));
        for (auto current_index : proximity_hit_list[current_device][selected_main_index])
            add_hit_mask(current_index, encode_bit(_HITMASK_ring_tag));
    }
}

#ifdef __ROOTCLING__
#pragma link C++ struct trigger_struct + ;
#pragma link C++ struct alcor_recodata + ;
#pragma link C++ struct alcor_recodata_struct + ;
#pragma link C++ struct std::vector < alcor_recodata_struct> + ;
#endif
