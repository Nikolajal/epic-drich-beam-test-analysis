#pragma once

/**
 * @file triggers/streaming/score.h
 * @brief Stage 1 of the streaming-trigger pipeline — DCR-weighted score.
 *
 * Time-cluster pre-filter for the Hough ring-finding stage downstream:
 * scans a frame's Cherenkov hits with a sliding time window, computes a
 * DCR-weighted score `S = Σ_hits 1/m_c`, and emits a
 * `_TRIGGER_STREAMING_RING_FOUND_` event when the standardised score
 * `n_σ = (S − E[S])/σ_S` crosses the configured threshold.  Frames
 * without a streaming trigger are dropped by the lightdata writer.
 *
 * **v0 entry point** (`run_streaming_trigger`) is the original
 * unweighted hit-count version, kept here as a reference and migration
 * path.  **v1 entry point** (`run_streaming_trigger_weighted`) is the
 * DCR-weighted score described above and is what the lightdata writer
 * currently calls.
 *
 * Configuration lives in [`conf/streaming.toml`](../../../conf/streaming.toml)
 * under `[streaming_trigger]`.  Design rationale and the threshold-tuning
 * workflow are in [`DISCUSSION.md`](DISCUSSION.md) § 1.
 */

#include <set>
#include <unordered_map>
#include <vector>
#include "alcor_spilldata.h"

class TH1F;
class TProfile;

/**
 * @brief Self-contained bundle of per-channel weights and the precomputed
 *        noise-hypothesis moments needed to threshold on $n_\sigma$.
 *
 * Built once at the start of a run via @ref build_streaming_trigger_weights
 * and consumed by the streaming-trigger hot loop for O(1) per-hit lookup
 * and stable thresholding.
 *
 * The moments under the pure-noise hypothesis $H_0$ for a window of width $T$:
 *
 *     E[S | H_0]   = T · N_ch
 *     Var[S | H_0] = T · Σ 1/λ_c
 *
 * are precomputed because both are invariant once the weight table is built.
 * The trigger then evaluates
 *
 *     n_σ = (S - E[S]) / sqrt(Var[S])
 *
 * per window in O(1).
 */
struct StreamingTriggerWeights
{
    /// channel ordinal → inverse-rate weight (seconds).  Hits on channels
    /// **not** in this map are skipped entirely by the trigger — there's no
    /// statistical basis to score them, and any constant fallback would
    /// collapse all such hits to the same n_σ (delta-function artefact in
    /// the QA hist).  See `run_streaming_trigger_weighted` for the skip site.
    std::unordered_map<int, float> weight_by_channel;

    /// Expected score per window under H_0 in dimensionless units:
    /// $\mathbb{E}[S] = N_{\mathrm{modelled}}$ (count of channels in the
    /// map — both reliably measured AND active this spill).
    float expected_score_per_window = 0.f;

    /// Standard deviation of score per window under H_0:
    /// $\sigma_S = \sqrt{\sum 1/m_c}$ over channels in the map.
    /// Precomputed as a sqrt so the per-window n_σ check is one
    /// subtraction + one division.
    float sigma_score_per_window = 0.f;

    /// Number of channels in the bundle (= `weight_by_channel.size()`).
    /// Equal to $\mathbb{E}[S]$ by construction; exposed separately as a
    /// diagnostic for logging.
    int n_channels_modelled = 0;

    /// Convert a raw score to its standardised $n_\sigma$ deviation.
    [[nodiscard]] float n_sigma_of(float score) const noexcept
    {
        return (sigma_score_per_window > 0.f)
                   ? (score - expected_score_per_window) / sigma_score_per_window
                   : 0.f;
    }
};

