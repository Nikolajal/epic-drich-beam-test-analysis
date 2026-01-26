#pragma once

#include <vector>
#include <iostream>
#include <cstdint>

#include "alcor_lightdata.h"

struct data_mask_struct
{
    uint8_t device;
    uint32_t mask;
};

struct alcor_spilldata_struct
{
    //  I/O utilities for HPC
    std::unordered_map<uint8_t, uint32_t> dead_mask;
    std::unordered_map<uint8_t, uint32_t> participants_mask;
    std::unordered_map<uint32_t, alcor_lightdata_struct> frame_and_lightdata;

    //  I/O utilities for reading/writing
    std::vector<data_mask_struct> dead_mask_list;
    std::vector<data_mask_struct> participants_mask_list;
    std::vector<uint32_t> frame_reference;
    std::vector<alcor_lightdata_struct> lightdata_list_in_frame;
    std::vector<data_mask_struct> *dead_mask_list_ptr = &dead_mask_list;
    std::vector<data_mask_struct> *participants_mask_list_ptr = &participants_mask_list;
    std::vector<uint32_t> *frame_reference_ptr = &frame_reference;
    std::vector<alcor_lightdata_struct> *lightdata_list_in_frame_ptr = &lightdata_list_in_frame;

    void clear()
    {
        //  Clear maps
        dead_mask.clear();
        participants_mask.clear();
        
        //  --- Clear nested vectors in frame_and_lightdata
        for (auto &kv : frame_and_lightdata)
            kv.second.clear();

        // Clear frame_and_lightdata map
        frame_and_lightdata.clear();

        // Clear top-level vectors
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
};

class alcor_spilldata : public alcor_lightdata
{
private:
    // Data structure
    alcor_spilldata_struct spilldata;
    std::unordered_map<uint32_t, bool> frame_reference_for_deletion;

public:
    //  Constructors
    alcor_spilldata() { spilldata.clear(); };

    //  Getters
    //  --- Copied value
    alcor_spilldata_struct get_spilldata() const { return spilldata; }
    std::unordered_map<uint32_t, alcor_lightdata_struct> get_frame() const { return spilldata.frame_and_lightdata; }
    std::unordered_map<uint8_t, uint32_t> get_participants_mask() const { return spilldata.participants_mask; }
    std::unordered_map<uint8_t, uint32_t> get_dead_mask() const { return spilldata.dead_mask; }
    std::vector<alcor_lightdata_struct> get_frame_list() const { return spilldata.lightdata_list_in_frame; }
    std::vector<uint32_t> get_frame_reference_list() const { return spilldata.frame_reference; }
    //  --- Linked reference
    alcor_spilldata_struct &get_spilldata_link() { return spilldata; }
    std::unordered_map<uint32_t, alcor_lightdata_struct> &get_frame_link() { return spilldata.frame_and_lightdata; }
    std::unordered_map<uint8_t, uint32_t> &get_participants_mask_link() { return spilldata.participants_mask; }
    std::unordered_map<uint8_t, uint32_t> &get_dead_mask_link() { return spilldata.dead_mask; }
    std::vector<alcor_lightdata_struct> &get_frame_list_link() { return spilldata.lightdata_list_in_frame; }
    std::vector<uint32_t> &get_frame_reference_list_link() { return spilldata.frame_reference; }

    //   Setters
    //  --- Copied value
    void set_spilldata(alcor_spilldata_struct v) { spilldata = v; }
    void set_frame(std::unordered_map<uint32_t, alcor_lightdata_struct> v) { spilldata.frame_and_lightdata = v; }
    void set_participants_mask(std::unordered_map<uint8_t, uint32_t> v) { spilldata.participants_mask = v; }
    void set_dead_mask(std::unordered_map<uint8_t, uint32_t> v) { spilldata.dead_mask = v; }
    //  --- Linked reference
    void set_spilldata_link(alcor_spilldata_struct &v) { spilldata = v; }
    void set_frame_link(std::unordered_map<uint32_t, alcor_lightdata_struct> &v) { spilldata.frame_and_lightdata = v; }
    void set_participants_mask_link(std::unordered_map<uint8_t, uint32_t> &v) { spilldata.participants_mask = v; }
    void set_dead_mask_link(std::unordered_map<uint8_t, uint32_t> &v) { spilldata.dead_mask = v; }

