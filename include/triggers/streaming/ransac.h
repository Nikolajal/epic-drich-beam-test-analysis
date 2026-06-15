#pragma once

/**
 * @file triggers/streaming/ransac.h
 * @brief Stage 2 of the streaming-trigger pipeline — RANSAC ring finder.
 *
 * For every frame where the score stage (stage 1, `score.h`) fired a
 * `_TRIGGER_STREAMING_RING_FOUND_` event, this stage:
 *
 *   1. Selects Cherenkov hits within `±time_window_ns` of the streaming
 *      trigger's `fine_time` (window inherited from the score stage's
 *      configuration — there is no separate `time_cut_ns` knob).
 *   2. Votes the surviving xy points into a RANSAC accumulator
 *      (`mist::ring_finding::HoughTransform`).
 *   3. Extracts up to **2 rings** (hardcoded — the detector has two
 *      Cherenkov radiators, no physical configuration produces more).
 *   4. Tags ring-member hits with `HitmaskRansacRingTagFirst` /
 *      `HitmaskRansacRingTagSecond` for downstream consumption.
 *   5. Emits one `_TRIGGER_RANSAC_RING_FOUND_` event per ring into the
 *      frame's trigger collection (mean time of tagged hits).
 *
 * Centre/radius refinement is **not** done here.  `recodata_writer`
 * re-fits the mask-tagged hits with full LOO + dual/solo split +
 * CB+pol3 radial fit, and all fit-derived observables live in
 * `recodata.root`'s `Rings/` subfolder.  Keeping the fit out of the
 * streaming path keeps this stage cheap and removes the architectural
 * ambiguity of two simultaneous fit pipelines.
 *
 * Configuration lives in [`conf/streaming.toml`](../../../conf/streaming.toml)
 * under `[streaming_ransac]` and is consumed via
 * `StreamingRansacConfigStruct`.  Design notes and open items are in
 * [`DISCUSSION.md`](DISCUSSION.md).
 */

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "alcor_spilldata.h"
#include "alcor_finedata.h"        // AlcorFinedataStruct
#include "triggers/events.h"       // TriggerEvent
#include "utility/config_reader.h" // StreamingRansacConfigStruct

namespace mist
{
namespace ring_finding
{
class HoughTransform;
}
} // namespace mist
class TH1F;
class TH2F;

/**
 * @brief Bundle of QA histogram pointers consumed by the RANSAC stage.
 *
 * All fields are raw pointers — the writer owns the histograms (via
 * `RootHist<T>`) and passes `.get()` here.  Any field left as `nullptr`
 * disables its corresponding fill.
 */
struct StreamingRansacQA
{
    /// Hitmap of Cherenkov hits flagged with `HitmaskStreamingRingTrigger`
    /// (i.e. hits that contributed to the score stage's cluster).
    TH2F *full_hitmap = nullptr;

    /// Hitmap of Cherenkov hits surviving the `±time_window_ns` time
    /// pre-cut around the streaming trigger's `fine_time`.
    TH2F *time_cut_hitmap = nullptr;

    /// Per-frame count of rings returned by `HoughTransform::find_rings`
    /// (0, 1, or 2 — capped by the hardcoded `max_rings = 2`).
    TH1F *nrings = nullptr;

    /// Hitmap of Cherenkov hits tagged as belonging to either ring.
    TH2F *ring_finder_hitmap = nullptr;

    /// Hitmap of hits tagged with `HitmaskRansacRingTagFirst`.
    TH2F *first_hitmap = nullptr;

    /// Hitmap of hits tagged with `HitmaskRansacRingTagSecond`.
    TH2F *second_hitmap = nullptr;

    /// **RANSAC peak** outputs for the first ring — taken straight from
    /// `RingResult::{cx, cy, radius}`, no refinement applied at this
    /// stage.  `recodata_writer` re-fits the mask-tagged hits and
    /// publishes the refined values under `recodata.root`'s `Rings/`
    /// subdirectory; an earlier in-trigger `fit_circle` step was
    /// removed (the lightdata-side fit was QA-only and architecturally
    /// duplicated the recodata fit).
    TH1F *ring_X_first_ransac = nullptr;
    TH1F *ring_Y_first_ransac = nullptr;
    TH1F *ring_R_first_ransac = nullptr;

    /// RANSAC peak outputs for the second ring — same role as above.
    TH1F *ring_X_second_ransac = nullptr;
    TH1F *ring_Y_second_ransac = nullptr;
    TH1F *ring_R_second_ransac = nullptr;

