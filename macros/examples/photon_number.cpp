#include "../lib_loader.h"
#include "utility/root_io.h"
#include "utility/root_hist.h"

// ─── Global analysis parameters ──────────────────────────────────────────────

static constexpr int kCoverageGranularity = 10;
static constexpr std::array<float, 2> kTimeCutNs = {-40.f, 40.f};
static const std::vector<std::array<float, 2>> kPhiGapRanges = {
    {-2.6f, -1.9f}, {2.4f, 2.95f}, {-1.65f, -1.1f}};

// ── Radial histogram binning ──────────────────────────────────────────────────
static constexpr int kRBins = 40;
static constexpr float kRLo = 25.f;
static constexpr float kRHi = 125.f;
static constexpr float kFitLo = 30.f; // fit start — avoids low-R structure
static constexpr float kFitHi = 80.f; // fit high limit — truncates high-R tail

enum class SensorType
{
    k1350,
    k1375,
    kUnknown
};
static SensorType sensor_type(int device)
{
    if (device >= 192 && device <= 195)
        return SensorType::k1350;
    if (device >= 196 && device <= 199)
        return SensorType::k1375;
    return SensorType::kUnknown;
}

// ─── radial_efficiency ───────────────────────────────────────────────────────
TH1F *radial_efficiency(TH2F *h_rphi, const TAxis *ref_axis,
                        const std::vector<std::array<float, 2>> &ranges,
                        bool inclusive = true)
{
    int n_out = ref_axis->GetNbins();
    double r_lo = ref_axis->GetXmin();
    double r_hi = ref_axis->GetXmax();
    static int eff_counter = 0;
    auto *h = new TH1F(TString::Format("h_eff_tmp_%d", eff_counter++).Data(),
                       h_rphi->GetName(), n_out, r_lo, r_hi);

    std::vector<std::array<float, 2>> active_ranges = ranges;
    if (!inclusive)
    {
        active_ranges.clear();
        float prev = -TMath::Pi();
        for (auto &r : ranges)
        {
            active_ranges.push_back({prev, r[0]});
            prev = r[1];
        }
        active_ranges.push_back({prev, (float)TMath::Pi()});
    }

    int n_phi = h_rphi->GetNbinsX();
    int n_r = h_rphi->GetNbinsY();
    int n_phi_selected = 0;
    for (int iphi = 1; iphi <= n_phi; ++iphi)
    {
        float phi = h_rphi->GetXaxis()->GetBinCenter(iphi);
        for (auto &r : active_ranges)
            if (phi > r[0] && phi < r[1])
            {
                ++n_phi_selected;
                break;
            }
    }
    if (n_phi_selected == 0)
        return h;

    std::vector<int> fine_count(n_out + 2, 0);
    for (int ir = 1; ir <= n_r; ++ir)
    {
        float r_center = h_rphi->GetYaxis()->GetBinCenter(ir);
        int iout = h->GetXaxis()->FindBin(r_center);
        if (iout < 1 || iout > n_out)
            continue;
        double sum = 0.;
        for (int iphi = 1; iphi <= n_phi; ++iphi)
        {
            float phi = h_rphi->GetXaxis()->GetBinCenter(iphi);
            for (auto &r : active_ranges)
                if (phi > r[0] && phi < r[1])
                {
                    sum += h_rphi->GetBinContent(iphi, ir);
                    break;
                }
        }
        h->AddBinContent(iout, sum);
        ++fine_count[iout];
    }
    for (int iout = 1; iout <= n_out; ++iout)
        if (fine_count[iout] > 0)
            h->SetBinContent(iout, h->GetBinContent(iout) / fine_count[iout]);
    h->Scale(1. / n_phi_selected);
    return h;
}

float phi_extrapolation_scale(const std::vector<std::array<float, 2>> &ranges,
                              bool inclusive = true)
{
    float gap_phi = 0.f;
    for (auto &r : ranges)
        gap_phi += r[1] - r[0];
    float selected_phi = inclusive ? gap_phi : (2.f * (float)TMath::Pi() - gap_phi);
    return 2.f * (float)TMath::Pi() / selected_phi;
}

// ─── Main ────────────────────────────────────────────────────────────────────
// ─── Fit result struct ───────────────────────────────────────────────────────
// Filled by photon_number() for each fitted histogram.
// Key format: "<hist_name>.<quantity>", e.g. "h_r_ex_gap.N_gamma"
// Quantities: N_gamma, N_gamma_err, mu, sigma, gs_frac, chi2_ndf
using FitResultMap = std::map<std::string, double>;
FitResultMap dummy_map;

static void store_result(FitResultMap &out, const std::string &hname,
                         double N_gamma, double N_gamma_err,
                         double mu, double sigma,
                         double gs_frac, double chi2_ndf)
{
    out[hname + ".N_gamma"] = N_gamma;
    out[hname + ".N_gamma_err"] = N_gamma_err;
    out[hname + ".mu"] = mu;
    out[hname + ".sigma"] = sigma;
    out[hname + ".gs_frac"] = gs_frac;
    out[hname + ".chi2_ndf"] = chi2_ndf;
}