/**
 * @brief Build the per-channel weight bundle from the writer's pre-Scale
 *        `h_dcr_per_channel` profile, working in dimensionless
 *        "expected hits per trigger window" units throughout.
 *
 * **Contract.**  The TProfile passed in is the **pre-Scale** view — the
 * writer's `RootHist<TProfile> h_dcr_per_channel` filled at the DCR-QA site
 * (only on `TriggerFirstFrames` frames) via
 * `TProfile::Fill(channel_ordinal, per_frame_hit_count)`.  At trigger build
 * time (inside the spill loop), each bin content is
 *
 *     μ_c = ⟨hits per frame for channel c⟩
 *
 * — pure noise counts, not the kHz rate the histogram is scaled to *after*
 * the spill loop ends (line :1248 in the writer).
 *
 * **Internal units.**  Rather than convert to Hz/seconds, the trigger works
 * entirely in dimensionless "expected hits per window" units:
 *
 *     k = time_window_ns / frame_length_ns        (dimensionless ratio)
 *     m_c = μ_c · k                                (hits/window for channel c)
 *     w_c = 1 / m_c                                (dimensionless weight)
 *     S = Σ w_c for hits in window                 (dimensionless score)
 *     E[S | H_0] = Σ_c m_c · w_c = N_measured     (just the count of channels)
 *     Var[S | H_0] = Σ_c m_c · w_c² = Σ_c 1/m_c
 *
 * Time units appear only inside `k`, where `time_window_ns` and
 * `frame_length_ns` must be in the *same* unit (ns here) so the ratio is
 * dimensionless.  The trigger does **not** consume the post-Scale kHz view.
 *
 * **Channel handling.**
 *  - **Unmeasured channels** (no entries in the noise sample — dead, missing
 *    RDO, in axis overflow): `μ_c == 0` → skipped entirely.  Not in the weight
 *    map; not counted in the moments.
 *  - **Under-measured channels** (< `min_noise_hits` total hits in the noise
 *    sample): also skipped.  This is the statistical reliability gate — it
 *    replaces a flat rate floor (which would clamp these channels' weights to
 *    the maximum, producing 20+ σ noise-tail outliers when they re-fire).
 *  - **Reliably-measured channels** (≥ `min_noise_hits`): get weight `1/m_c`
 *    with no further capping.  The lower the channel's rate, the larger its
 *    weight — but we trust it because we have the statistics to back it.
 *  - **default_weight** (used when a hit arrives on a channel not in the map):
 *    set to the **median** of measured weights after the build loop — robust
 *    to outliers and won't blow up the score.
 *
 * @param h_dcr_per_channel_pre_scale  Pre-Scale `TProfile` binned by channel
 *                                     ordinal; bin content = ⟨hits per frame⟩
 *                                     and `GetBinEntries(b)` = number of
 *                                     first-frames frames that contributed
 *                                     to channel `b-1` (used to recover
 *                                     total noise hits for the reliability
 *                                     gate).
 * @param time_window_ns               Sliding-window width [ns].
 * @param frame_length_ns              Frame duration [ns] — same unit as
 *                                     `time_window_ns`.
 * @param min_noise_hits               Minimum number of hits in the noise
 *                                     sample for a channel to enter the
 *                                     weight bundle.  Channels with fewer
 *                                     are excluded as statistically
 *                                     unreliable.  Default 5 ⇒ ~45 %
 *                                     relative error on the rate estimate;
 *                                     raise for stricter purity, lower for
 *                                     more channels in early spills.
 * @param active_channels              Optional set of channel ordinals
 *                                     active in the **current spill**
 *                                     (typically the writer's
 *                                     `active_sensors`).  When supplied,
 *                                     the bundle is restricted to the
 *                                     intersection of "measured in
 *                                     cumulative noise data" and "actively
 *                                     participating this spill".  This
 *                                     prevents `E[S]` and `σ_S` from
 *                                     counting channels that aren't firing
 *                                     this spill (e.g. an RDO that was on
 *                                     in earlier spills but is now off).
 *                                     Pass `nullptr` to use all measured
 *                                     channels (default).
 * @return Fully-populated @ref StreamingTriggerWeights ready for use.
 *
 * @see include/triggers/DISCUSSION.md § 2.
 */
