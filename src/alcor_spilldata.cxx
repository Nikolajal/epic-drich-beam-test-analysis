#include "alcor_spilldata.h"
#include <algorithm>
#include <unordered_map>

// ============================================================================
//  AlcorSpilldataStruct
// ============================================================================

void AlcorSpilldataStruct::clear()
{
    dead_mask.clear();
    participants_mask.clear();

    for (auto &[id, ld] : frame_and_lightdata)
        ld.clear();
    frame_and_lightdata.clear();

    for (auto &ld : lightdata_list_in_frame)
        ld.clear();
    lightdata_list_in_frame.clear();
    lightdata_list_in_frame.shrink_to_fit();

    dead_mask_list.clear();
    dead_mask_list.shrink_to_fit();

    participants_mask_list.clear();
    participants_mask_list.shrink_to_fit();

    frame_reference.clear();
    frame_reference.shrink_to_fit();
    // No `*_ptr` to re-sync — those live on AlcorSpilldata now, and the
    // wrapper is non-movable so their addresses are stable for its lifetime.
}

// ============================================================================
//  AlcorSpilldata — non-trivial utility methods
// ============================================================================

std::map<uint32_t, std::vector<uint8_t>> AlcorSpilldata::get_not_dead_participants()
{
    std::map<uint32_t, std::vector<uint8_t>> result;

    //  Prefer the flat-list representation (populated after TTree::GetEntry).
    if (!spilldata.participants_mask_list.empty())
    {
        // Build a (device → dead_mask) lookup ONCE up front so the inner
        // search over dead_mask_list collapses from O(M) to O(1).  Net cost:
        // O(N + M) instead of O(N·M) where N = participants_mask_list size,
        // M = dead_mask_list size.
        std::unordered_map<uint8_t, uint32_t> device_to_dead_mask;
        device_to_dead_mask.reserve(spilldata.dead_mask_list.size());
        for (const auto &dm : spilldata.dead_mask_list)
            device_to_dead_mask.emplace(dm.device, dm.mask);

        for (const auto &pm : spilldata.participants_mask_list)
        {
            uint32_t dead_mask = 0;
            if (auto it = device_to_dead_mask.find(pm.device);
                it != device_to_dead_mask.end())
                dead_mask = it->second;
            result[pm.device] = decode_bits(pm.mask & ~dead_mask);
        }
    }
    else
    {
        //  Fall back to the map representation (populated during online processing).
        for (const auto &[device, part_mask] : spilldata.participants_mask)
        {
            uint32_t dead_mask = spilldata.dead_mask[device];
            result[device] = decode_bits(part_mask & ~dead_mask);
        }
    }

    return result;
}

// ============================================================================
//  AlcorSpilldata — ROOT TTree I/O
// ============================================================================

void AlcorSpilldata::link_to_tree(TTree *input_tree)
{
    if (!input_tree)
        return;

    // SetBranchAddress takes the ADDRESS OF the pointer slot.  ROOT will
    // dereference *_ptr_ on each GetEntry and write into the vector it
    // refers to.  The slots live on the wrapper (this object) and were
    // anchored to spilldata's vectors in sync_ptrs_().  Since the wrapper
    // is non-movable, the addresses below remain valid for its lifetime.
    input_tree->SetBranchAddress("dead_mask", &dead_mask_list_ptr_);
    input_tree->SetBranchAddress("participants_mask", &participants_mask_list_ptr_);
    input_tree->SetBranchAddress("frame", &frame_reference_ptr_);
    input_tree->SetBranchAddress("lightdata", &lightdata_list_in_frame_ptr_);
}

void AlcorSpilldata::write_to_tree(TTree *output_tree)
{
    if (!output_tree)
        return;

    // Branch() takes the address of the std::vector itself; ROOT iterates
    // it on each Fill.  Stable because the wrapper (and therefore
    // spilldata) is non-movable.
    output_tree->Branch("dead_mask", &spilldata.dead_mask_list);
    output_tree->Branch("participants_mask", &spilldata.participants_mask_list);
    output_tree->Branch("frame", &spilldata.frame_reference);
    output_tree->Branch("lightdata", &spilldata.lightdata_list_in_frame);
}

