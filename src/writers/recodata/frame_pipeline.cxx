/**
 * @file frame_pipeline.cxx
 * @brief Implementation of `process_frame_pure` — lifted from the
 *        in-function `[&]`-captured lambda formerly inside
 *        `src/recodata_writer.cxx`'s parallel-dispatch loop.
 *
 * Algorithm is unchanged; only captures are replaced by an explicit
 * `FrameProcessContext`.  Bit-identical output was verified vs a
 * pre-extraction baseline snapshot at extraction time (since pruned).
 */

#include "writers/recodata/frame_pipeline.h"
#include "alcor_data.h"

#include <cmath>

#include "alcor_finedata.h"  // AlcorFinedata
#include "alcor_lightdata.h" // AlcorLightdata
#include "triggers/registry.h"
#include "writers/recodata.h" // BTANA_TRIGGER_MIN_SEPARATION

namespace btana::recodata
{

FrameResult process_frame_pure(AlcorLightdata &lightdata,
                               const FrameProcessContext &ctx)
{
    FrameResult res;
    const float frame_size_ns = ctx.framer_cfg.frame_size * BTANA_ALCOR_CC_TO_NS;

    for (const auto &current_trigger : lightdata.get_triggers())
    {
        if (current_trigger.index == _TRIGGER_UNKNOWN_)
            continue;
        const float reg_bin = ctx.registry.index_of(current_trigger.index) + 0.5f;

        const bool is_edge =
            (current_trigger.fine_time < ctx.edge_rejection_ns) ||
            (current_trigger.fine_time > frame_size_ns - ctx.edge_rejection_ns);
        if (is_edge)
        {
            res.edge_fills.emplace_back(reg_bin, current_trigger.fine_time);
            res.trigger_qa_fills.emplace_back(reg_bin, 1.5f); // edge-rejected
            res.had_edge = true;
            continue;
        }

        if (auto it = res.accepted_triggers.find(current_trigger.index);
            it != res.accepted_triggers.end())
        {
            const auto &prev = it->second;
            if (std::fabs((float)current_trigger.coarse - (float)prev.coarse) <
                BTANA_TRIGGER_MIN_SEPARATION)
                continue;                                     // temporal duplicate
            res.trigger_qa_fills.emplace_back(reg_bin, 2.5f); // duplicate-rejected
            res.rejected = true;
            break;
        }

        // First time seeing this trigger — accept.
        res.accepted_triggers[current_trigger.index] = current_trigger;
        for (const auto &chrk : lightdata.get_cherenkov_hits_link())
        {
            const float dt = AlcorFinedata(chrk).get_time_ns() - current_trigger.fine_time;
            res.time_diff_fills.emplace_back(current_trigger.index, dt);
        }
    }

    res.accepted = !res.rejected;
    if (res.rejected)
        return res;

    //  Per-spill physics check
    for (const auto &[idx, trig] : res.accepted_triggers)
    {
        if (idx == TriggerFirstFrames)
            continue;
        if (idx == _TRIGGER_STREAMING_RING_FOUND_)
            continue;
        if (idx == TriggerStartOfSpill)
            continue;
        if (idx == _TRIGGER_UNKNOWN_)
            continue;
        res.frame_is_physics = true;
        break;
    }

    //  Trigger-time Cherenkov occupancy (for h_trigger_cherenkov_hitmap) —
    //  gathered INDEPENDENTLY of the ring finder so the map shows where every
    //  in-time hit lands even on frames where no ring is reconstructed.  Uses
    //  the first hardware / TIMING trigger as the reference and the
    //  [hardware_ring_dt_min, max] window.  (Previously the hitmap drew from
    //  the ring-fit hit set, which silently gated it to ring-found frames.)
    for (const auto &[idx, trig] : res.accepted_triggers)
    {
        const bool hw = idx < static_cast<int>(TriggerFirstFrames) ||
                        idx == static_cast<int>(TriggerTiming);
        if (!hw)
            continue;
        const float dt_lo = ctx.ring_ctx.cfg.hardware_ring_dt_min_ns;
        const float dt_hi = ctx.ring_ctx.cfg.hardware_ring_dt_max_ns;
        for (const auto &chrk : lightdata.get_cherenkov_hits_link())
        {
            AlcorFinedata fh(chrk);
            if (fh.is_afterpulse())
                continue;
            const float dt = fh.get_time_ns() - trig.fine_time;
            if (dt >= dt_lo && dt <= dt_hi)
                res.occupancy_xy.push_back({fh.get_hit_x_rnd(), fh.get_hit_y_rnd()});
        }
        break; // one hardware-trigger reference per frame
    }

    //  Ring reconstruction.
    //
    //  PRIMARY path — refine the rings the streaming-RANSAC already isolated
    //  (hits tagged HitmaskRansacRingTagFirst / *Second).  The RANSAC now seeds
    //  on hardware triggers as well as the streaming self-trigger (see
    //  run_streaming_ransac_trigger), so tagged hits are present on every
    //  hardware- or streaming-triggered physics frame, in QA and production
    //  alike — and fitting the tagged arc captures far-off-centre rings the
    //  all-in-time-hits fit would average into a central blob.
    //
    //  FALLBACK — a frame the RANSAC never seeded (no tagged hits): the legacy
    //  single-circle fit to every in-time hit around the hardware trigger's
    //  reference time.
    {
        //  do_loo gated by the recodata config knob.  `skip_loo_residuals =
        //  true` (typically in conf/QA/streaming.toml) disables the per-hit
        //  leave-one-out loop — the biggest QA speedup lever, at the cost of
        //  no σ_photon measurement.  See RecodataConfigStruct.
        const bool do_loo = !ctx.ring_ctx.cfg.skip_loo_residuals;

        bool have_tagged = false;
        for (const auto &chrk : lightdata.get_cherenkov_hits_link())
        {
            AlcorFinedata fh(chrk);
            if (fh.has_mask_bit(HitmaskRansacRingTagFirst) ||
                fh.has_mask_bit(HitmaskRansacRingTagSecond))
            {
                have_tagged = true;
                break;
            }
        }

        //  Reconstruct ONLY from RANSAC-tagged ring hits.  No fallback: if the
        //  ring finder tagged nothing, there is no ring — fabricating a fit on
        //  every in-time hit (the old time-window path) just produced a
        //  near-origin centroid "ring" per frame, which swamped the centre
        //  distribution.  RANSAC now seeds on hardware triggers too, so a real
        //  ring is always tagged when present.
        if (have_tagged)
        {
            res.first = compute_ring_fit_tagged(
                HitmaskRansacRingTagFirst, lightdata, do_loo, ctx.ring_ctx);
            res.second = compute_ring_fit_tagged(
                HitmaskRansacRingTagSecond, lightdata, do_loo, ctx.ring_ctx);
            //  Genuine second ring iff the finder tagged second-ring hits —
            //  drives the dual/solo split downstream.
            res.frame_has_second_ring = res.second.n_hits > 0;
        }
    }
    return res;
}

} // namespace btana::recodata
