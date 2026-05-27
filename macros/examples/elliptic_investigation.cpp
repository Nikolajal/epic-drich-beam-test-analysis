/**
 * @file macros/examples/elliptic_investigation.cpp
 * @brief Test the hypothesis that "ring 2" is genuinely an ellipse
 *        (not a circle, as MIST assumes).
 *
 * ── Hypothesis ──────────────────────────────────────────────────────
 *
 * Ring 1 is a proper Cherenkov circle (e.g., first radiator).  Ring 2
 * is the SECOND physical ring (different radiator / different optical
 * path) but its true shape is an ELLIPSE, not a circle — because of
 * radiator non-uniformity, off-axis beam incidence, or focusing
 * optics asymmetries.  MIST's circular Hough fits a circle to it
 * (the only shape MIST knows about) and the circle parameters are
 * effectively a "best-circle approximation" of the underlying
 * ellipse.
 *
 * ── Investigation strategy ─────────────────────────────────────────
 *
 * Two-layer test:
 *
 *   Layer 1: correlation check (sanity).  For frames with both
 *   rings tagged, re-fit each ring as a circle.  If the centres are
 *   close together AND radii similar, "two rings" might be one
 *   distorted ring (alternative hypothesis).  Hists:
 *     h_R_first_vs_second, h_c[xy]_first_vs_second,
 *     h_dx, h_dy, h_dR, h_d_centre.
 *
 *   Layer 2: ellipse fit on RING 2 ONLY.  For each frame with ring 2
 *   tagged, fit an ellipse `(cx, cy, a, b, θ)` to its hits and
 *   compare χ² against the circle fit on the same hits.  If the
 *   ellipse fit gives:
 *     * Significantly lower χ² (relative improvement > some threshold)
 *     * Eccentricity systematically > 0 (not consistent with 0 within
 *       fit error)
 *   → hypothesis credibly supported.  Otherwise → circle is fine.
 *
 *   Hists for Layer 2:
 *     h_ellipse_semi_major, h_ellipse_semi_minor,
 *     h_ellipse_eccentricity, h_ellipse_position_angle,
 *     h_chi2_circle_vs_ellipse (TH2F),
 *     h_chi2_relative_improvement (TH1F),
 *     h_ellipse_a_vs_b (TH2F).
 *
 * ── Output ─────────────────────────────────────────────────────────
 *
 *   <run>/elliptic_investigation.root
 *
 * ── Reading the plots ──────────────────────────────────────────────
 *
 *  * `h_ellipse_eccentricity` shifted away from 0 → ring 2 is
 *    genuinely elliptic.
 *  * `h_chi2_relative_improvement` ≫ 0 → ellipse fits better than
 *    circle (the 5-param ellipse has more freedom than the 3-param
 *    circle, so SOME improvement is expected from over-fitting on
 *    low-N hits; the relevant magnitude is several × the
 *    over-fitting baseline).
 *  * `h_ellipse_a_vs_b` clusters on the diagonal → a ≈ b → circle.
 *    Off-diagonal → ellipse.
 */

#include "../lib_loader.h"
#include "util/root_io.h"
#include "util/root_hist.h"
#include "util/circle_fit.h"
#include "alcor_data.h" // HitmaskHoughRingTag*, encode_bit

#include <TH1F.h>
#include <TH2F.h>
#include <TFile.h>
#include <TTree.h>
#include <Math/Functor.h>
#include <Fit/Fitter.h>

#include <string>
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>

