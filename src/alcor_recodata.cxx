#include "alcor_recodata.h"

// =============================================================================
// Reference setters
// =============================================================================

void alcor_recodata::set_recodata_link(std::vector<alcor_finedata_struct> &v)
{
    recodata = v;
    recodata_ptr = &recodata;
}

void alcor_recodata::set_triggers_link(std::vector<trigger_event> &v)
{
    triggers = v;
    triggers_ptr = &triggers;
}

// =============================================================================
// Trigger search
// =============================================================================

std::optional<trigger_event> alcor_recodata::get_trigger_by_index(uint8_t index) const
{
    auto current_trigger = get_triggers();
    auto it = std::find_if(current_trigger.begin(), current_trigger.end(),
                           [index](const trigger_event &t)
                           { return t.index == index; });
    if (it != current_trigger.end())
        return *it;
    return std::nullopt;
}

// =============================================================================
// I/O utilities
// =============================================================================

void alcor_recodata::clear()
{
    recodata.clear();
    triggers.clear();
    recodata.shrink_to_fit();
    triggers.shrink_to_fit();
}

bool alcor_recodata::link_to_tree(TTree *input_tree)
{
    if (!input_tree)
    {
        mist::logger::error("(alcor_recodata::link_to_tree) input_tree is null");
        return false;
    }
    if (input_tree->GetEntries() == 0)
    {
        mist::logger::error("(alcor_recodata::link_to_tree) input_tree is empty");
        return false;
    }
    if (!input_tree->GetBranch("recodata") || !input_tree->GetBranch("triggers"))
    {
        mist::logger::error("(alcor_recodata::link_to_tree) missing expected branches");
        input_tree->Print();
        return false;
    }
    input_tree->SetBranchAddress("recodata", &recodata_ptr);
    input_tree->SetBranchAddress("triggers", &triggers_ptr);
    return true;
}

void alcor_recodata::write_to_tree(TTree *output_tree)
{
    if (!output_tree)
        return;
    output_tree->Branch("recodata", &recodata);
    output_tree->Branch("triggers", &triggers);
}

// =============================================================================
// Analysis utilities
// =============================================================================

void alcor_recodata::find_rings(float distance_length_cut, float distance_time_cut)
{
    auto i_index = -1;
    std::map<int, std::map<int, std::vector<int>>> proximity_hit_list;
    for (auto current_hit : recodata)
    {
        i_index++;
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
        int selected_main_index = -1;
        int selected_size = -1;
        for (auto &[main_index, current_index_list] : proximity_hit_list_device)
        {
            if ((int)current_index_list.size() < selected_size)
                continue;
            selected_main_index = main_index;
            selected_size = current_index_list.size();
        }
        add_hit_mask(selected_main_index, encode_bit(_HITMASK_ring_tag_first));
        for (auto current_index : proximity_hit_list[current_device][selected_main_index])
            add_hit_mask(current_index, encode_bit(_HITMASK_ring_tag_first));
    }
}

void alcor_recodata::build_hough_lut(const std::map<int, std::array<float, 2>> &index_to_hit_xy,
                                     float r_min, float r_max, float r_step, float cell_size)
{
    hough_cell_size = cell_size;

    hough_x_min = hough_y_min = std::numeric_limits<float>::max();
    hough_x_max = hough_y_max = std::numeric_limits<float>::lowest();
    for (auto &[idx, pos] : index_to_hit_xy)
    {
        hough_x_min = std::min(hough_x_min, pos[0]);
        hough_x_max = std::max(hough_x_max, pos[0]);
        hough_y_min = std::min(hough_y_min, pos[1]);
        hough_y_max = std::max(hough_y_max, pos[1]);
    }

    hough_x_min -= r_max;
    hough_x_max += r_max;
    hough_y_min -= r_max;
    hough_y_max += r_max;
    hough_nx = static_cast<int>((hough_x_max - hough_x_min) / cell_size) + 1;
    hough_ny = static_cast<int>((hough_y_max - hough_y_min) / cell_size) + 1;

    hough_r_bins.clear();
    for (float r = r_min; r <= r_max; r += r_step)
        hough_r_bins.push_back(r);

    hough_lut.clear();
    for (auto &[global_index, pos] : index_to_hit_xy)
    {
        auto &lut_entry = hough_lut[global_index];
        lut_entry.resize(hough_r_bins.size());

        for (int iR = 0; iR < (int)hough_r_bins.size(); ++iR)
        {
            float R = hough_r_bins[iR];
            float half_cell = cell_size * 0.5f;

            for (int ix = 0; ix < hough_nx; ++ix)
            {
                float cx = hough_x_min + ix * cell_size;
                float dx = cx - pos[0];
                if (std::fabs(dx) > R + half_cell)
                    continue;

                float dy2 = R * R - dx * dx;
                if (dy2 < 0)
                    continue;
                float dy = std::sqrt(dy2);

                for (int sign : {-1, 1})
                {
                    float cy = pos[1] + sign * dy;
                    int iy = static_cast<int>((cy - hough_y_min) / cell_size + 0.5f);
                    if (iy < 0 || iy >= hough_ny)
                        continue;

                    float actual_dist = std::hypot(cx - pos[0], hough_y_min + iy * cell_size - pos[1]);
                    if (std::fabs(actual_dist - R) < half_cell)
                        lut_entry[iR].push_back(iy * hough_nx + ix);
                }
            }
        }
    }
    hough_accum.assign(hough_r_bins.size() * hough_nx * hough_ny, 0);
    mist::logger::info(Form("Hough LUT built: %zu channels, %zu R bins, grid %dx%d",
                            hough_lut.size(), hough_r_bins.size(), hough_nx, hough_ny));
}

