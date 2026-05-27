#pragma once

/**
 * @file frame_pipeline.h
 * @brief Pure per-frame compute helper for the recodata writer's
 *        parallel-dispatch path.
 *
 * `process_frame_pure` is the function dispatched to
 * `std::async(std::launch::async, ...)` from the recodata writer's
 * per-spill loop.  Lifting it to a free function (from its previous
 * in-function `[&]`-captured lambda form) makes the parallel contract
 * **visible in the signature**: it reads `AlcorLightdata` (modulo
 * ROOT's non-const `link_to_tree`-style accessors) and a single
 * `const FrameProcessContext &` that bundles the captured-once state
 * (configs + registry + ring-fit geometry).  Nothing the function
 * touches mutates shared state — the returned `FrameResult` is the
 * sole output, drained serially in frame order on the main thread.
 *
 * The drain side (`drain_frame_result`) remains an in-function lambda
 * in `recodata_writer.cxx`: extracting it would require a ~25-field
 * context struct (output tree, recodata wrapper, per-spill bookkeeping
 * vectors, ~10 trigger-side hists, all the per-ring `RingFillHists`
 * bundles); for a ~100-line sequential `hist->Fill()` list the
 * context-struct overhead exceeds the readability gain.
 */

#include "alcor_data.h"                       // TriggerNumber, HitMask
#include "util/config_reader.h"               // FramerConfigStruct
#include "writers/recodata/ring_compute.h"    // RingComputeContext
#include "writers/recodata/types.h"           // FrameResult

struct TriggerRegistry;
class AlcorLightdata;

namespace btana::recodata {

/**
 * @brief Captured-once state for the parallel per-frame compute.
 *
 * All members are non-owning `const &` to objects that live for the
 * lifetime of the recodata writer's main scope.  Safe to share across
 * worker threads — no mutation paths.
 */
struct FrameProcessContext
{
    const FramerConfigStruct &framer_cfg;
    const TriggerRegistry    &registry;
    const RingComputeContext &ring_ctx;

    /// Edge-rejection guard window applied at spill boundaries [ns].
    /// Default 25 ns matches `BTANA_EDGE_REJECTION_NS`; override only
    /// when an analysis explicitly needs a different cut.
    float edge_rejection_ns = 25.f;
};

/**
 * @brief Pure per-frame compute pass.
 *
 * Reads triggers + Cherenkov hits from @p lightdata, gates on the
 * edge-rejection window + duplicate-trigger checks, runs ring fits
 * via @ref compute_ring_fit if the Hough trigger fired, and packages
 * everything into a `FrameResult` for the serial drain.
 *
 * Mutates nothing in @p ctx; safe to call concurrently from
 * `std::async` worker threads.
 *
 * @param lightdata Per-frame lightdata view.  ROOT-bound accessors
 *                  are non-const in the schema, but the function does
 *                  not modify the underlying data — only reads.
 * @param ctx       Captured-once state (configs + registry + ring fit
 *                  geometry).  See @ref FrameProcessContext.
 * @return          Per-frame compute payload, drained serially.
 */
FrameResult process_frame_pure(AlcorLightdata &lightdata,
                               const FrameProcessContext &ctx);

} // namespace btana::recodata
