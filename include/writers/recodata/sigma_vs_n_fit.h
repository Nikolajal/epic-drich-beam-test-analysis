#pragma once

/**
 * @file sigma_vs_n_fit.h
 * @brief One-parameter LOO σ(N) fit used by the recodata writer's
 *        finalize-QA stage (DISCUSSION § 2.6).
 *
 * Model: σ(N) = σ_photon · √(N / (N − 3))
 *
 * Exact for the LOO residual of a 3-parameter circle fit (cx, cy, R).
 * Each of the three free parameters contributes σ²_photon/(N-1) through
 * the Gauss-Newton hat matrix; the sum is 3σ²/(N-1); the N/(N-1)
 * Bessel-like correction yields N/(N-3).  Valid for any N > 3.  Replaces
 * the earlier degenerate two-parameter `sqrt(A/N + B)` form (with
 * A=3σ²_photon and B=σ²_photon, A=3B always — not independent).
 */

#include <string>
#include <vector>

#include "writers/recodata/types.h"

class TH2F;

namespace btana::recodata {

/**
 * @brief Per-slice Gaussian fit → 1-parameter LOO model fit on @p h2.
 *
 * @param h2                Input TH2F: x = N_hits, y = LOO residual.
 *                          No-op + warning logged if null or empty.
 * @param data_repository   PDF output: `<data_repository>/<run_name>/<h2_name>_sigma_vs_n.pdf`.
 * @param run_name          Same as above.
 * @param results           Append-only collector.  One row pushed if
 *                          the σ(N) fit completes; no row on the
 *                          null/empty / no-slice / one-slice guards.
 *
 * Writes a `<h2_name>_2` TH1F (per-slice σ) to the current `gDirectory`
 * (typically `Rings/`), plus a `<h2_name>_sigma_photon_mm` TNamed scalar
 * for downstream consumers, plus the σ_photon line in the operator log.
 */
void fit_sigma_vs_n(TH2F *h2,
                    const std::string &data_repository,
                    const std::string &run_name,
                    std::vector<VsNFitResult> &results);

} // namespace btana::recodata
