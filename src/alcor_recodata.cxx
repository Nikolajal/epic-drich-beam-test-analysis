#include "alcor_recodata.h"

//  Constructors
//  No implementations needed
//  TODO: recodata trigger fills every event

//  Getters
//  --- Pure getters
std::vector<alcor_recodata_struct> alcor_recodata::get_recodata() const { return recodata; }
std::vector<alcor_recodata_struct> *alcor_recodata::get_recodata_ptr() { return recodata_ptr; }
alcor_recodata_struct alcor_recodata::get_recodata(int i) const { return recodata[i]; }
std::vector<trigger_event> alcor_recodata::get_triggers() const { return triggers; }
std::vector<trigger_event> *alcor_recodata::get_triggers_ptr() { return triggers_ptr; }
int alcor_recodata::get_global_index(int i) const { return recodata[i].global_index; }
float alcor_recodata::get_hit_x(int i) const { return recodata[i].hit_x; }
float alcor_recodata::get_hit_y(int i) const { return recodata[i].hit_y; }
uint32_t alcor_recodata::get_hit_mask(int i) const { return recodata[i].hit_mask; }
float alcor_recodata::get_hit_t(int i) const { return recodata[i].hit_t; }

//  --- Reference getters
std::vector<alcor_recodata_struct> &alcor_recodata::get_recodata_link() { return recodata; }
alcor_recodata_struct &alcor_recodata::get_recodata_link(int i) { return recodata[i]; }
std::vector<trigger_event> &alcor_recodata::get_triggers_link() { return triggers; }

//  --- Derived Getters
//  --- --- Polar coordinates
float alcor_recodata::get_hit_r(int i) const { return get_hit_r(i, {0.f, 0.f}); }
float alcor_recodata::get_hit_r(int i, std::array<float, 2> v) const { return std::hypot(get_hit_x(i) - v[0], get_hit_y(i) - v[1]); }
float alcor_recodata::get_hit_phi(int i) const { return get_hit_phi(i, {0.f, 0.f}); }
float alcor_recodata::get_hit_phi(int i, std::array<float, 2> v) const { return std::atan2(get_hit_y(i) - v[1], get_hit_x(i) - v[0]); }
//  --- --- Randomised coordinates
float alcor_recodata::get_hit_x_rnd(int i) const { return recodata[i].hit_x + (_rnd_(_global_gen_) * 3.0 - 1.5); }
float alcor_recodata::get_hit_y_rnd(int i) const { return recodata[i].hit_y + (_rnd_(_global_gen_) * 3.0 - 1.5); }
float alcor_recodata::get_hit_r_rnd(int i) const { return get_hit_r_rnd(i, {0.f, 0.f}); }
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
std::optional<trigger_event> alcor_recodata::get_trigger_by_index(uint8_t index) const
{
    auto current_trigger = get_triggers();
    auto it = std::find_if(current_trigger.begin(), current_trigger.end(), [index](const trigger_event &t)
                           { return t.index == index; });
    if (it != current_trigger.end())
        return *it;
    else
        return std::nullopt;
}
std::optional<trigger_event> alcor_recodata::get_timing_trigger() const { return get_trigger_by_index(_TRIGGER_TIMING_); }

//  Setters
//  --- Pure setters
void alcor_recodata::set_recodata(std::vector<alcor_recodata_struct> v) { recodata = v; }
void alcor_recodata::set_recodata(int i, alcor_recodata_struct v) { recodata[i] = v; }
void alcor_recodata::set_triggers(const std::vector<trigger_event> v) { triggers = v; }
void alcor_recodata::set_recodata_ptr(std::vector<alcor_recodata_struct> *v) { recodata_ptr = v; }
void alcor_recodata::set_triggers_ptr(std::vector<trigger_event> *v) { triggers_ptr = v; }
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
void alcor_recodata::set_triggers_link(std::vector<trigger_event> &v)
{
    triggers = v;
    triggers_ptr = &triggers;
}

//  Add utilities
void alcor_recodata::add_hit_mask(int i, uint32_t v) { recodata[i].hit_mask |= v; }
void alcor_recodata::add_hit_mask_bit(int i, uint32_t v) { recodata[i].hit_mask |= encode_bit(v); }
void alcor_recodata::add_trigger(uint8_t index, uint16_t coarse, float fine_time) { triggers.emplace_back(index, coarse, fine_time); }
void alcor_recodata::add_trigger(trigger_event hit) { triggers.push_back(hit); }
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
bool alcor_recodata::is_cross_talk(int i) { return check_hit_mask(i, encode_bit(_HITMASK_cross_talk)); }
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
            if (current_index_list.size() < selected_size)
                continue;
            selected_main_index = main_index;
            selected_size = current_index_list.size();
        }
        add_hit_mask(selected_main_index, encode_bit(_HITMASK_ring_tag_first));
        for (auto current_index : proximity_hit_list[current_device][selected_main_index])
            add_hit_mask(current_index, encode_bit(_HITMASK_ring_tag_first));
    }
}