StreamingTriggerWeights
build_streaming_trigger_weights(const TProfile *h_dcr_per_channel_pre_scale,
                                float time_window_ns,
                                float frame_length_ns,
                                double min_noise_hits = 5.0,
                                const std::set<uint32_t> *active_channels = nullptr);

/**
 * @brief Evaluate the streaming trigger for a single frame and accumulate diagnostics.
 *
 * Scans the Cherenkov hits in @p current_spill for the given @p frame_id
 * using a sliding time window of width @p time_window_ns and a Hit-count
 * @p threshold.  Carry-over hits from the previous frame are provided via
 * @p carry_over_hits and updated in-place for the next call.
 *
 * @param current_spill              Spill data for the current frame.
 * @param frame_id                   Index of the frame to evaluate.
 * @param time_window_ns             Width of the sliding trigger window [ns].
 * @param threshold                  Minimum Hit count to fire the trigger.
 * @param carry_over_hits            Hits that spill into the next frame (in/out).
 * @param frame_length_ns            Frame duration in nanoseconds; used to shift
 *                                   carry-over Hit times at frame boundaries.
 *                                   Required (no default) — must be derived from
 *                                   the active @ref FramerConfigStruct so that
 *                                   changes in the TOML config propagate here.
 * @return @c true if the trigger fired for this frame.
 *
 * @note  All per-cluster timing-QA histogram arguments were dropped in
 *        the 2026-Q2 sweep (the last survivor was `h_sigma_vs_nhits`).
 *        See DISCUSSION.md § 2.5 for the dropped-hists list and the open
 *        items that may justify re-introducing a focused subset.
 */
bool run_streaming_trigger(AlcorSpilldata &current_spill,
                           int frame_id,
                           const float time_window_ns,
                           const int threshold,
                           std::vector<std::pair<int, float>> &carry_over_hits,
                           float frame_length_ns);

/**
 * @brief DCR-weighted streaming trigger (v1 — ).
 *
 * Replaces the v0 hit-count threshold with a likelihood-weighted score
 * $S = \sum_{\text{hits}} 1/\lambda_c$ thresholded at $n_\sigma^\star$
 * deviation from the noise-hypothesis expectation.  Quiet channels carry
 * stronger evidence; noisy channels are down-weighted.  See
 * [`include/triggers/DISCUSSION.md`](DISCUSSION.md) § 2 for the model
 * and § 2.4 for the workflow.
 *
 * Algorithm shape is identical to @ref run_streaming_trigger — sliding
 * deque, peak snapshot, end-of-cluster QA fills, carry-over — only the
 * "in cluster" predicate and the "peak" comparator are weighted now.
 *
 * @param weights                   Per-channel weights + precomputed H_0
 *                                  moments — build once per run via
 *                                  @ref build_streaming_trigger_weights.
 * @param n_sigma_threshold         Threshold on the standardised score.
 *                                  Cluster fires when $n_\sigma \geq n_\sigma^\star$.
 * @param h_score_for_qa            Optional `TH1F` to fill with the running
 *                                  $n_\sigma$ at every Hit insertion (i.e.
 *                                  every sliding-window evaluation).  May
 *                                  be `nullptr` — used to populate the
 *                                  noise and data QA score histograms; the
 *                                  caller decides which one to pass based
 *                                  on whether the current frame is in the
 *                                  first-frames or data-taking window.
 *
 * Other parameters mirror @ref run_streaming_trigger exactly.
 *
 * @return `true` if the cluster threshold was crossed at least once in this frame.
 */
bool run_streaming_trigger_weighted(
    AlcorSpilldata &current_spill,
    int frame_id,
    const float time_window_ns,
    const StreamingTriggerWeights &weights,
    const float n_sigma_threshold,
    std::vector<std::tuple<int, float, float>> &carry_over_hits, // (idx, time_ns, weight)
    TH1F *h_score_for_qa,
    float frame_length_ns);
