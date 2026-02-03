#include "alcor_spilldata.h"

// ----------------------
// alcor_spilldata_struct
// ----------------------
void alcor_spilldata_struct::clear()
{
    dead_mask.clear();
    participants_mask.clear();
    for (auto &kv : frame_and_lightdata)
        kv.second.clear();
    frame_and_lightdata.clear();
    for (auto &ld : lightdata_list_in_frame)
        ld.clear();
    lightdata_list_in_frame.clear();
    lightdata_list_in_frame.shrink_to_fit();
    lightdata_list_in_frame_ptr = &lightdata_list_in_frame;

    dead_mask_list.clear();
    dead_mask_list.shrink_to_fit();
    dead_mask_list_ptr = &dead_mask_list;

    participants_mask_list.clear();
    participants_mask_list.shrink_to_fit();
    participants_mask_list_ptr = &participants_mask_list;

    frame_reference.clear();
    frame_reference.shrink_to_fit();
    frame_reference_ptr = &frame_reference;
}

// ----------------------
// alcor_spilldata
// ----------------------
alcor_spilldata::alcor_spilldata()
{
    spilldata.clear();
}

// Getters (copied value)
alcor_spilldata_struct alcor_spilldata::get_spilldata() const { return spilldata; }
std::unordered_map<uint32_t, alcor_lightdata_struct> alcor_spilldata::get_frame() const { return spilldata.frame_and_lightdata; }
std::unordered_map<uint8_t, uint32_t> alcor_spilldata::get_participants_mask() const { return spilldata.participants_mask; }
std::unordered_map<uint8_t, uint32_t> alcor_spilldata::get_dead_mask() const { return spilldata.dead_mask; }
std::vector<alcor_lightdata_struct> alcor_spilldata::get_frame_list() const { return spilldata.lightdata_list_in_frame; }
std::vector<uint32_t> alcor_spilldata::get_frame_reference_list() const { return spilldata.frame_reference; }

// Getters (linked reference)
alcor_spilldata_struct &alcor_spilldata::get_spilldata_link() { return spilldata; }
std::unordered_map<uint32_t, alcor_lightdata_struct> &alcor_spilldata::get_frame_link() { return spilldata.frame_and_lightdata; }
std::unordered_map<uint8_t, uint32_t> &alcor_spilldata::get_participants_mask_link() { return spilldata.participants_mask; }
std::unordered_map<uint8_t, uint32_t> &alcor_spilldata::get_dead_mask_link() { return spilldata.dead_mask; }
std::vector<alcor_lightdata_struct> &alcor_spilldata::get_frame_list_link() { return spilldata.lightdata_list_in_frame; }
std::vector<uint32_t> &alcor_spilldata::get_frame_reference_list_link() { return spilldata.frame_reference; }

// Setters (copied value)
void alcor_spilldata::set_spilldata(alcor_spilldata_struct v) { spilldata = v; }
void alcor_spilldata::set_frame(std::unordered_map<uint32_t, alcor_lightdata_struct> v) { spilldata.frame_and_lightdata = v; }
void alcor_spilldata::set_participants_mask(std::unordered_map<uint8_t, uint32_t> v) { spilldata.participants_mask = v; }
void alcor_spilldata::set_dead_mask(std::unordered_map<uint8_t, uint32_t> v) { spilldata.dead_mask = v; }

// Setters (linked reference)
void alcor_spilldata::set_spilldata_link(alcor_spilldata_struct &v) { spilldata = v; }
void alcor_spilldata::set_frame_link(std::unordered_map<uint32_t, alcor_lightdata_struct> &v) { spilldata.frame_and_lightdata = v; }
void alcor_spilldata::set_participants_mask_link(std::unordered_map<uint8_t, uint32_t> &v) { spilldata.participants_mask = v; }
void alcor_spilldata::set_dead_mask_link(std::unordered_map<uint8_t, uint32_t> &v) { spilldata.dead_mask = v; }

// Utilities
void alcor_spilldata::clear()
{
    spilldata.clear();
    frame_reference_for_deletion.clear();
}
bool alcor_spilldata::has_trigger(uint32_t index_of_frame)
{
    return spilldata.frame_and_lightdata[index_of_frame].trigger_hits.size();
}
void alcor_spilldata::add_trigger_to_frame(uint32_t index_of_frame, trigger_struct trg)
{
    spilldata.frame_and_lightdata[index_of_frame].trigger_hits.push_back(trg);
}
void alcor_spilldata::do_not_write_frame(uint32_t index_of_frame)
{
    frame_reference_for_deletion[index_of_frame] = true;
}

std::map<uint32_t, std::vector<uint8_t>> alcor_spilldata::get_not_dead_participants()
{
    std::map<uint32_t, std::vector<uint8_t>> result;
    for (const auto &pm : spilldata.participants_mask_list)
    {
        uint8_t device = pm.device;
        uint32_t participants_mask = pm.mask;
        uint32_t dead_mask = 0;
        for (const auto &dm : spilldata.dead_mask_list)
            if (dm.device == device)
            {
                dead_mask = dm.mask;
                break;
            }
        uint32_t not_dead_participants_mask = (participants_mask & (~dead_mask));
        result[device] = decode_bits(not_dead_participants_mask);
    }
    return result;
}

// I/O operations
void alcor_spilldata::link_to_tree(TTree *input_tree)
{
    if (!input_tree)
        return;
    input_tree->SetBranchAddress("dead_mask", &(spilldata.dead_mask_list_ptr));
    input_tree->SetBranchAddress("participants_mask", &(spilldata.participants_mask_list_ptr));
    input_tree->SetBranchAddress("frame", &(spilldata.frame_reference_ptr));
    input_tree->SetBranchAddress("lightdata", &(spilldata.lightdata_list_in_frame_ptr));
}
void alcor_spilldata::write_to_tree(TTree *output_tree)
{
    if (!output_tree)
        return;
    output_tree->Branch("dead_mask", &spilldata.dead_mask_list);
    output_tree->Branch("participants_mask", &spilldata.participants_mask_list);
    output_tree->Branch("frame", &spilldata.frame_reference);
    output_tree->Branch("lightdata", &spilldata.lightdata_list_in_frame);
}
void alcor_spilldata::prepare_tree_fill()
{
    spilldata.dead_mask_list.clear();
    spilldata.participants_mask_list.clear();
    spilldata.frame_reference.clear();
    spilldata.lightdata_list_in_frame.clear();
    spilldata.lightdata_list_in_frame_ptr = &spilldata.lightdata_list_in_frame;

    spilldata.dead_mask_list.reserve(spilldata.dead_mask.size());
    spilldata.participants_mask_list.reserve(spilldata.participants_mask.size());

    for (auto &[device, mask] : spilldata.dead_mask)
        spilldata.dead_mask_list.push_back({device, mask});
    for (auto &[device, mask] : spilldata.participants_mask)
        spilldata.participants_mask_list.push_back({device, mask});

    for (auto it = spilldata.frame_and_lightdata.begin(); it != spilldata.frame_and_lightdata.end();)
    {
        uint32_t frame_id = it->first;
        if (!frame_reference_for_deletion[frame_id])
        {
            spilldata.frame_reference.push_back(frame_id);
            spilldata.lightdata_list_in_frame.push_back(std::move(it->second));
            it = spilldata.frame_and_lightdata.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
void alcor_spilldata::get_entry() {}