    /// **Filter 1 + 2 calibration QA** — `peak_votes` (y) vs `|active|`
    /// (x), one per ring slot.  Two threshold lines map onto the same
    /// axes:
    ///     y = min_hits                      (absolute floor)
    ///     y = threshold_fraction × x        (relative floor)
    /// Real rings cluster as a band high in `peak_votes`; random-
    /// coincidence peaks track the diagonal at lower density.  Tune
    /// `hough_threshold_fraction` (sets min_hits) and `threshold_fraction`
    /// by moving the two lines into the gap between the band and the
    /// diagonal.  Ring 2's `|active|` is reduced by ring 1's assignment
    /// — different denominator than ring 1's plot.
    TH2F *ring_peak_votes_vs_active_first = nullptr;
    TH2F *ring_peak_votes_vs_active_second = nullptr;

    /// **Filter 3 calibration QA** — per-hit distance from each
    /// assigned hit to the ring arc, i.e.
    /// `||sqrt((x-cx)^2 + (y-cy)^2) - R||` over `hit_indices`.  One
    /// vertical reference line at `x = collection_radius`; a clean
    /// ring's band falls to baseline well before that line, a noisy
    /// band extends past it.
    TH1F *ring_hit_arc_dist_first = nullptr;
    TH1F *ring_hit_arc_dist_second = nullptr;

    /// **Dual-ring sample QA** — mirrors of the first-ring QA above,
    /// gated on `found_rings.size() > 1` (i.e. filled only when a
    /// second ring is *also* found in the same frame).  Same definitions
    /// as the unsuffixed twins, restricted to the cleaner two-ring
    /// subset.  Useful for:
    ///   - confirming the second-ring hitmap's interpretation by
    ///     comparing ring-1 properties between the full sample
    ///     (`*_first`) and the dual-ring subset (`*_first_dual`);
    ///   - giving downstream tuning (threshold, collection_radius)
    ///     a sample that is cleaner by construction;
    ///   - diagnosing whether ring 1 looks systematically different
    ///     in dual-ring events vs first-only events (which would
    ///     hint at the first-ring being a fake in the single-ring
    ///     subset).
    TH2F *first_hitmap_dual = nullptr;
    TH1F *ring_X_first_dual = nullptr;
    TH1F *ring_Y_first_dual = nullptr;
    TH1F *ring_R_first_dual = nullptr;
    TH1F *ring_X_first_ransac_dual = nullptr;
    TH1F *ring_Y_first_ransac_dual = nullptr;
    TH1F *ring_R_first_ransac_dual = nullptr;
    TH2F *ring_peak_votes_vs_active_first_dual = nullptr;
    TH1F *ring_hit_arc_dist_first_dual = nullptr;

    /// **Solo-ring sample QA** — mirrors of the first-ring QA above,
    /// gated on `found_rings.size() == 1` (i.e. filled only when *no*
    /// second ring is found in the same frame).  Complement of the
    /// `_dual` set above — together they partition the full first-ring
    /// sample (`_solo + _dual = full *_first`).  Useful for:
    ///   - isolating events where only one radiator fired, which may
    ///     include single-photon noise that the dual-ring requirement
    ///     would have rejected;
    ///   - spotting fake first rings — if `_solo` shows a distribution
    ///     that looks systematically worse than `_dual` (wider arc_dist,
    ///     less localised centre), the 1-ring sample is contaminated
    ///     and you may want to tighten thresholds.
    TH2F *first_hitmap_solo = nullptr;
    TH1F *ring_X_first_solo = nullptr;
    TH1F *ring_Y_first_solo = nullptr;
    TH1F *ring_R_first_solo = nullptr;
    TH1F *ring_X_first_ransac_solo = nullptr;
    TH1F *ring_Y_first_ransac_solo = nullptr;
    TH1F *ring_R_first_ransac_solo = nullptr;
    TH2F *ring_peak_votes_vs_active_first_solo = nullptr;
    TH1F *ring_hit_arc_dist_first_solo = nullptr;
};

/**
 * @brief Run the RANSAC ring-finder on every ring-seeding trigger present
 *        in a frame, emitting one `_TRIGGER_RANSAC_RING_FOUND_` event
 *        per ring directly into the frame's trigger collection.
 *
 * Iterates over the frame's triggers, processes every ring-seeding trigger
 * (hardware triggers + `_TRIGGER_STREAMING_RING_FOUND_`; see
 * `is_ring_seed_trigger`), and (for each):
 *   - builds a ring-candidate list via a `±time_window_ns` time pre-cut
 *     (skipping hits already tagged by an earlier seed in this frame),
 *   - runs `mist::ring_finding::find_rings_ransac` on the candidates,
 *   - tags the contributing hits with `HitmaskRansacRingTagFirst / Second`,
 *   - emits the ring trigger event via `spilldata.add_trigger_to_frame`.
 *
 * Centre/radius refinement (Taubin `circle_fit`) and N_γ happen downstream
 * in `recodata_writer` on the mask-tagged hits.
 *
 * @param spilldata               Spill data for the current frame.  Mask bits
 *                                are written back on the Cherenkov hits
 *                                in-place; ring trigger events are appended via
 *                                `add_trigger_to_frame`.
 * @param frame_id                Index of the frame being processed.
 * @param streaming_trigger_count In/out counter incremented once per
 *                                streaming self-trigger processed across the
 *                                run; gates the first-1000 fills of
 *                                `qa.frames_examples`.
 * @param ispill                  Current spill index (0-based); QA only
 *                                fills `frames_examples` when `ispill == 0`.
 * @param time_window_ns          Time pre-cut width — inherited from the
 *                                streaming-score `time_window_ns`.
 * @param cfg                     Streaming-RANSAC config struct.  Supplies the
 *                                RANSAC knobs (`ransac_iterations`,
 *                                `ransac_min_significance`, `ransac_min_inliers`,
 *                                `collection_radius`, `r_min`, `r_max`).
 * @param qa                      QA histogram bundle (any field may be `nullptr`).
 */
