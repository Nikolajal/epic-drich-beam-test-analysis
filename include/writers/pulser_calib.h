#pragma once

/**
 * @file writers/pulser_calib.h
 * @brief Pulser-driven fine-time calibration pipeline.
 *
 * Reads ALCOR FIFO files directly (no framer, no lightdata.root, no
 * trigger pipeline), runs a per-channel joint closed-form linear
 * least-squares fit against the pulser period, and writes the
 * canonical `fine_calib.txt` (v2 schema, consumed by every downstream
 * `AlcorFinedata::read_calib_from_file`).
 *
 * ### Pipeline
 *
 *   1. `pulser_calib_writer(<repo>, <run>)` walks
 *      @c <repo>/<run>/rdo-NNN/decoded/alcdaq.fifo_M.root and produces:
 *        - @c <repo>/<run>/fine_calib.txt        (v2 schema)
 *        - @c <repo>/<run>/pulser_calib_qa.root  (diagnostic histograms)
 *
 *   2. Downstream writers (`recodata_writer`, `lightdata_writer`)
 *      load `fine_calib.txt` via `AlcorFinedata::read_calib_from_file`.
 *      No change to consumers.
 *
 * ### Method
 *
 * Per channel `(device, chip, eo_channel)`, fit nine unknowns by
 * Cholesky on the symmetric positive-definite normal equations:
 *
 *     unknowns = [θ_0, θ_1, θ_2, θ_3,   // per-TDC offsets (cc)
 *                 a_0, a_1, a_2, a_3,   // per-TDC slopes  (cc/fine bin)
 *                 T]                    // pulser period   (cc)
 *
 * Each consecutive same-spill hit pair contributes one row:
 *
 *     c_curr − c_prev  ≈  T + (θ_curr − θ_prev)
 *                            + (f_curr · a_curr − f_prev · a_prev)
 *
 * Constraints applied to the SPD system:
 *  - **Gauge fix**: pin θ_0 = 0.  Other θ become intra-channel TDC
 *    offsets relative to TDC 0.
 *  - **Slope guard**: TDCs with fewer than `slope_fit_min_fine_span`
 *    distinct fine bins have their slope pinned to
 *    `default_slope_cc_per_bin`.
 *  - **Fixed period**: when `cfg.pulser_period_cc > 0`, pin T at the
 *    operator value (e.g. 320000 cc for 1 kHz at 320 MHz); otherwise
 *    leave T free for a blind detector-level estimate.
 *
 * After the first solve, an **intermittent-slip correction pass**
 * (regime 2) re-snaps hits whose within-pulse phase deviates from the
 * per-(spill, TDC) median by close to an integer cc.  A safety cap
 * aborts the snap when too many candidates fire.  The normal
 * equations are then rebuilt on the slip-corrected coarse and
 * re-solved.  Whole-TDC permanent slip (regime 1) needs no special
 * pass — the fit's natural θ_t absorbs it.
 *
 * **Parallel**: each channel's fit touches only its local stack, no
 * shared state.  `pulser_calib_writer` dispatches channels via
 * `std::async` in chunks bounded by `hardware_concurrency()`.
 *
 * **Fine-band filter at ingest**: hits with `fine` outside
 * `[cfg.fine_min_valid, cfg.fine_max_valid]` (default [20, 160]) are
 * discarded before they enter any per-channel bucket.  See
 * include/writers/DISCUSSION.md.
 *
 * ### Storage
 *
 * `fine_calib.txt` is written as `key method p0 p1 p2` per line where
 *  - `key = GlobalIndex::raw()` (full 32-bit GlobalIndex)
 *  - `method = AlcorV2FitCalib` (= 1)
 *  - `p0 = a` (slope, cc per fine bin)
 *  - `p1 = -b` (negated intercept so downstream's
 *    `get_phase = fine·p0 − p1` recovers `c − (f·a + b)`)
 *  - `p2 = sqrt(chi²/N)` (per-pair residual sigma, cc)
 *
 * Out-of-band fitted slopes (outside `[cfg.slope_min, cfg.slope_max]`)
 * are replaced by `cfg.default_slope_cc_per_bin` at publish time;
 * intercepts outside `[cfg.b_min, cfg.b_max]` are clamped to the
 * nearest edge.  Both clamps are reflected in QA histogram axis
 * ranges.
 */

#include <string>

namespace btana
{

/**
 * @brief Run the pulser-driven fine-time calibration.
 *
 * @param data_repository   Top-level data directory (e.g. `"Data"`).
 * @param run_name          Run sub-directory under @p data_repository.
 *                          Must contain ALCOR FIFO files at
 *                          @c rdo-NNN/decoded/alcdaq.fifo_M.root
 *                          (or @c kc705-NNN/... for KC705 streams).
 * @param calib_config_file Path to the calibration TOML.  Defaults to
 *                          @c conf/calib/calibration_conf.toml.
 * @param force_rebuild     If @c true, overwrite an existing
 *                          `fine_calib.txt`.  When @c false and a
 *                          calibration already exists, the writer logs
 *                          a warning and bails — protects against
 *                          accidental overwrites of a vetted calibration.
 * @param max_spill         Cap on the number of spills processed.
 *                          @c -1 (default) processes the whole run.
 */
void pulser_calib_writer(
    const std::string &data_repository,
    const std::string &run_name,
    const std::string &calib_config_file = "conf/calib/calibration_conf.toml",
    bool force_rebuild = false,
    int max_spill = -1);

} // namespace btana