namespace
{

// ── Inline 5-parameter ellipse fit ─────────────────────────────────
//
// Minimises the sum of squared algebraic distances of each hit from
// the ellipse boundary.  Parameters: (cx, cy, a, b, θ) with a, b > 0
// and θ ∈ (-π/2, π/2].  Algebraic distance per point:
//
//     d = (x'/a)² + (y'/b)² - 1,
//     where (x', y') = R(θ) · (x - cx, y - cy)
//
// Fast (no Jacobians supplied — Migrad does numerical), good enough
// for the small per-event hit counts.  Seeds from the circle fit so
// the LM finds the correct local minimum.
//
// Convention enforced: a ≥ b (swap and rotate θ by π/2 if needed).

struct EllipseFit
{
    double cx = 0, cy = 0;
    double a = 0, b = 0; // a ≥ b enforced
    double theta = 0;    // major-axis angle, rad
    double chi2 = 0;     // Σ d² at minimum
    bool converged = false;
};

EllipseFit fit_ellipse(const std::vector<std::array<float, 2>> &pts,
                       double cx_seed, double cy_seed, double R_seed)
{
    EllipseFit out;
    const int N = static_cast<int>(pts.size());
    if (N < 6)
        return out; // 5 params + at least one residual

    auto cost = [&pts](const double *p) -> double
    {
        const double cx = p[0], cy = p[1];
        const double a = p[2], b = p[3];
        const double th = p[4];
        if (a <= 0. || b <= 0.)
            return 1e30;
        const double ct = std::cos(th), st = std::sin(th);
        double chi2 = 0.;
        for (const auto &q : pts)
        {
            const double dx = q[0] - cx;
            const double dy = q[1] - cy;
            const double xp = dx * ct + dy * st;
            const double yp = -dx * st + dy * ct;
            const double d = (xp / a) * (xp / a) + (yp / b) * (yp / b) - 1.;
            chi2 += d * d;
        }
        return chi2;
    };

    ROOT::Math::Functor functor(cost, 5);
    ROOT::Fit::Fitter fitter;
    fitter.Config().SetFunction(functor);
    fitter.Config().ParSettings(0).Set("cx", cx_seed, 0.5);
    fitter.Config().ParSettings(1).Set("cy", cy_seed, 0.5);
    fitter.Config().ParSettings(2).Set("a", R_seed, 0.5);
    fitter.Config().ParSettings(3).Set("b", R_seed, 0.5);
    fitter.Config().ParSettings(4).Set("theta", 0.0, 0.05);
    fitter.Config().ParSettings(2).SetLowerLimit(1.0);
    fitter.Config().ParSettings(3).SetLowerLimit(1.0);
    fitter.Config().ParSettings(4).SetLimits(-TMath::Pi() / 2., TMath::Pi() / 2.);
    fitter.Config().MinimizerOptions().SetPrintLevel(0);

    if (!fitter.FitFCN())
        return out;
    const auto &res = fitter.Result();
    if (!res.IsValid())
        return out;

    out.cx = res.Parameter(0);
    out.cy = res.Parameter(1);
    out.a = res.Parameter(2);
    out.b = res.Parameter(3);
    out.theta = res.Parameter(4);
    out.chi2 = res.MinFcnValue();
    out.converged = true;

    // Enforce a ≥ b: swap and rotate θ by π/2 if the minimiser
    // landed on the "b is major axis" branch.
    if (out.a < out.b)
    {
        std::swap(out.a, out.b);
        out.theta += TMath::Pi() / 2.;
        if (out.theta > TMath::Pi() / 2.)
            out.theta -= TMath::Pi();
    }
    return out;
}

// χ² of a circle fit, computed in the same algebraic-distance form as
// the ellipse fit so the two are directly comparable:
//
//     d_circle = (r/R)² - 1  where r = distance from (cx, cy)
//
double circle_algebraic_chi2(const std::vector<std::array<float, 2>> &pts,
                             double cx, double cy, double R)
{
    if (R <= 0.)
        return 1e30;
    double chi2 = 0.;
    for (const auto &q : pts)
    {
        const double dx = q[0] - cx;
        const double dy = q[1] - cy;
        const double r2 = dx * dx + dy * dy;
        const double d = r2 / (R * R) - 1.;
        chi2 += d * d;
    }
    return chi2;
}

} // namespace