void photon_number(std::string data_repository, std::string run_name,
                   FitResultMap &fit_results = dummy_map,
                   int max_frames = 10000000)
{
    std::string fname = data_repository + "/" + run_name + "/recodata.root";
    TFilePtr f_in(TFile::Open(fname.c_str(), "READ"));
    if (!f_in || f_in->IsZombie())
    {
        std::cerr << "[ERROR] Cannot open " << fname << "\n";
        return;
    }
    TTree *tree = (TTree *)f_in->Get("recodata");
    AlcorRecodata *reco = new AlcorRecodata();
    reco->link_to_tree(tree);
    int all_frames = (int)std::min((Long64_t)max_frames, tree->GetEntries());

    // ── Pass 0 ───────────────────────────────────────────────────────────────
    std::vector<int> spill_of_frame(all_frames, -1);
    std::vector<int> spill_trigger_count, spill_dcr_count;
    int current_spill = -1, total_used_frames_prescan = 0, total_dcr_frames_prescan = 0;

    for (int iframe = 0; iframe < all_frames; ++iframe)
    {
        tree->GetEntry(iframe);
        if (reco->is_start_of_spill())
        {
            ++current_spill;
            spill_trigger_count.push_back(0);
            spill_dcr_count.push_back(0);
            spill_of_frame[iframe] = current_spill;
            continue;
        }
        if (current_spill < 0)
            continue;
        spill_of_frame[iframe] = current_spill;
        if (reco->get_trigger_by_index(104))
        {
            ++spill_trigger_count[current_spill];
            ++total_used_frames_prescan;
        }
        else if (reco->get_trigger_by_index(100))
        {
            ++spill_dcr_count[current_spill];
            ++total_dcr_frames_prescan;
        }
    }
    const int n_spills = (int)spill_trigger_count.size();
    const float total_used_frames_f = (float)total_used_frames_prescan;
    const float total_dcr_frames_f = (float)total_dcr_frames_prescan;
    std::cout << "[Pass 0] " << n_spills << " spills, "
              << total_used_frames_prescan << " physics triggers, "
              << total_dcr_frames_prescan << " DCR frames\n";

    // ── Histograms ────────────────────────────────────────────────────────────
    RootHist<TH1F> h_fit_x("h_fit_x", ";circle center x (mm)", 240, -30, 30);
    RootHist<TH1F> h_fit_y("h_fit_y", ";circle center y (mm)", 240, -30, 30);
    RootHist<TH1F> h_fit_r("h_fit_r", ";circle radius (mm)", 400, 30, 130);
    RootHist<TH1F> h_dt("h_dt", ";t_{Hit}-t_{trig} (ns)", 200, -312.5, 312.5);

    RootHist<TH2F> h_xy_hits("h_xy_hits", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_rphi_hits("h_rphi_hits", ";#phi (rad);R (mm)",
                               400, -TMath::Pi(), TMath::Pi(), 75, 25, 125);

    RootHist<TH1F> h_r_full("h_r_full", ";R (mm)", kRBins, kRLo, kRHi);
    RootHist<TH1F> h_r_in_gap("h_r_in_gap", ";R (mm)", kRBins, kRLo, kRHi);
    RootHist<TH1F> h_r_ex_gap("h_r_ex_gap", ";R (mm)", kRBins, kRLo, kRHi);
    RootHist<TH1F> h_r_ex_gap_1350("h_r_ex_gap_1350", ";R (mm)", kRBins, kRLo, kRHi);
    RootHist<TH1F> h_r_ex_gap_1375("h_r_ex_gap_1375", ";R (mm)", kRBins, kRLo, kRHi);

    RootHist<TH1F> h_r_full_dcr("h_r_full_dcr", ";R (mm)", kRBins, kRLo, kRHi);
    RootHist<TH1F> h_r_in_gap_dcr("h_r_in_gap_dcr", ";R (mm)", kRBins, kRLo, kRHi);
    RootHist<TH1F> h_r_ex_gap_dcr("h_r_ex_gap_dcr", ";R (mm)", kRBins, kRLo, kRHi);
    RootHist<TH1F> h_r_ex_gap_1350_dcr("h_r_ex_gap_1350_dcr", ";R (mm)", kRBins, kRLo, kRHi);
    RootHist<TH1F> h_r_ex_gap_1375_dcr("h_r_ex_gap_1375_dcr", ";R (mm)", kRBins, kRLo, kRHi);

    static constexpr int kRFineBins = 100 * kCoverageGranularity;
    RootHist<TH2F> h_xy_cov("h_xy_cov", ";x (mm);y (mm)",
                            396 * kCoverageGranularity, -99, 99,
                            396 * kCoverageGranularity, -99, 99);
    RootHist<TH2F> h_rphi_cov("h_rphi_cov", ";#phi (rad);R (mm)",
                              400 * kCoverageGranularity, -TMath::Pi(), TMath::Pi(),
                              kRFineBins, kRLo, kRHi);
    RootHist<TH2F> h_rphi_cov_1350("h_rphi_cov_1350", ";#phi (rad);R (mm)",
                                   400 * kCoverageGranularity, -TMath::Pi(), TMath::Pi(),
                                   kRFineBins, kRLo, kRHi);
    RootHist<TH2F> h_rphi_cov_1375("h_rphi_cov_1375", ";#phi (rad);R (mm)",
                                   400 * kCoverageGranularity, -TMath::Pi(), TMath::Pi(),
                                   kRFineBins, kRLo, kRHi);

    // ── Pass 1: circle fits ───────────────────────────────────────────────────
    for (int iframe = 0; iframe < all_frames; ++iframe)
    {
        tree->GetEntry(iframe);
        if (reco->is_start_of_spill())
            continue;
        auto trig = reco->get_trigger_by_index(104);
        if (!trig)
            continue;
        std::vector<std::array<float, 2>> pts;
        float r_sum = 0.f;
        for (int ih = 0; ih < (int)reco->get_recodata().size(); ++ih)
        {
            if (reco->is_afterpulse(ih))
                continue;
            float dt = reco->get_hit_t(ih) - trig->fine_time;
            h_dt->Fill(dt);
            if (dt < kTimeCutNs[0] || dt > kTimeCutNs[1])
                continue;
            pts.push_back({reco->get_hit_x(ih), reco->get_hit_y(ih)});
            r_sum += reco->get_hit_r(ih);
        }
        if ((int)pts.size() > 4)
        {
            float r0 = r_sum / pts.size();
            auto res = fit_circle(pts, {0.f, 0.f, r0}, false, {{}});
            h_fit_x->Fill(res[0][0]);
            h_fit_y->Fill(res[1][0]);
            h_fit_r->Fill(res[2][0]);
        }
    }

    std::array<float, 3> G = {};
    auto find_peak = [&](TH1F *h) -> float
    {
        // Find the bin with the maximum content and return its center
        int max_bin = h->GetMaximumBin();
        return h->GetBinCenter(max_bin);
    };
    G[0] = find_peak(h_fit_x);
    G[1] = find_peak(h_fit_y);
    G[2] = find_peak(h_fit_r);

    // ── Pass 2: coverage + hits ───────────────────────────────────────────────
    std::map<int, std::array<std::vector<std::array<int, 2>>, 2>> ch_bins_cache;
    int used_frames = 0;

    for (int iframe = 0; iframe < all_frames; ++iframe)
    {
        tree->GetEntry(iframe);
        int ispill = spill_of_frame[iframe];
        if (ispill < 0)
            continue;

        if (reco->is_start_of_spill())
        {
            if (spill_trigger_count[ispill] == 0)
                continue;
            const float w = (float)spill_trigger_count[ispill] / total_used_frames_f;
            for (int ih = 0; ih < (int)reco->get_recodata().size(); ++ih)
            {
                int gidx = reco->get_global_index(ih);
                float cx = reco->get_hit_x(ih);
                float cy = reco->get_hit_y(ih);
                SensorType st = sensor_type(reco->get_device(ih));
                TH2F *h_cov_st = (st == SensorType::k1350)   ? h_rphi_cov_1350
                                 : (st == SensorType::k1375) ? h_rphi_cov_1375
                                                             : nullptr;

                auto it = ch_bins_cache.find(gidx);
                if (it != ch_bins_cache.end())
                {
                    for (auto &b : it->second[0])
                        h_xy_cov->AddBinContent(h_xy_cov->GetBin(b[0], b[1]), w);
                    for (auto &b : it->second[1])
                    {
                        h_rphi_cov->AddBinContent(h_rphi_cov->GetBin(b[0], b[1]), w);
                        if (h_cov_st)
                            h_cov_st->AddBinContent(h_cov_st->GetBin(b[0], b[1]), w);
                    }
                    continue;
                }
                auto &bins_xy = ch_bins_cache[gidx][0];
                auto &bins_rphi = ch_bins_cache[gidx][1];
                int bxlo = h_xy_cov->GetXaxis()->FindBin(cx - 1.5f), bxhi = h_xy_cov->GetXaxis()->FindBin(cx + 1.5f);
                int bylo = h_xy_cov->GetYaxis()->FindBin(cy - 1.5f), byhi = h_xy_cov->GetYaxis()->FindBin(cy + 1.5f);
                for (int bx = bxlo; bx <= bxhi; ++bx)
                    for (int by = bylo; by <= byhi; ++by)
                    {
                        h_xy_cov->AddBinContent(h_xy_cov->GetBin(bx, by), w);
                        bins_xy.push_back({bx, by});
                    }

                float cr = std::hypot(cx, cy), cphi = std::atan2(cy, cx);
                float dphi = 1.5f * sqrtf(2.f) / cr, dr = 1.5f * sqrtf(2.f);
                int brlo = h_rphi_cov->GetYaxis()->FindBin(cr - dr), brhi = h_rphi_cov->GetYaxis()->FindBin(cr + dr);
                int bplo = h_rphi_cov->GetXaxis()->FindBin(cphi - dphi), bphi_ = h_rphi_cov->GetXaxis()->FindBin(cphi + dphi);
                for (int bp = bplo; bp <= bphi_; ++bp)
                {
                    float phi_c = h_rphi_cov->GetXaxis()->GetBinCenter(bp);
                    for (int br = brlo; br <= brhi; ++br)
                    {
                        float r_c = h_rphi_cov->GetYaxis()->GetBinCenter(br);
                        if (std::fabs(r_c * cosf(phi_c) - cx) > 1.5f || std::fabs(r_c * sinf(phi_c) - cy) > 1.5f)
                            continue;
                        if (std::find(bins_rphi.begin(), bins_rphi.end(), std::array<int, 2>{bp, br}) != bins_rphi.end())
                            continue;
                        h_rphi_cov->AddBinContent(h_rphi_cov->GetBin(bp, br), w);
                        bins_rphi.push_back({bp, br});
                    }
                }
                if (h_cov_st)
                    for (auto &b : bins_rphi)
                        h_cov_st->AddBinContent(h_cov_st->GetBin(b[0], b[1]), w);
            }
            continue;
        }

        auto trig = reco->get_trigger_by_index(104);
        if (trig)
        {
            ++used_frames;
            for (int ih = 0; ih < (int)reco->get_recodata().size(); ++ih)
            {
                if (reco->is_afterpulse(ih))
                    continue;
                float dt = reco->get_hit_t(ih) - trig->fine_time;
                if (dt < kTimeCutNs[0] || dt > kTimeCutNs[1])
                    continue;
                float phi = reco->get_hit_phi(ih, {G[0], G[1]});
                float r = reco->get_hit_r(ih, {G[0], G[1]});
                h_xy_hits->Fill(reco->get_hit_x_rnd(ih), reco->get_hit_y_rnd(ih));
                h_rphi_hits->Fill(reco->get_hit_phi_rnd(ih, {G[0], G[1]}), reco->get_hit_r_rnd(ih, {G[0], G[1]}));
                h_r_full->Fill(r);
                bool in_gap = false;
                for (auto &range : kPhiGapRanges)
                    if (phi > range[0] && phi < range[1])
                    {
                        in_gap = true;
                        break;
                    }
                (in_gap ? h_r_in_gap : h_r_ex_gap)->Fill(r);
                if (!in_gap)
                {
                    auto st = sensor_type(reco->get_device(ih));
                    if (st == SensorType::k1350)
                        h_r_ex_gap_1350->Fill(r);
                    else if (st == SensorType::k1375)
                        h_r_ex_gap_1375->Fill(r);
                }
            }
            continue;
        }

        auto trig_dcr = reco->get_trigger_by_index(100);
        if (!trig_dcr)
            continue;
        for (int ih = 0; ih < (int)reco->get_recodata().size(); ++ih)
        {
            if (reco->is_afterpulse(ih))
                continue;
            float dt = reco->get_hit_t(ih) - trig_dcr->fine_time;
            if (dt < kTimeCutNs[0] || dt > kTimeCutNs[1])
                continue;
            float phi = reco->get_hit_phi(ih, {G[0], G[1]});
            float r = reco->get_hit_r(ih, {G[0], G[1]});
            h_r_full_dcr->Fill(r);
            bool in_gap = false;
            for (auto &range : kPhiGapRanges)
                if (phi > range[0] && phi < range[1])
                {
                    in_gap = true;
                    break;
                }
            (in_gap ? h_r_in_gap_dcr : h_r_ex_gap_dcr)->Fill(r);
            if (!in_gap)
            {
                auto st = sensor_type(reco->get_device(ih));
                if (st == SensorType::k1350)
                    h_r_ex_gap_1350_dcr->Fill(r);
                else if (st == SensorType::k1375)
                    h_r_ex_gap_1375_dcr->Fill(r);
            }
        }
    }

    if (used_frames != total_used_frames_prescan)
        std::cerr << "[WARN] used_frames mismatch: prescan=" << total_used_frames_prescan
                  << " mainloop=" << used_frames << "\n";

    // ── Acceptance correction ─────────────────────────────────────────────────
    const TAxis *r_axis = h_r_full->GetXaxis();
    auto apply_correction = [&](TH1F *h, TH1F *h_eff, float phi_scale)
    {
        h->Scale(1. / used_frames);
        h->Divide(h_eff);
        h->Scale(1., "width");
        h->Scale(phi_scale);
    };
    // radial_efficiency() scans a 4M-bin TH2 — compute each unique (h_cov,
    // phi_range, inclusive?) tuple ONCE and reuse the result for both the
    // signal correction and the DCR correction below.  Previous code called
    // each helper twice (apply_correction + apply_correction_dcr branches)
    // for ~5 redundant 4M-bin scans (CODE_REVIEW §6.1).
    TH1F *eff_full = radial_efficiency(h_rphi_cov, r_axis, {{-(float)TMath::Pi(), (float)TMath::Pi()}});
    TH1F *eff_in_gap = radial_efficiency(h_rphi_cov, r_axis, kPhiGapRanges, true);
    TH1F *eff_ex_gap = radial_efficiency(h_rphi_cov, r_axis, kPhiGapRanges, false);
    TH1F *eff_1350 = radial_efficiency(h_rphi_cov_1350, r_axis, kPhiGapRanges, false);
    TH1F *eff_1375 = radial_efficiency(h_rphi_cov_1375, r_axis, kPhiGapRanges, false);

    apply_correction(h_r_full, eff_full, 1.f);
    apply_correction(h_r_in_gap, eff_in_gap, phi_extrapolation_scale(kPhiGapRanges, true));
    apply_correction(h_r_ex_gap, eff_ex_gap, phi_extrapolation_scale(kPhiGapRanges, false));
    apply_correction(h_r_ex_gap_1350, eff_1350, phi_extrapolation_scale(kPhiGapRanges, false));
    apply_correction(h_r_ex_gap_1375, eff_1375, phi_extrapolation_scale(kPhiGapRanges, false));

    int used_dcr = total_dcr_frames_prescan;
    auto apply_correction_dcr = [&](TH1F *h, TH1F *h_eff, float phi_scale)
    {
        h->Scale(1. / used_dcr);
        h->Divide(h_eff);
        h->Scale(1., "width");
        h->Scale(phi_scale);
    };
    // Reuse the efficiency histograms computed above — args are identical.
    apply_correction_dcr(h_r_full_dcr, eff_full, 1.f);
    apply_correction_dcr(h_r_in_gap_dcr, eff_in_gap, phi_extrapolation_scale(kPhiGapRanges, true));
    apply_correction_dcr(h_r_ex_gap_dcr, eff_ex_gap, phi_extrapolation_scale(kPhiGapRanges, false));
    apply_correction_dcr(h_r_ex_gap_1350_dcr, eff_1350, phi_extrapolation_scale(kPhiGapRanges, false));
    apply_correction_dcr(h_r_ex_gap_1375_dcr, eff_1375, phi_extrapolation_scale(kPhiGapRanges, false));
    h_r_full->Add(h_r_full_dcr, -1.);
    h_r_in_gap->Add(h_r_in_gap_dcr, -1.);
    h_r_ex_gap->Add(h_r_ex_gap_dcr, -1.);
    h_r_ex_gap_1350->Add(h_r_ex_gap_1350_dcr, -1.);
    h_r_ex_gap_1375->Add(h_r_ex_gap_1375_dcr, -1.);

    // ── Fit model ─────────────────────────────────────────────────────────────
    //
    // Gaussian + broad Gaussian (right shoulder) + pol2 background.
    // Fit range [kFitLo, kFitHi] excludes low-R structure (air Cherenkov, reflections)
    // and truncates the high-R tail. The range is designed to isolate the core aerogel
    // ring signal and avoid biasing sigma with non-signal contributions.
    //
    // The fit integrates from kFitLo to kFitHi for N_gamma. Bins outside this range
    // are drawn but excluded from the fit.
    //
    // Parameters:
    //   p[0]  gaus_amp   — Gaussian amplitude (hits/trigger/mm at peak), forced positive via fabs()
    //   p[1]  mu         — peak position (mm)
    //   p[2]  sigma      — single-photon spatial resolution (mm)
    //   p[3]  gs_amp     — broad Gaussian amplitude (right shoulder), forced positive via fabs()
    //   p[4]  gs_sig     — broad Gaussian sigma (mm)
    //   p[5]  pol2_c0    — background constant
    //   p[6]  pol2_c1    — background slope
    //   p[7]  pol2_c2    — background curvature
    //
    // Both signal amplitudes wrapped in fabs() to guarantee physical (non-negative)
    // components, preventing fitter from exploring unphysical parameter space.

    auto full_model = [](double *x, double *p) -> double
    {
        double xx = x[0];
        // All amplitudes forced positive via fabs() to prevent unphysical solutions
        double gaus = std::fabs(p[0]) * std::exp(-0.5 * std::pow((xx - p[1]) / p[2], 2));
        double gs = std::fabs(p[3]) * std::exp(-0.5 * std::pow((xx - p[1]) / p[4], 2));
        double bkg = p[5] + p[6] * xx + p[7] * xx * xx;
        return gaus + gs + bkg;
    };

    int fit_line_color = kRed;

    auto fit_r_dist = [&](TH1F *h, float fit_hi = kFitHi) // captures fit_results
    {
        float mu0 = G[2];
        float rms = h->GetRMS();
        float sig_seed = std::clamp(rms * 0.5f, 0.5f, 5.0f);
        float pk = h->GetMaximum();

        // Sideband background fit over [kFitLo, fit_hi] with signal masked
        TF1 f_bkg(TString::Format("f_bkg_%s", h->GetName()).Data(), "pol2", kFitLo, fit_hi);
        {
            RootHist<TH1F> h_sb(static_cast<TH1F *>(h->Clone(TString::Format("h_sb_%s", h->GetName()).Data())));
            float mask = 3.5f * sig_seed;
            for (int i = 1; i <= h_sb->GetNbinsX(); ++i)
            {
                double xc = h_sb->GetBinCenter(i);
                if (xc < kFitLo || std::fabs(xc - mu0) < mask)
                {
                    h_sb->SetBinContent(i, 0.);
                    h_sb->SetBinError(i, 1e10);
                }
            }
            f_bkg.SetParameters(h->GetBinContent(h->FindBin(kFitLo + 1.)), 0., 0.);
            h_sb->Fit(&f_bkg, "RQ0");
            delete h_sb;
        }
        double bkg_c0 = f_bkg.GetParameter(0);
        double bkg_c1 = f_bkg.GetParameter(1);
        double bkg_c2 = f_bkg.GetParameter(2);

        TF1 f_model(TString::Format("f_model_%s", h->GetName()).Data(), full_model, kFitLo, fit_hi, 8);
        {
            const char *pnames[8] = {"gaus_amp", "mu", "sigma", "gs_amp", "gs_sig",
                                     "pol2_c0", "pol2_c1", "pol2_c2"};
            for (int i = 0; i < 8; ++i)
                f_model.SetParName(i, pnames[i]);
        }
        // Both amplitudes forced positive via fabs() in model
        f_model.SetParLimits(0, 1e-6, 50.); // gaus_amp (fabs applied in model)
        f_model.SetParLimits(2, 0.3, 3.0);  // sigma
        f_model.SetParLimits(3, 0.0, 1e9);  // gs_amp (fabs applied in model)
        f_model.SetParLimits(4, 4.0, 10.0); // gs_sig > core sigma

        // Pass 1: background fixed, no broad Gaussian
        {
            double pars[8] = {pk, mu0, sig_seed, 0.0, 8.0, bkg_c0, bkg_c1, bkg_c2};
            f_model.SetParameters(pars);
        }
        f_model.FixParameter(3, 0.);
        f_model.FixParameter(5, bkg_c0);
        f_model.FixParameter(6, bkg_c1);
        f_model.FixParameter(7, bkg_c2);
        h->Fit(&f_model, "RQ");

        // Pass 2: release broad Gaussian, background still fixed
        f_model.ReleaseParameter(3);
        h->Fit(&f_model, "RQ");

        // Pass 3: release background, full joint fit
        f_model.ReleaseParameter(5);
        f_model.ReleaseParameter(6);
        f_model.ReleaseParameter(7);
        TFitResultPtr res = h->Fit(&f_model, "RSQE");

        // Draw components
        f_model.SetLineColor(fit_line_color);
        f_model.SetLineWidth(2);
        f_model.SetLineStyle(1);
        f_model.DrawCopy("SAME");

        // Core Gaussian (dashed)
        TF1 f_core(TString::Format("f_core_%s", h->GetName()).Data(), full_model, kFitLo, fit_hi, 8);
        for (int i = 0; i < 8; ++i)
            f_core.SetParameter(i, f_model.GetParameter(i));
        f_core.SetParameter(3, 0.);
        f_core.SetParameter(5, 0.);
        f_core.SetParameter(6, 0.);
        f_core.SetParameter(7, 0.);
        f_core.SetLineColor(fit_line_color);
        f_core.SetLineWidth(1);
        f_core.SetLineStyle(kDashed);
        f_core.DrawCopy("SAME");

        // Broad Gaussian / right shoulder (dotted, orange)
        TF1 f_gs(TString::Format("f_gs_%s", h->GetName()).Data(), full_model, kFitLo, fit_hi, 8);
        for (int i = 0; i < 8; ++i)
            f_gs.SetParameter(i, f_model.GetParameter(i));
        f_gs.SetParameter(0, 0.);
        f_gs.SetParameter(5, 0.);
        f_gs.SetParameter(6, 0.);
        f_gs.SetParameter(7, 0.);
        f_gs.SetLineColor(kOrange + 1);
        f_gs.SetLineWidth(1);
        f_gs.SetLineStyle(kDotted);
        f_gs.DrawCopy("SAME");

        // Background (dash-dot, gray)
        TF1 f_bkg_draw(TString::Format("f_bkgd_%s", h->GetName()).Data(), full_model, kFitLo, fit_hi, 8);
        for (int i = 0; i < 8; ++i)
            f_bkg_draw.SetParameter(i, f_model.GetParameter(i));
        f_bkg_draw.SetParameter(0, 0.);
        f_bkg_draw.SetParameter(3, 0.);
        f_bkg_draw.SetLineColor(kGray + 1);
        f_bkg_draw.SetLineWidth(1);
        f_bkg_draw.SetLineStyle(9);
        f_bkg_draw.DrawCopy("SAME");

        // N_gamma: integrate signal only (zero background) over [kFitLo, fit_hi]
        TF1 f_sig(TString::Format("f_sig_%s", h->GetName()).Data(), full_model, kFitLo, fit_hi, 8);
        for (int i = 0; i < 8; ++i)
            f_sig.SetParameter(i, f_model.GetParameter(i));
        f_sig.SetParameter(5, 0.);
        f_sig.SetParameter(6, 0.);
        f_sig.SetParameter(7, 0.);

        const int kNGL = 500;
        double glX[500], glW[500];
        TF1::CalcGaussLegendreSamplingPoints(kNGL, glX, glW, 1e-10);
        double N_gamma = f_sig.IntegralFast(kNGL, glX, glW, kFitLo, fit_hi);
        // Error on N_gamma: simple propagation from amplitude error
        double frac_err = (f_model.GetParameter(0) > 0)
                              ? f_model.GetParError(0) / f_model.GetParameter(0)
                              : 0.;
        double N_gamma_err = N_gamma * frac_err;
        double chi2_ndf = (res.Get() && res->Ndf() > 0) ? res->Chi2() / res->Ndf() : -1.;

        double gs_frac = f_model.GetParameter(3) / (f_model.GetParameter(0) + 1e-9);
        std::cout << "[" << h->GetName() << "]"
                  << "  N_gamma = " << N_gamma << " +/- " << N_gamma_err
                  << "  mu = " << f_model.GetParameter(1) << " mm"
                  << "  sigma = " << f_model.GetParameter(2) << " mm"
                  << "  gs_frac = " << gs_frac
                  << "  chi2/ndf = " << chi2_ndf
                  << "\n";
        store_result(fit_results, h->GetName(),
                     N_gamma, N_gamma_err,
                     f_model.GetParameter(1), f_model.GetParameter(2),
                     gs_frac, chi2_ndf);
    };

    // ── Drawing ───────────────────────────────────────────────────────────────
    gStyle->SetOptStat(0);
    gStyle->SetOptFit(0);

    TLine *vline = new TLine();
    vline->SetLineWidth(2);
    vline->SetLineColor(kRed);
    vline->SetLineStyle(kDashed);

    TCanvas *c1 = new TCanvas("c_ring_params", "Ring parameters", 1200, 1200);
    c1->Divide(2, 2);
    c1->cd(1);
    h_fit_x->Draw();
    vline->DrawLineNDC(0.1 + (G[0] + 30) / 60. * 0.8, 0.1, 0.1 + (G[0] + 30) / 60. * 0.8, 0.9);
    c1->cd(2);
    h_fit_y->Draw();
    vline->DrawLineNDC(0.1 + (G[1] + 30) / 60. * 0.8, 0.1, 0.1 + (G[1] + 30) / 60. * 0.8, 0.9);
    c1->cd(3);
    h_fit_r->Draw();
    vline->DrawLineNDC(0.1 + (G[2] - 30) / 100. * 0.8, 0.1, 0.1 + (G[2] - 30) / 100. * 0.8, 0.9);
    c1->cd(4);
    h_dt->Scale(1. / used_frames);
    h_dt->Draw();

    TCanvas *c2 = new TCanvas("c_hit_maps", "Hit maps", 1200, 600);
    c2->Divide(2, 1);
    c2->cd(1);
    h_xy_hits->Scale(1. / used_frames);
    h_xy_hits->Draw("COLZ");
    c2->cd(2);
    h_rphi_hits->Scale(1. / used_frames);
    h_rphi_hits->Draw("COLZ");
    for (auto &range : kPhiGapRanges)
    {
        vline->DrawLine(range[0], kRLo, range[0], kRHi);
        vline->DrawLine(range[1], kRLo, range[1], kRHi);
    }

    // Draw a vertical line at kFitLo to show where the fit starts
    TLine *fit_start = new TLine(kFitLo, 0, kFitLo, 1e9);
    fit_start->SetLineColor(kBlue);
    fit_start->SetLineStyle(kDotted);
    fit_start->SetLineWidth(2);

    TCanvas *c3 = new TCanvas("c_r_distributions", "Corrected radial distributions", 1800, 600);
    c3->Divide(3, 1);
    c3->cd(1);
    h_r_full->Draw();
    fit_start->DrawLine(kFitLo, gPad->GetUymin(), kFitLo, gPad->GetUymax());
    fit_r_dist(h_r_full);
    c3->cd(2);
    h_r_in_gap->Draw();
    fit_start->DrawLine(kFitLo, gPad->GetUymin(), kFitLo, gPad->GetUymax());
    fit_r_dist(h_r_in_gap);
    c3->cd(3);
    h_r_ex_gap->Draw();
    fit_start->DrawLine(kFitLo, gPad->GetUymin(), kFitLo, gPad->GetUymax());
    fit_r_dist(h_r_ex_gap);

    TCanvas *c5 = new TCanvas("c_r_by_sensor", "By sensor type", 1200, 600);
    c5->Divide(2, 1);
    c5->cd(1);
    h_r_ex_gap_1350->SetLineColor(kBlue);
    h_r_ex_gap_1350->SetLineWidth(2);
    h_r_ex_gap_1375->SetLineColor(kRed);
    h_r_ex_gap_1375->SetLineWidth(2);
    auto *frame_sensor = (TH1F *)h_r_ex_gap_1350->Clone("frame_sensor");
    frame_sensor->SetMaximum(1.2 * std::max(h_r_ex_gap_1350->GetMaximum(), h_r_ex_gap_1375->GetMaximum()));
    frame_sensor->Draw("AXIS");
    h_r_ex_gap_1350->Draw("SAME HIST");
    h_r_ex_gap_1375->Draw("SAME HIST");
    fit_start->DrawLine(kFitLo, frame_sensor->GetMinimum(), kFitLo, frame_sensor->GetMaximum());
    fit_line_color = kBlue;
    fit_r_dist(h_r_ex_gap_1350);
    fit_line_color = kRed;
    fit_r_dist(h_r_ex_gap_1375);
    auto *leg = new TLegend(0.6, 0.7, 0.88, 0.88);
    leg->AddEntry(h_r_ex_gap_1350, "S13360-1350 (dev 192-195)", "l");
    leg->AddEntry(h_r_ex_gap_1375, "S13360-1375 (dev 196-199)", "l");
    leg->Draw();
    c5->cd(2);
    RootHist<TH1F> h_ratio(static_cast<TH1F *>(h_r_ex_gap_1375->Clone("h_sensor_ratio")));
    h_ratio->Divide(h_r_ex_gap_1350);
    h_ratio->SetTitle(";R (mm);yield ratio 1375/1350");
    h_ratio->SetLineColor(kBlack);
    h_ratio->SetLineWidth(2);
    h_ratio->Draw("HIST");
    TLine *unity = new TLine(kRLo, 1, kRHi, 1);
    unity->SetLineStyle(kDashed);
    unity->SetLineColor(kGray + 1);
    unity->Draw();

    TCanvas *c4 = new TCanvas("c_coverage", "Coverage maps", 1200, 600);
    c4->Divide(2, 1);
    c4->cd(1);
    h_xy_cov->Draw("COLZ");
    c4->cd(2);
    h_rphi_cov->Draw("COLZ");

    // ── Persist fit results to standard result store ───────────────────────
    //
    // Histogram names are mapped to (sensor, scope) key pairs.
    // "all" sensor covers the full phi-gap-excluded or included distributions;
    // "1350"/"1375" cover the per-SiPM-model splits.
    // Only histograms that were actually fitted are persisted.
    {
        // Map: histogram name → {sensor tag, quantity scope prefix}
        // Scope prefix becomes the left side of the dot in "ex_gap.n_gamma" etc.
        const std::vector<std::tuple<std::string, std::string, std::string>> kHistMap = {
            {"h_r_full", "all", "full"},
            {"h_r_in_gap", "all", "in_gap"},
            {"h_r_ex_gap", "all", "ex_gap"},
            {"h_r_ex_gap_1350", "1350", "ex_gap"},
            {"h_r_ex_gap_1375", "1375", "ex_gap"},
        };

        // Quantities stored per histogram (must match store_result() keys)
        const std::vector<std::pair<std::string, std::string>> kQtyMap = {
            {"N_gamma", "n_gamma"},
            {"N_gamma_err", "n_gamma_err"}, // stored as value, error=0
            {"mu", "mu"},
            {"sigma", "sigma"},
            {"gs_frac", "gs_frac"},
            {"chi2_ndf", "chi2_ndf"},
        };

        AnalysisResults ar(data_repository + "/standard_results.toml");
        ResultMap to_store;

        for (const auto &[hname, sensor, scope] : kHistMap)
        {
            // N_gamma and its error are stored together as value+error on one key
            const std::string key_ng = hname + ".N_gamma";
            const std::string key_ng_err = hname + ".N_gamma_err";
            if (fit_results.count(key_ng))
            {
                double ng = fit_results.at(key_ng);
                double ng_err = fit_results.count(key_ng_err)
                                    ? fit_results.at(key_ng_err)
                                    : 0.;
                to_store[{run_name, sensor, scope + ".n_gamma"}] = {ng, ng_err};
            }

            // Remaining scalar quantities (no paired error)
            for (const auto &[fit_suffix, store_qty] : kQtyMap)
            {
                if (store_qty == "n_gamma" || store_qty == "n_gamma_err")
                    continue; // handled above
                const std::string fkey = hname + "." + fit_suffix;
                if (fit_results.count(fkey))
                    to_store[{run_name, sensor, scope + "." + store_qty}] = {fit_results.at(fkey), 0.};
            }
        }

        ar.update(to_store);
        mist::logger::info("[photon_number] Persisted " +
                           std::to_string(to_store.size()) +
                           " results for run " + run_name);
    }
}