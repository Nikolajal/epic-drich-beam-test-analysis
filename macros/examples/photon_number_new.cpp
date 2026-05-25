#include "../lib_loader.h"
#include "util/root_io.h"
#include "util/root_hist.h"

// ═══════════════════════════════════════════════════════════════════════════════
// Global analysis parameters
// ═══════════════════════════════════════════════════════════════════════════════

static constexpr int kCoverageGranularity = 10;

// Prompt time window around the trigger [ns]
static constexpr std::array<float, 2> kPromptTimeCutNs = {-1.f, +1.f};

// Early-particle time window [ns] — satellite population ~1.8 ns before prompt
static constexpr std::array<float, 2> kEarlyTimeCutNs = {-3.0f, -1.0f};

// φ-gap ranges to exclude (PDU boundaries / dead regions)
static const std::vector<std::array<float, 2>> kPhiGapRanges = {
    {-2.6f, -1.9f}, {2.4f, 2.95f}, {-1.65f, -1.1f}, {-0.1f, 0.15f}, {0.50f, 1.25f}};

// ── Radial histogram binning ─────────────────────────────────────────────────
static constexpr int kRadialBins = 80;
static constexpr float kRadialLoMm = 25.f;
static constexpr float kRadialHiMm = 125.f;
static constexpr float kFitRangeLoMm = 30.f;  // fit start — avoids low-R structure
static constexpr float kFitRangeHiMm = 105.f; // fit end   — avoids sparse high-R bins

// ── Hit-pair histogram binning ───────────────────────────────────────────────
static constexpr float kPairTimeCutNs = 5.f;         // |dt_i| pre-selection [ns]
static constexpr float kPairSeparationMaxMm = 150.f; // maximum pair distance [mm]
static constexpr int kPairSeparationBins = 50;       // 3 mm/bin
static constexpr float kPairDeltaTimeMaxNs = 5.f;    // timing axis range [ns]
static constexpr int kPairDeltaTimeBins = 200;       // 50 ps/bin

// ═══════════════════════════════════════════════════════════════════════════════
// SensorType — maps device index to HPK model
// ═══════════════════════════════════════════════════════════════════════════════

enum class SensorType
{
    k1350,
    k1375,
    kUnknown
};

static SensorType sensor_type(int device_index)
{
    if (device_index >= 192 && device_index <= 195)
        return SensorType::k1350;
    if (device_index >= 196 && device_index <= 199)
        return SensorType::k1375;
    return SensorType::kUnknown;
}

// ═══════════════════════════════════════════════════════════════════════════════
// radial_efficiency
//
// Computes the mean fractional coverage as a function of R by averaging the
// rphi_coverage_map over the phi bins that fall inside (inclusive=true) or
// outside (inclusive=false) the supplied phi ranges.
// Returns a TH1F with the same R binning as radial_reference_axis.
// ═══════════════════════════════════════════════════════════════════════════════

TH1F *radial_efficiency(TH2F *rphi_coverage_map,
                        const TAxis *radial_reference_axis,
                        const std::vector<std::array<float, 2>> &phi_ranges,
                        bool inclusive = true)
{
    static int efficiency_histogram_counter = 0;
    auto *efficiency_histogram = new TH1F(
        TString::Format("h_efficiency_tmp_%d", efficiency_histogram_counter++).Data(),
        rphi_coverage_map->GetName(),
        radial_reference_axis->GetNbins(),
        radial_reference_axis->GetXmin(),
        radial_reference_axis->GetXmax());

    std::vector<std::array<float, 2>> active_phi_ranges = phi_ranges;
    if (!inclusive)
    {
        active_phi_ranges.clear();
        float range_left_edge = -TMath::Pi();
        for (auto &gap_range : phi_ranges)
        {
            active_phi_ranges.push_back({range_left_edge, gap_range[0]});
            range_left_edge = gap_range[1];
        }
        active_phi_ranges.push_back({range_left_edge, (float)TMath::Pi()});
    }

    const int n_phi_bins = rphi_coverage_map->GetNbinsX();
    const int n_r_bins = rphi_coverage_map->GetNbinsY();
    int n_selected_phi_bins = 0;
    for (int iphi = 1; iphi <= n_phi_bins; ++iphi)
    {
        float bin_phi_center = rphi_coverage_map->GetXaxis()->GetBinCenter(iphi);
        for (auto &active_range : active_phi_ranges)
            if (bin_phi_center > active_range[0] && bin_phi_center < active_range[1])
            {
                ++n_selected_phi_bins;
                break;
            }
    }
    if (n_selected_phi_bins == 0)
        return efficiency_histogram;

    std::vector<int> fine_bin_count(radial_reference_axis->GetNbins() + 2, 0);
    for (int ir = 1; ir <= n_r_bins; ++ir)
    {
        float bin_r_center = rphi_coverage_map->GetYaxis()->GetBinCenter(ir);
        int output_bin = efficiency_histogram->GetXaxis()->FindBin(bin_r_center);
        if (output_bin < 1 || output_bin > radial_reference_axis->GetNbins())
            continue;
        double coverage_sum = 0.;
        for (int iphi = 1; iphi <= n_phi_bins; ++iphi)
        {
            float bin_phi_center = rphi_coverage_map->GetXaxis()->GetBinCenter(iphi);
            for (auto &active_range : active_phi_ranges)
                if (bin_phi_center > active_range[0] && bin_phi_center < active_range[1])
                {
                    coverage_sum += rphi_coverage_map->GetBinContent(iphi, ir);
                    break;
                }
        }
        efficiency_histogram->AddBinContent(output_bin, coverage_sum);
        ++fine_bin_count[output_bin];
    }
    for (int iout = 1; iout <= radial_reference_axis->GetNbins(); ++iout)
        if (fine_bin_count[iout] > 0)
            efficiency_histogram->SetBinContent(
                iout, efficiency_histogram->GetBinContent(iout) / fine_bin_count[iout]);
    efficiency_histogram->Scale(1. / n_selected_phi_bins);
    return efficiency_histogram;
}

float phi_extrapolation_scale(const std::vector<std::array<float, 2>> &phi_ranges,
                              bool inclusive = true)
{
    float total_gap_phi = 0.f;
    for (auto &phi_range : phi_ranges)
        total_gap_phi += phi_range[1] - phi_range[0];
    float selected_phi_span = inclusive
                                  ? total_gap_phi
                                  : (2.f * (float)TMath::Pi() - total_gap_phi);
    return 2.f * (float)TMath::Pi() / selected_phi_span;
}

// ═══════════════════════════════════════════════════════════════════════════════
// FitResultMap — key: "<hist_name>.<quantity>", e.g. "h_radial_prompt_ex_gap.N_gamma"
// ═══════════════════════════════════════════════════════════════════════════════

using FitResultMap = std::map<std::string, double>;
FitResultMap dummy_map;

static void store_result(FitResultMap &result_map, const std::string &histogram_name,
                         double n_gamma, double n_gamma_err,
                         double peak_mu_mm, double peak_sigma_mm,
                         double alpha_param, double chi2_per_ndf)
{
    result_map[histogram_name + ".N_gamma"] = n_gamma;
    result_map[histogram_name + ".N_gamma_err"] = n_gamma_err;
    result_map[histogram_name + ".mu"] = peak_mu_mm;
    result_map[histogram_name + ".sigma"] = peak_sigma_mm;
    result_map[histogram_name + ".gs_frac"] = alpha_param;
    result_map[histogram_name + ".chi2_ndf"] = chi2_per_ndf;
}

// ═══════════════════════════════════════════════════════════════════════════════
// photon_number_new
// ═══════════════════════════════════════════════════════════════════════════════

