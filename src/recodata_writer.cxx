#include "utility.h"
#include <mist/logger/logger.h>
#include "alcor_data.h"
#include "TH1.h"
#include "TH2.h"
#include "TF1.h"
#include "TNamed.h"
#include "TCanvas.h"
#include "TPad.h"
#include "TLatex.h"
#include "TPaveText.h"
#include "TEllipse.h"
#include "TMarker.h"
#include "TLine.h"
#include "TArc.h"
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
#include "writers/recodata/ring_compute.h"   // compute_ring_fit_timewindow, fill_ring_hists
#include "writers/recodata/frame_pipeline.h" // process_frame_pure (parallel-dispatch entry point)
//  Live-QA pipeline: coverage map + eff(R) helpers
//  + per-ring fit_circle re-run on mask-tagged hits → N_photons /
//  radial(R) observables filled inline.
#include "utility/radiator_efficiency.h"
#include "analysis_results.h"
#include "utility/config_dump.h"
#include "utility/config_reader.h"
#include "utility/qa_publish.h" // util::qa::pdf_path + crop_pdf_inplace
#include <set>
#include <filesystem>
#include <memory>
#include <atomic>
#include <future>
#include <thread>

//  RingFitResult, FrameResult, RingFillHists were previously defined
//  inline (in an anonymous namespace at file scope, plus a function-
//  scope struct).  They've been lifted to
//  `include/writers/recodata/types.h` so the per-frame compute and
//  finalize-QA split translation units can share the same definitions.
using ::btana::recodata::fill_ring_hists;
using ::btana::recodata::fit_radial_distribution;
using ::btana::recodata::fit_sigma_vs_n;
using ::btana::recodata::FrameProcessContext;
using ::btana::recodata::FrameResult;
using ::btana::recodata::process_frame_pure;
using ::btana::recodata::RadialFitResult;
using ::btana::recodata::RingComputeContext;
using ::btana::recodata::RingFillHists;
using ::btana::recodata::RingFitResult;
using ::btana::recodata::VsNFitResult;

