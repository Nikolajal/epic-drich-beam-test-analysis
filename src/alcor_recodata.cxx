#include "alcor_recodata.h"
#include <cmath>
#include <sstream>
#include <iostream>

// ----------------------
// I/O helpers
// ----------------------

void alcor_recodata::add_trigger(uint8_t index, uint16_t coarse, float fine_time)
{
    trigger_struct new_trigger(index, coarse, fine_time);
    triggers.push_back(new_trigger);
}
void alcor_recodata::add_trigger(trigger_struct hit) { triggers.push_back(hit); }
void alcor_recodata::add_hit(float hit_x_val, float hit_y_val, float hit_t_val, uint32_t channel_val, float time_val, uint32_t hit_mask_val)
{
    alcor_recodata_struct new_hit;
    new_hit.hit_x = hit_x_val;
    new_hit.hit_y = hit_y_val;
    new_hit.hit_t = hit_t_val;
    new_hit.channel = channel_val;
    new_hit.time = time_val;
    new_hit.hit_mask = hit_mask_val;
    recodata.push_back(new_hit);
}

void alcor_recodata::add_hit_mask(int i, uint32_t v) { recodata[i].hit_mask += v; }

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
void alcor_recodata::clear()
{
    recodata.clear();
    triggers.clear();
    recodata.shrink_to_fit();
    triggers.shrink_to_fit();
}

// ----------------------
// Getters
// ----------------------

std::vector<alcor_recodata_struct> alcor_recodata::get() const { return recodata; }
int alcor_recodata::get_hit_tdc(int i) const { return recodata[i].channel % 4; }
int alcor_recodata::get_device(int i) const { return 192 + (recodata[i].channel / 1024); }
int alcor_recodata::get_fifo(int i) const { return (recodata[i].channel % 1024) / 32; }
int alcor_recodata::get_chip(int i) const { return get_fifo(i) / 4; }
int alcor_recodata::get_eo_channel(int i) const { return (recodata[i].channel % 1024) % 32 + 32 * (get_chip(i) % 2); }
int alcor_recodata::get_column(int i) const { return ((recodata[i].channel % 1024) % 32) / 4; }
int alcor_recodata::get_pixel(int i) const { return ((recodata[i].channel % 1024) % 32) % 4; }
int alcor_recodata::get_calib_index(int i) const { return get_hit_tdc(i) + 4 * get_eo_channel(i) + 128 * get_chip(i); }
int alcor_recodata::get_device_index(int i) const { return get_eo_channel(i) + 64 * (get_chip(i) / 2); }
int alcor_recodata::get_global_index(int i) const { return recodata[i].channel; }
float alcor_recodata::get_hit_r(int i) const { return std::sqrt(get_hit_x(i) * get_hit_x(i) + get_hit_y(i) * get_hit_y(i)); }
float alcor_recodata::get_hit_r(int i, std::array<float, 2> v) const
{
    return std::sqrt((get_hit_x(i) - v[0]) * (get_hit_x(i) - v[0]) +
                     (get_hit_y(i) - v[1]) * (get_hit_y(i) - v[1]));
}
float alcor_recodata::get_hit_r_rnd(int i, std::array<float, 2> v) const
{
    return std::sqrt((get_hit_x_rnd(i) - v[0]) * (get_hit_x_rnd(i) - v[0]) +
                     (get_hit_y_rnd(i) - v[1]) * (get_hit_y_rnd(i) - v[1]));
}
float alcor_recodata::get_hit_t(int i) const { return recodata[i].hit_t; }
float alcor_recodata::get_hit_x(int i) const { return recodata[i].hit_x; }
float alcor_recodata::get_hit_x_rnd(int i) const { return recodata[i].hit_x + (_rnd_(_global_gen_) * 3.0 - 1.5); }
float alcor_recodata::get_hit_y(int i) const { return recodata[i].hit_y; }
float alcor_recodata::get_hit_y_rnd(int i) const { return recodata[i].hit_y + (_rnd_(_global_gen_) * 3.0 - 1.5); }
float alcor_recodata::get_hit_phi(int i) const { return std::atan2(get_hit_y(i), get_hit_x(i)); }
float alcor_recodata::get_hit_phi(int i, std::array<float, 2> v) const { return std::atan2(get_hit_y(i) - v[1], get_hit_x(i) - v[0]); }
float alcor_recodata::get_hit_phi_rnd(int i, std::array<float, 2> v) const { return std::atan2(get_hit_y_rnd(i) - v[1], get_hit_x_rnd(i) - v[0]); }
uint32_t alcor_recodata::get_hit_mask(int i) const { return recodata[i].hit_mask; }
void alcor_recodata::add_hit(alcor_recodata_struct hit) { recodata.push_back(hit); }
std::vector<trigger_struct> alcor_recodata::get_triggers() const { return triggers; }
alcor_recodata_struct &alcor_recodata::get_link(int i) { return recodata[i]; }
std::vector<trigger_struct> &alcor_recodata::get_triggers_link() { return triggers; }
bool alcor_recodata::check_hit_mask(int i, uint32_t v) { return (get_hit_mask(i) & v) != 0; }
bool alcor_recodata::is_afterpulse(int i) { return check_hit_mask(i, encode_bit(_HITMASK_afterpulse)); }

// ----------------------
// Main logic
// ----------------------

void alcor_recodata::find_rings(float distance_length_cut, float distance_time_cut)
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
            float distance_length = std::sqrt((get_hit_r(j_index) - get_hit_r(i_index)) * (get_hit_r(j_index) - get_hit_r(i_index)));
            float distance_time = std::sqrt((get_hit_t(i_index) - get_hit_t(j_index)) * (get_hit_t(i_index) - get_hit_t(j_index)));
            if ((distance_length < distance_length_cut) && (distance_time < distance_time_cut))
                proximity_hit_list[get_device(i_index)][i_index].push_back(j_index);
        }
    }

    for (auto [current_device, proximity_hit_list_device] : proximity_hit_list)
    {
        int selected_main_index = -1;
        int selected_size = -1;
        for (auto [main_index, current_index_list] : proximity_hit_list_device)
        {
            if (current_index_list.size() > selected_size)
                continue;
            selected_main_index = main_index;
            selected_size = current_index_list.size();
        }
        add_hit_mask(selected_main_index, encode_bit(_HITMASK_ring_tag_first));
        for (auto current_index : proximity_hit_list[current_device][selected_main_index])
            add_hit_mask(current_index, encode_bit(_HITMASK_ring_tag_first));
    }
}