void AlcorSpilldata::prepare_tree_fill()
{
    //  1. Reset flat vectors.  No need to re-sync *_ptr_ — they were
    //     anchored once in the constructor and the wrapper is non-movable,
    //     so the addresses are still correct.
    spilldata.dead_mask_list.clear();
    spilldata.participants_mask_list.clear();
    spilldata.frame_reference.clear();
    spilldata.lightdata_list_in_frame.clear();

    //  2. Transfer masks into flat vectors (pre-reserve for a single allocation).
    spilldata.dead_mask_list.reserve(spilldata.dead_mask.size());
    spilldata.participants_mask_list.reserve(spilldata.participants_mask.size());

    for (const auto &[device, mask] : spilldata.dead_mask)
        spilldata.dead_mask_list.push_back({device, mask});
    for (const auto &[device, mask] : spilldata.participants_mask)
        spilldata.participants_mask_list.push_back({device, mask});

    //  3. Move non-suppressed frames into the flat vectors; skip deleted frames.
    //     `frame_and_lightdata` is an unordered_map (O(1) insert/destroy), so
    //     iteration is hash-bucket order, not ascending.  Downstream consumers
    //     (recodata_writer, analysis macros) expect `frame_reference` ascending
    //     — sort the keys once and pull the entries out in that order to keep
    //     the on-disk invariant identical to the previous std::map output.
    const auto ordered_keys = sorted_frame_ids(spilldata.frame_and_lightdata);
    spilldata.frame_reference.reserve(ordered_keys.size());
    spilldata.lightdata_list_in_frame.reserve(ordered_keys.size());
    for (uint32_t key : ordered_keys)
    {
        if (frame_reference_for_deletion[key])
            continue;
        auto it = spilldata.frame_and_lightdata.find(key);
        spilldata.frame_reference.push_back(key);
        spilldata.lightdata_list_in_frame.push_back(std::move(it->second));
    }

    //  4. Clear consumed working maps.
    spilldata.dead_mask.clear();
    spilldata.participants_mask.clear();
    spilldata.frame_and_lightdata.clear();
}

// ============================================================================
//  Free-function merge utilities
// ============================================================================

void merge_lightdata(AlcorLightdataStruct &lhs, AlcorLightdataStruct &&rhs)
{
    lhs.trigger_hits.insert(lhs.trigger_hits.end(),
                            std::make_move_iterator(rhs.trigger_hits.begin()),
                            std::make_move_iterator(rhs.trigger_hits.end()));
    lhs.timing_hits.insert(lhs.timing_hits.end(),
                           std::make_move_iterator(rhs.timing_hits.begin()),
                           std::make_move_iterator(rhs.timing_hits.end()));
    lhs.tracking_hits.insert(lhs.tracking_hits.end(),
                             std::make_move_iterator(rhs.tracking_hits.begin()),
                             std::make_move_iterator(rhs.tracking_hits.end()));
    lhs.cherenkov_hits.insert(lhs.cherenkov_hits.end(),
                              std::make_move_iterator(rhs.cherenkov_hits.begin()),
                              std::make_move_iterator(rhs.cherenkov_hits.end()));
}

void merge(AlcorSpilldataStruct &lhs, AlcorSpilldataStruct &&rhs)
{
    for (const auto &[key, mask] : rhs.dead_mask)
        lhs.dead_mask[key] |= mask;

    for (const auto &[key, mask] : rhs.participants_mask)
        lhs.participants_mask[key] |= mask;

    for (auto &[key, rhs_data] : rhs.frame_and_lightdata)
    {
        auto [it, inserted] = lhs.frame_and_lightdata.try_emplace(key, std::move(rhs_data));
        if (!inserted)
            merge_lightdata(it->second, std::move(rhs_data));
    }
}

std::vector<uint32_t> sorted_frame_ids(
    const std::unordered_map<uint32_t, AlcorLightdataStruct> &frame_link)
{
    std::vector<uint32_t> keys;
    keys.reserve(frame_link.size());
    for (const auto &kv : frame_link)
        keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    return keys;
}