namespace
{

//  Ring-shape classification policy.  kAuto applies the significance + 2%-floor
//  test; the force modes override it (e.g. for a before/after comparison or when
//  the operator knows the optics).  Selected by the --force-ring / --force-ellipse
//  CLI flags.
enum class RingShapeMode
{
    kAuto,
    kForceCircle,
    kForceEllipse
};

//  Result of fitting the Cherenkov ring as an ellipse about a fixed centre.
//  `is_ellipse == false` is the retro-compat circle case: a == b == R, θ = 0,
//  so every downstream consumer that expects a single radius still works.
struct RingEllipse
{
    double a = 0.0, b = 0.0; // semi-axes (a ≥ b), mm
    double a_err = 0.0, b_err = 0.0;
    double theta_deg = 0.0;  // major-axis rotation, (-90, 90]
    bool is_ellipse = false; // false → circle fallback (a == b)
    bool ok = false;         // false → fit failed; caller keeps circle
};

//  Fit r(φ) = a·b / √((b·cos Δ)² + (a·sin Δ)²), Δ = φ − θ, over the ring band
//  about the (already well-determined) centre.  The centre is NOT re-fitted —
//  the ring-centre map pins it to sub-bin precision; here we only resolve the
//  shape.  Falls back to a circle (a == b == R, θ = 0) unless the ellipticity
//  is BOTH statistically significant (|a − b| > `n_sigma_circle`·σ_(a−b)) AND
//  physically non-negligible (|a − b| > `min_ellipticity_frac`·R).  The second
//  gate matters: with high statistics the per-sector r errors shrink so far
//  that a sub-percent a−b clears any n-σ test — but a 0.4 mm distortion on a
//  47 mm ring is a circle in every way that counts.  The fallback keeps the
//  legacy single-R contract intact (retro-compat).
RingEllipse fit_ring_ellipse(TH2 *hm, double cx, double cy, double seed_R,
                             double seed_sigma, double n_sigma_band,
                             double n_sigma_circle, double min_ellipticity_frac,
                             RingShapeMode mode)
{
    RingEllipse out;
    out.a = out.b = seed_R;
    if (hm == nullptr || seed_R <= 0.0 || seed_sigma <= 0.0)
        return out;

    const int kPhiBins = 36;
    TProfile prof("ring_rphi", "", kPhiBins, -M_PI, M_PI);
    for (int ix = 1; ix <= hm->GetNbinsX(); ++ix)
        for (int iy = 1; iy <= hm->GetNbinsY(); ++iy)
        {
            const double w = hm->GetBinContent(ix, iy);
            if (w <= 0)
                continue;
            const double x = hm->GetXaxis()->GetBinCenter(ix) - cx;
            const double y = hm->GetYaxis()->GetBinCenter(iy) - cy;
            const double r = std::sqrt(x * x + y * y);
            if (std::fabs(r - seed_R) > n_sigma_band * seed_sigma)
                continue;
            prof.Fill(std::atan2(y, x), r, w);
        }
    if (prof.GetEntries() < 10)
        return out;

    TF1 fell("fell",
             "[0]*[1]/sqrt(pow([1]*cos(x-[2]),2)+pow([0]*sin(x-[2]),2))", -M_PI,
             M_PI);
    fell.SetParameters(seed_R, seed_R, 0.0);
    fell.SetParLimits(0, 0.3 * seed_R, 3.0 * seed_R);
    fell.SetParLimits(1, 0.3 * seed_R, 3.0 * seed_R);
    if (static_cast<int>(prof.Fit(&fell, "RQN")) != 0)
        return out;

    double a = fell.GetParameter(0), b = fell.GetParameter(1);
    double ae = fell.GetParError(0), be = fell.GetParError(1);
    double th = fell.GetParameter(2);
    //  Order a ≥ b so θ is unambiguously the major-axis angle (the model is
    //  symmetric under a↔b, θ→θ+90°).
    if (b > a)
    {
        std::swap(a, b);
        std::swap(ae, be);
        th += M_PI / 2.0;
    }
    double thd = th * 180.0 / M_PI;
    while (thd > 90.0)
        thd -= 180.0;
    while (thd <= -90.0)
        thd += 180.0;

    out.ok = true;
    out.a = a;
    out.b = b;
    out.a_err = ae;
    out.b_err = be;
    out.theta_deg = thd;

    const double s_ab = std::sqrt(ae * ae + be * be);
    const double d_ab = std::fabs(a - b);
    const double r_mean = 0.5 * (a + b);
    switch (mode)
    {
    case RingShapeMode::kForceCircle:
        out.is_ellipse = false;
        break;
    case RingShapeMode::kForceEllipse:
        out.is_ellipse = true; // fit succeeded (out.ok) to reach here
        break;
    case RingShapeMode::kAuto:
    default:
        out.is_ellipse = (s_ab > 0.0 && d_ab > n_sigma_circle * s_ab) &&
                         (d_ab > min_ellipticity_frac * r_mean);
        break;
    }
    if (!out.is_ellipse)
    {
        const double R = 0.5 * (a + b);
        out.a = out.b = R;
        out.theta_deg = 0.0;
    }
    return out;
}

//  Map a hit offset (dx, dy) from the ring centre to the elliptical radius
//  ρ = r · R̄ / r_ell(φ), R̄ = (a+b)/2 — so every point ON the ellipse lands at
//  R̄ regardless of azimuth, collapsing the a−b spread out of the radial
//  distribution.  Returns the plain radius r unchanged for the circle case
//  (is_ellipse == false), so callers can remap unconditionally.
double elliptical_radius(double dx, double dy, const RingEllipse &e)
{
    const double r = std::hypot(dx, dy);
    if (!e.is_ellipse || e.a <= 0.0 || e.b <= 0.0)
        return r;
    const double r_bar = 0.5 * (e.a + e.b);
    const double d = std::atan2(dy, dx) - e.theta_deg * M_PI / 180.0;
    const double r_ell =
        e.a * e.b / std::sqrt(std::pow(e.b * std::cos(d), 2) + std::pow(e.a * std::sin(d), 2));
    return r_ell > 0.0 ? r * r_bar / r_ell : r;
}

//  Draw a projection strip's count axis as 3 manual ticks at ~10/50/90% of the
//  data max, each rounded to 2 significant figures, with faint dashed gridlines.
//  ROOT 6.40 can't rotate axis labels, so for the narrow right strip the labels
//  are hand-drawn rotated 90° (top-to-bottom).  Caller hides the native count
//  labels and sets the count-axis frame max.  `count_is_x` true → Y-projection
//  (count along the bottom/horizontal axis); false → X-projection (count up the
//  left axis).  Drawn in the current pad's user coordinates.
void draw_proj_count_axis(TH1 *h, double data_max, double frame_max,
                          bool count_is_x, bool rotate_labels)
{
    if (data_max <= 0.0)
        return;
    const double binlo = h->GetXaxis()->GetXmin();
    const double binhi = h->GetXaxis()->GetXmax();
    const double span = binhi - binlo;
    auto round2 = [](double v)
    {
        if (v <= 0.0)
            return 0.0;
        const double e = std::pow(10.0, std::floor(std::log10(v)) - 1.0);
        return std::round(v / e) * e;
    };
    TLine grid;
    grid.SetLineColorAlpha(kGray + 1, 0.5);
    grid.SetLineStyle(2);
    TLine tick;
    tick.SetLineColor(kBlack);
    TLatex lab;
    lab.SetTextFont(42);
    lab.SetTextSize(0.085);
    lab.SetTextColor(kBlack);
    for (double frac : {0.1, 0.5, 0.9})
    {
        const double v = round2(frac * data_max);
        if (v <= 0.0 || v > frame_max)
            continue;
        if (count_is_x)
        {
            //  Y-projection: count along x (bottom); bins along y.  Labels
            //  rotated 270° (reading top-to-bottom; 90° read bottom-to-top, i.e.
            //  Y-flipped).
            grid.DrawLine(v, binlo, v, binhi);
            tick.DrawLine(v, binlo, v, binlo + 0.05 * span);
            lab.SetTextAngle(rotate_labels ? 270.0 : 0.0);
            lab.SetTextAlign(rotate_labels ? 12 : 23);
            lab.DrawLatex(v, binlo - 0.03 * span, TString::Format("%g", v));
        }
        else
        {
            //  X-projection: count up the left (y); bins along x.
            grid.DrawLine(binlo, v, binhi, v);
            tick.DrawLine(binlo, v, binlo + 0.05 * span, v);
            lab.SetTextAngle(0.0);
            lab.SetTextAlign(32);
            lab.DrawLatex(binlo - 0.015 * span, v, TString::Format("%g", v));
        }
    }
}

//  Standard "2D map + X/Y projections" panel — shared by ring_centre_xy,
//  ring_ab and ring_theta_ellipticity so the three look and behave identically.
//  3-pad layout; coherent filled-bar projections with 10/50/90% rounded ticks +
//  grid (Y rotated top-to-bottom); an opaque, SHADOWLESS white stat box in the
//  free top-right corner.  `add_diagonal` overlays the a=b reference line
//  (ring_ab only).  Saves <tag>.pdf under the recodata stage at index `idx`.
//  Callers own the domain logic (centre fit / σ fits / θ remap); this owns ONLY
//  the rendering.
void draw_map_with_projections(TH2 *h2, const std::vector<TString> &stat_lines,
                               bool add_diagonal, const char *canvas_name,
                               const std::string &run_dir, int idx,
                               const char *tag)
{
    std::unique_ptr<TH1D> px(h2->ProjectionX((std::string(tag) + "_px").c_str()));
    std::unique_ptr<TH1D> py(h2->ProjectionY((std::string(tag) + "_py").c_str()));
    px->SetDirectory(nullptr);
    py->SetDirectory(nullptr);
    px->SetTitle("");
    py->SetTitle("");
    for (TH1D *p : {px.get(), py.get()})
    {
        p->SetFillColor(kAzure - 9);
        p->SetLineColor(kAzure + 2);
        p->SetLineWidth(2);
        p->SetBarWidth(1.0);
        p->SetBarOffset(0.0);
    }
    const double pxmax = px->GetMaximum(), pymax = py->GetMaximum();
    px->SetMinimum(0.0);
    px->SetMaximum(pxmax * 1.10);
    py->SetMinimum(0.0);
    py->SetMaximum(pymax * 1.10);

    TCanvas c(canvas_name, "", 1000, 1000);
    const double kYW = 0.18, kXH = 0.18, kL = 0.11, kB = 0.10;
    TPad p2d("p2d_map", "", 0.00, 0.00, 1.00 - kYW, 1.00 - kXH);
    TPad pxp("pxproj_map", "", 0.00, 1.00 - kXH, 1.00 - kYW, 1.00);
    TPad pyp("pyproj_map", "", 1.00 - kYW, 0.00, 1.00, 1.00 - kXH);
    p2d.SetLeftMargin(kL);
    p2d.SetRightMargin(0.0);
    p2d.SetBottomMargin(kB);
    p2d.SetTopMargin(0.0);
    pxp.SetLeftMargin(kL);
    pxp.SetRightMargin(0.0);
    pxp.SetBottomMargin(0.0);
    pxp.SetTopMargin(0.04);
    pyp.SetLeftMargin(0.0);
    pyp.SetRightMargin(0.04);
    pyp.SetBottomMargin(kB);
    pyp.SetTopMargin(0.0);
    p2d.Draw();
    pxp.Draw();
    pyp.Draw();
    p2d.cd();
    h2->SetStats(0);
    h2->Draw("col");
    //  a = b reference line (the circle locus; ring_ab only).
    TLine diag(h2->GetXaxis()->GetXmin(), h2->GetXaxis()->GetXmin(),
               h2->GetXaxis()->GetXmax(), h2->GetXaxis()->GetXmax());
    if (add_diagonal)
    {
        diag.SetLineColor(kGray + 2);
        diag.SetLineStyle(2);
        diag.Draw();
    }
    for (TH1D *p : {px.get(), py.get()})
    {
        p->GetXaxis()->SetLabelSize(0.0);
        p->GetXaxis()->SetTickLength(0.0);
        p->GetYaxis()->SetLabelSize(0.0);
        p->GetYaxis()->SetTickLength(0.0);
        p->GetYaxis()->SetNdivisions(0);
    }
    pxp.cd();
    px->Draw("bar");
    draw_proj_count_axis(px.get(), pxmax, pxmax * 1.10, /*count_is_x=*/false,
                         /*rotate=*/false);
    pyp.cd();
    py->Draw("hbar");
    draw_proj_count_axis(py.get(), pymax, pymax * 1.10, /*count_is_x=*/true,
                         /*rotate=*/true);
    c.cd();
    TPaveText stat(0.82, 0.82, 0.999, 0.999, "NDC");
    stat.SetFillColor(kWhite);
    stat.SetFillStyle(1001);
    stat.SetBorderSize(0); // flat — boxes never carry a drop shadow
    stat.SetTextColor(kBlack);
    stat.SetTextFont(42);
    stat.SetTextSize(0.016);
    stat.SetTextAlign(12);
    for (const auto &s : stat_lines)
        stat.AddText(s);
    stat.Draw();
    const auto path = util::qa::pdf_path(run_dir, "recodata", idx, tag);
    c.SaveAs(path.string().c_str());
    util::qa::crop_pdf_inplace(path);
}

} // namespace

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill,
    bool force_rebuild,
    bool force_upstream,
    std::string mapping_conf,
    std::string trigger_conf,
    std::string framer_conf,
    std::string streaming_conf,
    std::string ring_shape_mode)
{
    //  Ring-shape policy for the radial coordinate (auto / circle / ellipse) —
    //  see --force-ring / --force-ellipse.  Parsed once; consumed by the
    //  aggregate ellipse fit below.
    const RingShapeMode shape_mode =
        ring_shape_mode == "circle"    ? RingShapeMode::kForceCircle
        : ring_shape_mode == "ellipse" ? RingShapeMode::kForceEllipse
                                       : RingShapeMode::kAuto;

    //  Clean PDF rendering — no stats box on any saved canvas.
    //  Applies globally for the rest of this process; affects both
    //  the radial fit canvases and the σ-vs-N canvases.
    gStyle->SetOptStat(0);

    //  ROOT thread-safety: required for the within-spill
    //  multithreading  Without this call, ROOT's
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
    //  if missing/corrupt or if force_upstream is set
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
        // + streaming-RANSAC trigger and need to follow the same QA-mode
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
    //  C9.1: AlcorSpilldata owned by unique_ptr — was raw `new` with
    //  no matching delete (leaked on every recodata run).  The arrow
    //  syntax at every consumer site is unchanged; only the declaration
    //  differs.  Stack-allocated would be cleaner still but the type
    //  has no default constructor that initialises the link target
    //  without `link_to_tree` being called after construction.
    auto spilldata = std::make_unique<AlcorSpilldata>();
    spilldata->link_to_tree(lightdata_tree);

    //  Calibration file: TOML v3 only — ``fine_calib.toml`` in the
    //  run dir, produced by ``pulser_calib_writer``.  The legacy
    //  ``fine_calib.txt`` path has been retired (task #172);
    //  ``read_calib_from_file`` hard-errors on non-`.toml` inputs.
    //
    //  Sweep audit: wrap in try/catch with fallback —
    //  same correctness fix applied to the lightdata caller.  C4.4
    //  promoted schema-mismatch + zero-entry from warning to
    //  std::runtime_error; an uncaught throw here aborts the recodata
    //  pipeline mid-run.  Fall back to "no calibration" (get_phase →
    //  0 per channel); the error itself is still surfaced loudly.
    {
        namespace fs = std::filesystem;
        const fs::path run_dir = fs::path(data_repository) / run_name;
        const fs::path toml_path = run_dir / "fine_calib.toml";
        try
        {
            AlcorFinedata::read_calib_from_file(toml_path.string());
        }
        catch (const std::exception &e)
        {
            mist::logger::error(
                "(recodata_writer) read_calib_from_file('" +
                toml_path.string() + "') failed: " +
                std::string(e.what()) +
                " — proceeding with phase = 0 for every channel; "
                "re-generate fine_calib.toml or fix the file then "
                "re-run.  Coarse-domain plots will be correct; "
                "only the sub-cc fine residual is lost.");
        }
    }

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
    //   No worker threads have spawned yet.
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

    // Iterate (device, chip, channel) directly via the GlobalIndex
    // overload and key the cache by `4 * channel_ordinal` — matches the
    // position-cache convention in Mapping.cxx and the MIST
    // HoughTransform `lut_key`.
    std::map<int, std::array<float, 2>> index_to_hit_xy;

    //  Average detector readiness for the run = active channels / full
    //  detector channels.  Set once the spill-weighted channel_weights
    //  are built (below) and printed on the XY coverage map.  Uses the
    //  CHANNEL-COUNT ratio, not the per-bin coverage mean: a whole
    //  missing readout (e.g. rdo-193, absent by default → 1/8 of the
    //  detector) must drag this down, which a mean over only the
    //  populated bins would silently ignore.
    double detector_readiness = 0.0;
    //  Per-hardware-trigger N_γ (photons / triggered frame); filled in the QA
    //  finalize block, published to AnalysisResults below.
    std::map<std::string, double> per_trigger_ngamma;
    {
        const int max_chip = ::gidx::kUsesSplitInTwo ? 4 : 8;
        constexpr int kChannelHi = 64;
        for (int device = ::gidx::kFirstDevice; device < ::gidx::kDeviceUpperBound; ++device)
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

    //  Edge rejection window: 25 ns fixed.  `current_trigger.fine_time`
    //  is in **ns** across the codebase, so the comparison below is
    //  ns-vs-ns.  `frame_size` from
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
        "Trigger selection QA;trigger;", // no Y title — the bin labels (accepted / edge-rejected / duplicate-rejected) carry it
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

    //  Y-axis upper edge is in NANOSECONDS — the fill below uses
    //  current_trigger.fine_time (TriggerEvent::fine_time, documented as
    //  ns in include/triggers/events.h:101).  Without the cc→ns multiply
    //  the cap was framer_cfg.frame_size cc ≈ 1024 ns, and ~68 % of every
    //  edge-rejected trigger landed in Y-overflow — silently defeating the
    //  shifter QA whose whole point is to validate the 25 ns edge cut.
    //  Same constant as the sibling line ~929 in this file.
    RootHist<TH2F> h_edge_trigger_position(
        "h_edge_trigger_position",
        "Position of edge-rejected triggers;trigger;fine time (ns)",
        n_triggers, 0, n_triggers,
        500, 0, framer_cfg.frame_size * BTANA_ALCOR_CC_TO_NS);
    for (int i = 0; i < n_triggers; ++i)
        h_edge_trigger_position->GetXaxis()->SetBinLabel(i + 1, registry.triggers[i].second.c_str());

    std::unordered_map<int, RootHist<TH1F>> h_trigger_time_diff_w_cherenkov;
    //  Trigger-to-Cherenkov-hit Δt range: ±1 rollover, in ns.  The
    //  previous ±500 ns was too narrow — it cut off any hit whose
    //  rollover differed from the trigger's, so the long-Δt
    //  population (DCR / out-of-spill / wrong-rollover-mapping)
    //  silently overflowed.  One rollover (32768 cc × 3.125 ns/cc =
    //  102400 ns) is the natural ceiling: anything beyond would imply
    //  a multi-rollover ambiguity that the unwrapping logic should
    //  have caught.  Bin width ≈ 41 ns (13 cc) — coarse enough for
    //  the long-tail view, fine enough to resolve the trigger peak.
    //  Constants come from alcor_data.h so any rollover-width revision
    //  there propagates here automatically.
    constexpr double kTriggerDtHalfRangeNs =
        BTANA_ALCOR_ROLLOVER_TO_CC * BTANA_ALCOR_CC_TO_NS;
    constexpr int kTriggerDtBins = 5000;

    // ─────────────────────────────────────────────────────────────────────
    //  Live-QA radiator pipeline
    // ─────────────────────────────────────────────────────────────────────
    //  Per-ring photon counting + radial distributions in recodata, so
    //  beam-test operators see Cherenkov physics observables live (the
    //  offline `photon_number_new.cpp` macro becomes a thin plotter).
    //
    //  Pipeline:
    //    init   : coverage map from `index_to_hit_xy` (geometry-only).
    //    frame  : for each frame with a RANSAC ring trigger, run
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
    auto recodata_cfg = recodata_conf_reader(streaming_conf, mapping_conf);
    //  Reuse the lightdata-side streaming/RANSAC QA knob for the centre
    //  (c_x, c_y) QA half-range so both writers stay in lock-step.  We
    //  only read the one field we need; the rest of streaming_ransac_cfg
    //  is consumed upstream by lightdata_writer on the cascade path.
    auto streaming_ransac_cfg = streaming_ransac_conf_reader(streaming_conf);

    //  Captured-once state for the per-frame, per-ring compute
    //  helpers (`compute_ring_fit_timewindow`, `fill_ring_hists`).
    //  Geometry + config knobs that don't change during the loop.  See
    //  `include/writers/recodata/ring_compute.h`.
    const RingComputeContext ring_ctx{index_to_hit_xy, recodata_cfg};

    //  Captured-once state for the parallel per-frame compute pass
    //  (`process_frame_pure`).  Lives in `frame_pipeline.{h,cxx}`.
    //  All members `const &` — safe to share across worker threads.
    const FrameProcessContext frame_proc_ctx{framer_cfg, registry, ring_ctx,
                                             BTANA_EDGE_REJECTION_NS};

    //  Coverage map is now built at FINALIZE
    //  During the spill loop we
    //  accumulate two pieces of information per spill:
    //
    //    * `n_physics_per_spill[i]` — number of RANSAC_RING_FOUND
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
    //  Used by the Crystal-Ball + pol3 fit at finalize
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

    //  Wide-arc mode fills the radial hists with per-ring 1/f_coverage
    //  weights (see fill_ring_hists); enable Sumw2 so the chi²-based radial
    //  fit downstream sees correct weighted bin errors.  For unit weights
    //  Sumw2 reproduces the default sqrt(N) errors, so this is a no-op in the
    //  legacy path — guarded purely to keep the legacy hists untouched.
    if (recodata_cfg.radial_eff_per_ring_centre)
        for (TH1F *hp : {h_radial_first.get(), h_radial_second.get(),
                         h_radial_first_dual.get(), h_radial_first_solo.get(),
                         h_radial_first_smeared.get(), h_radial_second_smeared.get(),
                         h_radial_first_dual_smeared.get(),
                         h_radial_first_solo_smeared.get()})
            hp->Sumw2();

    //  Headline physics observables  These four
    //  per-ring quantities are what beam-test operators care about:
    //    * fitted ring radius (gives Cherenkov angle / velocity / PID),
    //    * single-photon spatial resolution σ_r = std(|r_hit − R_fit|)
    //      per ring (run-level σ_single = mean of this hist's core),
    //    * (R, n_hits) correlation — bad fits surface as off-diagonal
    //      junk,
    //    * (cx, cy) per ring — beam centre + drift over the run.
    //
    //  Same R binning as h_radial_* so the two can be overlaid in
    //  TBrowser.  cx/cy half-range comes from streaming_ransac_cfg
    //  (`centre_xy_half_range_mm`), shared with the lightdata writer.
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

    //  cx / cy half-range: read from streaming_ransac_cfg so the
    //  lightdata- and recodata-side centre-XY hists keep the same axis.
    //  Bin width 0.25 mm: the ring-centre cluster core is ~1 mm wide, so a
    //  1 mm bin quantized the mode estimate too coarsely; 0.25 mm lets the
    //  Gaussian-core fit (below) resolve the centroid to sub-bin precision.
    const float kCentreXyHalfRangeMm = streaming_ransac_cfg.centre_xy_half_range_mm;
    const int kCentreXyBins = static_cast<int>(std::round(8.f * kCentreXyHalfRangeMm));
    RootHist<TH2F> h_centre_xy_first("h_centre_xy_first", ";c_{x} (mm);c_{y} (mm)",
                                     kCentreXyBins, -kCentreXyHalfRangeMm, kCentreXyHalfRangeMm,
                                     kCentreXyBins, -kCentreXyHalfRangeMm, kCentreXyHalfRangeMm);
    RootHist<TH2F> h_centre_xy_second("h_centre_xy_second", ";c_{x} (mm);c_{y} (mm)",
                                      kCentreXyBins, -kCentreXyHalfRangeMm, kCentreXyHalfRangeMm,
                                      kCentreXyBins, -kCentreXyHalfRangeMm, kCentreXyHalfRangeMm);

    //  In-cut trigger-Cherenkov hitmap: the (x, y) of every cherenkov hit
    //  selected by the hardware-trigger TIMING CUT from the config
    //  ([recodata] hardware_ring_dt_min_ns … hardware_ring_dt_max_ns around
    //  the trigger ref time) — the same in-time hits the ring fit runs on.
    //  Full detector range (±99 mm, 0.5 mm bins) so the Cherenkov ring shows.
    RootHist<TH2F> h_trigger_cherenkov_hitmap(
        "h_trigger_cherenkov_hitmap", ";x (mm);y (mm)",
        396, -99, 99, 396, -99, 99);

    //  Companion map: ONLY the hits the ring finder tagged as ring members
    //  (HitmaskRansacRingTag First/Second), i.e. the subset of the in-time
    //  occupancy above that was actually assigned to a reconstructed ring.
    //  Same geometry so the two can be compared directly (full occupancy vs
    //  ring-tagged).
    RootHist<TH2F> h_ring_tagged_hitmap(
        "h_ring_tagged_hitmap", ";x (mm);y (mm)",
        396, -99, 99, 396, -99, 99);

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
    // for the rationale (replaces the biased
    //  `h_sigma_*` and the wrong-observable `h_fit_sigma_R_vs_n_*`
    //  that this hist supersedes).
    RootHist<TH2F> h_residual_vs_n_first("h_residual_vs_n_first", ";N hits;r_{hit} - R_{-i} (mm)",
                                         kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);
    RootHist<TH2F> h_residual_vs_n_second("h_residual_vs_n_second", ";N hits;r_{hit} - R_{-i} (mm)",
                                          kNHitsBins, kNHitsXLo, kNHitsXHi, 100, -5.f, 5.f);

    //  Dual/solo splits for the first ring's vs_n observables.  Same
    //  semantics as the lightdata-side `_dual` / `_solo` splits already
    //  in StreamingRansacQA: filled per the (frame_has_second_ring)
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
    //  translation unit (Phase D of the recodata modularization).  See
    //  `include/writers/recodata/ring_compute.h`:
    //   - `compute_ring_fit_timewindow(t_ref, dt_min, dt_max, lightdata,
    //     do_loo, ring_ctx)` — pure compute, returns a `RingFitResult`;
    //     safe on worker threads.  Selects non-afterpulse cherenkov hits
    //     within [dt_min, dt_max] of the hardware-trigger ref time; initial
    //     guess is hit centroid + median radius.  `do_loo = false` skips
    //     the per-hit LOO loop.
    //   - `fill_ring_hists(result, bundle)` — drain, mutates the
    //     histograms in `bundle` (any pointer may be nullptr to skip).
    //  Both take `ring_ctx` (declared above) for the geometry + config
    //  bundle that used to be captured by reference.

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
    //  Per-channel mean fine-time offset, averaged over per-spill
    //  contributions that pass the |value| < 30 outlier filter.
    //  Both `values_list` and `offset_participants` must independently
    //  clear 20 samples before the offset is committed — the inner
    //  check guards the division below from a 0 denominator (all
    //  samples rejected as outliers) and a low-statistics tail.
    constexpr float kFineOffsetOutlierCutNs = 30.f;
    constexpr int kFineOffsetMinSamples = 20;
    for (auto &[channel_index, values_list] : map_of_offsets)
    {
        if (static_cast<int>(values_list.size()) < kFineOffsetMinSamples)
            continue;
        float offset_sum = 0.f;
        int offset_participants = 0;
        for (const auto &value : values_list)
        {
            if (std::fabs(value) > kFineOffsetOutlierCutNs)
                continue;
            offset_sum += value;
            ++offset_participants;
        }
        if (offset_participants < kFineOffsetMinSamples)
            continue;
        const float offset_value = offset_sum / static_cast<float>(offset_participants);
        AlcorFinedata::set_param2(channel_index, -offset_value / BTANA_ALCOR_CC_TO_NS);
    }
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
            if (device < ::gidx::kTimingDeviceLo)
                for (auto current_lane : lanes)
                    for (auto i_channel = 0; i_channel < 8; ++i_channel)
                    {
                        // Construct the synthetic dead-lane Hit with a
                        // GlobalIndex; split-in-two trick is applied here
                        // at the construction boundary.  The position-cache
                        // lookup uses `4 * channel_ordinal` (the same
                        // dense-int key the position cache was populated
                        // with — see the top-of-function loop and
                        // `Mapping.cxx`).
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
                        //  spill-weighted coverage map
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
        //  process_frame_pure (Stage 1A): pure-compute per-frame producer.
        //  Reads `lightdata`, produces a `FrameResult`.  No histogram
        //  fills, no `recodata.add_*`, no tree Fill, no per-spill
        //  counter mutation.  Thread-safe (only reads from shared
        //  refs in `FrameProcessContext`; writes only to its local
        //  FrameResult).  Body lifted to
        //  `src/writers/recodata/frame_pipeline.cxx` in Phase G2 —
        //  the function signature now makes the
        //  parallel contract visible at the call site.
        //  ───────────────────────────────────────────────────────────────────

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
                            kTriggerDtBins,
                            -kTriggerDtHalfRangeNs, +kTriggerDtHalfRangeNs);
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

            //  In-cut trigger-Cherenkov hitmap: accumulate the (x, y) of every
            //  cherenkov hit that passed the hardware-trigger timing cut
            //  ([recodata] hardware_ring_dt_min_ns … hardware_ring_dt_max_ns),
            //  regardless of whether the ring finder tagged/fitted a ring — so
            //  the map shows the full in-time occupancy, not just ring-found
            //  frames.  `res.occupancy_xy` is gathered in process_frame_pure
            //  independently of the (tagged) ring reconstruction.
            for (const auto &p : res.occupancy_xy)
                h_trigger_cherenkov_hitmap->Fill(p[0], p[1]);

            //  Companion: the ring-finder-tagged hits only (first + second ring).
            //  res.{first,second}.hit_xy carry the tagged ring members (smeared),
            //  recorded even when the fit itself did not converge.
            for (const auto &p : res.first.hit_xy)
                h_ring_tagged_hitmap->Fill(p[0], p[1]);
            for (const auto &p : res.second.hit_xy)
                h_ring_tagged_hitmap->Fill(p[0], p[1]);

            //  Radiator QA — drain the precomputed RingFitResults.  Now
            //  gated on a successful reconstruction (hardware-trigger
            //  time-window fit) rather than the RANSAC self-trigger.
            if (res.first.fit_ok || res.second.fit_ok)
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
                fill_ring_hists(res.first, first_hists,
                                recodata_cfg.radial_eff_per_ring_centre);

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
                fill_ring_hists(res.second, second_hists,
                                recodata_cfg.radial_eff_per_ring_centre);
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
                frame_results[iframe] = process_frame_pure(cur, frame_proc_ctx);
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
                        frame_results[my] = process_frame_pure(cur, frame_proc_ctx);
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
    //  --- Rings QA
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
    //  the radial hists' R values come from PER-EVENT RANSAC/fit
    //  centres.  Discrepancy < 1 % at observed centre wander.
    //
    //  Headline N_γ / single-photon σ for the cross-run AnalysisResults
    //  publish below.  Captured from the primary-ring radial fit INSIDE
    //  the Rings block; its locals don't survive to the publish, so we
    //  hoist the scalars here.
    double pub_n_gamma = 0.0, pub_sigma = 0.0, pub_sigma_err = 0.0;
    bool pub_have_ring = false;
    {
        // Subfolder name: "Rings/" — contents are per-ring observables
        // from the RANSAC output, not per-radiator-material splits.  The
        // ring↔radiator mapping (when wired in via `RadiatorInfoStruct`)
        // is a follow-up to V1; "Deferred items".

        //  ── Spill-by-spill active-channel weighting
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
            //  Detector readiness = the SPILL-WEIGHTED average fraction
            //  of active channels.  Each channel_weight already sums the
            //  physics-weighted fraction of spills that channel was
            //  active (∈ [0, 1]; a channel live every spill → ~1, one
            //  live half the spills → ~0.5, a never-seen channel like a
            //  missing rdo → 0 / absent).  Summing those weights and
            //  dividing by the FULL configured channel count
            //  (index_to_hit_xy, from the mapping geometry) therefore
            //  combines BOTH effects the operator asked for: the
            //  whole-readout gap (rdo-193 always 0) AND the per-spill
            //  channel drift (others come and go), averaged over the run.
            if (!index_to_hit_xy.empty())
            {
                double weight_sum = 0.0;
                for (const auto &[ch, w] : channel_weights)
                    weight_sum += w;
                detector_readiness =
                    weight_sum / static_cast<double>(index_to_hit_xy.size());
            }
        }
        else
        {
            mist::logger::warning(
                "(recodata_writer) no RANSAC_RING_FOUND triggers in this "
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

        //  Cartesian (x, y) coverage map — the operator-facing view
        //  (the rφ map stays for eff(R)).  Same channel footprints +
        //  spill-activity weights, just rasterised in (c_x, c_y).  Range
        //  mirrors the lightdata hitmaps (±99 mm, 396 bins).
        std::unique_ptr<TH2F> h_coverage_map_xy(
            util::radiator_efficiency::build_coverage_map_xy(
                index_to_hit_xy,
                396, -99.f, 99.f,
                396, -99.f, 99.f,
                recodata_cfg.channel_half_width_mm,
                channel_weights.empty() ? nullptr : &channel_weights));
        h_coverage_map_xy->SetName("h_coverage_map_xy");
        h_coverage_map_xy->SetTitle(";c_{x} (mm);c_{y} (mm)");
        //  Average detector readiness (the active/full-detector channel
        //  ratio computed above) is printed on the XY-map PDF emitted in
        //  the save block below.

        TDirectory *rings_dir = output_file->mkdir("Rings");
        rings_dir->cd();

        h_coverage_map_rphi->Write();
        h_coverage_map_xy->Write();

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

            if (!recodata_cfg.radial_eff_per_ring_centre)
            {
                //  Legacy fixed-nominal-centre correction: divide the
                //  aggregate radial hists by a single eff(R).
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
            }
            else
            {
                //  Wide-arc mode: the radial hists are ALREADY
                //  coverage-corrected per-ring at each ring's fitted centre
                //  (1/f_coverage per-hit weighting in fill_ring_hists).  A
                //  single fixed-centre eff(R) is meaningless for far-off
                //  arcs, so it is written below as a DIAGNOSTIC only — no
                //  divide (dividing would double-correct).
                mist::logger::info(
                    "(recodata_writer) wide-arc mode: radial hists "
                    "coverage-corrected per-ring at the fitted centre; "
                    "h_eff_R written as a diagnostic only (no divide).");
            }

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
        //     hist  Ported from
        //     `photon_number_new.cpp`'s
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
        //  by making it a parameter).
        //
        //  In-function lambda was lifted to a free function
        //  to reduce `recodata_writer.cxx` from 2.2k lines (DISCUSSION
        //  modularisation pass).  See `writers/recodata/radial_fit.h`
        //  for the signature; behavior is identical, verified against
        //  a baseline snapshot at extraction time (since pruned).

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

        //  Capture the primary-ring (first, un-smeared) fit for the
        //  cross-run publish — N_γ + single-photon σ (radial peak width).
        for (const auto &rr : radial_results)
            if (rr.name == "h_radial_first")
            {
                pub_n_gamma = rr.n_gamma;
                pub_sigma = rr.peak_sigma;
                pub_sigma_err = rr.peak_sigma_err;
                pub_have_ring = (rr.n_gamma > 0.0);
                break;
            }

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
        h_trigger_cherenkov_hitmap->Write();
        h_ring_tagged_hitmap->Write();

        //  vs_n fitting recipe
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

        //  In-function lambda was lifted to a free function
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

    //  ---
    //  --- Config — self-describing parameter dump.
    //
    //  Routed through util::ConfigDump for uniformity with the other
    //  writers (lightdata / pulser_calib / recotrack) so the QA
    //  dashboard's DataInspectPane reads them all the same way.
    //  Previously recodata.root carried NO Config/ at all, which is
    //  what the "doesn't carry a Config/ tree" message in the GUI
    //  came from.
    {
        util::ConfigDump dump(output_file.get());
        //  Runtime CLI flags.
        dump.add("max_spill", max_spill)
            .add("force_rebuild", force_rebuild)
            .add("force_upstream", force_upstream);
        //  Operationally interesting resolved values — the verbatim
        //  TOML snapshots below carry the full story; these surface the
        //  knobs people actually scan for in the dashboard.
        dump.add("frame_size", framer_cfg.frame_size)
            .add("frame_length_ns", framer_cfg.frame_length_ns())
            .add("n_phi_bins_coverage", recodata_cfg.n_phi_bins_coverage)
            .add("n_r_bins_coverage", recodata_cfg.n_r_bins_coverage)
            .add("r_min_coverage_mm", recodata_cfg.r_min_coverage_mm)
            .add("r_max_coverage_mm", recodata_cfg.r_max_coverage_mm)
            .add("channel_half_width_mm", recodata_cfg.channel_half_width_mm)
            .add("nominal_centre_x_mm", recodata_cfg.nominal_centre_x_mm)
            .add("nominal_centre_y_mm", recodata_cfg.nominal_centre_y_mm);
        //  Conf-file paths + verbatim TOML bodies for every conf the
        //  writer reads (mapping — now also carries the [coverage]
        //  geometry, trigger, framer) plus the streaming conf (which
        //  carries the [streaming_ransac] ring-reco knobs and is also
        //  forwarded to the lightdata cascade on --force-upstream so the
        //  cascade is reproducible from this file alone).
        dump.add_conf_file("mapping_conf", mapping_conf)
            .add_conf_file("trigger_conf", trigger_conf)
            .add_conf_file("framer_conf", framer_conf)
            .add_conf_file("streaming_conf", streaming_conf);
    }

    //  ---
    //  --- QA PDF emission (qa/recodata/*.pdf) — mirrors the
    //  --- lightdata_writer pattern: square 1000×1000 TCanvas, draw
    //  --- via class-appropriate option, save through util::qa::pdf_path
    //  --- so the layout convention matches across writers, then
    //  --- crop_pdf_inplace to strip ROOT's A4-portrait wrapper.
    //
    //  Curated set landing on the General overview's thematic rows:
    //    01 frames_per_spill     — data-taking health (rate / spill)
    //    02 trigger_qa           — accept / reject / duplicate per trigger
    //    03 coverage_map_rphi    — geometric coverage (r, φ)
    //    04 ring_centre_xy       — Cherenkov ring centroid map
    //    05 N_gamma_per_ring     — per-ring N_γ summary
    //
    //  ``N_gamma_per_ring_summary`` is built inside ``build_radial_summary``
    //  above and deleted after ``Write()`` — we read it back from the
    //  open output_file via ``Get`` rather than refactoring the lambda.
    //  ---
    {
        namespace fs = std::filesystem;
        const fs::path run_dir = fs::path(data_repository) / run_name;

        gStyle->SetOptStat(0);
        gStyle->SetPaintTextFormat(".0f");

        auto save_one = [&run_dir](int order, const std::string &name,
                                   TH1 *h, const char *draw_opt)
        {
            if (!h)
                return;
            TCanvas c(TString::Format("c_qa_recodata_%02d_%s",
                                      order, name.c_str()),
                      "", 1000, 1000);
            //  Wider left/bottom margins so 2D bin-labelled hists
            //  (trigger_qa, frames_per_spill) don't clip their long
            //  trigger / outcome labels.  Other classes also benefit
            //  from a touch more breathing room than the ROOT default.
            c.SetLeftMargin(0.18);
            c.SetBottomMargin(0.18);
            c.SetTopMargin(0.08);
            c.SetRightMargin(0.14);
            h->Draw(draw_opt);
            const auto path = util::qa::pdf_path(run_dir, "recodata", order, name);
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);
        };

        //  01 frames_per_spill — TH2F with bin-labelled Y; "colz text90"
        //     so the per-(spill, outcome) counts are readable.
        h_frames_per_spill->GetYaxis()->SetLabelSize(0.030);
        save_one(1, "frames_per_spill", h_frames_per_spill.get(), "colz text90");

        //  02 trigger_qa — TH2F with X = trigger name, Y = outcome.
        //     Use the same X-axis rotation idiom as the lightdata
        //     trigger_matrix so long trigger names don't crowd.
        h_trigger_qa->GetXaxis()->LabelsOption("v");
        h_trigger_qa->GetXaxis()->SetLabelSize(0.028);
        h_trigger_qa->GetYaxis()->SetLabelSize(0.030);
        save_one(2, "trigger_qa", h_trigger_qa.get(), "colz text90");

        //  03 coverage_map_xy — cartesian coverage, the operator-facing
        //     view (the rφ map stays in the ROOT file for eff(R) but is
        //     no longer the headline).  Bespoke save: draw colz + print
        //     the average detector readiness (active / full-detector
        //     channel ratio) as a banner at the top.
        if (auto *cov_xy = output_file->Get<TH2F>("Rings/h_coverage_map_xy"))
        {
            TCanvas c("c_qa_recodata_03_coverage_map_xy", "", 1000, 1000);
            c.SetLeftMargin(0.14);
            c.SetRightMargin(0.16);
            c.SetTopMargin(0.10);
            cov_xy->Draw("colz");
            TLatex banner;
            banner.SetNDC();
            banner.SetTextFont(42);
            banner.SetTextSize(0.030);
            banner.DrawLatex(
                0.14, 0.93,
                TString::Format(
                    "Average detector readiness: %.1f%%  "
                    "(active / full-detector channels)",
                    100.0 * detector_readiness));
            const auto path =
                util::qa::pdf_path(run_dir, "recodata", 3, "coverage_map_xy");
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);
        }

        //  Global ring centre (mode of the per-ring centre distribution) —
        //  hoisted to function scope so the per-hardware-trigger N_γ block below
        //  can reuse it instead of re-fitting a noisy per-ring centre.
        double best_x = 0.0, best_y = 0.0;
        //  Representative ring R / σ (from the luca_and_finger fit, else the
        //  first hardware trigger) — drawn as overlay circles on the hitmap.
        double ring_R = 0.0, ring_sigma = 0.0;
        //  04 ring_centre_xy — first-ring centre map with NARROW marginal X
        //  (top) and Y (right) projection strips that SHARE the map's axes
        //  (same binning): c_x and c_y are labelled once on the MAP (bottom +
        //  left, the outer side away from each strip), the strips hide their
        //  copies.  No colz palette (the projections convey the density).  The
        //  Y-projection grows rightward (outward).  Small black "best centre"
        //  (peak c_x, c_y) text in the top-left corner.
        if (auto *cxy = output_file->Get<TH2F>("Rings/h_centre_xy_first"))
        {
            std::unique_ptr<TH1D> px(cxy->ProjectionX("h_centre_x_first"));
            std::unique_ptr<TH1D> py(cxy->ProjectionY("h_centre_y_first"));
            px->SetDirectory(nullptr);
            py->SetDirectory(nullptr);
            //  px/py here serve only the centre fit (mode → Gaussian-core mean);
            //  the panel rendering (and its own projections) is drawn by
            //  draw_map_with_projections below.
            //  Centre seed = peak (mode) of each projection; refined below to
            //  the Gaussian-core mean for sub-bin precision (the fine 0.25 mm
            //  bins make the raw mode jittery, so the continuous fit centroid
            //  is the precise estimator and seeds the fit range).
            best_x = px->GetEntries() > 0
                         ? px->GetXaxis()->GetBinCenter(px->GetMaximumBin())
                         : 0.0;
            best_y = py->GetEntries() > 0
                         ? py->GetXaxis()->GetBinCenter(py->GetMaximumBin())
                         : 0.0;
            //  Centre spread σ_x, σ_y (Gaussian core of each projection) — the
            //  ring-to-ring centre movement that propagates into the radial σ
            //  (σ_radial² = σ_intrinsic² + σ_centre², σ_centre = √((σx²+σy²)/2)).
            //  On a converged fit the mean (param 1) replaces the mode bin as
            //  the centre; otherwise the mode stands as a robust fallback.
            double sig_cx = 0.0, sig_cy = 0.0;
            if (px->GetEntries() > 0)
            {
                TF1 gcx("gcx", "gaus");
                gcx.SetRange(best_x - 4.0, best_x + 4.0);
                if (static_cast<int>(px->Fit(&gcx, "RQ0")) == 0)
                {
                    best_x = gcx.GetParameter(1);
                    sig_cx = std::fabs(gcx.GetParameter(2));
                }
            }
            if (py->GetEntries() > 0)
            {
                TF1 gcy("gcy", "gaus");
                gcy.SetRange(best_y - 4.0, best_y + 4.0);
                if (static_cast<int>(py->Fit(&gcy, "RQ0")) == 0)
                {
                    best_y = gcy.GetParameter(1);
                    sig_cy = std::fabs(gcy.GetParameter(2));
                }
            }

            draw_map_with_projections(
                cxy,
                {TString::Format("c_{x} = %.1f mm", best_x),
                 TString::Format("#sigma_{x} = %.2f mm", sig_cx),
                 TString::Format("c_{y} = %.1f mm", best_y),
                 TString::Format("#sigma_{y} = %.2f mm", sig_cy)},
                /*add_diagonal=*/false, "c_qa_recodata_04_ring_centre_xy",
                run_dir, 4, "ring_centre_xy");
        }

        //  ── Aggregate ring shape (ellipse) ──────────────────────────────────
        //  Fit the run's ring shape ONCE here — the per-trigger N_γ radial AND
        //  the hitmap overlay both consume it.  Optics distort the ring into an
        //  ellipse; measuring the per-trigger radial in the elliptical radius ρ
        //  keeps the a−b spread from inflating σ.  A STABLE aggregate ellipse is
        //  used (not per-ring, which would overfit ~15-hit arcs and bias σ low).
        //  Seeded from the ring-tagged hits' mean/RMS radius about the pinned
        //  centre.  Thresholds: ±5σ band; ellipse only when a−b is BOTH >2σ_(a−b)
        //  AND >2% of R (else circle → ρ == r, retro-compat).
        constexpr double kRingBandNSigma = 5.0;
        constexpr double kEllipseNSigma = 2.0;
        constexpr double kEllipseMinFrac = 0.02;
        RingEllipse ell;
        if (auto *rt = output_file->Get<TH2F>("Rings/h_ring_tagged_hitmap"))
        {
            double sw = 0.0, sr = 0.0, sr2 = 0.0;
            for (int ix = 1; ix <= rt->GetNbinsX(); ++ix)
                for (int iy = 1; iy <= rt->GetNbinsY(); ++iy)
                {
                    const double w = rt->GetBinContent(ix, iy);
                    if (w <= 0)
                        continue;
                    const double dx = rt->GetXaxis()->GetBinCenter(ix) - best_x;
                    const double dy = rt->GetYaxis()->GetBinCenter(iy) - best_y;
                    const double r = std::hypot(dx, dy);
                    sw += w;
                    sr += w * r;
                    sr2 += w * r * r;
                }
            if (sw > 0.0)
            {
                const double mean = sr / sw;
                const double rms =
                    std::sqrt(std::max(0.0, sr2 / sw - mean * mean));
                ell = fit_ring_ellipse(rt, best_x, best_y, mean,
                                       rms > 0.0 ? rms : 1.0, kRingBandNSigma,
                                       kEllipseNSigma, kEllipseMinFrac,
                                       shape_mode);
            }
        }

        //  ── Per-hardware-trigger N_γ ─────────────────────────────────────────
        //  Photons per triggered frame, per hardware trigger.  Built entirely
        //  here from lightdata's per-trigger in-window (time-cut) hitmaps, about
        //  the single GLOBAL centre (best_x, best_y) — no per-ring centre fit, no
        //  RANSAC.  Each trigger's radial is the dist-to-global-centre of every
        //  in-window hit; fit_radial_distribution normalises by the trigger's
        //  frame count (frame-population entries) → N_γ / triggered frame.  The
        //  Gaussian peak / pol3 split separates the ring from the in-window DCR.
        //  Coverage-corrected by the active-channel azimuthal coverage of the
        //  global ring (geometry coverage ÷ detector readiness).
        {
            //  Radial efficiency eff(R) — R-DEPENDENT, computed ONCE: it depends
            //  only on the global centre + channel geometry, NOT the trigger, so
            //  it is shared by every trigger's correction below.  In each annulus:
            //  live pixel area on it ÷ the full annulus area (2πR·dR), ×
            //  detector_readiness.  Rendered once as radial_efficiency.
            const double pix_area =
                std::pow(2.0 * recodata_cfg.channel_half_width_mm, 2);
            const double eff_band = recodata_cfg.channel_half_width_mm;
            auto eff = std::make_unique<TH1F>(
                "h_radial_efficiency",
                ";R about global centre (mm);#it{eff}(R) = live area / annulus",
                radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
            eff->SetDirectory(nullptr);
            for (int b = 1; b <= eff->GetNbinsX(); ++b)
            {
                const double Rb = eff->GetBinCenter(b);
                double covered = 0.0;
                for (const auto &[ch, pos] : index_to_hit_xy)
                {
                    const double r_ch = std::hypot(pos[0] - best_x, pos[1] - best_y);
                    if (std::fabs(r_ch - Rb) < eff_band)
                        covered += pix_area;
                }
                const double annulus = 2.0 * M_PI * Rb * (2.0 * eff_band);
                eff->SetBinContent(b, annulus > 0.0
                                          ? (covered / annulus) * detector_readiness
                                          : 0.0);
            }
            {
                TCanvas c("c_qa_recodata_radial_efficiency", "", 1000, 700);
                c.SetLeftMargin(0.12);
                c.SetBottomMargin(0.12);
                eff->SetLineColor(kAzure + 2);
                eff->SetLineWidth(2);
                eff->SetMinimum(0.0);
                eff->Draw("hist");
                const auto path = util::qa::pdf_path(run_dir, "recodata", 5,
                                                     "radial_efficiency");
                c.SaveAs(path.string().c_str());
                util::qa::crop_pdf_inplace(path);
            }
            for (const auto &[tidx, tname] : registry.triggers)
            {
                //  Hardware triggers only — skip the synthetic ring-found markers.
                if (tname.find("RING_FOUND") != std::string::npos ||
                    tname == "UNKNOWN" || tname == "FirstFrames" ||
                    tname == "StartOfSpill")
                    continue;
                auto *wm = input_file->Get<TH2F>(
                    TString::Format("Triggers/%s/h_trigger_window_hitmap_%s",
                                    tname.c_str(), tname.c_str()));
                auto *fp = input_file->Get<TH1F>(
                    TString::Format("Triggers/%s/h_trigger_frame_population_%s",
                                    tname.c_str(), tname.c_str()));
                if (!wm || !fp || wm->GetEntries() < 500 || fp->GetEntries() < 1)
                    continue;

                //  Radial of every in-window hit about the global centre.
                auto radial = std::make_unique<TH1F>(
                    ("h_radial_trigger_" + tname).c_str(),
                    (";R about global centre (mm);hits / triggered frame"),
                    radial_hist_n_bins, radial_lo_mm, radial_hi_mm);
                radial->SetDirectory(nullptr);
                radial->Sumw2();
                for (int bx = 1; bx <= wm->GetNbinsX(); ++bx)
                    for (int by = 1; by <= wm->GetNbinsY(); ++by)
                    {
                        const double c = wm->GetBinContent(bx, by);
                        if (c <= 0.0)
                            continue;
                        const double dx = wm->GetXaxis()->GetBinCenter(bx) - best_x;
                        const double dy = wm->GetYaxis()->GetBinCenter(by) - best_y;
                        //  Elliptical radius ρ (≡ r when the ring is circular):
                        //  removes the optical a−b smearing from the per-trigger
                        //  radial so the fitted σ reflects true resolution.
                        radial->Fill(elliptical_radius(dx, dy, ell), c);
                    }

                //  eff(R) is the shared, trigger-independent curve computed above.
                //  RAW (uncorrected) radial, per frame — kept for the combined plot.
                std::unique_ptr<TH1F> raw(static_cast<TH1F *>(
                    radial->Clone(("ngamma_raw_" + tname).c_str())));
                raw->SetDirectory(nullptr);
                std::vector<RadialFitResult> raw_res;
                fit_radial_distribution(raw.get(), fp, "ngamma_" + tname + "_raw",
                                        recodata_cfg, data_repository, run_name,
                                        raw_res, "frame");
                //  CORRECTED: divide each radial bin by eff(R) (R-dependent),
                //  guarded against near-zero-efficiency bins.
                for (int b = 1; b <= radial->GetNbinsX(); ++b)
                {
                    const double e = eff->GetBinContent(b);
                    if (e > 0.05)
                    {
                        radial->SetBinContent(b, radial->GetBinContent(b) / e);
                        radial->SetBinError(b, radial->GetBinError(b) / e);
                    }
                }
                std::vector<RadialFitResult> one;
                fit_radial_distribution(radial.get(), fp,
                                        "ngamma_" + tname, recodata_cfg,
                                        data_repository, run_name, one, "frame");
                if (one.empty() || one.front().peak_sigma <= 0.0)
                    continue;
                const double n_gamma_trig = one.front().n_gamma;
                per_trigger_ngamma[tname] = n_gamma_trig;
                if (tname == "luca_and_finger" || ring_R <= 0.0)
                {
                    ring_R = one.front().peak_mu;
                    ring_sigma = one.front().peak_sigma;
                }
                const double eff_at_R =
                    eff->GetBinContent(eff->FindBin(one.front().peak_mu));
                mist::logger::info(
                    TString::Format(
                        "(recodata) N_gamma[%s] = %.2f / frame (R-dependent eff(R); "
                        "eff(%.1f mm)=%.3f, readiness %.3f)",
                        tname.c_str(), n_gamma_trig, one.front().peak_mu, eff_at_R,
                        detector_readiness)
                        .Data());
                //  Combined RAW (grey) + CORRECTED (red) on one canvas, lin + log,
                //  focused on the yield + σ rather than the full fit-param box.
                if (!raw_res.empty())
                    for (int logy = 0; logy <= 1; ++logy)
                    {
                        TCanvas cc((std::string("c_ngamma_combo_") + tname +
                                    (logy ? "_l" : ""))
                                       .c_str(),
                                   "", 1000, 700);
                        cc.SetLeftMargin(0.12);
                        cc.SetBottomMargin(0.12);
                        if (logy)
                            cc.SetLogy();
                        raw->SetLineColor(kGray + 2);
                        raw->SetLineWidth(2);
                        raw->SetMarkerColor(kGray + 2);
                        radial->SetLineColor(kRed + 1);
                        radial->SetLineWidth(2);
                        radial->SetMarkerColor(kRed + 1);
                        radial->SetTitle(
                            (tname + ": raw vs eff(R)-corrected;"
                                     "R about global centre (mm);photons / frame / bin")
                                .c_str());
                        radial->SetMaximum(1.3 * std::max(raw->GetMaximum(),
                                                          radial->GetMaximum()));
                        radial->SetMinimum(logy ? 0.3 : 0.0);
                        radial->Draw("hist");
                        raw->Draw("hist same");
                        TLatex l;
                        l.SetNDC();
                        l.SetTextFont(42);
                        l.SetTextSize(0.042);
                        l.SetTextColor(kGray + 2);
                        l.DrawLatex(0.45, 0.85,
                                    TString::Format("raw:  N_{#gamma} = %.1f,  #sigma = %.2f mm",
                                                    raw_res.front().n_gamma,
                                                    raw_res.front().peak_sigma));
                        l.SetTextColor(kRed + 1);
                        l.DrawLatex(0.45, 0.79,
                                    TString::Format("corrected:  N_{#gamma} = %.1f,  #sigma = %.2f mm",
                                                    one.front().n_gamma,
                                                    one.front().peak_sigma));
                        const auto path = util::qa::pdf_path(
                            run_dir, "recodata", 5,
                            std::string("ngamma_combo_") + tname + (logy ? "_logy" : ""));
                        cc.SaveAs(path.string().c_str());
                        util::qa::crop_pdf_inplace(path);
                    }
            }
        }

        //  04 trigger_cherenkov_hitmap — (x, y) of every in-time cherenkov hit →
        //  the Cherenkov ring.  Overlaid: the fitted global centre (marker) and
        //  the fitted ring shape.  Optics can distort the ring into an ellipse,
        //  so the shape is fit as an ellipse (semi-axes a, b, rotation θ) about
        //  the pinned centre; when |a−b| is consistent with 0 it collapses to a
        //  circle (R) — the legacy single-radius overlay.  The solid curve is
        //  the fit, dashed = ±3σ, dotted = ±5σ (σ = radial ring width).
        if (auto *hm = output_file->Get<TH2F>("Rings/h_trigger_cherenkov_hitmap"))
        {
            //  Reuse the aggregate ellipse hoisted above (shared with the
            //  per-trigger radial remap).  Ring-tagged hits feed the diagnostic.
            auto *rt = output_file->Get<TH2F>("Rings/h_ring_tagged_hitmap");

            TCanvas c("c_qa_recodata_04_trigger_cherenkov_hitmap", "", 1000, 1000);
            c.SetRightMargin(0.14);
            hm->Draw("colz");
            //  TEllipse(x, y, r1, r2, phimin, phimax, theta): circle fallback
            //  has r1 == r2 (== R) and theta == 0.  Bands offset both semi-axes
            //  by ±nσ (radial width), inner radii clamped at 0.
            const double th = ell.theta_deg;
            auto mk_ell = [&](double da)
            {
                return TEllipse(best_x, best_y, std::max(0.0, ell.a + da),
                                std::max(0.0, ell.b + da), 0.0, 360.0, th);
            };
            TEllipse e_fit = mk_ell(0.0);
            TEllipse e_3lo = mk_ell(-3 * ring_sigma);
            TEllipse e_3hi = mk_ell(+3 * ring_sigma);
            TEllipse e_5lo = mk_ell(-5 * ring_sigma);
            TEllipse e_5hi = mk_ell(+5 * ring_sigma);
            TMarker mk(best_x, best_y, 5); // '5' = an X marker at the fit centre
            //  Ellipse-axes overlay (ellipse case only): major (a) and minor (b)
            //  semi-axis spokes from the centre, a horizontal +X reference
            //  through the centre, and an arc marking θ (measured +X → major
            //  axis, CCW positive).  Declared here so they outlive SaveAs.
            const double th_rad = ell.theta_deg * M_PI / 180.0;
            TLine axis_a(best_x, best_y, best_x + ell.a * std::cos(th_rad),
                         best_y + ell.a * std::sin(th_rad));
            TLine axis_b(best_x, best_y,
                         best_x + ell.b * std::cos(th_rad + M_PI / 2.0),
                         best_y + ell.b * std::sin(th_rad + M_PI / 2.0));
            TLine xref(best_x, best_y, best_x + 1.10 * ell.a, best_y);
            TArc th_arc(best_x, best_y, 0.42 * ell.a,
                        std::min(0.0, ell.theta_deg),
                        std::max(0.0, ell.theta_deg));
            TLatex axlab;
            //  Info box: black text on an opaque white box (the red-on-colz
            //  caption was unreadable over hot pixels) — params on line 1, the
            //  line-style legend on line 2.
            TPaveText info(0.13, 0.83, 0.66, 0.895, "NDC");
            info.SetFillColor(kWhite);
            info.SetFillStyle(1001);
            info.SetBorderSize(0); // flat — no drop shadow
            info.SetTextColor(kBlack);
            info.SetTextFont(42);
            info.SetTextSize(0.024);
            info.SetTextAlign(12); // left, vertically centred
            info.SetMargin(0.02);
            if (ring_R > 0.0 && ring_sigma > 0.0)
            {
                for (TEllipse *e : {&e_fit, &e_3lo, &e_3hi, &e_5lo, &e_5hi})
                {
                    e->SetFillStyle(0);
                    e->SetLineColor(kRed + 1);
                    e->SetLineWidth(2);
                }
                e_fit.SetLineStyle(1);
                e_3lo.SetLineStyle(2);
                e_3hi.SetLineStyle(2);
                e_5lo.SetLineStyle(3);
                e_5hi.SetLineStyle(3);
                e_fit.Draw();
                e_3lo.Draw();
                e_3hi.Draw();
                e_5lo.Draw();
                e_5hi.Draw();
                mk.SetMarkerColor(kRed + 1);
                mk.SetMarkerSize(2);
                mk.Draw();
                if (ell.is_ellipse)
                {
                    //  Light-blue spokes so the axes read as distinct from the
                    //  red ellipse geometry.
                    const int kAxisCol = kAzure + 2;
                    axis_a.SetLineColor(kAxisCol);
                    axis_a.SetLineWidth(2);
                    axis_b.SetLineColor(kAxisCol);
                    axis_b.SetLineWidth(2);
                    xref.SetLineColor(kGray + 2);
                    xref.SetLineWidth(1);
                    xref.SetLineStyle(2);
                    th_arc.SetFillStyle(0);
                    th_arc.SetNoEdges();
                    th_arc.SetLineColorAlpha(kGray + 2, 0.5); // 50% transparent
                    th_arc.SetLineWidth(2);
                    axis_a.Draw();
                    axis_b.Draw();
                    xref.Draw();
                    th_arc.Draw();
                    //  Tags follow each spoke's inclination but are OFFSET
                    //  perpendicular to the line (so the spoke never strikes
                    //  through the text), pulled toward the centre into the empty
                    //  ring interior.  One uniform text size for a/b/θ.  Text
                    //  angle wrapped to (−90,90] (square pad → geometric angle ==
                    //  screen angle).
                    //  Value-only tags (the letter is dropped — the axis position
                    //  names it; full a/b/θ live in the info box).  a/b follow
                    //  their spoke inclination, offset to the BELOW side (−n̂) so
                    //  the value reads under the axis without strike-through.  θ
                    //  is anchored to the X-parallel reference like an axis label.
                    auto read_ang = [](double d)
                    {
                        while (d > 90.0)
                            d -= 180.0;
                        while (d <= -90.0)
                            d += 180.0;
                        return d;
                    };
                    const double kPerp = 0.055 * ell.a; // small perpendicular gap
                    axlab.SetTextFont(42);
                    axlab.SetTextSize(0.022);
                    axlab.SetTextAlign(12); // left → writing STARTS at the anchor
                    axlab.SetTextColor(kAxisCol);
                    //  a value: writing starts a little after the centre and reads
                    //  OUTWARD along θ̂, offset just BELOW the spoke (−n̂).
                    const double ra = 0.30 * ell.a;
                    axlab.SetTextAngle(read_ang(ell.theta_deg));
                    axlab.DrawLatex(
                        best_x + ra * std::cos(th_rad) + kPerp * std::sin(th_rad),
                        best_y + ra * std::sin(th_rad) - kPerp * std::cos(th_rad),
                        TString::Format("%.1f mm", ell.a));
                    //  b value: starts after the centre, reads outward along
                    //  (θ+90)̂, offset just ABOVE the spoke (+n̂).
                    const double thb = th_rad + M_PI / 2.0;
                    const double rb = 0.30 * ell.b;
                    axlab.SetTextAngle(read_ang(ell.theta_deg + 90.0));
                    axlab.DrawLatex(
                        best_x + rb * std::cos(thb) - kPerp * std::sin(thb),
                        best_y + rb * std::sin(thb) + kPerp * std::cos(thb),
                        TString::Format("%.1f mm", ell.b));
                    //  θ value: INSIDE the angle arc, centred on the bisector and
                    //  inclined along it (text follows the angle's centre line).
                    const double bis = 0.5 * th_rad;
                    const double r_th = 0.28 * ell.a; // inside the arc (0.42·a)
                    axlab.SetTextAngle(read_ang(0.5 * ell.theta_deg));
                    axlab.SetTextColor(kGray + 2);
                    axlab.DrawLatex(best_x + r_th * std::cos(bis),
                                    best_y + r_th * std::sin(bis),
                                    TString::Format("%.0f#circ", ell.theta_deg));
                }
                const TString shape =
                    ell.is_ellipse
                        ? TString::Format(
                              "fit: a=%.1f, b=%.1f mm, #theta=%.0f#circ, "
                              "#sigma=%.2f mm",
                              ell.a, ell.b, ell.theta_deg, ring_sigma)
                        : TString::Format("fit: R=%.1f, #sigma=%.2f mm", ell.a,
                                          ring_sigma);
                info.AddText(shape);
                info.AddText("solid = fit,  dashed = #pm3#sigma,  "
                             "dotted = #pm5#sigma");
                info.Draw();
            }
            const auto path = util::qa::pdf_path(run_dir, "recodata", 4,
                                                 "trigger_cherenkov_hitmap");
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);

            //  Radial-coordinate cross-check (elliptic rings only): the
            //  circular-r distribution is smeared by the a−b spread.  Re-map
            //  each ring hit to an elliptical radius ρ = r·R̄ / r_ell(φ)
            //  (θ-aware) so a perfect ellipse collapses to a δ at R̄ = (a+b)/2;
            //  the σ drop quantifies how much radial width was ellipticity vs
            //  intrinsic resolution.  Diagnostic only — the production radial
            //  fit is rewired separately once this is validated.
            if (ell.is_ellipse && ring_sigma > 0.0)
            {
                TH2F *src = rt != nullptr ? rt : hm;
                const double Rbar = 0.5 * (ell.a + ell.b);
                TH1F h_circ("h_radial_circ", "", 48, Rbar - 12.0, Rbar + 12.0);
                TH1F h_ellr("h_radial_ell", "", 48, Rbar - 12.0, Rbar + 12.0);
                for (int ix = 1; ix <= src->GetNbinsX(); ++ix)
                    for (int iy = 1; iy <= src->GetNbinsY(); ++iy)
                    {
                        const double w = src->GetBinContent(ix, iy);
                        if (w <= 0)
                            continue;
                        const double dx =
                            src->GetXaxis()->GetBinCenter(ix) - best_x;
                        const double dy =
                            src->GetYaxis()->GetBinCenter(iy) - best_y;
                        const double r = std::sqrt(dx * dx + dy * dy);
                        if (std::fabs(r - ring_R) > 5.0 * ring_sigma)
                            continue;
                        h_circ.Fill(r, w);
                        h_ellr.Fill(elliptical_radius(dx, dy, ell), w);
                    }
                auto fit_sigma = [&](TH1F &h) -> double
                {
                    if (h.GetEntries() < 20)
                        return 0.0;
                    TF1 g("g", "gaus");
                    g.SetRange(Rbar - 3 * ring_sigma, Rbar + 3 * ring_sigma);
                    return static_cast<int>(h.Fit(&g, "RQN")) == 0
                               ? std::fabs(g.GetParameter(2))
                               : 0.0;
                };
                const double s_circ = fit_sigma(h_circ);
                const double s_ell = fit_sigma(h_ellr);
                mist::logger::info(
                    TString::Format(
                        "(recodata_writer) ring shape a=%.2f b=%.2f mm "
                        "theta=%.1f deg  radial sigma circular=%.2f -> "
                        "elliptical=%.2f mm",
                        ell.a, ell.b, ell.theta_deg, s_circ, s_ell)
                        .Data());
                TCanvas cc("c_qa_recodata_04_radial_ellipse_vs_circle", "", 900,
                           700);
                h_circ.SetLineColor(kGray + 2);
                h_circ.SetLineWidth(2);
                h_ellr.SetLineColor(kRed + 1);
                h_ellr.SetLineWidth(2);
                h_circ.SetTitle("ring-hit radial: circular r vs elliptical "
                                "#rho;radius about centre (mm);hits / bin");
                h_circ.SetStats(0);
                h_circ.Draw("hist");
                h_ellr.Draw("hist same");
                TLatex lt;
                lt.SetNDC();
                lt.SetTextSize(0.034);
                lt.SetTextColor(kGray + 2);
                lt.DrawLatex(0.15, 0.86,
                             TString::Format("circular r:  #sigma = %.2f mm",
                                             s_circ));
                lt.SetTextColor(kRed + 1);
                lt.DrawLatex(0.15, 0.81,
                             TString::Format("elliptical #rho:  #sigma = %.2f mm",
                                             s_ell));
                const auto pth = util::qa::pdf_path(
                    run_dir, "recodata", 4, "radial_ellipse_vs_circle");
                cc.SaveAs(pth.string().c_str());
                util::qa::crop_pdf_inplace(pth);
            }
        }

        //  04 ring_tagged_hitmap — companion to the above: only the hits the
        //  ring finder tagged as ring members (vs the full in-time occupancy).
        if (auto *hm = output_file->Get<TH2F>("Rings/h_ring_tagged_hitmap"))
            save_one(4, "ring_tagged_hitmap", hm, "colz");

        //  06 sigma_photon_summary — per-ring single-photon resolution
        //     σ_photon (mm), the other Cherenkov headline the operator
        //     watches alongside N_γ.  Same read-back-from-file pattern.
        if (auto *sg = output_file->Get<TH1F>("Rings/h_sigma_photon_summary"))
            save_one(6, "sigma_photon_summary", sg, "hist text");
    }

    //  ---
    //  --- Publish cross-run scalars to AnalysisResults.
    //
    //  Same store (``<data_repository>/standard_results.toml`` =
    //  ``Data/standard_results.toml``; the .root backend was dropped)
    //  that lightdata + recotrack write to.  Sensor key is "all" —
    //  per-sensor splitting (1350 / 1375) would require redoing the
    //  fits per sensor, which is downstream-macro work; the dashboard
    //  trend reader can already slice on "all".
    {
        //  Sibling of the run directories — i.e. ``<data_repository>/standard_results.toml`` (= ``Data/``)
        //  — so the cross-run aggregate lives next to the per-run data
        //  it summarises.  Earlier ``extData/`` literal was a stale
        //  hard-code from the legacy macro paths and failed to open
        //  whenever the dashboard launched from a different cwd.
        AnalysisResults ar(data_repository + "/standard_results.toml");
        ResultMap rm{
            {{run_name, "all", "recodata.n_spills"},
             {static_cast<double>(all_spills), 0.0}},
            {{run_name, "all", "recodata.frame_size"},
             {static_cast<double>(framer_cfg.frame_size), 0.0}},
            {{run_name, "all", "recodata.nominal_centre_x_mm"},
             {static_cast<double>(recodata_cfg.nominal_centre_x_mm), 0.0}},
            {{run_name, "all", "recodata.nominal_centre_y_mm"},
             {static_cast<double>(recodata_cfg.nominal_centre_y_mm), 0.0}},
        };
        //  Freshly-fitted primary-ring N_γ + single-photon σ — only when a
        //  ring was actually reconstructed this run, so the cross-run
        //  trends reflect THIS run's physics instead of stale leftovers.
        //  Keys match cross_run_trends.DEFAULT_METRICS (full.n_gamma/full.sigma).
        if (pub_have_ring)
        {
            rm[{run_name, "all", "full.n_gamma"}] = {pub_n_gamma, 0.0};
            rm[{run_name, "all", "full.sigma"}] = {pub_sigma, pub_sigma_err};
        }
        //  Per-hardware-trigger N_γ (photons / triggered frame).
        for (const auto &[tname, ng] : per_trigger_ngamma)
            rm[{run_name, "all", "full.n_gamma." + tname}] = {ng, 0.0};
        ar.update(rm, /*source=*/"recodata");
    }
    //
    //  input_file and output_file closed automatically by TFilePtr dtors.
    //  End: QA plots
    //  --- --- --- --- --- ---
}
