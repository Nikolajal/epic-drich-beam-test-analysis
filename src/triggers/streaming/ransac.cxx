/**
 * @file triggers/streaming/ransac.cxx
 * @brief Implementation of the streaming-trigger RANSAC ring-finder stage.
 *
 * Algorithm parameters (`threshold_fraction`, `min_hits_slack`,
 * `collection_radius`) come from `StreamingRansacConfigStruct`.  The
 * detector-physics constants `max_rings = 2` (two radiators) and the
 * inherited `time_window_ns` (shared with the score stage) are not
 * knobs by design.
 *
 * See [`include/triggers/streaming/DISCUSSION.md`](../../../include/triggers/streaming/DISCUSSION.md)
 * for the algorithm, parameter physics, and open items.
 */

#include "triggers/streaming/ransac.h"

#include <algorithm> // std::max, std::sort, std::remove_if (C3.1 + C3.4)
#include <array>
#include <cmath>
#include <vector>

#include "TH1.h"
#include "TH2.h"

#include <mist/ring_finding/hough_transform.h>    // Hit, RingResult
#include <mist/ring_finding/ransac_ring_finder.h> // find_rings_ransac

#include "alcor_finedata.h"
#include "alcor_data.h"      // HitmaskStreamingRingTrigger, HitmaskRansacRingTagFirst/Second
#include "triggers/events.h" // TriggerEvent, _TRIGGER_STREAMING_RING_FOUND_, _TRIGGER_RANSAC_RING_FOUND_

//  A trigger that should SEED a RANSAC ring search.  The RANSAC is no longer
//  gated solely on the streaming self-trigger — it also runs on every real
//  hardware trigger so a hardware-triggered physics event always gets a ring
//  pass (and downstream recodata always has RANSAC-tagged hits to refine).
//  Seeds: config-defined hardware triggers (index < 100, e.g. luca_and_finger)
//  + the built-in TIMING + the streaming self-trigger.  Excluded: synthetic
//  markers (FirstFrames / StartOfSpill / UNKNOWN), the RANSAC's own output
//  (RANSAC_RING_FOUND — no recursion), and the legacy TRACKING / RING_FOUND
//  triggers (separate-DAQ / 2024-legacy, not Cherenkov event markers).
static bool is_ring_seed_trigger(int index)
{
    return index < static_cast<int>(TriggerFirstFrames) // config hardware [0,99]
           || index == static_cast<int>(TriggerTiming)  // built-in hardware timing
           || index == static_cast<int>(_TRIGGER_STREAMING_RING_FOUND_);
}

