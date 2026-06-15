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
#include <mist/ring_finding/circle_fit.h> // mist::ring_finding::circle_fit (Taubin)
#include "utility/radiator_efficiency.h"

namespace btana::recodata
{

namespace
{
    //  Minimal Point2-satisfying adapter so the hit (x, y) arrays can feed
    //  mist::ring_finding::circle_fit (which wants `.x`/`.y` members).
    struct FitPoint
    {
        double x, y;
    };
} // namespace

//  Selection-agnostic fit core.  Given the collected ring hits (the
//  parallel AlcorFinedata array + pixel-centre + per-hit smeared (x,y)),
//  run the circle fit, radial-residual σ, azimuthal coverage and the
//  optional LOO residuals.  Hit SELECTION (the ±Δt time window) is done by
//  `compute_ring_fit_timewindow` below; this core is factored out so the
//  selection and the fit stay decoupled.
//  Azimuthal span [rad] subtended by the hits about a given centre = 2π minus
//  the largest angular gap between consecutive hit bearings.
static float azimuthal_span_about(
    const std::vector<std::array<float, 2>> &ring_hits, float cx, float cy)
{
    constexpr float two_pi = 6.28318530717958647692f;
    if (ring_hits.size() < 2)
        return 0.f;
    std::vector<float> ang;
    ang.reserve(ring_hits.size());
    for (const auto &p : ring_hits)
        ang.push_back(std::atan2(p[1] - cy, p[0] - cx));
    std::sort(ang.begin(), ang.end());
    float max_gap = two_pi + ang.front() - ang.back();
    for (std::size_t i = 1; i < ang.size(); ++i)
        max_gap = std::max(max_gap, ang[i] - ang[i - 1]);
    return two_pi - max_gap;
}

//  Core ring fit.  When a valid finder seed is supplied (seed_R > 0) the fit is
//  SEEDED from the streaming-RANSAC (cx,cy,R): for a well-constrained (long) arc
//  the seedless Taubin refit is trusted as a refinement, but for a short
//  far-off-centre arc — where a free refit is high-variance and collapses the
//  centre toward the origin — the robust finder centre is kept and only the
//  radius is re-estimated from this frame's hits.  With no seed (seed_R <= 0,
//  the legacy time-window path) the behaviour is the original seedless Taubin
//  fit with the short-arc rejection guard.
static RingFitResult fit_collected_ring_hits(
    std::vector<AlcorFinedata> &ring_fdata,
    std::vector<std::array<float, 2>> &ring_hits,
    std::vector<std::array<float, 2>> &ring_hits_smeared,
    bool do_loo,
    const RingComputeContext &ctx,
    float seed_cx = 0.f,
    float seed_cy = 0.f,
    float seed_R = 0.f)
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

    const bool has_seed = seed_R > 0.f;

    //  Geometry fit: mist's closed-form Taubin algebraic circle fit — no
    //  seed, no iteration, and (unlike Kåsa or a centroid-seeded radial-
    //  residual minimiser) UNBIASED on partial arcs.  Pixel-centre inputs
    //  throughout (smearing is for hist filling only).
    std::vector<FitPoint> pts;
    pts.reserve(out.n_hits);
    for (const auto &p : ring_hits)
        pts.push_back({p[0], p[1]});

    const auto fit = mist::ring_finding::circle_fit(
        pts, mist::ring_finding::circle_method::taubin);
    const bool taubin_ok =
        fit.ok && std::isfinite(fit.radius) && fit.radius > 0.0;

