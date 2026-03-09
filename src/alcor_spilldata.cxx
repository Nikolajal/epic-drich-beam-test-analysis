#include "alcor_spilldata.h"

// ============================================================================
//  alcor_spilldata_struct
// ============================================================================

void alcor_spilldata_struct::clear()
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

    //  Re-synchronise the branch-address pointers after potential reallocation.
    dead_mask_list_ptr = &dead_mask_list;
    participants_mask_list_ptr = &participants_mask_list;
    frame_reference_ptr = &frame_reference;
    lightdata_list_in_frame_ptr = &lightdata_list_in_frame;
}

// ============================================================================
//  alcor_spilldata — non-trivial utility methods
// ============================================================================

std::map<uint32_t, std::vector<uint8_t>> alcor_spilldata::get_not_dead_participants()
{
    std::map<uint32_t, std::vector<uint8_t>> result;

    //  Prefer the flat-list representation (populated after TTree::GetEntry).
    if (!spilldata.participants_mask_list.empty())
    {
        for (const auto &pm : spilldata.participants_mask_list)
        {
            uint32_t dead_mask = 0;
            for (const auto &dm : spilldata.dead_mask_list)
            {
                if (dm.device == pm.device)
                {
                    dead_mask = dm.mask;
                    break;
                }
            }
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
//  alcor_spilldata — ROOT TTree I/O
// ============================================================================

void alcor_spilldata::link_to_tree(TTree *input_tree)
{
    if (!input_tree)
        return;

    input_tree->SetBranchAddress("dead_mask", &spilldata.dead_mask_list_ptr);
    input_tree->SetBranchAddress("participants_mask", &spilldata.participants_mask_list_ptr);
    input_tree->SetBranchAddress("frame", &spilldata.frame_reference_ptr);
    input_tree->SetBranchAddress("lightdata", &spilldata.lightdata_list_in_frame_ptr);
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
    //  1. Reset flat vectors and re-synchronise branch-address pointers.
    spilldata.dead_mask_list.clear();
    spilldata.participants_mask_list.clear();
    spilldata.frame_reference.clear();
    spilldata.lightdata_list_in_frame.clear();

    spilldata.dead_mask_list_ptr = &spilldata.dead_mask_list;
    spilldata.participants_mask_list_ptr = &spilldata.participants_mask_list;
    spilldata.frame_reference_ptr = &spilldata.frame_reference;
    spilldata.lightdata_list_in_frame_ptr = &spilldata.lightdata_list_in_frame;

    //  2. Transfer masks into flat vectors (pre-reserve for a single allocation).
    spilldata.dead_mask_list.reserve(spilldata.dead_mask.size());
    spilldata.participants_mask_list.reserve(spilldata.participants_mask.size());

    for (const auto &[device, mask] : spilldata.dead_mask)
        spilldata.dead_mask_list.push_back({device, mask});
    for (const auto &[device, mask] : spilldata.participants_mask)
        spilldata.participants_mask_list.push_back({device, mask});

    //  3. Move non-suppressed frames into the flat vectors; skip deleted frames.
    for (auto it = spilldata.frame_and_lightdata.begin();
         it != spilldata.frame_and_lightdata.end();)
    {
        if (!frame_reference_for_deletion[it->first])
        {
            spilldata.frame_reference.push_back(it->first);
            spilldata.lightdata_list_in_frame.push_back(std::move(it->second));
            it = spilldata.frame_and_lightdata.erase(it);
        }
        else
        {
            ++it;
        }
    }

    //  4. Clear consumed working maps.
    spilldata.dead_mask.clear();
    spilldata.participants_mask.clear();
    spilldata.frame_and_lightdata.clear();
}

// ============================================================================
//  Free-function merge / reduce utilities
// ============================================================================

void merge_lightdata(alcor_lightdata_struct &lhs, alcor_lightdata_struct &&rhs)
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

void merge(alcor_spilldata_struct &lhs, alcor_spilldata_struct &&rhs)
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

void merge(alcor_spilldata &lhs, alcor_spilldata &&rhs)
{
    merge(lhs.get_spilldata_link(), std::move(rhs.get_spilldata()));
}

alcor_spilldata_struct operator+(alcor_spilldata_struct lhs, alcor_spilldata_struct rhs)
{
    std::cerr << "[WARNING] (alcor_spilldata_struct operator+) "
                 "This function is under revision — results are unreliable!\n";

    lhs.dead_mask.merge(std::move(rhs.dead_mask));
    lhs.participants_mask.merge(std::move(rhs.participants_mask));
    lhs.frame_and_lightdata.merge(std::move(rhs.frame_and_lightdata));
    return lhs;
}

alcor_spilldata operator+(alcor_spilldata lhs, const alcor_spilldata &rhs)
{
    std::cerr << "[WARNING] (alcor_spilldata operator+) "
                 "This function is under revision — results are unreliable!\n";

    lhs.get_spilldata_link() = std::move(lhs.get_spilldata_link()) + rhs.get_spilldata();
    return lhs;
}