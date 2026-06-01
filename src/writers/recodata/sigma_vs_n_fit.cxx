/**
 * @file sigma_vs_n_fit.cxx
 * @brief Implementation of the one-parameter LOO σ(N) fit declared in
 *        `writers/recodata/sigma_vs_n_fit.h`.
 *
 * Lifted verbatim (with the in-function lambda captures replaced by
 * explicit parameters) from the in-function `fit_sigma_vs_n` lambda
 * formerly inside `src/recodata_writer.cxx`'s QA-finalize block.
 */

#include "writers/recodata/sigma_vs_n_fit.h"
#include "utility/qa_publish.h"  // util::qa::pdf_path — land PDFs under qa/recodata/

#include <cmath>     // std::abs, std::isfinite
#include <algorithm> // std::max
#include <memory>
#include <string>

#include "TCanvas.h"
#include "TDirectory.h"
#include "TF1.h"
#include "TH1.h"
#include "TH2.h"
#include "TNamed.h"
#include "TPaveText.h"
#include "TString.h"

#include "mist/logger/logger.h"

namespace btana::recodata
{

void fit_sigma_vs_n(TH2F *h2,
                    const std::string &data_repository,
                    const std::string &run_name,
                    std::vector<VsNFitResult> &results)
{
    if (!h2)
    {
        mist::logger::warning(
            "(recodata_writer) fit_sigma_vs_n: null TH2F; skipping.");
        return;
    }
    if (h2->GetEntries() == 0)
    {
        mist::logger::warning(TString::Format(
                                  "(recodata_writer) %s: TH2F empty (0 entries) — "
                                  "no σ-vs-N PDF emitted.",
                                  h2->GetName())
                                  .Data());
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
         ";#sigma (mm)")
            .c_str(),
        h2->GetNbinsX(),
        h2->GetXaxis()->GetXmin(),
        h2->GetXaxis()->GetXmax());
    h_sigma_slice->SetDirectory(gDirectory); // persisted in Rings/
    h_sigma_slice->SetMarkerStyle(20);
    h_sigma_slice->SetMarkerSize(0.8);

    constexpr int kMinSliceEntries = 5;
    int n_slices_fit = 0;
    for (int ix = 1; ix <= h2->GetNbinsX(); ++ix)
    {
        std::unique_ptr<TH1D> slice(h2->ProjectionY(
            (sigma_name + "_slice_" + std::to_string(ix)).c_str(),
            ix, ix));
        slice->SetDirectory(nullptr);
        if (slice->GetEntries() < kMinSliceEntries)
            continue;

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
        const double rms0 = slice->GetRMS();
        TF1 gfit("gfit", "gaus",
                 mean0 - 2.0 * rms0,
                 mean0 + 2.0 * rms0);
        gfit.SetParameters(slice->GetMaximum(),
                           mean0,
                           std::max(rms0, 0.1));
        int fit_status = static_cast<int>(slice->Fit(&gfit, "RQ0"));
        if (fit_status != 0)
            continue;

        //  Pass 2 refit, seeded from pass-1 result.
        const double mu1 = gfit.GetParameter(1);
        const double sigma1 = std::abs(gfit.GetParameter(2));
        if (sigma1 > 0. && std::isfinite(sigma1))
        {
            gfit.SetRange(mu1 - 2.5 * sigma1, mu1 + 2.5 * sigma1);
            fit_status = static_cast<int>(slice->Fit(&gfit, "RQ0"));
            if (fit_status != 0)
                continue;
        }
        const double sigma = std::abs(gfit.GetParameter(2));
        const double sigma_err = gfit.GetParError(2);
        if (sigma <= 0. || !std::isfinite(sigma))
            continue;
        h_sigma_slice->SetBinContent(ix, sigma);
        h_sigma_slice->SetBinError(ix, sigma_err);
        ++n_slices_fit;
    }
    if (n_slices_fit == 0)
    {
        mist::logger::warning(TString::Format(
                                  "(recodata_writer) %s: 0 slices passed per-slice "
                                  "Gaussian fit (min entries = %d) — skipping σ-vs-N PDF.",
                                  h2->GetName(), kMinSliceEntries)
                                  .Data());
        delete h_sigma_slice;
        return;
    }
    // σ_photon·sqrt(x/(x-3)) is a 1-param fit; need at least 2 σ points.
    if (n_slices_fit < 2)
    {
        mist::logger::warning(TString::Format(
                                  "(recodata_writer) %s: only %d slice yielded σ — "
                                  "cannot fit σ_photon·√(N/(N-3)). Skipping PDF.",
                                  h2->GetName(), n_slices_fit)
                                  .Data());
        return;
    }
    //  Restrict fit range to bins that actually have content —
    //  empty bins at the edges shouldn't be in [xmin,xmax] for
    //  the fit, or LM will widen the χ² window over zero.
    int first_pop = 0, last_pop = 0;
    for (int ib = 1; ib <= h_sigma_slice->GetNbinsX(); ++ib)
    {
        if (h_sigma_slice->GetBinContent(ib) > 0.)
        {
            if (first_pop == 0)
                first_pop = ib;
            last_pop = ib;
        }
    }
    const double fit_x_lo = h_sigma_slice->GetBinLowEdge(first_pop);
    const double fit_x_hi = h_sigma_slice->GetBinLowEdge(last_pop) + h_sigma_slice->GetBinWidth(last_pop);

