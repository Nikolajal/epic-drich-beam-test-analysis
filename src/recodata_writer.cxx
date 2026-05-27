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
#include "writers/recodata/types.h"          // RingFitResult, FrameResult, RingFillHists, RadialFitResult, VsNFitResult
#include "writers/recodata/radial_fit.h"     // fit_radial_distribution
#include "writers/recodata/sigma_vs_n_fit.h" // fit_sigma_vs_n
#include "writers/recodata/ring_compute.h"   // compute_ring_fit, fill_ring_hists, refit_and_fill_ring
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

//  RingFitResult, FrameResult, RingFillHists were previously defined
//  inline (in an anonymous namespace at file scope, plus a function-
//  scope struct).  They've been lifted to
//  `include/writers/recodata/types.h` so the per-frame compute and
//  finalize-QA split translation units can share the same definitions.
using ::btana::recodata::compute_ring_fit;
using ::btana::recodata::fill_ring_hists;
using ::btana::recodata::fit_radial_distribution;
using ::btana::recodata::fit_sigma_vs_n;
using ::btana::recodata::FrameResult;
using ::btana::recodata::RadialFitResult;
using ::btana::recodata::refit_and_fill_ring;
using ::btana::recodata::RingComputeContext;
using ::btana::recodata::RingFillHists;
using ::btana::recodata::RingFitResult;
using ::btana::recodata::VsNFitResult;

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill,
    bool force_rebuild,
    bool force_upstream,
    std::string mapping_conf,
    std::string trigger_conf,
    std::string framer_conf,
    std::string recodata_conf,
    std::string streaming_conf)
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
    //  if missing/corrupt or if force_upstream is set (CODE_REVIEW §D-06).
    //  TFilePtr is owning: closes + deletes on every exit path.
    std::string input_filename = data_repository + "/" + run_name + "/lightdata.root";
    TFilePtr input_file(TFile::Open(input_filename.c_str(), "READ"));
    if (!input_file || input_file->IsZombie() || force_upstream)
    {
        mist::logger::warning("(recodata_writer) " + input_filename +
                              " missing, corrupt, or rebuild forced — running lightdata_writer");
        // Forward the conf paths that recodata's CLI saw (resolved by
        // util::conf_path under --QA in main()).  trigger_conf, framer_conf,
        // and streaming_conf cascade — they affect lightdata's framing
        // + streaming-Hough trigger and need to follow the same QA-mode
        // resolution as the rest of the pipeline.  Other lightdata-only
        // paths (readout, fine-calib) stay at lightdata's own defaults
        // because recodata's CLI doesn't expose them and there is no QA
        // tuning for them today.
        lightdata_writer(data_repository, run_name, max_spill,
                         /*force_rebuild=*/force_upstream,
                         /*requested_n_threads=*/-1,
                         trigger_conf,
                         /*readout_config_file=*/"conf/readout_config.toml",
                         /*mapping_config_file=*/mapping_conf,
                         /*fine_calibration_config_file=*/"",
                         framer_conf,
                         streaming_conf);
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
                                            input_filename.c_str())
                                .Data());
        return;
    }
    AlcorSpilldata *spilldata = new AlcorSpilldata();
    spilldata->link_to_tree(lightdata_tree);

    AlcorFinedata::read_calib_from_file(data_repository + "/" + run_name + "/fine_calib.txt");

    auto fine_time_calib_th2f = input_file->Get<TH2F>("h_fine_calib");
    if (!fine_time_calib_th2f)
    {
        mist::logger::error(TString::Format("(recodata_writer) 'h_fine_calib' histogram missing in %s",
                                            input_filename.c_str())
                                .Data());
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
    if (std::filesystem::exists(outname) && !force_rebuild)
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
        constexpr int kDeviceLo = 192;
        constexpr int kDeviceHi = 224;
        const int max_chip = ::gidx::kUsesSplitInTwo ? 4 : 8;
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
    auto recodata_cfg = recodata_conf_reader(recodata_conf);

    //  Captured-once state for the per-frame, per-ring compute
    //  helpers (`compute_ring_fit`, `refit_and_fill_ring`).  Geometry +
    //  config knobs that don't change during the loop.  See
    //  `include/writers/recodata/ring_compute.h`.
    const RingComputeContext ring_ctx{index_to_hit_xy, recodata_cfg};

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
    std::vector<int> n_physics_per_spill(all_spills, 0);
    std::vector<std::set<int>> active_channels_per_spill(all_spills);

    //  Per-ring QA hists.  Same binning as the coverage map's R axis
    //  on radial hists so `eff(R)` can be `Divide`d cleanly at finalize.
    const int radial_n_bins = recodata_cfg.n_r_bins_coverage;
    const float radial_lo_mm = recodata_cfg.r_min_coverage_mm;
    const float radial_hi_mm = recodata_cfg.r_max_coverage_mm;

    //  N-hits axis range used by every N-hits hist below — 1D and 2D.
    //  Observed beam-test max is ~20 hits/ring; 25 leaves a 5-bin
    //  headroom for outliers without wasting half the axis on empty
    //  bins.  1 hit / bin (integer X).  Tighten to 20 if rings get
    //  trimmed further; widen if a future run sees N > 25.
    constexpr int kNHitsBins = 25;
    constexpr float kNHitsXLo = 0.f;
    constexpr float kNHitsXHi = 25.f;

    RootHist<TH1F> h_nhits_first("h_nhits_first", ";N hits in ring 1;Events",
                                 kNHitsBins, kNHitsXLo, kNHitsXHi);
    RootHist<TH1F> h_nhits_second("h_nhits_second", ";N hits in ring 2;Events",
                                  kNHitsBins, kNHitsXLo, kNHitsXHi);

    RootHist<TH1F> h_nphotons_first("h_nphotons_first", ";N photons (eff-corrected) ring 1;Events", 100, 0, 100);
    RootHist<TH1F> h_nphotons_second("h_nphotons_second", ";N photons (eff-corrected) ring 2;Events", 100, 0, 100);

    RootHist<TH1F> h_f_coverage_first("h_f_coverage_first", ";f_{coverage} ring 1;Events", 100, 0.f, 1.f);
    RootHist<TH1F> h_f_coverage_second("h_f_coverage_second", ";f_{coverage} ring 2;Events", 100, 0.f, 1.f);

    //  Radial-hit distributions — binned 1 mm/bin (NOT the coarser
    //  radial_n_bins used by h_R_*).  Finer binning here for the
    //  CB+pol3 fit: the macro convention quotes σ_peak in mm so a
    //  binning matched to that precision avoids smearing.  Bin count
    //  is computed from the coverage R range.
    const int radial_hist_n_bins = static_cast<int>(
        std::round(recodata_cfg.r_max_coverage_mm - recodata_cfg.r_min_coverage_mm));
    RootHist<TH1F> h_radial_first("h_radial_first", ";R (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_radial_second("h_radial_second", ";R (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    //  Dual / solo splits for the first-ring radial distribution.
    //  Same predicate as the vs_n splits (frame has second ring?).
    //  Used by the Crystal-Ball + pol3 fit at finalize (DISCUSSION § 2.6)
    //  to extract N_γ separately for clean two-radiator events vs
    //  single-radiator events.  Second ring is dual-by-definition.
    RootHist<TH1F> h_radial_first_dual("h_radial_first_dual", ";R (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_radial_first_solo("h_radial_first_solo", ";R (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);

    //  Smeared sibling radial hists (pixel-jittered hit positions).
    //  Same binning as the pixel-centre versions so the two can be
    //  cross-checked bin-for-bin.  Physics observable: σ²_intrinsic =
    //  σ²_smeared − 2·(pitch²/12) = σ²_smeared − 1.5 mm² (at 3 mm pitch).
    RootHist<TH1F> h_radial_first_smeared("h_radial_first_smeared", ";R_{smeared} (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_radial_second_smeared("h_radial_second_smeared", ";R_{smeared} (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_radial_first_dual_smeared("h_radial_first_dual_smeared", ";R_{smeared} (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH1F> h_radial_first_solo_smeared("h_radial_first_solo_smeared", ";R_{smeared} (mm);hits / efficiency", radial_hist_n_bins, radial_lo_mm, radial_hi_mm);

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
    RootHist<TH1F> h_R_first("h_R_first", ";R_{fit} (mm);events", radial_n_bins, radial_lo_mm, radial_hi_mm);
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

    RootHist<TH1F> h_sigma_first("h_sigma_first", ";#sigma_{single} (mm);events", 100, 0.f, 5.f);
    RootHist<TH1F> h_sigma_second("h_sigma_second", ";#sigma_{single} (mm);events", 100, 0.f, 5.f);

    RootHist<TH2F> h_R_vs_nhits_first("h_R_vs_nhits_first", ";N hits;R_{fit} (mm)",
                                      kNHitsBins, kNHitsXLo, kNHitsXHi,
                                      radial_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH2F> h_R_vs_nhits_second("h_R_vs_nhits_second", ";N hits;R_{fit} (mm)",
                                       kNHitsBins, kNHitsXLo, kNHitsXHi,
                                       radial_n_bins, radial_lo_mm, radial_hi_mm);

    //  cx / cy half-range: hard-code to 25 mm for now; this is the
    //  same default as the lightdata-side QA's `centre_xy_half_range_mm`.
    //  Bin width 1 mm = generous for visual ring-centre clusters.
    constexpr float kCentreXyHalfRangeMm = 25.f;
    constexpr int kCentreXyBins = 50;
    RootHist<TH2F> h_centre_xy_first("h_centre_xy_first", ";c_{x} (mm);c_{y} (mm)",
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
    RootHist<TH2F> h_residual_vs_n_first("h_residual_vs_n_first", ";N hits;r_{hit} - R_{-i} (mm)",
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
    RootHist<TH2F> h_R_vs_nhits_first_dual("h_R_vs_nhits_first_dual", ";N hits;R_{fit} (mm)",
                                           kNHitsBins, kNHitsXLo, kNHitsXHi,
                                           radial_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH2F> h_R_vs_nhits_first_solo("h_R_vs_nhits_first_solo", ";N hits;R_{fit} (mm)",
                                           kNHitsBins, kNHitsXLo, kNHitsXHi,
                                           radial_n_bins, radial_lo_mm, radial_hi_mm);
    RootHist<TH2F> h_residual_vs_n_first_dual("h_residual_vs_n_first_dual", ";N hits;r_{hit} - R_{-i} (mm)",
                                              kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);
    RootHist<TH2F> h_residual_vs_n_first_solo("h_residual_vs_n_first_solo", ";N hits;r_{hit} - R_{-i} (mm)",
                                              kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);

    //  Smeared sibling LOO-residual hists.  Same axis convention as
    //  the pixel-centre versions; fed by `loo_residuals_smeared` in
    //  RingFitResult, using the SAME per-hit jitter realisation that
    //  populated the radial smeared hists above.  σ_photon recovery
    //  from the smeared hist needs σ²_intrinsic = σ²_smeared − 1.5 mm²
    //  (at 3 mm pitch), versus σ²_smeared − 0.75 mm² for the unsmeared.
    RootHist<TH2F> h_residual_vs_n_first_smeared("h_residual_vs_n_first_smeared", ";N hits;r_{hit,smeared} - R_{-i} (mm)",
                                                 kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);
    RootHist<TH2F> h_residual_vs_n_second_smeared("h_residual_vs_n_second_smeared", ";N hits;r_{hit,smeared} - R_{-i} (mm)",
                                                  kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);
    RootHist<TH2F> h_residual_vs_n_first_dual_smeared("h_residual_vs_n_first_dual_smeared", ";N hits;r_{hit,smeared} - R_{-i} (mm)",
                                                      kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);
    RootHist<TH2F> h_residual_vs_n_first_solo_smeared("h_residual_vs_n_first_solo_smeared", ";N hits;r_{hit,smeared} - R_{-i} (mm)",
                                                      kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);

    //  Per-frame, per-ring compute helpers live in their own
    //  translation unit since 2026-05-27 (Phase D of the recodata
    //  modularization).  See `include/writers/recodata/ring_compute.h`:
    //   - `compute_ring_fit(ring_bit, lightdata, do_loo, ring_ctx)` —
    //     pure compute, returns a `RingFitResult`; safe on worker
    //     threads.  Initial guess: hit centroid + median radius
    //     (no dependence on the streaming-Hough `fit_circle_init_*`
    //     knobs).  `do_loo = false` skips the per-hit LOO loop.
    //   - `fill_ring_hists(result, bundle)` — drain, mutates the
    //     histograms in `bundle` (any pointer may be nullptr to skip).
    //   - `refit_and_fill_ring(ring_bit, bundle, lightdata, ring_ctx)` —
    //     compose-then-drain wrapper for the serial code path; gates
    //     do_loo on `bundle.h_residual_vs_n` AND the cfg's
    //     `skip_loo_residuals` knob.
    //  All three take `ring_ctx` (declared above) for the geometry +
    //  config bundle that used to be captured by reference.

    //  Enable a 50 MB tree cache before the two GetEntry passes (§4.7 minimum
    //  mitigation): the second full pass over the spill tree (calibration loop
    //  at line 497, compute loop at line 569) re-reads every basket from disk
    //  without it.  Proper single-pass restructure remains the open item.
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
                        const int chip_raw = current_lane / 4;
                        const int channel_raw = 8 * (current_lane % 4) + i_channel;
                        const int chip_logical = ::gidx::kUsesSplitInTwo
                                                     ? chip_raw / 2
                                                     : chip_raw;
                        const int channel_log = ::gidx::kUsesSplitInTwo
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
                        continue;                                     // temporal duplicate
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
            if (res.rejected)
                return res;

            //  Per-spill physics check (DISCUSSION § 2.6).
            for (const auto &[idx, trig] : res.accepted_triggers)
            {
                if (idx == TriggerFirstFrames)
                    continue;
                if (idx == _TRIGGER_STREAMING_RING_FOUND_)
                    continue;
                if (idx == TriggerStartOfSpill)
                    continue;
                if (idx == _TRIGGER_UNKNOWN_)
                    continue;
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
                //  do_loo gated by the recodata config knob.
                //  `skip_loo_residuals = true` (typically in
                //  conf/QA/recodata.toml) disables the per-hit
                //  leave-one-out fit loop — the biggest single
                //  speedup lever for QA mode at the cost of no
                //  σ_photon measurement.  See RecodataConfigStruct.
                const bool do_loo = !ring_ctx.cfg.skip_loo_residuals;
                res.first = compute_ring_fit(HitmaskHoughRingTagFirst,
                                             lightdata, do_loo, ring_ctx);
                res.second = compute_ring_fit(HitmaskHoughRingTagSecond,
                                              lightdata, do_loo, ring_ctx);
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
                                            registry.name_of(idx).c_str())
                                .Data(),
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
                first_hists.h_nhits = h_nhits_first.get();
                first_hists.h_nphotons = h_nphotons_first.get();
                first_hists.h_fcov = h_f_coverage_first.get();
                first_hists.h_radial = h_radial_first.get();
                first_hists.h_R = h_R_first.get();
                first_hists.h_sigma = h_sigma_first.get();
                first_hists.h_R_vs_nhits = h_R_vs_nhits_first.get();
                first_hists.h_centre_xy = h_centre_xy_first.get();
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
                //  Smeared sibling targets — same dual/solo predicate.
                first_hists.h_radial_smeared = h_radial_first_smeared.get();
                first_hists.h_residual_vs_n_smeared = h_residual_vs_n_first_smeared.get();
                first_hists.h_radial_split_smeared = res.frame_has_second_ring
                                                         ? h_radial_first_dual_smeared.get()
                                                         : h_radial_first_solo_smeared.get();
                first_hists.h_residual_vs_n_split_smeared = res.frame_has_second_ring
                                                                ? h_residual_vs_n_first_dual_smeared.get()
                                                                : h_residual_vs_n_first_solo_smeared.get();
                fill_ring_hists(res.first, first_hists);

                RingFillHists second_hists;
                second_hists.h_nhits = h_nhits_second.get();
                second_hists.h_nphotons = h_nphotons_second.get();
                second_hists.h_fcov = h_f_coverage_second.get();
                second_hists.h_radial = h_radial_second.get();
                second_hists.h_R = h_R_second.get();
                second_hists.h_sigma = h_sigma_second.get();
                second_hists.h_R_vs_nhits = h_R_vs_nhits_second.get();
                second_hists.h_centre_xy = h_centre_xy_second.get();
                second_hists.h_residual_vs_n = h_residual_vs_n_second.get();
                //  Second ring has no dual/solo split (always "dual"
                //  by definition); just plug the smeared headline hists.
                second_hists.h_radial_smeared = h_radial_second_smeared.get();
                second_hists.h_residual_vs_n_smeared = h_residual_vs_n_second_smeared.get();
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
                                   n_frames, n_threads)
                                   .Data());

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
        auto tick_progress = [&](size_t my_completion_idx)
        {
            if ((my_completion_idx & 63) == 0)
                post_processing.update(done.load(std::memory_order_relaxed),
                                       n_frames);
        };

        if (n_threads <= 1)
        {
            for (size_t iframe = 0; iframe < n_frames; ++iframe)
            {
                AlcorLightdata cur(frames_in_spill[iframe]);
                frame_results[iframe] = process_frame_pure(cur);
                const size_t now_done = done.fetch_add(1) + 1;
                tick_progress(now_done);
            }
        }
        else
        {
            std::atomic<size_t> next_frame{0};
            std::vector<std::future<void>> thread_pool;
            thread_pool.reserve(n_threads);
            for (size_t t = 0; t < n_threads; ++t)
            {
                thread_pool.push_back(std::async(std::launch::async, [&]()
                                                 {
                    while (true) {
                        const size_t my = next_frame.fetch_add(1);
                        if (my >= n_frames) return;
                        AlcorLightdata cur(frames_in_spill[my]);
                        frame_results[my] = process_frame_pure(cur);
                        const size_t now_done = done.fetch_add(1) + 1;
                        tick_progress(now_done);
                    } }));
            }
            for (auto &f : thread_pool)
                f.get();
        }
        //  Snap to 100% so the bar reflects "compute finished" even
        //  when the last ticks fell between mod-64 thresholds.
        post_processing.update(n_frames, n_frames);

        //  Serial drain in frame order.  All hist fills, recodata
        //  add_*, tree Fill, per-spill counter updates happen here.
        //  Bar is already at 100% from the compute snap above; this
        //  loop is fast so no in-loop ticks needed.
        for (size_t iframe = 0; iframe < n_frames; ++iframe)
        {
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
                //  Smeared sibling targets — same dual/solo predicate.
                first_hists.h_radial_smeared        = h_radial_first_smeared.get();
                first_hists.h_residual_vs_n_smeared = h_residual_vs_n_first_smeared.get();
                first_hists.h_radial_split_smeared  = frame_has_second_ring
                    ? h_radial_first_dual_smeared.get()
                    : h_radial_first_solo_smeared.get();
                first_hists.h_residual_vs_n_split_smeared = frame_has_second_ring
                    ? h_residual_vs_n_first_dual_smeared.get()
                    : h_residual_vs_n_first_solo_smeared.get();
                refit_and_fill_ring(HitmaskHoughRingTagFirst, first_hists, current_lightdata, ring_ctx);

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
                second_hists.h_radial_smeared        = h_radial_second_smeared.get();
                second_hists.h_residual_vs_n_smeared = h_residual_vs_n_second_smeared.get();
                refit_and_fill_ring(HitmaskHoughRingTagSecond, second_hists, current_lightdata, ring_ctx);
            }

            recodata_tree->Fill();
            recodata.clear();
            n_accepted++;
            h_frames_per_spill->Fill(i_spill, 1.5); // accepted
        }
#endif // ── end original loop body, replaced by process+drain above ──

        mist::logger::info(TString::Format("Spill %i done — accepted: %i  had-edge: %i  duplicate-rejected: %i  total: %zu",
                                           i_spill, n_accepted, n_edge, n_duplicate, frames_in_spill.size())
                               .Data());

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
        for (int n : n_physics_per_spill)
            total_physics += n;
        if (total_physics > 0)
        {
            for (int is = 0; is < all_spills; ++is)
            {
                if (n_physics_per_spill[is] <= 0)
                    continue;
                if (active_channels_per_spill[is].empty())
                    continue;
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
                                   index_to_hit_xy.size())
                                   .Data());
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

            h_radial_first->Divide(eff_R.get());
            h_radial_second->Divide(eff_R.get());
            h_radial_first_dual->Divide(eff_R.get());
            h_radial_first_solo->Divide(eff_R.get());

            //  Same eff(R) correction on the smeared sibling hists —
            //  the smearing acts at the per-hit level so the geometric
            //  acceptance correction is identical.
            h_radial_first_smeared->Divide(eff_R.get());
            h_radial_second_smeared->Divide(eff_R.get());
            h_radial_first_dual_smeared->Divide(eff_R.get());
            h_radial_first_solo_smeared->Divide(eff_R.get());

            eff_R->Write();
        }
        else
        {
            mist::logger::warning("(recodata_writer) radial_efficiency returned null; "
                                  "radial hists are NOT efficiency-corrected.");
        }

        h_nhits_first->Write();
        h_nhits_second->Write();
        h_nphotons_first->Write();
        h_nphotons_second->Write();
        h_f_coverage_first->Write();
        h_f_coverage_second->Write();
        h_radial_first->Write();
        h_radial_second->Write();
        h_radial_first_dual->Write();
        h_radial_first_solo->Write();
        h_radial_first_smeared->Write();
        h_radial_second_smeared->Write();
        h_radial_first_dual_smeared->Write();
        h_radial_first_solo_smeared->Write();

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
        //  bin-labeled TH1Fs at the end of this block.  Type lives in
        //  `include/writers/recodata/types.h`; the fit itself is
        //  implemented in `src/writers/recodata/radial_fit.cxx`.
        std::vector<RadialFitResult> radial_results;

        //  `h_R_count` is the per-event ring-radius hist matching the
        //  radial hist's sample (same dual/solo gate).  Used purely to
        //  obtain `N_rings = GetEntries()` for the per-ring N_γ.  Pass
        //  explicitly rather than looking up by name — was previously a
        //  gDirectory dependency that broke when h_R writes came AFTER
        //  the fit calls in the finalize block (n_rings = 0 bug, fixed
        //  by 2026-05-26 by making it a parameter).
        //
        //  In-function lambda was lifted to a free function on 2026-05-27
        //  to reduce `recodata_writer.cxx` from 2.2k lines (DISCUSSION
        //  modularisation pass).  See `writers/recodata/radial_fit.h`
        //  for the signature; behavior is identical, verified against
        //  baseline `Data/20251111-164951/baseline_pre_refactor/`.

        //  Apply to every eff-corrected radial hist: first, second,
        //  and dual/solo splits for the first ring.  Second ring is
        //  dual-by-definition so no _dual/_solo split.
        fit_radial_distribution(h_radial_first.get(), h_R_first.get(), "h_radial_first",
                                recodata_cfg, data_repository, run_name, radial_results);
        fit_radial_distribution(h_radial_second.get(), h_R_second.get(), "h_radial_second",
                                recodata_cfg, data_repository, run_name, radial_results);
        fit_radial_distribution(h_radial_first_dual.get(), h_R_first_dual.get(), "h_radial_first_dual",
                                recodata_cfg, data_repository, run_name, radial_results);
        fit_radial_distribution(h_radial_first_solo.get(), h_R_first_solo.get(), "h_radial_first_solo",
                                recodata_cfg, data_repository, run_name, radial_results);

        //  Smeared sibling fits — same recipe.  These will be compared
        //  bin-for-bin to the un-smeared versions; σ_intrinsic is then
        //  recovered via σ²_intrinsic = σ²_observed − k·(pitch²/12)
        //  with k=1 (unsmeared) or k=2 (smeared).
        fit_radial_distribution(h_radial_first_smeared.get(), h_R_first.get(), "h_radial_first_smeared",
                                recodata_cfg, data_repository, run_name, radial_results);
        fit_radial_distribution(h_radial_second_smeared.get(), h_R_second.get(), "h_radial_second_smeared",
                                recodata_cfg, data_repository, run_name, radial_results);
        fit_radial_distribution(h_radial_first_dual_smeared.get(), h_R_first_dual.get(), "h_radial_first_dual_smeared",
                                recodata_cfg, data_repository, run_name, radial_results);
        fit_radial_distribution(h_radial_first_solo_smeared.get(), h_R_first_solo.get(), "h_radial_first_solo_smeared",
                                recodata_cfg, data_repository, run_name, radial_results);

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
                for (size_t i = 0; i < radial_results.size(); ++i)
                {
                    std::string label = radial_results[i].name;
                    if (label.rfind(prefix, 0) == 0)
                        label = label.substr(prefix.size());
                    h->GetXaxis()->SetBinLabel(static_cast<int>(i) + 1, label.c_str());
                    h->SetBinContent(static_cast<int>(i) + 1, value_extractor(radial_results[i]));
                    h->SetBinError(static_cast<int>(i) + 1, error_extractor(radial_results[i]));
                }
                h->SetMarkerStyle(20);
                h->SetMarkerSize(1.0);
                h->Write();
                delete h;
            };
            build_radial_summary("h_N_gamma_per_ring_summary", "N_{#gamma} / ring", [](const RadialFitResult &r)
                                 { return r.n_gamma; }, [](const RadialFitResult &)
                                 { return 0.0; });
            build_radial_summary("h_peak_mu_summary", "peak #mu (mm)", [](const RadialFitResult &r)
                                 { return r.peak_mu; }, [](const RadialFitResult &r)
                                 { return r.peak_mu_err; });
            build_radial_summary("h_peak_sigma_summary", "peak #sigma (mm)", [](const RadialFitResult &r)
                                 { return r.peak_sigma; }, [](const RadialFitResult &r)
                                 { return r.peak_sigma_err; });
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
        //  For each h_residual_vs_n_* TH2F:
        //    1. Per-slice Gaussian fit to extract σ(N) — the width of
        //       the LOO-residual distribution at each hit-count slice.
        //    2. Fit σ(N) with the EXACT one-parameter LOO model:
        //
        //          σ(N) = σ_photon · sqrt( N / (N − 3) )
        //
        //       Derivation: the LOO residual Δr_i = r_hit,i − R_{-i}
        //       has Var(Δr_i) = σ²_photon · N/(N−3) because the three
        //       free parameters of the circle fit (cx, cy, R) each
        //       contribute σ²_photon/(N-1) to Var(R_{-i}), and their
        //       angular projections sum to 3σ²_photon/(N-1) ≈ 3σ²/N.
        //       The exact expression N/(N−3) replaces the approximate
        //       A/N + B expansion; it is valid for any N > 3.
        //
        //       One free parameter: σ_photon (mm).  The fitted value
        //       IS σ_photon — no √A vs √B ambiguity.
        //
        //  Fit range: populated bins with N > 3 (never an issue since
        //  min_hits_per_ring ≥ 5 in config).
        //  Collector for durable σ_photon QA — populated by
        //  `fit_sigma_vs_n` (free function in
        //  `src/writers/recodata/sigma_vs_n_fit.cxx`), consumed at the
        //  bottom of this block to build the `h_sigma_photon_summary`
        //  TH1F.  Type lives in `include/writers/recodata/types.h`.
        std::vector<VsNFitResult> vs_n_results;

        //  In-function lambda was lifted to a free function on 2026-05-27
        //  (DISCUSSION modularisation pass).  See
        //  `writers/recodata/sigma_vs_n_fit.h` for the signature.

        //  σ(N) fit applied to residual hists only.
        //  h_R_vs_nhits_*: fit suppressed for all ring slots — the
        //  A/N + B decomposition on R-vs-nhits conflates per-hit
        //  resolution with the ring's intrinsic radius variation and
        //  gives uninterpretable numbers.  The 2D hists are still
        //  written for visual inspection.
        h_R_vs_nhits_first->Write();
        h_R_vs_nhits_second->Write();
        h_residual_vs_n_first->Write();
        h_residual_vs_n_second->Write();
        h_R_vs_nhits_first_dual->Write();
        h_R_vs_nhits_first_solo->Write();
        h_residual_vs_n_first_dual->Write();
        h_residual_vs_n_first_solo->Write();
        //  Smeared sibling residual hists — written + fitted in parallel.
        h_residual_vs_n_first_smeared->Write();
        h_residual_vs_n_second_smeared->Write();
        h_residual_vs_n_first_dual_smeared->Write();
        h_residual_vs_n_first_solo_smeared->Write();
        fit_sigma_vs_n(h_residual_vs_n_first.get(),
                       data_repository, run_name, vs_n_results);
        fit_sigma_vs_n(h_residual_vs_n_second.get(),
                       data_repository, run_name, vs_n_results);
        fit_sigma_vs_n(h_residual_vs_n_first_dual.get(),
                       data_repository, run_name, vs_n_results);
        fit_sigma_vs_n(h_residual_vs_n_first_solo.get(),
                       data_repository, run_name, vs_n_results);
        fit_sigma_vs_n(h_residual_vs_n_first_smeared.get(),
                       data_repository, run_name, vs_n_results);
        fit_sigma_vs_n(h_residual_vs_n_second_smeared.get(),
                       data_repository, run_name, vs_n_results);
        fit_sigma_vs_n(h_residual_vs_n_first_dual_smeared.get(),
                       data_repository, run_name, vs_n_results);
        fit_sigma_vs_n(h_residual_vs_n_first_solo_smeared.get(),
                       data_repository, run_name, vs_n_results);

        //  ── Persistent σ summary plots ────────────────────────────
        //  One TH1F, bin-labeled by source, content = σ_photon ±
        //  uncertainty from the exact LOO fit σ(N)=σ_photon·√(N/(N-3))
        //  on residual hists.  4 bins: first / second / first_dual /
        //  first_solo.
        //  h_sigma_R_intrinsic_summary is omitted — all h_R_vs_nhits_*
        //  σ(N) fits are suppressed (the R observable mixes per-hit
        //  resolution with intrinsic ring-radius variation and gives
        //  uninterpretable numbers).
        auto build_summary_hist = [&](const std::string &hname,
                                      const std::string &ytitle,
                                      bool want_residual)
        {
            //  Pick the matching set out of the collector.
            std::vector<const VsNFitResult *> selected;
            for (const auto &r : vs_n_results)
                if (r.is_residual == want_residual)
                    selected.push_back(&r);
            if (selected.empty())
                return;

            TH1F *h = new TH1F(hname.c_str(),
                               (";source;" + ytitle).c_str(),
                               static_cast<int>(selected.size()),
                               0, static_cast<double>(selected.size()));
            for (size_t i = 0; i < selected.size(); ++i)
            {
                //  Strip the common prefix for a tighter axis label.
                std::string label = selected[i]->name;
                const std::string prefix_resid = "h_residual_vs_n_";
                const std::string prefix_R = "h_R_vs_nhits_";
                if (label.rfind(prefix_resid, 0) == 0)
                    label = label.substr(prefix_resid.size());
                else if (label.rfind(prefix_R, 0) == 0)
                    label = label.substr(prefix_R.size());
                h->GetXaxis()->SetBinLabel(static_cast<int>(i) + 1, label.c_str());
                h->SetBinContent(static_cast<int>(i) + 1, selected[i]->sigma_photon);
                h->SetBinError(static_cast<int>(i) + 1, selected[i]->sigma_photon_err);
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
