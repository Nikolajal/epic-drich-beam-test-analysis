/**
 * @file triggers/streaming/hough.cxx
 * @brief Implementation of the streaming-trigger Hough ring-finder stage.
 *
 * Algorithm parameters (`threshold_fraction`, `min_hits_slack`,
 * `collection_radius`) come from `StreamingHoughConfigStruct`.  The
 * detector-physics constants `max_rings = 2` (two radiators) and the
 * inherited `time_window_ns` (shared with the score stage) are not
 * knobs by design.
 *
 * See [`include/triggers/streaming/DISCUSSION.md`](../../../include/triggers/streaming/DISCUSSION.md)
 * for the algorithm, parameter physics, and open items.
 */

#include "triggers/streaming/hough.h"

#include <array>
#include <cmath>
#include <vector>

#include "TH1.h"
#include "TH2.h"

#include <mist/ring_finding/hough_transform.h>

#include "alcor_finedata.h"
#include "alcor_data.h"      // HitmaskStreamingRingTrigger, HitmaskHoughRingTagFirst/Second
#include "triggers/events.h" // TriggerEvent, _TRIGGER_STREAMING_RING_FOUND_, _TRIGGER_HOUGH_RING_FOUND_

void run_streaming_hough_trigger(
    AlcorSpilldata &spilldata,
    uint32_t frame_id,
    mist::ring_finding::HoughTransform &ring_finder,
    int min_active,
    int &streaming_trigger_count,
    int ispill,
    float time_window_ns,
    const StreamingHoughConfigStruct &cfg,
    const StreamingHoughQA &qa)
{
    auto &cherenkov_hits = spilldata.get_frame_cherenkov_hits(frame_id);
    auto &triggers_in_frame = spilldata.get_frame_trigger_hits(frame_id);

    //  Loop on all triggers; process the streaming-ring-found ones.
    for (auto current_trigger : triggers_in_frame)
    {
        if (current_trigger.index != _TRIGGER_STREAMING_RING_FOUND_)
            continue;

        auto index = -1;
        streaming_trigger_count++;

        std::vector<AlcorFinedata> ring_candidates;
        std::vector<int> ring_candidates_index;

        for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
        {
            index++;
            AlcorFinedata current_hit(current_cherenkov_hit_struct);
            if (current_hit.is_afterpulse())
                continue;

            if (current_hit.has_mask_bit(HitmaskStreamingRingTrigger) && qa.full_hitmap)
                qa.full_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());

            //  Time pre-cut around the streaming trigger's fine_time.  Width
            //  is inherited from the score-stage time_window_ns — see
            //  include/triggers/streaming/DISCUSSION.md § 2.2.
            if (std::fabs(current_hit.get_time_ns() - current_trigger.fine_time) < time_window_ns)
            {
                if (qa.time_cut_hitmap)
                    qa.time_cut_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                ring_candidates.push_back(current_hit);
                ring_candidates_index.push_back(index);
            }
        }

        //  Hough ring finder.  `max_rings = 2` is hardcoded (two-radiator
        //  detector — no physically realisable configuration produces
        //  more concentric rings in a single event).  Everything else
        //  (threshold_fraction, min_hits_slack, collection_radius) is
        //  config-driven via `cfg`.
        //
        //  Inline ALCOR → generic-Hit adapter (formerly
        //  `AlcorFinedata::alcor_find_rings_hough`; relocated here in the
        //  Phase 3 cleanup so AlcorFinedata is purely a per-hit value type).
        //  generic_to_alcor[i] maps generic_hits[i] back to its ring_candidates
        //  index for the mask write-back below.
        std::vector<mist::ring_finding::Hit> generic_hits;
        std::vector<int> generic_to_alcor;
        generic_hits.reserve(ring_candidates.size());
        generic_to_alcor.reserve(ring_candidates.size());
        for (int i = 0; i < static_cast<int>(ring_candidates.size()); ++i)
        {
            const auto &h = ring_candidates[i];
            // ring_candidates already pass the time pre-cut and
            // is_afterpulse filter above; device < 200 by construction
            // (Cherenkov-only collection).  Nothing more to filter here.
            generic_hits.push_back({h.get_hit_x(),
                                    h.get_hit_y(),
                                    h.get_time_ns(),
                                    static_cast<int>(4 * h.get_global_channel_index())});
            generic_to_alcor.push_back(i);
        }

        //  NOTE: `min_active` was added to MIST's find_rings signature
        //  between `min_hits` and `max_rings`; the pre-extraction call
        //  pre-dated that change and was silently passing `max_rings=7`
        //  (truncated from 7.5f) with `collection_radius` defaulted —
        //  caught and fixed during Phase 4.  `min_hits` is derived from
        //  `min_active` via the config slack ratio.
        const int min_hits_per_ring =
            static_cast<int>(min_active * cfg.min_hits_slack);
        auto found_rings = ring_finder.find_rings(
            generic_hits,
            /*threshold_fraction*/ cfg.threshold_fraction,
            /*min_hits*/ min_hits_per_ring,
            /*min_active*/ min_active,
            /*max_rings*/ 2,
            /*collection_radius*/ cfg.collection_radius,
            /*aggregation_window_cells*/ cfg.aggregation_window_cells);

        //  Write the per-ring mask bits back onto ring_candidates so the
        //  downstream loop can read them.  Ring index → mask bit lookup:
        constexpr std::array<HitMask, 2> ring_masks = {
            HitmaskHoughRingTagFirst,
            HitmaskHoughRingTagSecond};
        for (int ring_idx = 0;
             ring_idx < static_cast<int>(found_rings.size()) &&
             ring_idx < static_cast<int>(ring_masks.size());
             ++ring_idx)
        {
            for (int generic_idx : found_rings[ring_idx].hit_indices)
                ring_candidates[generic_to_alcor[generic_idx]]
                    .add_mask_bit(ring_masks[ring_idx]);
        }

        if (qa.nrings)
            qa.nrings->Fill(found_rings.size());

        index = -1;
        std::array<int, 2> hough_trigger_hits = {0, 0};
        std::array<float, 2> hough_trigger_time = {0.f, 0.f};
        std::vector<std::array<float, 2>> hough_triggered_first;
        std::vector<std::array<float, 2>> hough_triggered_second;

        for (const auto &current_hit : ring_candidates)
        {
            index++;
            const bool is_first = current_hit.has_mask_bit(HitmaskHoughRingTagFirst);
            const bool is_second = current_hit.has_mask_bit(HitmaskHoughRingTagSecond);

            if ((is_first || is_second) && qa.ring_finder_hitmap)
                qa.ring_finder_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());

            if (is_first)
            {
                if (qa.first_hitmap)
                    qa.first_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                //  Dual-ring mirror: only when a second ring also exists.
                //  found_rings.size() is constant for the loop so the
                //  check is essentially free.
                if (qa.first_hitmap_dual && found_rings.size() > 1)
                    qa.first_hitmap_dual->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                //  Solo-ring mirror: only when this is the *only* ring.
                if (qa.first_hitmap_solo && found_rings.size() == 1)
                    qa.first_hitmap_solo->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                hough_trigger_hits[0]++;
                hough_trigger_time[0] += current_hit.get_time_ns();
                hough_triggered_first.push_back({current_hit.get_hit_x(), current_hit.get_hit_y()});
            }
            if (is_second)
            {
                if (qa.second_hitmap)
                    qa.second_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                hough_trigger_hits[1]++;
                hough_trigger_time[1] += current_hit.get_time_ns();
                hough_triggered_second.push_back({current_hit.get_hit_x(), current_hit.get_hit_y()});
            }

            //  Propagate the mask bits set by alcor_find_rings_hough back
            //  onto the underlying cherenkov_hits struct.
            cherenkov_hits[ring_candidates_index[index]].HitMask = current_hit.get_mask();
        }

        //  Emit Hough trigger events per ring.  Centre/radius
        //  refinement happens in `recodata_writer` on the mask-tagged
        //  hits (full LOO + dual/solo splits + CB+pol3 radial fit);
        //  all fit-derived observables live in `recodata.root`'s
        //  `Rings/` subfolder.  The `fit_circle_init_{x,y,r}` knobs
        //  in `[streaming_hough]` are unused at runtime but retained
        //  for backwards compatibility with existing configs.

        //  |active| at each Hough pass — full pool for ring 1, pool
        //  minus ring-1 assignment for ring 2.  Used by the new
        //  peak_votes-vs-|active| 2D QA, which carries both
        //  Filter-1 (min_hits) and Filter-2 (threshold_fraction) lines
        //  on the same axes.
        const int active_at_pass_1 = static_cast<int>(generic_hits.size());
        const int active_at_pass_2 = active_at_pass_1 -
                                     (found_rings.empty() ? 0
                                                          : static_cast<int>(found_rings[0].hit_indices.size()));

        //  Lambda: fill the per-hit |r_hit − R_ring| arc-distance hist.
        //  Caller's `hist` may be nullptr.
        auto fill_arc_dist = [&](const mist::ring_finding::RingResult &ring, TH1F *hist)
        {
            if (!hist)
                return;
            for (int gi : ring.hit_indices)
            {
                const auto &h = generic_hits[gi];
                const float dx = h.x - ring.cx;
                const float dy = h.y - ring.cy;
                const float r_hit = std::sqrt(dx * dx + dy * dy);
                hist->Fill(std::fabs(r_hit - ring.radius));
            }
        };

        if (found_rings.size() > 0 && hough_trigger_hits[0] > 0)
        {
            //  Mean time of hits tagged on the first ring.  Guard on
            //  hough_trigger_hits[0] > 0 is defensive — MIST's
            //  find_rings should never return a ring with an empty
            //  hit_indices, but a 0-divide here would publish NaN
            //  into the trigger record.
            spilldata.add_trigger_to_frame(
                frame_id,
                {static_cast<uint8_t>(_TRIGGER_HOUGH_RING_FOUND_),
                 static_cast<uint16_t>(found_rings.size()),
                 static_cast<float>(hough_trigger_time[0] / hough_trigger_hits[0])});

            //  Hough peak (pre-fit) — RingResult is sorted descending by
            //  peak_votes so found_rings[0] is the strongest candidate.
            if (qa.ring_X_first_hough)
                qa.ring_X_first_hough->Fill(found_rings[0].cx);
            if (qa.ring_Y_first_hough)
                qa.ring_Y_first_hough->Fill(found_rings[0].cy);
            if (qa.ring_R_first_hough)
                qa.ring_R_first_hough->Fill(found_rings[0].radius);

            //  Filter 1+2 + 3 calibration QA.
            if (qa.ring_peak_votes_vs_active_first)
                qa.ring_peak_votes_vs_active_first->Fill(
                    active_at_pass_1, found_rings[0].peak_votes);
            fill_arc_dist(found_rings[0], qa.ring_hit_arc_dist_first);

            //  Dual-ring sample — same Hough-seed QA as above but
            //  only when a second ring is also present.
            if (found_rings.size() > 1)
            {
                if (qa.ring_X_first_hough_dual)
                    qa.ring_X_first_hough_dual->Fill(found_rings[0].cx);
                if (qa.ring_Y_first_hough_dual)
                    qa.ring_Y_first_hough_dual->Fill(found_rings[0].cy);
                if (qa.ring_R_first_hough_dual)
                    qa.ring_R_first_hough_dual->Fill(found_rings[0].radius);
                if (qa.ring_peak_votes_vs_active_first_dual)
                    qa.ring_peak_votes_vs_active_first_dual->Fill(
                        active_at_pass_1, found_rings[0].peak_votes);
                fill_arc_dist(found_rings[0], qa.ring_hit_arc_dist_first_dual);
            }
            //  Solo-ring sample — complement of (dual).
            if (found_rings.size() == 1)
            {
                if (qa.ring_X_first_hough_solo)
                    qa.ring_X_first_hough_solo->Fill(found_rings[0].cx);
                if (qa.ring_Y_first_hough_solo)
                    qa.ring_Y_first_hough_solo->Fill(found_rings[0].cy);
                if (qa.ring_R_first_hough_solo)
                    qa.ring_R_first_hough_solo->Fill(found_rings[0].radius);
                if (qa.ring_peak_votes_vs_active_first_solo)
                    qa.ring_peak_votes_vs_active_first_solo->Fill(
                        active_at_pass_1, found_rings[0].peak_votes);
                fill_arc_dist(found_rings[0], qa.ring_hit_arc_dist_first_solo);
            }
        }
        if (found_rings.size() > 1 && hough_trigger_hits[1] > 0)
        {
            spilldata.add_trigger_to_frame(
                frame_id,
                {static_cast<uint8_t>(_TRIGGER_HOUGH_RING_FOUND_),
                 static_cast<uint16_t>(found_rings.size()),
                 static_cast<float>(hough_trigger_time[1] / hough_trigger_hits[1])});

            if (qa.ring_X_second_hough)
                qa.ring_X_second_hough->Fill(found_rings[1].cx);
            if (qa.ring_Y_second_hough)
                qa.ring_Y_second_hough->Fill(found_rings[1].cy);
            if (qa.ring_R_second_hough)
                qa.ring_R_second_hough->Fill(found_rings[1].radius);

            if (qa.ring_peak_votes_vs_active_second)
                qa.ring_peak_votes_vs_active_second->Fill(
                    active_at_pass_2, found_rings[1].peak_votes);
            fill_arc_dist(found_rings[1], qa.ring_hit_arc_dist_second);
        }
    }
}
