#include <mist/ring_finding/hough_transform.h>

// ============================================================
//  Constructors
// ============================================================

hough_transform::hough_transform(const std::map<int, std::array<float, 2>> &index_to_hit_xy,
                                 float r_min, float r_max, float r_step, float cell_size)
{
    build_lut(index_to_hit_xy, r_min, r_max, r_step, cell_size);
}

// ============================================================
//  LUT construction  (unchanged from original)
// ============================================================

void hough_transform::build_lut(const std::map<int, std::array<float, 2>> &index_to_hit_xy,
                                float r_min, float r_max, float r_step, float cell_size)
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

    // Pad by r_max so ring centres outside the hit area are reachable
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

    // Build LUT: for each key, for each R bin, which accumulator cells does it vote for?
    // Two-pass arc rasterisation partitions the circle by slope to avoid double-counting:
    //   pass 1 handles the shallow half (|dy| <= |dx|), pass 2 the steep half (|dx| < |dy|).
    hough_lut.clear();
    for (auto &[key, pos] : index_to_hit_xy)
    {
        auto &lut_entry = hough_lut[key];
        lut_entry.resize(hough_r_bins.size());

        for (int iR = 0; iR < static_cast<int>(hough_r_bins.size()); ++iR)
        {
            float R = hough_r_bins[iR];
            int n_angles = 360;
            for (int ia = 0; ia < n_angles; ++ia)
            {
                float angle = 2.f * M_PI * ia / n_angles;
                int ix = static_cast<int>((pos[0] + R * std::cos(angle) - hough_x_min) / cell_size + 0.5f);
                int iy = static_cast<int>((pos[1] + R * std::sin(angle) - hough_y_min) / cell_size + 0.5f);
                if (ix < 0 || ix >= hough_nx || iy < 0 || iy >= hough_ny)
                    continue;
                lut_entry[iR].push_back(iy * hough_nx + ix);
            }

            // Deduplicate cells within each R bin
            std::sort(lut_entry[iR].begin(), lut_entry[iR].end());
            lut_entry[iR].erase(
                std::unique(lut_entry[iR].begin(), lut_entry[iR].end()),
                lut_entry[iR].end());
        }
    }

    hough_accum.assign(hough_r_bins.size() * hough_nx * hough_ny, 0);

    mist::logger::info(Form("(hough-transform::build_lut) Hough LUT built: %zu keys, %zu R bins, grid %dx%d",
                          hough_lut.size(), hough_r_bins.size(), hough_nx, hough_ny));
}

// ============================================================
//  Private helpers
// ============================================================

int hough_transform::vote_and_find_peak(const std::vector<hough_hit> &hits,
                                        const std::vector<int> &active_indices,
                                        int &best_iR, int &best_cell)
{
    // Reset accumulator for this pass
    std::fill(hough_accum.begin(), hough_accum.end(), 0);

    const int n_cells = hough_nx * hough_ny;
    const int n_r = static_cast<int>(hough_r_bins.size());

    best_iR = -1;
    best_cell = -1;
    int best_count = 0;

    for (int i : active_indices)
    {
        auto it = hough_lut.find(hits[i].lut_key);
        if (it == hough_lut.end())
            continue;

        const auto &lut_entry = it->second;
        for (int iR = 0; iR < n_r; ++iR)
            for (int cell : lut_entry[iR])
            {
                int val = ++hough_accum[iR * n_cells + cell];
                if (val > best_count)
                {
                    best_count = val;
                    best_iR = iR;
                    best_cell = cell;
                }
            }
    }

    return best_count;
}

hough_ring_result hough_transform::collect_ring_hits(const std::vector<hough_hit> &hits,
                                                     const std::vector<int> &active_indices,
                                                     float cx, float cy, float R,
                                                     float collection_radius) const
{
    hough_ring_result result;
    result.cx = cx;
    result.cy = cy;
    result.radius = R;
    result.peak_votes = 0; // filled by caller
    result.mean_time = 0.f;

    float time_sum = 0.f;

    for (int i : active_indices)
    {
        float dist = std::hypot(hits[i].x - cx, hits[i].y - cy);
        if (std::fabs(dist - R) < collection_radius)
        {
            result.hit_indices.push_back(i);
            time_sum += hits[i].time;
        }
    }

    if (!result.hit_indices.empty())
        result.mean_time = time_sum / static_cast<float>(result.hit_indices.size());

    return result;
}

// ============================================================
//  Per-event ring finding
// ============================================================

std::vector<hough_ring_result> hough_transform::find_rings(const std::vector<hough_hit> &hits,
                                                           float threshold_fraction,
                                                           int min_hits,
                                                           int min_active,
                                                           int max_rings,
                                                           float collection_radius)
{
    std::vector<hough_ring_result> found_rings;

    if (!is_lut_ready())
    {
        mist::logger::error("(hough_transform::find_rings) LUT is empty — call build_lut() first.");
        return found_rings;
    }

    // Build the initial active set: only hits whose LUT key is known
    std::vector<int> active_indices;
    active_indices.reserve(hits.size());
    for (int i = 0; i < static_cast<int>(hits.size()); ++i)
        if (hough_lut.count(hits[i].lut_key))
            active_indices.push_back(i);

    // Iterative hit-removal loop.
    // Each pass:
    //   1. Vote with the current active set and locate the peak.
    //   2. Check the threshold against the *current* active count so that a
    //      small second ring is not penalised by the size of the full event.
    //   3. Collect the ring hits and record their indices.
    //   4. Remove those indices from the active set before the next pass —
    //      the accumulator is reset at the top of vote_and_find_peak, so no
    //      spatial suppression is needed.

    const int threshold = std::max(min_hits, static_cast<int>(std::ceil(threshold_fraction * active_indices.size())));

    while (static_cast<int>(found_rings.size()) < max_rings &&
           static_cast<int>(active_indices.size()) >= min_hits)
    {
        int best_iR, best_cell;
        int best_count = vote_and_find_peak(hits, active_indices, best_iR, best_cell);

        if (best_count < threshold)
            break;

        int best_ix = best_cell % hough_nx;
        int best_iy = best_cell / hough_nx;
        float cx = hough_x_min + best_ix * hough_cell_size;
        float cy = hough_y_min + best_iy * hough_cell_size;
        float R = hough_r_bins[best_iR];

        hough_ring_result ring = collect_ring_hits(hits, active_indices, cx, cy, R, collection_radius);
        ring.peak_votes = best_count;

        if (static_cast<int>(ring.hit_indices.size()) < min_hits)
            break;

        found_rings.push_back(ring);

        // Remove ring hits from the active set for the next pass.
        // Build a fast lookup set from the ring's hit indices, then filter.
        std::unordered_set<int> ring_hit_set(ring.hit_indices.begin(), ring.hit_indices.end());
        active_indices.erase(
            std::remove_if(active_indices.begin(), active_indices.end(),
                           [&ring_hit_set](int idx)
                           { return ring_hit_set.count(idx); }),
            active_indices.end());

        // Don't attempt another ring if the remainder is below the original threshold
        if (static_cast<int>(active_indices.size()) < threshold)
            break;
    }

    return found_rings;
}