void alcor_recodata::find_rings_hough(float threshold_fraction, int min_hits)
{
    if (hough_lut.empty() || hough_r_bins.empty())
    {
        mist::logger::error("(alcor_recodata::find_rings_hough) LUT is empty — call build_hough_lut first.");
        return;
    }

    std::fill(hough_accum.begin(), hough_accum.end(), 0);

    std::vector<int> active_hits;
    for (int i = 0; i < (int)recodata.size(); ++i)
    {
        if (get_device(i) >= 200)
            continue;
        if (is_afterpulse(i))
            continue;
        if (hough_lut.find(recodata[i].global_index) == hough_lut.end())
            continue;
        active_hits.push_back(i);
    }

    if ((int)active_hits.size() < min_hits)
        return;

    int global_max_cell = -1, global_max_iR = -1, global_max_count = 0;
    const int n_cells = hough_nx * hough_ny;
    const int n_r = hough_r_bins.size();

    for (int i : active_hits)
    {
        auto &lut_entry = hough_lut.at(recodata[i].global_index);
        for (int iR = 0; iR < n_r; ++iR)
            for (int cell : lut_entry[iR])
            {
                int val = ++hough_accum[iR * n_cells + cell];
                if (val > global_max_count)
                {
                    global_max_count = val;
                    global_max_iR = iR;
                    global_max_cell = cell;
                }
            }
    }

    int threshold = std::max(min_hits, static_cast<int>(std::ceil(threshold_fraction * active_hits.size())));
    int best_iR = global_max_iR;
    int best_cell = global_max_cell;
    int best_count = global_max_count;
    int n_rings_found = 0;

    while (best_count >= threshold)
    {
        int best_ix = best_cell % hough_nx;
        int best_iy = best_cell / hough_nx;
        float cx = hough_x_min + best_ix * hough_cell_size;
        float cy = hough_y_min + best_iy * hough_cell_size;
        float R = hough_r_bins[best_iR];

        uint32_t ring_mask = (n_rings_found == 0)
                                 ? _HITMASK_hough_ring_tag_first
                                 : _HITMASK_hough_ring_tag_second;

        float trigger_time_sum = 0.f;
        int trigger_time_count = 0;
        for (int i : active_hits)
        {
            float dist = std::hypot(recodata[i].hit_x - cx, recodata[i].hit_y - cy);
            if (std::fabs(dist - R) < hough_cell_size)
            {
                add_hit_mask_bit(i, ring_mask);
                trigger_time_sum += get_hit_t(i);
                trigger_time_count++;
            }
        }
        float trigger_time = trigger_time_count > 0 ? trigger_time_sum / trigger_time_count : 0.f;
        n_rings_found++;

        add_trigger({_TRIGGER_HOUGH_RING_FOUND_,
                     static_cast<uint16_t>(n_rings_found),
                     static_cast<float>(trigger_time)});

        // Suppress the found peak region
        int r_cells = static_cast<int>(R / hough_cell_size) + 1;
        int ix_min = std::max(0, best_ix - r_cells);
        int ix_max = std::min(hough_nx - 1, best_ix + r_cells);
        int iy_min = std::max(0, best_iy - r_cells);
        int iy_max = std::min(hough_ny - 1, best_iy + r_cells);
        float R2 = R * R;
        for (int iR = 0; iR < n_r; ++iR)
            for (int iy = iy_min; iy <= iy_max; ++iy)
                for (int ix = ix_min; ix <= ix_max; ++ix)
                {
                    float dx = (ix - best_ix) * hough_cell_size;
                    float dy = (iy - best_iy) * hough_cell_size;
                    if (dx * dx + dy * dy < R2)
                        hough_accum[iR * n_cells + iy * hough_nx + ix] = 0;
                }

        // Re-scan for next peak
        best_count = 0;
        best_iR = -1;
        best_cell = -1;
        for (int iR = 0; iR < n_r; ++iR)
            for (int cell = 0; cell < n_cells; ++cell)
                if (hough_accum[iR * n_cells + cell] > best_count)
                {
                    best_count = hough_accum[iR * n_cells + cell];
                    best_iR = iR;
                    best_cell = cell;
                }

        if (n_rings_found >= 2)
            break;
    }
}