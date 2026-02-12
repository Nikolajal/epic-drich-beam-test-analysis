#include "alcor_recodata.h"

//  Constructors
//  No implementations needed
//  TODO: recodata trigger fills every event

//  Getters
//  --- Pure getters
std::vector<alcor_recodata_struct> alcor_recodata::get_recodata() const { return recodata; }
std::vector<alcor_recodata_struct> *alcor_recodata::get_recodata_ptr() { return recodata_ptr; }
alcor_recodata_struct alcor_recodata::get_recodata(int i) const { return recodata[i]; }
std::vector<trigger_struct> alcor_recodata::get_triggers() const { return triggers; }
std::vector<trigger_struct> *alcor_recodata::get_triggers_ptr() { return triggers_ptr; }
int alcor_recodata::get_global_index(int i) const { return recodata[i].global_index; }
float alcor_recodata::get_hit_x(int i) const { return recodata[i].hit_x; }
float alcor_recodata::get_hit_y(int i) const { return recodata[i].hit_y; }
uint32_t alcor_recodata::get_hit_mask(int i) const { return recodata[i].hit_mask; }
float alcor_recodata::get_hit_t(int i) const { return recodata[i].hit_t; }

//  --- Reference getters
std::vector<alcor_recodata_struct> &alcor_recodata::get_recodata_link() { return recodata; }
alcor_recodata_struct &alcor_recodata::get_recodata_link(int i) { return recodata[i]; }
std::vector<trigger_struct> &alcor_recodata::get_triggers_link() { return triggers; }

//  --- Derived Getters
//  --- --- Polar coordinates
float alcor_recodata::get_hit_r(int i) const { return get_hit_r(i, {0.f, 0.f}); }
float alcor_recodata::get_hit_r(int i, std::array<float, 2> v) const { return std::hypot(get_hit_x(i) - v[0], get_hit_y(i) - v[1]); }
float alcor_recodata::get_hit_phi(int i) const { return get_hit_phi(i, {0.f, 0.f}); }
float alcor_recodata::get_hit_phi(int i, std::array<float, 2> v) const { return std::atan2(get_hit_y(i) - v[1], get_hit_x(i) - v[0]); }
//  --- --- Randomised coordinates
float alcor_recodata::get_hit_x_rnd(int i) const { return recodata[i].hit_x + (_rnd_(_global_gen_) * 3.0 - 1.5); }
float alcor_recodata::get_hit_y_rnd(int i) const { return recodata[i].hit_y + (_rnd_(_global_gen_) * 3.0 - 1.5); }
float alcor_recodata::get_hit_r_rnd(int i) const { return get_hit_r(i, {0.f, 0.f}); }
float alcor_recodata::get_hit_r_rnd(int i, std::array<float, 2> v) const { return std::hypot(get_hit_x_rnd(i) - v[0], get_hit_y_rnd(i) - v[1]); }
float alcor_recodata::get_hit_phi_rnd(int i) const { return get_hit_phi_rnd(i, {0.f, 0.f}); }
float alcor_recodata::get_hit_phi_rnd(int i, std::array<float, 2> v) const { return std::atan2(get_hit_y_rnd(i) - v[1], get_hit_x_rnd(i) - v[0]); }
//  --- --- Reconstruction info
int alcor_recodata::get_hit_tdc(int i) const { return recodata[i].global_index % 4; }
int alcor_recodata::get_device(int i) const { return 192 + (recodata[i].global_index / 1024); }
int alcor_recodata::get_fifo(int i) const { return (recodata[i].global_index % 1024) / 32; }
int alcor_recodata::get_chip(int i) const { return get_fifo(i) / 4; }
int alcor_recodata::get_eo_channel(int i) const { return (recodata[i].global_index % 1024) % 32 + 32 * (get_chip(i) % 2); }
int alcor_recodata::get_column(int i) const { return ((recodata[i].global_index % 1024) % 32) / 4; }
int alcor_recodata::get_pixel(int i) const { return ((recodata[i].global_index % 1024) % 32) % 4; }
int alcor_recodata::get_calib_index(int i) const { return get_hit_tdc(i) + 4 * get_eo_channel(i) + 128 * get_chip(i); }
int alcor_recodata::get_device_index(int i) const { return get_eo_channel(i) + 64 * (get_chip(i) / 2); }
//  --- --- Trigger info
std::optional<trigger_struct> alcor_recodata::get_trigger_by_index(uint8_t index) const
{
    auto current_trigger = get_triggers();
    auto it = std::find_if(current_trigger.begin(), current_trigger.end(), [index](const trigger_struct &t)
                           { return t.index == index; });
    if (it != current_trigger.end())
        return *it;
    else
        return std::nullopt;
}
std::optional<trigger_struct> alcor_recodata::get_timing_trigger()const { return get_trigger_by_index(_TRIGGER_TIMING_); }