void alcor_recodata::build_hough_lut(const std::map<int, std::array<float, 2>> &index_to_hit_xy, float r_min, float r_max, float r_step, float cell_size)
{
    hough_cell_size = cell_size;

    // Derive accumulator bounds from hit positions
    hough_x_min = hough_y_min = std::numeric_limits<float>::max();
    hough_x_max = hough_y_max = std::numeric_limits<float>::lowest();
    for (auto &[idx, pos] : index_to_hit_xy)
    {
        hough_x_min = std::min(hough_x_min, pos[0]);
        hough_x_max = std::max(hough_x_max, pos[0]);
        hough_y_min = std::min(hough_y_min, pos[1]);
        hough_y_max = std::max(hough_y_max, pos[1]);
    }

    // Pad by r_max so ring centers outside the hit area are reachable
    hough_x_min -= r_max;
    hough_x_max += r_max;
    hough_y_min -= r_max;
    hough_y_max += r_max;
    hough_nx = static_cast<int>((hough_x_max - hough_x_min) / cell_size) + 1;
    hough_ny = static_cast<int>((hough_y_max - hough_y_min) / cell_size) + 1;

    // Build R bins
    hough_r_bins.clear();
    for (float r = r_min; r <= r_max; r += r_step)
        hough_r_bins.push_back(r);

    // Build LUT: for each channel, for each R bin, which accumulator cells does it vote for?
    hough_lut.clear();
    for (auto &[global_index, pos] : index_to_hit_xy)
    {
        auto &lut_entry = hough_lut[global_index];
        lut_entry.resize(hough_r_bins.size());

        //  Loop over the radius bins
        for (int iR = 0; iR < hough_r_bins.size(); ++iR)
        {
            float R = hough_r_bins[iR];
            float half_cell = cell_size * 0.5f;

            // Iterate over x cells only, compute y analytically
            // Center of the arc is at pos, radius R
            // For each ix, the center x coordinate is cx = x_min + ix * cell_size
            // The y coordinates on the circle are: cy = pos[1] ± sqrt(R² - (cx - pos[0])²)
            for (int ix = 0; ix < hough_nx; ++ix)
            {
                float cx = hough_x_min + ix * cell_size;
                float dx = cx - pos[0];
                if (std::fabs(dx) > R + half_cell)
                    continue; // outside circle entirely

                float dy2 = R * R - dx * dx;
                if (dy2 < 0)
                    continue;
                float dy = std::sqrt(dy2);

                // Two arc points: pos[1] + dy and pos[1] - dy
                for (int sign : {-1, 1})
                {
                    float cy = pos[1] + sign * dy;
                    int iy = static_cast<int>((cy - hough_y_min) / cell_size + 0.5f);
                    if (iy < 0 || iy >= hough_ny)
                        continue;

                    // Verify distance is within half_cell tolerance
                    float actual_dist = std::hypot(cx - pos[0], hough_y_min + iy * cell_size - pos[1]);
                    if (std::fabs(actual_dist - R) < half_cell)
                        lut_entry[iR].push_back(iy * hough_nx + ix);
                }
            }
        }
    }
    hough_accum.assign(hough_r_bins.size() * hough_nx * hough_ny, 0);
    mist::logger::info(Form("Hough LUT built: %zu channels, %zu R bins, grid %dx%d", hough_lut.size(), hough_r_bins.size(), hough_nx, hough_ny));
}

void alcor_recodata::find_rings_hough(float threshold_fraction, int min_hits)
{
    if (hough_lut.empty() || hough_r_bins.empty())
    {
        mist::logger::error("(alcor_recodata::find_rings_houg) hough_lut or hough_r_bins are empty, did you not run alcor_recodata::build_hough_lut before looking for rings with hough transform?");
        return;
    }

    // Flat 3D accumulator: [iR * nx * ny + iy * nx + ix]
    std::fill(hough_accum.begin(), hough_accum.end(), 0);

    // Collect non-afterpulse, non-crosstalk hits with valid LUT entries
    // Respect device < 200 boundary
    std::vector<int> active_hits;
    for (int i = 0; i < recodata.size(); ++i)
    {
        if (get_device(i) >= 200)
            continue;
        if (is_afterpulse(i))
            continue;
        // if (is_cross_talk(i))
        //     continue;
        auto it = hough_lut.find(recodata[i].global_index);
        if (it == hough_lut.end())
            continue;
        active_hits.push_back(i);
    }

    if (active_hits.size() < min_hits)
        return;

    // Vote
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

    // Iterative peak finding with suppression
    int threshold = std::max(min_hits, static_cast<int>(std::ceil(threshold_fraction * active_hits.size())));

    // Use the max tracked during voting for the first iteration
    int best_iR = global_max_iR;
    int best_cell = global_max_cell;
    int best_count = global_max_count;
    int n_rings_found = 0;

    while (best_count >= threshold)
    {
        // Extract peak center
        int best_ix = best_cell % hough_nx;
        int best_iy = best_cell / hough_nx;
        float cx = hough_x_min + best_ix * hough_cell_size;
        float cy = hough_y_min + best_iy * hough_cell_size;
        float R = hough_r_bins[best_iR];

        // Tag hits on this ring arc and compute trigger time
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
                trigger_time_sum += recodata[i].hit_t;
                trigger_time_count++;
            }
        }
        float trigger_time = trigger_time_count > 0 ? trigger_time_sum / trigger_time_count : 0.f;
        n_rings_found++;

        // Add trigger
        add_trigger({_TRIGGER_HOUGH_RING_FOUND_,
                     static_cast<uint16_t>(n_rings_found),
                     static_cast<float>(trigger_time)});

        // Suppress: bounded box only, no sqrt
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

        // Re-scan for next peak (only reached if multiple rings expected)
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