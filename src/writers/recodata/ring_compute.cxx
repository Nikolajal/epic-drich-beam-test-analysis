/**
 * @file ring_compute.cxx
 * @brief Implementation of the per-frame, per-ring compute helpers
 *        declared in `writers/recodata/ring_compute.h`.
 *
 * Lifted verbatim (with the lambda captures replaced by an explicit
 * @ref btana::recodata::RingComputeContext) from the in-function
 * `compute_ring_fit_pure`, `fill_ring_hists`, and `refit_and_fill_ring`
 * lambdas formerly inside `src/recodata_writer.cxx`.  Algorithms are
 * unchanged; bit-identical output verified against
 * `Data/20251111-164951/baseline_pre_refactor/recodata.root`.
 */

#include "writers/recodata/ring_compute.h"

#include <array>
#include <cmath>
#include <vector>

#include "TH1.h"
#include "TH2.h"

#include "alcor_finedata.h"          // AlcorFinedata
#include "alcor_lightdata.h"         // AlcorLightdata
#include "util/circle_fit.h"         // fit_circle
#include "util/radiator_efficiency.h"

namespace btana::recodata {

RingFitResult compute_ring_fit(HitMask ring_bit,
                               AlcorLightdata &lightdata,
                               bool do_loo,
                               const RingComputeContext &ctx)
{
    RingFitResult out;

    //  Collect both pixel-centre and a SINGLE pixel-jittered sample
    //  per hit.  Drawing one smeared sample per hit at this point keeps
    //  the LOO loop, the smeared radial hist, and the smeared σ-vs-N
    //  hist all consistent (they see the same realisation of pixel
    //  jitter for hit i).  Drawing fresh samples per accessor call
    //  would inflate the variance and decorrelate the cross-checks.
    std::vector<AlcorFinedata>           ring_fdata;     ///< parallel array — kept only for the per-hit accessor mid-loop
    std::vector<std::array<float, 2>>    ring_hits;
    std::vector<std::array<float, 2>>    ring_hits_smeared;
    ring_fdata.reserve(40);
    ring_hits.reserve(40);
    ring_hits_smeared.reserve(40);

    for (const auto &hit_struct : lightdata.get_cherenkov_hits_link())
    {
        AlcorFinedata fh(hit_struct);
        if (!fh.has_mask_bit(ring_bit))
            continue;
        ring_fdata.push_back(fh);
        ring_hits.push_back({fh.get_hit_x(), fh.get_hit_y()});
        //  Single per-hit smeared sample, captured once here.
        ring_hits_smeared.push_back({fh.get_hit_x_rnd(), fh.get_hit_y_rnd()});
    }
    out.n_hits = static_cast<int>(ring_hits.size());
    if (out.n_hits < ctx.cfg.min_hits_per_ring)
        return out;  // fit_ok stays false

    //  Centroid + median radial as initial guess (pixel-centre — fit
    //  itself stays on the canonical pixel-centre inputs to keep
    //  determinism; smearing is for hist filling only).
    float sum_x = 0.f, sum_y = 0.f;
    for (const auto &p : ring_hits) { sum_x += p[0]; sum_y += p[1]; }
    const float cx0 = sum_x / out.n_hits;
    const float cy0 = sum_y / out.n_hits;
    float sum_r = 0.f;
    for (int i = 0; i < out.n_hits; ++i)
        sum_r += ring_fdata[i].get_hit_r({cx0, cy0});
    const float R0 = sum_r / out.n_hits;

    const auto fit = fit_circle(ring_hits, {cx0, cy0, R0}, /*fix_XY=*/false);
    out.cx = fit[0][0];
    out.cy = fit[1][0];
    out.R  = fit[2][0];
    if (!std::isfinite(out.cx) || !std::isfinite(out.cy) ||
        !std::isfinite(out.R)  || out.R <= 0.f)
        return out;  // fit_ok stays false, but n_hits is populated

    //  Per-ring σ from radial residuals — pixel-centre observable.
    //  Use the class helper instead of manual hypot.
    float sum_dev = 0.f, sum_dev_sq = 0.f;
    out.radial_per_hit.reserve(out.n_hits);
    out.radial_per_hit_smeared.reserve(out.n_hits);
    for (int i = 0; i < out.n_hits; ++i)
    {
        const float r_hit = ring_fdata[i].get_hit_r({out.cx, out.cy});
        const float dev   = r_hit - out.R;
        sum_dev    += dev;
        sum_dev_sq += dev * dev;
        out.radial_per_hit.push_back(r_hit);
        //  Smeared sibling: reuse the captured per-hit smeared (x, y)
        //  rather than calling get_hit_r_rnd which would draw a fresh
        //  jitter — see the rationale at the top of this function.
        const float r_hit_smeared = std::hypot(
            ring_hits_smeared[i][0] - out.cx,
            ring_hits_smeared[i][1] - out.cy);
        out.radial_per_hit_smeared.push_back(r_hit_smeared);
    }
    const float mean_dev = sum_dev / out.n_hits;
    const float variance = std::max(0.f, sum_dev_sq / out.n_hits - mean_dev * mean_dev);
    out.sigma_r = std::sqrt(variance);

    out.f_coverage = util::radiator_efficiency::azimuthal_coverage_fraction(
        ctx.index_to_hit_xy, out.cx, out.cy, out.R,
        ctx.cfg.delta_r_for_coverage_mm);

    //  LOO residuals (optional — gate on do_loo).
    if (do_loo)
    {
        const std::array<float, 3> loo_seed = {out.cx, out.cy, out.R};
        out.loo_residuals.reserve(out.n_hits);
        out.loo_residuals_smeared.reserve(out.n_hits);
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
            //  Pixel-centre residual.
            const float r_loo = ring_fdata[i_excl].get_hit_r({cx_loo, cy_loo});
            out.loo_residuals.push_back(r_loo - R_loo);
            //  Smeared residual using the SAME jitter realisation as
            //  was used to fill h_radial_*_smeared.
            const float r_loo_smeared = std::hypot(
                ring_hits_smeared[i_excl][0] - cx_loo,
                ring_hits_smeared[i_excl][1] - cy_loo);
            out.loo_residuals_smeared.push_back(r_loo_smeared - R_loo);
        }
    }

