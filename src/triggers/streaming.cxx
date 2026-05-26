/**
 * @file triggers/streaming.cxx
 * @brief Implementation of @ref run_streaming_trigger.
 *
 * Extracted unchanged from `src/lightdata_writer.cxx` during the D-12
 * refactor.  See [`include/triggers/DISCUSSION.md`](../../include/triggers/DISCUSSION.md)
 * for the v0 → v1 transition plan (DCR-weighted score + $n_\sigma$ threshold).
 */

#include "triggers/streaming.h"

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
    double sum_inv_m  = 0.0;
    int    n_modelled = 0;

    for (int b = 1; b <= n_bins; ++b)
    {
        const double mu_per_frame = h_dcr_per_channel_pre_scale->GetBinContent(b);
        if (mu_per_frame <= 0.0)
            continue;                              // skip unmeasured / missing channels

        // Statistical reliability gate — require enough total noise hits for
        // the rate estimate to be meaningful.  Replaces the old `lambda_floor`
        // cap: a flat rate floor rewards rare-fire channels with the maximum
        // weight (1/m_floor), producing 20+ σ outliers in the noise QA hist.
        // The min-hits filter EXCLUDES under-measured channels instead.  For
        // a TProfile, total hits = mean(y) × number of fills.
        const double n_fills    = h_dcr_per_channel_pre_scale->GetBinEntries(b);
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

        const float m_c    = static_cast<float>(mu_per_frame) * k;   // expected hits/window
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
    out.sigma_score_per_window    = std::sqrt(static_cast<float>(sum_inv_m));
    out.n_channels_modelled       = n_modelled;

    return out;
}

