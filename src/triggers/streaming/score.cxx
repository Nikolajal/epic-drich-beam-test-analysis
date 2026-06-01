/**
 * @file triggers/streaming/score.cxx
 * @brief Implementation of the streaming-trigger score stage
 *        (`run_streaming_trigger` v0 and `run_streaming_trigger_weighted` v1).
 *
 * Originally extracted from `src/lightdata_writer.cxx` during the
 * refactor; now lives under the streaming/ subfolder.  See
 * [`include/triggers/streaming/DISCUSSION.md`](../../../include/triggers/streaming/DISCUSSION.md)
 * for the algorithm.
 */

#include "triggers/streaming/score.h"
#include "alcor_data.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <tuple>
#include <vector>

#include "TH1.h"
#include "TH2.h"
#include "TProfile.h"

#include "alcor_finedata.h"
#include "utility/global_index.h"
#include "triggers/events.h"

// ─────────────────────────────────────────────────────────────────────────────
//  In-beam baseline — left-side sideband sampler.
// ─────────────────────────────────────────────────────────────────────────────

StreamingInBeamRates
compute_streaming_inbeam_rates(AlcorSpilldata &spill,
                               float sideband_lo_ns,
                               float sideband_hi_ns,
                               float frame_length_ns,
                               const std::set<uint8_t> &sideband_exclude_triggers)
{
    StreamingInBeamRates out;
    if (sideband_hi_ns <= sideband_lo_ns || frame_length_ns <= 0.f)
        return out;

    const double sideband_width_ns = static_cast<double>(sideband_hi_ns - sideband_lo_ns);
    if (sideband_width_ns <= 0.)
        return out;

    //  Aggregate per-channel hit counts across every valid sideband.
    std::unordered_map<int, uint64_t> hits_by_channel;
    uint64_t n_sidebands = 0;

    auto &frame_link = spill.get_frame_link();
    for (auto &kv : frame_link)
    {
        const uint32_t frame_id = kv.first;
        const auto &triggers = spill.get_frame_trigger_hits(frame_id);
        if (triggers.empty())
            continue;

        const auto &cherenkov = spill.get_frame_cherenkov_hits(frame_id);
        if (cherenkov.empty())
            continue;

        for (const auto &trig : triggers)
        {
            if (sideband_exclude_triggers.count(trig.index))
                continue;

            const double t_lo = static_cast<double>(trig.fine_time) + sideband_lo_ns;
            const double t_hi = static_cast<double>(trig.fine_time) + sideband_hi_ns;
            //  Sideband must lie inside the frame — if it spills past the
            //  frame boundary the rate would be underestimated.  Skip
            //  anchors that are too close to the frame start to admit the
            //  full sideband; this keeps the per-channel µ unbiased.
            if (t_lo < 0.)
                continue;

            ++n_sidebands;

            //  Linear scan — cherenkov_hits is typically O(10–100) per
            //  frame so a sort + binary search isn't worth it.
            for (const auto &h_struct : cherenkov)
            {
                AlcorFinedata hit(h_struct);
                if (hit.is_afterpulse())
                    continue;
                const double t_hit = static_cast<double>(hit.get_time_ns());
                if (t_hit < t_lo || t_hit >= t_hi)
                    continue;
                const int channel_ord =
                    ::GlobalIndex(hit.get_global_index()).channel_ordinal();
                ++hits_by_channel[channel_ord];
            }
        }
    }

    if (n_sidebands == 0 || hits_by_channel.empty())
        return out;

    //  Convert per-channel sideband-hit count into "expected hits per
    //  frame": µ_c = (N_hits_c / N_sidebands) · (T_frame / T_sideband).
    //  In this form µ_c adds directly into the DCR µ_c inside
    //  build_streaming_trigger_weights — the score's H_0 becomes
    //  "noise + in-beam diffuse activity."
    const double scale = static_cast<double>(frame_length_ns) /
                         (static_cast<double>(n_sidebands) * sideband_width_ns);
    out.reserve(hits_by_channel.size());
    for (const auto &[ch, cnt] : hits_by_channel)
        out.emplace(ch, static_cast<float>(static_cast<double>(cnt) * scale));

    return out;
}

