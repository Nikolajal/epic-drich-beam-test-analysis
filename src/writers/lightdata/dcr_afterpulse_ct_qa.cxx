/**
 * @file dcr_afterpulse_ct_qa.cxx
 * @brief Implementation of `fill_dcr_afterpulse_ct_qa` — lifted from
 *        the per-frame inner loop of `lightdata_writer()`.
 *
 * Algorithm unchanged from the in-function version; only captures are
 * replaced by explicit parameters.  Bit-identical output was verified
 * vs a baseline snapshot at extraction time (since pruned).
 */

#include "writers/lightdata/dcr_afterpulse_ct_qa.h"
#include "alcor_data.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric> // std::iota

#include "TH1.h"
#include "TH2.h"
#include "TProfile.h"

#include "alcor_finedata.h"
#include "utility/global_index.h" // GlobalIndex

namespace btana::lightdata
{

void fill_dcr_afterpulse_ct_qa(
    const std::vector<AlcorFinedataStruct> &cherenkov_hits,
    const std::set<uint32_t> &active_sensors,
    std::unordered_map<uint32_t, uint16_t> &active_sensors_count,
    std::vector<CtHit> &ct_hits,
    std::vector<std::size_t> &sorted_by_time,
    const QaConfigStruct &qa_cfg,
    const DcrAfterpulseCtHists &h)
{
    //  Channel-by-channel Hit counting — start each frame from a clean
    //  map so counts cannot accumulate across frames.  The hit-side key
    //  must match the active_sensors key type — channel_ordinal (dense
    //  small int), NOT global_channel_raw (sparse packed millions,
    //  which would silently mismatch active_sensors entries AND
    //  overflow the per-channel TProfile axes).
    active_sensors_count.clear();
    for (const auto &channel_key : active_sensors)
        active_sensors_count[channel_key] = 0;
    for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
    {
        const uint32_t channel_key = static_cast<uint32_t>(::GlobalIndex(
                                                               current_cherenkov_hit_struct.GlobalIndex)
                                                               .channel_ordinal());
        active_sensors_count[channel_key]++;
        //  Smeared DCR hitmap: one fill per Hit at the channel's
        //  smeared physical position.
        AlcorFinedata dcr_hit_fd(current_cherenkov_hit_struct);
        const float dx = dcr_hit_fd.get_hit_x_rnd();
        if (h.h_dcr_hitmap && dx > -990.f)
            h.h_dcr_hitmap->Fill(dx, dcr_hit_fd.get_hit_y_rnd());
    }
    //  Fill the DCR per-channel TProfile.
    if (h.h_dcr_per_channel)
        for (auto &[GlobalIndex, count] : active_sensors_count)
            h.h_dcr_per_channel->Fill(GlobalIndex, count);

    //  --- Afterpulse & cross-talk QA
    //  Pre-decode per-Hit fields for the pairwise comparisons below.
    //  ct_hits + sorted_by_time are caller-owned (hoisted out of the
    //  per-frame loop); clear() preserves capacity.
    ct_hits.clear();
    ct_hits.reserve(cherenkov_hits.size());
    for (const auto &s : cherenkov_hits)
    {
        const ::GlobalIndex gi(s.GlobalIndex);
        ct_hits.push_back({static_cast<uint64_t>(s.rollover) * 32768u + s.coarse,
                           static_cast<uint32_t>(gi.channel_ordinal()),
                           gi.device(),
                           gi.fifo(),
                           s.hit_x, s.hit_y});
    }

    //  Build a time-sorted index so the CT inner loop can use binary
    //  search to restrict candidates to [dt_min, dt_max).  O(N log N
    //  + N · k_win) instead of O(N²).
    sorted_by_time.resize(ct_hits.size());
    std::iota(sorted_by_time.begin(), sorted_by_time.end(), 0);
    std::sort(sorted_by_time.begin(), sorted_by_time.end(),
              [&ct_hits](std::size_t a, std::size_t b)
              { return ct_hits[a].global_t < ct_hits[b].global_t; });

    for (std::size_t i = 0; i < cherenkov_hits.size(); ++i)
    {
        const auto &s = cherenkov_hits[i];
        const auto &hit = ct_hits[i];

        const bool is_ap = (s.HitMask >> HitmaskAfterpulse) & 1u;
        const bool is_ap_near = (s.HitMask >> HitmaskAfterpulseNear) & 1u;
        const bool is_ap_far = (s.HitMask >> HitmaskAfterpulseFar) & 1u;

        AlcorFinedata hit_fd(s);

        //  Afterpulse QA — sideband subtraction.
        //  Per-channel TProfiles: mean = P(same-channel hit in window).
        //  Subtracted TProfile uses signed weight so mean = 100·(P_near − P_far)
        //  = afterpulse probability in %.
        if (h.h_afterpulse_near_per_channel)
            h.h_afterpulse_near_per_channel->Fill(hit.channel, is_ap_near ? 100.0 : 0.0);
        if (h.h_afterpulse_far_per_channel)
            h.h_afterpulse_far_per_channel->Fill(hit.channel, is_ap_far ? 100.0 : 0.0);
        if (h.h_afterpulse_per_channel)
            h.h_afterpulse_per_channel->Fill(hit.channel,
                                             100.0 * (static_cast<int>(is_ap_near) - static_cast<int>(is_ap_far)));
        //  Smeared 2D maps — weighted Fills (single fill per hit, weight = ±100).
        if (hit.x > -990.f)
        {
            if (is_ap_near && h.h_afterpulse_near_hitmap)
                h.h_afterpulse_near_hitmap->Fill(
                    hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd(), 100.0);
            if (is_ap_far && h.h_afterpulse_far_hitmap)
                h.h_afterpulse_far_hitmap->Fill(
                    hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd(), 100.0);
            if (is_ap_near && h.h_afterpulse_hitmap)
                h.h_afterpulse_hitmap->Fill(
                    hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd(), +100.0);
            if (is_ap_far && h.h_afterpulse_hitmap)
                h.h_afterpulse_hitmap->Fill(
                    hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd(), -100.0);
        }

        //  Cross-talk: skip afterpulse hits as DUI.
        if (is_ap)
            continue;

        int n_phys_ct = 0, n_elec_ct = 0;
        //  Binary-search pre-filter: only iterate hits whose global_t
        //  falls in [hit.global_t + dt_min, hit.global_t + dt_max).
        const int64_t t_lo = static_cast<int64_t>(hit.global_t) + qa_cfg.ct_scan_dt_min;
        const int64_t t_hi = static_cast<int64_t>(hit.global_t) + qa_cfg.ct_scan_dt_max;
        const auto jt_lo = std::lower_bound(
            sorted_by_time.begin(), sorted_by_time.end(), t_lo,
            [&ct_hits](std::size_t idx, int64_t t)
            { return static_cast<int64_t>(ct_hits[idx].global_t) < t; });
        const auto jt_hi = std::lower_bound(
            jt_lo, sorted_by_time.end(), t_hi,
            [&ct_hits](std::size_t idx, int64_t t)
            { return static_cast<int64_t>(ct_hits[idx].global_t) < t; });
        for (auto jt = jt_lo; jt != jt_hi; ++jt)
        {
            const std::size_t j = *jt;
            if (j == i || ct_hits[j].channel == hit.channel)
                continue;
            const int64_t dt = static_cast<int64_t>(ct_hits[j].global_t) -
                               static_cast<int64_t>(hit.global_t);
            const bool is_elec = ct_hits[j].device == hit.device &&
                                 ct_hits[j].fifo == hit.fifo;
            //  Physical CT requires strictly positive Δt (causal optical/charge coupling)
            const bool is_phys = dt >= 0 &&
                                 hit.x > -990.f && ct_hits[j].x > -990.f &&
                                 std::hypot(ct_hits[j].x - hit.x, ct_hits[j].y - hit.y) <= qa_cfg.ct_phys_radius_mm;
            //  Fill Δt for all neighbour types — used for DCR sideband estimation
            if (is_phys && h.h_phys_ct_dt)
                h.h_phys_ct_dt->Fill(static_cast<double>(dt));
            if (is_elec && h.h_elec_ct_dt)
                h.h_elec_ct_dt->Fill(static_cast<double>(dt));
            //  2D diagnostic: (Δchannel, Δt) filtered per neighbour type
            const double dchannel = static_cast<double>(ct_hits[j].channel) -
                                    static_cast<double>(hit.channel);
            if (is_elec && h.h_elec_ct_dchannel_dt)
                h.h_elec_ct_dchannel_dt->Fill(dchannel, static_cast<double>(dt));
            if (is_phys && h.h_phys_ct_dchannel_dt)
                h.h_phys_ct_dchannel_dt->Fill(dchannel, static_cast<double>(dt));
            //  CT signal windows from qa_cfg.  Use the wider of the two
            //  upper bounds as the loop's early-exit gate to keep the
            //  filtering symmetric for both neighbour types.
            const int ct_signal_hi_any =
                std::max(qa_cfg.ct_elec_signal_hi, qa_cfg.ct_phys_signal_hi);
            if (dt > ct_signal_hi_any)
                continue;
            if (is_elec &&
                dt >= qa_cfg.ct_elec_signal_lo && dt <= qa_cfg.ct_elec_signal_hi)
                ++n_elec_ct;
            if (is_phys &&
                dt >= qa_cfg.ct_phys_signal_lo && dt <= qa_cfg.ct_phys_signal_hi)
                ++n_phys_ct;
        }

        //  Per-channel CT probability profiles (boolean: any CT?).
        if (h.h_phys_ct_per_channel)
            h.h_phys_ct_per_channel->Fill(hit.channel, n_phys_ct > 0 ? 100.0 : 0.0);
        if (h.h_elec_ct_per_channel)
            h.h_elec_ct_per_channel->Fill(hit.channel, n_elec_ct > 0 ? 100.0 : 0.0);
        //  Smeared CT hitmaps — weight = n_ct_neighbours × 100 per primary hit.
        if (hit.x > -990.f)
        {
            if (n_phys_ct > 0 && h.h_phys_ct_hitmap)
                h.h_phys_ct_hitmap->Fill(hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd(),
                                         100.0 * n_phys_ct);
            if (n_elec_ct > 0 && h.h_elec_ct_hitmap)
                h.h_elec_ct_hitmap->Fill(hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd(),
                                         100.0 * n_elec_ct);
        }
    }
}

} // namespace btana::lightdata
