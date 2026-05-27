/**
 * @file triggers/streaming/score.cxx
 * @brief Implementation of the streaming-trigger score stage
 *        (`run_streaming_trigger` v0 and `run_streaming_trigger_weighted` v1).
 *
 * Originally extracted from `src/lightdata_writer.cxx` during the D-12
 * refactor; relocated into the streaming/ subfolder during Phase 3 of
 * the streaming-trigger consolidation.  See
 * [`include/triggers/streaming/DISCUSSION.md`](../../../include/triggers/streaming/DISCUSSION.md)
 * § 1 for the algorithm.
 */

#include "triggers/streaming/score.h"
#include "alcor_data.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <tuple>
#include <vector>

#include "TH1.h"
#include "TH2.h"
#include "TProfile.h"

#include "alcor_finedata.h"
#include "util/global_index.h"
#include "triggers/events.h"

StreamingTriggerWeights
build_streaming_trigger_weights(const TProfile *h_dcr_per_channel_pre_scale,
                                float time_window_ns,
                                float frame_length_ns,
                                double min_noise_hits /*= 5.0*/,
                                const std::set<uint32_t> *active_channels /*= nullptr*/)
{
    StreamingTriggerWeights out;
    if (h_dcr_per_channel_pre_scale == nullptr ||
        time_window_ns <= 0.f || frame_length_ns <= 0.f)
        return out;

    // ── Internal units: "expected hits per trigger window" ─────────────────
    // The TProfile bin content is μ_c = ⟨hits per frame for channel c⟩
    // (pre-Scale view; the kHz scaling at writer line :1248 hasn't run yet).
    // Convert to m_c = expected hits per trigger window via the dimensionless
    // ratio k = T_win / T_frame.  All subsequent moments are dimensionless.
    const float k = time_window_ns / frame_length_ns;

    const int n_bins = h_dcr_per_channel_pre_scale->GetNbinsX();
    out.weight_by_channel.reserve(static_cast<size_t>(n_bins));

    // E[S | H_0]   = Σ_c m_c · w_c = N_measured       (count of channels)
    // Var[S | H_0] = Σ_c m_c · w_c² = Σ_c 1/m_c       (accumulator below)
    double sum_inv_m = 0.0;
    int n_modelled = 0;

    for (int b = 1; b <= n_bins; ++b)
    {
        const double mu_per_frame = h_dcr_per_channel_pre_scale->GetBinContent(b);
        if (mu_per_frame <= 0.0)
            continue; // skip unmeasured / missing channels

        // Statistical reliability gate — require enough total noise hits for
        // the rate estimate to be meaningful.  Replaces the old `lambda_floor`
        // cap: a flat rate floor rewards rare-fire channels with the maximum
        // weight (1/m_floor), producing 20+ σ outliers in the noise QA hist.
        // The min-hits filter EXCLUDES under-measured channels instead.  For
        // a TProfile, total hits = mean(y) × number of fills.
        const double n_fills = h_dcr_per_channel_pre_scale->GetBinEntries(b);
        const double total_hits = mu_per_frame * n_fills;
        if (total_hits < min_noise_hits)
            continue;

        // Per-spill participation filter — if the caller supplied the set of
        // channels active in the current spill, only count those.  Otherwise
        // E[S] and σ_S would over-count channels measured in cumulative noise
        // but not actually firing this spill (e.g. an RDO that dropped out).
        const int channel_ord = b - 1;
        if (active_channels != nullptr &&
            active_channels->find(static_cast<uint32_t>(channel_ord)) == active_channels->end())
            continue;

        const float m_c = static_cast<float>(mu_per_frame) * k; // expected hits/window
        const float weight = 1.f / m_c;
        out.weight_by_channel.emplace(channel_ord, weight);
        sum_inv_m += static_cast<double>(weight);
        ++n_modelled;
    }

    // No fallback weight for uncalibrated channels — the trigger hot loop
    // skips hits on channels not in `weight_by_channel` entirely.  Any
    // constant fallback would map every such hit to the same n_σ value
    // and produce a delta-function spike in the QA hist.

    out.expected_score_per_window = static_cast<float>(n_modelled);
    out.sigma_score_per_window = std::sqrt(static_cast<float>(sum_inv_m));
    out.n_channels_modelled = n_modelled;

    return out;
}

