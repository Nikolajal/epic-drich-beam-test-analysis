#include "utility.h"
#include "TH1.h"
#include "TH2.h"
#include "TF1.h"
#include "TNamed.h"
#include "TCanvas.h"
#include "TPaveText.h"
#include "TStyle.h"
#include "TROOT.h"
#include "TTree.h"
#include "TFile.h"
#include "mapping.h"
#include "math.h"
#include "TProfile.h"
#include "parallel_streaming_framer.h"
#include "alcor_recodata.h"
#include "alcor_spilldata.h"
#include "writers/lightdata.h"
#include "writers/recodata.h"
//  Live-QA pipeline (DISCUSSION § 2.6): coverage map + eff(R) helpers
//  + per-ring fit_circle re-run on mask-tagged hits → N_photons /
//  radial(R) observables filled inline.
#include "util/radiator_efficiency.h"
#include "util/circle_fit.h"
#include "util/config_reader.h"
#include <set>
#include <memory>
#include <atomic>
#include <future>
#include <thread>

// ─────────────────────────────────────────────────────────────────────
//  Per-frame compute result struct (DISCUSSION § 2.7).
//
//  Produced by the pure-compute pass over each frame (see lambda
//  `process_frame_pure` in `recodata_writer`).  Drained in frame order
//  by the serial finalize lambda (`drain_frame_result`) which does the
//  shared-state mutations: histogram fills, recodata add_*, tree Fill,
//  per-spill counter increments.
//
//  Separation is a prerequisite for the within-spill multithreading
//  (Stage 2): the compute is parallelisable; the drain is not.
// ─────────────────────────────────────────────────────────────────────
namespace {

struct RingFitResult
{
    bool   fit_ok    = false;
    int    n_hits    = 0;
    float  cx        = 0.f;
    float  cy        = 0.f;
    float  R         = 0.f;
    float  sigma_r   = 0.f;   ///< per-ring RMS of radial residuals
    float  f_coverage = 0.f;
    std::vector<float> radial_per_hit;   ///< |hit − fit-centre| per ring hit
    std::vector<float> loo_residuals;    ///< r_hit_i − R_-i per ring hit (LOO)
};

struct FrameResult
{
    int  i_frame   = -1;       ///< index in frames_in_spill
    bool accepted  = false;    ///< neither rejected nor edge-only
    bool rejected  = false;    ///< duplicate trigger detected → drop frame
    bool had_edge  = false;    ///< at least one edge-rejected trigger

    //  Hist-fill payloads — recorded in compute, played back in drain.
    //  `(reg_bin_centre, value)` tuples; the drain just does
    //  `hist->Fill(reg_bin_centre, value)`.
    std::vector<std::pair<float, float>> edge_fills;           ///< h_edge_trigger_position
    std::vector<std::pair<float, float>> trigger_qa_fills;     ///< h_trigger_qa (the 1.5 / 2.5 y values)
    //  Time-diff fills: (trigger_index, Δt_ns).  Drain looks up /
    //  lazily creates the per-trigger hist before filling.
    std::vector<std::pair<uint8_t, float>> time_diff_fills;

    std::map<uint8_t, TriggerEvent> accepted_triggers;
    bool frame_is_physics      = false;  ///< increments n_physics_per_spill
    bool frame_has_second_ring = false;

