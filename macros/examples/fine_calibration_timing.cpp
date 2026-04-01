#include "../lib_loader.h"
#include <mist/mist.h>

/**
 * @file fine_calibration.cpp
 * @brief Timing and Cherenkov per-channel calibration pipeline.
 *
 * @details
 * Pipeline overview:
 *
 *  Pass 0  — Fine-time calibration scan.
 *            Fills a TH3F (global_index × delta_t × fine_par) used to fit
 *            the linear fine→phase correction (param0, param1) for every
 *            timing TDC channel.  Uses the raw rolling-window timing
 *            coincidence (no sigma weighting).
 *
 *  Pass 1  — Iterative per-channel timing offset correction.
 *            Reconstructs t_ref per frame from the timing coincidence,
 *            fills per-channel delta_t histograms, fits a Gaussian in a
 *            restricted range, and updates param1 (offset) + param2 (sigma)
 *            for timing channels.  Run twice for convergence.
 *
 *  Pass 2  — Cherenkov per-channel time offset extraction.
 *            Reconstructs t_ref from the (now calibrated) timing detector,
 *            fills per-channel delta_t histograms for Cherenkov channels,
 *            extracts the mode (windowed bin-sum) with an optional restricted
 *            Gaussian fit for high-statistics channels, and stores the result
 *            in param2 for Cherenkov channels.  Run twice.
 *
 *  Self-trigger analysis — measures the in-cluster time RMS and per-channel
 *            delta_t vs cluster mean using a Cherenkov-only sliding window.
 *            Run before and after pass 2 to track calibration improvement.
 *
 * param conventions:
 *
 *  Timing channels  (global_index 8180-8580):
 *    param0  angular coefficient of fine-phase linear fit  [cc/tick]
 *    param1  offset of fine-phase linear fit + iterative timing offset [cc]
 *    param2  per-channel Gaussian sigma from timing calibration [ns]
 *            used for 1/sigma^2 weighting in t_ref reconstruction
 *
 *  Cherenkov channels  (global_index 0-8100):
 *    param2  per-channel time offset in clock cycles (additive correction)
 *            param2 = -mode(delta_t) / _ALCOR_CC_TO_NS_
 *            NOTE: param2 meaning differs from timing channels.
 *            Ranges are disjoint - no runtime collision.
 *
 * @author Nicola Rubini
 */

// Timing constants
constexpr int kTimingChip0Id = 0;
constexpr int kTimingChip1Id = 2;
constexpr int kChip0Target = 32;
constexpr int kChip1Target = 31;
constexpr float kRollingWindowNs = 5.f;

// t_ref gate: |mean0 - mean1 - kTRefCenterNs| < kTRefHalfWidthNs
constexpr float kTRefCenterNs = 0.35f;
constexpr float kTRefHalfWidthNs = 0.45f;

// =============================================================================
// §1  Sliding window service
// =============================================================================

/**
 * @brief Configuration for the generic ALCOR sliding-window coincidence engine.
 *
 * Precondition: the hit vector passed to alcor_sliding_window() must be
 * sorted in ascending time order before the call.
 */
struct alcor_sliding_window_config
{
    float window_width = 5.f;
    bool use_ns = true;
    bool reset_on_fire = true;
    bool fire_at_peak = false;

    std::function<bool(const std::deque<alcor_finedata> &)> condition;
    std::function<void(const std::deque<alcor_finedata> &)> on_fire;
};

/**
 * @brief Generic sliding-window coincidence engine over sorted ALCOR hits.
 */
void alcor_sliding_window(
    const std::vector<alcor_finedata> &sorted_hits,
    const alcor_sliding_window_config &cfg)
{
    if (!cfg.condition || !cfg.on_fire)
        return;

    std::deque<alcor_finedata> window;
    int last_fired_size = -1; // track size at last fire for peak detection

    for (int i = 0; i < static_cast<int>(sorted_hits.size()); ++i)
    {
        const auto &hit = sorted_hits[i];
        const float t = cfg.use_ns ? hit.get_time_ns() : hit.get_time();

        while (!window.empty())
        {
            const float t_front = cfg.use_ns
                                      ? window.front().get_time_ns()
                                      : window.front().get_time();
            if (t - t_front > cfg.window_width)
                window.pop_front();
            else
                break;
        }
        window.push_back(hit);

        if (!cfg.condition(window))
            continue;

        if (cfg.fire_at_peak)
        {
            // Determine if next hit would cause an eviction — i.e. we're at peak
            bool is_peak = false;
            if (i + 1 >= static_cast<int>(sorted_hits.size()))
            {
                // Last hit in frame — fire unconditionally
                is_peak = true;
            }
            else
            {
                const float t_next = cfg.use_ns
                                         ? sorted_hits[i + 1].get_time_ns()
                                         : sorted_hits[i + 1].get_time();
                const float t_front = cfg.use_ns
                                          ? window.front().get_time_ns()
                                          : window.front().get_time();
                // Next hit would evict the oldest — window is about to shrink
                is_peak = (t_next - t_front > cfg.window_width);
            }

            if (is_peak)
            {
                cfg.on_fire(window);
                if (cfg.reset_on_fire)
                    window.clear();
            }
        }
        else
        {
            cfg.on_fire(window);
            if (cfg.reset_on_fire)
                window.clear();
        }
    }
}