    //  Exact one-parameter LOO model:
    //      σ(N) = σ_photon · sqrt( N / (N − 3) )
    //
    //  Derivation: for a 3-parameter circle fit (cx, cy, R) the
    //  leave-one-out residual variance is exactly
    //      Var(Δr_i) = σ²_photon · N / (N − 3)
    //  (each of the 3 parameters contributes σ²/(N-1) through
    //  the Gauss-Newton hat matrix; summing gives 3σ²/(N-1),
    //  then the N/(N-1) Bessel-like correction yields N/(N-3).)
    //
    //  The old two-parameter model sqrt(A/N + B) with A=3σ²
    //  and B=σ² has A=3B always — the parameters are not
    //  independent, the fit is degenerate, and for small N
    //  (beam-test rings: N~5–15) the approximation is poor.
    TF1 *f_scaling = new TF1(
        (std::string(h2->GetName()) + "_scaling").c_str(),
        "[0]*sqrt(x/(x-3))", fit_x_lo, fit_x_hi);
    f_scaling->SetParameter(0, 1.5); // seed: σ_photon ~ 1.5 mm
    f_scaling->SetParName(0, "sigma_photon");
    f_scaling->SetParLimits(0, 0.1, 10.0);
    h_sigma_slice->Fit(f_scaling, "RQS");

    //  ── Canvas with σ-per-N hist + fit + σ_photon label, as PDF ──
    //  Same pattern as the radial canvas above (and as the
    //  offline `photon_number_new.cpp` macro): in-memory
    //  canvas + SaveAs PDF.  No ROOT-file write.
    //
    //  Model: σ(N) = σ_photon · √(N/(N-3))   [exact LOO, 1 parameter]
    {
        const double sigma_for_pave = f_scaling->GetParameter(0);
        const double sigma_err_for_pave = f_scaling->GetParError(0);

        TCanvas c(
            (std::string("c_") + h2->GetName() + "_sigma_vs_n").c_str(),
            (std::string("sigma vs N: ") + h2->GetName()).c_str(),
            900, 650);
        c.SetGrid();
        //  Explicit X/Y range.  X clamped to [4, 20] so the
        //  empty low-N bins (N≤3 where the model diverges) and
        //  the rarely-populated high-N tail don't distort the
        //  axis.  Y upper bound: h_residual_vs_n_* → [0.5, 2.5]
        //  mm (σ_photon ≲ 2 mm in beam-test conditions).
        //  Both choices fixed across dual/solo/first/second so
        //  operator comparison plots share the same scale.
        h_sigma_slice->GetXaxis()->SetRangeUser(4., 20.);
        h_sigma_slice->GetYaxis()->SetRangeUser(0.5, 2.5);
        h_sigma_slice->Draw("E1");

        TPaveText pave(0.55, 0.68, 0.92, 0.88, "NDC");
        pave.SetFillStyle(0);
        pave.SetBorderSize(1);
        pave.SetTextAlign(12);
        pave.SetTextSize(0.034);
        pave.AddText("#sigma(N) = #sigma_{photon} #sqrt{N/(N#minus3)}");
        pave.AddText(TString::Format("#sigma_{photon} = %.3f #pm %.3f mm",
                                     sigma_for_pave, sigma_err_for_pave)
                         .Data());
        pave.Draw();

        c.Update();
        //  Land under qa/recodata/ so the dashboard discovers it (was
        //  dumping into the run root).
        const auto pdf = util::qa::pdf_path(
            data_repository + "/" + run_name, "recodata", 21,
            std::string(h2->GetName()) + "_sigma_vs_n");
        c.SaveAs(pdf.string().c_str());
        util::qa::crop_pdf_inplace(pdf);
    }

    //  ── Extract σ_photon as a labeled scalar ──────────────────
    //  Exact one-parameter LOO fit: σ_photon is par[0] directly.
    //  Stored as a TNamed alongside the 2D hist so downstream
    //  consumers (live displays, summary scripts) can read it
    //  without re-fitting.  Also echoed to the log so the
    //  operator sees the run's resolution in realtime.
    const double sigma_phot = f_scaling->GetParameter(0);
    const double sigma_err = f_scaling->GetParError(0);
    TNamed sigma_named(
        (std::string(h2->GetName()) + "_sigma_photon_mm").c_str(),
        TString::Format("%.4f +/- %.4f", sigma_phot, sigma_err).Data());
    sigma_named.Write();
    mist::logger::info(TString::Format(
                           "(recodata_writer) %s: sigma_photon = %.3f +/- %.3f mm "
                           "[exact LOO fit: σ(N) = σ_photon·√(N/(N-3))]",
                           h2->GetName(), sigma_phot, sigma_err)
                           .Data());

    //  Push into the summary collector.  Tagging by hist-name
    //  prefix is uglier than a separate function arg but keeps
    //  the lambda single-purpose — minor.
    const std::string hname = h2->GetName();
    const bool is_residual = hname.find("h_residual_vs_n_") == 0;
    results.push_back({hname, sigma_phot, sigma_err, is_residual});
}

} // namespace btana::recodata