//  Setters
//  --- Pure setters
void alcor_recodata::set_recodata(std::vector<alcor_recodata_struct> v) { recodata = v; }
void alcor_recodata::set_recodata(int i, alcor_recodata_struct v) { recodata[i] = v; }
void alcor_recodata::set_triggers(const std::vector<trigger_struct> v) { triggers = v; }
void alcor_recodata::set_recodata_ptr(std::vector<alcor_recodata_struct> *v) { recodata_ptr = v; }
void alcor_recodata::set_triggers_ptr(std::vector<trigger_struct> *v) { triggers_ptr = v; }
void alcor_recodata::set_global_index(int i, uint32_t v) { recodata[i].global_index = v; }
void alcor_recodata::set_hit_x(int i, float v) { recodata[i].hit_x = v; }
void alcor_recodata::set_hit_y(int i, float v) { recodata[i].hit_y = v; }
void alcor_recodata::set_hit_mask(int i, uint32_t v) { recodata[i].hit_mask = v; }
void alcor_recodata::set_hit_t(int i, float v) { recodata[i].hit_t = v; }
//  --- Reference setters
void alcor_recodata::set_recodata_link(std::vector<alcor_recodata_struct> &v)
{
    recodata = v;
    recodata_ptr = &recodata;
}
void alcor_recodata::set_triggers_link(std::vector<trigger_struct> &v)
{
    triggers = v;
    triggers_ptr = &triggers;
}

//  Add utilities
void alcor_recodata::add_hit_mask(int i, uint32_t v) { recodata[i].hit_mask |= v; }
void alcor_recodata::add_trigger(uint8_t index, uint16_t coarse, float fine_time) { triggers.emplace_back(index, coarse, fine_time); }
void alcor_recodata::add_trigger(trigger_struct hit) { triggers.push_back(hit); }
void alcor_recodata::add_hit(uint32_t gi, float x, float y, uint32_t mask, float ht) { recodata.emplace_back(gi, x, y, mask, ht); }
void alcor_recodata::add_hit(alcor_recodata_struct hit) { recodata.push_back(hit); }

//  Bool checks
bool alcor_recodata::check_trigger(uint8_t v) { return get_trigger_by_index(v).has_value(); }
bool alcor_recodata::is_start_of_spill() { return check_trigger(_TRIGGER_START_OF_SPILL_); }
bool alcor_recodata::is_first_frames() { return check_trigger(_TRIGGER_FIRST_FRAMES_); }
bool alcor_recodata::is_timing_available() { return check_trigger(_TRIGGER_TIMING_); }
bool alcor_recodata::is_embedded_tracking_available() { return check_trigger(_TRIGGER_TRACKING_); }
bool alcor_recodata::is_ring_found() { return check_trigger(_TRIGGER_RING_FOUND_); }
bool alcor_recodata::check_hit_mask(int i, uint32_t v) { return (get_hit_mask(i) & v) != 0; }
bool alcor_recodata::is_afterpulse(int i) { return check_hit_mask(i, encode_bit(_HITMASK_afterpulse)); }
bool alcor_recodata::is_ring_tagged(int i) { return check_hit_mask(i, encode_bits({_HITMASK_ring_tag_first, _HITMASK_ring_tag_second})); }

//  I/O utilities
void alcor_recodata::clear()
{
    recodata.clear();
    triggers.clear();
    recodata.shrink_to_fit();
    triggers.shrink_to_fit();
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

//  Analysis utilities
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