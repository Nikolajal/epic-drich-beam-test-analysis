/**
 * @file ring_compute.cxx
 * @brief Implementation of the per-frame, per-ring compute helpers
 *        declared in `writers/recodata/ring_compute.h`.
 *
 * Lifted (with the lambda captures replaced by an explicit
 * @ref btana::recodata::RingComputeContext) from the per-frame ring-fit
 * lambdas formerly inside `src/recodata_writer.cxx`.  Algorithms are
 * unchanged.
 */

#include "writers/recodata/ring_compute.h"
#include "alcor_data.h"

#include <array>
#include <cmath>
#include <vector>

#include "TH1.h"
#include "TH2.h"

#include "alcor_finedata.h"  // AlcorFinedata
#include "alcor_lightdata.h" // AlcorLightdata
#include "utility/circle_fit.h" // fit_circle
#include "utility/radiator_efficiency.h"

namespace btana::recodata
{

//  Selection-agnostic fit core.  Given the collected ring hits (the
//  parallel AlcorFinedata array + pixel-centre + per-hit smeared (x,y)),
//  run the circle fit, radial-residual σ, azimuthal coverage and the
//  optional LOO residuals.  Hit SELECTION (the ±Δt time window) is done by
//  `compute_ring_fit_timewindow` below; this core is factored out so the
//  selection and the fit stay decoupled.
static RingFitResult fit_collected_ring_hits(
    std::vector<AlcorFinedata> &ring_fdata,
    std::vector<std::array<float, 2>> &ring_hits,
    std::vector<std::array<float, 2>> &ring_hits_smeared,
    bool do_loo,
    const RingComputeContext &ctx)
{
    RingFitResult out;
    out.n_hits = static_cast<int>(ring_hits.size());
    //  Carry the in-cut hit positions back for the trigger-Cherenkov hitmap —
    //  smeared so the map isn't a discrete pixel comb.  Recorded BEFORE the
    //  min-hits gate so the hitmap shows every in-cut hit, even on frames with
    //  too few hits to attempt a fit.
    out.hit_xy = ring_hits_smeared;
    if (out.n_hits < ctx.cfg.min_hits_per_ring)
        return out; // fit_ok stays false

    //  Centroid + median radial as initial guess (pixel-centre — fit
    //  itself stays on the canonical pixel-centre inputs to keep
    //  determinism; smearing is for hist filling only).
    float sum_x = 0.f, sum_y = 0.f;
    for (const auto &p : ring_hits)
    {
        sum_x += p[0];
        sum_y += p[1];
    }
    const float cx0 = sum_x / out.n_hits;
    const float cy0 = sum_y / out.n_hits;
    float sum_r = 0.f;
    for (int i = 0; i < out.n_hits; ++i)
        sum_r += ring_fdata[i].get_hit_r({cx0, cy0});
    const float R0 = sum_r / out.n_hits;

    const auto fit = fit_circle(ring_hits, {cx0, cy0, R0}, /*fix_XY=*/false);
    out.cx = fit[0][0];
    out.cy = fit[1][0];
    out.R = fit[2][0];
    if (!std::isfinite(out.cx) || !std::isfinite(out.cy) ||
        !std::isfinite(out.R) || out.R <= 0.f)
        return out; // fit_ok stays false, but n_hits is populated

    //  Per-ring σ from radial residuals — pixel-centre observable.
    //  Use the class helper instead of manual hypot.
    float sum_dev = 0.f, sum_dev_sq = 0.f;
    out.radial_per_hit.reserve(out.n_hits);
    out.radial_per_hit_smeared.reserve(out.n_hits);
    for (int i = 0; i < out.n_hits; ++i)
    {
        const float r_hit = ring_fdata[i].get_hit_r({out.cx, out.cy});
        const float dev = r_hit - out.R;
        sum_dev += dev;
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
        ctx.cfg.delta_r_for_coverage_mm, ctx.cfg.channel_half_width_mm);

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
            const float R_loo = loo_fit[2][0];
            if (!std::isfinite(cx_loo) || !std::isfinite(cy_loo) ||
                !std::isfinite(R_loo) || R_loo <= 0.f)
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

RingFitResult compute_ring_fit_timewindow(float t_ref_ns,
                                          float dt_min_ns,
                                          float dt_max_ns,
                                          AlcorLightdata &lightdata,
                                          bool do_loo,
                                          const RingComputeContext &ctx)
{
    //  Time-window selection: every non-afterpulse cherenkov hit whose
    //  (t_hit − t_ref) falls in [dt_min_ns, dt_max_ns] of the hardware-
    //  trigger reference time.  Used by recodata to reconstruct rings on
    //  hardware-trigger frames where the streaming/Hough self-trigger
    //  (which tags ring hits) is disabled (e.g. QA mode).  Shares the fit
    //  core with the Hough-tagged path.
    std::vector<AlcorFinedata> ring_fdata;
    std::vector<std::array<float, 2>> ring_hits;
    std::vector<std::array<float, 2>> ring_hits_smeared;
    ring_fdata.reserve(40);
    ring_hits.reserve(40);
    ring_hits_smeared.reserve(40);
    for (const auto &hit_struct : lightdata.get_cherenkov_hits_link())
    {
        AlcorFinedata fh(hit_struct);
        if (fh.is_afterpulse())
            continue;
        const float dt = fh.get_time_ns() - t_ref_ns;
        if (dt < dt_min_ns || dt > dt_max_ns)
            continue;
        ring_fdata.push_back(fh);
        ring_hits.push_back({fh.get_hit_x(), fh.get_hit_y()});
        ring_hits_smeared.push_back({fh.get_hit_x_rnd(), fh.get_hit_y_rnd()});
    }
    return fit_collected_ring_hits(ring_fdata, ring_hits, ring_hits_smeared,
                                   do_loo, ctx);
}

void fill_ring_hists(const RingFitResult &r, const RingFillHists &h)
{
    if (r.n_hits == 0)
        return;
    //  Always fill h_nhits if a hist is present (matches original
    //  semantics: even fit failures with enough hits get counted).
    if (h.h_nhits)
        h.h_nhits->Fill(r.n_hits);
    if (!r.fit_ok)
        return; // remaining fills require a valid fit

    if (h.h_fcov)
        h.h_fcov->Fill(r.f_coverage);
    if (h.h_nphotons && r.f_coverage > 0.f)
        h.h_nphotons->Fill(static_cast<float>(r.n_hits) / r.f_coverage);
    if (h.h_R)
        h.h_R->Fill(r.R);
    if (h.h_R_split)
        h.h_R_split->Fill(r.R);
    if (h.h_sigma)
        h.h_sigma->Fill(r.sigma_r);
    if (h.h_R_vs_nhits)
        h.h_R_vs_nhits->Fill(r.n_hits, r.R);
    if (h.h_R_vs_nhits_split)
        h.h_R_vs_nhits_split->Fill(r.n_hits, r.R);
    if (h.h_centre_xy)
        h.h_centre_xy->Fill(r.cx, r.cy);

    if (h.h_radial)
        for (float r_hit : r.radial_per_hit)
            h.h_radial->Fill(r_hit);
    if (h.h_radial_split)
        for (float r_hit : r.radial_per_hit)
            h.h_radial_split->Fill(r_hit);

    if (h.h_residual_vs_n)
        for (float dev : r.loo_residuals)
            h.h_residual_vs_n->Fill(r.n_hits, dev);
    if (h.h_residual_vs_n_split)
        for (float dev : r.loo_residuals)
            h.h_residual_vs_n_split->Fill(r.n_hits, dev);

    //  Smeared siblings — same indexing as the un-smeared loops.
    //  `radial_per_hit_smeared` and `loo_residuals_smeared` are filled
    //  by `fit_collected_ring_hits` using a single per-hit jitter sample,
    //  so the (pixel-centre, smeared) hist pair shares the same hit set.
    if (h.h_radial_smeared)
        for (float r_hit : r.radial_per_hit_smeared)
            h.h_radial_smeared->Fill(r_hit);
    if (h.h_radial_split_smeared)
        for (float r_hit : r.radial_per_hit_smeared)
            h.h_radial_split_smeared->Fill(r_hit);

    if (h.h_residual_vs_n_smeared)
        for (float dev : r.loo_residuals_smeared)
            h.h_residual_vs_n_smeared->Fill(r.n_hits, dev);
    if (h.h_residual_vs_n_split_smeared)
        for (float dev : r.loo_residuals_smeared)
            h.h_residual_vs_n_split_smeared->Fill(r.n_hits, dev);
}

} // namespace btana::recodata