StreamingTriggerWeights
build_streaming_trigger_weights(const TProfile *h_dcr_per_channel_pre_scale,
                                float time_window_ns,
                                float frame_length_ns,
                                double min_noise_hits /*= 5.0*/,
                                const std::set<uint32_t> *active_channels /*= nullptr*/,
                                const StreamingInBeamRates *in_beam_rates /*= nullptr*/)
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
    //
    // When in_beam_rates is supplied, each channel's µ_c is
    //
    //     µ_c_total = µ_c_DCR + µ_c_in_beam
    //
    // so the H_0 hypothesis becomes "DCR + in-beam diffuse activity" rather
    // than DCR-only.  Both contributions are already in hits-per-frame units;
    // adding them composes the variances correctly because the score is
    // linear in µ_c (Var[S] = Σ_c 1/m_c, and 1/(m_c_DCR + m_c_in_beam) is
    // exactly what we want — NOT 1/m_c_DCR + 1/m_c_in_beam, which would
    // double-count).  Normalisation thus reduces to "add the two rates."
    const float k = time_window_ns / frame_length_ns;

    const int n_bins = h_dcr_per_channel_pre_scale->GetNbinsX();
    out.weight_by_channel.reserve(static_cast<size_t>(n_bins));

    // E[S | H_0]   = Σ_c m_c · w_c = N_measured       (count of channels)
    // Var[S | H_0] = Σ_c m_c · w_c² = Σ_c 1/m_c       (accumulator below)
    double sum_inv_m = 0.0;
    int n_modelled = 0;

    for (int b = 1; b <= n_bins; ++b)
    {
        const double mu_dcr_only = h_dcr_per_channel_pre_scale->GetBinContent(b);
        //  Reliability gate must be evaluated on the DCR sample alone —
        //  n_fills is the DCR first-frames fill count, so multiplying it
        //  by the combined µ_DCR + µ_in_beam would mix populations.  We
        //  also REQUIRE the channel to have some DCR statistics; in-beam
        //  only firings are not enough to characterise the baseline.
        if (mu_dcr_only <= 0.0)
            continue;
        const double n_fills = h_dcr_per_channel_pre_scale->GetBinEntries(b);
        const double dcr_total_hits = mu_dcr_only * n_fills;
        if (dcr_total_hits < min_noise_hits)
            continue;

        //  Past the reliability gate — now fold the in-beam contribution
        //  into the combined µ that the weight, E[S], and σ_S all use.
        //  Additive in hits-per-frame units; see header comment for why
        //  this composes the variance correctly.
        double mu_per_frame = mu_dcr_only;
        if (in_beam_rates != nullptr)
        {
            auto it = in_beam_rates->find(b - 1);
            if (it != in_beam_rates->end())
                mu_per_frame += static_cast<double>(it->second);
        }

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

    //  Build and sort finedata hits.
    //
    //  WARNING — index-permutation tracking is mandatory.  cherenkov_hits is
    //  GlobalIndex-sorted by parallel_streaming_framer's post-merge step,
    //  but cherenkov_finedata_hits gets sorted by time below.  When the
    //  trigger fires and we write the streaming-ring mask back onto
    //  cherenkov_hits, ihit-into-cherenkov-hits would target a DIFFERENT
    //  physical hit than ihit-into-cherenkov_finedata_hits.  orig_idx maps
    //  the time-sorted slot back to the original cherenkov_hits row.
    std::vector<AlcorFinedata> cherenkov_finedata_hits;
    std::vector<int> orig_idx;
    cherenkov_finedata_hits.reserve(cherenkov_hits.size());
    orig_idx.reserve(cherenkov_hits.size());
    for (int i = 0; i < static_cast<int>(cherenkov_hits.size()); ++i)
    {
        cherenkov_finedata_hits.emplace_back(cherenkov_hits[i]);
        orig_idx.push_back(i);
    }
    //  Joint sort: permute orig_idx by the same key the AlcorFinedata
    //  operator< would use, then materialise the time-sorted finedata vector
    //  through the permutation.  Two-pass keeps the permutation explicit.
    std::sort(orig_idx.begin(), orig_idx.end(),
              [&](int a, int b) { return cherenkov_finedata_hits[a] < cherenkov_finedata_hits[b]; });
    {
        std::vector<AlcorFinedata> tmp;
        tmp.reserve(cherenkov_finedata_hits.size());
        for (int i : orig_idx)
            tmp.push_back(cherenkov_finedata_hits[i]);
        cherenkov_finedata_hits.swap(tmp);
    }

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
    //  served their purpose during early trigger commissioning and
    //  were eating runtime and disk for histograms nobody was reading.
    //  See DISCUSSION.md for the full list and the open items that may
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
            cherenkov_hits[orig_idx[ihit]].HitMask = cherenkov_finedata_hits[ihit].get_mask();

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
//  v1 — DCR-weighted streaming trigger
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
    //
    //  WARNING — index-permutation tracking is mandatory.  cherenkov_hits is
    //  GlobalIndex-sorted by parallel_streaming_framer's post-merge step;
    //  sorting cherenkov_finedata_hits by time desynchronises the two, so
    //  the mask write-back below needs orig_idx[ihit] to hit the correct
    //  row in cherenkov_hits.  See the v0 path above for the same fix.
    std::vector<AlcorFinedata> cherenkov_finedata_hits;
    std::vector<int> orig_idx;
    cherenkov_finedata_hits.reserve(cherenkov_hits.size());
    orig_idx.reserve(cherenkov_hits.size());
    for (int i = 0; i < static_cast<int>(cherenkov_hits.size()); ++i)
    {
        cherenkov_finedata_hits.emplace_back(cherenkov_hits[i]);
        orig_idx.push_back(i);
    }
    std::sort(orig_idx.begin(), orig_idx.end(),
              [&](int a, int b) { return cherenkov_finedata_hits[a] < cherenkov_finedata_hits[b]; });
    {
        std::vector<AlcorFinedata> tmp;
        tmp.reserve(cherenkov_finedata_hits.size());
        for (int i : orig_idx)
            tmp.push_back(cherenkov_finedata_hits[i]);
        cherenkov_finedata_hits.swap(tmp);
    }

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
        //  C7.6 — multiplicity upper-bound cut.  When the operator
        //  opts in via `weights.max_hits_per_window > 0` and the
        //  peak window count exceeds it, the cluster is treated as
        //  pile-up and SUPPRESSED: state is still reset (so the
        //  scan continues normally), but no `_TRIGGER_STREAMING_
        //  RING_FOUND_` is emitted and `has_fired` stays false.
        //  Default `max_hits_per_window == 0` disables the cut →
        //  fully backwards-compatible.
        const bool suppress =
            (weights.max_hits_per_window > 0 &&
             peak_count > weights.max_hits_per_window);
        if (!suppress)
        {
            has_fired = true;

            const float trigger_time = median_of(peak_times);
            current_spill.add_trigger_to_frame(frame_id,
                                               {static_cast<uint8_t>(_TRIGGER_STREAMING_RING_FOUND_),
                                                static_cast<uint16_t>(peak_count),
                                                static_cast<float>(trigger_time)});
        }

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
            cherenkov_hits[orig_idx[ihit]].HitMask = cherenkov_finedata_hits[ihit].get_mask();

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

void fill_window_score_samples(
    AlcorSpilldata &current_spill,
    int frame_id,
    const StreamingTriggerWeights &weights,
    float t_lo_ns,
    float t_hi_ns,
    float time_window_ns,
    TH1F *hist)
{
    if (hist == nullptr || weights.sigma_score_per_window <= 0.f)
        return;

    auto &cherenkov_hits = current_spill.get_frame_cherenkov_hits(frame_id);
    //  cherenkov_hits comes GlobalIndex-sorted (by channel) from the framer's
    //  post-merge step — NOT time-sorted.  The trailing-window scorer below
    //  needs monotonically increasing time (it `break`s past t_hi and evicts
    //  the window front), so materialise a time-sorted (time, channel) list
    //  first, mirroring the explicit sort in run_streaming_trigger_weighted.
    //  No index-permutation tracking is needed here: this routine only fills
    //  a QA histogram and never writes hit masks back.
    std::vector<std::pair<float, int>> sorted_hits;  // (time_ns, channel_ord)
    sorted_hits.reserve(cherenkov_hits.size());
    for (auto &hit_struct : cherenkov_hits)
    {
        AlcorFinedata hit(hit_struct);
        if (hit.is_afterpulse())
            continue;
        sorted_hits.emplace_back(
            hit.get_time_ns(),
            ::GlobalIndex(hit.get_global_index()).channel_ordinal());
    }
    std::sort(sorted_hits.begin(), sorted_hits.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    //  Sliding window mirroring run_streaming_trigger_weighted: maintain
    //  a running score over the trailing time_window_ns and fill once per
    //  modelled hit.  Fills only for hits in [t_lo, t_hi]; the window
    //  still accumulates hits before t_lo so the running score is correct
    //  at the region's left edge.  Hits are now time-ordered, so we can stop
    //  once past t_hi.
    std::deque<std::pair<float, float>> window;  // (time, weight)
    float running_score = 0.f;
    for (const auto &[t, channel_ord] : sorted_hits)
    {
        if (t > t_hi_ns)
            break;  // time-ordered → nothing more in range

        //  Evict front-of-window hits outside the trailing window.
        while (!window.empty() && (t - window.front().first) > time_window_ns)
        {
            running_score -= window.front().second;
            window.pop_front();
        }
        //  Same weight-resolution rule as the sliding scorer: an
        //  uncalibrated channel contributes nothing.
        const auto wit = weights.weight_by_channel.find(channel_ord);
        if (wit == weights.weight_by_channel.end())
            continue;
        window.emplace_back(t, wit->second);
        running_score += wit->second;
        if (t >= t_lo_ns)
            hist->Fill(weights.n_sigma_of(running_score));
    }
}