// =============================================================================
// §2  Shared config factories
// =============================================================================

/**
 * @brief Builds the timing-coincidence condition used by all passes.
 *
 * Fires when >= kChip0Target distinct TDC channels on chip 0 AND
 * >= kChip1Target on chip 1 are present in the window.
 */
alcor_sliding_window_config make_timing_coincidence_config(
    std::function<void(const std::deque<alcor_finedata> &)> on_fire,
    bool reset = true)
{
    alcor_sliding_window_config cfg;
    cfg.window_width = kRollingWindowNs;
    cfg.use_ns = false;
    cfg.reset_on_fire = reset;

    cfg.condition = [](const std::deque<alcor_finedata> &w)
    {
        std::unordered_set<int> ch0, ch1;
        for (const auto &h : w)
        {
            const int tdc = h.get_global_index() / 4;
            if (h.get_chip() == kTimingChip0Id)
                ch0.insert(tdc);
            if (h.get_chip() == kTimingChip1Id)
                ch1.insert(tdc);
        }
        return static_cast<int>(ch0.size()) >= kChip0Target &&
               static_cast<int>(ch1.size()) >= kChip1Target;
    };

    cfg.on_fire = std::move(on_fire);
    return cfg;
}

/**
 * @brief Builds the Cherenkov self-trigger condition.
 *
 * Fires when >= min_hits hits are present in a window of width window_ns.
 */
alcor_sliding_window_config make_cherenkov_selftrigger_config(
    int min_hits,
    float window_ns,
    std::function<void(const std::deque<alcor_finedata> &)> on_fire,
    bool reset = true,
    bool fire_at_peak = true) // ← default true for self-trigger
{
    alcor_sliding_window_config cfg;
    cfg.window_width = window_ns;
    cfg.use_ns = true;
    cfg.reset_on_fire = reset;
    cfg.fire_at_peak = fire_at_peak;

    cfg.condition = [min_hits](const std::deque<alcor_finedata> &w)
    {
        return static_cast<int>(w.size()) >= min_hits;
    };

    cfg.on_fire = std::move(on_fire);
    return cfg;
}

// =============================================================================
// §3  Shared helpers
// =============================================================================

/**
 * @brief Compute per-chip means from a coincidence window.
 *
 * Uses param2 as 1/sigma^2 weight when sigma_weight is true.
 * Falls back to unit weight when param2 <= 0 or sigma_weight is false.
 */
void compute_chip_means(
    const std::deque<alcor_finedata> &window,
    bool sigma_weight,
    float &mean0, float &mean1,
    int &n0, int &n1,
    float &sum0_cc, float &sum1_cc)
{
    float w0 = 0, w1 = 0;
    sum0_cc = 0;
    sum1_cc = 0;
    n0 = 0;
    n1 = 0;

    for (const auto &h : window)
    {
        const float sigma = alcor_finedata::get_param2(h.get_global_index());
        const float wi = (sigma_weight && sigma > 0.f)
                             ? 1.f / (sigma * sigma)
                             : 1.f;
        if (h.get_chip() == kTimingChip0Id)
        {
            sum0_cc += h.get_time() * wi;
            w0 += wi;
            ++n0;
        }
        if (h.get_chip() == kTimingChip1Id)
        {
            sum1_cc += h.get_time() * wi;
            w1 += wi;
            ++n1;
        }
    }
    mean0 = (w0 > 0) ? sum0_cc / w0 : 0.f;
    mean1 = (w1 > 0) ? sum1_cc / w1 : 0.f;
}

/**
 * @brief Robust mode estimator: center of the densest +-half_window_ns window.
 *
 * Scans all bins and returns the center of the bin whose neighborhood has the
 * highest integrated weight. Robust down to ~10 weighted entries.
 */
double windowed_mode(TH1D *h, double half_window_ns)
{
    const double bw = h->GetBinWidth(1);
    const int half_bins = std::max(1, (int)std::round(half_window_ns / bw));
    const int n = h->GetNbinsX();
    double best_sum = -1.;
    int best_bin = h->GetMaximumBin();

    for (int b = 1 + half_bins; b <= n - half_bins; ++b)
    {
        const double s = h->Integral(b - half_bins, b + half_bins);
        if (s > best_sum)
        {
            best_sum = s;
            best_bin = b;
        }
    }
    return h->GetBinCenter(best_bin);
}

// =============================================================================
// §4  Analysis passes
// =============================================================================