    //  Decide the ring geometry.  `taubin_refined` records whether the chosen
    //  (cx,cy,R) came from the seedless Taubin refit (true) or the finder seed
    //  with radius from the hits (false) — it selects the σ_r / LOO treatment.
    bool taubin_refined;
    if (has_seed)
    {
        //  Span about the ROBUST finder centre (stable even for a far arc).
        const float span = azimuthal_span_about(ring_hits, seed_cx, seed_cy);
        const bool long_arc = span >= ctx.cfg.arc_span_min_rad;
        //  Trust the free refit only when the arc is long enough to constrain
        //  it AND the refit stays near the seed (a sanity bound — a genuine
        //  refinement is a small correction, not a jump to a different ring).
        const float dcx = static_cast<float>(fit.x0) - seed_cx;
        const float dcy = static_cast<float>(fit.y0) - seed_cy;
        const bool near_seed =
            taubin_ok && std::hypot(dcx, dcy) < 0.5f * seed_R &&
            std::fabs(static_cast<float>(fit.radius) - seed_R) < 0.5f * seed_R;
        if (long_arc && near_seed)
        {
            out.cx = static_cast<float>(fit.x0);
            out.cy = static_cast<float>(fit.y0);
            out.R = static_cast<float>(fit.radius);
            taubin_refined = true;
        }
        else
        {
            //  Keep the finder centre; re-estimate R as the mean hit distance
            //  to it (well-conditioned at a fixed centre, even for a sliver).
            out.cx = seed_cx;
            out.cy = seed_cy;
            double rsum = 0.0;
            for (int i = 0; i < out.n_hits; ++i)
                rsum += ring_fdata[i].get_hit_r({out.cx, out.cy});
            out.R = static_cast<float>(rsum / out.n_hits);
            taubin_refined = false;
        }
        //  Never reject when seeded — the finder already validated the ring.
    }
    else
    {
        //  Legacy seedless path.
        if (!taubin_ok)
            return out; // degenerate / (near-)collinear → fit_ok stays false
        out.cx = static_cast<float>(fit.x0);
        out.cy = static_cast<float>(fit.y0);
        out.R = static_cast<float>(fit.radius);
        taubin_refined = true;
        //  Wide-arc quality guard: even Taubin can't pin a far centre from a
        //  very short arc, so reject if the azimuthal span about the fitted
        //  centre is below threshold.
        if (ctx.cfg.radial_eff_per_ring_centre &&
            azimuthal_span_about(ring_hits, out.cx, out.cy) <
                ctx.cfg.arc_span_min_rad)
            return out; // arc too short to constrain the centre
    }

    //  Per-ring σ_r: the Taubin residual RMS when refined, else the RMS of the
    //  radial residuals about the chosen (fixed-centre) ring.  The per-hit
    //  radial arrays feed the radial(R) hists (pixel-centre + smeared sibling).
    out.radial_per_hit.reserve(out.n_hits);
    out.radial_per_hit_smeared.reserve(out.n_hits);
    double resid_sq = 0.0;
    for (int i = 0; i < out.n_hits; ++i)
    {
        const float r_pix = ring_fdata[i].get_hit_r({out.cx, out.cy});
        out.radial_per_hit.push_back(r_pix);
        const float d = r_pix - out.R;
        resid_sq += static_cast<double>(d) * d;
        //  Smeared sibling: reuse the captured per-hit smeared (x, y) rather
        //  than re-drawing jitter, so the (pixel-centre, smeared) hist pair
        //  shares one hit set.
        out.radial_per_hit_smeared.push_back(std::hypot(
            ring_hits_smeared[i][0] - out.cx,
            ring_hits_smeared[i][1] - out.cy));
    }
    out.sigma_r = taubin_refined
                      ? static_cast<float>(fit.rms_residual)
                      : static_cast<float>(std::sqrt(resid_sq / out.n_hits));

    out.f_coverage = util::radiator_efficiency::azimuthal_coverage_fraction(
        ctx.index_to_hit_xy, out.cx, out.cy, out.R,
        ctx.cfg.delta_r_for_coverage_mm, ctx.cfg.channel_half_width_mm);

