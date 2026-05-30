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

    //  Ring detection + fits (only if frame carries the Hough trigger).
    if (res.accepted_triggers.count(_TRIGGER_HOUGH_RING_FOUND_))
    {
        for (const auto &hit_struct : lightdata.get_cherenkov_hits_link())
        {
            AlcorFinedata fh(hit_struct);
            if (fh.has_mask_bit(HitmaskHoughRingTagSecond))
            {
                res.frame_has_second_ring = true;
                break;
            }
        }
        //  do_loo gated by the recodata config knob.
        //  `skip_loo_residuals = true` (typically in
        //  conf/QA/recodata.toml) disables the per-hit
        //  leave-one-out fit loop — the biggest single
        //  speedup lever for QA mode at the cost of no
        //  σ_photon measurement.  See RecodataConfigStruct.
        const bool do_loo = !ctx.ring_ctx.cfg.skip_loo_residuals;
        res.first = compute_ring_fit(HitmaskHoughRingTagFirst,
                                     lightdata, do_loo, ctx.ring_ctx);
        res.second = compute_ring_fit(HitmaskHoughRingTagSecond,
                                      lightdata, do_loo, ctx.ring_ctx);
    }
    return res;
}

} // namespace btana::recodata