/**
 * @brief Pass 0 — fine-time calibration scan.
 *
 * For each timing coincidence window fills:
 *  - h_timing_delta_chip_means : (mean0 - mean1) [ns]
 *  - h_timing_delta_channel    : per-channel leave-one-out delta_t [ns]
 *  - h_fine_calib_scan         : TH3F (global_index x coarse_delta x fine_par)
 */
void run_pass0_analysis(
    std::vector<alcor_lightdata_struct> frames_in_spill,
    TH1F *h_timing_delta_chip_means,
    TH2F *h_timing_delta_channel,
    TH3F *h_fine_calib_scan,
    bool sigma_weight = false)
{
    for (auto &lds : frames_in_spill)
    {
        alcor_lightdata frame(lds);
        auto &timing_hits = frame.get_timing_hits_link();
        if (timing_hits.empty())
            continue;

        std::vector<alcor_finedata> sorted_timing;
        sorted_timing.reserve(timing_hits.size());
        for (const auto &r : timing_hits)
            sorted_timing.emplace_back(r);
        std::sort(sorted_timing.begin(), sorted_timing.end());

        auto cfg = make_timing_coincidence_config(
            [&](const std::deque<alcor_finedata> &w)
            {
                float mean0, mean1, sum0_cc, sum1_cc;
                int n0, n1;
                compute_chip_means(w, sigma_weight,
                                   mean0, mean1, n0, n1, sum0_cc, sum1_cc);

                if (h_timing_delta_chip_means)
                    h_timing_delta_chip_means->Fill(
                        (mean0 - mean1) * _ALCOR_CC_TO_NS_);

                for (const auto &h : w)
                {
                    const float ref = (h.get_chip() == kTimingChip0Id)
                                          ? mean1
                                          : mean0;
                    if (h_fine_calib_scan)
                        h_fine_calib_scan->Fill(
                            h.get_global_index(),
                            h.get_coarse() - ref,
                            h.get_fine());
                }

                for (const auto &h : w)
                {
                    const float t = h.get_time_ns();
                    float ref;
                    if (h.get_chip() == kTimingChip0Id && n0 > 1)
                        ref = (sum0_cc * _ALCOR_CC_TO_NS_ - t) / (n0 - 1);
                    else if (h.get_chip() == kTimingChip1Id && n1 > 1)
                        ref = (sum1_cc * _ALCOR_CC_TO_NS_ - t) / (n1 - 1);
                    else
                        continue;
                    if (h_timing_delta_channel)
                        h_timing_delta_channel->Fill(h.get_global_index(), t - ref);
                }
            });

        alcor_sliding_window(sorted_timing, cfg);
    }
}

/**
 * @brief Pass 1 — t_ref reconstruction + Cherenkov delta_t fill.
 *
 * Reconstructs t_ref from the timing coincidence (gated on inter-chip delta).
 * Then fills h_cherenkov_delta_channel with delta_t = t_hit - t_ref for every
 * Cherenkov hit in frames that have a valid t_ref.
 *
 * If h_cherenkov_delta_channel is null, only the timing QA histogram is filled.
 * h_fine_scan_cherenkov may be null; if non-null, fills (global_index, dt, fine).
 */
void run_pass1_analysis(
    std::vector<alcor_lightdata_struct> frames_in_spill,
    TH1F *h_timing_delta_chip_means,
    TH2F *h_cherenkov_delta_channel,
    TH3F *h_fine_scan_cherenkov,
    bool sigma_weight = false,
    std::function<float(const alcor_finedata &)> weight_fn = [](const alcor_finedata &)
    { return 1.f; })
{
    for (auto &lds : frames_in_spill)
    {
        alcor_lightdata frame(lds);
        auto &timing_hits = frame.get_timing_hits_link();
        auto &cherenkov_hits = frame.get_cherenkov_hits_link();
        if (timing_hits.empty())
            continue;

        std::vector<alcor_finedata> sorted_timing;
        sorted_timing.reserve(timing_hits.size());
        for (const auto &r : timing_hits)
            sorted_timing.emplace_back(r);
        std::sort(sorted_timing.begin(), sorted_timing.end());

        std::vector<float> t_refs;

        auto cfg = make_timing_coincidence_config(
            [&](const std::deque<alcor_finedata> &w)
            {
                float mean0, mean1, sum0_cc, sum1_cc;
                int n0, n1;
                compute_chip_means(w, sigma_weight,
                                   mean0, mean1, n0, n1, sum0_cc, sum1_cc);

                const float delta_ns = (mean0 - mean1) * _ALCOR_CC_TO_NS_;
                if (h_timing_delta_chip_means)
                    h_timing_delta_chip_means->Fill(delta_ns);

                if (std::fabs(delta_ns - kTRefCenterNs) < kTRefHalfWidthNs)
                    t_refs.push_back(0.5f * (mean0 + mean1) * _ALCOR_CC_TO_NS_);
            });

        alcor_sliding_window(sorted_timing, cfg);

        if (t_refs.empty() || cherenkov_hits.empty() ||
            !h_cherenkov_delta_channel)
            continue;

        std::vector<alcor_finedata> sorted_cher;
        sorted_cher.reserve(cherenkov_hits.size());
        for (const auto &r : cherenkov_hits)
            sorted_cher.emplace_back(r);
        std::sort(sorted_cher.begin(), sorted_cher.end());

        for (float t_ref : t_refs)
        {
            if (t_ref < 0.1f)
                continue;
            for (const auto &hit : sorted_cher)
            {
                const float w = weight_fn(hit);
                if (w < 1e-4f)
                    continue;
                const float dt = hit.get_time_ns() - t_ref;
                if (std::fabs(dt) > 12.f)
                    continue;
                if (h_cherenkov_delta_channel)
                    h_cherenkov_delta_channel->Fill(hit.get_global_index(), dt, w);
                // Fill fine scan — same structure as timing pass0:
                // x = global_index, y = delta_t, z = fine parameter
                // This lets you check whether the fine interpolation non-linearity
                // is correlated with the residual delta_t for Cherenkov channels,
                // exactly as done for timing channels in pass0.
                if (h_fine_scan_cherenkov)
                    h_fine_scan_cherenkov->Fill(
                        hit.get_global_index(),
                        dt,
                        hit.get_fine());
            }
        }
    }
}