    //  Utilities
    void clear()
    {
        spilldata.clear();
        frame_reference_for_deletion.clear();
    }
    bool has_trigger(uint32_t index_of_frame) { return spilldata.frame_and_lightdata[index_of_frame].trigger_hits.size(); }
    void add_trigger_to_frame(uint32_t index_of_frame, trigger_struct trg) { spilldata.frame_and_lightdata[index_of_frame].trigger_hits.push_back(trg); }
    void do_not_write_frame(uint32_t index_of_frame) { frame_reference_for_deletion[index_of_frame] = true; }
    std::map<uint32_t, std::vector<uint8_t>> get_not_dead_participants();

    //  I/O operations
    void link_to_tree(TTree *input_tree);
    void write_to_tree(TTree *output_tree);
    void prepare_tree_fill();
    void get_entry();
};

std::map<uint32_t, std::vector<uint8_t>> alcor_spilldata::get_not_dead_participants()
{
    //  Result map
    std::map<uint32_t, std::vector<uint8_t>> result;

    for (const auto &pm : spilldata.participants_mask_list)
    {
        uint8_t device = pm.device;
        uint32_t participants_mask = pm.mask;

        // Trova il dead mask per questo device
        uint32_t dead_mask = 0;
        for (const auto &dm : spilldata.dead_mask_list)
            if (dm.device == device)
            {
                dead_mask = dm.mask;
                break;
            }

        // Maschera finale: partec. vivi = partecipanti & ~dead
        uint32_t not_dead_participants_mask = (participants_mask & (~dead_mask));

        // Decodifica e salva
        result[device] = decode_bits(not_dead_participants_mask);
    }
    return result;
}

//  I/O Operations
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
    // Clear previous content
    spilldata.dead_mask_list.clear();
    spilldata.participants_mask_list.clear();
    spilldata.frame_reference.clear();
    spilldata.lightdata_list_in_frame.clear();
    spilldata.lightdata_list_in_frame_ptr = &spilldata.lightdata_list_in_frame;

    // Reserve memory for small maps (dead/participants)
    spilldata.dead_mask_list.reserve(spilldata.dead_mask.size());
    spilldata.participants_mask_list.reserve(spilldata.participants_mask.size());

    // Fill dead/participants lists
    for (auto &[device, mask] : spilldata.dead_mask)
        spilldata.dead_mask_list.push_back({device, mask});
    for (auto &[device, mask] : spilldata.participants_mask)
        spilldata.participants_mask_list.push_back({device, mask});

    // Now handle frames one by one to prevent huge RAM spike
    for (auto it = spilldata.frame_and_lightdata.begin(); it != spilldata.frame_and_lightdata.end();)
    {
        uint32_t frame_id = it->first;

        if (!frame_reference_for_deletion[frame_id])
        {
            // Append to vector for ROOT
            spilldata.frame_reference.push_back(frame_id);
            spilldata.lightdata_list_in_frame.push_back(std::move(it->second));

            // Immediately release map element to avoid RAM accumulation
            it = spilldata.frame_and_lightdata.erase(it);
        }
        else
        {
            ++it; // skip frame marked for deletion
        }
    }
}
void alcor_spilldata::get_entry()
{
}

#ifdef __ROOTCLING__
#pragma link C++ struct data_mask_struct + ;
#pragma link C++ struct alcor_spilldata_struct + ;
#pragma link C++ class std::vector < data_mask_struct> + ;
#pragma link C++ class std::vector < uint32_t> + ;
#pragma link C++ class std::vector < alcor_lightdata_struct> + ;
#pragma link C++ class std::unordered_map < uint8_t, uint32_t> + ;
#pragma link C++ class std::unordered_map < uint8_t, uint32_t> + ;
#pragma link C++ class std::unordered_map < uint32_t, alcor_lightdata_struct> + ;
#endif