    out.fit_ok = true;
    return out;
}

void fill_ring_hists(const RingFitResult &r, const RingFillHists &h)
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

    //  Smeared siblings — same indexing as the un-smeared loops.
    //  `radial_per_hit_smeared` and `loo_residuals_smeared` are filled
    //  by `compute_ring_fit` using a single per-hit jitter sample, so
    //  the (pixel-centre, smeared) hist pair shares the same hit set.
    if (h.h_radial_smeared)
        for (float r_hit : r.radial_per_hit_smeared) h.h_radial_smeared->Fill(r_hit);
    if (h.h_radial_split_smeared)
        for (float r_hit : r.radial_per_hit_smeared) h.h_radial_split_smeared->Fill(r_hit);

    if (h.h_residual_vs_n_smeared)
        for (float dev : r.loo_residuals_smeared)
            h.h_residual_vs_n_smeared->Fill(r.n_hits, dev);
    if (h.h_residual_vs_n_split_smeared)
        for (float dev : r.loo_residuals_smeared)
            h.h_residual_vs_n_split_smeared->Fill(r.n_hits, dev);
}

bool refit_and_fill_ring(HitMask ring_bit,
                         const RingFillHists &h,
                         AlcorLightdata &lightdata,
                         const RingComputeContext &ctx)
{
    //  `do_loo` is on by default but gated off by the
    //  `skip_loo_residuals` knob in conf/recodata.toml (typically
    //  set in conf/QA/recodata.toml).  When off, the LOO loop is
    //  skipped saving ~N extra fit_circle calls per ring per
    //  event; the per-hit residual hists then stay empty and the
    //  σ_photon fit at finalize silently no-ops.
    const bool do_loo = !ctx.cfg.skip_loo_residuals
                        && (h.h_residual_vs_n          || h.h_residual_vs_n_split
                         || h.h_residual_vs_n_smeared  || h.h_residual_vs_n_split_smeared);
    const RingFitResult r = compute_ring_fit(ring_bit, lightdata, do_loo, ctx);
    fill_ring_hists(r, h);
    return r.fit_ok;
}

} // namespace btana::recodata