void photon_number_new(std::string data_repository, std::string run_name,
                       FitResultMap &fit_results = dummy_map,
                       int max_frames = 10000000)
{
    ROOT::Math::MinimizerOptions::SetDefaultPrintLevel(-1);

    // CODE_REVIEW §6.5: every `new TH1F/TH2F` in this function was being
    // auto-attached to the current TDirectory.  Once the input recodata.root
    // is opened below, gDirectory == that input file → ~50 histograms inherit
    // it.  When the file is closed (or even auto-closed at scope exit on a
    // long run) the histograms are freed under the caller's feet.  Disabling
    // AddDirectory globally for this function's lifetime prevents the
    // automatic attachment; histograms are owned by the local raw pointers
    // and live until the macro returns.  RAII guard restores the flag on
    // every return / exception path.
    struct AddDirectoryGuard {
        bool prev;
        AddDirectoryGuard() : prev(TH1::AddDirectoryStatus()) { TH1::AddDirectory(false); }
        ~AddDirectoryGuard() { TH1::AddDirectory(prev); }
    } _add_directory_guard;

    // ── Open input file ───────────────────────────────────────────────────────
    std::string input_filename = data_repository + "/" + run_name + "/recodata.root";
    TFilePtr input_file(TFile::Open(input_filename.c_str(), "READ"));
    if (!input_file || input_file->IsZombie())
    {
        std::cerr << "[ERROR] [photon_number] Cannot open " << input_filename << "\n";
        return;
    }
    TTree *recodata_tree = (TTree *)input_file->Get("recodata");
    AlcorRecodata *recodata = new AlcorRecodata();
    recodata->link_to_tree(recodata_tree);
    const int total_frames_to_process = (int)std::min((Long64_t)max_frames,
                                                      recodata_tree->GetEntries());
    AlcorFinedata::read_calib_from_file(data_repository + "/" + "20251111-164951" + "/gold_timing_fine_calib.txt");

    // ══════════════════════════════════════════════════════════════════════════
    // Pass 0 — spill accounting
    // ══════════════════════════════════════════════════════════════════════════

    std::vector<int> spill_index_of_frame(total_frames_to_process, -1);
    std::vector<int> physics_trigger_count_per_spill, dcr_frame_count_per_spill;
    int current_spill_index = -1, total_physics_frames = 0, total_dcr_frames = 0;

    for (int iframe = 0; iframe < total_frames_to_process; ++iframe)
    {
        recodata_tree->GetEntry(iframe);
        if (recodata->is_start_of_spill())
        {
            ++current_spill_index;
            physics_trigger_count_per_spill.push_back(0);
            dcr_frame_count_per_spill.push_back(0);
            spill_index_of_frame[iframe] = current_spill_index;
            continue;
        }
        if (current_spill_index < 0)
            continue;
        spill_index_of_frame[iframe] = current_spill_index;
        if (recodata->get_trigger_by_index(104))
        {
            ++physics_trigger_count_per_spill[current_spill_index];
            ++total_physics_frames;
        }
        else if (recodata->get_trigger_by_index(100))
        {
            ++dcr_frame_count_per_spill[current_spill_index];
            ++total_dcr_frames;
        }
    }

    mist::logger::info("[photon_number] Pass 0: " + std::to_string(physics_trigger_count_per_spill.size()) + " spills, " + std::to_string(total_physics_frames) + " physics triggers, " + std::to_string(total_dcr_frames) + " DCR frames");

    // ══════════════════════════════════════════════════════════════════════════
    // Histogram declarations
    // ══════════════════════════════════════════════════════════════════════════

    // ── Circle-fit parameter distributions ───────────────────────────────────
    RootHist<TH1F> h_circle_fit_x_prompt("h_circle_fit_x_prompt", ";circle center x (mm)", 240, -30, 30);
    RootHist<TH1F> h_circle_fit_y_prompt("h_circle_fit_y_prompt", ";circle center y (mm)", 240, -30, 30);
    RootHist<TH1F> h_circle_fit_r_prompt("h_circle_fit_r_prompt", ";circle radius (mm)", 400, 30, 130);
    RootHist<TH1F> h_circle_fit_x_early("h_circle_fit_x_early", ";circle center x (mm)", 240, -30, 30);
    RootHist<TH1F> h_circle_fit_y_early("h_circle_fit_y_early", ";circle center y (mm)", 240, -30, 30);
    RootHist<TH1F> h_circle_fit_r_early("h_circle_fit_r_early", ";circle radius (mm)", 400, 30, 130);

    // ── Hit timing distribution ───────────────────────────────────────────────
    RootHist<TH1F> h_delta_time_all_hits("h_delta_time_all_hits",
                                           ";t_{Hit}-t_{trig} (ns)", 10000, -312.5, 312.5);

    // ── 2D Hit maps ──────────────────────────────────────────────────────────
    RootHist<TH2F> h_hit_map_xy("h_hit_map_xy", ";x (mm);y (mm)",
                                  396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_hit_map_rphi("h_hit_map_rphi", ";#phi (rad);R (mm)",
                                    400, -TMath::Pi(), TMath::Pi(), 75, 25, 125);

    // ── Persistence maps for early/prompt coincidence frames ─────────────────
    // Only filled in frames where both prompt AND early hits are present.
    RootHist<TH2F> h_persistence_xy_prompt("h_persistence_xy_prompt",
                                             ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_persistence_xy_early("h_persistence_xy_early",
                                            ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_persistence_rphi_prompt("h_persistence_rphi_prompt",
                                               ";#phi (rad);R (mm)", 400, -TMath::Pi(), TMath::Pi(), 75, 25, 125);
    RootHist<TH2F> h_persistence_rphi_early("h_persistence_rphi_early",
                                              ";#phi (rad);R (mm)", 400, -TMath::Pi(), TMath::Pi(), 75, 25, 125);

    // ── Radial distributions — prompt signal ─────────────────────────────────
    RootHist<TH1F> h_radial_prompt_full("h_radial_prompt_full", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_prompt_in_gap("h_radial_prompt_in_gap", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_prompt_ex_gap("h_radial_prompt_ex_gap", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_prompt_ex_gap_1350("h_radial_prompt_ex_gap_1350", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_prompt_ex_gap_1375("h_radial_prompt_ex_gap_1375", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);

    // ── Radial distributions — DCR background ────────────────────────────────
    RootHist<TH1F> h_radial_dcr_full("h_radial_dcr_full", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_dcr_in_gap("h_radial_dcr_in_gap", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_dcr_ex_gap("h_radial_dcr_ex_gap", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_dcr_ex_gap_1350("h_radial_dcr_ex_gap_1350", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_dcr_ex_gap_1375("h_radial_dcr_ex_gap_1375", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);

    // ── Radial distributions — early satellite population ────────────────────
    // Uses early_ring_center as ring centre; DCR-subtracted using window-width scaling.
    RootHist<TH1F> h_radial_early_full("h_radial_early_full", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_early_in_gap("h_radial_early_in_gap", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_early_ex_gap("h_radial_early_ex_gap", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_early_ex_gap_1350("h_radial_early_ex_gap_1350", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH1F> h_radial_early_ex_gap_1375("h_radial_early_ex_gap_1375", ";R (mm)", kRadialBins, kRadialLoMm, kRadialHiMm);

    // ── Coverage maps ─────────────────────────────────────────────────────────
    static constexpr int kRadialFineBins = 100 * kCoverageGranularity;
    RootHist<TH2F> h_coverage_map_xy("h_coverage_map_xy", ";x (mm);y (mm)",
                                       396 * kCoverageGranularity, -99, 99,
                                       396 * kCoverageGranularity, -99, 99);
    RootHist<TH2F> h_coverage_map_rphi("h_coverage_map_rphi", ";#phi (rad);R (mm)",
                                         400 * kCoverageGranularity, -TMath::Pi(), TMath::Pi(),
                                         kRadialFineBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH2F> h_coverage_map_rphi_1350("h_coverage_map_rphi_1350", ";#phi (rad);R (mm)",
                                              400 * kCoverageGranularity, -TMath::Pi(), TMath::Pi(),
                                              kRadialFineBins, kRadialLoMm, kRadialHiMm);
    RootHist<TH2F> h_coverage_map_rphi_1375("h_coverage_map_rphi_1375", ";#phi (rad);R (mm)",
                                              400 * kCoverageGranularity, -TMath::Pi(), TMath::Pi(),
                                              kRadialFineBins, kRadialLoMm, kRadialHiMm);

    // ── Hit-pair timing vs spatial separation ─────────────────────────────────
    // Hits time-sorted per frame: t_i <= t_j for i < j → dt_ij <= 0 always.
    RootHist<TH2F> h_pair_dt_trig_vs_separation(
        "h_pair_dt_trig_vs_separation", ";#DeltaR_{ij} (mm);t_{i}-t_{trig} (ns)",
        kPairSeparationBins, 0., kPairSeparationMaxMm,
        kPairDeltaTimeBins, -kPairDeltaTimeMaxNs, kPairDeltaTimeMaxNs);
    RootHist<TH2F> h_pair_dt_hit_vs_separation(
        "h_pair_dt_hit_vs_separation", ";#DeltaR_{ij} (mm);t_{i}-t_{j} (ns)",
        kPairSeparationBins, 0., kPairSeparationMaxMm,
        kPairDeltaTimeBins, -kPairDeltaTimeMaxNs, kPairDeltaTimeMaxNs);

    // ── dt distributions split by sensor type and phi region ─────────────────
    // Raw t_hit - t_trig with no time window — both prompt peak and early
    // satellite visible simultaneously. Binning matches h_delta_time_all_hits
    // but restricted to [-5, +5] ns to zoom in on the relevant structure.
    static constexpr int kDtSensorBins = 500; // 20 ps/bin over 10 ns
    static constexpr float kDtSensorLoNs = -5.f;
    static constexpr float kDtSensorHiNs = +5.f;
    RootHist<TH1F> h_dt_1350_full("h_dt_1350_full",
                                    ";t_{Hit}-t_{trig} (ns);hits/frame", kDtSensorBins, kDtSensorLoNs, kDtSensorHiNs);
    RootHist<TH1F> h_dt_1350_in_gap("h_dt_1350_in_gap",
                                      ";t_{Hit}-t_{trig} (ns);hits/frame", kDtSensorBins, kDtSensorLoNs, kDtSensorHiNs);
    RootHist<TH1F> h_dt_1350_ex_gap("h_dt_1350_ex_gap",
                                      ";t_{Hit}-t_{trig} (ns);hits/frame", kDtSensorBins, kDtSensorLoNs, kDtSensorHiNs);
    RootHist<TH1F> h_dt_1375_full("h_dt_1375_full",
                                    ";t_{Hit}-t_{trig} (ns);hits/frame", kDtSensorBins, kDtSensorLoNs, kDtSensorHiNs);
    RootHist<TH1F> h_dt_1375_in_gap("h_dt_1375_in_gap",
                                      ";t_{Hit}-t_{trig} (ns);hits/frame", kDtSensorBins, kDtSensorLoNs, kDtSensorHiNs);
    RootHist<TH1F> h_dt_1375_ex_gap("h_dt_1375_ex_gap",
                                      ";t_{Hit}-t_{trig} (ns);hits/frame", kDtSensorBins, kDtSensorLoNs, kDtSensorHiNs);

    // ── Early-to-prompt spatial correlation ───────────────────────────────────
    // h_early_to_prompt_nearest_dR: for each early Hit, distance to nearest
    //   prompt Hit in the same frame.
    //   peak at 0 mm     → same-channel re-fire
    //   peak at 3.3 mm   → cardinal cross-talk neighbour
    //   peak at 4.67 mm  → diagonal cross-talk neighbour
    //   broad/flat       → uncorrelated (ring geometry)
    // h_same_channel_refire_count: channels firing in both windows per frame.
    // h_early_hit_multiplicity_per_frame: early hits per frame — tests
    //   whether the early population is bimodal (second particle) or Poisson
    //   (constant fractional rate, consistent with time-walk or optical effect).
    RootHist<TH1F> h_early_to_prompt_nearest_dR(
        "h_early_to_prompt_nearest_dR",
        ";#DeltaR_{early#rightarrownearest prompt} (mm);entries",
        150, 0., 50.);
    RootHist<TH1F> h_same_channel_refire_count(
        "h_same_channel_refire_count",
        ";N_{channels firing in both windows} per frame;frames",
        20, 0., 20.);
    RootHist<TH1F> h_early_hit_multiplicity_per_frame(
        "h_early_hit_multiplicity_per_frame",
        ";N_{hits} in early window per frame;frames",
        50, 0, 50);

    // ══════════════════════════════════════════════════════════════════════════
    // Pass 1 — circle fitting
    //
    // Collect hits in prompt and early windows separately per frame and run
    // Taubin algebraic circle fits to extract ring centre distributions.
    // ══════════════════════════════════════════════════════════════════════════

    for (int iframe = 0; iframe < total_frames_to_process; ++iframe)
    {
        recodata_tree->GetEntry(iframe);
        if (recodata->is_start_of_spill())
            continue;
        auto physics_trigger = recodata->get_trigger_by_index(104);
        if (!physics_trigger)
            continue;

        std::vector<std::array<float, 2>> prompt_hit_positions, early_hit_positions;
        float prompt_radius_sum = 0.f, early_radius_sum = 0.f;

        for (int ihit = 0; ihit < (int)recodata->get_recodata().size(); ++ihit)
        {
            const float delta_time_ns = recodata->get_hit_t(ihit) - physics_trigger->fine_time;
            h_delta_time_all_hits->Fill(delta_time_ns);
            const float hit_x_mm = recodata->get_hit_x(ihit);
            const float hit_y_mm = recodata->get_hit_y(ihit);
            const float hit_r_mm = recodata->get_hit_r(ihit);

            if (delta_time_ns >= kPromptTimeCutNs[0] && delta_time_ns <= kPromptTimeCutNs[1])
            {
                prompt_hit_positions.push_back({hit_x_mm, hit_y_mm});
                prompt_radius_sum += hit_r_mm;
            }
            else if (delta_time_ns >= kEarlyTimeCutNs[0] && delta_time_ns <= kEarlyTimeCutNs[1])
            {
                early_hit_positions.push_back({hit_x_mm, hit_y_mm});
                early_radius_sum += hit_r_mm;
            }
        }

        if ((int)prompt_hit_positions.size() > 4)
        {
            auto circle_fit_result = fit_circle(prompt_hit_positions,
                                                {0.f, 0.f, prompt_radius_sum / prompt_hit_positions.size()}, false, {{}});
            h_circle_fit_x_prompt->Fill(circle_fit_result[0][0]);
            h_circle_fit_y_prompt->Fill(circle_fit_result[1][0]);
            h_circle_fit_r_prompt->Fill(circle_fit_result[2][0]);
        }
        if ((int)early_hit_positions.size() > 4)
        {
            auto circle_fit_result = fit_circle(early_hit_positions,
                                                {0.f, 0.f, early_radius_sum / early_hit_positions.size()}, false, {{}});
            h_circle_fit_x_early->Fill(circle_fit_result[0][0]);
            h_circle_fit_y_early->Fill(circle_fit_result[1][0]);
            h_circle_fit_r_early->Fill(circle_fit_result[2][0]);
        }
    }

    // ── Extract ring centres from double-Gaussian fits ────────────────────────
    TF1 *double_gaussian_fit = new TF1("double_gaussian_fit",
                                       "[0]*TMath::Gaus(x,[1],[2],true)+[3]*TMath::Gaus(x,[1],[4],true)", -30, 130);

    auto extract_peak_position = [&](TH1F *histogram, float fit_lo, float fit_hi) -> float
    {
        double_gaussian_fit->SetRange(fit_lo, fit_hi);
        double_gaussian_fit->SetParameters(histogram->GetMaximum(), histogram->GetMean(),
                                           1., histogram->GetMaximum() * 0.3, 5.);
        histogram->Fit(double_gaussian_fit, "QNR");
        return double_gaussian_fit->GetParameter(1);
    };

    std::array<float, 3> prompt_ring_center = {};
    prompt_ring_center[0] = extract_peak_position(h_circle_fit_x_prompt, -30, 30);
    prompt_ring_center[1] = extract_peak_position(h_circle_fit_y_prompt, -30, 30);
    prompt_ring_center[2] = extract_peak_position(h_circle_fit_r_prompt, 30, 130);
    mist::logger::info("[photon_number] prompt_ring_center:" + std::string("  x=") + std::to_string(prompt_ring_center[0]) + "  y=" + std::to_string(prompt_ring_center[1]) + "  R=" + std::to_string(prompt_ring_center[2]));

    std::array<float, 3> early_ring_center = {};
    if (h_circle_fit_x_early->GetEntries() > 10)
    {
        early_ring_center[0] = extract_peak_position(h_circle_fit_x_early, -30, 30);
        early_ring_center[1] = extract_peak_position(h_circle_fit_y_early, -30, 30);
        early_ring_center[2] = extract_peak_position(h_circle_fit_r_early, 30, 130);
        mist::logger::info("[photon_number] early_ring_center:" + std::string("  x=") + std::to_string(early_ring_center[0]) + "  y=" + std::to_string(early_ring_center[1]) + "  R=" + std::to_string(early_ring_center[2]));
    }
    else
    {
        early_ring_center = prompt_ring_center;
        mist::logger::warning("[photon_number] Early population too sparse — using prompt centre as fallback");
    }

    // ══════════════════════════════════════════════════════════════════════════
    // Pass 2 — coverage maps + Hit filling + pair histograms
    //
    // Spill-start frames → coverage maps (weighted by physics trigger fraction).
    // Physics-trigger frames → radial distributions + pair timing + spatial
    //   correlation histograms.
    // DCR-trigger frames → DCR background radial distributions.
    // ══════════════════════════════════════════════════════════════════════════

    std::map<int, std::array<std::vector<std::array<int, 2>>, 2>> channel_bin_cache;
    int n_used_physics_frames = 0;

    for (int iframe = 0; iframe < total_frames_to_process; ++iframe)
    {
        recodata_tree->GetEntry(iframe);
        const int frame_spill_index = spill_index_of_frame[iframe];
        if (frame_spill_index < 0)
            continue;

        // ── Spill-start: fill coverage maps ──────────────────────────────────
        if (recodata->is_start_of_spill())
        {
            if (physics_trigger_count_per_spill[frame_spill_index] == 0)
                continue;
            const float spill_weight = (float)physics_trigger_count_per_spill[frame_spill_index] / (float)total_physics_frames;

            for (int ihit = 0; ihit < (int)recodata->get_recodata().size(); ++ihit)
            {
                const int global_channel_index = recodata->get_global_index(ihit);
                const float channel_center_x_mm = recodata->get_hit_x(ihit);
                const float channel_center_y_mm = recodata->get_hit_y(ihit);
                const SensorType sensor_model = sensor_type(recodata->get_device(ihit));
                TH2F *sensor_coverage_map =
                    (sensor_model == SensorType::k1350) ? h_coverage_map_rphi_1350 : (sensor_model == SensorType::k1375) ? h_coverage_map_rphi_1375
                                                                                                                         : nullptr;

                auto cached_channel = channel_bin_cache.find(global_channel_index);
                if (cached_channel != channel_bin_cache.end())
                {
                    for (auto &bin_xy : cached_channel->second[0])
                        h_coverage_map_xy->AddBinContent(
                            h_coverage_map_xy->GetBin(bin_xy[0], bin_xy[1]), spill_weight);
                    for (auto &bin_rphi : cached_channel->second[1])
                    {
                        h_coverage_map_rphi->AddBinContent(
                            h_coverage_map_rphi->GetBin(bin_rphi[0], bin_rphi[1]), spill_weight);
                        if (sensor_coverage_map)
                            sensor_coverage_map->AddBinContent(
                                sensor_coverage_map->GetBin(bin_rphi[0], bin_rphi[1]), spill_weight);
                    }
                    continue;
                }

                auto &cached_bins_xy = channel_bin_cache[global_channel_index][0];
                auto &cached_bins_rphi = channel_bin_cache[global_channel_index][1];

                int bin_x_lo = h_coverage_map_xy->GetXaxis()->FindBin(channel_center_x_mm - 1.5f);
                int bin_x_hi = h_coverage_map_xy->GetXaxis()->FindBin(channel_center_x_mm + 1.5f);
                int bin_y_lo = h_coverage_map_xy->GetYaxis()->FindBin(channel_center_y_mm - 1.5f);
                int bin_y_hi = h_coverage_map_xy->GetYaxis()->FindBin(channel_center_y_mm + 1.5f);
                for (int bin_x = bin_x_lo; bin_x <= bin_x_hi; ++bin_x)
                    for (int bin_y = bin_y_lo; bin_y <= bin_y_hi; ++bin_y)
                    {
                        h_coverage_map_xy->AddBinContent(
                            h_coverage_map_xy->GetBin(bin_x, bin_y), spill_weight);
                        cached_bins_xy.push_back({bin_x, bin_y});
                    }

                const float channel_radius_mm = std::hypot(channel_center_x_mm, channel_center_y_mm);
                const float channel_phi_rad = std::atan2(channel_center_y_mm, channel_center_x_mm);
                const float delta_phi_rad = 1.5f * sqrtf(2.f) / channel_radius_mm;
                const float delta_r_mm = 1.5f * sqrtf(2.f);
                int bin_r_lo = h_coverage_map_rphi->GetYaxis()->FindBin(channel_radius_mm - delta_r_mm);
                int bin_r_hi = h_coverage_map_rphi->GetYaxis()->FindBin(channel_radius_mm + delta_r_mm);
                int bin_phi_lo = h_coverage_map_rphi->GetXaxis()->FindBin(channel_phi_rad - delta_phi_rad);
                int bin_phi_hi = h_coverage_map_rphi->GetXaxis()->FindBin(channel_phi_rad + delta_phi_rad);
                for (int bin_phi = bin_phi_lo; bin_phi <= bin_phi_hi; ++bin_phi)
                {
                    float polar_bin_phi = h_coverage_map_rphi->GetXaxis()->GetBinCenter(bin_phi);
                    for (int bin_r = bin_r_lo; bin_r <= bin_r_hi; ++bin_r)
                    {
                        float polar_bin_r = h_coverage_map_rphi->GetYaxis()->GetBinCenter(bin_r);
                        if (std::fabs(polar_bin_r * cosf(polar_bin_phi) - channel_center_x_mm) > 1.5f ||
                            std::fabs(polar_bin_r * sinf(polar_bin_phi) - channel_center_y_mm) > 1.5f)
                            continue;
                        if (std::find(cached_bins_rphi.begin(), cached_bins_rphi.end(),
                                      std::array<int, 2>{bin_phi, bin_r}) != cached_bins_rphi.end())
                            continue;
                        h_coverage_map_rphi->AddBinContent(
                            h_coverage_map_rphi->GetBin(bin_phi, bin_r), spill_weight);
                        cached_bins_rphi.push_back({bin_phi, bin_r});
                    }
                }
                if (sensor_coverage_map)
                    for (auto &bin_rphi : cached_bins_rphi)
                        sensor_coverage_map->AddBinContent(
                            sensor_coverage_map->GetBin(bin_rphi[0], bin_rphi[1]), spill_weight);
            }
            continue;
        }

        // ── Physics trigger: radial distributions + pair timing + correlations ─
        auto physics_trigger = recodata->get_trigger_by_index(104);
        if (physics_trigger)
        {
            ++n_used_physics_frames;
            const int nhits = (int)recodata->get_recodata().size();

            // ── Radial distributions ──────────────────────────────────────────
            for (int ihit = 0; ihit < nhits; ++ihit)
            {
                if (recodata->is_afterpulse(ihit))
                    continue;
                const float delta_time_ns = recodata->get_hit_t(ihit) - physics_trigger->fine_time;

                // Prompt signal
                if (delta_time_ns >= kPromptTimeCutNs[0] && delta_time_ns <= kPromptTimeCutNs[1])
                {
                    const float hit_phi_rad = recodata->get_hit_phi(ihit,
                                                                    {prompt_ring_center[0], prompt_ring_center[1]});
                    const float hit_radius_mm = recodata->get_hit_r(ihit,
                                                                    {prompt_ring_center[0], prompt_ring_center[1]});
                    h_hit_map_xy->Fill(recodata->get_hit_x_rnd(ihit), recodata->get_hit_y_rnd(ihit));
                    h_hit_map_rphi->Fill(
                        recodata->get_hit_phi_rnd(ihit, {prompt_ring_center[0], prompt_ring_center[1]}),
                        recodata->get_hit_r_rnd(ihit, {prompt_ring_center[0], prompt_ring_center[1]}));
                    h_radial_prompt_full->Fill(hit_radius_mm);
                    bool hit_is_in_phi_gap = false;
                    for (auto &gap_range : kPhiGapRanges)
                        if (hit_phi_rad > gap_range[0] && hit_phi_rad < gap_range[1])
                        {
                            hit_is_in_phi_gap = true;
                            break;
                        }
                    (hit_is_in_phi_gap ? h_radial_prompt_in_gap : h_radial_prompt_ex_gap)
                        ->Fill(hit_radius_mm);
                    if (!hit_is_in_phi_gap)
                    {
                        const SensorType sensor_model = sensor_type(recodata->get_device(ihit));
                        if (sensor_model == SensorType::k1350)
                            h_radial_prompt_ex_gap_1350->Fill(hit_radius_mm);
                        else if (sensor_model == SensorType::k1375)
                            h_radial_prompt_ex_gap_1375->Fill(hit_radius_mm);
                    }
                }
                // Early satellite — uses early_ring_center for correct R reconstruction
                else if (delta_time_ns >= kEarlyTimeCutNs[0] && delta_time_ns <= kEarlyTimeCutNs[1])
                {
                    const float hit_phi_rad = recodata->get_hit_phi(ihit,
                                                                    {early_ring_center[0], early_ring_center[1]});
                    const float hit_radius_mm = recodata->get_hit_r(ihit,
                                                                    {early_ring_center[0], early_ring_center[1]});
                    h_radial_early_full->Fill(hit_radius_mm);
                    bool hit_is_in_phi_gap = false;
                    for (auto &gap_range : kPhiGapRanges)
                        if (hit_phi_rad > gap_range[0] && hit_phi_rad < gap_range[1])
                        {
                            hit_is_in_phi_gap = true;
                            break;
                        }
                    (hit_is_in_phi_gap ? h_radial_early_in_gap : h_radial_early_ex_gap)
                        ->Fill(hit_radius_mm);
                    if (!hit_is_in_phi_gap)
                    {
                        const SensorType sensor_model = sensor_type(recodata->get_device(ihit));
                        if (sensor_model == SensorType::k1350)
                            h_radial_early_ex_gap_1350->Fill(hit_radius_mm);
                        else if (sensor_model == SensorType::k1375)
                            h_radial_early_ex_gap_1375->Fill(hit_radius_mm);
                    }
                }
            }

            // ── Hit-pair timing (no afterpulse veto, time-sorted, i < j) ──────
            // t_i <= t_j by construction → dt_ij <= 0 always.
            // Same-channel pairs (separation < 0.5 mm) excluded.
            {
                std::vector<float> hit_x_positions(nhits), hit_y_positions(nhits),
                    hit_delta_times(nhits);
                for (int ihit = 0; ihit < nhits; ++ihit)
                {
                    hit_x_positions[ihit] = recodata->get_hit_x(ihit);
                    hit_y_positions[ihit] = recodata->get_hit_y(ihit);
                    hit_delta_times[ihit] = recodata->get_hit_t(ihit) - physics_trigger->fine_time;
                }
                std::vector<int> time_sorted_indices(nhits);
                std::iota(time_sorted_indices.begin(), time_sorted_indices.end(), 0);
                std::sort(time_sorted_indices.begin(), time_sorted_indices.end(),
                          [&](int index_a, int index_b)
                          { return hit_delta_times[index_a] < hit_delta_times[index_b]; });

                std::vector<float> sorted_x(nhits), sorted_y(nhits), sorted_dt(nhits);
                for (int k = 0; k < nhits; ++k)
                {
                    sorted_x[k] = hit_x_positions[time_sorted_indices[k]];
                    sorted_y[k] = hit_y_positions[time_sorted_indices[k]];
                    sorted_dt[k] = hit_delta_times[time_sorted_indices[k]];
                }

                for (int ref_hit = 0; ref_hit < nhits; ++ref_hit)
                {
                    if (std::fabs(sorted_dt[ref_hit]) >= kPairTimeCutNs)
                        continue;
                    for (int partner_hit = ref_hit + 1; partner_hit < nhits; ++partner_hit)
                    {
                        const float dx_mm = sorted_x[partner_hit] - sorted_x[ref_hit];
                        const float dy_mm = sorted_y[partner_hit] - sorted_y[ref_hit];
                        const float pair_separation_mm = std::sqrt(dx_mm * dx_mm + dy_mm * dy_mm);
                        if (pair_separation_mm < 0.5f)
                            continue;
                        h_pair_dt_trig_vs_separation->Fill(pair_separation_mm, sorted_dt[ref_hit]);
                        h_pair_dt_hit_vs_separation->Fill(pair_separation_mm,
                                                          sorted_dt[ref_hit] - sorted_dt[partner_hit]);
                    }
                }
            }

            // ── Persistence maps (coincidence frames only) ────────────────────
            {
                bool frame_has_prompt_hit = false, frame_has_early_hit = false;
                for (int ihit = 0; ihit < nhits; ++ihit)
                {
                    if (recodata->is_afterpulse(ihit))
                        continue;
                    const float delta_time_ns = recodata->get_hit_t(ihit) - physics_trigger->fine_time;
                    if (delta_time_ns >= kPromptTimeCutNs[0] && delta_time_ns <= kPromptTimeCutNs[1])
                        frame_has_prompt_hit = true;
                    else if (delta_time_ns >= kEarlyTimeCutNs[0] && delta_time_ns <= kEarlyTimeCutNs[1])
                        frame_has_early_hit = true;
                    if (frame_has_prompt_hit && frame_has_early_hit)
                        break;
                }

                if (frame_has_prompt_hit && frame_has_early_hit)
                {
                    for (int ihit = 0; ihit < nhits; ++ihit)
                    {
                        if (recodata->is_afterpulse(ihit))
                            continue;
                        const float delta_time_ns = recodata->get_hit_t(ihit) - physics_trigger->fine_time;
                        if (delta_time_ns >= kPromptTimeCutNs[0] && delta_time_ns <= kPromptTimeCutNs[1])
                        {
                            h_persistence_xy_prompt->Fill(
                                recodata->get_hit_x_rnd(ihit), recodata->get_hit_y_rnd(ihit));
                            h_persistence_rphi_prompt->Fill(
                                recodata->get_hit_phi_rnd(ihit,
                                                          {prompt_ring_center[0], prompt_ring_center[1]}),
                                recodata->get_hit_r_rnd(ihit,
                                                        {prompt_ring_center[0], prompt_ring_center[1]}));
                        }
                        else if (delta_time_ns >= kEarlyTimeCutNs[0] && delta_time_ns <= kEarlyTimeCutNs[1])
                        {
                            h_persistence_xy_early->Fill(
                                recodata->get_hit_x_rnd(ihit), recodata->get_hit_y_rnd(ihit));
                            h_persistence_rphi_early->Fill(
                                recodata->get_hit_phi_rnd(ihit,
                                                          {early_ring_center[0], early_ring_center[1]}),
                                recodata->get_hit_r_rnd(ihit,
                                                        {early_ring_center[0], early_ring_center[1]}));
                        }
                    }
                }
            }

            // ── Early Hit multiplicity per frame ──────────────────────────────
            {
                int early_hit_count_this_frame = 0;
                for (int ihit = 0; ihit < nhits; ++ihit)
                {
                    if (recodata->is_afterpulse(ihit))
                        continue;
                    const float delta_time_ns = recodata->get_hit_t(ihit) - physics_trigger->fine_time;
                    if (delta_time_ns >= kEarlyTimeCutNs[0] && delta_time_ns <= kEarlyTimeCutNs[1])
                        ++early_hit_count_this_frame;
                }
                h_early_hit_multiplicity_per_frame->Fill(early_hit_count_this_frame);
            }

            // ── Early-to-prompt spatial correlation ───────────────────────────
            // For each early Hit, find the nearest prompt Hit in the same frame.
            // Channel index used to detect same-channel re-fires (dR = 0 exactly).
            {
                struct HitRecord
                {
                    float x_mm, y_mm;
                    int global_channel_index;
                };
                std::vector<HitRecord> prompt_hit_records, early_hit_records;

                for (int ihit = 0; ihit < nhits; ++ihit)
                {
                    if (recodata->is_afterpulse(ihit))
                        continue;
                    const float delta_time_ns = recodata->get_hit_t(ihit) - physics_trigger->fine_time;
                    HitRecord record = {recodata->get_hit_x(ihit), recodata->get_hit_y(ihit),
                                        static_cast<int>(recodata->get_global_index(ihit))};
                    if (delta_time_ns >= kPromptTimeCutNs[0] && delta_time_ns <= kPromptTimeCutNs[1])
                        prompt_hit_records.push_back(record);
                    else if (delta_time_ns >= kEarlyTimeCutNs[0] && delta_time_ns <= kEarlyTimeCutNs[1])
                        early_hit_records.push_back(record);

                    // ── dt by sensor and phi region (no time window) ──────────────
                    // Filled for all hits regardless of timing window so both the
                    // prompt peak and early satellite are visible simultaneously.
                    {
                        const SensorType sensor_model = sensor_type(recodata->get_device(ihit));
                        const float hit_phi_rad_prompt = recodata->get_hit_phi(ihit,
                                                                               {prompt_ring_center[0], prompt_ring_center[1]});
                        bool hit_is_in_phi_gap = false;
                        for (auto &gap_range : kPhiGapRanges)
                            if (hit_phi_rad_prompt > gap_range[0] && hit_phi_rad_prompt < gap_range[1])
                            {
                                hit_is_in_phi_gap = true;
                                break;
                            }

                        if (sensor_model == SensorType::k1350)
                        {
                            h_dt_1350_full->Fill(delta_time_ns);
                            (hit_is_in_phi_gap ? h_dt_1350_in_gap : h_dt_1350_ex_gap)
                                ->Fill(delta_time_ns);
                        }
                        else if (sensor_model == SensorType::k1375)
                        {
                            h_dt_1375_full->Fill(delta_time_ns);
                            (hit_is_in_phi_gap ? h_dt_1375_in_gap : h_dt_1375_ex_gap)
                                ->Fill(delta_time_ns);
                        }
                    }
                }

                if (!prompt_hit_records.empty() && !early_hit_records.empty())
                {
                    int n_same_channel_refires = 0;
                    for (const auto &early_hit : early_hit_records)
                        for (const auto &prompt_hit : prompt_hit_records)
                            if (early_hit.global_channel_index == prompt_hit.global_channel_index)
                            {
                                ++n_same_channel_refires;
                                break;
                            }
                    h_same_channel_refire_count->Fill(n_same_channel_refires);

                    for (const auto &early_hit : early_hit_records)
                    {
                        float nearest_prompt_separation_mm = std::numeric_limits<float>::max();
                        for (const auto &prompt_hit : prompt_hit_records)
                        {
                            const float dx_mm = prompt_hit.x_mm - early_hit.x_mm;
                            const float dy_mm = prompt_hit.y_mm - early_hit.y_mm;
                            const float separation_mm = std::sqrt(dx_mm * dx_mm + dy_mm * dy_mm);
                            if (separation_mm < nearest_prompt_separation_mm)
                                nearest_prompt_separation_mm = separation_mm;
                        }
                        h_early_to_prompt_nearest_dR->Fill(nearest_prompt_separation_mm);
                    }
                }
            }

            continue;
        }

        // ── DCR trigger: background radial distributions ───────────────────────
        auto dcr_trigger = recodata->get_trigger_by_index(100);
        if (!dcr_trigger)
            continue;

        for (int ihit = 0; ihit < (int)recodata->get_recodata().size(); ++ihit)
        {
            if (recodata->is_afterpulse(ihit))
                continue;
            const float delta_time_ns = recodata->get_hit_t(ihit) - dcr_trigger->fine_time;
            if (delta_time_ns < kPromptTimeCutNs[0] || delta_time_ns > kPromptTimeCutNs[1])
                continue;
            const float hit_phi_rad = recodata->get_hit_phi(ihit,
                                                            {prompt_ring_center[0], prompt_ring_center[1]});
            const float hit_radius_mm = recodata->get_hit_r(ihit,
                                                            {prompt_ring_center[0], prompt_ring_center[1]});
            h_radial_dcr_full->Fill(hit_radius_mm);
            bool hit_is_in_phi_gap = false;
            for (auto &gap_range : kPhiGapRanges)
                if (hit_phi_rad > gap_range[0] && hit_phi_rad < gap_range[1])
                {
                    hit_is_in_phi_gap = true;
                    break;
                }
            (hit_is_in_phi_gap ? h_radial_dcr_in_gap : h_radial_dcr_ex_gap)->Fill(hit_radius_mm);
            if (!hit_is_in_phi_gap)
            {
                const SensorType sensor_model = sensor_type(recodata->get_device(ihit));
                if (sensor_model == SensorType::k1350)
                    h_radial_dcr_ex_gap_1350->Fill(hit_radius_mm);
                else if (sensor_model == SensorType::k1375)
                    h_radial_dcr_ex_gap_1375->Fill(hit_radius_mm);
            }
        }
    }

    if (n_used_physics_frames != total_physics_frames)
        mist::logger::warning("[photon_number] physics frame count mismatch: prescan=" + std::to_string(total_physics_frames) + "  mainloop=" + std::to_string(n_used_physics_frames));

    // ══════════════════════════════════════════════════════════════════════════
    // Acceptance correction
    //
    // Each radial histogram: ÷ n_frames → ÷ efficiency → ÷ bin_width → × phi_scale
    // DCR subtracted from signal.
    // Early DCR scale = early_window_width / prompt_window_width.
    // ══════════════════════════════════════════════════════════════════════════

    const TAxis *radial_binning_axis = h_radial_prompt_full->GetXaxis();

    auto apply_acceptance_correction = [&](TH1F *radial_histogram,
                                           TH1F *efficiency_histogram,
                                           float phi_extrapolation_factor,
                                           int normalisation_frame_count)
    {
        radial_histogram->Scale(1. / normalisation_frame_count);
        radial_histogram->Divide(efficiency_histogram);
        radial_histogram->Scale(1., "width");
        radial_histogram->Scale(phi_extrapolation_factor);
    };

    TH1F *eff_full = radial_efficiency(h_coverage_map_rphi, radial_binning_axis,
                                       {{-(float)TMath::Pi(), (float)TMath::Pi()}});
    TH1F *eff_in_gap = radial_efficiency(h_coverage_map_rphi, radial_binning_axis, kPhiGapRanges, true);
    TH1F *eff_ex_gap = radial_efficiency(h_coverage_map_rphi, radial_binning_axis, kPhiGapRanges, false);
    TH1F *eff_1350 = radial_efficiency(h_coverage_map_rphi_1350, radial_binning_axis, kPhiGapRanges, false);
    TH1F *eff_1375 = radial_efficiency(h_coverage_map_rphi_1375, radial_binning_axis, kPhiGapRanges, false);

    const float phi_scale_full = 1.f;
    const float phi_scale_in_gap = phi_extrapolation_scale(kPhiGapRanges, true);
    const float phi_scale_ex_gap = phi_extrapolation_scale(kPhiGapRanges, false);

    // Prompt signal
    apply_acceptance_correction(h_radial_prompt_full, eff_full, phi_scale_full, n_used_physics_frames);
    apply_acceptance_correction(h_radial_prompt_in_gap, eff_in_gap, phi_scale_in_gap, n_used_physics_frames);
    apply_acceptance_correction(h_radial_prompt_ex_gap, eff_ex_gap, phi_scale_ex_gap, n_used_physics_frames);
    apply_acceptance_correction(h_radial_prompt_ex_gap_1350, eff_1350, phi_scale_ex_gap, n_used_physics_frames);
    apply_acceptance_correction(h_radial_prompt_ex_gap_1375, eff_1375, phi_scale_ex_gap, n_used_physics_frames);

    // DCR background
    apply_acceptance_correction(h_radial_dcr_full, eff_full, phi_scale_full, total_dcr_frames);
    apply_acceptance_correction(h_radial_dcr_in_gap, eff_in_gap, phi_scale_in_gap, total_dcr_frames);
    apply_acceptance_correction(h_radial_dcr_ex_gap, eff_ex_gap, phi_scale_ex_gap, total_dcr_frames);
    apply_acceptance_correction(h_radial_dcr_ex_gap_1350, eff_1350, phi_scale_ex_gap, total_dcr_frames);
    apply_acceptance_correction(h_radial_dcr_ex_gap_1375, eff_1375, phi_scale_ex_gap, total_dcr_frames);

    // DCR subtraction from prompt
    h_radial_prompt_full->Add(h_radial_dcr_full, -1.);
    h_radial_prompt_in_gap->Add(h_radial_dcr_in_gap, -1.);
    h_radial_prompt_ex_gap->Add(h_radial_dcr_ex_gap, -1.);
    h_radial_prompt_ex_gap_1350->Add(h_radial_dcr_ex_gap_1350, -1.);
    h_radial_prompt_ex_gap_1375->Add(h_radial_dcr_ex_gap_1375, -1.);

    // Early satellite — DCR scale = early_window / prompt_window
    const float early_window_dcr_scale =
        (kEarlyTimeCutNs[1] - kEarlyTimeCutNs[0]) / (kPromptTimeCutNs[1] - kPromptTimeCutNs[0]);

    apply_acceptance_correction(h_radial_early_full, eff_full, phi_scale_full, n_used_physics_frames);
    apply_acceptance_correction(h_radial_early_in_gap, eff_in_gap, phi_scale_in_gap, n_used_physics_frames);
    apply_acceptance_correction(h_radial_early_ex_gap, eff_ex_gap, phi_scale_ex_gap, n_used_physics_frames);
    apply_acceptance_correction(h_radial_early_ex_gap_1350, eff_1350, phi_scale_ex_gap, n_used_physics_frames);
    apply_acceptance_correction(h_radial_early_ex_gap_1375, eff_1375, phi_scale_ex_gap, n_used_physics_frames);

    h_radial_early_full->Add(h_radial_dcr_full, -early_window_dcr_scale);
    h_radial_early_in_gap->Add(h_radial_dcr_in_gap, -early_window_dcr_scale);
    h_radial_early_ex_gap->Add(h_radial_dcr_ex_gap, -early_window_dcr_scale);
    h_radial_early_ex_gap_1350->Add(h_radial_dcr_ex_gap_1350, -early_window_dcr_scale);
    h_radial_early_ex_gap_1375->Add(h_radial_dcr_ex_gap_1375, -early_window_dcr_scale);

    // ══════════════════════════════════════════════════════════════════════════
    // Crystal Ball + pol3 fit model
    //
    // CB left power-law tail accounts for chromatic dispersion low-R smearing.
    // Two-pass strategy: background fixed first, then full joint fit.
    // ══════════════════════════════════════════════════════════════════════════

    auto crystal_ball_plus_pol3 = [](double *x_val, double *params) -> double
    {
        const double reduced_x = (x_val[0] - params[1]) / params[2];
        const double cb_alpha = params[3], cb_n = params[4];
        double cb_value;
        if (reduced_x > -cb_alpha)
            cb_value = params[0] * std::exp(-0.5 * reduced_x * reduced_x);
        else
        {
            double cb_normalisation = std::pow(cb_n / cb_alpha, cb_n) * std::exp(-0.5 * cb_alpha * cb_alpha);
            cb_value = params[0] * cb_normalisation * std::pow(cb_n / cb_alpha - cb_alpha - reduced_x, -cb_n);
        }
        return cb_value + params[5] + params[6] * x_val[0] + params[7] * x_val[0] * x_val[0] + params[8] * x_val[0] * x_val[0] * x_val[0];
    };

    int current_fit_line_color = kRed;

    auto fit_radial_distribution = [&](TH1F *radial_histogram,
                                       bool draw_fit_annotation = false)
    {
        const float peak_position_seed = prompt_ring_center[2];
        const float sigma_seed = std::clamp((float)radial_histogram->GetRMS() * 0.4f, 0.5f, 4.0f);
        const float peak_amplitude_seed = radial_histogram->GetMaximum();

        TF1 background_prefit(TString::Format("background_prefit_%s", radial_histogram->GetName()).Data(),
                              "pol3", kFitRangeLoMm, kFitRangeHiMm);
        {
            RootHist<TH1F> sideband_clone(static_cast<TH1F*>(radial_histogram->Clone(
                TString::Format("sideband_clone_%s", radial_histogram->GetName()).Data())));
            for (int ibin = 1; ibin <= sideband_clone->GetNbinsX(); ++ibin)
            {
                double bin_center = sideband_clone->GetBinCenter(ibin);
                bool bin_is_in_signal_region =
                    (bin_center > peak_position_seed - 4.f * sigma_seed &&
                     bin_center < peak_position_seed + 2.f * sigma_seed);
                if (bin_center < kFitRangeLoMm || bin_center > kFitRangeHiMm || bin_is_in_signal_region)
                {
                    sideband_clone->SetBinContent(ibin, 0.);
                    sideband_clone->SetBinError(ibin, 1e10);
                }
            }
            background_prefit.SetParameters(0.08, 0., 0., 0.);
            sideband_clone->Fit(&background_prefit, "RQ0");
            delete sideband_clone;
        }

        TF1 cb_fit_model(TString::Format("cb_fit_%s", radial_histogram->GetName()).Data(),
                         crystal_ball_plus_pol3, kFitRangeLoMm, kFitRangeHiMm, 9);
        const char *parameter_names[9] = {
            "cb_amplitude", "peak_mu", "peak_sigma", "cb_alpha", "cb_n",
            "background_c0", "background_c1", "background_c2", "background_c3"};
        for (int iparam = 0; iparam < 9; ++iparam)
            cb_fit_model.SetParName(iparam, parameter_names[iparam]);

        cb_fit_model.SetParameters(peak_amplitude_seed, peak_position_seed, sigma_seed, 1.5, 3.0,
                                   background_prefit.GetParameter(0), background_prefit.GetParameter(1),
                                   background_prefit.GetParameter(2), background_prefit.GetParameter(3));
        cb_fit_model.SetParLimits(0, 0., 1e9);
        cb_fit_model.SetParLimits(2, 1.5, 5.0);
        cb_fit_model.SetParLimits(3, 0.5, 5.0);
        cb_fit_model.SetParLimits(4, 1.1, 20.0);

        for (int iparam = 5; iparam < 9; ++iparam)
            cb_fit_model.FixParameter(iparam, background_prefit.GetParameter(iparam - 5));
        radial_histogram->Fit(&cb_fit_model, "RQ");
        for (int iparam = 5; iparam < 9; ++iparam)
            cb_fit_model.ReleaseParameter(iparam);
        // Single Fit instead of the previous three identical-args calls
        // (CODE_REVIEW §6.6).  The fitter's result is deterministic given
        // the same input + start values; re-fitting with the same options
        // changes nothing but wall-clock time.
        TFitResultPtr fit_result = radial_histogram->Fit(&cb_fit_model, "RSQE");

        cb_fit_model.SetLineColor(current_fit_line_color);
        cb_fit_model.SetLineWidth(2);
        cb_fit_model.SetLineStyle(1);
        cb_fit_model.DrawCopy("SAME");

        TF1 signal_only_component(TString::Format("signal_only_%s", radial_histogram->GetName()).Data(),
                                  crystal_ball_plus_pol3, kFitRangeLoMm, kFitRangeHiMm, 9);
        for (int iparam = 0; iparam < 9; ++iparam)
            signal_only_component.SetParameter(iparam, cb_fit_model.GetParameter(iparam));
        for (int iparam = 5; iparam < 9; ++iparam)
            signal_only_component.SetParameter(iparam, 0.);
        signal_only_component.SetLineColor(current_fit_line_color);
        signal_only_component.SetLineWidth(1);
        signal_only_component.SetLineStyle(kDashed);
        signal_only_component.DrawCopy("SAME");

        TF1 background_only_component(TString::Format("background_only_%s", radial_histogram->GetName()).Data(),
                                      "pol3", kFitRangeLoMm, kFitRangeHiMm);
        for (int iparam = 0; iparam < 4; ++iparam)
            background_only_component.SetParameter(iparam, cb_fit_model.GetParameter(5 + iparam));
        background_only_component.SetLineColor(kGray + 1);
        background_only_component.SetLineWidth(1);
        background_only_component.SetLineStyle(9);
        background_only_component.DrawCopy("SAME");

        const int kNGaussLegendrePoints = 500;
        double gauss_legendre_x[500], gauss_legendre_w[500];
        TF1::CalcGaussLegendreSamplingPoints(kNGaussLegendrePoints,
                                             gauss_legendre_x, gauss_legendre_w, 1e-10);
        const double n_gamma_integrated = signal_only_component.IntegralFast(
            kNGaussLegendrePoints, gauss_legendre_x, gauss_legendre_w,
            kFitRangeLoMm, kFitRangeHiMm);
        const double amplitude_rel_err = (cb_fit_model.GetParameter(0) > 0)
                                             ? cb_fit_model.GetParError(0) / cb_fit_model.GetParameter(0)
                                             : 0.;
        const double n_gamma_error = n_gamma_integrated * amplitude_rel_err;
        const double chi2_per_ndf_value = (fit_result->Ndf() > 0)
                                              ? fit_result->Chi2() / fit_result->Ndf()
                                              : -1.;

        mist::logger::info("[" + std::string(radial_histogram->GetName()) + "]" + "  N_gamma=" + std::to_string(n_gamma_integrated) + " +/- " + std::to_string(n_gamma_error) + "  peak_mu=" + std::to_string(cb_fit_model.GetParameter(1)) + " mm" + "  peak_sigma=" + std::to_string(cb_fit_model.GetParameter(2)) + " mm" + "  cb_alpha=" + std::to_string(cb_fit_model.GetParameter(3)) + "  cb_n=" + std::to_string(cb_fit_model.GetParameter(4)) + "  chi2/ndf=" + std::to_string(chi2_per_ndf_value));

        store_result(fit_results, radial_histogram->GetName(),
                     n_gamma_integrated, n_gamma_error,
                     cb_fit_model.GetParameter(1), cb_fit_model.GetParameter(2),
                     cb_fit_model.GetParameter(3), chi2_per_ndf_value);

        // ── Draw fit results directly on the plot (opt-in) ───────────────────
        // TPaveText in NDC coordinates sits at a fixed fraction of the pad,
        // so it overlays the plot regardless of axis ranges.
        if (draw_fit_annotation)
        {
            auto *fit_results_pave = new TPaveText(0.55, 0.65, 0.88, 0.88, "NDC");
            fit_results_pave->SetFillStyle(0);
            fit_results_pave->SetBorderSize(0);
            fit_results_pave->SetTextAlign(12);
            fit_results_pave->SetTextSize(0.035);
            fit_results_pave->SetTextColor(current_fit_line_color);
            fit_results_pave->AddText(TString::Format("N_{#gamma} = %.1f #pm %.1f",
                                           n_gamma_integrated, n_gamma_error).Data());
            fit_results_pave->AddText(TString::Format("#mu = %.2f mm",
                                           cb_fit_model.GetParameter(1)).Data());
            fit_results_pave->AddText(TString::Format("#sigma = %.2f mm",
                                           cb_fit_model.GetParameter(2)).Data());
            //fit_results_pave->AddText(Form("#chi^{2}/ndf = %.2f",
            //                               chi2_per_ndf_value));
            fit_results_pave->Draw();
        }
    };

    // ══════════════════════════════════════════════════════════════════════════
    // Drawing
    // ══════════════════════════════════════════════════════════════════════════

    gStyle->SetOptStat(0);
    gStyle->SetOptFit(0);

    TLine *vertical_dashed_line = new TLine();
    vertical_dashed_line->SetLineWidth(2);
    vertical_dashed_line->SetLineColor(kRed);
    vertical_dashed_line->SetLineStyle(kDashed);

    TLine *fit_range_start_line = new TLine(kFitRangeLoMm, 0, kFitRangeLoMm, 1e9);
    fit_range_start_line->SetLineColor(kBlue);
    fit_range_start_line->SetLineStyle(kDotted);
    fit_range_start_line->SetLineWidth(2);
    TLine *fit_range_end_line = new TLine(kFitRangeHiMm, 0, kFitRangeHiMm, 1e9);
    fit_range_end_line->SetLineColor(kBlue);
    fit_range_end_line->SetLineStyle(kDotted);
    fit_range_end_line->SetLineWidth(2);

    auto normalise_to_peak_height = [](TH1F *histogram_to_scale, TH1F *reference_histogram)
    {
        if (histogram_to_scale->GetMaximum() > 0.)
            histogram_to_scale->Scale(reference_histogram->GetMaximum() / histogram_to_scale->GetMaximum());
    };

    auto make_info_pave = [](double x1, double y1, double x2, double y2) -> TPaveText *
    {
        auto *pave = new TPaveText(x1, y1, x2, y2, "NDC");
        pave->SetFillStyle(0);
        pave->SetBorderSize(0);
        pave->SetTextAlign(12);
        pave->SetTextSize(0.035);
        return pave;
    };

    auto set_line_style = [](TH1F *histogram, int line_color)
    { histogram->SetLineColor(line_color); histogram->SetLineWidth(2); };

    // ── c1: Ring parameters — prompt (red) vs early (blue) ───────────────────
    set_line_style(h_circle_fit_x_prompt, kRed + 1);
    set_line_style(h_circle_fit_x_early, kBlue + 1);
    set_line_style(h_circle_fit_y_prompt, kRed + 1);
    set_line_style(h_circle_fit_y_early, kBlue + 1);
    set_line_style(h_circle_fit_r_prompt, kRed + 1);
    set_line_style(h_circle_fit_r_early, kBlue + 1);
    normalise_to_peak_height(h_circle_fit_x_early, h_circle_fit_x_prompt);
    normalise_to_peak_height(h_circle_fit_y_early, h_circle_fit_y_prompt);
    normalise_to_peak_height(h_circle_fit_r_early, h_circle_fit_r_prompt);

    TCanvas *canvas_ring_parameters = new TCanvas("c_ring_params", "Ring parameters", 1200, 1200);
    canvas_ring_parameters->Divide(2, 2);

    auto draw_ring_parameter_pad = [&](int pad_index,
                                       TH1F *prompt_histogram, TH1F *early_histogram,
                                       float prompt_peak_value, float early_peak_value,
                                       const char *quantity_label)
    {
        canvas_ring_parameters->cd(pad_index);
        prompt_histogram->Draw("HIST");
        early_histogram->Draw("HIST SAME");
        auto *info_pave = make_info_pave(0.12, 0.62, 0.58, 0.88);
        info_pave->AddText("#color[632]{Prompt}");
        info_pave->AddText(TString::Format("#color[632]{#mu_{%s} = %.2f mm}", quantity_label, prompt_peak_value).Data());
        info_pave->AddText("#color[600]{Early}");
        info_pave->AddText(TString::Format("#color[600]{#mu_{%s} = %.2f mm}", quantity_label, early_peak_value).Data());
        info_pave->Draw();
    };
    draw_ring_parameter_pad(1, h_circle_fit_x_prompt, h_circle_fit_x_early,
                            prompt_ring_center[0], early_ring_center[0], "x");
    draw_ring_parameter_pad(2, h_circle_fit_y_prompt, h_circle_fit_y_early,
                            prompt_ring_center[1], early_ring_center[1], "y");
    draw_ring_parameter_pad(3, h_circle_fit_r_prompt, h_circle_fit_r_early,
                            prompt_ring_center[2], early_ring_center[2], "R");
    canvas_ring_parameters->cd(4);
    h_delta_time_all_hits->Scale(1. / n_used_physics_frames);
    h_delta_time_all_hits->Draw();

    // ── c2: Hit maps ──────────────────────────────────────────────────────────
    TCanvas *canvas_hit_maps = new TCanvas("c_hit_maps", "Hit maps", 1200, 600);
    canvas_hit_maps->Divide(2, 1);
    canvas_hit_maps->cd(1);
    h_hit_map_xy->Scale(1. / n_used_physics_frames);
    h_hit_map_xy->Draw("COLZ");
    canvas_hit_maps->cd(2);
    h_hit_map_rphi->Scale(1. / n_used_physics_frames);
    h_hit_map_rphi->Draw("COLZ");
    for (auto &gap_range : kPhiGapRanges)
    {
        vertical_dashed_line->DrawLine(gap_range[0], kRadialLoMm, gap_range[0], kRadialHiMm);
        vertical_dashed_line->DrawLine(gap_range[1], kRadialLoMm, gap_range[1], kRadialHiMm);
    }

    // ── c3: Corrected radial distributions ───────────────────────────────────
    TCanvas *canvas_radial_distributions = new TCanvas("c_r_distributions",
                                                       "Corrected radial distributions", 1800, 600);
    canvas_radial_distributions->Divide(3, 1);
    auto draw_radial_pad = [&](int pad_index, TH1F *radial_histogram)
    {
        canvas_radial_distributions->cd(pad_index);
        radial_histogram->Draw();
        fit_range_start_line->DrawLine(kFitRangeLoMm, gPad->GetUymin(), kFitRangeLoMm, gPad->GetUymax());
        fit_range_end_line->DrawLine(kFitRangeHiMm, gPad->GetUymin(), kFitRangeHiMm, gPad->GetUymax());
        fit_radial_distribution(radial_histogram, true);
    };
    draw_radial_pad(1, h_radial_prompt_full);
    draw_radial_pad(2, h_radial_prompt_in_gap);
    draw_radial_pad(3, h_radial_prompt_ex_gap);

    // ── c4: Coverage maps ─────────────────────────────────────────────────────
    TCanvas *canvas_coverage_maps = new TCanvas("c_coverage", "Coverage maps", 1200, 600);
    canvas_coverage_maps->Divide(2, 1);
    canvas_coverage_maps->cd(1);
    h_coverage_map_xy->Draw("COLZ");
    canvas_coverage_maps->cd(2);
    h_coverage_map_rphi->Draw("COLZ");

    // ── c5: Per-sensor radial distributions + yield ratio ────────────────────
    TCanvas *canvas_sensor_comparison = new TCanvas("c_r_by_sensor", "By sensor type", 1200, 600);
    canvas_sensor_comparison->Divide(2, 1);
    canvas_sensor_comparison->cd(1);
    h_radial_prompt_ex_gap_1350->SetLineColor(kBlue);
    h_radial_prompt_ex_gap_1350->SetLineWidth(2);
    h_radial_prompt_ex_gap_1375->SetLineColor(kRed);
    h_radial_prompt_ex_gap_1375->SetLineWidth(2);
    {
        auto *axis_frame = (TH1F *)h_radial_prompt_ex_gap_1350->Clone("axis_frame_sensor");
        axis_frame->SetMaximum(1.2 * std::max(h_radial_prompt_ex_gap_1350->GetMaximum(),
                                              h_radial_prompt_ex_gap_1375->GetMaximum()));
        axis_frame->Draw("AXIS");
        h_radial_prompt_ex_gap_1350->Draw("SAME HIST");
        h_radial_prompt_ex_gap_1375->Draw("SAME HIST");
        fit_range_start_line->DrawLine(kFitRangeLoMm, axis_frame->GetMinimum(),
                                       kFitRangeLoMm, axis_frame->GetMaximum());
        current_fit_line_color = kBlue;
        fit_radial_distribution(h_radial_prompt_ex_gap_1350, true);
        current_fit_line_color = kRed;
        fit_radial_distribution(h_radial_prompt_ex_gap_1375, true);
        auto *sensor_legend = new TLegend(0.6, 0.7, 0.88, 0.88);
        sensor_legend->AddEntry(h_radial_prompt_ex_gap_1350, "HPK S13360-3050VS (dev 192-195)", "l");
        sensor_legend->AddEntry(h_radial_prompt_ex_gap_1375, "HPK S13360-3075VS (dev 196-199)", "l");
        sensor_legend->Draw();
    }
    canvas_sensor_comparison->cd(2);
    {
        RootHist<TH1F> h_yield_ratio_1375_over_1350(static_cast<TH1F*>(h_radial_prompt_ex_gap_1375->Clone("h_yield_ratio_1375_over_1350")));
        h_yield_ratio_1375_over_1350->Divide(h_radial_prompt_ex_gap_1350);
        h_yield_ratio_1375_over_1350->SetTitle(";R (mm);yield ratio 1375/1350");
        h_yield_ratio_1375_over_1350->SetLineColor(kBlack);
        h_yield_ratio_1375_over_1350->SetLineWidth(2);
        h_yield_ratio_1375_over_1350->Draw("HIST");
        TLine *unity_line = new TLine(kRadialLoMm, 1, kRadialHiMm, 1);
        unity_line->SetLineStyle(kDashed);
        unity_line->SetLineColor(kGray + 1);
        unity_line->Draw();
    }

    // ── c6: Raw pair timing histograms ───────────────────────────────────────
    TCanvas *canvas_pair_timing_raw = new TCanvas("c_dt_vs_dR",
                                                  "Pair timing vs spatial separation", 1200, 600);
    canvas_pair_timing_raw->Divide(2, 1);
    canvas_pair_timing_raw->cd(1);
    h_pair_dt_trig_vs_separation->Draw("COLZ");
    canvas_pair_timing_raw->cd(2);
    h_pair_dt_hit_vs_separation->Draw("COLZ");

    // ── c7: Column-normalised pair histograms — P(dt | dR) ───────────────────
    auto column_normalise_2d = [](TH2F *input_histogram) -> TH2F *
    {
        RootHist<TH2F> normalised(static_cast<TH2F*>(input_histogram->Clone(
            TString::Format("%s_col_normalised", input_histogram->GetName()).Data())));
        normalised->SetTitle(TString::Format("%s  [col-normalised]", input_histogram->GetTitle()).Data());
        for (int ix = 1; ix <= normalised->GetNbinsX(); ++ix)
        {
            double column_integral = 0.;
            for (int iy = 1; iy <= normalised->GetNbinsY(); ++iy)
                column_integral += normalised->GetBinContent(ix, iy);
            if (column_integral <= 0.)
                continue;
            for (int iy = 1; iy <= normalised->GetNbinsY(); ++iy)
            {
                normalised->SetBinContent(ix, iy, normalised->GetBinContent(ix, iy) / column_integral);
                normalised->SetBinError(ix, iy, normalised->GetBinError(ix, iy) / column_integral);
            }
        }
        return normalised;
    };

    TCanvas *canvas_pair_timing_normalised = new TCanvas("c_dt_vs_dR_norm",
                                                         "Pair timing vs dR [col-normalised]", 1200, 600);
    canvas_pair_timing_normalised->Divide(2, 1);
    canvas_pair_timing_normalised->cd(1);
    gPad->SetLogz(0);
    column_normalise_2d(h_pair_dt_trig_vs_separation)->Draw("COLZ");
    canvas_pair_timing_normalised->cd(2);
    gPad->SetLogz(0);
    column_normalise_2d(h_pair_dt_hit_vs_separation)->Draw("COLZ");

    // ── c8: Slice-fit results vs ΔR ──────────────────────────────────────────
    // Per dR column of h_pair_dt_hit_vs_separation: fit
    //   A1·G(x,0,σ1) + A2·G(x,μ2,σ2) + pol2 background
    // Prompt peak mean fixed at 0 (time-ordering construction).
    {
        auto make_profile_histogram = [&](const char *name, const char *y_axis_title) -> TH1F *
        {
            auto *profile = new TH1F(name, TString::Format(";#DeltaR_{ij} (mm);%s", y_axis_title).Data(),
                                     kPairSeparationBins, 0., kPairSeparationMaxMm);
            profile->SetMarkerStyle(20);
            profile->SetLineWidth(2);
            return profile;
        };
        TH1F *h_prompt_sigma_vs_separation = make_profile_histogram("h_prompt_sigma_vs_separation", "#sigma_{1} prompt (ns)");
        TH1F *h_satellite_mu_vs_separation = make_profile_histogram("h_satellite_mu_vs_separation", "#mu_{2} satellite (ns)");
        TH1F *h_satellite_sigma_vs_separation = make_profile_histogram("h_satellite_sigma_vs_separation", "#sigma_{2} satellite (ns)");
        TH1F *h_satellite_yield_ratio = make_profile_histogram("h_satellite_yield_ratio", "A_{2}/A_{1}");

        TF1 double_gaussian_plus_pol2("double_gaussian_plus_pol2",
                                      "[0]*TMath::Gaus(x,0,[1],true)+[2]*TMath::Gaus(x,[3],[4],true)+[5]+[6]*x+[7]*x*x",
                                      -5., 0.);
        double_gaussian_plus_pol2.SetParNames("prompt_amplitude", "prompt_sigma",
                                              "satellite_amplitude", "satellite_mu", "satellite_sigma",
                                              "background_c0", "background_c1", "background_c2");

        for (int separation_bin = 1; separation_bin <= kPairSeparationBins; ++separation_bin)
        {
            TH1D *slice_projection = h_pair_dt_hit_vs_separation->ProjectionY(
                TString::Format("slice_projection_%d", separation_bin).Data(), separation_bin, separation_bin);
            if (slice_projection->GetEntries() < 20)
            {
                delete slice_projection;
                continue;
            }

            const double slice_peak = slice_projection->GetMaximum();
            const double slice_left_bkg = slice_projection->GetBinContent(1);

            double_gaussian_plus_pol2.SetParameters(slice_peak, 0.25,
                                                    slice_peak * 0.3, -1.5, 0.5, slice_left_bkg, 0., 0.);
            double_gaussian_plus_pol2.SetParLimits(0, 0., 1e9);
            double_gaussian_plus_pol2.SetParLimits(1, 0.02, 1.0);
            double_gaussian_plus_pol2.SetParLimits(2, 0., 1e9);
            double_gaussian_plus_pol2.SetParLimits(3, -4.5, -0.3);
            double_gaussian_plus_pol2.SetParLimits(4, 0.1, 2.0);
            double_gaussian_plus_pol2.SetParLimits(5, 0., 1e9);

            TFitResultPtr slice_fit = slice_projection->Fit(&double_gaussian_plus_pol2, "RQSE");
            if (!slice_fit.Get() || !slice_fit->IsValid())
            {
                delete slice_projection;
                continue;
            }

            const double prompt_amplitude = double_gaussian_plus_pol2.GetParameter(0);
            const double prompt_amplitude_err = double_gaussian_plus_pol2.GetParError(0);
            const double prompt_sigma_val = double_gaussian_plus_pol2.GetParameter(1);
            const double prompt_sigma_err = double_gaussian_plus_pol2.GetParError(1);
            const double satellite_amplitude = double_gaussian_plus_pol2.GetParameter(2);
            const double satellite_amp_err = double_gaussian_plus_pol2.GetParError(2);
            const double satellite_mu_val = double_gaussian_plus_pol2.GetParameter(3);
            const double satellite_mu_err = double_gaussian_plus_pol2.GetParError(3);
            const double satellite_sigma_val = double_gaussian_plus_pol2.GetParameter(4);
            const double satellite_sigma_err = double_gaussian_plus_pol2.GetParError(4);
            const double amplitude_ratio = (prompt_amplitude > 0.)
                                               ? satellite_amplitude / prompt_amplitude
                                               : 0.;
            const double amplitude_ratio_err = (prompt_amplitude > 0. && satellite_amplitude > 0.)
                                                   ? amplitude_ratio * std::sqrt(
                                                                           (satellite_amp_err / satellite_amplitude) * (satellite_amp_err / satellite_amplitude) +
                                                                           (prompt_amplitude_err / prompt_amplitude) * (prompt_amplitude_err / prompt_amplitude))
                                                   : 0.;

            auto fill_bin = [&](TH1F *profile, double value, double error)
            { profile->SetBinContent(separation_bin, value); profile->SetBinError(separation_bin, error); };
            fill_bin(h_prompt_sigma_vs_separation, std::fabs(prompt_sigma_val), prompt_sigma_err);
            fill_bin(h_satellite_mu_vs_separation, satellite_mu_val, satellite_mu_err);
            fill_bin(h_satellite_sigma_vs_separation, std::fabs(satellite_sigma_val), satellite_sigma_err);
            fill_bin(h_satellite_yield_ratio, amplitude_ratio, amplitude_ratio_err);
            delete slice_projection;
        }

        TCanvas *canvas_slice_fit_results = new TCanvas("c_fit_vs_dR",
                                                        "Slice fit results vs dR", 1600, 400);
        canvas_slice_fit_results->Divide(4, 1);
        auto draw_profile_pad = [&](int pad_index, TH1F *profile_histogram, int line_color)
        {
            canvas_slice_fit_results->cd(pad_index);
            profile_histogram->SetLineColor(line_color);
            profile_histogram->SetMarkerColor(line_color);
            profile_histogram->Draw("E1");
        };
        draw_profile_pad(1, h_prompt_sigma_vs_separation, kBlue + 1);
        draw_profile_pad(2, h_satellite_mu_vs_separation, kRed + 1);
        draw_profile_pad(3, h_satellite_sigma_vs_separation, kRed + 1);
        draw_profile_pad(4, h_satellite_yield_ratio, kGreen + 2);
    }

    // ── c9: Prompt vs early — 2×3 per-sensor layout ──────────────────────────
    //  Row 1 (HPK-1350): [ Full phi ]  [ In gap ]  [ Ex gap ]
    //  Row 2 (HPK-1375): [ Full phi ]  [ In gap ]  [ Ex gap ]
    // Full phi and In gap are sensor-agnostic; Ex gap uses per-sensor histograms.
    // Early histograms normalised to prompt peak height for shape comparison.
    {
        for (auto *histogram : {h_radial_prompt_full, h_radial_prompt_in_gap,
                                h_radial_prompt_ex_gap, h_radial_prompt_ex_gap_1350,
                                h_radial_prompt_ex_gap_1375})
            set_line_style(histogram, kRed + 1);
        for (auto *histogram : {h_radial_early_full, h_radial_early_in_gap,
                                h_radial_early_ex_gap, h_radial_early_ex_gap_1350,
                                h_radial_early_ex_gap_1375})
            set_line_style(histogram, kBlue + 1);

        normalise_to_peak_height(h_radial_early_full, h_radial_prompt_full);
        normalise_to_peak_height(h_radial_early_in_gap, h_radial_prompt_in_gap);
        normalise_to_peak_height(h_radial_early_ex_gap, h_radial_prompt_ex_gap);
        normalise_to_peak_height(h_radial_early_ex_gap_1350, h_radial_prompt_ex_gap_1350);
        normalise_to_peak_height(h_radial_early_ex_gap_1375, h_radial_prompt_ex_gap_1375);

        TCanvas *canvas_early_vs_prompt = new TCanvas("c_early_vs_prompt",
                                                      "Prompt vs early per sensor", 1800, 800);
        canvas_early_vs_prompt->Divide(3, 2);

        auto draw_sensor_comparison_pad = [&](int pad_index,
                                              TH1F *prompt_histogram,
                                              TH1F *early_histogram,
                                              const char *pad_title)
        {
            canvas_early_vs_prompt->cd(pad_index);
            gPad->SetLeftMargin(0.12);
            gPad->SetTopMargin(0.12);
            const double y_axis_max = 1.2 * std::max(prompt_histogram->GetMaximum(),
                                                     early_histogram->GetMaximum());
            prompt_histogram->SetTitle("");
            prompt_histogram->GetYaxis()->SetRangeUser(0., y_axis_max);
            prompt_histogram->Draw("HIST");
            early_histogram->Draw("HIST SAME");
            fit_range_start_line->DrawLine(kFitRangeLoMm, 0., kFitRangeLoMm, y_axis_max);
            fit_range_end_line->DrawLine(kFitRangeHiMm, 0., kFitRangeHiMm, y_axis_max);
            auto *pad_title_pave = new TPaveText(0.12, 0.90, 0.88, 0.98, "NDC");
            pad_title_pave->SetFillStyle(0);
            pad_title_pave->SetBorderSize(0);
            pad_title_pave->SetTextAlign(22);
            pad_title_pave->SetTextSize(0.05);
            pad_title_pave->AddText(pad_title);
            pad_title_pave->Draw();
            auto *comparison_legend = new TLegend(0.55, 0.72, 0.88, 0.88);
            comparison_legend->SetBorderSize(0);
            comparison_legend->AddEntry(prompt_histogram, "Prompt", "l");
            comparison_legend->AddEntry(early_histogram, "Early (-1.8 ns)", "l");
            comparison_legend->Draw();
        };

        draw_sensor_comparison_pad(1, h_radial_prompt_full, h_radial_early_full,
                                   "HPK-1350  Full #phi");
        draw_sensor_comparison_pad(2, h_radial_prompt_in_gap, h_radial_early_in_gap,
                                   "HPK-1350  In gap");
        draw_sensor_comparison_pad(3, h_radial_prompt_ex_gap_1350, h_radial_early_ex_gap_1350,
                                   "HPK-1350  Ex gap");
        draw_sensor_comparison_pad(4, h_radial_prompt_full, h_radial_early_full,
                                   "HPK-1375  Full #phi");
        draw_sensor_comparison_pad(5, h_radial_prompt_in_gap, h_radial_early_in_gap,
                                   "HPK-1375  In gap");
        draw_sensor_comparison_pad(6, h_radial_prompt_ex_gap_1375, h_radial_early_ex_gap_1375,
                                   "HPK-1375  Ex gap");
    }

    // ── c10: Persistence maps for coincidence frames ──────────────────────────
    // Row 1: prompt hits.  Row 2: early hits.
    // Left: xy with shaded phi-gap wedges.  Right: (phi, R) with gap lines.
    {
        auto draw_phi_gap_wedge_xy = [&](float phi_lo, float phi_hi, float shade_radius_mm)
        {
            const int n_arc_points = 30;
            const int n_total_points = n_arc_points + 2;
            std::vector<double> wedge_x(n_total_points + 1), wedge_y(n_total_points + 1);
            wedge_x[0] = prompt_ring_center[0];
            wedge_y[0] = prompt_ring_center[1];
            for (int ipoint = 0; ipoint < n_arc_points; ++ipoint)
            {
                float arc_phi = phi_lo + (phi_hi - phi_lo) * ipoint / (float)(n_arc_points - 1);
                wedge_x[ipoint + 1] = prompt_ring_center[0] + shade_radius_mm * cosf(arc_phi);
                wedge_y[ipoint + 1] = prompt_ring_center[1] + shade_radius_mm * sinf(arc_phi);
            }
            wedge_x[n_total_points] = prompt_ring_center[0];
            wedge_y[n_total_points] = prompt_ring_center[1];
            TPolyLine *phi_gap_wedge = new TPolyLine(n_total_points + 1,
                                                     wedge_x.data(), wedge_y.data());
            phi_gap_wedge->SetFillColorAlpha(kGray + 1, 0.35);
            phi_gap_wedge->SetFillStyle(1001);
            phi_gap_wedge->SetLineColor(kGray + 2);
            phi_gap_wedge->SetLineWidth(1);
            phi_gap_wedge->DrawPolyLine(n_total_points + 1, wedge_x.data(), wedge_y.data(), "F SAME");
            phi_gap_wedge->Draw("L SAME");
        };

        const float kPhiGapShadeRadiusMm = 90.f;

        TCanvas *canvas_coincidence_persistence = new TCanvas("c_coincidence_persistence",
                                                              "Persistence maps — coincidence frames", 1200, 800);
        canvas_coincidence_persistence->Divide(2, 2);

        canvas_coincidence_persistence->cd(1);
        h_persistence_xy_prompt->SetTitle("Prompt hits (coincidence frames);x (mm);y (mm)");
        h_persistence_xy_prompt->Draw("COLZ");
        for (auto &gap_range : kPhiGapRanges)
            draw_phi_gap_wedge_xy(gap_range[0], gap_range[1], kPhiGapShadeRadiusMm);

        canvas_coincidence_persistence->cd(2);
        h_persistence_rphi_prompt->SetTitle("Prompt hits (coincidence frames);#phi (rad);R (mm)");
        h_persistence_rphi_prompt->Draw("COLZ");
        for (auto &gap_range : kPhiGapRanges)
        {
            vertical_dashed_line->DrawLine(gap_range[0], 25., gap_range[0], 125.);
            vertical_dashed_line->DrawLine(gap_range[1], 25., gap_range[1], 125.);
        }

        canvas_coincidence_persistence->cd(3);
        h_persistence_xy_early->SetTitle("Early hits (coincidence frames);x (mm);y (mm)");
        h_persistence_xy_early->Draw("COLZ");
        for (auto &gap_range : kPhiGapRanges)
            draw_phi_gap_wedge_xy(gap_range[0], gap_range[1], kPhiGapShadeRadiusMm);

        canvas_coincidence_persistence->cd(4);
        h_persistence_rphi_early->SetTitle("Early hits (coincidence frames);#phi (rad);R (mm)");
        h_persistence_rphi_early->Draw("COLZ");
        for (auto &gap_range : kPhiGapRanges)
        {
            vertical_dashed_line->DrawLine(gap_range[0], 25., gap_range[0], 125.);
            vertical_dashed_line->DrawLine(gap_range[1], 25., gap_range[1], 125.);
        }
    }

    // ── c11: Early-to-prompt spatial correlation ──────────────────────────────
    // Left: nearest prompt Hit distance per early Hit (log scale).
    //   Vertical markers at known cross-talk length scales.
    // Right: same-channel re-fire count per frame.
    {
        TCanvas *canvas_early_prompt_spatial = new TCanvas("c_early_prompt_spatial",
                                                           "Early-to-prompt spatial correlation", 1200, 500);
        canvas_early_prompt_spatial->Divide(2, 1);

        canvas_early_prompt_spatial->cd(1);
        gPad->SetLogy(1);
        h_early_to_prompt_nearest_dR->SetLineColor(kBlue + 1);
        h_early_to_prompt_nearest_dR->SetLineWidth(2);
        h_early_to_prompt_nearest_dR->Draw("HIST");

        const double nearest_dR_max = h_early_to_prompt_nearest_dR->GetMaximum();
        auto draw_marker_line = [&](float x_mm, int line_color, const char *label)
        {
            TLine *marker = new TLine(x_mm, 0., x_mm, nearest_dR_max);
            marker->SetLineColor(line_color);
            marker->SetLineStyle(kDashed);
            marker->SetLineWidth(2);
            marker->Draw();
        };
        draw_marker_line(0.f, kRed + 1, "Same channel (dR=0)");
        draw_marker_line(3.3f, kOrange + 1, "Cardinal neighbour (3.3 mm)");
        draw_marker_line(4.67f, kGreen + 2, "Diagonal neighbour (4.67 mm)");

        auto *neighbour_legend = new TLegend(0.45, 0.70, 0.88, 0.88);
        neighbour_legend->SetBorderSize(0);
        neighbour_legend->AddEntry((TObject *)nullptr, "Same channel (dR=0)", "");
        neighbour_legend->AddEntry((TObject *)nullptr, "Cardinal neighbour (3.3 mm)", "");
        neighbour_legend->AddEntry((TObject *)nullptr, "Diagonal neighbour (4.67 mm)", "");
        neighbour_legend->Draw();

        canvas_early_prompt_spatial->cd(2);
        h_same_channel_refire_count->SetLineColor(kBlue + 1);
        h_same_channel_refire_count->SetLineWidth(2);
        h_same_channel_refire_count->Draw("HIST");
    }

    // ── c12: Early Hit multiplicity per frame ─────────────────────────────────
    // A monotonically falling Poisson-like distribution rules out the hypothesis
    // of a discrete second particle (~23% of frames). Instead consistent with a
    // constant fractional early-Hit rate (time-walk or optical effect).
    {
        TCanvas *canvas_early_multiplicity = new TCanvas("c_early_multiplicity",
                                                         "Early Hit multiplicity per frame", 800, 600);
        h_early_hit_multiplicity_per_frame->SetLineColor(kBlue + 1);
        h_early_hit_multiplicity_per_frame->SetLineWidth(2);
        h_early_hit_multiplicity_per_frame->Draw("HIST");
    }

    // ── c13: dt distributions by sensor type and phi region ───────────────────
    // 2×3 grid: rows = HPK-1350 / HPK-1375, cols = Full phi / In gap / Ex gap.
    // No time window applied — prompt peak (~0 ns) and early satellite (~-1.8 ns)
    // both visible. Scaled per frame for direct comparison between sensors.
    // If the early satellite position differs between sensors or phi regions it
    // would indicate different time-walk, inconsistent with a pure 2 p.e. picture.
    {
        for (auto *histogram : {h_dt_1350_full, h_dt_1350_in_gap, h_dt_1350_ex_gap,
                                h_dt_1375_full, h_dt_1375_in_gap, h_dt_1375_ex_gap})
            histogram->Scale(1. / n_used_physics_frames);

        // Normalise 1375 to 1350 peak height per column for shape comparison
        auto norm_col = [](TH1F *h_to_scale, TH1F *h_ref)
        {
            if (h_to_scale->GetMaximum() > 0.)
                h_to_scale->Scale(h_ref->GetMaximum() / h_to_scale->GetMaximum());
        };
        norm_col(h_dt_1375_full, h_dt_1350_full);
        norm_col(h_dt_1375_in_gap, h_dt_1350_in_gap);
        norm_col(h_dt_1375_ex_gap, h_dt_1350_ex_gap);

        for (auto *histogram : {h_dt_1350_full, h_dt_1350_in_gap, h_dt_1350_ex_gap})
        {
            histogram->SetLineColor(kBlue + 1);
            histogram->SetLineWidth(2);
        }
        for (auto *histogram : {h_dt_1375_full, h_dt_1375_in_gap, h_dt_1375_ex_gap})
        {
            histogram->SetLineColor(kRed + 1);
            histogram->SetLineWidth(2);
        }

        TCanvas *canvas_dt_by_sensor = new TCanvas("c_dt_by_sensor",
                                                   "dt distributions by sensor and phi region", 1800, 800);
        canvas_dt_by_sensor->Divide(3, 2);

        auto draw_dt_pad = [&](int pad_index,
                               TH1F *histogram_1350, TH1F *histogram_1375,
                               const char *pad_title)
        {
            canvas_dt_by_sensor->cd(pad_index);
            gPad->SetLogy(1);
            gPad->SetTopMargin(0.12);
            const double y_axis_max = 1.5 * std::max(histogram_1350->GetMaximum(),
                                                     histogram_1375->GetMaximum());
            histogram_1350->SetTitle("");
            histogram_1350->GetYaxis()->SetRangeUser(1e-5, y_axis_max);
            histogram_1350->Draw("HIST");
            histogram_1375->Draw("HIST SAME");

            // Mark the prompt and early windows
            TLine *prompt_lo = new TLine(kPromptTimeCutNs[0], 0.,
                                         kPromptTimeCutNs[0], y_axis_max);
            TLine *prompt_hi = new TLine(kPromptTimeCutNs[1], 0.,
                                         kPromptTimeCutNs[1], y_axis_max);
            TLine *early_lo = new TLine(kEarlyTimeCutNs[0], 0.,
                                        kEarlyTimeCutNs[0], y_axis_max);
            TLine *early_hi = new TLine(kEarlyTimeCutNs[1], 0.,
                                        kEarlyTimeCutNs[1], y_axis_max);
            for (auto *line : {prompt_lo, prompt_hi})
            {
                line->SetLineColor(kGray + 1);
                line->SetLineStyle(kDashed);
                line->SetLineWidth(1);
                line->Draw();
            }
            for (auto *line : {early_lo, early_hi})
            {
                line->SetLineColor(kOrange + 1);
                line->SetLineStyle(kDashed);
                line->SetLineWidth(1);
                line->Draw();
            }

            auto *pad_title_pave = new TPaveText(0.12, 0.90, 0.88, 0.98, "NDC");
            pad_title_pave->SetFillStyle(0);
            pad_title_pave->SetBorderSize(0);
            pad_title_pave->SetTextAlign(22);
            pad_title_pave->SetTextSize(0.05);
            pad_title_pave->AddText(pad_title);
            pad_title_pave->Draw();

            auto *sensor_legend = new TLegend(0.55, 0.75, 0.88, 0.88);
            sensor_legend->SetBorderSize(0);
            sensor_legend->AddEntry(histogram_1350, "HPK-1350", "l");
            sensor_legend->AddEntry(histogram_1375, "HPK-1375", "l");
            sensor_legend->Draw();
        };

        draw_dt_pad(1, h_dt_1350_full, h_dt_1375_full, "Full #phi");
        draw_dt_pad(2, h_dt_1350_in_gap, h_dt_1375_in_gap, "In gap");
        draw_dt_pad(3, h_dt_1350_ex_gap, h_dt_1375_ex_gap, "Ex gap");
        // Row 2: same data, log scale already set, zoom into [-3, +1] ns
        // to better resolve the satellite structure
        auto draw_dt_pad_zoom = [&](int pad_index,
                                    TH1F *histogram_1350, TH1F *histogram_1375,
                                    const char *pad_title)
        {
            canvas_dt_by_sensor->cd(pad_index);
            gPad->SetLogy(1);
            gPad->SetTopMargin(0.12);
            histogram_1350->GetXaxis()->SetRangeUser(-3.f, 1.f);
            histogram_1375->GetXaxis()->SetRangeUser(-3.f, 1.f);
            const double y_axis_max = 1.5 * std::max(
                                                histogram_1350->GetMaximum(),
                                                histogram_1375->GetMaximum());
            histogram_1350->SetTitle("");
            histogram_1350->GetYaxis()->SetRangeUser(1e-5, y_axis_max);
            histogram_1350->Draw("HIST");
            histogram_1375->Draw("HIST SAME");

            TLine *early_lo = new TLine(kEarlyTimeCutNs[0], 0.,
                                        kEarlyTimeCutNs[0], y_axis_max);
            TLine *early_hi = new TLine(kEarlyTimeCutNs[1], 0.,
                                        kEarlyTimeCutNs[1], y_axis_max);
            for (auto *line : {early_lo, early_hi})
            {
                line->SetLineColor(kOrange + 1);
                line->SetLineStyle(kDashed);
                line->SetLineWidth(1);
                line->Draw();
            }

            auto *pad_title_pave = new TPaveText(0.12, 0.90, 0.88, 0.98, "NDC");
            pad_title_pave->SetFillStyle(0);
            pad_title_pave->SetBorderSize(0);
            pad_title_pave->SetTextAlign(22);
            pad_title_pave->SetTextSize(0.05);
            pad_title_pave->AddText(TString::Format("%s  [zoom]", pad_title).Data());
            pad_title_pave->Draw();

            auto *sensor_legend = new TLegend(0.55, 0.75, 0.88, 0.88);
            sensor_legend->SetBorderSize(0);
            sensor_legend->AddEntry(histogram_1350, "HPK-1350", "l");
            sensor_legend->AddEntry(histogram_1375, "HPK-1375", "l");
            sensor_legend->Draw();
        };

        draw_dt_pad_zoom(4, h_dt_1350_full, h_dt_1375_full, "Full #phi");
        draw_dt_pad_zoom(5, h_dt_1350_in_gap, h_dt_1375_in_gap, "In gap");
        draw_dt_pad_zoom(6, h_dt_1350_ex_gap, h_dt_1375_ex_gap, "Ex gap");
    }

    // ══════════════════════════════════════════════════════════════════════════
    // Persist fit results
    // ══════════════════════════════════════════════════════════════════════════
    {
        const std::vector<std::tuple<std::string, std::string, std::string>> kHistogramResultMap = {
            {"h_radial_prompt_full", "all", "full"},
            {"h_radial_prompt_in_gap", "all", "in_gap"},
            {"h_radial_prompt_ex_gap", "all", "ex_gap"},
            {"h_radial_prompt_ex_gap_1350", "1350", "ex_gap"},
            {"h_radial_prompt_ex_gap_1375", "1375", "ex_gap"},
        };
        const std::vector<std::pair<std::string, std::string>> kQuantityKeyMap = {
            {"N_gamma", "n_gamma"},
            {"N_gamma_err", "n_gamma_err"},
            {"mu", "mu"},
            {"sigma", "sigma"},
            {"gs_frac", "gs_frac"},
            {"chi2_ndf", "chi2_ndf"},
        };

        AnalysisResults analysis_results_store(data_repository + "/standard_results.root");
        ResultMap quantities_to_store;

        for (const auto &[histogram_name, sensor_tag, scope_prefix] : kHistogramResultMap)
        {
            const std::string n_gamma_key = histogram_name + ".N_gamma";
            const std::string n_gamma_err_key = histogram_name + ".N_gamma_err";
            if (fit_results.count(n_gamma_key))
            {
                double n_gamma_val = fit_results.at(n_gamma_key);
                double n_gamma_err_val = fit_results.count(n_gamma_err_key)
                                             ? fit_results.at(n_gamma_err_key)
                                             : 0.;
                quantities_to_store[{run_name, sensor_tag, scope_prefix + ".n_gamma"}] = {n_gamma_val, n_gamma_err_val};
            }
            for (const auto &[fit_result_suffix, store_quantity_name] : kQuantityKeyMap)
            {
                if (store_quantity_name == "n_gamma" || store_quantity_name == "n_gamma_err")
                    continue;
                const std::string fit_result_key = histogram_name + "." + fit_result_suffix;
                if (fit_results.count(fit_result_key))
                    quantities_to_store[{run_name, sensor_tag, scope_prefix + "." + store_quantity_name}] = {fit_results.at(fit_result_key), 0.};
            }
        }

        analysis_results_store.update(quantities_to_store);
        mist::logger::info("[photon_number] Persisted " + std::to_string(quantities_to_store.size()) + " results for run " + run_name);
    }
}