/**
 * @brief Self-trigger analysis — in-cluster time RMS and per-channel delta_t.
 *
 * Slides a Cherenkov-only window of width window_ns. When >= min_hits hits
 * are present, computes cluster mean and RMS, fills diagnostic histograms,
 * and fills h_delta_selfref with per-channel delta_t = t_hit - t_cluster_mean.
 *
 * Run before and after pass 2 to directly measure calibration improvement:
 *  - h_delta_selfref shows the batch-4 TDC offset structure before calib
 *    and should collapse toward zero after.
 *  - h_window_sigma should narrow as calibration improves.
 *
 * All histogram arguments may be null.
 */
void run_selftrigger_analysis(
    std::vector<alcor_lightdata_struct> frames_in_spill,
    TH1F *h_window_multiplicity,
    TH1F *h_window_sigma,
    TH2F *h_sigma_vs_multiplicity,
    TH2F *h_delta_selfref = nullptr,
    int min_hits = 8,
    float window_ns = 3.f)
{
    for (auto &lds : frames_in_spill)
    {
        alcor_lightdata frame(lds);
        auto &cherenkov_hits = frame.get_cherenkov_hits_link();
        if (cherenkov_hits.empty())
            continue;

        std::vector<alcor_finedata> sorted_cher;
        sorted_cher.reserve(cherenkov_hits.size());
        for (const auto &r : cherenkov_hits)
            sorted_cher.emplace_back(r);
        std::sort(sorted_cher.begin(), sorted_cher.end());

        auto cfg = make_cherenkov_selftrigger_config(
            min_hits, window_ns,
            [&](const std::deque<alcor_finedata> &w)
            {
                double sum = 0, sum2 = 0;
                for (const auto &h : w)
                {
                    const double t = h.get_time_ns();
                    sum += t;
                    sum2 += t * t;
                }
                const int n = static_cast<int>(w.size());
                const double mean = sum / n;
                const double sigma = std::sqrt(
                    std::max(0., sum2 / n - mean * mean));

                if (h_window_multiplicity)
                    h_window_multiplicity->Fill(n);
                if (h_window_sigma)
                    h_window_sigma->Fill(sigma);
                if (h_sigma_vs_multiplicity)
                    h_sigma_vs_multiplicity->Fill(n, sigma);

                if (h_delta_selfref)
                    for (const auto &h : w)
                        h_delta_selfref->Fill(
                            h.get_global_index(),
                            h.get_time_ns() - mean);
            });

        alcor_sliding_window(sorted_cher, cfg);
    }
}

// =============================================================================
// §5  Main calibration function
// =============================================================================