bool run_streaming_trigger(AlcorSpilldata &current_spill,
                           int frame_id,
                           const float time_window_ns,
                           const int threshold,
                           std::vector<std::pair<int, float>> &carry_over_hits,
                           float frame_length_ns)
{
    auto &cherenkov_hits = current_spill.get_frame_cherenkov_hits(frame_id);

    //  Build and sort finedata hits
    std::vector<AlcorFinedata> cherenkov_finedata_hits;
    cherenkov_finedata_hits.reserve(cherenkov_hits.size());
    for (const auto &h : cherenkov_hits)
        cherenkov_finedata_hits.emplace_back(h);
    std::sort(cherenkov_finedata_hits.begin(), cherenkov_finedata_hits.end());

    //  Deque window: front = oldest Hit, back = newest Hit.
    //  Eviction is always from the front (O(1)), insertion always at the back (O(1)).
    //  Each entry is {original_index, time_ns}.
    std::deque<std::pair<int, float>> window;
    for (const auto &entry : carry_over_hits)
        window.push_back(entry);

    //  Snapshot of the window at peak occupancy — used for QA histograms and
    //  trigger time. Stored as a sorted vector of times for median computation.
    std::vector<float> peak_times;

    bool in_cluster = false;
    int peak_count = 0;
    bool has_fired = false;

    //  Helper: median of a sorted vector of floats.
    //  Precondition: times must be sorted, size > 0.
    auto median_of = [](std::vector<float> times) -> float
    {
        //  Already sorted
        //  std::sort(times.begin(), times.end());
        const int n = static_cast<int>(times.size());
        return (n % 2 == 1)
                   ? times[n / 2]
                   : (times[n / 2 - 1] + times[n / 2]) * 0.5f;
    };

    //  end_of_cluster: emit the trigger event.
    //
    //  All per-cluster timing diagnostics (leading-edge Δt, EM peak
    //  split into early/main with leave-one-out residuals, half-centroid
    //  fills, h_delta_median_vs_window, the TDC-step LSB-collision
    //  diagnostics, and most recently h_sigma_vs_nhits — odd/even median
    //  σ residuals) were dropped in the 2026-Q2 QA sweep.  They had
    //  served their purpose during pre-D-12 trigger commissioning and
    //  were eating runtime and disk for histograms nobody was reading.
    //  DISCUSSION.md § 2.5 has the full list and the open items that may
    //  justify reintroducing a focused subset.
    auto end_of_cluster = [&]()
    {
        has_fired = true;

        //  Trigger time is the median of the peak window — robust to DCR outliers.
        const float trigger_time = median_of(peak_times);
        current_spill.add_trigger_to_frame(frame_id,
                                           {static_cast<uint8_t>(_TRIGGER_STREAMING_RING_FOUND_),
                                            static_cast<uint16_t>(peak_count),
                                            static_cast<float>(trigger_time)});

        in_cluster = false;
        peak_count = 0;
        peak_times.clear();
    };

    for (int ihit = 0; ihit < static_cast<int>(cherenkov_finedata_hits.size()); ++ihit)
    {
        if (cherenkov_finedata_hits[ihit].is_afterpulse())
            continue;

        const float current_time = cherenkov_finedata_hits[ihit].get_time_ns();

        //  Evict from the front of the deque — O(1) per eviction.
        while (!window.empty() && (current_time - window.front().second) > time_window_ns)
            window.pop_front();
        window.push_back({ihit, current_time});

        const int count = static_cast<int>(window.size());

        if (count >= threshold)
        {
            in_cluster = true;

            cherenkov_finedata_hits[ihit].set_streaming_ring_trigger_mask();
            cherenkov_hits[ihit].HitMask = cherenkov_finedata_hits[ihit].get_mask();

            //  Update peak snapshot when occupancy grows.
            if (count > peak_count)
            {
                peak_count = count;
                peak_times.clear();
                for (const auto &entry : window)
                    peak_times.push_back(entry.second);
                // peak_times is already in time order since window is ordered
                // (deque front = oldest, back = newest, hits are sorted)
            }
        }
        else if (in_cluster)
        {
            end_of_cluster();
        }
    }

    if (in_cluster)
        end_of_cluster();

    //  Carry-over: hits still in the deque window at frame boundary.
    carry_over_hits.clear();
    for (const auto &entry : window)
        carry_over_hits.push_back({-1, entry.second - frame_length_ns});

    return has_fired;
}

// ─────────────────────────────────────────────────────────────────────────────
//  v1 — DCR-weighted streaming trigger (D-12)
// ─────────────────────────────────────────────────────────────────────────────