    RingFitResult first;
    RingFitResult second;
};

} // namespace

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill,
    bool force_recodata_rebuild,
    bool force_lightdata_rebuild,
    std::string mapping_conf,
    std::string trigger_conf,
    std::string framer_conf)
{
    //  Clean PDF rendering — no stats box on any saved canvas.
    //  Applies globally for the rest of this process; affects both
    //  the radial fit canvases and the σ-vs-N canvases.
    gStyle->SetOptStat(0);

    //  ROOT thread-safety: required for the within-spill
    //  multithreading (DISCUSSION § 2.7).  Without this call, ROOT's
    //  internal global state (TROOT registries, gROOT->Get*, the
    //  TF1/TFormula bookkeeping touched by every `ROOT::Fit::Fitter`
    //  construction inside `fit_circle`) is protected by a process-
    //  wide mutex that effectively serialises the worker threads.
    //  With it, ROOT becomes properly reentrant and we get the
    //  expected ~N× speedup on multi-core.  Idempotent — safe to
    //  call once per recodata_writer invocation.
    ROOT::EnableThreadSafety();

    //  Framer configuration
    auto framer_cfg = FramerConfReader(framer_conf);

    //  Input file — open lightdata.root, auto-rebuild via lightdata_writer
    //  if missing/corrupt or if force_lightdata_rebuild is set (CODE_REVIEW §D-06).
    //  TFilePtr is owning: closes + deletes on every exit path.
    std::string input_filename = data_repository + "/" + run_name + "/lightdata.root";
    TFilePtr input_file(TFile::Open(input_filename.c_str(), "READ"));
    if (!input_file || input_file->IsZombie() || force_lightdata_rebuild)
    {
        mist::logger::warning("(recodata_writer) " + input_filename +
                              " missing, corrupt, or rebuild forced — running lightdata_writer");
        // Default trailing args (calibration / framer config) preserve the
        // previous helper's hardcoded behaviour.  Callers that need to
        // forward custom config paths should call lightdata_writer directly
        // before recodata_writer rather than relying on this fallback.
        lightdata_writer(data_repository, run_name, max_spill,
                         force_lightdata_rebuild, /*requested_n_threads=*/-1);
        input_file.reset(TFile::Open(input_filename.c_str(), "READ"));
        if (!input_file || input_file->IsZombie())
        {
            mist::logger::error("(recodata_writer) Could not open " + input_filename +
                                " even after rebuild — aborting");
            return;
        }
    }

    //  Link lightdata tree locally — use TFile::Get<TTree> which returns the
    //  correctly typed pointer (cleaner than a C-style cast that can mask a
    //  type mismatch).  Bail out if the branch is missing.
    auto *lightdata_tree = input_file->Get<TTree>("lightdata");
    if (!lightdata_tree)
    {
        mist::logger::error(TString::Format("(recodata_writer) 'lightdata' tree missing in %s",
                                 input_filename.c_str()).Data());
        return;
    }
    AlcorSpilldata *spilldata = new AlcorSpilldata();
    spilldata->link_to_tree(lightdata_tree);

    AlcorFinedata::read_calib_from_file(data_repository + "/" + run_name + "/fine_calib.txt");

    auto fine_time_calib_th2f = input_file->Get<TH2F>("h_fine_calib");
    if (!fine_time_calib_th2f)
    {
        mist::logger::error(TString::Format("(recodata_writer) 'h_fine_calib' histogram missing in %s",
                                 input_filename.c_str()).Data());
        return;
    }
    AlcorFinedata::generate_calibration(fine_time_calib_th2f, true);
    //  Calibration table is now built; signal immutability so per-Hit
    //  AlcorFinedata::get_phase() readers skip the shared_mutex
    //  (CODE_REVIEW §3.1).  No worker threads have spawned yet.
    AlcorFinedata::freeze_calibration();

    //  Progress tracking — multi-bar with one subtask (per-frame post-processing).
    //  Main bar shows overall spill progress; the subtask clock is restarted
    //  at the head of each spill via progress_bars.restart() so it reflects
    //  THIS spill's reconstruction time, not the cumulative total.
    mist::logger::MultiProgressBar progress_bars(mist::logger::BarStyle::Block);
    progress_bars.set_unit("spills");
    auto &post_processing = progress_bars.add_subtask("post-processing");

    //  Generate Mapping
    Mapping current_mapping(mapping_conf);

    //  Build trigger registry from config
    //  The registry maps raw uint8 trigger values to a dense ordered list of
    //  (value, name) pairs — config-defined triggers first, built-in defaults
    //  after. This gives contiguous histogram bins with meaningful labels,
    //  avoiding the 252 empty bins you'd get from a raw 0–255 axis.
    auto trigger_configs = trigger_conf_reader(trigger_conf);
    TriggerRegistry registry(trigger_configs);
    const int n_triggers = registry.size();

    //  Get number of spills, limited to maximum requested spills
    auto n_spills = lightdata_tree->GetEntries();
    auto all_spills = std::min((int)n_spills, (int)max_spill);

    //  Prepare output file
    std::string outname = data_repository + "/" + run_name + "/recodata.root";
    if (std::filesystem::exists(outname) && !force_recodata_rebuild)
    {
        mist::logger::info(TString::Format("Output file already exists, skipping: %s", outname.c_str()).Data());
        return;
    }

    TFilePtr output_file(TFile::Open(outname.c_str(), "RECREATE"));
    if (!output_file || output_file->IsZombie())
    {
        mist::logger::error(TString::Format("(recodata_writer) Failed to create output file %s", outname.c_str()).Data());
        return;
    }
    TTree *recodata_tree = new TTree("recodata", "Recodata tree");
    AlcorRecodata recodata;
    recodata.write_to_tree(recodata_tree);

    //  Cache channel positions from Mapping

    // Phase 5: iterate (device, chip, channel) directly via the
    // GlobalIndex overload and key the cache by `4 * channel_ordinal` —
    // matches the position-cache convention in Mapping.cxx and the
    // MIST HoughTransform `lut_key`.
    std::map<int, std::array<float, 2>> index_to_hit_xy;
    {
        constexpr int kDeviceLo  = 192;
        constexpr int kDeviceHi  = 224;
        const int max_chip       = ::gidx::kUsesSplitInTwo ? 4 : 8;
        constexpr int kChannelHi = 64;
        for (int device = kDeviceLo; device < kDeviceHi; ++device)
            for (int chip = 0; chip < max_chip; ++chip)
                for (int channel = 0; channel < kChannelHi; ++channel)
                {
                    const auto gi = ::GlobalIndex::from_components(
                        device, /*fifo=*/0, chip, channel, /*tdc=*/0);
                    auto position = current_mapping.get_position_from_global_index(gi);
                    if (!position)
                        continue;
                    if (fabs((*position)[0]) < 5 && fabs((*position)[1]) < 5)
                        continue;
                    index_to_hit_xy[4 * gi.channel_ordinal()] = *position;
                }
    }

    //  (Smallest-r channel diagnostic removed: served its purpose
    //  during the PDU 99 phantom-position investigation.  The
    //  underlying Mapping fix in `src/mapping.cxx` is the durable
    //  fix; the channel set is now correct by construction.  Restore
    //  this diagnostic via git history if a similar Mapping
    //  regression is ever suspected.)

    //  Edge rejection window: 25 ns fixed.  Used to be converted to
    //  clock cycles and compared against `current_trigger.fine_time`,
    //  but `fine_time` is documented as **ns** across the codebase —
    //  the comparison was a hidden units mismatch that rejected
    //  almost all triggers (their fine_time in ns was always greater
    //  than the ~4-cc threshold flipped against frame_size_cc).  Now
    //  both sides of the inequality are in ns.  `frame_size` from
    //  framer config is in clock cycles → multiply by CC_TO_NS to
    //  match.
    constexpr float edge_rejection_ns = 25.f;

    //  ── Trigger selection QA histograms ──────────────────────────────────────
    //  X axis uses registry position (dense, named) instead of raw trigger index.
    //  registry.index_of(raw) maps any observed trigger value to its bin.
    //
    //  h_trigger_qa: per-trigger-type outcome counts
    //    Y bin 1 = accepted, 2 = edge-rejected, 3 = duplicate-rejected
    //  h_frames_per_spill: frame counts per spill per outcome category
    //  h_edge_trigger_position: coarse time of edge-rejected triggers,
    //    to verify the 25 ns cut is well placed

    RootHist<TH2F> h_trigger_qa(
        "h_trigger_qa",
        "Trigger selection QA;trigger;outcome",
        n_triggers, 0, n_triggers,
        3, 0, 3);
    for (int i = 0; i < n_triggers; ++i)
        h_trigger_qa->GetXaxis()->SetBinLabel(i + 1, registry.triggers[i].second.c_str());
    h_trigger_qa->GetYaxis()->SetBinLabel(1, "accepted");
    h_trigger_qa->GetYaxis()->SetBinLabel(2, "edge-rejected");
    h_trigger_qa->GetYaxis()->SetBinLabel(3, "duplicate-rejected");

    RootHist<TH2F> h_frames_per_spill(
        "h_frames_per_spill",
        "Frame counts per spill;spill;category",
        all_spills, 0, all_spills,
        4, 0, 4);
    h_frames_per_spill->GetYaxis()->SetBinLabel(1, "total");
    h_frames_per_spill->GetYaxis()->SetBinLabel(2, "accepted");
    h_frames_per_spill->GetYaxis()->SetBinLabel(3, "had edge trigger");
    h_frames_per_spill->GetYaxis()->SetBinLabel(4, "duplicate-rejected");

    RootHist<TH2F> h_edge_trigger_position(
        "h_edge_trigger_position",
        "Position of edge-rejected triggers;trigger;coarse time (cc)",
        n_triggers, 0, n_triggers,
        500, 0, framer_cfg.frame_size);
    for (int i = 0; i < n_triggers; ++i)
        h_edge_trigger_position->GetXaxis()->SetBinLabel(i + 1, registry.triggers[i].second.c_str());

    std::unordered_map<int, RootHist<TH1F>> h_trigger_time_diff_w_cherenkov;

    // ─────────────────────────────────────────────────────────────────────
    //  Live-QA radiator pipeline (DISCUSSION § 2.6)
    // ─────────────────────────────────────────────────────────────────────
    //  Per-ring photon counting + radial distributions in recodata, so
    //  beam-test operators see Cherenkov physics observables live (the
    //  offline `photon_number_new.cpp` macro becomes a thin plotter).
    //
    //  Pipeline:
    //    init   : coverage map from `index_to_hit_xy` (geometry-only).
    //    frame  : for each frame with a Hough ring trigger, run
    //             `fit_circle` on the lightdata-tagged hits to recover
    //             per-event (cx, cy, R), then fill N_hits / N_photons /
    //             f_coverage / radial(R).  No `TriggerEvent` schema
    //             bump — the mask bits already carry the assignment.
    //    final  : derive `eff(R)` from the coverage map, divide the
    //             radial hists by it, write everything to the
    //             "Radiator" output subfolder.
    //
    //  V1 scope: no φ-gap split, no sensor-model split, no time-window
    //  variants.  All deferred to a finer-analysis follow-up.
    auto recodata_cfg = recodata_conf_reader("conf/recodata.toml");

    //  Coverage map is now built at FINALIZE (DISCUSSION § 2.6, spill-
    //  by-spill active-channel correction).  During the spill loop we
    //  accumulate two pieces of information per spill:
    //
    //    * `n_physics_per_spill[i]` — number of HOUGH_RING_FOUND
    //      triggers in spill i.  Sets the spill's weight in the
    //      coverage map: `weight_i = n_physics_i / Σ n_physics`.
    //    * `active_channels_per_spill[i]` — set of channel keys with
    //      at least one Cherenkov hit in any FIRST_FRAMES frame of
    //      spill i.  Channels in this set are "active" for that spill.
    //
    //  At finalize, the per-channel weight passed to
    //  `build_coverage_map` is:
    //
    //     channel_weight[k] = Σ over spills active in of weight_i
    //
    //  This matches the offline `photon_number_new.cpp` macro
    //  exactly so eff(R) values stay comparable.  Permanently-dead
    //  channels (never in any spill's active set) get weight 0 and
    //  contribute nothing.  Channels active for, e.g., 50 % of
    //  spills (weighted by physics rate) get weight ≈ 0.5.
    //
    //  The `RootHist<TH2F>` is constructed empty (no helper call at
    //  init) — we attach the computed TH2F at finalize via assignment.
    std::vector<int>                  n_physics_per_spill(all_spills, 0);
    std::vector<std::set<int>>        active_channels_per_spill(all_spills);

    //  Per-ring QA hists.  Same binning as the coverage map's R axis
    //  on radial hists so `eff(R)` can be `Divide`d cleanly at finalize.
    const int   radial_n_bins = recodata_cfg.n_r_bins_coverage;
    const float radial_lo_mm  = recodata_cfg.r_min_coverage_mm;
    const float radial_hi_mm  = recodata_cfg.r_max_coverage_mm;

    //  N-hits axis range used by every N-hits hist below — 1D and 2D.
    //  Observed beam-test max is ~20 hits/ring; 25 leaves a 5-bin
    //  headroom for outliers without wasting half the axis on empty
    //  bins.  1 hit / bin (integer X).  Tighten to 20 if rings get
    //  trimmed further; widen if a future run sees N > 25.
    constexpr int   kNHitsBins   = 25;
    constexpr float kNHitsXLo    = 0.f;
    constexpr float kNHitsXHi    = 25.f;

    RootHist<TH1F> h_nhits_first ("h_nhits_first",  ";N hits in ring 1;Events",
                                   kNHitsBins, kNHitsXLo, kNHitsXHi);
    RootHist<TH1F> h_nhits_second("h_nhits_second", ";N hits in ring 2;Events",
                                   kNHitsBins, kNHitsXLo, kNHitsXHi);

    RootHist<TH1F> h_nphotons_first ("h_nphotons_first",  ";N photons (eff-corrected) ring 1;Events", 100, 0, 100);
    RootHist<TH1F> h_nphotons_second("h_nphotons_second", ";N photons (eff-corrected) ring 2;Events", 100, 0, 100);

    RootHist<TH1F> h_f_coverage_first ("h_f_coverage_first",  ";f_{coverage} ring 1;Events", 100, 0.f, 1.f);
    RootHist<TH1F> h_f_coverage_second("h_f_coverage_second", ";f_{coverage} ring 2;Events", 100, 0.f, 1.f);

    //  Radial-hit distributions — binned 1 mm/bin (NOT the coarser
    //  radial_n_bins used by h_R_*).  Finer binning here for the
    //  CB+pol3 fit: the macro convention quotes σ_peak in mm so a
    //  binning matched to that precision avoids smearing.  Bin count
    //  is computed from the coverage R range.
    const int   radial_hist_n_bins = static_cast<int>(
        std::round(recodata_cfg.r_max_coverage_mm - recodata_cfg.r_min_coverage_mm));
    RootHist<TH1F> h_radial_first ("h_radial_first",  ";R (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_radial_second("h_radial_second", ";R (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    //  Dual / solo splits for the first-ring radial distribution.
    //  Same predicate as the vs_n splits (frame has second ring?).
    //  Used by the Crystal-Ball + pol3 fit at finalize (DISCUSSION § 2.6)
    //  to extract N_γ separately for clean two-radiator events vs
    //  single-radiator events.  Second ring is dual-by-definition.
    RootHist<TH1F> h_radial_first_dual("h_radial_first_dual", ";R (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_radial_first_solo("h_radial_first_solo", ";R (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);

    //  Headline physics observables (DISCUSSION § 2.6).  These four
    //  per-ring quantities are what beam-test operators care about:
    //    * fitted ring radius (gives Cherenkov angle / velocity / PID),
    //    * single-photon spatial resolution σ_r = std(|r_hit − R_fit|)
    //      per ring (run-level σ_single = mean of this hist's core),
    //    * (R, n_hits) correlation — bad fits surface as off-diagonal
    //      junk,
    //    * (cx, cy) per ring — beam centre + drift over the run.
    //
    //  Same R binning as h_radial_* so the two can be overlaid in
    //  TBrowser.  cx/cy half-range hardcoded to 25 mm (matches the
    //  lightdata-side `centre_xy_half_range_mm` default); tighten if
    //  needed once beam stability is characterised.
    RootHist<TH1F> h_R_first ("h_R_first",  ";R_{fit} (mm);events", radial_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_R_second("h_R_second", ";R_{fit} (mm);events", radial_n_bins, radial_lo_mm, radial_hi_mm);
    //  Dual / solo splits for the first-ring fitted radius.  Same
    //  predicate as the radial-hist splits (frame has second ring?).
    //  Two roles: (1) per-event physics observable — compare R
    //  between clean two-radiator events and single-ring ones;
    //  (2) authoritative ring-count source for the per-ring N_γ
    //  calc in the CB+pol3 fit at finalize (each entry = one
    //  successful ring fit).
    RootHist<TH1F> h_R_first_dual("h_R_first_dual", ";R_{fit} (mm);events", radial_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_R_first_solo("h_R_first_solo", ";R_{fit} (mm);events", radial_n_bins, radial_lo_mm, radial_hi_mm);

    RootHist<TH1F> h_sigma_first ("h_sigma_first",  ";#sigma_{single} (mm);events", 100, 0.f, 5.f);
    RootHist<TH1F> h_sigma_second("h_sigma_second", ";#sigma_{single} (mm);events", 100, 0.f, 5.f);

    RootHist<TH2F> h_R_vs_nhits_first ("h_R_vs_nhits_first",  ";N hits;R_{fit} (mm)",
                                        kNHitsBins, kNHitsXLo, kNHitsXHi,
                                        radial_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH2F> h_R_vs_nhits_second("h_R_vs_nhits_second", ";N hits;R_{fit} (mm)",
                                        kNHitsBins, kNHitsXLo, kNHitsXHi,
                                        radial_n_bins, radial_lo_mm, radial_hi_mm);

    //  cx / cy half-range: hard-code to 25 mm for now; this is the
    //  same default as the lightdata-side QA's `centre_xy_half_range_mm`.
    //  Bin width 1 mm = generous for visual ring-centre clusters.
    constexpr float kCentreXyHalfRangeMm = 25.f;
    constexpr int   kCentreXyBins        = 50;
    RootHist<TH2F> h_centre_xy_first ("h_centre_xy_first",  ";c_{x} (mm);c_{y} (mm)",
                                       kCentreXyBins, -kCentreXyHalfRangeMm, kCentreXyHalfRangeMm,
                                       kCentreXyBins, -kCentreXyHalfRangeMm, kCentreXyHalfRangeMm);
    RootHist<TH2F> h_centre_xy_second("h_centre_xy_second", ";c_{x} (mm);c_{y} (mm)",
                                       kCentreXyBins, -kCentreXyHalfRangeMm, kCentreXyHalfRangeMm,
                                       kCentreXyBins, -kCentreXyHalfRangeMm, kCentreXyHalfRangeMm);

    //  Per-hit radial residual vs N_hits (LEAVE-ONE-OUT fit).
    //
    //  For each hit i in a ring, run `fit_circle` with `exclude_points
    //  = {i}` to get the leave-i-out fit (cx_-i, cy_-i, R_-i).  Then
    //  the per-hit residual is
    //
    //      Δr_i = sqrt((x_i − cx_-i)² + (y_i − cy_-i)²) − R_-i
    //
    //  which is UNBIASED — hit i did not participate in the fit it's
    //  being measured against.
    //
    //  Filled per-hit (not per-event), one entry per surviving hit.
    //  Slice by N_hits (X) and Gaussian-fit each slice (Y) offline to
    //  extract σ_photon(N).  The expected behaviour is **flat** in N:
    //  the per-hit residual width = σ_photon regardless of N.  Any
    //  N-dependence flags correlated noise (afterpulses, etc.) — see
    //  the time-aware-assignment open item, task #33.
    //
    //  Residual range ±5 mm easily covers physical σ_photon (~1 mm)
    //  + outliers; 100 bins = 0.1 mm/bin.
    //
    //  Cost: ~N extra fits per ring per event.  At N ~ 12 hits and
    //  21k events × 2 rings, ~25 s extra per run.  Acceptable; see
    //  DISCUSSION § 2.6 for the rationale (replaces the biased
    //  `h_sigma_*` and the wrong-observable `h_fit_sigma_R_vs_n_*`
    //  that this hist supersedes).
    RootHist<TH2F> h_residual_vs_n_first ("h_residual_vs_n_first",  ";N hits;r_{hit} - R_{-i} (mm)",
                                           kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);
    RootHist<TH2F> h_residual_vs_n_second("h_residual_vs_n_second", ";N hits;r_{hit} - R_{-i} (mm)",
                                           kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);

    //  Dual/solo splits for the first ring's vs_n observables.  Same
    //  semantics as the lightdata-side `_dual` / `_solo` splits already
    //  in StreamingHoughQA: filled per the (frame_has_second_ring)
    //  predicate computed once per frame.  Lets the operator A/B the
    //  first-ring quality between events where a second ring fired
    //  (dual = clean two-radiator events) and where it didn't (solo =
    //  potentially fake-ring-contaminated single-ring sample).  Second
    //  ring is dual-by-definition so needs no _dual/_solo split.
    RootHist<TH2F> h_R_vs_nhits_first_dual ("h_R_vs_nhits_first_dual",  ";N hits;R_{fit} (mm)",
                                              kNHitsBins, kNHitsXLo, kNHitsXHi,
                                              radial_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH2F> h_R_vs_nhits_first_solo ("h_R_vs_nhits_first_solo",  ";N hits;R_{fit} (mm)",
                                              kNHitsBins, kNHitsXLo, kNHitsXHi,
                                              radial_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH2F> h_residual_vs_n_first_dual("h_residual_vs_n_first_dual", ";N hits;r_{hit} - R_{-i} (mm)",
                                                kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);
    RootHist<TH2F> h_residual_vs_n_first_solo("h_residual_vs_n_first_solo", ";N hits;r_{hit} - R_{-i} (mm)",
                                                kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);

    //  Re-fit helper.  Captures `index_to_hit_xy` + cfg by reference so
    //  it's cheap to call per-frame per-ring.  Returns true on success
    //  (NaN / R<=0 / too few hits → no fill, returns false), so the
    //  caller can short-circuit downstream work for failed rings.
    //
    //  Initial guess: hit centroid as (x0, y0), median |r − centroid|
    //  as R0.  Robust to per-event variation, no dependence on TOML
    //  fit_circle_init from the streaming-Hough config.
    //  Per-ring re-fit + fill helper.  Mask-bit selector + 8 output
    //  hist pointers.  Any pointer may be nullptr — the corresponding
    //  fill is skipped.  Returns true if the fit was attempted AND
    //  converged (NaN / R<=0 → false); see DISCUSSION § 2.6 for the
    //  full semantics of each output.
    struct RingFillHists
    {
        TH1F *h_nhits           = nullptr;
        TH1F *h_nphotons        = nullptr;
        TH1F *h_fcov            = nullptr;
        TH1F *h_radial          = nullptr;
        TH1F *h_R               = nullptr;  ///< fitted ring radius
        TH1F *h_sigma           = nullptr;  ///< per-ring RMS of radial residuals (biased — see DISCUSSION § 2.6)
        TH2F *h_R_vs_nhits      = nullptr;  ///< correlation
        TH2F *h_centre_xy       = nullptr;  ///< fit centre map
        TH2F *h_residual_vs_n   = nullptr;  ///< per-hit LOO residual (mm) vs N_hits — unbiased σ_photon
        // Optional dual/solo-split twins for vs_n observables.  Caller
        // sets to either the _dual or _solo hist of the appropriate
        // ring slot based on the (frame_has_second_ring) predicate.
        TH2F *h_R_vs_nhits_split    = nullptr;
        TH2F *h_residual_vs_n_split = nullptr;
        TH1F *h_radial_split        = nullptr;  ///< dual/solo split of h_radial
        TH1F *h_R_split             = nullptr;  ///< dual/solo split of h_R
    };
    //  Pure-compute ring fit — no histogram fills, no shared-state
    //  mutation.  Returns a RingFitResult (defined in the anonymous
    //  namespace at file scope).  Safe to call from worker threads
    //  (DISCUSSION § 2.7).  The drain — `fill_ring_hists` below, or
    //  the existing `refit_and_fill_ring` for the serial code path —
    //  consumes the result and does the hist mutations.
    //
    //  `do_loo`: skip the leave-one-out residual loop when no
    //  downstream observable needs it (saves ~N extra fit_circle
    //  calls per ring).
    auto compute_ring_fit_pure = [&](HitMask ring_bit,
                                     AlcorLightdata &lightdata,
                                     bool do_loo) -> RingFitResult
    {
        RingFitResult out;
        std::vector<std::array<float, 2>> ring_hits;
        ring_hits.reserve(40);
        for (const auto &hit_struct : lightdata.get_cherenkov_hits_link())
        {
            AlcorFinedata fh(hit_struct);
            if (fh.has_mask_bit(ring_bit))
                ring_hits.push_back({fh.get_hit_x(), fh.get_hit_y()});
        }
        out.n_hits = static_cast<int>(ring_hits.size());
        if (out.n_hits < recodata_cfg.min_hits_per_ring)
            return out;  // fit_ok stays false

        //  Centroid + median radial as initial guess.
        float sum_x = 0.f, sum_y = 0.f;
        for (const auto &p : ring_hits) { sum_x += p[0]; sum_y += p[1]; }
        const float cx0 = sum_x / out.n_hits;
        const float cy0 = sum_y / out.n_hits;
        float sum_r = 0.f;
        for (const auto &p : ring_hits)
            sum_r += std::hypot(p[0] - cx0, p[1] - cy0);
        const float R0 = sum_r / out.n_hits;

        const auto fit = fit_circle(ring_hits, {cx0, cy0, R0}, /*fix_XY=*/false);
        out.cx = fit[0][0];
        out.cy = fit[1][0];
        out.R  = fit[2][0];
        if (!std::isfinite(out.cx) || !std::isfinite(out.cy) ||
            !std::isfinite(out.R)  || out.R <= 0.f)
            return out;  // fit_ok stays false, but n_hits is populated

        //  Per-ring σ from radial residuals.
        float sum_dev = 0.f, sum_dev_sq = 0.f;
        out.radial_per_hit.reserve(out.n_hits);
        for (const auto &p : ring_hits)
        {
            const float r_hit = std::hypot(p[0] - out.cx, p[1] - out.cy);
            const float dev   = r_hit - out.R;
            sum_dev    += dev;
            sum_dev_sq += dev * dev;
            out.radial_per_hit.push_back(r_hit);
        }
        const float mean_dev = sum_dev / out.n_hits;
        const float variance = std::max(0.f, sum_dev_sq / out.n_hits - mean_dev * mean_dev);
        out.sigma_r = std::sqrt(variance);

        out.f_coverage = util::radiator_efficiency::azimuthal_coverage_fraction(
            index_to_hit_xy, out.cx, out.cy, out.R, recodata_cfg.delta_r_for_coverage_mm);

        //  LOO residuals (optional — gate on do_loo).
        if (do_loo)
        {
            const std::array<float, 3> loo_seed = {out.cx, out.cy, out.R};
            out.loo_residuals.reserve(out.n_hits);
            for (int i_excl = 0; i_excl < out.n_hits; ++i_excl)
            {
                const auto loo_fit = fit_circle(ring_hits, loo_seed,
                                                /*fix_XY=*/false,
                                                /*exclude_points=*/{i_excl});
                const float cx_loo = loo_fit[0][0];
                const float cy_loo = loo_fit[1][0];
                const float R_loo  = loo_fit[2][0];
                if (!std::isfinite(cx_loo) || !std::isfinite(cy_loo) ||
                    !std::isfinite(R_loo)  || R_loo <= 0.f)
                    continue;
                const float r_loo = std::hypot(ring_hits[i_excl][0] - cx_loo,
                                               ring_hits[i_excl][1] - cy_loo);
                out.loo_residuals.push_back(r_loo - R_loo);
            }
        }

        out.fit_ok = true;
        return out;
    };

    //  Drain helper: replay the side effects (hist fills) given a
    //  precomputed RingFitResult and a RingFillHists target.  This is
    //  the serial part that mutates shared state.
    auto fill_ring_hists = [&](const RingFitResult &r, const RingFillHists &h)
    {
        if (r.n_hits == 0) return;
        //  Always fill h_nhits if a hist is present (matches original
        //  semantics: even fit failures with enough hits get counted).
        if (h.h_nhits) h.h_nhits->Fill(r.n_hits);
        if (!r.fit_ok) return;  // remaining fills require a valid fit

        if (h.h_fcov)     h.h_fcov->Fill(r.f_coverage);
        if (h.h_nphotons && r.f_coverage > 0.f)
            h.h_nphotons->Fill(static_cast<float>(r.n_hits) / r.f_coverage);
        if (h.h_R)        h.h_R->Fill(r.R);
        if (h.h_R_split)  h.h_R_split->Fill(r.R);
        if (h.h_sigma)    h.h_sigma->Fill(r.sigma_r);
        if (h.h_R_vs_nhits) h.h_R_vs_nhits->Fill(r.n_hits, r.R);
        if (h.h_R_vs_nhits_split) h.h_R_vs_nhits_split->Fill(r.n_hits, r.R);
        if (h.h_centre_xy)  h.h_centre_xy->Fill(r.cx, r.cy);

        if (h.h_radial)
            for (float r_hit : r.radial_per_hit) h.h_radial->Fill(r_hit);
        if (h.h_radial_split)
            for (float r_hit : r.radial_per_hit) h.h_radial_split->Fill(r_hit);

        if (h.h_residual_vs_n)
            for (float dev : r.loo_residuals)
                h.h_residual_vs_n->Fill(r.n_hits, dev);
        if (h.h_residual_vs_n_split)
            for (float dev : r.loo_residuals)
                h.h_residual_vs_n_split->Fill(r.n_hits, dev);
    };

    auto refit_and_fill_ring = [&](HitMask ring_bit,
                                   const RingFillHists &h,
                                   AlcorLightdata &lightdata) -> bool
    {
        //  Stage 1A: delegate to compute + drain.  Preserves the
        //  original signature/behaviour for any caller that prefers
        //  the inline pattern; in the serial loop this is what gets
        //  called.  In Stage 2 the parallel dispatch will call
        //  `compute_ring_fit_pure` directly and the drain via
        //  `fill_ring_hists` from the merge step.
        const bool do_loo = (h.h_residual_vs_n || h.h_residual_vs_n_split);
        const RingFitResult r = compute_ring_fit_pure(ring_bit, lightdata, do_loo);
        fill_ring_hists(r, h);
        return r.fit_ok;
    };


    //  Enable a 50 MB tree cache before the two GetEntry passes (§4.7 minimum
    //  mitigation): the second full pass over the spill tree at line :275
    //  re-reads every basket from disk without it.  Proper single-pass
    //  restructure remains the open item.
    lightdata_tree->SetCacheSize(50 * 1024 * 1024);
    lightdata_tree->AddBranchToCache("*", true);

    //  ── Loop over spills ─────────────────────────────────────────────────────
    std::map<int, std::vector<float>> map_of_offsets;
    for (int i_spill = 0; i_spill < all_spills; ++i_spill)
    {
        lightdata_tree->GetEntry(i_spill);
        spilldata->get_entry();
        auto &frames_in_spill = spilldata->get_frame_list_link();
        auto &frame_reference = spilldata->get_frame_reference_list_link();

        //  ── First loop for calibration ──────────────────────────────────────────
        for (auto &current_lightdata_struct : frames_in_spill)
        {
            AlcorLightdata current_lightdata(current_lightdata_struct);
            auto current_trigger_list = current_lightdata.get_triggers();
            auto timing_trigger = std::find_if(current_trigger_list.begin(),
                                               current_trigger_list.end(),
                                               [](const TriggerEvent &e)
                                               {
                                                   return e.index == _TRIGGER_STREAMING_RING_FOUND_;
                                               });
            if (timing_trigger != current_trigger_list.end())
            {
                for (const auto &current_cherenkov : current_lightdata.get_cherenkov_hits_link())
                {
                    AlcorFinedata current_hit(current_cherenkov);
                    auto index = current_hit.get_global_index();
                    map_of_offsets[index].push_back(
                        current_hit.get_time_ns() - timing_trigger->fine_time);
                }
            }
        }
    }
    for (auto &[channel_index, values_list] : map_of_offsets)
    {
        if (values_list.size() < 20)
            continue;

        auto offset_value = 0.f;
        auto offset_participants = 0;
        for (auto &value : values_list)
        {
            if (fabs(value) > 30)
                continue;
            offset_value += value;
            offset_participants++;
        }
        offset_value /= offset_participants;

        if (offset_participants < 20)
            continue;

        if (!channel_index)
        {
            AlcorFinedata temy_testt;
            temy_testt.set_global_index(0);
            temy_testt.set_rollover(0);
            temy_testt.set_coarse(0);
            temy_testt.set_fine(0);
            mist::logger::debug(TString::Format("channel %i - offset: %f", channel_index, offset_value).Data());
            mist::logger::debug(TString::Format("channel %i - param0: %f", channel_index, AlcorFinedata::get_param0(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - param1: %f", channel_index, AlcorFinedata::get_param1(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - param2: %f", channel_index, AlcorFinedata::get_param2(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - get_time_ns: %f", channel_index, temy_testt.get_time_ns()).Data());
            AlcorFinedata::set_param2(channel_index, -offset_value / BTANA_ALCOR_CC_TO_NS);
            mist::logger::debug(TString::Format("channel %i - param0: %f", channel_index, AlcorFinedata::get_param0(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - param1: %f", channel_index, AlcorFinedata::get_param1(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - param2: %f", channel_index, AlcorFinedata::get_param2(channel_index)).Data());
            mist::logger::debug(TString::Format("channel %i - get_time_ns: %f", channel_index, temy_testt.get_time_ns()).Data());
        }
        AlcorFinedata::set_param2(channel_index, -offset_value / BTANA_ALCOR_CC_TO_NS);
    }
    mist::logger::debug("Save face");
    mist::logger::debug("Save face");
    mist::logger::debug("Save face");
    for (int i_spill = 0; i_spill < all_spills; ++i_spill)
    {
        //  Per-spill multi-bar reset (skip first iteration — the subtask is
        //  not yet active on the first pass through).
        if (i_spill > 0)
            progress_bars.restart(/*flush=*/false);
        progress_bars.update(i_spill, all_spills);

        lightdata_tree->GetEntry(i_spill);
        spilldata->get_entry();
        auto &frames_in_spill = spilldata->get_frame_list_link();
        auto &frame_reference = spilldata->get_frame_reference_list_link();

        //  Start-of-spill event: dead lane map
        auto lanes_participating = spilldata->get_not_dead_participants();
        for (auto [device, lanes] : lanes_participating)
            if (device < 200)
                for (auto current_lane : lanes)
                    for (auto i_channel = 0; i_channel < 8; ++i_channel)
                    {
                        // Phase 5: construct the synthetic dead-lane Hit
                        // with a new-layout GlobalIndex.  Split-in-two
                        // trick is applied here at the construction
                        // boundary.  The stored value is the new-layout
                        // raw; the position-cache lookup uses
                        // `4 * channel_ordinal` (the same dense-int key
                        // the position cache was populated with — see the
                        // top-of-function loop and `Mapping.cxx`).
                        const int chip_raw     = current_lane / 4;
                        const int channel_raw  = 8 * (current_lane % 4) + i_channel;
                        const int chip_logical = ::gidx::kUsesSplitInTwo
                                                     ? chip_raw / 2
                                                     : chip_raw;
                        const int channel_log  = ::gidx::kUsesSplitInTwo
                                                     ? channel_raw + 32 * (chip_raw % 2)
                                                     : channel_raw;
                        const auto gi = ::GlobalIndex::from_components(
                            device, current_lane, chip_logical, channel_log, 0);
                        const int pos_key = 4 * gi.channel_ordinal();
                        recodata.add_hit(
                            0., 0., 0.,
                            index_to_hit_xy[pos_key][0],
                            index_to_hit_xy[pos_key][1],
                            gi.raw(),
                            encode_bit(HitmaskDeadLane));
                        //  Per-spill active-channel mask for the
                        //  spill-weighted coverage map (DISCUSSION § 2.6).
                        //  Pulled directly from the same not_dead_participants
                        //  list that's written to the StartOfSpill marker
                        //  frame — matches the offline `photon_number_new.cpp`
                        //  macro's source (it reads hits from
                        //  `is_start_of_spill()` frames, which are exactly
                        //  these synthetic per-participant hits).
                        if (index_to_hit_xy.count(pos_key))
                            active_channels_per_spill[i_spill].insert(pos_key);
                    }
        recodata.add_trigger({TriggerStartOfSpill, static_cast<uint16_t>(framer_cfg.frame_size / 2)});
        recodata_tree->Fill();
        recodata.clear();

        //  ── Loop over frames ──────────────────────────────────────────────────
        int n_accepted = 0, n_edge = 0, n_duplicate = 0;

        //  ───────────────────────────────────────────────────────────────────
        //  process_frame_pure (Stage 1B): pure-compute per-frame producer.
        //  Reads `lightdata`, produces a `FrameResult`.  No histogram
        //  fills, no `recodata.add_*`, no tree Fill, no per-spill
        //  counter mutation.  Thread-safe (only reads from shared
        //  refs; writes only to its local FrameResult).
        //  ───────────────────────────────────────────────────────────────────
        auto process_frame_pure = [&](AlcorLightdata &lightdata) -> FrameResult
        {
            FrameResult res;
            const float frame_size_ns = framer_cfg.frame_size * BTANA_ALCOR_CC_TO_NS;

            for (const auto &current_trigger : lightdata.get_triggers())
            {
                if (current_trigger.index == _TRIGGER_UNKNOWN_)
                    continue;
                const float reg_bin = registry.index_of(current_trigger.index) + 0.5f;

                const bool is_edge =
                    (current_trigger.fine_time < edge_rejection_ns) ||
                    (current_trigger.fine_time > frame_size_ns - edge_rejection_ns);
                if (is_edge)
                {
                    res.edge_fills.emplace_back(reg_bin, current_trigger.fine_time);
                    res.trigger_qa_fills.emplace_back(reg_bin, 1.5f); // edge-rejected
                    res.had_edge = true;
                    continue;
                }

                if (auto it = res.accepted_triggers.find(current_trigger.index);
                    it != res.accepted_triggers.end())
                {
                    const auto &prev = it->second;
                    if (std::fabs((float)current_trigger.coarse - (float)prev.coarse) <
                        BTANA_TRIGGER_MIN_SEPARATION)
                        continue;  // temporal duplicate
                    res.trigger_qa_fills.emplace_back(reg_bin, 2.5f); // duplicate-rejected
                    res.rejected = true;
                    break;
                }

                // First time seeing this trigger — accept.
                res.accepted_triggers[current_trigger.index] = current_trigger;
                for (const auto &chrk : lightdata.get_cherenkov_hits_link())
                {
                    const float dt = AlcorFinedata(chrk).get_time_ns() - current_trigger.fine_time;
                    res.time_diff_fills.emplace_back(current_trigger.index, dt);
                }
            }

            res.accepted = !res.rejected;
            if (res.rejected) return res;

            //  Per-spill physics check (DISCUSSION § 2.6).
            for (const auto &[idx, trig] : res.accepted_triggers)
            {
                if (idx == TriggerFirstFrames)             continue;
                if (idx == _TRIGGER_STREAMING_RING_FOUND_) continue;
                if (idx == TriggerStartOfSpill)            continue;
                if (idx == _TRIGGER_UNKNOWN_)              continue;
                res.frame_is_physics = true;
                break;
            }

            //  Ring detection + fits (only if frame carries the Hough trigger).
            if (res.accepted_triggers.count(_TRIGGER_HOUGH_RING_FOUND_))
            {
                for (const auto &hit_struct : lightdata.get_cherenkov_hits_link())
                {
                    AlcorFinedata fh(hit_struct);
                    if (fh.has_mask_bit(HitmaskHoughRingTagSecond))
                    {
                        res.frame_has_second_ring = true;
                        break;
                    }
                }
                res.first  = compute_ring_fit_pure(HitmaskHoughRingTagFirst,
                                                   lightdata, /*do_loo=*/true);
                res.second = compute_ring_fit_pure(HitmaskHoughRingTagSecond,
                                                   lightdata, /*do_loo=*/true);
            }
            return res;
        };

        //  ───────────────────────────────────────────────────────────────────
        //  drain_frame_result (Stage 1B): serial consumer.  Plays back
        //  every side effect (hist fills, recodata.add_*, tree Fill,
        //  per-spill counter updates) given a precomputed FrameResult
        //  and the original AlcorLightdata wrapper (for the hit-copy
        //  loop).  Always called serially in frame order.
        //  ───────────────────────────────────────────────────────────────────
        auto drain_frame_result = [&](const FrameResult &res,
                                       AlcorLightdata &lightdata)
        {
            h_frames_per_spill->Fill(i_spill, 0.5); // total

            //  Trigger-validation hist fills.
            for (const auto &[bin, fine_t] : res.edge_fills)
                h_edge_trigger_position->Fill(bin, fine_t);
            for (const auto &[bin, outcome] : res.trigger_qa_fills)
                h_trigger_qa->Fill(bin, outcome);
            //  Time-diff hist fills — lazy create on first encounter.
            for (const auto &[idx, dt] : res.time_diff_fills)
            {
                if (!h_trigger_time_diff_w_cherenkov.count(idx))
                    h_trigger_time_diff_w_cherenkov[idx] =
                        RootHist<TH1F>(
                            TString::Format("h_trigger_time_diff_w_cherenkov_%s",
                                            registry.name_of(idx).c_str()).Data(),
                            ";#Delta_{t} (t_{Hit} - t_{trigger}) ns;Normalised entries",
                            5e3, -500, 500);
                h_trigger_time_diff_w_cherenkov[idx]->Fill(dt);
            }

            if (res.rejected)
            {
                n_duplicate++;
                h_frames_per_spill->Fill(i_spill, 3.5);
                return;
            }
            if (res.had_edge)
            {
                n_edge++;
                h_frames_per_spill->Fill(i_spill, 2.5);
            }

            for (auto &[index, trigger] : res.accepted_triggers)
            {
                h_trigger_qa->Fill(registry.index_of(index) + 0.5, 0.5); // accepted
                recodata.add_trigger(trigger);
            }
            if (res.frame_is_physics)
                ++n_physics_per_spill[i_spill];

            //  Cherenkov hits — copy from lightdata (still in scope).
            for (const auto &chrk : lightdata.get_cherenkov_hits_link())
                recodata.add_hit(chrk);

            //  Radiator QA — drain the precomputed RingFitResults.
            if (res.accepted_triggers.count(_TRIGGER_HOUGH_RING_FOUND_))
            {
                RingFillHists first_hists;
                first_hists.h_nhits         = h_nhits_first.get();
                first_hists.h_nphotons      = h_nphotons_first.get();
                first_hists.h_fcov          = h_f_coverage_first.get();
                first_hists.h_radial        = h_radial_first.get();
                first_hists.h_R             = h_R_first.get();
                first_hists.h_sigma         = h_sigma_first.get();
                first_hists.h_R_vs_nhits    = h_R_vs_nhits_first.get();
                first_hists.h_centre_xy     = h_centre_xy_first.get();
                first_hists.h_residual_vs_n = h_residual_vs_n_first.get();
                first_hists.h_R_vs_nhits_split = res.frame_has_second_ring
                    ? h_R_vs_nhits_first_dual.get()
                    : h_R_vs_nhits_first_solo.get();
                first_hists.h_residual_vs_n_split = res.frame_has_second_ring
                    ? h_residual_vs_n_first_dual.get()
                    : h_residual_vs_n_first_solo.get();
                first_hists.h_radial_split = res.frame_has_second_ring
                    ? h_radial_first_dual.get()
                    : h_radial_first_solo.get();
                first_hists.h_R_split = res.frame_has_second_ring
                    ? h_R_first_dual.get()
                    : h_R_first_solo.get();
                fill_ring_hists(res.first, first_hists);

                RingFillHists second_hists;
                second_hists.h_nhits         = h_nhits_second.get();
                second_hists.h_nphotons      = h_nphotons_second.get();
                second_hists.h_fcov          = h_f_coverage_second.get();
                second_hists.h_radial        = h_radial_second.get();
                second_hists.h_R             = h_R_second.get();
                second_hists.h_sigma         = h_sigma_second.get();
                second_hists.h_R_vs_nhits    = h_R_vs_nhits_second.get();
                second_hists.h_centre_xy     = h_centre_xy_second.get();
                second_hists.h_residual_vs_n = h_residual_vs_n_second.get();
                fill_ring_hists(res.second, second_hists);
            }

            recodata_tree->Fill();
            recodata.clear();
            n_accepted++;
            h_frames_per_spill->Fill(i_spill, 1.5);
        };

        //  ─── Stage 2: frames-within-spill multithreading ─────────
        //
        //  Pattern mirrors `parallel_streaming_framer.cxx::next_spill`:
        //  N workers dispatched via std::async, work distributed via
        //  atomic frame-index counter, results written to disjoint
        //  slots in a pre-sized vector → no contention in the parallel
        //  phase.  Drain runs serially in frame order to preserve
        //  recodata.root write ordering + histogram fill ordering.
        //
        //  Thread safety:
        //   * `process_frame_pure` reads only `[&]`-captured shared
        //     state (registry, recodata_cfg, framer_cfg, edge_rejection_ns,
        //     index_to_hit_xy) — all read-only after init.
        //   * It calls `compute_ring_fit_pure` → `fit_circle` (ROOT's
        //     Minuit2 fitter) which is documented thread-safe per
        //     instance.  Each call constructs its own local Fitter.
        //   * NO histogram fills, NO recodata.add_*, NO tree Fill in
        //     the parallel phase.
        //
        //  Falls back to a serial path when n_threads <= 1.
        const size_t n_frames = frames_in_spill.size();
        const size_t n_threads = std::max<size_t>(
            1, std::min<size_t>(std::thread::hardware_concurrency(), n_frames));
        if (i_spill == 0)
            mist::logger::info(TString::Format(
                "(recodata_writer) parallel dispatch: hardware_concurrency=%u  "
                "n_frames_first_spill=%zu  n_threads=%zu",
                std::thread::hardware_concurrency(),
                n_frames, n_threads).Data());

        std::vector<FrameResult> frame_results(n_frames);

        //  Progress: workers tick `post_processing` directly after
        //  each frame.  Throttled to once per 64 frames per worker so
        //  the mutex contention stays negligible.  Safe to call from
        //  multiple threads because MIST's MultiProgressBar::update
        //  holds the registry lock across erase_all + redraw_all (the
        //  upstream race condition that previously corrupted the
        //  cursor band was fixed in this branch of mist — see the
        //  log_print_guard pattern in mist/logger.cxx).
        std::atomic<size_t> done{0};
        auto tick_progress = [&](size_t my_completion_idx) {
            if ((my_completion_idx & 63) == 0)
                post_processing.update(done.load(std::memory_order_relaxed),
                                       n_frames);
        };

        if (n_threads <= 1) {
            for (size_t iframe = 0; iframe < n_frames; ++iframe) {
                AlcorLightdata cur(frames_in_spill[iframe]);
                frame_results[iframe] = process_frame_pure(cur);
                const size_t now_done = done.fetch_add(1) + 1;
                tick_progress(now_done);
            }
        } else {
            std::atomic<size_t> next_frame{0};
            std::vector<std::future<void>> thread_pool;
            thread_pool.reserve(n_threads);
            for (size_t t = 0; t < n_threads; ++t) {
                thread_pool.push_back(std::async(std::launch::async, [&]() {
                    while (true) {
                        const size_t my = next_frame.fetch_add(1);
                        if (my >= n_frames) return;
                        AlcorLightdata cur(frames_in_spill[my]);
                        frame_results[my] = process_frame_pure(cur);
                        const size_t now_done = done.fetch_add(1) + 1;
                        tick_progress(now_done);
                    }
                }));
            }
            for (auto &f : thread_pool) f.get();
        }
        //  Snap to 100% so the bar reflects "compute finished" even
        //  when the last ticks fell between mod-64 thresholds.
        post_processing.update(n_frames, n_frames);

        //  Serial drain in frame order.  All hist fills, recodata
        //  add_*, tree Fill, per-spill counter updates happen here.
        //  Bar is already at 100% from the compute snap above; this
        //  loop is fast so no in-loop ticks needed.
        for (size_t iframe = 0; iframe < n_frames; ++iframe) {
            AlcorLightdata current_lightdata(frames_in_spill[iframe]);
            drain_frame_result(frame_results[iframe], current_lightdata);
        }
#if 0  // ─────────────── original loop body (Stage-1A baseline) ───────────────
            h_frames_per_spill->Fill(i_spill, 0.5); // total

            //  ── Trigger selection ───────────────────────────────────────────────
            //  1. Triggers within 25 ns of either boundary → edge-rejected.
            //     Frame not immediately discarded; type may still have a valid instance.
            //  2. Two distinct valid instances of the same type → frame rejected.
            //  3. Two valid instances of the same type within BTANA_TRIGGER_MIN_SEPARATION
            //     cc → temporal duplicate, second dropped silently, frame kept.

            bool frame_rejected = false;
            bool had_edge = false;
            std::map<uint8_t, TriggerEvent> accepted_triggers;

            // Trigger selection order matters:
            //   1. Skip the UNKNOWN sentinel.
            //   2. Edge rejection — edge-rejected triggers do NOT count toward
            //      the per-frame "we've seen this index already" set.
            //   3. Duplicate check (against accepted_triggers from earlier
            //      iterations of this same loop, i.e. earlier in the frame):
            //      - within BTANA_TRIGGER_MIN_SEPARATION cc → temporal dup,
            //        drop silently;
            //      - else → distinct second firing of the same trigger →
            //        reject the whole frame.
            //   4. Otherwise: accept (insert into accepted_triggers), create
            //      the time-diff histogram lazily, fill it.
            // The previous version inserted at the top of the loop BEFORE the
            // duplicate check, so the check at (3) was always true and every
            // trigger was silently dropped on the temporal-duplicate branch.
            for (const auto &current_trigger : current_lightdata.get_triggers())
            {
                if (current_trigger.index == _TRIGGER_UNKNOWN_)
                    continue;

                const int reg_bin = registry.index_of(current_trigger.index) + 0.5; // centre of bin

                //  Both sides in ns.  `frame_size` is in clock cycles
                //  by framer convention → convert to ns for the
                //  comparison with `fine_time` (ns).
                const float frame_size_ns =
                    framer_cfg.frame_size * BTANA_ALCOR_CC_TO_NS;
                bool is_edge = (current_trigger.fine_time < edge_rejection_ns) ||
                               (current_trigger.fine_time > frame_size_ns - edge_rejection_ns);
                if (is_edge)
                {
                    h_edge_trigger_position->Fill(reg_bin, current_trigger.fine_time);
                    h_trigger_qa->Fill(reg_bin, 1.5); // edge-rejected
                    had_edge = true;
                    continue;
                }

                if (auto it = accepted_triggers.find(current_trigger.index);
                    it != accepted_triggers.end())
                {
                    const auto &prev = it->second;
                    if (std::fabs((float)current_trigger.coarse - (float)prev.coarse) < BTANA_TRIGGER_MIN_SEPARATION)
                        continue; // temporal duplicate, drop silently

                    h_trigger_qa->Fill(reg_bin, 2.5); // duplicate-rejected
                    frame_rejected = true;
                    break;
                }

                // First time seeing this trigger index in this frame — accept.
                accepted_triggers[current_trigger.index] = current_trigger;
                if (!h_trigger_time_diff_w_cherenkov.count(current_trigger.index))
                    h_trigger_time_diff_w_cherenkov[current_trigger.index] =
                        RootHist<TH1F>(
                            TString::Format("h_trigger_time_diff_w_cherenkov_%s", registry.name_of(current_trigger.index).c_str()).Data(),
                            ";#Delta_{t} (t_{Hit} - t_{trigger}) ns;Normalised entries",
                            5e3,
                            -500,
                            500);
                for (const auto &current_cherenkov_hit_struct : current_lightdata.get_cherenkov_hits_link())
                    h_trigger_time_diff_w_cherenkov[current_trigger.index]->Fill(
                        AlcorFinedata(current_cherenkov_hit_struct).get_time_ns() -
                        current_trigger.fine_time);
            }

            if (frame_rejected)
            {
                n_duplicate++;
                h_frames_per_spill->Fill(i_spill, 3.5); // duplicate-rejected
                continue;
            }

            if (had_edge)
            {
                n_edge++;
                h_frames_per_spill->Fill(i_spill, 2.5); // had edge trigger (frame kept)
            }

            for (auto &[index, trigger] : accepted_triggers)
            {
                h_trigger_qa->Fill(registry.index_of(index) + 0.5, 0.5); // accepted
                recodata.add_trigger(trigger);
            }

            //  ── Per-spill physics counter for the spill-weighted
            //     coverage map (DISCUSSION § 2.6).  A frame counts as
            //     "physics" if it carries any accepted trigger except
            //     the bookkeeping / sampling sentinels:
            //
            //       * TriggerFirstFrames (100)         — DCR sampling
            //       * _TRIGGER_STREAMING_RING_FOUND_ (104) — internal
            //                                              streaming trigger
            //       * TriggerStartOfSpill (200)        — spill boundary marker
            //       * _TRIGGER_UNKNOWN_ (255)          — sentinel
            //
            //     Everything else (luca_and_finger, broad_scintillator,
            //     TIMING, TRACKING, RING_FOUND, HOUGH_RING_FOUND, …) is
            //     a beam-defining or downstream-physics trigger and
            //     contributes to the spill's physics weight.
            //
            //     Active-channel mask is populated separately at the
            //     spill's StartOfSpill-marker construction (line ~660
            //     above) directly from
            //     `spilldata->get_not_dead_participants`.
            {
                bool frame_is_physics = false;
                for (const auto &[idx, trig] : accepted_triggers)
                {
                    if (idx == TriggerFirstFrames)             continue;
                    if (idx == _TRIGGER_STREAMING_RING_FOUND_) continue;
                    if (idx == TriggerStartOfSpill)            continue;
                    if (idx == _TRIGGER_UNKNOWN_)              continue;
                    frame_is_physics = true;
                    break;
                }
                if (frame_is_physics)
                    ++n_physics_per_spill[i_spill];
            }

            //  ── Cherenkov hits ─────────────────────────────────────────────────
            for (const auto &current_cherenkov_hit_struct : current_lightdata.get_cherenkov_hits_link())
                recodata.add_hit(current_cherenkov_hit_struct);

            //  ── Live-QA radiator fills (DISCUSSION § 2.6) ──────────────────────
            //  Gated on the frame carrying a `_TRIGGER_HOUGH_RING_FOUND_`
            //  trigger.  The lightdata-side Hough already mask-tagged
            //  the contributing hits; we re-fit per ring on those tags
            //  to recover (cx, cy, R) without a TriggerEvent schema bump.
            //  Two rings are queried unconditionally — `refit_and_fill_ring`
            //  short-circuits on `< min_hits_per_ring` matching the
            //  ring's bit, so frames with only ring 1 still produce
            //  no ring-2 fill.
            if (accepted_triggers.count(_TRIGGER_HOUGH_RING_FOUND_))
            {
                //  Detect "frame has a second ring" once, by scanning
                //  hit masks.  Cheaper than counting hits and tagging
                //  separately for each ring slot.  Used by the dual /
                //  solo split logic below.
                bool frame_has_second_ring = false;
                for (const auto &hit_struct : current_lightdata.get_cherenkov_hits_link())
                {
                    AlcorFinedata fh(hit_struct);
                    if (fh.has_mask_bit(HitmaskHoughRingTagSecond))
                    {
                        frame_has_second_ring = true;
                        break;
                    }
                }

                RingFillHists first_hists;
                first_hists.h_nhits       = h_nhits_first.get();
                first_hists.h_nphotons    = h_nphotons_first.get();
                first_hists.h_fcov        = h_f_coverage_first.get();
                first_hists.h_radial      = h_radial_first.get();
                first_hists.h_R           = h_R_first.get();
                first_hists.h_sigma       = h_sigma_first.get();
                first_hists.h_R_vs_nhits  = h_R_vs_nhits_first.get();
                first_hists.h_centre_xy   = h_centre_xy_first.get();
                first_hists.h_residual_vs_n = h_residual_vs_n_first.get();
                //  Split twins — fill (dual) when a second ring is
                //  present in the frame, (solo) when it isn't.
                first_hists.h_R_vs_nhits_split = frame_has_second_ring
                    ? h_R_vs_nhits_first_dual.get()
                    : h_R_vs_nhits_first_solo.get();
                first_hists.h_residual_vs_n_split = frame_has_second_ring
                    ? h_residual_vs_n_first_dual.get()
                    : h_residual_vs_n_first_solo.get();
                first_hists.h_radial_split = frame_has_second_ring
                    ? h_radial_first_dual.get()
                    : h_radial_first_solo.get();
                first_hists.h_R_split = frame_has_second_ring
                    ? h_R_first_dual.get()
                    : h_R_first_solo.get();
                refit_and_fill_ring(HitmaskHoughRingTagFirst, first_hists, current_lightdata);

                RingFillHists second_hists;
                second_hists.h_nhits      = h_nhits_second.get();
                second_hists.h_nphotons   = h_nphotons_second.get();
                second_hists.h_fcov       = h_f_coverage_second.get();
                second_hists.h_radial     = h_radial_second.get();
                second_hists.h_R          = h_R_second.get();
                second_hists.h_sigma      = h_sigma_second.get();
                second_hists.h_R_vs_nhits = h_R_vs_nhits_second.get();
                second_hists.h_centre_xy  = h_centre_xy_second.get();
                second_hists.h_residual_vs_n = h_residual_vs_n_second.get();
                //  Second ring is dual-by-definition — no split twin.
                refit_and_fill_ring(HitmaskHoughRingTagSecond, second_hists, current_lightdata);
            }

            recodata_tree->Fill();
            recodata.clear();
            n_accepted++;
            h_frames_per_spill->Fill(i_spill, 1.5); // accepted
        }
#endif  // ── end original loop body, replaced by process+drain above ──

        mist::logger::info(TString::Format("Spill %i done — accepted: %i  had-edge: %i  duplicate-rejected: %i  total: %zu",
                                i_spill, n_accepted, n_edge, n_duplicate, frames_in_spill.size()).Data());

        //  Reflect the just-completed spill on the main bar.
        progress_bars.update(i_spill + 1, all_spills);
    } // end spill loop

    post_processing.finish(/*flush=*/false);
    progress_bars.finish();

    //  --- --- --- --- --- ---
    //  QA plots
    //  ---
    output_file->cd();
    recodata_tree->Write();
    //  ---
    //  --- Trigger QA
    TDirectory *trigger_dir = output_file->mkdir("Triggers");
    trigger_dir->cd();
    h_trigger_qa->Write();
    h_frames_per_spill->Write();
    h_edge_trigger_position->Write();
    for (auto &[key, val] : h_trigger_time_diff_w_cherenkov)
        val->Write();

    //  ---
    //  --- Rings QA (DISCUSSION § 2.6: live photon-counting pipeline)
    //
    //  Finalize step:
    //   1. Compute eff(R) from the coverage map using the radial hist's
    //      X-axis as the output binning (so the Divide is bin-aligned).
    //   2. Divide the radial hists by eff(R) — turns "hits per bin" into
    //      "hits per bin per unit acceptance".  Downstream fits
    //      (Gaussian-on-CB → N_γ) read this directly.
    //   3. Write coverage map + eff(R) + per-ring hists to the
    //      "Radiator" subfolder.  Layout chosen so all live-QA plots
    //      live one directory deep in the output file.
    //
    //  Centre-convention reminder: the coverage map and eff(R) use the
    //  FIXED nominal centre from `[recodata].nominal_centre_{x,y}_mm`;
    //  the radial hists' R values come from PER-EVENT Hough/fit
    //  centres.  Discrepancy < 1 % at observed centre wander.
    {
        // Subfolder name: "Rings/" — contents are per-ring observables
        // from the Hough output, not per-radiator-material splits.  The
        // ring↔radiator mapping (when wired in via `RadiatorInfoStruct`)
        // is a follow-up to V1; see DISCUSSION § 2.6 "Deferred items".

        //  ── Spill-by-spill active-channel weighting (DISCUSSION § 2.6)
        //
        //  Build per-channel weights from the per-spill bookkeeping we
        //  accumulated during the spill loop:
        //
        //     spill_weight[s] = n_physics_per_spill[s] / Σ n_physics
        //     channel_weight[k] = Σ over spills active in of spill_weight[s]
        //
        //  Channels never active get weight 0 (silent mask).  Channels
        //  active in every spill get weight 1.  Intermediate channels
        //  get a fractional weight reflecting their duty cycle weighted
        //  by physics rate.  Result: eff(R) reflects the actually-
        //  delivered acceptance, not the geometric upper bound.
        std::map<int, float> channel_weights;
        long total_physics = 0;
        for (int n : n_physics_per_spill) total_physics += n;
        if (total_physics > 0)
        {
            for (int is = 0; is < all_spills; ++is)
            {
                if (n_physics_per_spill[is] <= 0) continue;
                if (active_channels_per_spill[is].empty()) continue;
                const float spill_weight =
                    static_cast<float>(n_physics_per_spill[is]) /
                    static_cast<float>(total_physics);
                for (int channel_key : active_channels_per_spill[is])
                    channel_weights[channel_key] += spill_weight;
            }
            mist::logger::info(TString::Format(
                "(recodata_writer) spill-weighted coverage: "
                "total_physics=%ld  active_channels=%zu / total=%zu",
                total_physics, channel_weights.size(),
                index_to_hit_xy.size()).Data());
        }
        else
        {
            mist::logger::warning(
                "(recodata_writer) no HOUGH_RING_FOUND triggers in this "
                "run — coverage map falls back to geometric upper bound.");
        }

        //  Build the TH2F with weighted channels.  If channel_weights
        //  is empty (no physics triggers, e.g. background-only run),
        //  pass nullptr → geometric upper bound (legacy V1 behaviour).
        std::unique_ptr<TH2F> h_coverage_map_rphi(
            util::radiator_efficiency::build_coverage_map(
                index_to_hit_xy,
                recodata_cfg.n_phi_bins_coverage,
                recodata_cfg.r_min_coverage_mm,
                recodata_cfg.r_max_coverage_mm,
                recodata_cfg.n_r_bins_coverage,
                recodata_cfg.channel_half_width_mm,
                recodata_cfg.nominal_centre_x_mm,
                recodata_cfg.nominal_centre_y_mm,
                recodata_cfg.min_channel_r_for_coverage_mm,
                channel_weights.empty() ? nullptr : &channel_weights));
        h_coverage_map_rphi->SetName("h_coverage_map_rphi");
        h_coverage_map_rphi->SetTitle(";#phi (rad);R (mm)");

        TDirectory *rings_dir = output_file->mkdir("Rings");
        rings_dir->cd();

        h_coverage_map_rphi->Write();

        //  eff(R) — owned locally; written below into the same dir.
        //  Use the per-ring radial hist's X-axis so the binning matches
        //  exactly (Divide requires identical binning).
        std::unique_ptr<TH1F> eff_R(
            util::radiator_efficiency::radial_efficiency(
                h_coverage_map_rphi.get(),
                h_radial_first->GetXaxis()));
        if (eff_R)
        {
            eff_R->SetName("h_eff_R");
            eff_R->SetTitle(";R (mm);#it{eff}(R)");

            //  Defensive: zero-eff bins would blow up the division.
            //  Replace with a tiny sentinel; the corresponding radial
            //  bin will then end up near zero, not NaN.  ROOT's
            //  TH1F::Divide already handles zero by skipping, but
            //  belt-and-braces here for the rare boundary cases.
            for (int ib = 1; ib <= eff_R->GetNbinsX(); ++ib)
                if (eff_R->GetBinContent(ib) <= 0.0)
                    eff_R->SetBinContent(ib, 0.0);

            h_radial_first ->Divide(eff_R.get());
            h_radial_second->Divide(eff_R.get());
            h_radial_first_dual->Divide(eff_R.get());
            h_radial_first_solo->Divide(eff_R.get());

            eff_R->Write();
        }
        else
        {
            mist::logger::warning("(recodata_writer) radial_efficiency returned null; "
                                  "radial hists are NOT efficiency-corrected.");
        }

        h_nhits_first ->Write();
        h_nhits_second->Write();
        h_nphotons_first ->Write();
        h_nphotons_second->Write();
        h_f_coverage_first ->Write();
        h_f_coverage_second->Write();
        h_radial_first ->Write();
        h_radial_second->Write();
        h_radial_first_dual->Write();
        h_radial_first_solo->Write();

        //  ── Crystal-Ball + pol3 fit on the eff-corrected radial
        //     hist (DISCUSSION § 2.6).  Ported from
        //     `macros/examples/photon_number_new.cpp`'s
        //     `fit_radial_distribution` lambda (lines 952–1037).
        //
        //  Recipe:
        //    1. Sideband-only prefit: clone the hist, zero out bins in
        //       the signal region (peak ± few σ), fit pol3 on the
        //       remaining bins to get the background shape.
        //    2. Combined CB + pol3 fit on the full range, with the pol3
        //       parameters initialised from step 1 and FROZEN for the
        //       first iteration (so the CB can find the peak before
        //       background floats), then released for a final iteration.
        //    3. Extract N_γ = signal-only integral over the fit range.
        //       Signal-only = CB component, evaluated by zeroing the
        //       pol3 parameters in a copy of the fit function.
        //    4. Store as TNamed scalars next to the hist:
        //       <name>_N_gamma   = string "value"
        //       <name>_peak_mu   = string "mu mm"
        //       <name>_peak_sigma = string "sigma mm"
        //
        //  Caveats:
        //   * Fit may fail on low-stats hists; we skip silently
        //     (logged as warning).
        //   * Initial peak seed uses the hist's max bin — robust for
        //     a well-defined Cherenkov ring but bad if the hist is
        //     dominated by background.  If σ_seed clamps to 4 mm
        //     consistently, the hist is too flat to fit.
        //  Collector for durable CB+pol3 summary plots.  Three numbers
        //  per radial hist (N_γ, peak_μ, peak_σ) accumulated as we fit
        //  each of the four radial-hist samples; built into three
        //  bin-labeled TH1Fs at the end of this block.
        struct RadialFitResult {
            std::string name;
            double n_gamma;
            double peak_mu;       double peak_mu_err;
            double peak_sigma;    double peak_sigma_err;
        };
        std::vector<RadialFitResult> radial_results;

        //  `h_R_count` is the per-event ring-radius hist matching the
        //  radial hist's sample (same dual/solo gate).  Used purely to
        //  obtain `N_rings = GetEntries()` for the per-ring N_γ.  Pass
        //  explicitly rather than looking up by name — was previously a
        //  gDirectory dependency that broke when h_R writes came AFTER
        //  the fit calls in the finalize block (n_rings = 0 bug, fixed
        //  by 2026-05-26 by making it a parameter).
        auto fit_radial_distribution = [&](TH1F *h, TH1F *h_R_count, const std::string &tag)
        {
            if (!h || h->GetEntries() < 100) {
                mist::logger::warning(TString::Format(
                    "(recodata_writer) %s: too few entries for radial fit (%lld); skipping.",
                    tag.c_str(), (long long)h->GetEntries()).Data());
                return;
            }
            //  Normalise the hist to PER-RING so amplitude & integral
            //  express photons directly (no external /N_rings step).
            //  After scaling:
            //    • bin i = (photons per ring) in radial bin i
            //    • Σ(signal bins) × bin_width = N_γ per ring
            //    • CB amplitude param [0] = peak photon density (per
            //      ring per mm) at μ
            //  N_rings = entries of the passed-in h_R_count hist (one
            //  entry per successful ring fit, populated in
            //  refit_and_fill_ring).  If null/empty we skip the scale
            //  and the headline number degrades to "total photons
            //  across all rings" — graceful fallback for hists we
            //  forgot to wire a count hist to.
            const long n_rings = h_R_count
                ? static_cast<long>(h_R_count->GetEntries())
                : 0;
            if (n_rings > 0) {
                h->Scale(1.0 / static_cast<double>(n_rings));
                h->GetYaxis()->SetTitle("photons / ring / bin");
            }
            //  Acceptance band — used for the peak seed search (to avoid
            //  eff(R) corner blow-ups) and as the wide envelope.
            const float r_band_lo = recodata_cfg.r_min_coverage_mm + 5.f;  // ≈ 30 mm
            const float r_band_hi = recodata_cfg.r_max_coverage_mm - 5.f;  // ≈ 120 mm

            //  Peak seed: search ONLY the interior of the hist, well
            //  inside the eff(R) acceptance band.  After eff division
            //  the corners can blow up (eff → 0 near the geometric
            //  corners) and a single noisy bin there will out-score
            //  the true Cherenkov peak.  Restrict to [r_band_lo+10,
            //  r_band_hi-10] for the seed search and apply one round
            //  of TH1::Smooth (3-bin running average) to suppress
            //  single-bin spikes.
            float peak_seed = 0.5f * (r_band_lo + r_band_hi);
            float amp_seed  = h->GetMaximum();
            {
                std::unique_ptr<TH1F> smoothed(static_cast<TH1F*>(
                    h->Clone((tag + "_seed_smoothed").c_str())));
                smoothed->SetDirectory(nullptr);
                smoothed->Smooth(1);
                const int interior_lo = smoothed->FindBin(r_band_lo + 10.f);
                const int interior_hi = smoothed->FindBin(r_band_hi - 10.f);
                int   best_bin = interior_lo;
                double best_val = -1.;
                for (int ib = interior_lo; ib <= interior_hi; ++ib) {
                    const double v = smoothed->GetBinContent(ib);
                    if (v > best_val) { best_val = v; best_bin = ib; }
                }
                peak_seed = smoothed->GetBinCenter(best_bin);
                amp_seed  = h->GetBinContent(best_bin);   // raw amplitude, not smoothed
            }
            const float sigma_seed = std::clamp(
                static_cast<float>(h->GetRMS()) * 0.4f, 1.0f, 4.0f);

            //  Fit range: the FULL acceptance band.  Narrowing around
            //  the peak helps low-stats hists (e.g. solo) where pol3
            //  doesn't need to span the right-shoulder structure, but
            //  breaks high-stats fits (first_dual) where the side-
            //  band constraints are needed to keep the Hessian non-
            //  singular.  Keep wide; use a richer background model
            //  (pol5 instead of pol3) so the background can bend
            //  through the right-shoulder physical structure.
            const float fit_lo = r_band_lo;
            const float fit_hi = r_band_hi;

            //  Background-only model (pol3) for the sideband prefit.
            //  Symmetric ±4σ mask — the previous asymmetric (4σ low,
            //  2σ high) leaked Gaussian signal into the right sideband
            //  and dragged the pol3 baseline up, biasing the headline
            //  fit downward.  (Tried pol5 — too flexible, the higher-
            //  order terms ate the signal peak on high-stats hists
            //  and produced singular Hessians on low-stats ones.
            //  pol3 stays even though the shoulder/floor structure
            //  it can't capture inflates χ² — physics params μ, σ,
            //  N_γ remain robust.)
            TF1 bg_prefit((tag + "_bg_prefit").c_str(),
                          "pol3", fit_lo, fit_hi);
            {
                std::unique_ptr<TH1F> sideband(static_cast<TH1F*>(
                    h->Clone((tag + "_sideband").c_str())));
                sideband->SetDirectory(nullptr);
                for (int ib = 1; ib <= sideband->GetNbinsX(); ++ib) {
                    const double bc = sideband->GetBinCenter(ib);
                    const bool in_signal =
                        (bc > peak_seed - 4.f * sigma_seed) &&
                        (bc < peak_seed + 4.f * sigma_seed);
                    if (bc < fit_lo || bc > fit_hi || in_signal) {
                        sideband->SetBinContent(ib, 0.);
                        sideband->SetBinError(ib, 1e10);
                    }
                }
                bg_prefit.SetParameters(0.08, 0., 0., 0.);
                sideband->Fit(&bg_prefit, "RQ0");
            }

            //  Combined Crystal-Ball + pol3 as a TFormula STRING (not
            //  a C++ lambda) — so the resulting TF1 is fully
            //  serializable: writes cleanly to the ROOT file and
            //  reads back with the function still callable.  A
            //  lambda-backed TF1 saves its parameters but loses the
            //  callable function pointer, which makes the saved TF1
            //  (and any canvas drawing it) segfault on TBrowser open.
            //
            //  Identical analytic form to the offline macro's
            //  `crystal_ball_plus_pol3` lambda:
            //
            //      t = (x - μ) / σ
            //      if t > -α:  cb = N · exp(-t²/2)
            //      else:       cb = N · (n/α)^n · exp(-α²/2)
            //                       · (n/α - α - t)^{-n}
            //      f(x) = cb + c0 + c1·x + c2·x² + c3·x³
            //
            //  TFormula's `?:` ternary handles the conditional.  The
            //  base of the pow() in the left-tail branch is provably
            //  positive whenever t ≤ -α (n, α > 0 by ParLimits), so
            //  pow with a negative exponent stays real-valued.
            static const char *kCbPol3Formula =
                "(((x-[1])/[2] > -[3]) ? "
                "[0]*exp(-0.5*((x-[1])/[2])*((x-[1])/[2])) : "
                "[0]*pow([4]/[3],[4])*exp(-0.5*[3]*[3])*pow([4]/[3] - [3] - (x-[1])/[2], -[4]))"
                " + [5] + [6]*x + [7]*x*x + [8]*x*x*x";
            TF1 cb_fit((tag + "_cb_fit").c_str(),
                       kCbPol3Formula, fit_lo, fit_hi);
            const char *parnames[9] = {
                "cb_amp", "peak_mu", "peak_sigma", "cb_alpha", "cb_n",
                "bg_c0", "bg_c1", "bg_c2", "bg_c3"};
            for (int i = 0; i < 9; ++i) cb_fit.SetParName(i, parnames[i]);
            cb_fit.SetParameters(amp_seed, peak_seed, sigma_seed, 1.5, 3.0,
                                 bg_prefit.GetParameter(0), bg_prefit.GetParameter(1),
                                 bg_prefit.GetParameter(2), bg_prefit.GetParameter(3));
            cb_fit.SetParLimits(0, 0., 1e9);
            cb_fit.SetParLimits(2, 0.3, 5.0);  // peak σ physically bounded
            cb_fit.SetParLimits(3, 0.5, 5.0);
            cb_fit.SetParLimits(4, 1.1, 20.0);

            //  Stage 1: freeze background to prefit, find the peak.
            for (int i = 5; i < 9; ++i)
                cb_fit.FixParameter(i, bg_prefit.GetParameter(i - 5));
            h->Fit(&cb_fit, "RQ");
            //  Stage 2: release background, full fit using stage-1
            //  refined peak/σ as starting point.
            for (int i = 5; i < 9; ++i) cb_fit.ReleaseParameter(i);
            h->Fit(&cb_fit, "RQ");
            //  Stage 3: one more pass with everything free, starting
            //  from the converged stage-2 result.  Cheap; tightens
            //  the answer when stage 2 drifted off in a single step.
            h->Fit(&cb_fit, "RQM");
            //  cb_fit gets auto-attached to h's function list by Fit()
            //  and ships with the hist on h->Write — no separate
            //  cb_fit.Write() needed (and a separate write would leak
            //  a TF1 with no canvas owner, which TBrowser handles
            //  poorly).

            //  Background-only component (pol3 with the fitted bg
            //  params) — used inline below for the PDF canvas overlay
            //  and the N_γ extraction.  Not written to the ROOT file.
            TF1 bg_only((tag + "_bg_only").c_str(), "pol3", fit_lo, fit_hi);
            for (int i = 0; i < 4; ++i)
                bg_only.SetParameter(i, cb_fit.GetParameter(5 + i));

            //  N_γ per ring = signal-only integral.  Since the hist
            //  was scaled by 1/N_rings up top, this integral is now
            //  directly the per-ring count.  Compute by integrating
            //  the full fit, subtracting the bg-only integral, then
            //  dividing by the HIST'S OWN bin width to convert from
            //  TF1::Integral's "amplitude × mm" to "amplitude × bins"
            //  = Σ bin contents = photons per ring.
            const double total_int = cb_fit.Integral(fit_lo, fit_hi);
            const double bg_int    = bg_only.Integral(fit_lo, fit_hi);
            const double bin_width = h->GetXaxis()->GetBinWidth(1);
            const double n_gamma   = (total_int - bg_int) / bin_width;
            //  Keep n_gamma_total (= total across run) for legacy log
            //  output: multiply back by N_rings.  Useful for spotting
            //  high-stats anomalies independent of ring count.
            const double n_gamma_total = (n_rings > 0)
                ? n_gamma * static_cast<double>(n_rings)
                : n_gamma;

            //  ── Canvas with hist + fit + values, written as TWO PDFs ──
            //  Same pattern as `macros/examples/photon_number_new.cpp`:
            //  in-memory canvas, Draw + DrawCopy, SaveAs PDF.  No
            //  ROOT-file write (TF1 + TCanvas serialization is fragile).
            //
            //  Full-range plot (no μ ± 4σ zoom — user reverted to the
            //  whole hist axis so the background tails are visible).
            //  Both linear and log-Y PDFs saved (the log version
            //  surfaces the background structure clearly).
            {
                TCanvas c(("c_" + tag).c_str(),
                          ("Radial fit: " + tag).c_str(),
                          900, 650);
                c.SetGrid();
                h->Draw("E1");                  // data + auto fit overlay
                bg_only.SetLineColor(kGray + 2);
                bg_only.SetLineStyle(2);
                bg_only.DrawCopy("same");       // pol3 background dashed

                //  Full fit-parameter table on the canvas.  Top group:
                //  headline physics (N_γ, χ²/ndf).  Middle: 5-param
                //  Crystal-Ball (amp, μ, σ, α, n).  Bottom: 4-param
                //  pol3 background (c0..c3).  Errors shown where
                //  meaningful (CB & μ/σ); pol3 errors omitted to keep
                //  the box compact — they're available on the fit
                //  result if needed.
                const double chi2 = cb_fit.GetChisquare();
                const int    ndf  = cb_fit.GetNDF();
                const double chi2_per_ndf = (ndf > 0) ? chi2 / ndf : 0.0;

                //  NDC corners as requested: (0.9, 0.9, 0.65, 0.5).
                //  ROOT normalises the rectangle, so this lands in the
                //  upper-right corner (x: 0.65–0.9, y: 0.5–0.9).
                TPaveText pave(0.9, 0.9, 0.65, 0.5, "NDC");
                pave.SetFillStyle(0);
                pave.SetBorderSize(1);
                pave.SetTextAlign(12);
                pave.SetTextSize(0.028);
                pave.AddText(TString::Format("N_{#gamma} / ring = %.2f", n_gamma).Data());
                pave.AddText(TString::Format("over %ld rings", n_rings).Data());
                pave.AddText(TString::Format("#chi^{2}/ndf = %.2f / %d = %.2f",
                                              chi2, ndf, chi2_per_ndf).Data());
                pave.AddText(" ");
                pave.AddText("Crystal-Ball:");
                pave.AddText(TString::Format("  amp = %.3g #pm %.2g",
                    cb_fit.GetParameter(0), cb_fit.GetParError(0)).Data());
                pave.AddText(TString::Format("  #mu = %.3f #pm %.3f mm",
                    cb_fit.GetParameter(1), cb_fit.GetParError(1)).Data());
                pave.AddText(TString::Format("  #sigma = %.3f #pm %.3f mm",
                    cb_fit.GetParameter(2), cb_fit.GetParError(2)).Data());
                pave.AddText(TString::Format("  #alpha = %.3f #pm %.3f",
                    cb_fit.GetParameter(3), cb_fit.GetParError(3)).Data());
                pave.AddText(TString::Format("  n = %.3f #pm %.3f",
                    cb_fit.GetParameter(4), cb_fit.GetParError(4)).Data());
                pave.AddText(" ");
                pave.AddText("pol3 background:");
                pave.AddText(TString::Format("  c_{0} = %.3g", cb_fit.GetParameter(5)).Data());
                pave.AddText(TString::Format("  c_{1} = %.3g", cb_fit.GetParameter(6)).Data());
                pave.AddText(TString::Format("  c_{2} = %.3g", cb_fit.GetParameter(7)).Data());
                pave.AddText(TString::Format("  c_{3} = %.3g", cb_fit.GetParameter(8)).Data());
                pave.Draw();

                //  Linear Y.
                c.SetLogy(0);
                c.Update();
                const std::string pdf_lin = data_repository + "/" + run_name +
                                             "/" + tag + ".pdf";
                c.SaveAs(pdf_lin.c_str());

                //  Log Y — same canvas, just flip the Y scale.
                c.SetLogy(1);
                c.Update();
                const std::string pdf_log = data_repository + "/" + run_name +
                                             "/" + tag + "_logy.pdf";
                c.SaveAs(pdf_log.c_str());
            }

            mist::logger::info(TString::Format(
                "(recodata_writer) %s: N_gamma/ring=%.2f  (total=%.0f over %ld rings)  "
                "chi2/ndf=%.2f/%d",
                tag.c_str(), n_gamma, n_gamma_total, n_rings,
                cb_fit.GetChisquare(), cb_fit.GetNDF()).Data());
            mist::logger::info(TString::Format(
                "(recodata_writer) %s   CB: amp=%.3g+/-%.2g  mu=%.3f+/-%.3f mm  "
                "sigma=%.3f+/-%.3f mm  alpha=%.3f+/-%.3f  n=%.3f+/-%.3f",
                tag.c_str(),
                cb_fit.GetParameter(0), cb_fit.GetParError(0),
                cb_fit.GetParameter(1), cb_fit.GetParError(1),
                cb_fit.GetParameter(2), cb_fit.GetParError(2),
                cb_fit.GetParameter(3), cb_fit.GetParError(3),
                cb_fit.GetParameter(4), cb_fit.GetParError(4)).Data());
            mist::logger::info(TString::Format(
                "(recodata_writer) %s   pol3 bg: c0=%.3g  c1=%.3g  c2=%.3g  c3=%.3g",
                tag.c_str(),
                cb_fit.GetParameter(5), cb_fit.GetParameter(6),
                cb_fit.GetParameter(7), cb_fit.GetParameter(8)).Data());

            //  Push into the summary collector.
            radial_results.push_back({tag, n_gamma,
                cb_fit.GetParameter(1), cb_fit.GetParError(1),
                cb_fit.GetParameter(2), cb_fit.GetParError(2)});
        };

        //  Apply to every eff-corrected radial hist: first, second,
        //  and dual/solo splits for the first ring.  Second ring is
        //  dual-by-definition so no _dual/_solo split.
        fit_radial_distribution(h_radial_first.get(),       h_R_first.get(),       "h_radial_first");
        fit_radial_distribution(h_radial_second.get(),      h_R_second.get(),      "h_radial_second");
        fit_radial_distribution(h_radial_first_dual.get(),  h_R_first_dual.get(),  "h_radial_first_dual");
        fit_radial_distribution(h_radial_first_solo.get(),  h_R_first_solo.get(),  "h_radial_first_solo");

        //  ── Persistent CB+pol3 summary plots ──────────────────────
        //  Three bin-labeled TH1Fs collecting the headline numbers
        //  from each radial fit.  Operators glance at these for the
        //  per-run physics summary; downstream scripts read them
        //  programmatically without re-parsing TNamed strings.
        if (!radial_results.empty())
        {
            auto build_radial_summary = [&](const std::string &hname,
                                             const std::string &ytitle,
                                             auto value_extractor,
                                             auto error_extractor)
            {
                TH1F *h = new TH1F(hname.c_str(),
                                    (";radial source;" + ytitle).c_str(),
                                    static_cast<int>(radial_results.size()),
                                    0, static_cast<double>(radial_results.size()));
                const std::string prefix = "h_radial_";
                for (size_t i = 0; i < radial_results.size(); ++i) {
                    std::string label = radial_results[i].name;
                    if (label.rfind(prefix, 0) == 0)
                        label = label.substr(prefix.size());
                    h->GetXaxis()->SetBinLabel(static_cast<int>(i) + 1, label.c_str());
                    h->SetBinContent(static_cast<int>(i) + 1, value_extractor(radial_results[i]));
                    h->SetBinError  (static_cast<int>(i) + 1, error_extractor(radial_results[i]));
                }
                h->SetMarkerStyle(20);
                h->SetMarkerSize(1.0);
                h->Write();
                delete h;
            };
            build_radial_summary("h_N_gamma_per_ring_summary",
                                  "N_{#gamma} / ring",
                                  [](const RadialFitResult &r) { return r.n_gamma; },
                                  [](const RadialFitResult &)   { return 0.0; });
            build_radial_summary("h_peak_mu_summary", "peak #mu (mm)",
                                  [](const RadialFitResult &r) { return r.peak_mu; },
                                  [](const RadialFitResult &r) { return r.peak_mu_err; });
            build_radial_summary("h_peak_sigma_summary", "peak #sigma (mm)",
                                  [](const RadialFitResult &r) { return r.peak_sigma; },
                                  [](const RadialFitResult &r) { return r.peak_sigma_err; });
        }

        //  Headline physics observables.  Ring radius, single-photon
        //  σ, R-vs-N correlation, centre map per ring.  These are the
        //  TBrowser-readable physics QA: glance at h_R_* for the
        //  Cherenkov ring radius, h_sigma_* for single-photon
        //  resolution, h_centre_xy_* for beam centre/drift.
        h_R_first->Write();
        h_R_second->Write();
        h_R_first_dual->Write();
        h_R_first_solo->Write();
        h_sigma_first->Write();
        h_sigma_second->Write();
        h_centre_xy_first->Write();
        h_centre_xy_second->Write();

        //  vs_n fitting recipe (DISCUSSION § 2.6).
        //
        //  For each vs_n TH2F:
        //    1. FitSlicesY → Gaussian fit per X (N_hits) slice;
        //       ROOT auto-creates `<name>_2` containing σ per slice.
        //    2. Fit that σ(N) hist with sqrt([0]/x + [1]) — the
        //       canonical statistical-floor model:
        //          σ²(N) = A/N + B
        //       where A is the per-hit statistical contribution and
        //       B is the irreducible floor (σ_photon² for residuals;
        //       intrinsic ring-radius spread for R hists).
        //    3. Write both the sliced-σ hist and the fitted TF1 next
        //       to the parent TH2F.
        //
        //  Fit range [5, 40]: 5 = min_hits_per_ring; 40 = X-axis upper
        //  edge of the vs_n hists.  Slices below 5 hits don't pass the
        //  refit gate, slices above 40 don't exist.  Initial params:
        //  A=1, B=0.5 — gentle bias toward a non-zero floor; the LM
        //  converges in a few iterations for both R and residual hists.
        //
        //  Caveats: FitSlicesY uses ROOT's default Gaussian — for
        //  slices with non-Gaussian tails (e.g. fake-ring outliers in
        //  h_R_vs_nhits) the σ estimate is biased.  If the locus
        //  looks pathological, switch to a custom slice-fit (Gaussian
        //  on linear background, range-limited).  Not done in V1.
        //  Collector for durable σ_photon QA — populated by
        //  `fit_sigma_vs_n` below, used at the bottom of this block
        //  to build the `h_sigma_photon_summary` / `h_sigma_R_summary`
        //  TH1Fs.  Better QA than the per-hist TNamed scalars because
        //  it shows all ring slots side-by-side in a single TBrowser
        //  plot — operators can spot dual/solo or first/second
        //  asymmetries at a glance.
        struct VsNFitResult {
            std::string name;
            double sigma_photon;
            double sigma_photon_err;
            bool is_residual;   ///< true → goes into σ_photon summary;
                                ///< false → goes into σ_R summary
        };
        std::vector<VsNFitResult> vs_n_results;

        auto fit_sigma_vs_n = [&](TH2F *h2)
        {
            if (!h2) {
                mist::logger::warning(
                    "(recodata_writer) fit_sigma_vs_n: null TH2F; skipping.");
                return;
            }
            if (h2->GetEntries() == 0) {
                mist::logger::warning(TString::Format(
                    "(recodata_writer) %s: TH2F empty (0 entries) — "
                    "no σ-vs-N PDF emitted.",
                    h2->GetName()).Data());
                return;
            }
            // Manual per-slice σ extraction — replaces TH2::FitSlicesY,
            // which was silently producing no σ slot on small/sparse
            // residual hists (cause unclear; varies with ROOT version
            // and option string).  Doing it by hand is ~15 lines and
            // gives us full control over the per-slice fit range,
            // minimum-entries cut, and seed values — easier to debug
            // when a slot returns no σ point.
            const std::string sigma_name = std::string(h2->GetName()) + "_2";
            TH1F *h_sigma_slice = new TH1F(
                sigma_name.c_str(),
                (std::string(";") + h2->GetXaxis()->GetTitle() +
                 ";#sigma (mm)").c_str(),
                h2->GetNbinsX(),
                h2->GetXaxis()->GetXmin(),
                h2->GetXaxis()->GetXmax());
            h_sigma_slice->SetDirectory(gDirectory);  // persisted in Rings/
            h_sigma_slice->SetMarkerStyle(20);
            h_sigma_slice->SetMarkerSize(0.8);

            constexpr int kMinSliceEntries = 5;
            int n_slices_fit = 0;
            for (int ix = 1; ix <= h2->GetNbinsX(); ++ix) {
                std::unique_ptr<TH1D> slice(h2->ProjectionY(
                    (sigma_name + "_slice_" + std::to_string(ix)).c_str(),
                    ix, ix));
                slice->SetDirectory(nullptr);
                if (slice->GetEntries() < kMinSliceEntries) continue;

                //  Two-pass Gaussian fit for robustness against fake-ring
                //  outliers contaminating each N-slice:
                //
                //    Pass 1 ("seeding"): fit in [mean - 2·RMS, mean + 2·RMS].
                //      Tight enough to lock onto the core peak; loose enough
                //      that a reasonable Gaussian seed is found even with
                //      ~10–20% outlier contamination.
                //    Pass 2 ("polish"): refit in [μ₁ - 2.5·σ₁, μ₁ + 2.5·σ₁]
                //      using pass-1 μ,σ.  Locks the fit window onto the actual
                //      peak (not the raw distribution's mean which can be
                //      dragged by outliers).  The 2.5σ window contains 98.8%
                //      of a true Gaussian so we don't lose signal; outliers
                //      beyond that are excluded from the χ².
                //
                //  Previous single-pass [mean ± 3·RMS] was producing σ ≈ 13 mm
                //  on the R-vs-nhits hists where fake rings populate broad
                //  tails — both mean and RMS were dragged by the tails, the
                //  fit range was huge, and the Gaussian width converged to
                //  ~3× the true core width.
                const double mean0 = slice->GetMean();
                const double rms0  = slice->GetRMS();
                TF1 gfit("gfit", "gaus",
                         mean0 - 2.0 * rms0,
                         mean0 + 2.0 * rms0);
                gfit.SetParameters(slice->GetMaximum(),
                                    mean0,
                                    std::max(rms0, 0.1));
                int fit_status = static_cast<int>(slice->Fit(&gfit, "RQ0"));
                if (fit_status != 0) continue;

                //  Pass 2 refit, seeded from pass-1 result.
                const double mu1    = gfit.GetParameter(1);
                const double sigma1 = std::abs(gfit.GetParameter(2));
                if (sigma1 > 0. && std::isfinite(sigma1)) {
                    gfit.SetRange(mu1 - 2.5 * sigma1, mu1 + 2.5 * sigma1);
                    fit_status = static_cast<int>(slice->Fit(&gfit, "RQ0"));
                    if (fit_status != 0) continue;
                }
                const double sigma     = std::abs(gfit.GetParameter(2));
                const double sigma_err = gfit.GetParError(2);
                if (sigma <= 0. || !std::isfinite(sigma)) continue;
                h_sigma_slice->SetBinContent(ix, sigma);
                h_sigma_slice->SetBinError  (ix, sigma_err);
                ++n_slices_fit;
            }
            if (n_slices_fit == 0) {
                mist::logger::warning(TString::Format(
                    "(recodata_writer) %s: 0 slices passed per-slice "
                    "Gaussian fit (min entries = %d) — skipping σ-vs-N PDF.",
                    h2->GetName(), kMinSliceEntries).Data());
                delete h_sigma_slice;
                return;
            }
            // sqrt(A/N+B) is a 2-param fit; need at least 3 σ points
            // to have any constraint.
            if (n_slices_fit < 3) {
                mist::logger::warning(TString::Format(
                    "(recodata_writer) %s: only %d slices yielded σ — "
                    "cannot fit sqrt(A/N+B). Skipping PDF.",
                    h2->GetName(), n_slices_fit).Data());
                return;
            }
            //  Restrict fit range to bins that actually have content —
            //  empty bins at the edges shouldn't be in [xmin,xmax] for
            //  the fit, or LM will widen the χ² window over zero.
            int first_pop = 0, last_pop = 0;
            for (int ib = 1; ib <= h_sigma_slice->GetNbinsX(); ++ib) {
                if (h_sigma_slice->GetBinContent(ib) > 0.) {
                    if (first_pop == 0) first_pop = ib;
                    last_pop = ib;
                }
            }
            const double fit_x_lo = h_sigma_slice->GetBinLowEdge(first_pop);
            const double fit_x_hi = h_sigma_slice->GetBinLowEdge(last_pop)
                                  + h_sigma_slice->GetBinWidth(last_pop);

            TF1 *f_scaling = new TF1(
                (std::string(h2->GetName()) + "_scaling").c_str(),
                "sqrt([0]/x + [1])", fit_x_lo, fit_x_hi);
            f_scaling->SetParameters(1.0, 0.5);
            f_scaling->SetParNames("A", "B");
            //  ParLimits: A ≥ 0 (statistical contribution is positive),
            //  B ≥ 0 (σ² floor can't be negative — this was the prior
            //  bug that produced B = -0.484 → σ_photon = √(-0.484) =
            //  NaN → canvas paint failure).  Upper bounds generous.
            f_scaling->SetParLimits(0, 0., 1e4);
            f_scaling->SetParLimits(1, 0., 1e2);
            h_sigma_slice->Fit(f_scaling, "RQS");

            //  ── Canvas with σ-per-N hist + fit + values, as PDF ──
            //  Same pattern as the radial canvas above (and as the
            //  offline `photon_number_new.cpp` macro): in-memory
            //  canvas + SaveAs PDF.  No ROOT-file write.
            //
            //  σ²(N) = A/N + B is the canonical statistical-floor
            //  decomposition of a per-sample-averaged measurement:
            //      • A = σ²_per-sample  → σ_photon = √A
            //        (the per-hit spatial precision; this is the
            //        physically meaningful "single-photon resolution"
            //        operators quote.)
            //      • B = σ²_floor       → √B = irreducible spread
            //        (for R_vs_nhits: intrinsic ring-radius variation;
            //         for residual_vs_n: degenerate with A — both
            //         should equal σ²_photon for LOO residuals.)
            //  Previous code reported σ_photon = √B which was only
            //  correct by coincidence on the residual hist (where A
            //  and B are degenerate) and outright wrong on R_vs_nhits.
            {
                const double A_for_pave         = f_scaling->GetParameter(0);
                const double A_err_for_pave     = f_scaling->GetParError(0);
                const double B_for_pave         = f_scaling->GetParameter(1);
                const double B_err_for_pave     = f_scaling->GetParError(1);
                const double sigma_for_pave     = (A_for_pave > 0.) ? std::sqrt(A_for_pave) : 0.;
                const double sigma_err_for_pave = (A_for_pave > 0.)
                    ? 0.5 * A_err_for_pave / std::sqrt(A_for_pave)
                    : 0.;
                const double floor_for_pave     = (B_for_pave > 0.) ? std::sqrt(B_for_pave) : 0.;
                const double floor_err_for_pave = (B_for_pave > 0.)
                    ? 0.5 * B_err_for_pave / std::sqrt(B_for_pave)
                    : 0.;

                TCanvas c(
                    (std::string("c_") + h2->GetName() + "_sigma_vs_n").c_str(),
                    (std::string("sigma vs N: ") + h2->GetName()).c_str(),
                    900, 650);
                c.SetGrid();
                //  Explicit X/Y range.  X clamped to the populated
                //  bins so the empty edges (which would otherwise
                //  drive Y autoranging to [0,10] and trip the
                //  "wmin==wmax" axis-paint error) don't appear.  Y
                //  hard-clamped to [0.5, 4.0] mm — operator-facing
                //  σ_photon comparison plots all use the same scale
                //  so dual/solo/first/second can be eyeballed
                //  side-by-side without auto-range zoom drift.
                h_sigma_slice->GetXaxis()->SetRangeUser(fit_x_lo, fit_x_hi);
                h_sigma_slice->GetYaxis()->SetRangeUser(0.5, 4.0);
                h_sigma_slice->Draw("E1");

                TPaveText pave(0.55, 0.60, 0.92, 0.88, "NDC");
                pave.SetFillStyle(0);
                pave.SetBorderSize(1);
                pave.SetTextAlign(12);
                pave.SetTextSize(0.032);
                pave.AddText("#sigma^{2}(N) = A/N + B");
                pave.AddText(TString::Format("A = %.4f #pm %.4f mm^{2}",
                    A_for_pave, A_err_for_pave).Data());
                pave.AddText(TString::Format("B = %.4f #pm %.4f mm^{2}",
                    B_for_pave, B_err_for_pave).Data());
                pave.AddText(TString::Format("#sigma_{photon} = #sqrt{A} = %.3f #pm %.3f mm",
                    sigma_for_pave, sigma_err_for_pave).Data());
                pave.AddText(TString::Format("#sqrt{B} (floor) = %.3f #pm %.3f mm",
                    floor_for_pave, floor_err_for_pave).Data());
                pave.Draw();

                c.Update();
                const std::string pdf_path = data_repository + "/" + run_name +
                                              "/" + h2->GetName() + "_sigma_vs_n.pdf";
                c.SaveAs(pdf_path.c_str());
            }

            //  ── Extract σ_photon as a labeled scalar ──────────────────
            //  σ²(N) = A/N + B → σ_photon = √A (the per-hit precision
            //  contribution; this is what averaging over N hits
            //  reduces as 1/√N).  B is the irreducible floor (a
            //  different number — for R hists it's the intrinsic
            //  ring-radius spread, for LOO residuals it's also
            //  σ²_photon in the degenerate decomposition).  Stored as
            //  a TNamed alongside the fit so downstream consumers
            //  (live displays, summary scripts) can read it without
            //  re-fitting.  Also echoed to the log so the operator
            //  sees the run's resolution in realtime.
            const double A          = f_scaling->GetParameter(0);
            const double A_err      = f_scaling->GetParError(0);
            const double B          = f_scaling->GetParameter(1);
            const double sigma_phot = (A > 0.) ? std::sqrt(A) : 0.;
            const double sigma_err  = (A > 0.) ? 0.5 * A_err / std::sqrt(A) : 0.;
            TNamed sigma_named(
                (std::string(h2->GetName()) + "_sigma_photon_mm").c_str(),
                TString::Format("%.4f +/- %.4f", sigma_phot, sigma_err).Data());
            sigma_named.Write();
            mist::logger::info(TString::Format(
                "(recodata_writer) %s: sigma_photon = sqrt(A) = %.3f +/- %.3f mm "
                "(fit: A=%.3f B=%.3f, sqrt(B) floor = %.3f mm)",
                h2->GetName(), sigma_phot, sigma_err,
                A, B, (B > 0.) ? std::sqrt(B) : 0.).Data());

            //  Push into the summary collector.  Tagging by hist-name
            //  prefix is uglier than a separate function arg but keeps
            //  the lambda single-purpose — minor.
            const std::string hname = h2->GetName();
            const bool is_residual  = hname.find("h_residual_vs_n_") == 0;
            vs_n_results.push_back({hname, sigma_phot, sigma_err, is_residual});
        };

        //  Apply to every vs_n hist: top-level (first/second) + dual/solo
        //  splits for the first ring.  Second ring is dual-by-definition.
        h_R_vs_nhits_first->Write();
        h_R_vs_nhits_second->Write();
        h_residual_vs_n_first->Write();
        h_residual_vs_n_second->Write();
        h_R_vs_nhits_first_dual->Write();
        h_R_vs_nhits_first_solo->Write();
        h_residual_vs_n_first_dual->Write();
        h_residual_vs_n_first_solo->Write();
        fit_sigma_vs_n(h_R_vs_nhits_first.get());
        fit_sigma_vs_n(h_R_vs_nhits_second.get());
        fit_sigma_vs_n(h_residual_vs_n_first.get());
        fit_sigma_vs_n(h_residual_vs_n_second.get());
        fit_sigma_vs_n(h_R_vs_nhits_first_dual.get());
        fit_sigma_vs_n(h_R_vs_nhits_first_solo.get());
        fit_sigma_vs_n(h_residual_vs_n_first_dual.get());
        fit_sigma_vs_n(h_residual_vs_n_first_solo.get());

        //  ── Persistent σ summary plots ────────────────────────────
        //  Two TH1Fs, bin-labeled by source, content = σ ± uncertainty
        //  from the sqrt(A/N + B) fit's floor (√B).  These are the
        //  durable QA plots: one glance shows σ_photon (single-hit
        //  resolution) and σ_R_intrinsic (irreducible ring-radius
        //  spread) across every ring slot — first / second / dual /
        //  solo — for at-a-glance dual-ring vs solo-ring comparison.
        //  Each summary plot has up to 4 bins (residuals → 4
        //  populations; R hists → same 4 populations) — non-fit-able
        //  ring slots show as empty bins.
        auto build_summary_hist = [&](const std::string &hname,
                                       const std::string &ytitle,
                                       bool want_residual)
        {
            //  Pick the matching set out of the collector.
            std::vector<const VsNFitResult*> selected;
            for (const auto &r : vs_n_results)
                if (r.is_residual == want_residual)
                    selected.push_back(&r);
            if (selected.empty()) return;

            TH1F *h = new TH1F(hname.c_str(),
                                (";source;" + ytitle).c_str(),
                                static_cast<int>(selected.size()),
                                0, static_cast<double>(selected.size()));
            for (size_t i = 0; i < selected.size(); ++i) {
                //  Strip the common prefix for a tighter axis label.
                std::string label = selected[i]->name;
                const std::string prefix_resid = "h_residual_vs_n_";
                const std::string prefix_R     = "h_R_vs_nhits_";
                if (label.rfind(prefix_resid, 0) == 0)
                    label = label.substr(prefix_resid.size());
                else if (label.rfind(prefix_R, 0) == 0)
                    label = label.substr(prefix_R.size());
                h->GetXaxis()->SetBinLabel(static_cast<int>(i) + 1, label.c_str());
                h->SetBinContent(static_cast<int>(i) + 1, selected[i]->sigma_photon);
                h->SetBinError  (static_cast<int>(i) + 1, selected[i]->sigma_photon_err);
            }
            h->SetMarkerStyle(20);
            h->SetMarkerSize(1.0);
            h->Write();
            delete h;
        };
        build_summary_hist("h_sigma_photon_summary",
                            "#sigma_{photon} (mm)",
                            /*want_residual=*/true);
        build_summary_hist("h_sigma_R_intrinsic_summary",
                            "#sigma_{R, intrinsic} (mm)",
                            /*want_residual=*/false);
    }
    //
    //  input_file and output_file closed automatically by TFilePtr dtors.
    //  End: QA plots
    //  --- --- --- --- --- ---
}

