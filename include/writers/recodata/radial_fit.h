#pragma once

/**
 * @file radial_fit.h
 * @brief Crystal-Ball + pol3 radial fit used by the recodata writer's
 *        finalize-QA stage
 *
 * Extracted from the in-function `fit_radial_distribution` lambda so
 * the fit recipe can live in its own translation unit, be unit-tested
 * in isolation, and not bloat `src/recodata_writer.cxx`.
 *
 * Algorithm shape (unchanged from the in-function version):
 *   1. Scale `h` to per-ring (divide by `h_R_count->GetEntries()`).
 *   2. Seed the peak by scanning a smoothed copy of `h` inside the
 *      eff(R) acceptance band.
 *   3. Sideband-only pol3 prefit (signal region masked at ±4σ_seed).
 *   4. Two-stage CB+pol3 fit: stage 1 freezes background, stage 2
 *      releases it.  No IMPROVE pass.
 *   5. N_γ per ring from signal-only integral; logs + per-fit PDFs
 *      saved to `<data_repository>/<run_name>/<tag>.pdf` (linear + log).
 *   6. One row pushed into @p results.
 */

#include <string>
#include <vector>

#include "writers/recodata/types.h"
#include "utility/config_reader.h" // RecodataConfigStruct

class TH1F;

namespace btana::recodata
{

/**
 * @brief Crystal-Ball + pol3 radial fit on an eff-corrected radial hist.
 *
 * @param h                 Radial hist (eff-corrected).  Modified in
 *                          place (scaled to per-ring, fit function attached).
 *                          No-op + warning logged if entries < 100.
 * @param h_R_count         Per-event ring-radius hist matching @p h's
 *                          sample (same dual/solo gate).  Only
 *                          `GetEntries()` is read — used to compute
 *                          `N_rings` for the per-ring scaling.  May be
 *                          null (degrades to total-photons headline).
 * @param tag               Stable name used for canvas titles, PDF
 *                          filenames, and the `results` row.  Convention:
 *                          the hist's own name (e.g. `"h_radial_first"`).
 * @param cfg               Recodata config — only the coverage R range
 *                          is read (for the acceptance band).
 * @param data_repository   PDF output: `<data_repository>/<run_name>/<tag>.pdf`.
 * @param run_name          Same as above.
 * @param results           Append-only collector.  One row pushed if
 *                          the fit completes; no row on the entry-count
 *                          guard.
 */
void fit_radial_distribution(TH1F *h,
                             TH1F *h_R_count,
                             const std::string &tag,
                             const RecodataConfigStruct &cfg,
                             const std::string &data_repository,
                             const std::string &run_name,
                             std::vector<RadialFitResult> &results,
                             const std::string &norm_unit = "ring");

} // namespace btana::recodata
