#pragma once

/**
 * @file ring_compute.h
 * @brief Per-frame, per-ring compute helpers extracted from the
 *        in-function lambdas of `recodata_writer()` (Phase D of the
 *        modularization pass).
 *
 * The three free functions here form the inner core of the recodata
 * writer's main loop:
 *
 *  - @ref compute_ring_fit  — pure compute, no shared-state mutation;
 *                             safe to call from worker threads.
 *  - @ref fill_ring_hists   — pure drain, mutates histograms only;
 *                             must run on a single thread.
 *  - @ref refit_and_fill_ring — convenience wrapper that composes the
 *                               two above for the serial code path.
 *
 * State that used to be captured by reference from the parent
 * function's scope is now passed explicitly via @ref RingComputeContext
 * (geometry + config knobs that are set once at writer init).
 */

#include <map>
#include <array>

#include "alcor_data.h"             // HitMask
#include "writers/recodata/types.h" // RingFitResult, RingFillHists
#include "utility/config_reader.h"     // RecodataConfigStruct

class AlcorLightdata;

namespace btana::recodata
{

/**
 * @brief Captured-once state shared across all ring-fit calls.
 *
 * Both members are non-owning references — the caller must keep the
 * referenced objects alive for the lifetime of every function that
 * takes a `RingComputeContext` by const-reference.  In practice the
 * recodata writer's main scope owns both and passes the context to
 * the inner loop.
 */
struct RingComputeContext
{
    /// Channel index → (x, y) position [mm].  Used by the per-ring
    /// `azimuthal_coverage_fraction` calculation.  Built once at
    /// writer init from `Mapping::get_index_to_position_map()`.
    const std::map<int, std::array<float, 2>> &index_to_hit_xy;

    /// Recodata-side config (per-ring photon counting + coverage
    /// thresholds, plus the QA-mode `skip_loo_residuals` knob).
    const RecodataConfigStruct &cfg;
};

/**
 * @brief Pure-compute single-ring fit.
 *
 * Selects hits tagged with @p ring_bit from @p lightdata, runs
 * `fit_circle` for the centre + radius, computes the per-ring radial
 * residual RMS, and (optionally) the leave-one-out residual per hit.
 *
 * No shared state is mutated — safe to call from worker threads
 *  Caller pairs the returned @ref RingFitResult
 * with @ref fill_ring_hists in the drain phase.
 *
 * @param ring_bit  Mask bit selecting which ring's hits to use.
 * @param lightdata Frame's lightdata view (read-only, but the link
 *                  member function isn't `const` in the schema).
 * @param do_loo    When true, run the per-hit leave-one-out fit loop
 *                  (~N extra fit_circle calls).  False ⇒ leave
 *                  `out.loo_residuals` empty.  Gated by the QA path's
 *                  `skip_loo_residuals` knob upstream.
 * @param ctx       Geometry + config bundle.
 */
RingFitResult compute_ring_fit(HitMask ring_bit,
                               AlcorLightdata &lightdata,
                               bool do_loo,
                               const RingComputeContext &ctx);

/**
 * @brief Drain helper: replay the histogram fills implied by a
 *        precomputed @ref RingFitResult into the @p h target bundle.
 *
 * Any pointer in @p h may be nullptr — the corresponding fill is
 * skipped.  Mutates only the passed-in histograms; safe to call
 * iff the caller serializes hist access.
 */
void fill_ring_hists(const RingFitResult &r, const RingFillHists &h);

/**
 * @brief Convenience wrapper for the serial code path: compute then
 *        fill in one call.
 *
 * Internally derives `do_loo` from whether @p h carries a non-null
 * residual histogram AND `ctx.cfg.skip_loo_residuals` is false.
 *
 * @return `true` iff the fit converged.
 */
bool refit_and_fill_ring(HitMask ring_bit,
                         const RingFillHists &h,
                         AlcorLightdata &lightdata,
                         const RingComputeContext &ctx);

} // namespace btana::recodata