RansacMutations run_streaming_ransac_compute(
    const std::vector<AlcorFinedataStruct> &cherenkov_hits,
    const std::vector<TriggerEvent> &seed_triggers,
    const std::vector<int> &streaming_mask_indices,
    int ispill,
    float time_window_ns,
    const StreamingRansacConfigStruct &cfg,
    const StreamingRansacQA &qa,
    const std::unordered_map<int, float> &channel_score_weights)
{
    (void)ispill; // reserved (was the frames_examples gate; no live use)
    RansacMutations mutations;

    //  best_ring_votes keeps, per slot, the strongest ring seen across all
    //  seed triggers in this frame — mirrors the serial `frame_ld` ring
    //  geometry write, but buffered into `mutations` for the serial drain.
    std::array<int, 2> best_ring_votes = {0, 0};

    //  Which hits the score stage flagged with `HitmaskStreamingRingTrigger`.
    //  In the parallel pass those bits are not yet written to the hits (the
    //  score drain runs later, serially), so the `full_hitmap` QA reads this
    //  precomputed set instead of `has_mask_bit`.
    std::vector<char> is_streaming_masked(cherenkov_hits.size(), 0);
    for (int idx : streaming_mask_indices)
        if (idx >= 0 && idx < static_cast<int>(cherenkov_hits.size()))
            is_streaming_masked[idx] = 1;

    //  Local cross-seed dedup overlay.  The serial path persists ring-tag
    //  bits onto `cherenkov_hits` after each seed pass (the write-back at the
    //  bottom of the loop) and the next seed reads them back to skip
    //  already-claimed hits.  In the pure pass we never touch the input
    //  hits, so accumulate the ring tags into this per-hit raw-mask overlay
    //  instead; it is OR'd onto each freshly-built AlcorFinedata's mask so
    //  `has_mask_bit` behaves exactly as it did serially.  Stored as the raw
    //  uint32 mask (a set of `1u << HitmaskRansacRingTag*` bits).
    std::vector<uint32_t> ring_tag_overlay(cherenkov_hits.size(), 0u);

    //  Loop on all triggers; process every ring-seeding trigger (hardware
    //  triggers + the streaming self-trigger — see is_ring_seed_trigger).
    for (auto current_trigger : seed_triggers)
    {
        if (!is_ring_seed_trigger(current_trigger.index))
            continue;

        auto index = -1;
        //  streaming_trigger_count tracks the streaming self-trigger only
        //  (its historical meaning); hardware-seeded passes don't bump it.
        if (current_trigger.index == _TRIGGER_STREAMING_RING_FOUND_)
            mutations.streaming_trigger_count_inc++;

        std::vector<AlcorFinedata> ring_candidates;
        std::vector<int> ring_candidates_index;

        for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
        {
            index++;
            AlcorFinedata current_hit(current_cherenkov_hit_struct);
            //  Overlay the ring tags claimed by earlier seeds in this frame
            //  (serial path persisted these on the struct; we keep them in a
            //  local overlay so the pure pass mutates nothing shared).
            if (ring_tag_overlay[index])
                current_hit.set_mask(current_hit.get_mask() | ring_tag_overlay[index]);
            if (current_hit.is_afterpulse())
                continue;

            if (is_streaming_masked[index] && qa.full_hitmap)
                qa.full_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());

            //  Cross-seed dedup: a hit already assigned to a ring by an
            //  earlier seed trigger in THIS frame (tag bits tracked in the
            //  overlay above) is not reconsidered — so a ring isn't re-found
            //  and re-emitted when a hardware trigger and a streaming fire
            //  coincide in one frame.
            if (current_hit.has_mask_bit(HitmaskRansacRingTagFirst) ||
                current_hit.has_mask_bit(HitmaskRansacRingTagSecond))
                continue;

            //  Time pre-cut around the seeding trigger's fine_time.  Width
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

        //  No untagged in-window hits for this seed (e.g. a coincident seed
        //  whose ring was already claimed by an earlier one) — nothing to do,
        //  skip the find + QA fills so the nrings hist isn't polluted with 0s.
        if (ring_candidates.empty())
            continue;

        //  RANSAC ring finder.  `max_rings = 2` is hardcoded (two-radiator
        //  detector — no physically realisable configuration produces
        //  more concentric rings in a single event).  Everything else
        //  (threshold_fraction, min_hits_slack, collection_radius) is
        //  config-driven via `cfg`.
        //
        //  Inline ALCOR → generic-Hit adapter (formerly
        //  `AlcorFinedata::alcor_find_rings_ransac`, kept inline here so
        //  AlcorFinedata stays a pure per-hit value type).
        //  generic_to_alcor[i] maps generic_hits[i] back to its ring_candidates
        //  index for the mask write-back below.
        std::vector<mist::ring_finding::Hit> generic_hits;
        std::vector<int> generic_to_alcor;
        //  Per-hit weight for the RANSAC consensus = 1/m_c (the streaming
        //  score's `weight_by_channel`, used DIRECTLY).  A high-DCR channel
        //  fires often from dark counts, so a hit there is probably junk →
        //  small weight; a low-DCR (quiet) channel rarely fires, so a hit there
        //  is more likely a true Cherenkov photon → large weight.  This pushes
        //  the consensus onto the real ring and away from noisy-channel clumps.
        //  Weights are normalised to mean 1.
        std::vector<float> occ_weights;
        const bool use_occ = !channel_score_weights.empty();
        generic_hits.reserve(ring_candidates.size());
        generic_to_alcor.reserve(ring_candidates.size());
        occ_weights.reserve(ring_candidates.size());
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
            if (use_occ)
            {
                const int ch = ::GlobalIndex(h.get_global_index()).channel_ordinal();
                const auto it = channel_score_weights.find(ch);
                occ_weights.push_back(
                    it != channel_score_weights.end()
                        ? it->second // = 1/m_c: low-DCR channels weigh more
                        : 0.f);      // uncalibrated channel → no vote weight
            }
        }
        if (use_occ)
        {
            double sum = 0.0;
            int n = 0;
            for (const float w : occ_weights)
                if (w > 0.f)
                {
                    sum += w;
                    ++n;
                }
            if (n > 0 && sum > 0.0)
            {
                const float inv_mean = static_cast<float>(n / sum);
                for (float &w : occ_weights)
                    w *= inv_mean; // normalise to mean 1 over calibrated hits
            }
        }

        //  Grid-free RANSAC ring finder (replaced the RANSAC accumulator).
        //  Candidates are scored by their 1/m_c-weighted inlier EXCESS over the
        //  expected uniform background, divided by the visible on-sensor arc
        //  length (a completeness / linear-density correction) and gated on
        //  significance.  The density correction is what makes the finder
        //  agnostic to centre position: a far-off-centre arc that shows only a
        //  slice of its ring competes on equal footing with a fully-visible
        //  small ring, instead of always losing on raw seen-hit count.  Low-DCR
        //  (likely-Cherenkov) hits dominate the consensus, and the far-off-centre
        //  / wide-radius range is free (no accumulator).  `max_rings = 2` (two
        //  radiators).
        mist::ring_finding::RansacOptions ropt;
        ropt.max_rings = 2;
        ropt.iterations = cfg.ransac_iterations;
        ropt.inlier_band = cfg.collection_radius; // reuse the hit-assignment band
        ropt.min_significance = cfg.ransac_min_significance;
        ropt.min_inliers = cfg.ransac_min_inliers;
        ropt.r_min = cfg.r_min;
        ropt.r_max = cfg.r_max;
        //  Pass the KNOWN sensor fiducial as the acceptance reference.  Per-event
        //  hits are sparse and clustered, so the finder must not infer the sensor
        //  extent from them — without this the completeness/density correction has
        //  no geometric reference and a far arc loses to a small near-cluster
        //  circle.  Square ±sensor_half_extent_mm window (the dRICH hit plane).
        ropt.fiducial_xmin = -cfg.sensor_half_extent_mm;
        ropt.fiducial_xmax = cfg.sensor_half_extent_mm;
        ropt.fiducial_ymin = -cfg.sensor_half_extent_mm;
        ropt.fiducial_ymax = cfg.sensor_half_extent_mm;
        auto found_rings =
            mist::ring_finding::find_rings_ransac(generic_hits, ropt, occ_weights);

        //  C3.4: post-find_rings sanity filter + near-duplicate dedup.
        //
        //  MIST's `find_rings` already applies min_hits / threshold_fraction
        //  gates inside the peak finder, but the returned rings can still:
        //   (a) carry NaN cx/cy/radius if a degenerate accumulator cell
        //       slipped through (numerical edge case at LUT boundaries),
        //   (b) sit outside [r_min, r_max] (rare but seen at the
        //       aggregation-window seam — the SAT counter can land in the
        //       outermost half-cell which is just outside the nominal
        //       radius range), or
        //   (c) be cell-boundary near-duplicates of each other (the same
        //       ring split across two adjacent accumulator cells when the
        //       true centre sits on a cell edge — two peaks separated by
        //       one `cell_size` with effectively the same `radius`).
        //
        //  Drop (a)+(b); merge (c) keeping the higher-peak_votes survivor.
        //  Quality-ratio floor (D-04 follow-up) deferred to a later pass —
        //  needs a calibration sweep to pick the floor.
        {
            const auto is_invalid =
                [&cfg](const mist::ring_finding::RingResult &r)
            {
                if (std::isnan(r.cx) || std::isnan(r.cy) || std::isnan(r.radius))
                    return true;
                if (r.radius < cfg.r_min || r.radius > cfg.r_max)
                    return true;
                //  Centre acceptance: physical Cherenkov rings sit near the
                //  detector centre.  Reject rings whose centre is farther than
                //  centre_padding_mm from (0, 0) on either axis — a hard cut on
                //  the otherwise centre-free RANSAC.  Config-driven, so a wide
                //  centre_padding_mm (e.g. production's 1000 mm) stays permissive
                //  and keeps the far-off-centre arc path; a tight value (QA's
                //  15 mm) prunes spurious far rings, which also speeds the
                //  downstream re-fit / QA.
                if (std::fabs(r.cx) > cfg.centre_padding_mm ||
                    std::fabs(r.cy) > cfg.centre_padding_mm)
                    return true;
                return false;
            };
            found_rings.erase(
                std::remove_if(found_rings.begin(), found_rings.end(), is_invalid),
                found_rings.end());

            if (found_rings.size() >= 2)
            {
                //  Sort by peak_votes descending so the dedup keeps the
                //  strongest survivor of each cluster.
                std::sort(found_rings.begin(), found_rings.end(),
                          [](const mist::ring_finding::RingResult &a,
                             const mist::ring_finding::RingResult &b)
                          { return a.peak_votes > b.peak_votes; });

                std::vector<mist::ring_finding::RingResult> deduped;
                deduped.reserve(found_rings.size());
                for (const auto &r : found_rings)
                {
                    bool is_dup = false;
                    for (const auto &kept : deduped)
                    {
                        const float dx = r.cx - kept.cx;
                        const float dy = r.cy - kept.cy;
                        const float centre_dist =
                            std::sqrt(dx * dx + dy * dy);
                        //  Threshold: centres within one accumulator cell
                        //  AND radii within one r_step → same ring.
                        if (centre_dist < cfg.cell_size &&
                            std::fabs(r.radius - kept.radius) < cfg.r_step)
                        {
                            is_dup = true;
                            break;
                        }
                    }
                    if (!is_dup)
                        deduped.push_back(r);
                }
                found_rings = std::move(deduped);
            }
        }

        //  Write the per-ring mask bits back onto ring_candidates so the
        //  downstream loop can read them.  Ring index → mask bit lookup:
        constexpr std::array<HitMask, 2> ring_masks = {
            HitmaskRansacRingTagFirst,
            HitmaskRansacRingTagSecond};
        for (int ring_idx = 0;
             ring_idx < static_cast<int>(found_rings.size()) &&
             ring_idx < static_cast<int>(ring_masks.size());
             ++ring_idx)
        {
            for (int generic_idx : found_rings[ring_idx].hit_indices)
                ring_candidates[generic_to_alcor[generic_idx]]
                    .add_mask_bit(ring_masks[ring_idx]);

            //  Propagate this ring's geometry to the frame so recodata can SEED
            //  its fit from the finder's robust completeness-corrected (cx,cy,R)
            //  rather than re-finding it (a free re-fit on a sparse short arc is
            //  high-variance and collapses the far centre toward the origin).
            //  Keep the strongest (peak_votes) ring per slot across all seed
            //  triggers in the frame — recodata pools every hit tagged with a
            //  slot's mask, and the strongest seed best represents that pool.
            const auto &rr = found_rings[ring_idx];
            if (rr.peak_votes > best_ring_votes[ring_idx])
            {
                best_ring_votes[ring_idx] = rr.peak_votes;
                if (ring_idx == 0)
                {
                    mutations.has_ring1 = true;
                    mutations.r1_cx = rr.cx;
                    mutations.r1_cy = rr.cy;
                    mutations.r1_r = rr.radius;
                }
                else
                {
                    mutations.has_ring2 = true;
                    mutations.r2_cx = rr.cx;
                    mutations.r2_cy = rr.cy;
                    mutations.r2_r = rr.radius;
                }
            }
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
            const bool is_first = current_hit.has_mask_bit(HitmaskRansacRingTagFirst);
            const bool is_second = current_hit.has_mask_bit(HitmaskRansacRingTagSecond);

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

            //  Buffer the ring-tag bit for the serial drain to OR onto the
            //  underlying cherenkov hit (OR, not overwrite, so the
            //  streaming-ring bit set by the score drain survives), and record
            //  it in the local overlay so a later seed's cross-seed dedup sees
            //  this hit as already claimed.  A hit is assigned to at most one
            //  ring per seed pass (the dedup skips already-tagged hits), so at
            //  most one tag is added here per hit.
            const int hit_idx = ring_candidates_index[index];
            if (is_first)
            {
                mutations.mask_writes.push_back({hit_idx, HitmaskRansacRingTagFirst});
                ring_tag_overlay[hit_idx] |= (1u << HitmaskRansacRingTagFirst);
            }
            else if (is_second)
            {
                mutations.mask_writes.push_back({hit_idx, HitmaskRansacRingTagSecond});
                ring_tag_overlay[hit_idx] |= (1u << HitmaskRansacRingTagSecond);
            }
        }

        //  Emit RANSAC trigger events per ring.  Centre/radius
        //  refinement happens in `recodata_writer` on the mask-tagged
        //  hit collection — full leave-one-out plus dual/solo splits
        //  plus CB+pol3 radial fit.  All fit-derived observables live
        //  in `recodata.root`'s `Rings/` subfolder.  The
        //  `fit_circle_init_{x,y,r}` knobs in `[streaming_ransac]` were
        //  removed in C3.5 — recodata seeds the refit from the RANSAC
        //  peak directly.  Configs that still carry the keys log a
        //  one-shot deprecation warning per key (tolerance ends v2.1).

        //  |active| at each RANSAC pass — full pool for ring 1, pool
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
            mutations.ransac_triggers.push_back(
                {static_cast<uint8_t>(_TRIGGER_RANSAC_RING_FOUND_),
                 static_cast<uint16_t>(found_rings.size()),
                 static_cast<float>(hough_trigger_time[0] / hough_trigger_hits[0])});

            //  RANSAC peak (pre-fit) — RingResult is sorted descending by
            //  peak_votes so found_rings[0] is the strongest candidate.
            if (qa.ring_X_first_ransac)
                qa.ring_X_first_ransac->Fill(found_rings[0].cx);
            if (qa.ring_Y_first_ransac)
                qa.ring_Y_first_ransac->Fill(found_rings[0].cy);
            if (qa.ring_R_first_ransac)
                qa.ring_R_first_ransac->Fill(found_rings[0].radius);

            //  Filter 1+2 + 3 calibration QA.
            if (qa.ring_peak_votes_vs_active_first)
                qa.ring_peak_votes_vs_active_first->Fill(
                    active_at_pass_1, found_rings[0].peak_votes);
            fill_arc_dist(found_rings[0], qa.ring_hit_arc_dist_first);

            //  Dual-ring sample — same RANSAC-seed QA as above but
            //  only when a second ring is also present.
            if (found_rings.size() > 1)
            {
                if (qa.ring_X_first_ransac_dual)
                    qa.ring_X_first_ransac_dual->Fill(found_rings[0].cx);
                if (qa.ring_Y_first_ransac_dual)
                    qa.ring_Y_first_ransac_dual->Fill(found_rings[0].cy);
                if (qa.ring_R_first_ransac_dual)
                    qa.ring_R_first_ransac_dual->Fill(found_rings[0].radius);
                if (qa.ring_peak_votes_vs_active_first_dual)
                    qa.ring_peak_votes_vs_active_first_dual->Fill(
                        active_at_pass_1, found_rings[0].peak_votes);
                fill_arc_dist(found_rings[0], qa.ring_hit_arc_dist_first_dual);
            }
            //  Solo-ring sample — complement of (dual).
            if (found_rings.size() == 1)
            {
                if (qa.ring_X_first_ransac_solo)
                    qa.ring_X_first_ransac_solo->Fill(found_rings[0].cx);
                if (qa.ring_Y_first_ransac_solo)
                    qa.ring_Y_first_ransac_solo->Fill(found_rings[0].cy);
                if (qa.ring_R_first_ransac_solo)
                    qa.ring_R_first_ransac_solo->Fill(found_rings[0].radius);
                if (qa.ring_peak_votes_vs_active_first_solo)
                    qa.ring_peak_votes_vs_active_first_solo->Fill(
                        active_at_pass_1, found_rings[0].peak_votes);
                fill_arc_dist(found_rings[0], qa.ring_hit_arc_dist_first_solo);
            }
        }
        if (found_rings.size() > 1 && hough_trigger_hits[1] > 0)
        {
            mutations.ransac_triggers.push_back(
                {static_cast<uint8_t>(_TRIGGER_RANSAC_RING_FOUND_),
                 static_cast<uint16_t>(found_rings.size()),
                 static_cast<float>(hough_trigger_time[1] / hough_trigger_hits[1])});

            if (qa.ring_X_second_ransac)
                qa.ring_X_second_ransac->Fill(found_rings[1].cx);
            if (qa.ring_Y_second_ransac)
                qa.ring_Y_second_ransac->Fill(found_rings[1].cy);
            if (qa.ring_R_second_ransac)
                qa.ring_R_second_ransac->Fill(found_rings[1].radius);

            if (qa.ring_peak_votes_vs_active_second)
                qa.ring_peak_votes_vs_active_second->Fill(
                    active_at_pass_2, found_rings[1].peak_votes);
            fill_arc_dist(found_rings[1], qa.ring_hit_arc_dist_second);
        }
    }
    return mutations;
}

