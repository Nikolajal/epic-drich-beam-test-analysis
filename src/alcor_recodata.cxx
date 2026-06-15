#include "alcor_recodata.h"
#include <mist/logger/logger.h>
#include "alcor_data.h"

// =============================================================================
// Trigger search
// =============================================================================

std::optional<TriggerEvent> AlcorRecodata::get_trigger_by_index(uint8_t index) const
{
    // Use the const-ref accessor — get_triggers() returns by reference now
    // (no per-call deep copy of the trigger vector).
    const auto &current_trigger = get_triggers();
    auto it = std::find_if(current_trigger.begin(), current_trigger.end(),
                           [index](const TriggerEvent &t)
                           { return t.index == index; });
    if (it != current_trigger.end())
        return *it;
    return std::nullopt;
}

// =============================================================================
// I/O utilities
// =============================================================================

void AlcorRecodata::clear()
{
    recodata.clear();
    triggers.clear();
    recodata.shrink_to_fit();
    triggers.shrink_to_fit();
}

bool AlcorRecodata::link_to_tree(TTree *input_tree)
{
    if (!input_tree)
    {
        mist::logger::error("(AlcorRecodata::link_to_tree) input_tree is null");
        return false;
    }
    if (input_tree->GetEntries() == 0)
    {
        mist::logger::error("(AlcorRecodata::link_to_tree) input_tree is empty");
        return false;
    }
    if (!input_tree->GetBranch("recodata") || !input_tree->GetBranch("triggers"))
    {
        mist::logger::error("(AlcorRecodata::link_to_tree) missing expected branches");
        input_tree->Print();
        return false;
    }
    input_tree->SetBranchAddress("recodata", &recodata_ptr);
    input_tree->SetBranchAddress("triggers", &triggers_ptr);
    return true;
}

void AlcorRecodata::write_to_tree(TTree *output_tree)
{
    if (!output_tree)
        return;
    output_tree->Branch("recodata", &recodata);
    output_tree->Branch("triggers", &triggers);
}

// =============================================================================
// Analysis utilities
// =============================================================================

void AlcorRecodata::find_rings(float distance_length_cut, float distance_time_cut)
{
    std::map<int, std::map<int, std::vector<int>>> proximity_hit_list;
    // Index-only loop: the previous `for (auto current_hit : recodata)` copied
    // every Hit struct just to throw the value away.
    for (int i_index = 0; i_index < (int)recodata.size(); ++i_index)
    {
        for (auto j_index = i_index + 1; j_index < (int)recodata.size(); j_index++)
        {
            if (get_device(i_index) != get_device(j_index))
                continue;
            float distance_length = std::fabs(get_hit_r(j_index) - get_hit_r(i_index));
            float distance_time = std::fabs(get_hit_t(i_index) - get_hit_t(j_index));
            if (distance_length < distance_length_cut && distance_time < distance_time_cut)
                proximity_hit_list[get_device(i_index)][i_index].push_back(j_index);
        }
    }

    for (auto &[current_device, proximity_hit_list_device] : proximity_hit_list)
    {
        (void)current_device; // device key not used directly; partners come from proximity_hit_list_device.
        int selected_main_index = -1;
        int selected_size = -1;
        for (auto &[main_index, current_index_list] : proximity_hit_list_device)
        {
            if ((int)current_index_list.size() <= selected_size)
                continue;
            selected_main_index = main_index;
            selected_size = current_index_list.size();
        }
        // Guard: defensive against any future code path that could leave the
        // device entry without a winning main_index.  add_hit_mask(-1, …) would
        // index recodata[-1] (UB on std::vector::operator[]).
        if (selected_main_index < 0)
            continue;
        add_hit_mask(selected_main_index, encode_bit(HitmaskRingTagFirst));
        // Reuse the already-destructured device-local map instead of repeating
        // the proximity_hit_list[current_device][...] double lookup.
        for (auto current_index : proximity_hit_list_device[selected_main_index])
            add_hit_mask(current_index, encode_bit(HitmaskRingTagFirst));
    }
}

//  CONVENTION-BREAK NOTICE — see same notice in src/alcor_finedata.cxx.
//  `is_ring_tagged` was inline in the header until the IWYU
//  sweep; that sweep was driven by a misdiagnosed autoload problem.
//  Per project convention the body belongs in the header.  Not reverting
//  blindly; do not generalise.
bool AlcorRecodata::is_ring_tagged(int i)
{
    return check_hit_mask(i, encode_bits({HitmaskRingTagFirst, HitmaskRingTagSecond}));
}
