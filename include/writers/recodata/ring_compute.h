#pragma once

/**
 * @file ring_compute.h
 * @brief Per-frame, per-ring compute helpers extracted from the
 *        in-function lambdas of `recodata_writer()` (Phase D of the
 *        modularization pass).
 *
 * The two free functions here form the inner core of the recodata
 * writer's main loop:
 *
 *  - @ref compute_ring_fit_timewindow — pure compute, no shared-state
 *                             mutation; safe to call from worker threads.
 *  - @ref fill_ring_hists   — pure drain, mutates histograms only;
 *                             must run on a single thread.
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
 * @brief Pure-compute single-ring fit by TIME WINDOW.
 *
 * Selects every non-afterpulse cherenkov hit whose `(t_hit − t_ref_ns)`
 * falls in `[dt_min_ns, dt_max_ns]`, runs `fit_circle` for the centre +
 * radius, computes the per-ring radial residual RMS, and (optionally) the
 * leave-one-out residual per hit.  This is how recodata reconstructs rings
 * on hardware-trigger frames when the streaming/Hough self-trigger — which
 * tags the ring hits — is disabled (e.g. QA mode).  The window is
 * asymmetric on purpose (the Cherenkov light sits in a specific Δt band
 * after the trigger).
 *
 * No shared state is mutated — safe to call from worker threads.  Caller
 * pairs the returned @ref RingFitResult with @ref fill_ring_hists in the
 * drain phase.
 *
 * @param t_ref_ns  Hardware-trigger reference time [ns].
 * @param dt_min_ns Lower edge of the acceptance window [ns] (rel. to ref).
 * @param dt_max_ns Upper edge of the acceptance window [ns] (rel. to ref).
 * @param do_loo    When true, run the per-hit leave-one-out fit loop
 *                  (~N extra fit_circle calls).  Gated by the QA path's
 *                  `skip_loo_residuals` knob upstream.
 * @param ctx       Geometry + config bundle.
 */
RingFitResult compute_ring_fit_timewindow(float t_ref_ns,
                                          float dt_min_ns,
                                          float dt_max_ns,
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

} // namespace btana::recodata