void fine_calibration_timing(
    std::string data_repository,
    std::string run_name,
    int max_spill = 1,
    bool force_recodata_rebuild = false,
    bool force_lightdata_rebuild = false,
    std::string mapping_conf = "conf/mapping_conf.2025.toml",
    std::string trigger_conf = "conf/trigger_conf.toml")
{
    // Input
    const std::string input_filename =
        data_repository + "/" + run_name + "/lightdata.root";
    TFile *input_file = new TFile(input_filename.c_str());

    TTree *lightdata_tree = (TTree *)input_file->Get("lightdata");
    alcor_spilldata *spilldata = new alcor_spilldata();
    spilldata->link_to_tree(lightdata_tree);

    auto fine_time_calib_th2f = (TH2F *)input_file->Get("h_fine_calib");
    alcor_finedata::generate_calibration(fine_time_calib_th2f, true);

    mapping current_mapping(mapping_conf);
    auto trigger_configs = trigger_conf_reader(trigger_conf);
    trigger_registry registry(trigger_configs);

    const int all_spills = static_cast<int>(
        std::min((long long)lightdata_tree->GetEntries(), (long long)max_spill));

    // Output
    const std::string outname =
        data_repository + "/" + run_name + "/timing_fine_calib.root";
    if (std::filesystem::exists(outname) && !force_recodata_rebuild)
        mist::logger::info(
            Form("Output file already exists, skipping: %s", outname.c_str()));
    TFile *output_file = TFile::Open(outname.c_str(), "RECREATE");

    // Fit functions
    TF2 *fit_linear_gaus = new TF2(
        "fit_linear_gaus",
        "[0]*TMath::Gaus(y,[1]*x+[2],[3])",
        0, 255, -12, 12);
    fit_linear_gaus->SetParLimits(0, 0, 20. * all_spills);
    fit_linear_gaus->SetParLimits(1, 1e-3, 1e-1);
    fit_linear_gaus->SetParLimits(2, -3, 0);
    fit_linear_gaus->SetParLimits(3, 1e-2, 1e+0);

    TF1 *fit_gaus = new TF1(
        "fit_gaus", "[0]*TMath::Gaus(x,[1],[2])", -12, 12);
    fit_gaus->SetParLimits(0, 0, 20. * all_spills);
    fit_gaus->SetParLimits(1, -3, 3);
    fit_gaus->SetParLimits(2, 1e-2, 1e+1);

    // -------------------------------------------------------------------------
    // Spill loop helper — passes the spill index to the action so callers
    // can route fills into per-spill containers.
    // -------------------------------------------------------------------------
    auto spill_loop = [&](auto action)
    {
        for (int i = 0; i < all_spills; ++i)
        {
            lightdata_tree->GetEntry(i);
            spilldata->get_entry();
            action(i, spilldata->get_frame_list_link());
        }
    };

    // -------------------------------------------------------------------------
    // Pass 0 — fine-time calibration scan
    // -------------------------------------------------------------------------
    TH1F *h_chip_means_pass0 = new TH1F(
        "h_timing_delta_chip_means_pass0",
        "Mean chip 0 - Mean chip 1;#Delta t (ns);Entries",
        800, -4.0, 4.0);
    TH2F *h_channel_pass0 = new TH2F(
        "h_timing_delta_channel_pass0",
        "Timing residuals pass 0;Global index;#Delta t (ns)",
        400, 8180.0, 8580.0, 600, -12.0, 12.0);
    TH3F *h_fine_scan_pass0 = new TH3F(
        "h_fine_calib_scan_pass0",
        "Fine calibration scan;Global index;#Delta t (ns);Fine parameter",
        400, 8180.0, 8580.0, 240, -12.0, 12.0, 256, -0.5, 255.5);

    mist::logger::info("Starting pass 0");
    spill_loop([&](int /*spill*/, auto &frames)
               { run_pass0_analysis(frames,
                                    h_chip_means_pass0, h_channel_pass0, h_fine_scan_pass0); });
    mist::logger::info("All done for pass 0");

    TGraphErrors *g_offset = new TGraphErrors();
    TGraphErrors *g_stddev = new TGraphErrors();
    TGraphErrors *g_linpar = new TGraphErrors();

    h_fine_scan_pass0->GetYaxis()->SetRangeUser(-2, 2);
    h_fine_scan_pass0->GetZaxis()->SetRangeUser(10, 120);

    for (int bx = 1; bx <= h_fine_scan_pass0->GetNbinsX(); ++bx)
    {
        if (h_fine_scan_pass0->Integral(bx, bx, 1, -1, 1, -1) == 0)
            continue;

        const int global_idx = static_cast<int>(
            h_fine_scan_pass0->GetXaxis()->GetBinCenter(bx));

        h_fine_scan_pass0->GetXaxis()->SetRange(bx, bx);
        TH2D *h_zy = (TH2D *)h_fine_scan_pass0->Project3D(
            Form("%d_yz", global_idx));

        fit_linear_gaus->SetParameter(0, 10.);
        fit_linear_gaus->SetParameter(1, 0.008);
        fit_linear_gaus->SetParameter(2, 0.0);
        fit_linear_gaus->SetParameter(3, 0.1);
        for (int i = 0; i < 3; ++i)
            h_zy->Fit(fit_linear_gaus, "Q");

        const double conv = fit_linear_gaus->GetParameter(1);
        const double offset = fit_linear_gaus->GetParameter(2);
        const double sigma = fit_linear_gaus->GetParameter(3);

        const int pt = g_offset->GetN();
        g_offset->SetPoint(pt, global_idx, offset);
        g_offset->SetPointError(pt, 0., fit_linear_gaus->GetParError(2));
        g_stddev->SetPoint(pt, global_idx, sigma);
        g_stddev->SetPointError(pt, 0., fit_linear_gaus->GetParError(3));
        g_linpar->SetPoint(pt, global_idx, conv);
        g_linpar->SetPointError(pt, 0., fit_linear_gaus->GetParError(1));

        alcor_finedata::switch_to_fit_v2(
            global_idx, calibration_method_t::_ALCOR_v2_FIT_CALIB_,
            conv, offset, sigma);
        delete h_zy;
    }

    h_fine_scan_pass0->GetXaxis()->SetRange(0, 0);
    h_fine_scan_pass0->GetYaxis()->SetRange(0, 0);
    h_fine_scan_pass0->GetZaxis()->SetRange(0, 0);

    // -------------------------------------------------------------------------
    // Pass 1 — iterative per-channel timing offset (run twice)
    // -------------------------------------------------------------------------
    auto run_calib_timing = [&]()
    {
        TH1F *h_chip_means = new TH1F(
            "h_timing_delta_chip_means_pass1",
            "Mean chip 0 - Mean chip 1;#Delta t (ns);Entries",
            800, -4.0, 4.0);
        TH2F *h_channel = new TH2F(
            "h_timing_delta_channel_pass1",
            "Timing residuals pass 1;Global index;#Delta t (ns)",
            400, 8180.0, 8580.0, 600, -12.0, 12.0);
        TH3F *h_fine_scan = new TH3F(
            "h_fine_calib_scan_pass1",
            "Fine calibration scan pass 1;Global index;#Delta t (ns);Fine parameter",
            400, 8180.0, 8580.0, 240, -12.0, 12.0, 256, -0.5, 255.5);

        spill_loop([&](int /*spill*/, auto &frames)
                   { run_pass0_analysis(frames, h_chip_means, h_channel, h_fine_scan); });

        for (int bx = 1; bx <= h_channel->GetNbinsX(); ++bx)
        {
            if (h_channel->Integral(bx, bx, 1, -1) == 0)
                continue;
            const int global_idx = static_cast<int>(
                h_channel->GetXaxis()->GetBinCenter(bx));

            TH1D *h_y = h_channel->ProjectionY(
                Form("%d_y", global_idx), bx, bx);
            for (int i = 0; i < 3; ++i)
                h_y->Fit(fit_gaus, "Q");

            const double mean = fit_gaus->GetParameter(1);
            const double sigma = fit_gaus->GetParameter(2);
            const double prev = alcor_finedata::get_param1(global_idx);

            alcor_finedata::set_param1(global_idx,
                                       static_cast<float>(prev - mean / _ALCOR_CC_TO_NS_));
            alcor_finedata::set_param2(global_idx,
                                       static_cast<float>(sigma));
            delete h_y;
        }
        delete h_chip_means;
        delete h_channel;
        delete h_fine_scan;
    };

    mist::logger::info("Starting pass 1");
    for (int i = 0; i < 2; ++i)
        run_calib_timing();

    TH1F *h_chip_means_pass2 = new TH1F(
        "h_timing_delta_chip_means_pass2",
        "Mean chip 0 - Mean chip 1 (post timing calib);#Delta t (ns);Entries",
        800, -4.0, 4.0);
    TH2F *h_channel_pass2 = new TH2F(
        "h_timing_delta_channel_pass2",
        "Timing residuals after convergence;Global index;#Delta t (ns)",
        400, 8180.0, 8580.0, 600, -12.0, 12.0);

    spill_loop([&](int /*spill*/, auto &frames)
               { run_pass0_analysis(frames,
                                    h_chip_means_pass2, h_channel_pass2, nullptr, false); });
    mist::logger::info("All done for pass 1");

    // -------------------------------------------------------------------------
    // Self-trigger QA — BEFORE Cherenkov calibration
    //
    // Captures the raw batch-4 TDC offset structure using the cluster mean
    // as internal reference. Compare against h_selfref_after to measure
    // how much calibration collapsed the per-channel spread.
    // -------------------------------------------------------------------------
    TH2F *h_selfref_before = new TH2F(
        "h_selfref_before",
        "Self-trigger #Delta t before Cherenkov calib;Global index;#Delta t (ns)",
        8100, 0, 8100, 300, -12.0, 12.0);

    spill_loop([&](int /*spill*/, auto &frames)
               { run_selftrigger_analysis(frames,
                                          nullptr, nullptr, nullptr,
                                          h_selfref_before); });

    // -------------------------------------------------------------------------
    // Pass 2 — Cherenkov per-channel offset (run twice)
    //
    // Weight hook: replace the uniform lambda with a geometric ring-weight
    // closure once the constrained circle fit is implemented.
    // -------------------------------------------------------------------------
    TH2F *h_cherenkov_delta = new TH2F(
        "h_cherenkov_delta_channel",
        "Cherenkov #Delta t vs channel;Global index;#Delta t (ns)",
        8100, 0, 8100, 300, -12.0, 12.0);

    TH1F *h_tref_quality = new TH1F(
        "h_tref_quality",
        "t_{ref} inter-chip delta;#Delta t (ns);Entries",
        400, -2.0, 2.0);

    TH3F *h_fine_scan_cherenkov_pass0 = new TH3F(
        "h_fine_scan_cherenkov_pass0",
        "Fine calibration scan;Global index;#Delta t (ns);Fine parameter",
        8100, 0, 8100,
        400, -20.0, 20.0,
        120, -20.5, 140.5);

    // Per-spill fine scan: one TH3F per spill, filled in the final diagnostic
    // pass after calibration converges. Useful for checking temporal stability
    // of the fine-parameter vs delta_t correlation across spills.
    std::vector<TH3F *> h_fine_scan_cher_per_spill(all_spills, nullptr);
    for (int i = 0; i < all_spills; ++i)
        h_fine_scan_cher_per_spill[i] = new TH3F(
            Form("h_fine_scan_cherenkov_spill%03d", i),
            Form("Fine scan Cherenkov spill %d;Global index;#Delta t (ns);Fine parameter", i),
            8100, 0, 8100,
            150, -15.0, 15.0,
            120, -20.5, 140.5);

    auto extract_cherenkov_offsets = [&](float min_weighted_entries = 20.f)
    {
        for (int bx = 1; bx <= h_cherenkov_delta->GetNbinsX(); ++bx)
        {
            if (h_cherenkov_delta->Integral(bx, bx, 1, -1) < min_weighted_entries)
                continue;

            const int global_idx = static_cast<int>(
                h_cherenkov_delta->GetXaxis()->GetBinCenter(bx));

            TH1D *h_y = h_cherenkov_delta->ProjectionY(
                Form("cher_py_%d", global_idx), bx, bx);
            h_y->SetDirectory(nullptr);

            // Sparse channels: coarsen bins for fit stability.
            // Dense channels: keep full resolution.
            if (h_y->GetEffectiveEntries() < 150.)
                h_y->Rebin(4);

            // Mode computed on the (possibly rebinned) histogram
            const double mode = windowed_mode(h_y, 0.5);
            double mean = mode;

            if (h_y->GetEffectiveEntries() >= 50.)
            {
                fit_gaus->SetRange(mode - 1.5, mode + 1.5);
                fit_gaus->SetParLimits(1, mode - 1.5, mode + 1.5);
                fit_gaus->SetParameter(1, mode);
                for (int i = 0; i < 2; ++i)
                    h_y->Fit(fit_gaus, "QR");

                const double fit_mean = fit_gaus->GetParameter(1);
                const double fit_sigma = fit_gaus->GetParameter(2);

                // Sanity: reject diverged fits
                if (fit_sigma < 2.0 && std::fabs(fit_mean - mode) < 1.0)
                    mean = fit_mean;
            }

            const float prev = alcor_finedata::get_param2(global_idx);
            alcor_finedata::set_param2(global_idx,
                                       prev + static_cast<float>(-mean / _ALCOR_CC_TO_NS_));
            delete h_y;
        }
    };

    auto run_calib_cherenkov = [&]()
    {
        h_cherenkov_delta->Reset();
        spill_loop([&](int /*spill*/, auto &frames)
                   { run_pass1_analysis(frames,
                                        h_tref_quality,
                                        h_cherenkov_delta,
                                        nullptr,
                                        false, // sigma_weight off
                                        [](const alcor_finedata &)
                                        { return 1.f; } // swap for ring-weight lambda
                     ); });
        extract_cherenkov_offsets();
    };

    // -------------------------------------------------------------------------
    // Self-trigger histograms — declared here so before/after pass 2 is
    // possible.  h_selfref_before is filled above; h_selfref_after after pass 2.
    // -------------------------------------------------------------------------
    TH1F *h_st_multiplicity = new TH1F(
        "h_selftrigger_multiplicity",
        "Self-trigger cluster multiplicity;N hits;Entries",
        60, 0, 60);
    TH1F *h_st_sigma = new TH1F(
        "h_selftrigger_sigma",
        "Self-trigger cluster time RMS;#sigma (ns);Entries",
        200, 0, 10);
    TH2F *h_st_sigma_vs_mult = new TH2F(
        "h_selftrigger_sigma_vs_mult",
        "Cluster time RMS vs multiplicity;N hits;#sigma (ns)",
        60, 0, 60, 200, 0, 10);
    TH2F *h_selfref_after = new TH2F(
        "h_selfref_after",
        "Self-trigger #Delta t after Cherenkov calib;Global index;#Delta t (ns)",
        8100, 0, 8100, 300, -12.0, 12.0);

    mist::logger::info("Starting pass 2");
    for (int i = 0; i < 2; ++i)
        run_calib_cherenkov();
    mist::logger::info("All done for pass 2");

    TH1F *h_chip_means_pass3 = new TH1F(
        "h_timing_delta_chip_means_pass3",
        "Mean chip 0 - Mean chip 1 (post-Cherenkov);#Delta t (ns);Entries",
        800, -4.0, 4.0);
    TH2F *h_cherenkov_final = new TH2F(
        "h_cherenkov_delta_final",
        "Cherenkov #Delta t after calibration;Global index;#Delta t (ns)",
        8100, 0, 8100, 300, -12.0, 12.0);

    // Final diagnostic pass: fills global summary + per-spill fine scans.
    spill_loop([&](int spill, auto &frames)
               {
                   run_pass1_analysis(frames,
                                      h_chip_means_pass3,
                                      h_cherenkov_final,
                                      h_fine_scan_cher_per_spill[spill],
                                      false);
                   run_pass1_analysis(frames,
                                      nullptr,
                                      nullptr,
                                      h_fine_scan_cherenkov_pass0,
                                      false);
               });

    // Self-trigger AFTER Cherenkov calibration
    spill_loop([&](int /*spill*/, auto &frames)
               { run_selftrigger_analysis(frames,
                                          h_st_multiplicity, h_st_sigma, h_st_sigma_vs_mult,
                                          h_selfref_after); });

    // -------------------------------------------------------------------------
    // Persist calibration and write output
    // -------------------------------------------------------------------------
    alcor_finedata::write_calib_to_file(
        data_repository + "/" + run_name + "/timing_fine_calib.txt");

    auto draw = [](TObject *obj, const char *opt = "")
    {
        new TCanvas();
        obj->Draw(opt);
    };
    draw(g_offset, "ALP");
    draw(g_stddev, "ALP");
    draw(g_linpar, "ALP");
    draw(h_channel_pass0);
    draw(h_channel_pass2);
    draw(h_cherenkov_delta);
    draw(h_cherenkov_final);
    draw(h_selfref_before);
    draw(h_selfref_after);
    draw(h_st_sigma);
    draw(h_st_sigma_vs_mult, "COLZ");

    new TCanvas();
    h_chip_means_pass0->SetLineWidth(2);
    h_chip_means_pass0->SetLineColor(kBlack);
    h_chip_means_pass0->Draw();
    h_chip_means_pass2->SetLineWidth(2);
    h_chip_means_pass2->SetLineColor(kGreen - 1);
    h_chip_means_pass2->Draw("SAME");
    h_chip_means_pass3->SetLineWidth(2);
    h_chip_means_pass3->SetLineColor(kBlue);
    h_chip_means_pass3->Draw("SAME");

    // ── Projected δt: before and after Cherenkov calibration ─────────────────
    TH1D *h_proj_before = (TH1D *)h_cherenkov_delta->ProjectionY(
        "h_cherenkov_delta_proj_before");
    h_proj_before->SetTitle(
        "Cherenkov #Delta t projection;#Delta t (ns);Entries");

    TH1D *h_proj_after = (TH1D *)h_cherenkov_final->ProjectionY(
        "h_cherenkov_delta_proj_after");

    h_proj_before->SetLineWidth(2);
    h_proj_before->SetLineColor(kBlack);
    h_proj_after->SetLineWidth(2);
    h_proj_after->SetLineColor(kGreen - 1);

    new TCanvas();
    h_proj_before->Draw("HIST");
    h_proj_after->Draw("HIST SAME");

    // Add a legend
    TLegend *leg_proj = new TLegend(0.65, 0.70, 0.88, 0.88);
    leg_proj->SetBorderSize(0);
    leg_proj->AddEntry(h_proj_before, "Before calib", "l");
    leg_proj->AddEntry(h_proj_after, "After calib", "l");
    leg_proj->Draw();

    for (TObject *obj : {
             (TObject *)h_chip_means_pass0,
             (TObject *)h_chip_means_pass2,
             (TObject *)h_chip_means_pass3,
             (TObject *)h_channel_pass0,
             (TObject *)h_channel_pass2,
             (TObject *)h_fine_scan_pass0,
             (TObject *)h_cherenkov_delta,
             (TObject *)h_cherenkov_final,
             (TObject *)h_tref_quality,
             (TObject *)h_selfref_before,
             (TObject *)h_selfref_after,
             (TObject *)h_st_multiplicity,
             (TObject *)h_st_sigma,
             (TObject *)h_st_sigma_vs_mult,
             (TObject *)g_offset,
             (TObject *)g_stddev,
             (TObject *)g_linpar,
             (TObject *)h_proj_before,
             (TObject *)h_proj_after,
             (TObject *)h_fine_scan_cherenkov_pass0})
        obj->Write();

    for (int i = 0; i < all_spills; ++i)
        h_fine_scan_cher_per_spill[i]->Write();

    output_file->Close();
}