void run_streaming_ransac_trigger(
    AlcorSpilldata &spilldata,
    uint32_t frame_id,
    int &streaming_trigger_count,
    int ispill,
    float time_window_ns,
    const StreamingRansacConfigStruct &cfg,
    const StreamingRansacQA &qa,
    const std::unordered_map<int, float> &channel_score_weights)
{
    //  Serial entry point = compute then immediate, in-place drain, so
    //  behaviour is identical to the pre-split implementation.  Gather the
    //  inputs the pure pass needs from the spill, run it (filling the QA
    //  bundle directly), then apply the buffered mutations.
    auto &cherenkov_hits = spilldata.get_frame_cherenkov_hits(frame_id);

    //  SNAPSHOT the seed-trigger list before iterating — the drain below
    //  re-enters add_trigger_to_frame (the RANSAC ring triggers), which
    //  push_back's into the same vector get_frame_trigger_hits returns by
    //  reference; driving the pure pass from a copy keeps it stable.
    const std::vector<TriggerEvent> seed_triggers(
        spilldata.get_frame_trigger_hits(frame_id).begin(),
        spilldata.get_frame_trigger_hits(frame_id).end());

    //  Hits the score stage flagged with the streaming-ring bit — the serial
    //  path reads `has_mask_bit(HitmaskStreamingRingTrigger)` directly off the
    //  struct (already drained at this point); reconstruct the index set so
    //  the compute's `full_hitmap` QA fires on exactly the same hits.
    std::vector<int> streaming_mask_indices;
    for (int i = 0; i < static_cast<int>(cherenkov_hits.size()); ++i)
        if (AlcorFinedata(cherenkov_hits[i]).has_mask_bit(HitmaskStreamingRingTrigger))
            streaming_mask_indices.push_back(i);

    const RansacMutations mutations = run_streaming_ransac_compute(
        cherenkov_hits, seed_triggers, streaming_mask_indices, ispill,
        time_window_ns, cfg, qa, channel_score_weights);

    streaming_trigger_count += mutations.streaming_trigger_count_inc;
    for (const auto &[hit_idx, bit] : mutations.mask_writes)
    {
        AlcorFinedata tagged(cherenkov_hits[hit_idx]);
        tagged.add_mask_bit(bit);
        cherenkov_hits[hit_idx].HitMask = tagged.get_mask();
    }
    auto &frame_ld = spilldata.get_frame_link()[frame_id];
    if (mutations.has_ring1)
    {
        frame_ld.ring1_cx = mutations.r1_cx;
        frame_ld.ring1_cy = mutations.r1_cy;
        frame_ld.ring1_radius = mutations.r1_r;
    }
    if (mutations.has_ring2)
    {
        frame_ld.ring2_cx = mutations.r2_cx;
        frame_ld.ring2_cy = mutations.r2_cy;
        frame_ld.ring2_radius = mutations.r2_r;
    }
    for (const auto &trg : mutations.ransac_triggers)
        spilldata.add_trigger_to_frame(frame_id, trg);
}
