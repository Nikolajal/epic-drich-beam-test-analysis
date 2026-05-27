#pragma once

/**
 * @file triggers/streaming/hough.h
 * @brief Stage 2 of the streaming-trigger pipeline — Hough ring finder.
 *
 * For every frame where the score stage (stage 1, `score.h`) fired a
 * `_TRIGGER_STREAMING_RING_FOUND_` event, this stage:
 *
 *   1. Selects Cherenkov hits within `±time_window_ns` of the streaming
 *      trigger's `fine_time` (window inherited from the score stage's
 *      configuration — there is no separate `time_cut_ns` knob).
 *   2. Votes the surviving xy points into a Hough accumulator
 *      (`mist::ring_finding::HoughTransform`).
 *   3. Extracts up to **2 rings** (hardcoded — the detector has two
 *      Cherenkov radiators, no physical configuration produces more).
 *   4. Refines each ring's centre with a least-squares
 *      [`fit_circle`](../../util/circle_fit.h).
 *   5. Emits one `_TRIGGER_HOUGH_RING_FOUND_` event per ring into the
 *      frame's trigger collection.
 *
 * Configuration lives in [`conf/streaming.toml`](../../../conf/streaming.toml)
 * under `[streaming_hough]` and is consumed via
 * `StreamingHoughConfigStruct`.  Design rationale, sub-cell refinement
 * options, and the `fit_circle` audit are in
 * [`DISCUSSION.md`](DISCUSSION.md) § 2.
 */

#include <cstdint>

#include "alcor_spilldata.h"
#include "util/config_reader.h" // StreamingHoughConfigStruct

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
 * @brief Bundle of QA histogram pointers consumed by the Hough stage.
 *
 * All fields are raw pointers — the writer owns the histograms (via
 * `RootHist<T>`) and passes `.get()` here.  Any field left as `nullptr`
 * disables its corresponding fill.
 */
struct StreamingHoughQA
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

    /// Hitmap of hits tagged with `HitmaskHoughRingTagFirst`.
    TH2F *first_hitmap = nullptr;

    /// Hitmap of hits tagged with `HitmaskHoughRingTagSecond`.
    TH2F *second_hitmap = nullptr;

    /// fit_circle results for the first ring — centre X, Y, radius.
    /// **Caveat:** the writer's distributions show long tails to unphysical
    /// values on background events (the streaming trigger fires, the Hough
    /// returns a low-quality "ring", `fit_circle` extrapolates nonsense).
    /// See DISCUSSION.md § 2.5 for the open item on adding per-ring sanity
    /// cuts on (X, Y, R) and on the Hough peak quality.
    TH1F *ring_X_first = nullptr;
    TH1F *ring_Y_first = nullptr;
    TH1F *ring_R_first = nullptr;

    /// fit_circle results for the second ring — same caveat as the first.
    TH1F *ring_X_second = nullptr;
    TH1F *ring_Y_second = nullptr;
    TH1F *ring_R_second = nullptr;

    /// **Hough peak** outputs for the first ring — taken straight from
    /// `RingResult::{cx, cy, radius}`, before any `fit_circle` refinement.
    /// Useful for diagnosing fit pull: overlay these with the `ring_*`
    /// (fit) hists above to see where the fit moves the centre/radius
    /// most aggressively (or runs away on bad events).
    TH1F *ring_X_first_hough = nullptr;
    TH1F *ring_Y_first_hough = nullptr;
    TH1F *ring_R_first_hough = nullptr;

    /// Hough peak outputs for the second ring — same role as above.
    TH1F *ring_X_second_hough = nullptr;
    TH1F *ring_Y_second_hough = nullptr;
    TH1F *ring_R_second_hough = nullptr;

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
    TH1F *ring_X_first_hough_dual = nullptr;
    TH1F *ring_Y_first_hough_dual = nullptr;
    TH1F *ring_R_first_hough_dual = nullptr;
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
    TH1F *ring_X_first_hough_solo = nullptr;
    TH1F *ring_Y_first_hough_solo = nullptr;
    TH1F *ring_R_first_hough_solo = nullptr;
    TH2F *ring_peak_votes_vs_active_first_solo = nullptr;
    TH1F *ring_hit_arc_dist_first_solo = nullptr;
};

/**
 * @brief Run the Hough ring-finder on every streaming trigger present
 *        in a frame, emitting one `_TRIGGER_HOUGH_RING_FOUND_` event
 *        per ring directly into the frame's trigger collection.
 *
 * Iterates over the frame's triggers, processes every entry tagged
 * `_TRIGGER_STREAMING_RING_FOUND_`, and (for each):
 *   - builds a ring-candidate list via a `±time_window_ns` time pre-cut,
 *   - runs `HoughTransform::find_rings` on the candidates,
 *   - tags the original hits with `HitmaskHoughRingTagFirst / Second`,
 *   - calls `fit_circle` to refine each ring's centre,
 *   - emits the Hough trigger event via `spilldata.add_trigger_to_frame`.
 *
 * @param spilldata               Spill data for the current frame.
 *                                Mask bits are written back on the
 *                                Cherenkov hits in-place; Hough trigger
 *                                events are appended via
 *                                `add_trigger_to_frame`.
 * @param frame_id                Index of the frame being processed.
 * @param ring_finder             Pre-built shared Hough transform (its
 *                                `build_lut` must already have been
 *                                called with the active geometry, with
 *                                `r_min`/`r_max`/`r_step`/`cell_size`
 *                                from @p cfg).
 * @param min_active              `min_active` passed to MIST's
 *                                `find_rings`; `min_hits` is derived as
 *                                `min_active × cfg.min_hits_slack`.
 *                                The caller is expected to compute this
 *                                as `ceil(cfg.hough_threshold_fraction
 *                                × N_active_cherenkov)` (the Hough
 *                                stage doesn't see the channel count).
 * @param streaming_trigger_count In/out counter incremented once per
 *                                streaming trigger processed across the
 *                                whole run; used to gate the first-1000
 *                                fills of `qa.frames_examples`.
 * @param ispill                  Current spill index (0-based); QA only
 *                                fills `frames_examples` when `ispill == 0`.
 * @param time_window_ns          Time pre-cut width — inherited from
 *                                the streaming-score `time_window_ns`
 *                                (NOT a knob in `cfg`;).
 * @param cfg                     Streaming-Hough config struct.  Supplies
 *                                `threshold_fraction`, `min_hits_slack`,
 *                                `collection_radius`, and the
 *                                `fit_circle_init_{x,y,r}` initial guess.
 *                                The LUT-geometry fields (`r_min` etc.)
 *                                are consumed by the caller when
 *                                constructing @p ring_finder, not here.
 * @param qa                      QA histogram bundle (any field may be `nullptr`).
 */
void run_streaming_hough_trigger(
    AlcorSpilldata &spilldata,
    uint32_t frame_id,
    mist::ring_finding::HoughTransform &ring_finder,
    int min_active,
    int &streaming_trigger_count,
    int ispill,
    float time_window_ns,
    const StreamingHoughConfigStruct &cfg,
    const StreamingHoughQA &qa);