    //  LOO residuals (optional — gate on do_loo).
    if (do_loo)
    {
        out.loo_residuals.reserve(out.n_hits);
        out.loo_residuals_smeared.reserve(out.n_hits);
        if (taubin_refined)
        {
            //  Re-fit (Taubin, closed-form) on the hit set minus one point.
            std::vector<FitPoint> loo_pts;
            loo_pts.reserve(out.n_hits > 0 ? out.n_hits - 1 : 0);
            for (int i_excl = 0; i_excl < out.n_hits; ++i_excl)
            {
                loo_pts.clear();
                for (int j = 0; j < out.n_hits; ++j)
                    if (j != i_excl)
                        loo_pts.push_back({ring_hits[j][0], ring_hits[j][1]});
                const auto loo_fit = mist::ring_finding::circle_fit(
                    loo_pts, mist::ring_finding::circle_method::taubin);
                if (!loo_fit.ok || !std::isfinite(loo_fit.radius) ||
                    loo_fit.radius <= 0.0)
                    continue;
                const float cx_loo = static_cast<float>(loo_fit.x0);
                const float cy_loo = static_cast<float>(loo_fit.y0);
                const float R_loo = static_cast<float>(loo_fit.radius);
                out.loo_residuals.push_back(
                    ring_fdata[i_excl].get_hit_r({cx_loo, cy_loo}) - R_loo);
                out.loo_residuals_smeared.push_back(std::hypot(
                                                        ring_hits_smeared[i_excl][0] - cx_loo,
                                                        ring_hits_smeared[i_excl][1] - cy_loo) -
                                                    R_loo);
            }
        }
        else
        {
            //  Seed-fixed ring: the geometry does not depend on the hits, so a
            //  leave-one-out refit is just the residual of the excluded hit to
            //  the fixed ring.  Stable for short arcs where a free LOO refit
            //  would be meaningless.
            for (int i_excl = 0; i_excl < out.n_hits; ++i_excl)
            {
                out.loo_residuals.push_back(
                    ring_fdata[i_excl].get_hit_r({out.cx, out.cy}) - out.R);
                out.loo_residuals_smeared.push_back(std::hypot(
                                                        ring_hits_smeared[i_excl][0] - out.cx,
                                                        ring_hits_smeared[i_excl][1] - out.cy) -
                                                    out.R);
            }
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
    //  hardware-trigger frames where the streaming/RANSAC self-trigger
    //  (which tags ring hits) is disabled (e.g. QA mode).  Shares the fit
    //  core with the RANSAC-tagged path.
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

RingFitResult compute_ring_fit_tagged(HitMask ring_tag,
                                      AlcorLightdata &lightdata,
                                      bool do_loo,
                                      const RingComputeContext &ctx)
{
    //  Hit selection: every non-afterpulse cherenkov hit the streaming-RANSAC
    //  stage tagged with `ring_tag`.  The RANSAC already isolated the ring
    //  members (voting + collection_radius), so this fits the actual arc
    //  rather than the whole in-time hit cloud.  Shares the fit core with the
    //  time-window path.
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
        if (!fh.has_mask_bit(ring_tag))
            continue;
        ring_fdata.push_back(fh);
        ring_hits.push_back({fh.get_hit_x(), fh.get_hit_y()});
        ring_hits_smeared.push_back({fh.get_hit_x_rnd(), fh.get_hit_y_rnd()});
    }

    //  Seed the fit from the streaming-RANSAC ring this slot's hits belong to —
    //  the finder's completeness-corrected (cx,cy,R) is robust on far short
    //  arcs where a free re-fit collapses toward the origin.  The first/second
    //  slot maps to ring1/ring2 in the per-frame struct (radius 0 ⇒ no seed,
    //  e.g. an old lightdata.root without the fields → legacy seedless fit).
    const auto &ld = lightdata.get_lightdata_link();
    const bool first = (ring_tag == HitmaskRansacRingTagFirst);
    const float seed_cx = first ? ld.ring1_cx : ld.ring2_cx;
    const float seed_cy = first ? ld.ring1_cy : ld.ring2_cy;
    const float seed_R = first ? ld.ring1_radius : ld.ring2_radius;
    return fit_collected_ring_hits(ring_fdata, ring_hits, ring_hits_smeared,
                                   do_loo, ctx, seed_cx, seed_cy, seed_R);
}

void fill_ring_hists(const RingFitResult &r, const RingFillHists &h,
                     bool eff_weight_per_ring)
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

    //  Radial(R) distribution.  In wide-arc mode the aggregate
    //  fixed-nominal-centre eff(R) divide downstream is disabled, so we
    //  correct here instead: weight every hit by 1/f_coverage (the ring's
    //  azimuthal coverage at its fitted centre), scaling each arc up to the
    //  equivalent full ring.  When f_coverage is non-positive the ring can't
    //  be corrected, so its radial hits are skipped (rather than polluting
    //  the distribution uncorrected).  Legacy mode fills unit-weight and the
    //  eff(R) divide happens at finalize.
    const bool fill_radial = !eff_weight_per_ring || r.f_coverage > 0.f;
    const float radial_w =
        (eff_weight_per_ring && r.f_coverage > 0.f) ? 1.f / r.f_coverage : 1.f;

    if (fill_radial && h.h_radial)
        for (float r_hit : r.radial_per_hit)
            h.h_radial->Fill(r_hit, radial_w);
    if (fill_radial && h.h_radial_split)
        for (float r_hit : r.radial_per_hit)
            h.h_radial_split->Fill(r_hit, radial_w);

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
    if (fill_radial && h.h_radial_smeared)
        for (float r_hit : r.radial_per_hit_smeared)
            h.h_radial_smeared->Fill(r_hit, radial_w);
    if (fill_radial && h.h_radial_split_smeared)
        for (float r_hit : r.radial_per_hit_smeared)
            h.h_radial_split_smeared->Fill(r_hit, radial_w);

    if (h.h_residual_vs_n_smeared)
        for (float dev : r.loo_residuals_smeared)
            h.h_residual_vs_n_smeared->Fill(r.n_hits, dev);
    if (h.h_residual_vs_n_split_smeared)
        for (float dev : r.loo_residuals_smeared)
            h.h_residual_vs_n_split_smeared->Fill(r.n_hits, dev);
}

} // namespace btana::recodata