void run_streaming_ransac_trigger(
    AlcorSpilldata &spilldata,
    uint32_t frame_id,
    int &streaming_trigger_count,
    int ispill,
    float time_window_ns,
    const StreamingRansacConfigStruct &cfg,
    const StreamingRansacQA &qa,
    const std::unordered_map<int, float> &channel_score_weights);

// ─────────────────────────────────────────────────────────────────────
//  Frame-level multithreading: parallel find_rings_ransac + serial drain
//
//  `run_streaming_ransac_trigger` reads the frame's hits + triggers from
//  the spill and writes back to it (ring-tag mask bits, per-frame ring
//  geometry, `_TRIGGER_RANSAC_RING_FOUND_` events) — none of which is safe
//  under the parallel per-frame pass (the spill's unordered_map and ROOT
//  histograms are not thread-safe).  `run_streaming_ransac_compute` does
//  the expensive work (candidate collection + `find_rings_ransac` + dedup +
//  ring tagging) on a worker thread, reading only its frame's hits + the
//  seed triggers, filling the (per-thread-cloned) QA bundle, and BUFFERING
//  the spill mutations into a @ref RansacMutations record.  The caller
//  replays those mutations serially, in frame order, after the
//  streaming-score drain (so the RANSAC ring-tag bits OR onto the
//  streaming-ring bit and the trigger order stays streaming-then-RANSAC).
//
//  RANSAC is grid-free (`find_rings_ransac` is a free function with no
//  shared accumulator), so the ONLY per-thread state is the per-thread QA
//  clone bundle — there is no per-thread HoughTransform.
// ─────────────────────────────────────────────────────────────────────

/// Spill mutations a frame's RANSAC stage would apply, deferred for serial
/// replay.
struct RansacMutations
{
    /// RANSAC ring-tag bits to OR onto a cherenkov hit's mask: (hit index in
    /// the frame's `cherenkov_hits`, bits = HitmaskRansacRingTag{First,Second}).
    /// OR (not overwrite) so the streaming-ring bit set by the score drain
    /// survives.
    std::vector<std::pair<int, HitMask>> mask_writes;

    /// `_TRIGGER_RANSAC_RING_FOUND_` events (one per ring), appended via
    /// `add_trigger_to_frame` after the streaming trigger.
    std::vector<TriggerEvent> ransac_triggers;

    /// Increment to the run-wide streaming-self-trigger counter (one per
    /// `_TRIGGER_STREAMING_RING_FOUND_` seed processed).
    int streaming_trigger_count_inc = 0;

    /// Per-frame ring geometry to propagate to the frame link for recodata
    /// seeding.  Only the slots whose `has_ring*` flag is set are written;
    /// these keep the strongest (peak_votes) ring per slot across all seed
    /// triggers in the frame, matching the serial `best_ring_votes` logic.
    bool has_ring1 = false;
    bool has_ring2 = false;
    float r1_cx = 0.f, r1_cy = 0.f, r1_r = 0.f;
    float r2_cx = 0.f, r2_cy = 0.f, r2_r = 0.f;
};

/// Pure-compute RANSAC stage for one frame.  Thread-safe given a per-thread
/// @p qa clone bundle; reads only @p frame_hits (positions must already be
/// assigned), the @p seed_triggers to drive (hardware + TIMING + streaming,
/// in the SAME order the serial path would iterate them), and @p
/// streaming_mask_indices naming the hits the score flagged (used for the
/// `full_hitmap` QA, since the streaming-ring bits are not yet written to
/// the hits during the parallel pass).  Runs `find_rings_ransac` + dedup +
/// tagging, fills the QA clones, and returns the buffered spill mutations
/// for serial replay.
RansacMutations run_streaming_ransac_compute(
    const std::vector<AlcorFinedataStruct> &frame_hits,
    const std::vector<TriggerEvent> &seed_triggers,
    const std::vector<int> &streaming_mask_indices,
    int ispill,
    float time_window_ns,
    const StreamingRansacConfigStruct &cfg,
    const StreamingRansacQA &qa,
    const std::unordered_map<int, float> &channel_score_weights);