void elliptic_investigation(std::string data_repository, std::string run_name)
{
    // ── Open recodata ──────────────────────────────────────────────
    const std::string filename = data_repository + "/" + run_name + "/recodata.root";
    TFilePtr input_file(TFile::Open(filename.c_str(), "READ"));
    if (!input_file || input_file->IsZombie())
    {
        mist::logger::error("[elliptic_investigation] Could not open " + filename);
        return;
    }
    TTree *recodata_tree = static_cast<TTree *>(input_file->Get("recodata"));
    if (!recodata_tree)
    {
        mist::logger::error("[elliptic_investigation] 'recodata' tree missing in " + filename);
        return;
    }
    auto *recodata = new AlcorRecodata();
    recodata->link_to_tree(recodata_tree);
    const long n_frames = recodata_tree->GetEntries();
    mist::logger::info("[elliptic_investigation] " + std::to_string(n_frames) + " frames to scan");

    // ── Output ─────────────────────────────────────────────────────
    const std::string outname = data_repository + "/" + run_name + "/elliptic_investigation.root";
    TFilePtr output_file(TFile::Open(outname.c_str(), "RECREATE"));
    if (!output_file || output_file->IsZombie())
    {
        mist::logger::error("[elliptic_investigation] Could not create " + outname);
        delete recodata;
        return;
    }

    // ── Layer-1 hists: correlations (cross-check) ─────────────────
    RootHist<TH2F> h_R_first_vs_second(
        "h_R_first_vs_second", ";R_{first} (mm);R_{second} (mm)",
        100, 25.f, 125.f, 100, 25.f, 125.f);
    RootHist<TH2F> h_cx_first_vs_second(
        "h_cx_first_vs_second", ";c_{x,first} (mm);c_{x,second} (mm)",
        50, -25.f, 25.f, 50, -25.f, 25.f);
    RootHist<TH2F> h_cy_first_vs_second(
        "h_cy_first_vs_second", ";c_{y,first} (mm);c_{y,second} (mm)",
        50, -25.f, 25.f, 50, -25.f, 25.f);
    RootHist<TH1F> h_dx(
        "h_dx_first_minus_second", ";c_{x,first} - c_{x,second} (mm);events",
        100, -25.f, 25.f);
    RootHist<TH1F> h_dy(
        "h_dy_first_minus_second", ";c_{y,first} - c_{y,second} (mm);events",
        100, -25.f, 25.f);
    RootHist<TH1F> h_dR(
        "h_dR_first_minus_second", ";R_{first} - R_{second} (mm);events",
        200, -50.f, 50.f);
    RootHist<TH1F> h_d_centre(
        "h_d_centre_first_minus_second", ";|c_{first} - c_{second}| (mm);events",
        100, 0.f, 50.f);

    // ── Layer-2 hists: ellipse fit on ring 2 ──────────────────────
    RootHist<TH1F> h_a(
        "h_ellipse_semi_major", ";a (mm);events", 200, 25.f, 125.f);
    RootHist<TH1F> h_b(
        "h_ellipse_semi_minor", ";b (mm);events", 200, 25.f, 125.f);
    RootHist<TH1F> h_ecc(
        "h_ellipse_eccentricity",
        ";e = #sqrt{1 - (b/a)^{2}};events", 100, 0.f, 1.f);
    RootHist<TH1F> h_pa(
        "h_ellipse_position_angle",
        ";#theta (rad) [major-axis tilt];events", 100, -TMath::Pi() / 2., TMath::Pi() / 2.);
    RootHist<TH2F> h_a_vs_b(
        "h_ellipse_a_vs_b",
        ";a (mm);b (mm)", 100, 25.f, 125.f, 100, 25.f, 125.f);
    RootHist<TH2F> h_chi2_circle_vs_ellipse(
        "h_chi2_circle_vs_ellipse",
        ";#chi^{2}_{circle};#chi^{2}_{ellipse}",
        100, 0.f, 5.f, 100, 0.f, 5.f);
    RootHist<TH1F> h_chi2_improvement(
        "h_chi2_relative_improvement",
        ";(#chi^{2}_{circle} - #chi^{2}_{ellipse}) / #chi^{2}_{circle};events",
        100, -0.2f, 1.f);

    // ── Frame loop ─────────────────────────────────────────────────
    long n_dual_frames = 0;
    long n_layer1_ok = 0;
    long n_layer2_ok = 0;
    const uint32_t mask_first = encode_bit(HitmaskHoughRingTagFirst);
    const uint32_t mask_second = encode_bit(HitmaskHoughRingTagSecond);
    constexpr int kMinHitsCircle = 5;
    constexpr int kMinHitsEllipse = 8; // 5 params + small safety margin

    for (long iframe = 0; iframe < n_frames; ++iframe)
    {
        recodata_tree->GetEntry(iframe);
        if (recodata->is_start_of_spill())
            continue;

        std::vector<std::array<float, 2>> hits_first, hits_second;
        const int nhits = static_cast<int>(recodata->get_recodata().size());
        for (int i = 0; i < nhits; ++i)
        {
            const float x = recodata->get_hit_x(i);
            const float y = recodata->get_hit_y(i);
            if (recodata->check_hit_mask(i, mask_first))
                hits_first.push_back({x, y});
            if (recodata->check_hit_mask(i, mask_second))
                hits_second.push_back({x, y});
        }

        // Centroid + mean-radial seed for any circle fit.
        auto centroid_seed = [](const std::vector<std::array<float, 2>> &pts)
        {
            float sx = 0, sy = 0;
            for (auto &p : pts)
            {
                sx += p[0];
                sy += p[1];
            }
            const float cx = sx / pts.size(), cy = sy / pts.size();
            float sr = 0;
            for (auto &p : pts)
                sr += std::hypot(p[0] - cx, p[1] - cy);
            return std::array<float, 3>{cx, cy, sr / pts.size()};
        };

        // ── Layer 1: dual-ring correlations ────────────────────────
        const bool have_both =
            (int)hits_first.size() >= kMinHitsCircle &&
            (int)hits_second.size() >= kMinHitsCircle;
        float ring2_cx_circle = 0.f, ring2_cy_circle = 0.f, ring2_R_circle = 0.f;
        bool ring2_circle_ok = false;
        if (have_both)
        {
            ++n_dual_frames;
            const auto fit1 = fit_circle(hits_first, centroid_seed(hits_first), false);
            const auto fit2 = fit_circle(hits_second, centroid_seed(hits_second), false);
            const float cx1 = fit1[0][0], cy1 = fit1[1][0], R1 = fit1[2][0];
            const float cx2 = fit2[0][0], cy2 = fit2[1][0], R2 = fit2[2][0];
            const bool ok1 = std::isfinite(cx1) && std::isfinite(cy1) && std::isfinite(R1) && R1 > 0;
            const bool ok2 = std::isfinite(cx2) && std::isfinite(cy2) && std::isfinite(R2) && R2 > 0;
            if (ok1 && ok2)
            {
                ++n_layer1_ok;
                h_R_first_vs_second->Fill(R1, R2);
                h_cx_first_vs_second->Fill(cx1, cx2);
                h_cy_first_vs_second->Fill(cy1, cy2);
                h_dx->Fill(cx1 - cx2);
                h_dy->Fill(cy1 - cy2);
                h_dR->Fill(R1 - R2);
                h_d_centre->Fill(std::hypot(cx1 - cx2, cy1 - cy2));
            }
            if (ok2)
            {
                ring2_cx_circle = cx2;
                ring2_cy_circle = cy2;
                ring2_R_circle = R2;
                ring2_circle_ok = true;
            }
        }

        // ── Layer 2: ellipse fit on ring 2 hits ───────────────────
        //
        //   ring2 doesn't require ring1 to exist — this layer runs
        //   on EVERY frame with enough ring-2 hits.  But if we already
        //   have a circle fit from Layer 1 we reuse it as the seed
        //   (faster convergence and a fair χ² comparison on the same
        //   hits).  Otherwise we recompute the circle seed here.
        if ((int)hits_second.size() < kMinHitsEllipse)
            continue;
        if (!ring2_circle_ok)
        {
            const auto fit2_only = fit_circle(hits_second,
                                              centroid_seed(hits_second), false);
            ring2_cx_circle = fit2_only[0][0];
            ring2_cy_circle = fit2_only[1][0];
            ring2_R_circle = fit2_only[2][0];
            if (!std::isfinite(ring2_R_circle) || ring2_R_circle <= 0.f)
                continue;
            ring2_circle_ok = true;
        }

        const EllipseFit ef = fit_ellipse(hits_second,
                                          ring2_cx_circle,
                                          ring2_cy_circle,
                                          ring2_R_circle);
        if (!ef.converged)
            continue;

        const double chi2_circle = circle_algebraic_chi2(
            hits_second, ring2_cx_circle, ring2_cy_circle, ring2_R_circle);
        const double rel_imp = (chi2_circle > 0.)
                                   ? (chi2_circle - ef.chi2) / chi2_circle
                                   : 0.;
        const double ecc = (ef.a > 0. && ef.b <= ef.a)
                               ? std::sqrt(1. - (ef.b / ef.a) * (ef.b / ef.a))
                               : 0.;

        ++n_layer2_ok;
        h_a->Fill(ef.a);
        h_b->Fill(ef.b);
        h_ecc->Fill(ecc);
        h_pa->Fill(ef.theta);
        h_a_vs_b->Fill(ef.a, ef.b);
        h_chi2_circle_vs_ellipse->Fill(chi2_circle, ef.chi2);
        h_chi2_improvement->Fill(rel_imp);
    }

    mist::logger::info("[elliptic_investigation] dual frames=" +
                       std::to_string(n_dual_frames) +
                       "  layer1 OK=" + std::to_string(n_layer1_ok) +
                       "  layer2 OK=" + std::to_string(n_layer2_ok));

    // ── Headline interpretation in the log ─────────────────────────
    if (n_layer2_ok > 100)
    {
        mist::logger::info(TString::Format(
                               "[elliptic_investigation] mean a=%.3f b=%.3f mm  "
                               "<eccentricity>=%.3f  <chi2_imp>=%.3f",
                               h_a->GetMean(), h_b->GetMean(),
                               h_ecc->GetMean(), h_chi2_improvement->GetMean())
                               .Data());
        mist::logger::info(
            "[elliptic_investigation] Reading: <eccentricity> ~0 AND <chi2_imp> ~0 → "
            "ring 2 is consistent with a circle.  <eccentricity> > 0.2 AND <chi2_imp> > 0.3 → "
            "ring 2 is genuinely elliptic (hypothesis supported).");
    }
    else
    {
        mist::logger::warning("[elliptic_investigation] too few Layer-2 fits for a "
                              "credible verdict.  Need a higher-statistics run.");
    }

    // ── Write & cleanup ────────────────────────────────────────────
    output_file->cd();
    h_R_first_vs_second->Write();
    h_cx_first_vs_second->Write();
    h_cy_first_vs_second->Write();
    h_dx->Write();
    h_dy->Write();
    h_dR->Write();
    h_d_centre->Write();
    h_a->Write();
    h_b->Write();
    h_ecc->Write();
    h_pa->Write();
    h_a_vs_b->Write();
    h_chi2_circle_vs_ellipse->Write();
    h_chi2_improvement->Write();
    mist::logger::info("[elliptic_investigation] wrote " + outname);

    delete recodata;
}