bool run_streaming_trigger(AlcorSpilldata &current_spill,
                           int frame_id,
                           const float time_window_ns,
                           const int threshold,
                           std::vector<std::pair<int, float>> &carry_over_hits,
                           TH1F *h_delta_t_leading_edge,
                           TH1F *h_delta_t_half_centroid,
                           TH1F *h_delta_t_half_center_left,
                           TH1F *h_delta_t_half_center_right,
                           TH2F *h_sigma_vs_nhits,
                           TH2F *h_median_vs_window,
                           TH1F *h_tdc_step_sizes,
                           TH1F *h_tdc_zero_times,
                           TH1F *h_tdc_zero_cluster_size,
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

    auto end_of_cluster = [&]()
    {
        // --- Leading edge reference
        const float t_leading = peak_times.front(); // already sorted
        for (auto i_ter = 1; i_ter < static_cast<int>(peak_times.size()); i_ter++)
            h_delta_t_leading_edge->Fill(peak_times[i_ter] - t_leading);

        auto clean_times = [](const std::vector<float> &times) -> std::vector<float>
        {
            //  Minimum-IQR recursive outlier removal.
            //  At each step, compare the two candidate ranges obtained by excluding
            //  either the first or last endpoint. Remove the endpoint that creates
            //  the larger gap relative to the inner spread — but ONLY if that gap
            //  exceeds a minimum ratio threshold, meaning it is a genuine outlier.
            //  Recurse until no outlier is found or fewer than 3 hits remain.
            //
            //  kOutlierRatio: the endpoint gap must be at least this many times
            //  larger than the other candidate range to be considered an outlier.
            //  Empirically 2.0 is a reasonable starting point — if the removed
            //  endpoint is a real Cherenkov photon, both ranges are comparable
            //  and no removal occurs.
            constexpr float kOutlierRatio = 2.0f;

            std::vector<float> result = times;
            while (true)
            {
                const int n = static_cast<int>(result.size());
                if (n <= 3)
                    break; // cannot clean further — need at least 3 hits

                const float range_excl_last = result[n - 2] - result[0];  // range excluding last
                const float range_excl_first = result[n - 1] - result[1]; // range excluding first

                if (range_excl_last <= range_excl_first && range_excl_last * kOutlierRatio < range_excl_first)
                    result.pop_back(); // last Hit is a genuine outlier
                else if (range_excl_first < range_excl_last && range_excl_first * kOutlierRatio < range_excl_last)
                    result.erase(result.begin()); // first Hit is a genuine outlier
                else
                    break; // no outlier found — stop
            }
            return result;
        };
        std::vector<float> cleaned_times = clean_times(peak_times);

        constexpr float kPeakSeparation_ns = 2.0f;
        constexpr float kSigma_ns = 0.3f;
        constexpr float kPopulationRatio = 20.f; // main:early

        std::vector<float> first_times;
        std::vector<float> second_times;

        if (cleaned_times.size() >= 2)
        {
            // Start: all hits assigned to main peak
            // log-likelihood ratio for Hit t:
            //   LLR = log[ p(t|main) * 19 ] - log[ p(t|early) * 1 ]
            // If LLR < 0, Hit is more likely from early peak.
            // We don't know the peak centers, so we bootstrap:
            // center_main  = mean of all hits (good starting point, 19/20 are main)
            // center_early = center_main - kPeakSeparation_ns

            auto sorted_for_init = cleaned_times; // already sorted
            float center_main = median_of(sorted_for_init);
            float center_early = center_main - kPeakSeparation_ns;

            // One EM pass is enough given the strong ratio prior
            for (int iter = 0; iter < 3; ++iter)
            {
                first_times.clear();
                second_times.clear();
                float sum_main = 0.f, sum_early = 0.f;
                int n_main = 0, n_early = 0;

                for (auto t : cleaned_times)
                {
                    const float d_main = (t - center_main) / kSigma_ns;
                    const float d_early = (t - center_early) / kSigma_ns;
                    // log-ratio: positive → main, negative → early
                    const float llr = 0.5f * (d_early * d_early - d_main * d_main) + std::log(kPopulationRatio);
                    if (llr >= 0.f)
                    {
                        second_times.push_back(t);
                        sum_main += t;
                        n_main++;
                    }
                    else
                    {
                        first_times.push_back(t);
                        sum_early += t;
                        n_early++;
                    }
                }

                // Update centers for next iteration
                if (n_main > 0)
                    center_main = sum_main / n_main;
                if (n_early > 0)
                    center_early = sum_early / n_early;
                // If no early hits found, fix center_early relative to main
                if (n_early == 0)
                    center_early = center_main - kPeakSeparation_ns;
            }
        }
        else
        {
            second_times = cleaned_times;
        }

        auto leave_one_out_mean = [&](const std::vector<float> &input_vec) -> std::vector<float>
        {
            std::vector<float> result;
            if (input_vec.size() < 2)
                return result;

            float full_val = 0.f;
            for (auto val : input_vec)
                full_val += val;

            const int n = static_cast<int>(input_vec.size());
            for (auto val : input_vec)
                result.push_back(val - (full_val - val) / (n - 1));

            return result;
        };

        auto clean_first_times = leave_one_out_mean(first_times);
        auto clean_second_times = leave_one_out_mean(second_times);
        for (auto val : clean_first_times)
            h_delta_t_half_center_left->Fill(val);
        for (auto val : clean_second_times)
            h_delta_t_half_center_right->Fill(val);

        // --- Split-half median
        // Odd/even split by index; each half uses the median of the *other* half
        // as reference — statistically independent, so the difference distribution
        // is unbiased. σ_measured = √2 × σ_single_photon.
        std::vector<float> even_times, odd_times;
        for (auto i_ter = 0; i_ter < static_cast<int>(cleaned_times.size()); i_ter++)
        {
            if (i_ter % 2 == 0)
                even_times.push_back(cleaned_times[i_ter]);
            else
                odd_times.push_back(cleaned_times[i_ter]);
        }

        if (!even_times.empty() && !odd_times.empty())
        {
            const float even_median = median_of(even_times);
            const float odd_median = median_of(odd_times);
            for (auto t : even_times)
            {
                h_sigma_vs_nhits->Fill(cleaned_times.size(), t - odd_median);
                h_delta_t_half_centroid->Fill(t - odd_median);
            }
            for (auto t : odd_times)
            {
                h_sigma_vs_nhits->Fill(cleaned_times.size(), t - even_median);
                h_delta_t_half_centroid->Fill(t - even_median);
            }
            h_median_vs_window->Fill(cleaned_times.back() - cleaned_times.front(), even_median - odd_median);
        }

        constexpr float kTdcEpsilon = 1e-4f; // 0.1 ps — below any physical LSB
        int n_zero_steps = 0;
        for (auto i = 1; i < static_cast<int>(cleaned_times.size()); i++)
        {
            const float step = cleaned_times[i] - cleaned_times[i - 1];
            h_tdc_step_sizes->Fill(step);
            if (std::fabs(step) < kTdcEpsilon)
            {
                n_zero_steps++;
                h_tdc_zero_times->Fill(cleaned_times[i]);
            }
        }
        if (n_zero_steps > 0)
            h_tdc_zero_cluster_size->Fill(peak_count);

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
    TH1F *h_delta_t_leading_edge,
    TH1F *h_delta_t_half_centroid,
    TH1F *h_delta_t_half_center_left,
    TH1F *h_delta_t_half_center_right,
    TH2F *h_sigma_vs_nhits,
    TH2F *h_median_vs_window,
    TH1F *h_tdc_step_sizes,
    TH1F *h_tdc_zero_times,
    TH1F *h_tdc_zero_cluster_size,
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
    bool   in_cluster  = false;
    float  peak_score  = 0.f;
    int    peak_count  = 0;     // kept for QA histograms that bin on hit count
    bool   has_fired   = false;

    // ── Helpers (verbatim from v0 — algorithmic shape is unchanged) ──
    auto median_of = [](std::vector<float> times) -> float
    {
        const int n = static_cast<int>(times.size());
        return (n % 2 == 1) ? times[n / 2]
                            : (times[n / 2 - 1] + times[n / 2]) * 0.5f;
    };

    auto end_of_cluster = [&]()
    {
        const float t_leading = peak_times.front();
        for (auto i_ter = 1; i_ter < static_cast<int>(peak_times.size()); i_ter++)
            h_delta_t_leading_edge->Fill(peak_times[i_ter] - t_leading);

        auto clean_times = [](const std::vector<float> &times) -> std::vector<float>
        {
            constexpr float kOutlierRatio = 2.0f;
            std::vector<float> result = times;
            while (true)
            {
                const int n = static_cast<int>(result.size());
                if (n <= 3) break;
                const float range_excl_last  = result[n - 2] - result[0];
                const float range_excl_first = result[n - 1] - result[1];
                if (range_excl_last <= range_excl_first && range_excl_last * kOutlierRatio < range_excl_first)
                    result.pop_back();
                else if (range_excl_first < range_excl_last && range_excl_first * kOutlierRatio < range_excl_last)
                    result.erase(result.begin());
                else
                    break;
            }
            return result;
        };
        std::vector<float> cleaned_times = clean_times(peak_times);

        constexpr float kPeakSeparation_ns = 2.0f;
        constexpr float kSigma_ns          = 0.3f;
        constexpr float kPopulationRatio   = 20.f;
        std::vector<float> first_times, second_times;

        if (cleaned_times.size() >= 2)
        {
            auto sorted_for_init = cleaned_times;
            float center_main  = median_of(sorted_for_init);
            float center_early = center_main - kPeakSeparation_ns;
            for (int iter = 0; iter < 3; ++iter)
            {
                first_times.clear();
                second_times.clear();
                float sum_main = 0.f, sum_early = 0.f;
                int   n_main = 0, n_early = 0;
                for (auto t : cleaned_times)
                {
                    const float d_main  = (t - center_main)  / kSigma_ns;
                    const float d_early = (t - center_early) / kSigma_ns;
                    const float llr = 0.5f * (d_early * d_early - d_main * d_main) + std::log(kPopulationRatio);
                    if (llr >= 0.f) { second_times.push_back(t); sum_main  += t; ++n_main;  }
                    else            { first_times .push_back(t); sum_early += t; ++n_early; }
                }
                if (n_main  > 0) center_main  = sum_main  / n_main;
                if (n_early > 0) center_early = sum_early / n_early;
                if (n_early == 0) center_early = center_main - kPeakSeparation_ns;
            }
        }
        else
        {
            second_times = cleaned_times;
        }

        auto leave_one_out_mean = [&](const std::vector<float> &input_vec) -> std::vector<float>
        {
            std::vector<float> result;
            if (input_vec.size() < 2) return result;
            float full_val = 0.f;
            for (auto val : input_vec) full_val += val;
            const int n = static_cast<int>(input_vec.size());
            for (auto val : input_vec) result.push_back(val - (full_val - val) / (n - 1));
            return result;
        };
        for (auto v : leave_one_out_mean(first_times))  h_delta_t_half_center_left ->Fill(v);
        for (auto v : leave_one_out_mean(second_times)) h_delta_t_half_center_right->Fill(v);

        std::vector<float> even_times, odd_times;
        for (auto i_ter = 0; i_ter < static_cast<int>(cleaned_times.size()); i_ter++)
            ((i_ter % 2 == 0) ? even_times : odd_times).push_back(cleaned_times[i_ter]);
        if (!even_times.empty() && !odd_times.empty())
        {
            const float even_median = median_of(even_times);
            const float odd_median  = median_of(odd_times);
            for (auto t : even_times)
            {
                h_sigma_vs_nhits ->Fill(cleaned_times.size(), t - odd_median);
                h_delta_t_half_centroid->Fill(t - odd_median);
            }
            for (auto t : odd_times)
            {
                h_sigma_vs_nhits ->Fill(cleaned_times.size(), t - even_median);
                h_delta_t_half_centroid->Fill(t - even_median);
            }
            h_median_vs_window->Fill(cleaned_times.back() - cleaned_times.front(), even_median - odd_median);
        }

        constexpr float kTdcEpsilon = 1e-4f;
        int n_zero_steps = 0;
        for (auto i = 1; i < static_cast<int>(cleaned_times.size()); i++)
        {
            const float step = cleaned_times[i] - cleaned_times[i - 1];
            h_tdc_step_sizes->Fill(step);
            if (std::fabs(step) < kTdcEpsilon)
            {
                ++n_zero_steps;
                h_tdc_zero_times->Fill(cleaned_times[i]);
            }
        }
        if (n_zero_steps > 0)
            h_tdc_zero_cluster_size->Fill(peak_count);

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