bool run_streaming_trigger_weighted(
    AlcorSpilldata &current_spill,
    int frame_id,
    const float time_window_ns,
    const StreamingTriggerWeights &weights,
    const float n_sigma_threshold,
    std::vector<std::tuple<int, float, float>> &carry_over_hits,
    TH1F *h_score_for_qa,
    float frame_length_ns)
{
    auto &cherenkov_hits = current_spill.get_frame_cherenkov_hits(frame_id);

    //  Build and sort finedata hits (mirrors v0).
    std::vector<AlcorFinedata> cherenkov_finedata_hits;
    cherenkov_finedata_hits.reserve(cherenkov_hits.size());
    for (const auto &h : cherenkov_hits)
        cherenkov_finedata_hits.emplace_back(h);
    std::sort(cherenkov_finedata_hits.begin(), cherenkov_finedata_hits.end());

    //  Deque entries carry the per-channel weight so eviction can update the
    //  running sum without a second lookup.
    //  {original_index, time_ns, weight}
    using WinEntry = std::tuple<int, float, float>;
    std::deque<WinEntry> window;
    float running_score = 0.f;
    for (const auto &entry : carry_over_hits)
    {
        window.push_back(entry);
        running_score += std::get<2>(entry);
    }

    std::vector<float> peak_times;
    bool in_cluster = false;
    float peak_score = 0.f;
    int peak_count = 0; // kept for QA histograms that bin on hit count
    bool has_fired = false;

    // ── Helpers (verbatim from v0 — algorithmic shape is unchanged) ──
    auto median_of = [](std::vector<float> times) -> float
    {
        const int n = static_cast<int>(times.size());
        return (n % 2 == 1) ? times[n / 2]
                            : (times[n / 2 - 1] + times[n / 2]) * 0.5f;
    };

    //  Algorithm shape matches v0 — see v0's end_of_cluster for the
    //  rationale on the dropped diagnostics (2026-Q2 QA sweep).
    auto end_of_cluster = [&]()
    {
        has_fired = true;

        const float trigger_time = median_of(peak_times);
        current_spill.add_trigger_to_frame(frame_id,
                                           {static_cast<uint8_t>(_TRIGGER_STREAMING_RING_FOUND_),
                                            static_cast<uint16_t>(peak_count),
                                            static_cast<float>(trigger_time)});

        in_cluster = false;
        peak_score = 0.f;
        peak_count = 0;
        peak_times.clear();
    };

    // ── Main sliding-window loop ────────────────────────────────────────────
    for (int ihit = 0; ihit < static_cast<int>(cherenkov_finedata_hits.size()); ++ihit)
    {
        if (cherenkov_finedata_hits[ihit].is_afterpulse())
            continue;

        const float current_time = cherenkov_finedata_hits[ihit].get_time_ns();

        //  Evict front-of-window hits whose timestamps fall outside the
        //  trigger window — keep running_score in sync.
        while (!window.empty() && (current_time - std::get<1>(window.front())) > time_window_ns)
        {
            running_score -= std::get<2>(window.front());
            window.pop_front();
        }

        //  Resolve this hit's weight from its channel ordinal.  If the channel
        //  has no calibrated weight (not in `weight_by_channel` — fewer than
        //  `min_noise_hits` in the cumulative noise sample, or not active in
        //  the current spill's filter set), **skip the hit entirely**.
        //  Reason: any constant fallback (median, default, etc.) collapses
        //  every uncalibrated-channel hit to the same n_σ, producing a
        //  delta-function spike in the QA hist.  We have no statistical
        //  basis to score the contribution → drop it.
        const int channel_ord =
            ::GlobalIndex(cherenkov_finedata_hits[ihit].get_global_index()).channel_ordinal();
        const auto wit = weights.weight_by_channel.find(channel_ord);
        if (wit == weights.weight_by_channel.end())
            continue;
        const float w = wit->second;

        window.emplace_back(ihit, current_time, w);
        running_score += w;

        //  Standardised score and QA fill — only when the weight bundle has
        //  been built (non-zero σ).  Skipping the fill while σ == 0 protects
        //  against the spill-0 bootstrap producing a spurious spike at
        //  n_σ = 0, which would otherwise dominate the noise QA hist's
        //  display range and corrupt threshold tuning.
        const float n_sigma = weights.n_sigma_of(running_score);
        if (h_score_for_qa && weights.sigma_score_per_window > 0.f)
            h_score_for_qa->Fill(n_sigma);

        if (n_sigma >= n_sigma_threshold)
        {
            in_cluster = true;

            cherenkov_finedata_hits[ihit].set_streaming_ring_trigger_mask();
            cherenkov_hits[ihit].HitMask = cherenkov_finedata_hits[ihit].get_mask();

            //  Update peak snapshot when the score grows past the previous max.
            if (running_score > peak_score)
            {
                peak_score = running_score;
                peak_count = static_cast<int>(window.size());
                peak_times.clear();
                peak_times.reserve(window.size());
                for (const auto &entry : window)
                    peak_times.push_back(std::get<1>(entry));
                //  peak_times is in time order — window front = oldest, hits are sorted.
            }
        }
        else if (in_cluster)
        {
            end_of_cluster();
        }
    }

    if (in_cluster)
        end_of_cluster();

    //  Carry-over: hits still in window at frame boundary, time-shifted by
    //  -frame_length_ns so the next frame's window timing stays continuous.
    carry_over_hits.clear();
    carry_over_hits.reserve(window.size());
    for (const auto &entry : window)
        carry_over_hits.emplace_back(-1,
                                     std::get<1>(entry) - frame_length_ns,
                                     std::get<2>(entry));

    return has_fired;
}
