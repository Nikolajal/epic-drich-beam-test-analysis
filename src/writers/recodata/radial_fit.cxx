/**
 * @file radial_fit.cxx
 * @brief Implementation of the CB+pol3 radial fit declared in
 *        `writers/recodata/radial_fit.h`.
 *
 * Lifted verbatim (with the in-function lambda captures replaced by
 * explicit parameters) from the in-function `fit_radial_distribution`
 * lambda formerly inside `src/recodata_writer.cxx`'s QA-finalize
 * block.  Algorithm is unchanged; behavior is identical, verifiable
 * against the pre-refactor baseline in
 * `Data/20251111-164951/baseline_pre_refactor/recodata.root`.
 */

#include "writers/recodata/radial_fit.h"

#include <algorithm>   // std::clamp
#include <memory>      // std::unique_ptr
#include <string>

#include "TCanvas.h"
#include "TF1.h"
#include "TH1.h"
#include "TPaveText.h"
#include "TString.h"

#include "mist/logger/logger.h"

namespace btana::recodata {

void fit_radial_distribution(TH1F *h,
                             TH1F *h_R_count,
                             const std::string &tag,
                             const RecodataConfigStruct &cfg,
                             const std::string &data_repository,
                             const std::string &run_name,
                             std::vector<RadialFitResult> &results)
{
    if (!h || h->GetEntries() < 100) {
        mist::logger::warning(TString::Format(
            "(recodata_writer) %s: too few entries for radial fit (%lld); skipping.",
            tag.c_str(), (long long)(h ? h->GetEntries() : 0)).Data());
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
    const float r_band_lo = cfg.r_min_coverage_mm + 5.f;  // ≈ 30 mm
    const float r_band_hi = cfg.r_max_coverage_mm - 5.f;  // ≈ 120 mm

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
    //  Switched 2026-05-27 from Crystal-Ball + pol3 to simple
    //  Gaussian + pol3.  The CB tail parameters (α, n) were rail-
    //  locking on the low-statistics / broad samples (especially the
    //  second ring), making the fit unstable across runs.  A simple
    //  Gaussian peak has no tail rail to lock on; for these radial
    //  distributions the (broad) tail is largely background-driven
    //  anyway and is better absorbed by pol3 than by CB's analytic
    //  power-law arm.
    //
    //  Form:
    //      f(x) = N · exp(−(x − μ)² / (2σ²))
    //           + c0 + c1·x + c2·x² + c3·x³
    //
    //  ParLimits keep the peak σ in the same physical band [1.5, 5.0]
    //  mm so the result is comparable to the historical CB output.
    //  The CB+pol3 results from the previous run remain a valid
    //  benchmark in `Data/20251111-164951/post_refactor_phase1/`.
    static const char *kGaussPol3Formula =
        "[0]*exp(-0.5*((x-[1])/[2])*((x-[1])/[2]))"
        " + [3] + [4]*x + [5]*x*x + [6]*x*x*x";
    TF1 cb_fit((tag + "_cb_fit").c_str(),
               kGaussPol3Formula, fit_lo, fit_hi);
    const char *parnames[7] = {
        "peak_amp", "peak_mu", "peak_sigma",
        "bg_c0", "bg_c1", "bg_c2", "bg_c3"};
    for (int i = 0; i < 7; ++i) cb_fit.SetParName(i, parnames[i]);
    cb_fit.SetParameters(amp_seed, peak_seed, sigma_seed,
                         bg_prefit.GetParameter(0), bg_prefit.GetParameter(1),
                         bg_prefit.GetParameter(2), bg_prefit.GetParameter(3));
    cb_fit.SetParLimits(0, 0., 1e9);
    cb_fit.SetParLimits(2, 1.5, 5.0);   // peak σ physically bounded

    //  Two-stage strategy (no IMPROVE) — same as before.
    //  Stage 1: freeze background to prefit → minimiser finds the
    //           Gaussian peak (amp, μ, σ) alone.
    //  Stage 2: release background, full 7-param fit from the
    //           stage-1 starting point.  "S" saves the covariance.
    //  bg params now live at indices 3..6 instead of 5..8.
    for (int i = 3; i < 7; ++i)
        cb_fit.FixParameter(i, bg_prefit.GetParameter(i - 3));
    h->Fit(&cb_fit, "RQ");
    for (int i = 3; i < 7; ++i) cb_fit.ReleaseParameter(i);
    h->Fit(&cb_fit, "RQS");
    //  cb_fit gets auto-attached to h's function list by Fit()
    //  and ships with the hist on h->Write — no separate
    //  cb_fit.Write() needed (and a separate write would leak
    //  a TF1 with no canvas owner, which TBrowser handles
    //  poorly).

    //  Background-only component (pol3 with the fitted bg
    //  params) — used inline below for the PDF canvas overlay
    //  and the N_γ extraction.  Not written to the ROOT file.
    //  pol3 params live at indices 3..6 now (Gauss+pol3).
    TF1 bg_only((tag + "_bg_only").c_str(), "pol3", fit_lo, fit_hi);
    for (int i = 0; i < 4; ++i)
        bg_only.SetParameter(i, cb_fit.GetParameter(3 + i));

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

        //  Fit-parameter table on the canvas.  Top group: headline
        //  physics (N_γ, χ²/ndf).  Middle: 3-param Gaussian (amp,
        //  μ, σ).  Bottom: 4-param pol3 background (c0..c3).
        const double chi2 = cb_fit.GetChisquare();
        const int    ndf  = cb_fit.GetNDF();
        const double chi2_per_ndf = (ndf > 0) ? chi2 / ndf : 0.0;

        //  NDC corners: upper-right corner (x: 0.65–0.9, y: 0.5–0.9).
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
        pave.AddText("Gaussian peak:");
        pave.AddText(TString::Format("  amp = %.3g #pm %.2g",
            cb_fit.GetParameter(0), cb_fit.GetParError(0)).Data());
        pave.AddText(TString::Format("  #mu = %.3f #pm %.3f mm",
            cb_fit.GetParameter(1), cb_fit.GetParError(1)).Data());
        pave.AddText(TString::Format("  #sigma = %.3f #pm %.3f mm",
            cb_fit.GetParameter(2), cb_fit.GetParError(2)).Data());
        pave.AddText(" ");
        pave.AddText("pol3 background:");
        pave.AddText(TString::Format("  c_{0} = %.3g", cb_fit.GetParameter(3)).Data());
        pave.AddText(TString::Format("  c_{1} = %.3g", cb_fit.GetParameter(4)).Data());
        pave.AddText(TString::Format("  c_{2} = %.3g", cb_fit.GetParameter(5)).Data());
        pave.AddText(TString::Format("  c_{3} = %.3g", cb_fit.GetParameter(6)).Data());
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
        "(recodata_writer) %s   Gauss: amp=%.3g+/-%.2g  mu=%.3f+/-%.3f mm  "
        "sigma=%.3f+/-%.3f mm",
        tag.c_str(),
        cb_fit.GetParameter(0), cb_fit.GetParError(0),
        cb_fit.GetParameter(1), cb_fit.GetParError(1),
        cb_fit.GetParameter(2), cb_fit.GetParError(2)).Data());
    mist::logger::info(TString::Format(
        "(recodata_writer) %s   pol3 bg: c0=%.3g  c1=%.3g  c2=%.3g  c3=%.3g",
        tag.c_str(),
        cb_fit.GetParameter(3), cb_fit.GetParameter(4),
        cb_fit.GetParameter(5), cb_fit.GetParameter(6)).Data());

    //  Push into the summary collector.
    results.push_back({tag, n_gamma,
        cb_fit.GetParameter(1), cb_fit.GetParError(1),
        cb_fit.GetParameter(2), cb_fit.GetParError(2)});
}

} // namespace btana::recodata
