/**
 * @file writers/pulser_calib_writer.cxx
 * @brief Implementation of @ref btana::pulser_calib_writer.
 *
 * See `include/writers/pulser_calib.h` for the algorithmic overview.
 * Architecture (per-channel closed-form least squares, parallel):
 *
 *  - **Reads raw FIFOs directly** via `AlcorDataStreamer`.  No framer,
 *    no `lightdata.root`, no trigger pipeline.
 *
 *  - **Fine-band filter at ingest.**  Hits with `fine` outside
 *    `[cfg.fine_min_valid, cfg.fine_max_valid]` are discarded — ALCOR
 *    fine values typically populate ~[30, 130]; outside is
 *    pathological readout noise that would poison the slope fit.
 *
 *  - **Per-channel joint fit (parallel).**  For each physical channel
 *    `(device, chip, eo_channel)`, build the 9-parameter linear system
 *
 *        unknowns = [θ_0..θ_3, a_0..a_3, T]
 *        row(pair) = T + (θ_curr − θ_prev) + (f_curr·a_curr − f_prev·a_prev)
 *                  ≈ c_curr − c_prev      (consecutive same-spill pair)
 *
 *    over all consecutive same-spill hit pairs whose `|Δc − T_nominal|`
 *    passes the safety filter.  Pin θ_0 = 0 (gauge), pin slopes whose
 *    TDC's fine span is below the guard, pin T at the operator's
 *    fixed period (or leave free), then Cholesky-solve the symmetric
 *    positive-definite normal equations.  No iteration, no
 *    convergence concept, ~µs per channel, ~10-thread parallel via
 *    `std::async` batching.  See `solve_spd<N>` and `pin_parameter<N>`
 *    at the top of this file.
 *
 *  - **Slip correction (regime 2).**  After the first solve, hits
 *    whose within-pulse phase deviates from the per-(spill, TDC)
 *    median by close to an integer cc are re-snapped (intermittent
 *    mid-run slip).  A safety cap aborts the snap when too many
 *    candidates would fire (the per-spill phase distribution is too
 *    wide to trust).  Then the normal equations are rebuilt on the
 *    corrected coarse and re-solved.  Regime 1 (whole-TDC permanent
 *    slip) needs no special pass — the fit's natural θ_t absorbs it
 *    and the published calibration is correct end-to-end.  See
 *    include/writers/DISCUSSION.md for the open questions on the slip
 *    detector and the half-integer satellite mystery.
 *
 *  - **No rollover correction.**  Each channel's hits all come from
 *    one stream; `get_coarse_global_time() = coarse + rollover ·
 *    rollover_period` is exact intrinsically.  The framer's
 *    `resolve_rollover_offsets()` is for cross-stream alignment —
 *    not used here.
 *
 * Output `fine_calib.toml` is the TOML v3 schema (`[[entry]]` table
 * with `key / method / a / minus_b / sigma`); downstream consumers
 * read via `AlcorFinedata::read_calib_from_file`.  The legacy `.txt`
 * format was retired in task #172.
 */

#include "writers/pulser_calib.h"
#include "writers/anchor_dt_canvas.h"
#include "mapping.h"
#include "utility/conf_path.h"

#include "alcor_data.h"
#include "analysis_results.h"
#include "alcor_data_streamer.h"
#include "alcor_finedata.h" // CalibrationMethod enum (v2 schema selector at write time)
#include "utility/config_dump.h"
#include "utility/config_reader.h"
#include "utility/global_index.h"
#include "utility/qa_publish.h"

#include <TCanvas.h>
#include <TF1.h>
#include <TLegend.h>
#include <TFile.h>
#include <TFitResult.h>
#include <TFitResultPtr.h>
#include <TH1F.h>
#include <TH2F.h>
#include <TLatex.h>
#include <TNamed.h>
#include <TPad.h>
#include <TParameter.h>
#include <TString.h>
#include <TStyle.h>

#include <mist/logger/logger.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace btana
{

namespace
{

constexpr double CC_TO_NS = 3.125; ///< 320 MHz → 3.125 ns/cc.  Matches BTANA_ALCOR_CC_TO_NS upstream.
//  Slope-fit guard threshold and fallback slope come from
//  CalibConfigStruct (configurable via conf/calib/calibration_conf.toml).

// ---------------------------------------------------------------------------
//  Channel addressing.  A physical channel is (device, chip, eo_channel).
//  Per channel, the 4 TDCs are distinguished by `tdc ∈ {0,1,2,3}`.
// ---------------------------------------------------------------------------

struct ChannelKey
{
    int device;
    int chip;
    int eo_channel;

    bool operator<(const ChannelKey &o) const noexcept
    {
        return std::tie(device, chip, eo_channel) <
               std::tie(o.device, o.chip, o.eo_channel);
    }
    bool operator==(const ChannelKey &o) const noexcept
    {
        return device == o.device && chip == o.chip && eo_channel == o.eo_channel;
    }
};

struct ChannelHit
{
    int64_t abs_coarse_cc; ///< get_coarse_global_time(), int64 to be safe
    uint8_t fine;          ///< raw fine bin
    uint8_t tdc;           ///< 0..3
};

//  Per-channel data is bucketed by spill: `get_coarse_global_time()`
//  is monotonic within a spill (rollover counter increments cleanly)
//  but jumps unpredictably across spills (different starting rollover
//  reference per stream / spill).  Per-spill fits + cross-spill
//  aggregation handle this without needing cross-spill rollover
//  resolution.
struct SpillBucket
{
    std::vector<ChannelHit> hits;
};
struct ChannelBucket
{
    //  Sparse — only spills where the channel saw at least one hit.
    std::vector<SpillBucket> per_spill;
};

//  Anchor reference signal (e.g. the KC705 testpulse) read out by ALCOR on a
//  dedicated FIFO.  It carries only a coarse counter — `tdc/fine/pixel/column`
//  are all sentinel (-1), so it has NO valid channel ordinal and cannot live
//  in `ChannelBucket` (keyed by device/chip/eo_channel).  Salvaged by
//  (device, fifo) and kept as a per-spill list of anchor-pulse coarse times.
struct AnchorBucket
{
    //  per_spill_coarse[s] = anchor-pulse get_coarse_global_time() values in
    //  spill s, in read (time) order.  The pulse period is ~constant
    //  (the testpulse cadence), so consecutive diffs ≈ the nominal period.
    std::vector<std::vector<int64_t>> per_spill_coarse;
};

// ---------------------------------------------------------------------------
//  Per-channel fit — CLOSED-FORM linear least squares.
//
//  For each consecutive same-spill hit pair (p → h) the residual is
//
//      r = (c_h − c_p) − (θ_{t_h} − θ_{t_p}) − (f_h · a_{t_h} − f_p · a_{t_p}) − T
//
//  which is LINEAR in every unknown (4 offsets θ, 4 slopes a, period T).
//  Per-spill phase cancels in the difference — no mod-T trick, no
//  cross-spill stitching, no rollover correction beyond what
//  `get_coarse_global_time()` already does.
//
//  Stacking one row per pair gives  A · x = y  with
//
//      x = [θ_0, θ_1, θ_2, θ_3, a_0, a_1, a_2, a_3, T]
//      y_k = c_h − c_p             (T column absorbs the constant; or
//                                   y_k = c_h − c_p − T_fixed when T
//                                   is fixed by the operator and the
//                                   T column is dropped)
//
//      A_k[t]    = +1 if t = t_h, −1 if t = t_p     (offsets)
//      A_k[t+4]  = +f_h if t = t_h, −f_p if t = t_p (slopes)
//      A_k[8]    = −1                               (period, when free)
//
//  Solution comes from the normal equations  Aᵀ A x = Aᵀ y, an
//  N×N (N=8 or 9) symmetric positive-definite system in-place
//  Cholesky-solved per channel.  No iteration, no convergence
//  concept, ~µs per channel.  No shared global state → trivially
//  parallel across channels.
//
//  Gauge fix on θ_0 = 0 implemented by zeroing row/col 0 and pinning
//  the diagonal to 1 (constraint x_0 = 0).  Per-TDC slope guard
//  (insufficient fine-bin span) implemented the same way: pin
//  a_t = default_slope, eliminate from the system, re-solve.
//
//  Safety filter on coarse pair gap (only |c_h − c_p − T_nominal| <
//  tolerance contributes) catches missed-pulse jumps, spill-boundary
//  slip-throughs, DAQ glitches.
// ---------------------------------------------------------------------------

//  ── c_h − c_p diagnostic histogram bins ────────────────────────
//  Per-channel arrays of counts populated by fit_channel before the
//  safety filter is applied.  Summed across channels at writer time
//  into ROOT TH1Fs.  Done with raw fixed-size arrays (instead of
//  TH1F per channel) so the per-channel work is allocation-free and
//  the parallel fit loop has no ROOT-object-construction contention.
//
//  ZOOM hist: ±1000 cc around T_nominal, 1 cc bins → shows the
//      tightness of the 1T peak.  Bin index 0 corresponds to
//      T_nominal − 1000.
//  WIDE hist: [0, 5T_nominal], T_nominal/64 ≈ 5000 cc bins → shows
//      multi-T leakage (2T, 3T peaks).
constexpr int CDIFF_ZOOM_BINS = 2001; // [T-1000, T+1000], 1 cc/bin
constexpr int CDIFF_WIDE_BINS = 320;  // [0, 5T], T/64 cc/bin

struct ChannelResult
{
    ChannelKey key;
    double T_cc = 0.0;          ///< fitted pulser period (or fixed value)
    double chi2_per_pair = 0.0; ///< final chi²/N for QA
    long total_hits = 0;
    long n_pairs_used = 0;
    long n_pairs_seen = 0;  ///< pairs visited BEFORE the safety filter
    bool ok = false;        ///< solver succeeded AND ≥1 TDC met threshold
    bool converged = false; ///< Cholesky succeeded (matrix SPD)
    struct PerTdcOut
    {
        bool fitted = false;
        double a = 0.0;       ///< slope cc/bin
        double b = 0.0;       ///< offset cc (b[0] fixed to gauge value)
        double sigma_a = 0.0; ///< placeholder (not yet propagated)
        double sigma_b = 0.0;
        long n_hits = 0;         ///< total hits on this TDC across all spills
        long n_slipped_hits = 0; ///< hits re-snapped by the slip-correction pass
    };
    PerTdcOut tdc[4]{};
    //  Diagnostic histograms — see comment above.
    std::array<long, CDIFF_ZOOM_BINS> cdiff_zoom{};
    std::array<long, CDIFF_WIDE_BINS> cdiff_wide{};
};

//  ── Tiny symmetric positive-definite solver ─────────────────────
//  Cholesky factorisation of the N×N normal-equations matrix
//  (row-major), then forward + back substitution to solve
//  `normal_matrix · solution = rhs`.  Returns false if the matrix
//  is not positive-definite (degenerate / rank-deficient).
//
//  N is small (≤9 here), so the cache-friendly hand-rolled triple
//  loop beats calling out to ROOT/Eigen/BLAS in both runtime and
//  link surface — and the lack of shared global state is what
//  lets the per-channel outer loop go parallel.
template <int N>
inline bool solve_spd(const double normal_matrix[N * N],
                      const double rhs[N],
                      double solution[N])
{
    //  Step 1 — Cholesky factorisation: normal_matrix = L · Lᵀ
    //  with L lower-triangular, stored in `cholesky_lower`.
    double cholesky_lower[N * N] = {};
    for (int row = 0; row < N; ++row)
    {
        for (int col = 0; col <= row; ++col)
        {
            double partial_sum = normal_matrix[row * N + col];
            for (int inner = 0; inner < col; ++inner)
                partial_sum -= cholesky_lower[row * N + inner] *
                               cholesky_lower[col * N + inner];
            if (row == col)
            {
                //  Diagonal must be strictly positive — failure
                //  signals non-positive-definite input (degenerate
                //  / rank-deficient channel).
                if (partial_sum <= 0.0)
                    return false;
                cholesky_lower[row * N + row] = std::sqrt(partial_sum);
            }
            else
                cholesky_lower[row * N + col] =
                    partial_sum / cholesky_lower[col * N + col];
        }
    }

    //  Step 2 — forward substitution: L · forward_subst_result = rhs.
    double forward_subst_result[N];
    for (int row = 0; row < N; ++row)
    {
        double partial_sum = rhs[row];
        for (int inner = 0; inner < row; ++inner)
            partial_sum -= cholesky_lower[row * N + inner] *
                           forward_subst_result[inner];
        forward_subst_result[row] =
            partial_sum / cholesky_lower[row * N + row];
    }

    //  Step 3 — back substitution: Lᵀ · solution = forward_subst_result.
    for (int row = N - 1; row >= 0; --row)
    {
        double partial_sum = forward_subst_result[row];
        for (int inner = row + 1; inner < N; ++inner)
            partial_sum -= cholesky_lower[inner * N + row] * solution[inner];
        solution[row] = partial_sum / cholesky_lower[row * N + row];
    }
    return true;
}

//  Pin the `param_idx`-th unknown at `pinned_value` in a
//  symmetric-positive-definite system `normal_matrix · x = rhs`,
//  preserving SPD after the substitution.  Used for the gauge fix
//  (θ_0 = 0), the slope guard, and the fixed-period case.
//
//  Algorithm: move the pinned column's contribution to the RHS,
//  zero the pinned row and column, set the pinned diagonal to 1,
//  and set rhs[param_idx] = pinned_value.  The solver then returns
//  `solution[param_idx] = pinned_value` and the remaining unknowns
//  consistent with the pin.
template <int N>
inline void pin_parameter(double normal_matrix[N * N], double rhs[N],
                          int param_idx, double pinned_value)
{
    //  1. Move the pinned column's contribution to the RHS.
    for (int row = 0; row < N; ++row)
        rhs[row] -= normal_matrix[row * N + param_idx] * pinned_value;
    //  2. Zero the pinned row...
    for (int col = 0; col < N; ++col)
        normal_matrix[param_idx * N + col] = 0.0;
    //  3. ...and the pinned column.
    for (int row = 0; row < N; ++row)
        normal_matrix[row * N + param_idx] = 0.0;
    //  4. Pin the diagonal so the solver returns `pinned_value`.
    normal_matrix[param_idx * N + param_idx] = 1.0;
    rhs[param_idx] = pinned_value;
}

ChannelResult fit_channel(const ChannelKey &channel_key, ChannelBucket bucket,
                          const CalibConfigStruct &cfg)
{
    ChannelResult result;
    result.key = channel_key;

    //  ── Step 1: hit accounting + slope-guard inputs ────────────────
    //  Count total hits, per-TDC hits, and the per-TDC fine-bin span.
    //  The fine span feeds the slope guard later: a TDC that saw too
    //  few distinct fine bins has no lever arm for its slope fit and
    //  is pinned to the default.
    long hits_per_tdc[4] = {0, 0, 0, 0};
    int fine_min_per_tdc[4] = {256, 256, 256, 256};
    int fine_max_per_tdc[4] = {-1, -1, -1, -1};
    for (auto &spill_bucket : bucket.per_spill)
        for (const auto &hit : spill_bucket.hits)
        {
            ++result.total_hits;
            if (hit.tdc < 4)
            {
                ++hits_per_tdc[hit.tdc];
                const int fine_bin = static_cast<int>(hit.fine);
                if (fine_bin < fine_min_per_tdc[hit.tdc])
                    fine_min_per_tdc[hit.tdc] = fine_bin;
                if (fine_bin > fine_max_per_tdc[hit.tdc])
                    fine_max_per_tdc[hit.tdc] = fine_bin;
            }
        }
    for (int tdc_idx = 0; tdc_idx < 4; ++tdc_idx)
        result.tdc[tdc_idx].n_hits = hits_per_tdc[tdc_idx];

    if (result.total_hits < cfg.min_hits_per_tdc * 4)
        return result;

    //  ── Step 2: pulser-period setup ────────────────────────────────
    //  TOML knob > 0 ⇒ operator pins the period (e.g. 320000 cc for a
    //  1 kHz pulser at 320 MHz).  Otherwise leave the period as a
    //  free 9th fit parameter for a blind detector-level estimate.
    const bool fit_period_from_data = !(cfg.pulser_period_cc > 0.0);
    const double nominal_period_cc =
        cfg.pulser_period_cc > 0.0 ? cfg.pulser_period_cc : 320000.0;

    //  ── Step 3: build the normal-equations system ──────────────────
    //  Parameters (unknowns) of the per-channel fit, in fixed order:
    //      index 0..3   per-TDC offsets    θ_t   (cc)
    //      index 4..7   per-TDC slopes     a_t   (cc per fine bin)
    //      index 8      pulser period      T     (cc)
    //  Total of 9 unknowns — fixed by the architecture.
    constexpr int NUM_FIT_PARAMS = 9;
    static constexpr int OFFSET_PARAM_BASE = 0; // θ_t lives at index t
    static constexpr int SLOPE_PARAM_BASE = 4;  // a_t lives at index t + 4
    static constexpr int PERIOD_PARAM_IDX = 8;
    //  At most 5 unknowns per pair row contribute (2 offsets + 2
    //  slopes + 1 period).  We accumulate Aᵀ·A and Aᵀ·y as a sparse
    //  rank-1 update per pair — O(25) flops per pair, no inner-loop
    //  allocation.
    static constexpr int MAX_NONZEROS_PER_PAIR_ROW = 5;

    double normal_matrix[NUM_FIT_PARAMS * NUM_FIT_PARAMS] = {};
    double rhs_vector[NUM_FIT_PARAMS] = {};
    long pair_count_used = 0;             // pairs that pass the safety filter
    double pair_target_sum_squared = 0.0; // accumulated Σ y² for chi² recovery

    //  build_normal_equations — assemble (normal_matrix, rhs_vector,
    //  pair_count_used, pair_target_sum_squared) from the current
    //  state of bucket.per_spill[*].hits[*].abs_coarse_cc.  Called
    //  twice in the slip-aware path: once on the raw coarse counts
    //  (first pass) and once on the slip-corrected counts (second
    //  pass).  `fill_diag_hists` is true only on the first pass so
    //  the as-observed pair-coarse-difference diagnostic histograms
    //  reflect what the data looked like before any correction.
    auto build_normal_equations = [&](bool fill_diag_hists)
    {
        std::fill_n(normal_matrix, NUM_FIT_PARAMS * NUM_FIT_PARAMS, 0.0);
        std::fill_n(rhs_vector, NUM_FIT_PARAMS, 0.0);
        pair_count_used = 0;
        pair_target_sum_squared = 0.0;
        for (const auto &spill_bucket : bucket.per_spill)
        {
            const auto &hits = spill_bucket.hits;
            if (hits.size() < 2)
                continue;
            for (std::size_t hit_idx = 1; hit_idx < hits.size(); ++hit_idx)
            {
                const auto &prev_hit = hits[hit_idx - 1];
                const auto &curr_hit = hits[hit_idx];
                if (prev_hit.tdc > 3 || curr_hit.tdc > 3)
                    continue;
                const double pair_coarse_diff_cc =
                    static_cast<double>(curr_hit.abs_coarse_cc) -
                    static_cast<double>(prev_hit.abs_coarse_cc);

                //  Diagnostic histograms — filled BEFORE the safety
                //  filter so they show what's being rejected too.
                if (fill_diag_hists)
                {
                    ++result.n_pairs_seen;
                    const double cdiff_relative_to_zoom_lo =
                        pair_coarse_diff_cc - (nominal_period_cc - 1000.0);
                    const int zoom_bin = static_cast<int>(
                        std::floor(cdiff_relative_to_zoom_lo));
                    if (zoom_bin >= 0 && zoom_bin < CDIFF_ZOOM_BINS)
                        ++result.cdiff_zoom[zoom_bin];
                    const double wide_bin_width_cc = nominal_period_cc / 64.0;
                    const int wide_bin = static_cast<int>(
                        std::floor(pair_coarse_diff_cc / wide_bin_width_cc));
                    if (wide_bin >= 0 && wide_bin < CDIFF_WIDE_BINS)
                        ++result.cdiff_wide[wide_bin];
                }

                //  Safety filter: pair must look like one pulser
                //  period.  Multi-T missed-pulse jumps, spill-boundary
                //  leaks, glitches are rejected here.
                if (std::abs(pair_coarse_diff_cc - nominal_period_cc) >
                    cfg.consecutive_pair_tolerance_cc)
                    continue;

                //  Build the sparse A-row for this pair.  The model
                //  says:  pair_coarse_diff_cc =
                //         T + (θ_curr − θ_prev)
                //           + (fine_curr · a_curr − fine_prev · a_prev)
                //  Each contribution is one nonzero column entry.
                int sparse_col_indices[MAX_NONZEROS_PER_PAIR_ROW];
                double sparse_col_values[MAX_NONZEROS_PER_PAIR_ROW];
                int num_nonzeros = 0;

                //  Offsets: contribute only when the two hits are on
                //  different TDCs (otherwise θ_curr − θ_prev = 0).
                if (curr_hit.tdc != prev_hit.tdc)
                {
                    sparse_col_indices[num_nonzeros] =
                        OFFSET_PARAM_BASE + curr_hit.tdc;
                    sparse_col_values[num_nonzeros] = +1.0;
                    ++num_nonzeros;
                    sparse_col_indices[num_nonzeros] =
                        OFFSET_PARAM_BASE + prev_hit.tdc;
                    sparse_col_values[num_nonzeros] = -1.0;
                    ++num_nonzeros;
                }
                //  Slopes: always two entries (+f_curr, -f_prev).
                //  For same-TDC pairs they both fall on the same
                //  slope column and the rank-1 update collapses them
                //  automatically.
                sparse_col_indices[num_nonzeros] =
                    SLOPE_PARAM_BASE + curr_hit.tdc;
                sparse_col_values[num_nonzeros] =
                    +static_cast<double>(curr_hit.fine);
                ++num_nonzeros;
                sparse_col_indices[num_nonzeros] =
                    SLOPE_PARAM_BASE + prev_hit.tdc;
                sparse_col_values[num_nonzeros] =
                    -static_cast<double>(prev_hit.fine);
                ++num_nonzeros;
                //  Period: only when T is a free parameter.  Row
                //  entry −1 because the model has +T on the RHS that
                //  we move to the LHS.
                if (fit_period_from_data)
                {
                    sparse_col_indices[num_nonzeros] = PERIOD_PARAM_IDX;
                    sparse_col_values[num_nonzeros] = -1.0;
                    ++num_nonzeros;
                }

                //  Target y.  When T is free, the period column
                //  absorbs the constant; when fixed, we move T to the
                //  target.
                const double pair_target_cc = fit_period_from_data
                                                  ? pair_coarse_diff_cc
                                                  : (pair_coarse_diff_cc - nominal_period_cc);

                //  Rank-1 update of Aᵀ·A and Aᵀ·y.
                for (int outer = 0; outer < num_nonzeros; ++outer)
                {
                    rhs_vector[sparse_col_indices[outer]] +=
                        sparse_col_values[outer] * pair_target_cc;
                    for (int inner = 0; inner < num_nonzeros; ++inner)
                        normal_matrix[sparse_col_indices[outer] * NUM_FIT_PARAMS +
                                      sparse_col_indices[inner]] +=
                            sparse_col_values[outer] *
                            sparse_col_values[inner];
                }
                pair_target_sum_squared += pair_target_cc * pair_target_cc;
                ++pair_count_used;
            }
        }
    };

    //  ── Step 4: slope-guard mask ────────────────────────────────
    //  Computed from raw hit coverage and stable across the slip
    //  correction step (slip only shifts coarse, never fine).
    bool slope_guard_fired[4] = {false, false, false, false};
    for (int tdc_idx = 0; tdc_idx < 4; ++tdc_idx)
    {
        const int fine_span = (fine_max_per_tdc[tdc_idx] >= fine_min_per_tdc[tdc_idx])
                                  ? (fine_max_per_tdc[tdc_idx] - fine_min_per_tdc[tdc_idx] + 1)
                                  : 0;
        if (fine_span < cfg.slope_fit_min_fine_span)
            slope_guard_fired[tdc_idx] = true;
    }

    //  apply_constraints — pin parameters that the data cannot
    //  determine on its own.  Called after every
    //  build_normal_equations() invocation.
    auto apply_constraints = [&]()
    {
        //  Gauge fix: pin θ_0 = 0.  The other θ values become
        //  intra-channel TDC offsets relative to TDC 0.  Without
        //  this the system has a one-parameter family of global
        //  time shifts.
        pin_parameter<NUM_FIT_PARAMS>(
            normal_matrix, rhs_vector, OFFSET_PARAM_BASE + 0, 0.0);
        //  Slope guards: pin slopes for TDCs with too-narrow fine
        //  coverage to the default.
        for (int tdc_idx = 0; tdc_idx < 4; ++tdc_idx)
            if (slope_guard_fired[tdc_idx])
                pin_parameter<NUM_FIT_PARAMS>(
                    normal_matrix, rhs_vector,
                    SLOPE_PARAM_BASE + tdc_idx,
                    cfg.default_slope_cc_per_bin);
        //  Period: when fixed by the operator we pin parameter 8 at
        //  nominal_period_cc.  Required for SPD — otherwise row 8 of
        //  Aᵀ·A is all zero and Cholesky fails.
        if (!fit_period_from_data)
            pin_parameter<NUM_FIT_PARAMS>(
                normal_matrix, rhs_vector,
                PERIOD_PARAM_IDX, nominal_period_cc);
    };

    //  ── Step 5: first-pass solve ────────────────────────────────
    build_normal_equations(/*fill_diag_hists=*/true);
    result.n_pairs_used = pair_count_used;
    if (pair_count_used == 0)
        return result;
    apply_constraints();
    double fitted_params[NUM_FIT_PARAMS] = {};
    result.converged = solve_spd<NUM_FIT_PARAMS>(
        normal_matrix, rhs_vector, fitted_params);
    if (!result.converged)
        return result;

    //  ── Step 6: intermittent slip correction (regime 2) ─────────
    //  Known ALCOR quirk: some hits arrive with the coarse counter
    //  shifted by an integer number of clock cycles.  Two regimes:
    //
    //    Regime 1 (whole-TDC permanent slip): every hit on a given
    //    TDC is shifted by the same integer.  The fit's natural θ_t
    //    absorbs this — the published calibration is correct without
    //    any extra correction.  No code path needed here.
    //
    //    Regime 2 (intermittent slip mid-run): a fraction of one
    //    TDC's hits are shifted, the rest aren't.  The single-θ fit
    //    averages over both populations and is wrong for both.
    //    Handled here per-hit, per (spill, TDC).
    //
    //  Algorithm per (spill, TDC):
    //    1. For every hit on this TDC in this spill, compute
    //       calib_time_cc = c − (θ_t + fine · a_t), then reduce
    //       modulo the pulser period → within_pulse_phase_cc is the
    //       hit's phase inside one pulser cycle.
    //    2. Median across THIS TDC's hits in THIS spill is the
    //       TDC's "true" phase for this spill.  Per-(spill,TDC)
    //       medianising is essential: pooling across TDCs would
    //       conflate the intrinsic inter-TDC offsets (cable, ALCOR
    //       sampling phase) with real slip events.
    //    3. Per hit, the phase deviation from this median rounds to
    //       an integer cc → the slip in coarse counter units.
    //    4. Snap only if the rounded slip is non-zero and the
    //       deviation sits within `slip_confidence_cc` of that
    //       integer.  Looser confidence ⇒ over-snaps under the
    //       natural ~0.5 cc scatter from sub-optimal slope fits.
    //
    //  Safety cap (`slip_max_snap_fraction`): if more than this
    //  fraction of hits in one (spill, TDC) would snap, the
    //  distribution is too noisy / wide to trust — abort that
    //  (spill, TDC) and leave its hits untouched.
    long slipped_hits_per_tdc[4] = {0, 0, 0, 0};
    {
        const double period_used_cc = fit_period_from_data
                                          ? fitted_params[PERIOD_PARAM_IDX]
                                          : nominal_period_cc;
        const double slip_confidence_thr_cc = cfg.slip_confidence_cc;
        const double max_snap_fraction = cfg.slip_max_snap_fraction;
        constexpr std::size_t MIN_HITS_PER_TDC_FOR_MEDIAN = 4;

        //  Reusable scratch buffers — allocated once, reused per
        //  (spill, TDC).  Outer index is the TDC slot (0..3).
        std::vector<int> hit_indices_per_tdc[4];
        std::vector<double> phase_residuals_per_tdc[4];
        std::vector<double> sorted_phase_residuals_per_tdc[4];
        bool any_hits_were_snapped = false;

        for (auto &spill_bucket : bucket.per_spill)
        {
            if (spill_bucket.hits.empty())
                continue;
            for (int tdc_idx = 0; tdc_idx < 4; ++tdc_idx)
            {
                hit_indices_per_tdc[tdc_idx].clear();
                phase_residuals_per_tdc[tdc_idx].clear();
            }
            //  Pass 1 — bucket hits into 4 per-TDC lists, compute
            //  each hit's within-pulse phase.
            for (std::size_t spill_hit_idx = 0;
                 spill_hit_idx < spill_bucket.hits.size();
                 ++spill_hit_idx)
            {
                const auto &hit = spill_bucket.hits[spill_hit_idx];
                if (hit.tdc > 3)
                    continue;
                const double calib_time_cc =
                    static_cast<double>(hit.abs_coarse_cc) -
                    (fitted_params[OFFSET_PARAM_BASE + hit.tdc] +
                     static_cast<double>(hit.fine) *
                         fitted_params[SLOPE_PARAM_BASE + hit.tdc]);
                const double pulse_number =
                    std::round(calib_time_cc / period_used_cc);
                const double within_pulse_phase_cc =
                    calib_time_cc - pulse_number * period_used_cc;
                hit_indices_per_tdc[hit.tdc].push_back(
                    static_cast<int>(spill_hit_idx));
                phase_residuals_per_tdc[hit.tdc].push_back(
                    within_pulse_phase_cc);
            }
            //  Pass 2 — per TDC, compute the spill's median phase
            //  and snap, with the safety check on snap fraction.
            for (int tdc_idx = 0; tdc_idx < 4; ++tdc_idx)
            {
                if (phase_residuals_per_tdc[tdc_idx].size() < MIN_HITS_PER_TDC_FOR_MEDIAN)
                    continue;

                sorted_phase_residuals_per_tdc[tdc_idx] =
                    phase_residuals_per_tdc[tdc_idx];
                const auto median_iter =
                    sorted_phase_residuals_per_tdc[tdc_idx].begin() +
                    static_cast<std::ptrdiff_t>(
                        sorted_phase_residuals_per_tdc[tdc_idx].size() / 2);
                std::nth_element(
                    sorted_phase_residuals_per_tdc[tdc_idx].begin(),
                    median_iter,
                    sorted_phase_residuals_per_tdc[tdc_idx].end());
                const double phase_median_cc = *median_iter;

                //  First scan — count how many hits would snap
                //  WITHOUT modifying anything.  If the candidate
                //  fraction is over the safety cap, abort this
                //  (spill, TDC) entirely.  Cheap because per-(spill,
                //  TDC) hit counts are small.
                long num_snap_candidates = 0;
                for (std::size_t tdc_local_idx = 0;
                     tdc_local_idx < phase_residuals_per_tdc[tdc_idx].size();
                     ++tdc_local_idx)
                {
                    const double phase_deviation_cc =
                        phase_residuals_per_tdc[tdc_idx][tdc_local_idx] -
                        phase_median_cc;
                    const int slip_cc_count = static_cast<int>(
                        std::round(phase_deviation_cc));
                    if (slip_cc_count != 0 &&
                        std::abs(phase_deviation_cc -
                                 static_cast<double>(slip_cc_count)) <
                            slip_confidence_thr_cc)
                        ++num_snap_candidates;
                }
                const double snap_candidate_fraction =
                    static_cast<double>(num_snap_candidates) /
                    static_cast<double>(phase_residuals_per_tdc[tdc_idx].size());
                if (snap_candidate_fraction > max_snap_fraction)
                    continue; // distribution too wide — leave hits alone

                //  Safe to apply.
                for (std::size_t tdc_local_idx = 0;
                     tdc_local_idx < phase_residuals_per_tdc[tdc_idx].size();
                     ++tdc_local_idx)
                {
                    const double phase_deviation_cc =
                        phase_residuals_per_tdc[tdc_idx][tdc_local_idx] -
                        phase_median_cc;
                    const int slip_cc_count = static_cast<int>(
                        std::round(phase_deviation_cc));
                    if (slip_cc_count != 0 &&
                        std::abs(phase_deviation_cc -
                                 static_cast<double>(slip_cc_count)) <
                            slip_confidence_thr_cc)
                    {
                        const int spill_hit_idx =
                            hit_indices_per_tdc[tdc_idx][tdc_local_idx];
                        spill_bucket.hits[spill_hit_idx].abs_coarse_cc -=
                            static_cast<int64_t>(slip_cc_count);
                        ++slipped_hits_per_tdc[tdc_idx];
                        any_hits_were_snapped = true;
                    }
                }
            }
        }

        //  ── Step 7: second-pass solve (only if anything snapped) ──
        //  Diagnostic histograms NOT refilled — they reflect the
        //  as-observed (pre-correction) pair distribution.
        if (any_hits_were_snapped)
        {
            build_normal_equations(/*fill_diag_hists=*/false);
            result.n_pairs_used = pair_count_used;
            if (pair_count_used == 0)
                return result;
            apply_constraints();
            for (int param_idx = 0; param_idx < NUM_FIT_PARAMS; ++param_idx)
                fitted_params[param_idx] = 0.0;
            result.converged = solve_spd<NUM_FIT_PARAMS>(
                normal_matrix, rhs_vector, fitted_params);
            if (!result.converged)
                return result;
        }

        //  Permanent slip (regime 1) needs no special pass — the
        //  fit's natural θ_t absorbs it.  An earlier implementation
        //  subtracted the integer-rounded θ from every hit on a
        //  given TDC and re-fit, producing a "clean" θ ≈ 0 — but the
        //  published calibration then had no slip information, so
        //  downstream apply at production time (where the slip is
        //  still in the hardware) silently went off by the slip
        //  magnitude.  Removed.

        for (int tdc_idx = 0; tdc_idx < 4; ++tdc_idx)
            result.tdc[tdc_idx].n_slipped_hits = slipped_hits_per_tdc[tdc_idx];
    }

    //  ── Step 8: chi² recovery for the residual-sigma report ──────
    //  In principle chi² = ‖y − A·x‖² = ‖y‖² − xᵀ·Aᵀ·y =
    //  pair_target_sum_squared − xᵀ·rhs_vector_original.
    //  Reconstructing the original rhs_vector (before pin_parameter
    //  modified it) is fiddly; cheaper to walk the pairs once more
    //  and accumulate the residual directly using the solved
    //  fitted_params.  Same O(pair_count_used) cost as the build,
    //  one pass.
    double total_chi2 = 0.0;
    const double period_used_cc = fit_period_from_data
                                      ? fitted_params[PERIOD_PARAM_IDX]
                                      : nominal_period_cc;
    for (const auto &spill_bucket : bucket.per_spill)
    {
        const auto &hits = spill_bucket.hits;
        if (hits.size() < 2)
            continue;
        for (std::size_t hit_idx = 1; hit_idx < hits.size(); ++hit_idx)
        {
            const auto &prev_hit = hits[hit_idx - 1];
            const auto &curr_hit = hits[hit_idx];
            if (prev_hit.tdc > 3 || curr_hit.tdc > 3)
                continue;
            const double pair_coarse_diff_cc =
                static_cast<double>(curr_hit.abs_coarse_cc) -
                static_cast<double>(prev_hit.abs_coarse_cc);
            if (std::abs(pair_coarse_diff_cc - nominal_period_cc) >
                cfg.consecutive_pair_tolerance_cc)
                continue;
            const double prev_calib_time_cc =
                static_cast<double>(prev_hit.abs_coarse_cc) -
                (fitted_params[OFFSET_PARAM_BASE + prev_hit.tdc] +
                 static_cast<double>(prev_hit.fine) *
                     fitted_params[SLOPE_PARAM_BASE + prev_hit.tdc]);
            const double curr_calib_time_cc =
                static_cast<double>(curr_hit.abs_coarse_cc) -
                (fitted_params[OFFSET_PARAM_BASE + curr_hit.tdc] +
                 static_cast<double>(curr_hit.fine) *
                     fitted_params[SLOPE_PARAM_BASE + curr_hit.tdc]);
            const double pair_residual_cc =
                curr_calib_time_cc - prev_calib_time_cc - period_used_cc;
            total_chi2 += pair_residual_cc * pair_residual_cc;
        }
    }
    result.chi2_per_pair =
        total_chi2 / static_cast<double>(pair_count_used);
    result.T_cc = period_used_cc;

    //  ── Step 9: publish per-TDC parameters ────────────────────────
    //  A TDC is published only if it saw at least `min_hits_per_tdc`
    //  hits.  Below-threshold TDCs are left unfitted and end up in
    //  the run-summary skipped list.
    for (int tdc_idx = 0; tdc_idx < 4; ++tdc_idx)
    {
        if (hits_per_tdc[tdc_idx] < cfg.min_hits_per_tdc)
            continue;
        result.tdc[tdc_idx].fitted = true;
        result.tdc[tdc_idx].b =
            fitted_params[OFFSET_PARAM_BASE + tdc_idx];
        result.tdc[tdc_idx].a = slope_guard_fired[tdc_idx]
                                    ? cfg.default_slope_cc_per_bin
                                    : fitted_params[SLOPE_PARAM_BASE + tdc_idx];
        //  Per-TDC residual sigma is not propagated yet — the v2
        //  consumers don't need it.  Computing it would require the
        //  per-parameter variance from the Aᵀ·A inverse diagonal,
        //  feasible but not done here.
        result.tdc[tdc_idx].sigma_a = 0.0;
        result.tdc[tdc_idx].sigma_b = 0.0;
        result.ok = true;
    }
    return result;
}

// ---------------------------------------------------------------------------
//  Enumerate FIFO files in a run directory.
//  Pattern: <run_dir>/rdo-NNN/decoded/alcdaq.fifo_M.root
//  Also handles kc705-NNN/decoded/alcdaq.fifo_M.root.
// ---------------------------------------------------------------------------
std::vector<std::string> enumerate_fifos(const std::string &run_dir)
{
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    if (!fs::exists(run_dir))
        return out;
    for (const auto &rdo : fs::directory_iterator(run_dir))
    {
        if (!rdo.is_directory())
            continue;
        const auto rdo_name = rdo.path().filename().string();
        //  rdo-* or kc705-* — the only two RDO flavours present today.
        if (rdo_name.rfind("rdo-", 0) != 0 && rdo_name.rfind("kc705-", 0) != 0)
            continue;
        const fs::path decoded = rdo.path() / "decoded";
        if (!fs::exists(decoded))
            continue;
        for (const auto &f : fs::directory_iterator(decoded))
        {
            const auto name = f.path().filename().string();
            //  alcdaq.fifo_*.root, exclude .dcr.root etc.
            if (name.rfind("alcdaq.fifo_", 0) != 0)
                continue;
            if (name.size() < 5 || name.substr(name.size() - 5) != ".root")
                continue;
            if (name.find(".dcr.") != std::string::npos)
                continue;
            out.push_back(f.path().string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

void pulser_calib_writer(
    const std::string &data_repository,
    const std::string &run_name,
    const std::string &calib_config_file,
    bool force_rebuild,
    int max_spill,
    int anchor_device_override,
    int anchor_chip_override,
    int anchor_eo_channel_override,
    int anchor_fifo_override,
    double pulser_period_cc_override)
{
    namespace fs = std::filesystem;

    const std::string run_dir = data_repository + "/" + run_name;
    const std::string qa_root_path = run_dir + "/pulser_calib_qa.root";

    mist::logger::info("(pulser_calib_writer) === START ===");
    mist::logger::info("(pulser_calib_writer) run_dir = " + run_dir);

    //  ── Config + IO resolution ────────────────────────────────────
    auto cfg = calib_conf_reader(calib_config_file);
    if (force_rebuild)
        cfg.force_rebuild = true;

    //  CLI-level anchor overrides (≥0 wins over TOML).  Plumbed for the
    //  Run Manager card so an operator can flip the anchor channel
    //  per-launch — e.g. when 192/0/ch0 fails mid-shift — without
    //  rewriting the persistent calibration_conf.toml that may be
    //  shared across runs.  Sentinel -1 means "don't override".
    //  The eventual cfg.anchor_* values are dumped into the QA file's
    //  Config/ folder downstream, so reproducibility is preserved.
    if (anchor_device_override >= 0 && anchor_device_override != cfg.anchor_device)
    {
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) CLI override: anchor_device %d -> %d",
                               cfg.anchor_device, anchor_device_override)
                               .Data());
        cfg.anchor_device = anchor_device_override;
    }
    if (anchor_chip_override >= 0 && anchor_chip_override != cfg.anchor_chip)
    {
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) CLI override: anchor_chip %d -> %d",
                               cfg.anchor_chip, anchor_chip_override)
                               .Data());
        cfg.anchor_chip = anchor_chip_override;
    }
    if (anchor_eo_channel_override >= 0 && anchor_eo_channel_override != cfg.anchor_eo_channel)
    {
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) CLI override: anchor_eo_channel %d -> %d",
                               cfg.anchor_eo_channel, anchor_eo_channel_override)
                               .Data());
        cfg.anchor_eo_channel = anchor_eo_channel_override;
    }
    if (anchor_fifo_override >= 0 && anchor_fifo_override != cfg.anchor_fifo)
    {
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) CLI override: anchor_fifo %d -> %d",
                               cfg.anchor_fifo, anchor_fifo_override)
                               .Data());
        cfg.anchor_fifo = anchor_fifo_override;
    }
    //  Pulser period override: dashboard hands us pulser frequency in
    //  Hz, the CLI driver converts to ``cc`` via the 320 MHz clock
    //  before reaching this fn, so we get a straight cc value to
    //  drop into cfg.  Sentinel -1.0 = "no override" (use TOML).
    //  Note: 0.0 is a *valid* override — it means "fit per channel"
    //  (the existing TOML semantic) — so the gate is strictly ``< 0``.
    if (pulser_period_cc_override >= 0.0 && pulser_period_cc_override != cfg.pulser_period_cc)
    {
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) CLI override: pulser_period_cc %.3f -> %.3f",
                               cfg.pulser_period_cc, pulser_period_cc_override)
                               .Data());
        cfg.pulser_period_cc = pulser_period_cc_override;
    }
    const auto resolved = resolve_fine_calib_path(cfg, run_dir);
    switch (resolved.kind)
    {
    case CalibPathResolution::Override:
        mist::logger::warning("(pulser_calib_writer) override_path is set and resolves to '" +
                              resolved.path + "'.  Producing a new calibration would not be read " +
                              "by consumers as long as override_path remains set.  Bailing — " +
                              "either clear override_path or pass --force-rebuild.");
        return;
    case CalibPathResolution::Default:
        mist::logger::warning("(pulser_calib_writer) default path '" + resolved.path +
                              "' already exists.  Pass --force-rebuild to overwrite.  Bailing.");
        return;
    case CalibPathResolution::MissingNeedsRebuild:
        mist::logger::info("(pulser_calib_writer) no existing calibration — building.");
        break;
    case CalibPathResolution::ForceRebuildRequested:
        mist::logger::info("(pulser_calib_writer) --force-rebuild — recomputing calibration.");
        break;
    }
    const std::string fine_calib_path = resolved.path;

    //  ── Phase 1a: serial FIFO read, bucket per channel ────────────
    const auto fifo_paths = enumerate_fifos(run_dir);
    if (fifo_paths.empty())
    {
        mist::logger::error("(pulser_calib_writer) no FIFO files found under " + run_dir);
        return;
    }
    mist::logger::info("(pulser_calib_writer) found " + std::to_string(fifo_paths.size()) +
                       " FIFO files; reading + bucketing per channel");

    std::map<ChannelKey, ChannelBucket> channels;
    AnchorBucket anchor;        //  salvaged (device, anchor_fifo) reference pulses
    long total_anchor_hits = 0; //  count of salvaged anchor pulses
    long total_hits_read = 0;
    long total_hits_rejected_fine_oob = 0; //  fine bin outside [fine_min_valid, fine_max_valid]
    int spills_seen = 0;
    const int fine_lo = cfg.fine_min_valid;
    const int fine_hi = cfg.fine_max_valid;

    for (const auto &path : fifo_paths)
    {
        AlcorDataStreamer alcor_stream(path);
        if (!alcor_stream.is_valid())
        {
            mist::logger::warning("(pulser_calib_writer) cannot open FIFO " + path + " — skipping");
            continue;
        }
        int spill_idx = -1; //  bumped to 0 on the first is_start_spill()
        while (alcor_stream.read_next())
        {
            const auto &alcor_hit = alcor_stream.current();
            if (alcor_hit.is_start_spill())
            {
                ++spill_idx;
                if (path == fifo_paths.front()) //  count spills via the first FIFO
                    ++spills_seen;
                if (max_spill > 0 && spill_idx >= max_spill)
                    break;
                continue;
            }
            if (spill_idx < 0)
                continue; //  hits before the first start_spill (rare; defensive)
            if (max_spill > 0 && spill_idx >= max_spill)
                continue;

            //  ── Anchor salvage ───────────────────────────────────────────
            //  The pulsed reference (e.g. KC705 testpulse) is read out on a
            //  dedicated FIFO with tdc/fine/pixel/column all = -1.  It has no
            //  valid channel ordinal and would be dropped by the tdc/fine
            //  filters and the (device,chip,eo_channel) keying below.  Catch
            //  it FIRST by (device, fifo) and keep its coarse-global time as
            //  the per-spill anchor reference.  Active only when configured.
            //  The anchor PULSES are `trigger_tag` (type 9); the same FIFO
            //  also carries start_spill (7) / end_spill (15) markers, which
            //  must NOT be salvaged as pulses.  Require trigger_tag so a
            //  markers-only run salvages 0 (not a stray end-marker).
            if (cfg.anchor_fifo >= 0 &&
                alcor_hit.get_device() == cfg.anchor_device &&
                alcor_hit.get_fifo() == cfg.anchor_fifo &&
                alcor_hit.is_trigger_tag())
            {
                if (static_cast<int>(anchor.per_spill_coarse.size()) <= spill_idx)
                    anchor.per_spill_coarse.resize(spill_idx + 1);
                anchor.per_spill_coarse[spill_idx].push_back(
                    static_cast<int64_t>(alcor_hit.get_coarse_global_time()));
                ++total_anchor_hits;
                continue;
            }

            const int tdc_idx = alcor_hit.get_tdc();
            if (tdc_idx < 0 || tdc_idx > 3)
                continue;

            //  Fine-band filter: ALCOR fine bins typically populate
            //  ~[30, 130]; values outside `[fine_min_valid,
            //  fine_max_valid]` are pathological readout artefacts
            //  (early pickup, end-of-cycle wrap, noise spikes) and
            //  should not enter the fit.  Discard here at ingest so
            //  no downstream stage even sees them.  Tracked in
            //  include/writers/DISCUSSION.md as a pragmatic safety;
            //  the design ideal is for the fit to identify these as
            //  outliers naturally without an explicit filter.
            const int fine_value = alcor_hit.get_fine();
            if (fine_value < fine_lo || fine_value > fine_hi)
            {
                ++total_hits_rejected_fine_oob;
                continue;
            }

            ChannelKey channel_key{alcor_hit.get_device(),
                                   alcor_hit.get_chip(),
                                   alcor_hit.get_eo_channel()};
            ChannelHit channel_hit;
            channel_hit.abs_coarse_cc =
                static_cast<int64_t>(alcor_hit.get_coarse_global_time());
            channel_hit.fine = static_cast<uint8_t>(fine_value);
            channel_hit.tdc = static_cast<uint8_t>(tdc_idx);

            auto &bucket = channels[channel_key];
            //  Grow per-spill vector lazily — sparsity is okay
            //  (empty SpillBuckets cost almost nothing).
            if (static_cast<int>(bucket.per_spill.size()) <= spill_idx)
                bucket.per_spill.resize(spill_idx + 1);
            bucket.per_spill[spill_idx].hits.push_back(channel_hit);
            ++total_hits_read;
        }
    }
    mist::logger::info("(pulser_calib_writer) ingested " + std::to_string(total_hits_read) +
                       " hits across " + std::to_string(channels.size()) + " channels");
    if (cfg.anchor_fifo >= 0)
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) salvaged %ld anchor pulses from "
                               "(device=%d, fifo=%d) across %zu spills",
                               total_anchor_hits, cfg.anchor_device, cfg.anchor_fifo,
                               anchor.per_spill_coarse.size())
                               .Data());
    if (total_hits_rejected_fine_oob > 0)
    {
        const double rejected_frac =
            static_cast<double>(total_hits_rejected_fine_oob) /
            static_cast<double>(total_hits_read + total_hits_rejected_fine_oob);
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) fine-band filter: rejected %ld hits with fine outside [%d, %d] (%.4f%% of read)",
                               total_hits_rejected_fine_oob,
                               fine_lo, fine_hi, 100.0 * rejected_frac)
                               .Data());
    }

    if (channels.empty())
    {
        mist::logger::error("(pulser_calib_writer) no hits read — bailing");
        return;
    }

    //  ── Anchor-Δ diagnostic: TH2F (spill, channel.cc − anchor.cc) ──
    //  Tracks how each channel's coarse counter drifts away from the
    //  anchor (pulser reference) per spill.  Pulser fires once per
    //  period across all channels, so for an in-spec channel the
    //  difference should sit at 0 ± slipping-tolerance.  Excursions
    //  by ±1 rollover (= BTANA_ALCOR_ROLLOVER_TO_CC = 32768 cc) flag
    //  channels whose coarse counter wrapped relative to the anchor —
    //  exactly the population the operator wants to spot.
    //
    //  Filled *before* the parallel fit moves the per-channel buckets
    //  out of `channels` (line ~1009 below).  All channels go onto a
    //  single histogram so a one-shot read of the plot tells the
    //  operator "every channel is happy" or "channel X drifted".
    //
    //  Index-based pair matching per spill: anchor.hits[i] is taken
    //  to correspond to channel.hits[i] (one hit per pulser shot is
    //  the calibration assumption).  Length mismatches truncate to
    //  the shorter list — defensive; channels that missed a pulse
    //  still contribute their matched fraction.
    constexpr int kRollover = BTANA_ALCOR_ROLLOVER_TO_CC;
    //  Variable-bin Y axis — see ``util::qa::make_anchor_dt_y_edges``
    //  for the layout.  93× smaller than the uniform 65 737-bin
    //  pattern, and the two "gap" bins make the off-canvas count
    //  trivial to read.  Shared with lightdata_writer so the
    //  canvas helper assumes one bin layout.
    const auto kAnchorYEdges = util::qa::make_anchor_dt_y_edges(kRollover);
    //  C5.6 — guard against `spills_seen == 0`.  Without the clamp the
    //  X axis becomes `[-0.5, -0.5]` (zero-width), which TH2F accepts
    //  silently but produces NaN/Inf bin edges that propagate into the
    //  diagnostic file.  When no spills were seen we keep a single-bin
    //  placeholder so the histogram exists (empty) without booby-trapping
    //  downstream readers.  Re-applied after the pulser
    //  refactor restored the bug pattern.
    const int anchor_n_x_bins = std::max(1, spills_seen);
    const double anchor_x_hi = (spills_seen > 0)
                                   ? (spills_seen - 0.5)
                                   : 0.5;
    auto h_anchor_dt_vs_spill = std::make_unique<TH2F>(
        "h_anchor_dt_vs_spill",
        //  TLatex codes (#Delta etc.) so the title renders properly —
        //  raw UTF-8 in TString::Format gets mangled by ROOT's text
        //  rendering on most fonts.
        TString::Format(
            //  ``#Deltat_{trg}`` — the value is a coarse-counter
            //  difference (cc units, formula c_ch − c_anchor) but
            //  operators read it as a trigger-relative time gap, so
            //  the subscript ``trg`` makes the per-trigger nature
            //  explicit while the X axis remains ``spill`` (the bin
            //  index is per-spill).
            "#Deltat_{trg} vs spill (channel - anchor [%d/%d/ch%d]);"
            "spill;#Deltat_{trg} (cc)  = c_{ch} - c_{anchor}",
            cfg.anchor_device, cfg.anchor_chip, cfg.anchor_eo_channel),
        anchor_n_x_bins, -0.5, anchor_x_hi,
        static_cast<int>(kAnchorYEdges.size() - 1),
        kAnchorYEdges.data());
    // Keep the histogram off the global ROOT directory list so the
    // automatic .root write at qa->Write() doesn't pick it up — we
    // place it explicitly under Diagnostics/ later.
    h_anchor_dt_vs_spill->SetDirectory(nullptr);

    //  1D companion: the same channel−anchor Δt collapsed over spill —
    //  the coincidence distribution.  A peak near 0 means the channel
    //  hits are anchored to the reference; a flat band means they are
    //  not (free-running anchor).  Fixed 1-cc binning over the main
    //  coincidence window [−250, 250] cc (matches the 2D's centre pad).
    //  Channel−anchor Δt: 1D distribution + coincidence map.  The Δt range
    //  is set by the nearest-pulse window = ±period/2, so it MUST scale with
    //  the pulser rate (±160 cc at 1 MHz, ±16000 cc at 10 kHz) — a fixed
    //  FIXED ±250 cc histogram (1-cc bins).  The coincidence peak is brought
    //  INTO this window by subtracting a delay (cfg.anchor_delay_cc, mimicking
    //  the trigger setup) rather than chasing the peak with an adaptive range.
    //  dt_win is the FULL nearest-pulse range (±period/2), used only to store
    //  Δt for the peak/delay measurement + the per-pixel coincidence map.
    auto h_anchor_dt_1d = std::make_unique<TH1F>(
        "h_anchor_dt_1d",
        "calibration anchor #Deltat (channel #minus anchor #minus delay);"
        "#Deltat (cc)  = c_{ch} #minus c_{anchor} #minus delay;hits",
        201, -100.5, 100.5);
    h_anchor_dt_1d->SetDirectory(nullptr);
    int dt_win = 250; // full nearest-pulse half-range (cc); set from period below

    //  Coincidence hitmap (FIFO/laser mode): per-pixel count of hits inside
    //  the per-pixel coincidence window → lights up the laser spot.  Rendered
    //  as PDF 08.
    std::unique_ptr<TH2F> h_coinc_map;
    double coinc_shift_cc = 0.0;    // measured average peak position (cc)
    double anchor_delay_used = 0.0; // delay actually subtracted (cc)

    //  ── Consecutive anchor-pulse Δt (the pulse cadence) ───────────────
    //  The anchor's OWN cadence: the time between consecutive salvaged
    //  pulses (coarse[i] − coarse[i-1]).  For a healthy pulser this is a
    //  tight GAUSSIAN at the pulse period; its mean is the average pulse
    //  RATE (= 320 MHz clock / period_cc) and its width is the cadence
    //  jitter.  Missed pulses produce 2×/3× satellite peaks, which we
    //  exclude by windowing the fit to ±30 % of the (robust) median
    //  period.  Only meaningful in FIFO-salvage mode (the pulse train).
    std::unique_ptr<TH1F> h_anchor_consecutive_dt;
    double anchor_period_cc = 0.0;
    double anchor_jitter_cc = 0.0;
    double anchor_rate_hz = 0.0;
    long n_anchor_consecutive_filled = 0;
    if (cfg.anchor_fifo >= 0)
    {
        std::vector<double> diffs;
        for (const auto &pulses : anchor.per_spill_coarse)
            for (size_t i = 1; i < pulses.size(); ++i)
                diffs.push_back(
                    static_cast<double>(pulses[i] - pulses[i - 1]));
        if (!diffs.empty())
        {
            //  Robust period seed = median (immune to the missed-pulse
            //  2×/3× tail that would drag a plain mean upward).
            auto mid = diffs.begin() +
                       static_cast<std::ptrdiff_t>(diffs.size() / 2);
            std::nth_element(diffs.begin(), mid, diffs.end());
            const double med = *mid;
            const double lo = 0.7 * med, hi = 1.3 * med;
            h_anchor_consecutive_dt = std::make_unique<TH1F>(
                "h_anchor_consecutive_dt",
                "consecutive anchor-pulse #Deltat;"
                "#Deltat (cc)  = c_{i} #minus c_{i-1};pulses",
                200, lo, hi);
            h_anchor_consecutive_dt->SetDirectory(nullptr);
            for (double d : diffs)
                if (d >= lo && d < hi)
                {
                    h_anchor_consecutive_dt->Fill(d);
                    ++n_anchor_consecutive_filled;
                }
            //  Period = the robust MEDIAN (immune to missed-pulse
            //  sub-populations that can pull a Gaussian fit to a wrong
            //  sub-peak — observed on multi-rate runs).  The Gaussian only
            //  refines the JITTER (σ), and only when it lands on the median
            //  (else keep the histogram RMS).
            anchor_period_cc = med;
            anchor_jitter_cc = h_anchor_consecutive_dt->GetRMS();
            if (h_anchor_consecutive_dt->GetEntries() > 0)
            {
                h_anchor_consecutive_dt->Fit("gaus", "Q0");
                if (auto *fn = h_anchor_consecutive_dt->GetFunction("gaus"))
                {
                    if (std::abs(fn->GetParameter(1) - med) < 0.05 * med)
                        anchor_jitter_cc = std::abs(fn->GetParameter(2));
                    else
                        //  Fit landed off the median (missed-pulse sub-peak);
                        //  drop it so the 06 plot doesn't draw a misleading line.
                        h_anchor_consecutive_dt->GetListOfFunctions()->Remove(fn);
                }
            }
            //  Average rate: convert the coarse-count period to NS first
            //  (period_ns = period_cc · CC_TO_NS), then rate = 1 / period.
            const double anchor_period_ns = anchor_period_cc * CC_TO_NS;
            anchor_rate_hz =
                (anchor_period_ns > 0.0) ? 1.0e9 / anchor_period_ns : 0.0;
            mist::logger::info(
                TString::Format(
                    "(pulser_calib_writer) anchor cadence: period = %.2f cc "
                    "(%.1f ns), jitter #sigma = %.2f cc, average rate = "
                    "%.4g Hz (%.4f MHz) from %ld in-window diffs",
                    anchor_period_cc, anchor_period_cc * CC_TO_NS,
                    anchor_jitter_cc, anchor_rate_hz, anchor_rate_hz / 1e6,
                    n_anchor_consecutive_filled)
                    .Data());

            //  Auto-derive the pulser period from the MEASURED anchor
            //  cadence.  The external pulser drives both the FIFO anchor
            //  and the laser, so the anchor's consecutive-Δt period IS the
            //  channel pulse period.  Without this the fit keeps the 1 kHz
            //  (320000 cc) TOML default, and the consecutive-pair selector
            //  (|Δc − pulser_period_cc| < tol, line ~453) rejects every
            //  real 1 MHz (320 cc) pair → no coincident pairs → empty fit.
            //  An explicit --pulser-frequency-hz (override ≥ 0) still wins.
            if (pulser_period_cc_override < 0.0 && anchor_period_cc > 0.0)
            {
                const double measured = std::round(anchor_period_cc);
                if (measured != cfg.pulser_period_cc)
                {
                    mist::logger::info(TString::Format(
                                           "(pulser_calib_writer) pulser_period_cc auto-set from "
                                           "anchor cadence: %.0f -> %.0f cc (%.4f MHz); was the "
                                           "TOML/default value",
                                           cfg.pulser_period_cc, measured,
                                           (measured > 0 ? 320.0e6 / measured / 1e6 : 0.0))
                                           .Data());
                    cfg.pulser_period_cc = measured;
                }
            }
        }
    }

    //  Δt STORAGE range = ±period/2 (the full nearest-pulse range) so the
    //  peak/delay can be measured wherever it sits.  The DISPLAY histogram
    //  stays the fixed ±250 cc created above; the peak is shifted into it by
    //  the delay.  Capped at int16 for the per-pixel store.
    if (cfg.anchor_fifo >= 0 && anchor_period_cc > 1.0)
        dt_win = std::min(32000,
                          std::max(250,
                                   static_cast<int>(
                                       std::round(anchor_period_cc / 2.0))));

    long n_anchor_pairs_filled = 0;
    if (cfg.anchor_fifo >= 0 && !anchor.per_spill_coarse.empty())
    {
        //  ── FIFO-salvage anchor ──────────────────────────────────────
        //  The reference is the salvaged (device, anchor_fifo) pulse train
        //  (e.g. KC705 testpulse), not an addressable channel.  Reference
        //  EACH channel hit to the NEAREST anchor pulse in the same spill:
        //  Δt = channel_coarse − nearest_anchor_coarse.  The per-spill
        //  anchor coarse list is strictly monotonic (rollover increments
        //  cleanly → get_coarse_global_time() never decreases; verified),
        //  so it is already sorted and a binary search finds the nearest.
        //  Per-pixel Δt store (chip, eo_channel) → in-window Δt values, used
        //  after the loop to build the coincidence hitmap (laser spot).
        std::map<ChannelKey, std::vector<int16_t>> ch_dt_for_map;
        //  Flat (spill, Δt) store for the Δt-vs-spill 2D — filled post-loop
        //  with the delay subtracted (same recentring as the 1D).
        std::vector<std::pair<int16_t, int16_t>> dt2d;
        for (const auto &[ch_key, ch_bucket] : channels)
        {
            const int n_spills = static_cast<int>(std::min(
                anchor.per_spill_coarse.size(), ch_bucket.per_spill.size()));
            for (int s = 0; s < n_spills; ++s)
            {
                const auto &anchors = anchor.per_spill_coarse[s];
                if (anchors.empty())
                    continue;
                auto &dt_store = ch_dt_for_map[ch_key];
                for (const auto &ch_hit : ch_bucket.per_spill[s].hits)
                {
                    const int64_t tc = ch_hit.abs_coarse_cc;
                    const auto it =
                        std::lower_bound(anchors.begin(), anchors.end(), tc);
                    int64_t nearest;
                    if (it == anchors.begin())
                        nearest = anchors.front();
                    else if (it == anchors.end())
                        nearest = anchors.back();
                    else
                    {
                        const int64_t hi = *it, lo = *(it - 1);
                        nearest = (tc - lo <= hi - tc) ? lo : hi;
                    }
                    const int64_t dt = tc - nearest;
                    //  Both the 2D (Δt vs spill) and the 1D are filled
                    //  post-loop AFTER the delay is known, so the peak is
                    //  recentred into the window in both.  Stash full-range
                    //  Δt here (per-pixel for the map, flat for the 2D).
                    if (dt >= -dt_win && dt <= dt_win)
                    {
                        dt_store.push_back(static_cast<int16_t>(dt));
                        dt2d.emplace_back(static_cast<int16_t>(s),
                                          static_cast<int16_t>(dt));
                    }
                    ++n_anchor_pairs_filled;
                }
            }
        }
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) anchor-Δ diagnostic filled with %ld "
                               "(channel hit, nearest FIFO-%d anchor pulse) pairs across %d spills",
                               n_anchor_pairs_filled, cfg.anchor_fifo, spills_seen)
                               .Data());

        //  ── Coincidence hitmap (laser spot) ──────────────────────────
        //  The coincidence peak is NARROW but SHIFTED by N cc, and the shift
        //  can differ per pixel (laser/cable delay) — which is exactly why
        //  the all-channel sum looks flat.  So detect the peak PER PIXEL:
        //  for each pixel build its own Δt histogram, take the tallest bin,
        //  and map the EXCESS over that pixel's own DCR floor inside ±kHalf
        //  cc of its peak.  A laser-lit pixel shows a sharp peak (large,
        //  significant excess); a DCR-only pixel is flat (≈ 0).  The map
        //  therefore reveals the illuminated spot independent of any global
        //  alignment.
        if (!ch_dt_for_map.empty())
        {
            constexpr int kHalf = 2;   // ± window (cc) around each peak
            const int kRange = dt_win; // Δt confined to ±period/2
            const int kNb = 2 * kRange + 1;
            //  PHYSICAL detector map: each lit pixel placed at its real (x, y)
            //  in mm via the channel→position Mapping (same geometry the
            //  lightdata hitmaps use: 396×396 over ±99 mm).
            Mapping pixmap(util::conf_path("mapping_conf.toml", std::string{}));
            h_coinc_map = std::make_unique<TH2F>(
                "h_coinc_map",
                TString::Format(
                    "coincidence hitmap (device %d, FIFO %d) — hits in the "
                    "per-pixel coincidence window;x (mm);y (mm)",
                    cfg.anchor_device, cfg.anchor_fifo),
                396, -99, 99, 396, -99, 99);
            h_coinc_map->SetDirectory(nullptr);

            long n_lit = 0, n_unmapped = 0;
            std::vector<double> lit_shifts;
            for (const auto &[key, dts] : ch_dt_for_map)
            {
                if (dts.size() < 50)
                    continue;
                std::vector<int> cnt(kNb, 0);
                for (int16_t d : dts)
                    if (d >= -kRange && d <= kRange)
                        ++cnt[d + kRange];
                const int peak = static_cast<int>(
                    std::max_element(cnt.begin(), cnt.end()) - cnt.begin());
                const double bg =
                    static_cast<double>(dts.size()) / kNb; // uniform/bin
                long s = 0;
                for (int b = std::max(0, peak - kHalf);
                     b <= std::min(kNb - 1, peak + kHalf); ++b)
                    s += cnt[b];
                const double nbins = 2 * kHalf + 1;
                const double excess = s - nbins * bg;
                //  Significance (excess over the DCR floor) only DECIDES which
                //  pixels are lit; the value plotted is the RAW hit count in
                //  the coincidence window (no DCR subtraction).
                if (excess > 5.0 * std::sqrt(nbins * bg + 1.0))
                {
                    const auto xy = pixmap.get_position_from_device_chip_eoch(
                        key.device, key.chip, key.eo_channel);
                    if (xy)
                        h_coinc_map->Fill((*xy)[0], (*xy)[1],
                                          static_cast<double>(s));
                    else
                        ++n_unmapped;
                    lit_shifts.push_back(peak - kRange);
                    ++n_lit;
                }
            }
            if (n_unmapped > 0)
                mist::logger::warning(TString::Format(
                                          "(pulser_calib_writer) coincidence map: %ld lit pixels had "
                                          "no (x,y) in mapping_conf.toml (unplaced)",
                                          n_unmapped)
                                          .Data());
            double med_shift = 0.0;
            if (!lit_shifts.empty())
            {
                auto m = lit_shifts.begin() + lit_shifts.size() / 2;
                std::nth_element(lit_shifts.begin(), m, lit_shifts.end());
                med_shift = *m;
            }
            coinc_shift_cc = med_shift;

            //  ── Delay (mimic trigger setup) ──────────────────────────
            //  Subtract a delay so the coincidence peak lands inside the fixed
            //  ±250 cc window.  Two modes:
            //    cfg.anchor_delay_cc != 0  → PINNED: use it literally.
            //    cfg.anchor_delay_cc == 0  → AUTO-PICKER: centre on the
            //      MEASURED peak, but ONLY when it was picked up correctly —
            //      i.e. enough lit pixels (a real coincidence spot).  Below
            //      that floor the peak isn't trustworthy, so no shift (0) and
            //      the run is not spuriously recentred.
            constexpr long kMinLitForAutoDelay = 10;
            const bool auto_picker = (cfg.anchor_delay_cc == 0.0);
            const bool peak_ok = (n_lit >= kMinLitForAutoDelay);
            anchor_delay_used = !auto_picker  ? cfg.anchor_delay_cc
                                : peak_ok      ? coinc_shift_cc
                                              : 0.0;
            //  1D integrated Δt — recentred by the delay (±100 cc window).
            for (const auto &[key, dts] : ch_dt_for_map)
                for (int16_t d : dts)
                {
                    const double shifted = d - anchor_delay_used;
                    if (shifted >= -100.5 && shifted <= 100.5)
                        h_anchor_dt_1d->Fill(shifted);
                }
            //  2D Δt vs spill — recentred by the same delay so the peak
            //  lands in the canvas main pad.
            for (const auto &[s16, d16] : dt2d)
                h_anchor_dt_vs_spill->Fill(
                    static_cast<double>(s16),
                    static_cast<double>(d16) - anchor_delay_used);
            mist::logger::info(TString::Format(
                                   "(pulser_calib_writer) coincidence: %ld lit pixels, measured "
                                   "average peak = %.0f cc (%.1f ns); delay subtracted = %.0f cc "
                                   "[%s] -> peak %srecentred",
                                   n_lit, med_shift, med_shift * CC_TO_NS,
                                   anchor_delay_used,
                                   !auto_picker ? "pinned"
                                   : peak_ok    ? "auto-picker"
                                                : "auto-picker: peak not "
                                                  "confident, no shift",
                                   (anchor_delay_used != 0.0) ? "" : "NOT ")
                                   .Data());
        }
    }
    else
    {
        //  ── Legacy channel anchor ────────────────────────────────────
        const ChannelKey anchor_key{
            static_cast<uint16_t>(cfg.anchor_device),
            static_cast<uint16_t>(cfg.anchor_chip),
            static_cast<uint16_t>(cfg.anchor_eo_channel)};
        auto anchor_it = channels.find(anchor_key);
        if (anchor_it == channels.end())
        {
            mist::logger::warning(TString::Format(
                                      "(pulser_calib_writer) anchor channel %d/%d/ch%d not present in "
                                      "ingested hits — anchor-Δ diagnostic will be empty.",
                                      cfg.anchor_device, cfg.anchor_chip, cfg.anchor_eo_channel)
                                      .Data());
        }
        else
        {
            const auto &anchor_bucket = anchor_it->second;
            for (const auto &[ch_key, ch_bucket] : channels)
            {
                // Cap at the shorter per_spill so we don't read past the
                // end of either channel's sparse vector.
                const int n_spills = static_cast<int>(std::min(
                    anchor_bucket.per_spill.size(),
                    ch_bucket.per_spill.size()));
                for (int s = 0; s < n_spills; ++s)
                {
                    const auto &anchor_hits = anchor_bucket.per_spill[s].hits;
                    const auto &ch_hits = ch_bucket.per_spill[s].hits;
                    const int n_pairs = static_cast<int>(std::min(
                        anchor_hits.size(), ch_hits.size()));
                    for (int i = 0; i < n_pairs; ++i)
                    {
                        const double dc = static_cast<double>(
                            ch_hits[i].abs_coarse_cc - anchor_hits[i].abs_coarse_cc);
                        h_anchor_dt_vs_spill->Fill(s, dc);
                        h_anchor_dt_1d->Fill(dc);
                        ++n_anchor_pairs_filled;
                    }
                }
            }
            mist::logger::info(TString::Format(
                                   "(pulser_calib_writer) anchor-Δ diagnostic filled with %ld "
                                   "(channel, anchor) pairs across %d spills",
                                   n_anchor_pairs_filled, spills_seen)
                                   .Data());
        }
    }

    //  ── Per-channel closed-form fits, parallel ───────────────────
    //  The chi² is quadratic in every unknown → linear least squares
    //  with a closed-form Cholesky solve per channel (~µs).  No
    //  shared global state — fit_channel touches only its local stack,
    //  so we dispatch in chunks via std::async to use all cores.

    //  Move the map into a positional vector so workers can index
    //  without iterator-stability worries.
    std::vector<std::pair<ChannelKey, ChannelBucket>> work;
    work.reserve(channels.size());
    for (auto &kv : channels)
        work.emplace_back(kv.first, std::move(kv.second));
    channels.clear();

    const unsigned n_threads =
        std::max(1u, std::thread::hardware_concurrency());
    mist::logger::info("(pulser_calib_writer) Stage 1: fitting " +
                       std::to_string(work.size()) +
                       " channels (closed-form, " +
                       std::to_string(n_threads) + " threads)");

    std::vector<ChannelResult> results(work.size());
    const long progress_stride =
        std::max<long>(1, static_cast<long>(work.size()) / 10);

    //  Chunked dispatch: at most ~4·n_threads channels in flight so
    //  the std::future queue stays bounded on huge runs.
    const std::size_t batch_size = static_cast<std::size_t>(n_threads) * 4;
    std::size_t next = 0;
    long n_fit = 0;
    while (next < work.size())
    {
        const std::size_t end = std::min(next + batch_size, work.size());
        std::vector<std::future<ChannelResult>> batch;
        batch.reserve(end - next);
        for (std::size_t i = next; i < end; ++i)
        {
            batch.push_back(std::async(
                std::launch::async,
                [&work, i, &cfg]()
                {
                    return fit_channel(work[i].first,
                                       std::move(work[i].second), cfg);
                }));
        }
        for (std::size_t i = next; i < end; ++i)
        {
            results[i] = batch[i - next].get();
            ++n_fit;
            if (n_fit % progress_stride == 0 ||
                n_fit == static_cast<long>(work.size()))
                mist::logger::info("(pulser_calib_writer)   ... fitted " +
                                   std::to_string(n_fit) + "/" +
                                   std::to_string(work.size()) + " channels");
        }
        next = end;
    }
    work.clear();
    work.shrink_to_fit();

    //  ── Build the flat "published" list ───────────────────────────
    //  Per-channel closed-form fits ran in the parallel block above.
    //  Each ChannelResult holds 4 per-TDC (a, b) directly — no
    //  per-spill aggregation, no Stage 2/3 reshuffling.  By the gauge
    //  fix (θ_0 pinned at 0), b_TDC0 ≡ 0 for every channel and the
    //  other b values are intra-channel TDC offsets relative to TDC 0.
    //
    //  Cross-channel alignment is NOT computed here — different
    //  channels' calibrations are each self-consistent against their
    //  own TDC 0.  A future Stage 2 (γ-mode in the design notes)
    //  would tie all channels to a common detector clock; this
    //  baseline ships the per-channel result as-is.
    //
    //  Physical clamps applied HERE (at publish time), not inside
    //  fit_channel — keeps the raw fit auditable while ensuring both
    //  the fine_calib.toml file and the QA histograms see the same
    //  bounded values.

    struct PublishedTdc
    {
        ChannelKey key;
        int tdc_idx;
        bool ok;
        double a;
        double b;
        double sigma;
        bool a_clamped;
        bool b_clamped;
    };
    std::vector<PublishedTdc> published;
    long n_solve_failed = 0;
    for (const auto &r : results)
    {
        if (!r.converged)
            ++n_solve_failed;
        for (int it = 0; it < 4; ++it)
        {
            const auto &t = r.tdc[it];
            PublishedTdc pt;
            pt.key = r.key;
            pt.tdc_idx = it;
            pt.ok = t.fitted;
            pt.a_clamped = false;
            pt.b_clamped = false;
            if (t.fitted)
            {
                //  Slope clamp.  Outside the physical band, the
                //  fitted slope is noise (sparse fine coverage) — fall
                //  back to default_slope so downstream cc→ns conversion
                //  stays in spec.  This is also where slope_guard
                //  fall-backs from fit_channel survive into output.
                double a = t.a;
                if (a < cfg.slope_min || a > cfg.slope_max)
                {
                    a = cfg.default_slope_cc_per_bin;
                    pt.a_clamped = true;
                }
                //  Intercept clamp: hard-coded ±20 cc by default
                //  (cable/routing physics).  Out-of-band b is the
                //  integer-T residue from independent inter-chip
                //  clock starts, NOT a real channel delay.
                double b = t.b;
                if (b < cfg.b_min)
                {
                    b = cfg.b_min;
                    pt.b_clamped = true;
                }
                else if (b > cfg.b_max)
                {
                    b = cfg.b_max;
                    pt.b_clamped = true;
                }
                pt.a = a;
                pt.b = b;
                //  Per-TDC σ for the v2 schema's 3rd parameter:
                //  use the sqrt(chi²/N) of the joint fit, in cc.
                //  All 4 TDCs of one channel share the same fit so
                //  they share the same residual sigma.
                pt.sigma = std::sqrt(r.chi2_per_pair);
            }
            published.push_back(pt);
        }
    }
    mist::logger::info(TString::Format(
                           "(pulser_calib_writer) %ld of %lu channel solves failed (Cholesky)",
                           n_solve_failed,
                           static_cast<unsigned long>(results.size()))
                           .Data());

    //  Slip-correction stats (regime 1+2 — see fit_channel comment).
    long long n_slipped_total = 0;
    long n_channels_with_slips = 0;
    long n_slipped_per_tdc_idx[4] = {0, 0, 0, 0};
    for (const auto &r : results)
    {
        long n_chan_slip = 0;
        for (int t = 0; t < 4; ++t)
        {
            n_slipped_per_tdc_idx[t] += r.tdc[t].n_slipped_hits;
            n_chan_slip += r.tdc[t].n_slipped_hits;
        }
        n_slipped_total += n_chan_slip;
        if (n_chan_slip > 0)
            ++n_channels_with_slips;
    }
    const double frac_slip = (total_hits_read > 0)
                                 ? static_cast<double>(n_slipped_total) /
                                       static_cast<double>(total_hits_read)
                                 : 0.0;
    mist::logger::info(TString::Format(
                           "(pulser_calib_writer) slip correction: %lld hits re-snapped across %ld channels (%.4f%% of total hits)",
                           n_slipped_total, n_channels_with_slips, 100.0 * frac_slip)
                           .Data());
    mist::logger::info(TString::Format(
                           "(pulser_calib_writer)   per-TDC slip totals: TDC0=%ld  TDC1=%ld  TDC2=%ld  TDC3=%ld",
                           n_slipped_per_tdc_idx[0], n_slipped_per_tdc_idx[1],
                           n_slipped_per_tdc_idx[2], n_slipped_per_tdc_idx[3])
                           .Data());

    //  ── Phase 3a: write the calibration file ──────────────────────
    //  Output format selected by the configured path's extension:
    //    `.toml` → TOML v3 schema ([[entry]] tables per GlobalIndex)
    //    anything else → legacy whitespace-separated v2 text
    //  Both reach the same `AlcorFinedata::read_calib_from_file`,
    //  which auto-detects on the consumer side.
    {
        const std::string lower_ext =
            fine_calib_path.size() >= 5
                ? fine_calib_path.substr(fine_calib_path.size() - 5)
                : std::string();
        std::string lower_ext_lc = lower_ext;
        std::transform(lower_ext_lc.begin(), lower_ext_lc.end(),
                       lower_ext_lc.begin(),
                       [](unsigned char ch)
                       { return std::tolower(ch); });
        const bool emit_toml = (lower_ext_lc == ".toml");

        std::ofstream out(fine_calib_path);
        if (!out)
        {
            mist::logger::error("(pulser_calib_writer) cannot open " +
                                fine_calib_path + " for write");
            return;
        }
        if (emit_toml)
        {
            out << "# fine_calib.toml — generated by pulser_calib_writer\n"
                << "# Schema documented at: include/alcor_finedata.h "
                   "(read_calib_from_file)\n\n"
                << "schema = \"fine_calib.v3\"\n\n";
        }

        long n_published = 0;
        long n_skipped = 0;
        for (const auto &pt : published)
        {
            if (!pt.ok)
            {
                ++n_skipped;
                continue;
            }
            //  Build the full GlobalIndex.  fifo is uniquely
            //  determined by (chip, eo_channel) — each physical
            //  channel lives in exactly one FIFO, 8 channels per FIFO
            //  inside one chip:  fifo = 4·chip + ((eo_channel & 31) >> 3)
            //  (eo_channel >= 32 ⇒ chip is odd, the low 5 bits index
            //  within the half-chip.)  Writing the full raw value as
            //  the key fixes the prior collision bug where multiple
            //  devices wrote to the same `calib_index`.
            const int fifo = 4 * pt.key.chip + ((pt.key.eo_channel & 31) >> 3);
            const auto gi = ::GlobalIndex::try_from_components(
                pt.key.device, fifo, pt.key.chip, pt.key.eo_channel, pt.tdc_idx);
            if (!gi)
                continue;
            const uint32_t key = gi->raw();
            const int method_id = static_cast<int>(CalibrationMethod::AlcorV2FitCalib);
            //  v2 / v3 wire convention: p0 = a, p1 = -b, p2 = sigma.
            if (emit_toml)
            {
                out << "[[entry]]\n"
                    << "key     = " << key << "\n"
                    << "method  = " << method_id << "\n"
                    << "a       = " << pt.a << "\n"
                    << "minus_b = " << -pt.b << "\n"
                    << "sigma   = " << pt.sigma << "\n\n";
            }
            else
            {
                out << key << " "
                    << method_id << " "
                    << pt.a << " " << -pt.b << " " << pt.sigma << "\n";
            }
            ++n_published;
        }
        mist::logger::info(
            "(pulser_calib_writer) wrote " + std::to_string(n_published) +
            " entries to " + fine_calib_path +
            (emit_toml ? "  [TOML v3]" : "  [text v2]") +
            "  (skipped " + std::to_string(n_skipped) +
            " TDCs below threshold)");
    }

    //  ── Phase 3b: QA root file ────────────────────────────────────
    //
    //  Layout (mirrors the TDirectory pattern from lightdata_writer /
    //  recodata_writer — embedded params live in `Config/`, run-level
    //  summary in `RunSummary/`, fits in `Fits/`, spread diagnostics
    //  in `Spreads/`).  Top-level stays uncluttered.
    std::unique_ptr<TFile> qa(TFile::Open(qa_root_path.c_str(), "RECREATE"));
    if (!qa || qa->IsZombie())
    {
        mist::logger::error("(pulser_calib_writer) cannot open " + qa_root_path + " for write");
        return;
    }

    //  ── RunSummary/ ──────────────────────────────────────────────
    {
        TDirectory *rs_dir = qa->mkdir("RunSummary");
        rs_dir->cd();
        TParameter<Long64_t> p_total("total_hits_read", total_hits_read);
        TParameter<Long64_t> p_rejected_fine(
            "hits_rejected_fine_out_of_band", total_hits_rejected_fine_oob);
        TParameter<int> p_chans("channels_seen", static_cast<int>(results.size()));
        TParameter<int> p_spills("spills_seen", spills_seen);
        long n_skipped = 0;
        for (const auto &pt : published)
            if (!pt.ok)
                ++n_skipped;
        TParameter<Long64_t> p_skipped("skipped_count", n_skipped);
        p_total.Write();
        p_rejected_fine.Write();
        p_chans.Write();
        p_spills.Write();
        p_skipped.Write();

        //  Slip-correction stats (see fit_channel slip detection).
        TParameter<Long64_t> p_slip_total("slip_total_hits_resnapped", n_slipped_total);
        TParameter<Long64_t> p_slip_chans("slip_channels_with_at_least_one_slip", n_channels_with_slips);
        TParameter<double> p_slip_frac("slip_fraction_of_total_hits", frac_slip);
        TParameter<Long64_t> p_slip_t0("slip_hits_tdc0", n_slipped_per_tdc_idx[0]);
        TParameter<Long64_t> p_slip_t1("slip_hits_tdc1", n_slipped_per_tdc_idx[1]);
        TParameter<Long64_t> p_slip_t2("slip_hits_tdc2", n_slipped_per_tdc_idx[2]);
        TParameter<Long64_t> p_slip_t3("slip_hits_tdc3", n_slipped_per_tdc_idx[3]);
        p_slip_total.Write();
        p_slip_chans.Write();
        p_slip_frac.Write();
        p_slip_t0.Write();
        p_slip_t1.Write();
        p_slip_t2.Write();
        p_slip_t3.Write();

        //  Per-channel slipped-hit count distribution (regime-2 snap
        //  only).  Most channels have 0 slipped hits; a small heavy
        //  tail can have hundreds-to-thousands when an intermittent
        //  slip affects a significant fraction of one TDC's hits.
        //  Use variable-width bins so the "zero slip" bulk lives in
        //  bin 1 (width 1) and the populated tail uses pseudo-log
        //  spacing.  ROOT exposes this via the TH1F(name, title,
        //  nbins, edges[]) ctor — the edges array spans the range
        //  with each entry one bin boundary.
        {
            const std::vector<double> slip_edges = {
                0, 1, 3, 10, 30, 100,
                300, 1000, 3000, 10000, 30000, 100000};
            TH1F h_slip("h_slipped_hits_per_channel",
                        "Slip-corrected hits per channel (sum over 4 TDCs);n_slipped_hits;channels",
                        static_cast<int>(slip_edges.size()) - 1,
                        slip_edges.data());
            for (const auto &r : results)
            {
                long n = 0;
                for (int t = 0; t < 4; ++t)
                    n += r.tdc[t].n_slipped_hits;
                h_slip.Fill(static_cast<double>(n));
            }
            h_slip.Write();
        }

        //  Per-channel pulser period.  Skip the histogram entirely
        //  when the operator pinned T from the TOML — every channel
        //  is identically T_nominal, the hist would be a 2055-entry
        //  delta function pretending to be a distribution.  The
        //  scalar TParameters still publish the value for any
        //  consumer that wants it.
        std::vector<double> Ts;
        for (const auto &r : results)
            if (r.ok && r.T_cc > 0.0)
                Ts.push_back(r.T_cc);
        if (!Ts.empty())
        {
            std::sort(Ts.begin(), Ts.end());
            const double Tmed = Ts[Ts.size() / 2];
            TParameter<double> p_T_cc("pulser_period_cc", Tmed);
            TParameter<double> p_T_ns("pulser_period_ns", Tmed * CC_TO_NS);
            p_T_cc.Write();
            p_T_ns.Write();
            const bool period_fixed = cfg.pulser_period_cc > 0.0;
            if (!period_fixed)
            {
                //  Free-period fit: ~50 ppm spread typical.  Zoom to
                //  ±0.1 % of the median (well above ALCOR clock jitter)
                //  with 200 bins → ~3 cc/bin around the 320 k median.
                const double lo = Tmed * 0.999;
                const double hi = Tmed * 1.001;
                TH1F h_T("h_T_cc_per_channel",
                         "Per-channel pulser period;T (cc);channels",
                         200, lo, hi);
                for (double v : Ts)
                    h_T.Fill(v);
                h_T.Write();
            }
        }
    }

    //  ── Fits/ ────────────────────────────────────────────────────
    //
    //  Binning targets ~10 bins per σ of the actual data, with a
    //  data-driven axis where the clamp band is much wider than
    //  what the population uses.  Clamped pile-ups (at slope_min,
    //  slope_max, default_slope, b_min, b_max) live at the
    //  histogram's bin centres or edges as appropriate so they show
    //  up as visible spikes rather than smeared bins.
    {
        TDirectory *fit_dir = qa->mkdir("Fits");
        fit_dir->cd();

        //  Slope — span = slope_max - slope_min (default 0.01).
        //  Typical fitted σ ≈ 5e-4 ⇒ 10 bins per σ ⇒ 5e-5 cc/bin
        //  ⇒ 200 bins over the band.  default_slope sits at a bin
        //  centre by construction since (default-slope_min)/bin_w is
        //  an integer when slope band is symmetric about default.
        TH1F h_a("h_fit_slope_a",
                 "Per-TDC slope a (clamped to [slope_min, slope_max]);a (cc/bin);TDCs",
                 200, cfg.slope_min, cfg.slope_max);

        //  Intercept b — band ±20 cc, fit σ ≈ 4 cc ⇒ 10 bins per σ
        //  ⇒ ~0.4 cc/bin ⇒ 100 bins on [-20, 20].  Use 0.2 cc/bin
        //  (200 bins) for finer structure — the population sits in
        //  the central ±15 cc but we keep the clamp edges visible.
        //
        //  Pool histogram (all 4 TDCs) PLUS one per-TDC-index hist so
        //  the gauge-fix spike at b=0 (every channel contributes
        //  exactly one entry at 0 from TDC 0) is separable from the
        //  intra-channel TDC-1/2/3 distributions.  A bimodal pool
        //  hist often reduces to four well-behaved unimodal per-TDC
        //  hists once the TDC-0 spike is split off.
        const double b_span = cfg.b_max - cfg.b_min;
        const int b_nbins = std::max(40, static_cast<int>(b_span / 0.2));
        TH1F h_b("h_fit_intercept_b",
                 "Per-TDC intercept b (clamped to [b_min, b_max]);b (cc);TDCs",
                 b_nbins, cfg.b_min, cfg.b_max);
        std::unique_ptr<TH1F> h_b_per_tdc[4];
        for (int t = 0; t < 4; ++t)
        {
            h_b_per_tdc[t] = std::make_unique<TH1F>(
                TString::Format("h_fit_intercept_b_tdc%d", t),
                TString::Format(
                    "Per-TDC intercept b — TDC %d only;b (cc);channels", t),
                b_nbins, cfg.b_min, cfg.b_max);
            h_b_per_tdc[t]->SetDirectory(nullptr);
        }

        //  Residual σ — pulser-runs cluster tightly (σ ~ 0.7 cc, rms
        //  of the population across TDCs is ~5e-3 cc).  Old [0,1]
        //  axis with 200 bins put all data in ~3 bins around 0.69.
        //  Use a wider [0, 2] axis with very fine binning (4e-3
        //  cc/bin) to resolve the population and still show tails
        //  out to 6 ns equivalent.
        TH1F h_s("h_fit_residual_sigma",
                 "Per-TDC fit residual sigma;sigma (cc);TDCs",
                 500, 0.0, 2.0);
        for (const auto &pt : published)
        {
            if (!pt.ok)
                continue;
            h_a.Fill(pt.a);
            h_b.Fill(pt.b);
            h_s.Fill(pt.sigma);
            if (pt.tdc_idx >= 0 && pt.tdc_idx < 4)
                h_b_per_tdc[pt.tdc_idx]->Fill(pt.b);
        }
        h_a.Write();
        h_b.Write();
        h_s.Write();
        //  Per-TDC b histograms.  Expected shape: TDC 0 is a delta
        //  function at 0 (gauge fix).  TDCs 1, 2, 3 each show a main
        //  peak near 0 (intrinsic per-TDC fixed shift + cable
        //  routing) with satellite peaks at ±N·T_coarse cc — these
        //  are the known channel-level multi-clock-cycle slips
        //  (hardware quirk, partially absorbed by the slip-correction
        //  pass).  The split makes both effects visible per TDC.
        //
        //  Gaussian fits in [-1, +1] cc isolate the central "fixed
        //  shift" peak from the satellite contamination — the fit
        //  mean is the TDC's systematic shift, the sigma is the
        //  intrinsic-spread floor.  Stored as TParameters next to
        //  the histograms so consumers can compare numerically
        //  without re-fitting.
        for (int t = 0; t < 4; ++t)
            fit_dir->WriteObject(h_b_per_tdc[t].get(),
                                 h_b_per_tdc[t]->GetName());
        for (int t = 1; t < 4; ++t)
        {
            if (h_b_per_tdc[t]->Integral(
                    h_b_per_tdc[t]->FindBin(-1.0),
                    h_b_per_tdc[t]->FindBin(+1.0)) < 10)
                continue;
            TF1 g(TString::Format("g_b_tdc%d", t), "gaus", -1.0, +1.0);
            //  Seed from the bin range to help convergence on
            //  narrow cores (~0.5 cc sigma).
            g.SetParameter(0, h_b_per_tdc[t]->GetMaximum());
            g.SetParameter(1, 0.0);
            g.SetParameter(2, 0.3);
            //  Q=quiet, R=use TF1 range, 0=no draw, S=return result.
            auto fitres = h_b_per_tdc[t]->Fit(&g, "QR0S");
            if (fitres.Get() && fitres->Status() == 0)
            {
                TParameter<double> p_mean(
                    TString::Format("gaus_mean_tdc%d_cc", t),
                    g.GetParameter(1));
                TParameter<double> p_sigma(
                    TString::Format("gaus_sigma_tdc%d_cc", t),
                    g.GetParameter(2));
                TParameter<double> p_mean_ns(
                    TString::Format("gaus_mean_tdc%d_ns", t),
                    g.GetParameter(1) * CC_TO_NS);
                TParameter<double> p_sigma_ns(
                    TString::Format("gaus_sigma_tdc%d_ns", t),
                    g.GetParameter(2) * CC_TO_NS);
                p_mean.Write();
                p_sigma.Write();
                p_mean_ns.Write();
                p_sigma_ns.Write();
            }
        }
    }

    //  ── Spreads/ ─────────────────────────────────────────────────
    //  Hierarchical b-spread diagnostics: within-channel (4 TDCs),
    //  within-chip (32 channels × 4 TDCs), within-device (all chips).
    //  Answers "where does the ±10 cc clustering end?": pulser data
    //  expectation is tight within chip and within device, broader
    //  across devices (independent DAQ clock-start offsets per RDO).
    {
        TDirectory *sp_dir = qa->mkdir("Spreads");
        sp_dir->cd();

        //  --- Within-channel: 6 SEPARATE TDC-pair histograms ──
        //  One TH1F per unique TDC pair (i, j) with i < j.  Each
        //  histogram is filled across ALL channels of the detector
        //  with `b_i − b_j` measured on that channel.  Splitting per
        //  pair (rather than pooling all pairs into one hist) lets
        //  the operator spot pair-specific systematics — e.g.
        //  "TDC 0 vs TDC 3 has a consistent offset across the
        //  detector but TDC 1 vs TDC 2 doesn't".
        //
        //  Storage as `unique_ptr<TH1F>` + immediate `SetDirectory(nullptr)`
        //  detaches them from the auto-attach-to-current-dir mechanism.
        //  Without that detach, the explicit Write() below runs first
        //  (one TKey written), then `qa->Write()` at the end of the
        //  function picks them up again (second TKey, cycle 2) →
        //  duplicate histograms in the file.
        std::unique_ptr<TH1F> h_b_pair[4][4];
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j)
            {
                const TString name = TString::Format(
                    "h_b_delta_tdc%d_minus_tdc%d", i, j);
                const TString title = TString::Format(
                    "Intra-channel b delta (b[TDC%d] - b[TDC%d]);delta (cc);channels",
                    i, j);
                //  Pair-delta range: max possible |b[i] - b[j]| =
                //  (b_max - b_min) after clamping, but empirically
                //  the population sits in ±10 cc with σ ≈ 3 cc.
                //  Show the FULL clamp-bounded range so saturated
                //  pile-ups at the edges are visible, with bin width
                //  set to ~0.3 cc (≈ 10 bins per σ).
                const double pair_half = cfg.b_max - cfg.b_min;
                const int pair_nbins = std::max(
                    40, static_cast<int>(2.0 * pair_half / 0.3));
                h_b_pair[i][j] = std::make_unique<TH1F>(
                    name, title, pair_nbins, -pair_half, +pair_half);
                h_b_pair[i][j]->SetDirectory(nullptr);
            }

        std::vector<double> pair_delta_cc; //  all pair |Δ|, pooled for the median scalar
        for (const auto &r : results)
        {
            if (!r.ok)
                continue;
            //  Track b per TDC index 0..3 (NaN where the TDC is unfitted).
            double b_tdc[4] = {std::nan(""), std::nan(""), std::nan(""), std::nan("")};
            for (int t = 0; t < 4; ++t)
                if (r.tdc[t].fitted)
                    b_tdc[t] = r.tdc[t].b;
            //  Fill one entry into each per-pair histogram for every
            //  channel where BOTH TDCs of the pair are fitted.
            for (int i = 0; i < 4; ++i)
                for (int j = i + 1; j < 4; ++j)
                {
                    if (!std::isfinite(b_tdc[i]) || !std::isfinite(b_tdc[j]))
                        continue;
                    const double d = b_tdc[i] - b_tdc[j];
                    h_b_pair[i][j]->Fill(d);
                    pair_delta_cc.push_back(std::abs(d));
                }
        }
        //  Write each pair hist explicitly into Spreads/.  The detached
        //  objects (SetDirectory(nullptr) above) won't be re-written by
        //  the final qa->Write().
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j)
                if (h_b_pair[i][j])
                    sp_dir->WriteObject(h_b_pair[i][j].get(),
                                        h_b_pair[i][j]->GetName());

        //  Gaussian fit on each pair-delta within ±1 cc — isolates
        //  the central core from the satellite contamination so
        //  the operator can read off the genuine per-TDC-pair
        //  alignment (mean) and intrinsic floor (sigma).  Stored
        //  alongside the histograms in Spreads/.
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j)
            {
                if (!h_b_pair[i][j])
                    continue;
                if (h_b_pair[i][j]->Integral(
                        h_b_pair[i][j]->FindBin(-1.0),
                        h_b_pair[i][j]->FindBin(+1.0)) < 10)
                    continue;
                TF1 g(TString::Format("g_pair_%d_%d", i, j), "gaus", -1.0, +1.0);
                g.SetParameter(0, h_b_pair[i][j]->GetMaximum());
                g.SetParameter(1, 0.0);
                g.SetParameter(2, 0.3);
                auto fitres = h_b_pair[i][j]->Fit(&g, "QR0S");
                if (fitres.Get() && fitres->Status() == 0)
                {
                    TParameter<double> p_mean(
                        TString::Format("gaus_mean_tdc%d_minus_tdc%d_cc", i, j),
                        g.GetParameter(1));
                    TParameter<double> p_sigma(
                        TString::Format("gaus_sigma_tdc%d_minus_tdc%d_cc", i, j),
                        g.GetParameter(2));
                    TParameter<double> p_mean_ns(
                        TString::Format("gaus_mean_tdc%d_minus_tdc%d_ns", i, j),
                        g.GetParameter(1) * CC_TO_NS);
                    TParameter<double> p_sigma_ns(
                        TString::Format("gaus_sigma_tdc%d_minus_tdc%d_ns", i, j),
                        g.GetParameter(2) * CC_TO_NS);
                    p_mean.Write();
                    p_sigma.Write();
                    p_mean_ns.Write();
                    p_sigma_ns.Write();
                }
            }

        //  Summary scalars + log line — intra-channel only.
        //  Within-chip / within-device spread histograms intentionally
        //  not kept: cross-chip max−min is dominated by
        //  independent-clock integer-T residues that aren't a
        //  meaningful "spread" — the 6 per-pair plots above carry
        //  the intra-channel diagnostic we actually want.
        auto median = [](std::vector<double> v) -> double
        {
            if (v.empty())
                return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const double med_pair_cc = median(pair_delta_cc);
        TParameter<double> p_med_pair("median_abs_intra_channel_pair_delta_cc", med_pair_cc);
        p_med_pair.Write();

        if (!pair_delta_cc.empty())
        {
            const long n_pairs = static_cast<long>(pair_delta_cc.size());
            auto count_below_ps = [&](double thr_ps)
            {
                const double thr_cc = thr_ps / (CC_TO_NS * 1000.0);
                long n = 0;
                for (double v : pair_delta_cc)
                    if (v < thr_cc)
                        ++n;
                return static_cast<double>(n) / n_pairs;
            };
            TParameter<double> p50("fraction_pairs_abs_delta_below_50ps", count_below_ps(50.0));
            TParameter<double> p100("fraction_pairs_abs_delta_below_100ps", count_below_ps(100.0));
            TParameter<double> p250("fraction_pairs_abs_delta_below_250ps", count_below_ps(250.0));
            p50.Write();
            p100.Write();
            p250.Write();
        }

        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) intra-channel pair |Δ| median = %.4f cc (%.2f ps)",
                               med_pair_cc, med_pair_cc * CC_TO_NS * 1000.0)
                               .Data());
    }

    //  ── Diagnostics/ ─────────────────────────────────────────────
    //  Sum per-channel cdiff arrays into a single TH1F view of the
    //  `c_h − c_p` distribution.  Lets the operator see whether the
    //  tolerance window correctly captures the 1·T peak — too tight
    //  ⇒ visible truncation at ±tol; too loose ⇒ 2T, 3T tails leak.
    {
        TDirectory *diag_dir = qa->mkdir("Diagnostics");
        diag_dir->cd();
        const double T_nominal =
            cfg.pulser_period_cc > 0.0 ? cfg.pulser_period_cc : 320000.0;

        auto h_zoom = std::make_unique<TH1F>(
            "h_pair_coarse_diff_zoom",
            TString::Format(
                "Consecutive-pair (c_h - c_p) - T_{nominal=%.0f cc};delta (cc);pairs",
                T_nominal),
            CDIFF_ZOOM_BINS, -1000.0, +1000.0 + 1.0);
        h_zoom->SetDirectory(nullptr);
        auto h_wide = std::make_unique<TH1F>(
            "h_pair_coarse_diff_wide",
            TString::Format(
                "Consecutive-pair coarse diff over [0, 5T] (T=%.0f cc);c_h - c_p (cc);pairs",
                T_nominal),
            CDIFF_WIDE_BINS, 0.0, 5.0 * T_nominal);
        h_wide->SetDirectory(nullptr);

        long long n_pairs_seen_total = 0;
        long long n_pairs_inside_tol = 0;
        for (const auto &r : results)
        {
            n_pairs_seen_total += r.n_pairs_seen;
            n_pairs_inside_tol += r.n_pairs_used;
            for (int k = 0; k < CDIFF_ZOOM_BINS; ++k)
                if (r.cdiff_zoom[k])
                    h_zoom->AddBinContent(k + 1, r.cdiff_zoom[k]);
            for (int k = 0; k < CDIFF_WIDE_BINS; ++k)
                if (r.cdiff_wide[k])
                    h_wide->AddBinContent(k + 1, r.cdiff_wide[k]);
        }
        h_zoom->SetEntries(static_cast<double>(n_pairs_seen_total));
        h_wide->SetEntries(static_cast<double>(n_pairs_seen_total));
        diag_dir->WriteObject(h_zoom.get(), h_zoom->GetName());
        diag_dir->WriteObject(h_wide.get(), h_wide->GetName());

        TParameter<Long64_t> p_seen("n_pairs_seen", n_pairs_seen_total);
        TParameter<Long64_t> p_used("n_pairs_inside_tolerance", n_pairs_inside_tol);
        const double frac = (n_pairs_seen_total > 0)
                                ? static_cast<double>(n_pairs_inside_tol) /
                                      static_cast<double>(n_pairs_seen_total)
                                : 0.0;
        TParameter<double> p_frac("fraction_pairs_inside_tolerance", frac);
        p_seen.Write();
        p_used.Write();
        p_frac.Write();
        mist::logger::info(TString::Format(
                               "(pulser_calib_writer) pair safety filter: %lld of %lld within ±%.1f cc (%.4f%%)",
                               n_pairs_inside_tol, n_pairs_seen_total,
                               cfg.consecutive_pair_tolerance_cc,
                               100.0 * frac)
                               .Data());

        //  ── Anchor-Δ vs spill — fill happened earlier, write here ──
        //  Stored in Diagnostics/ alongside the other pair plots; also
        //  rendered to a 3-pad PDF picked up by the QA Calibration
        //  sub-tab (slim top zoom at +rollover, main at ±250 cc,
        //  slim bottom zoom at −rollover).
        if (h_anchor_dt_vs_spill)
        {
            diag_dir->WriteObject(h_anchor_dt_vs_spill.get(),
                                  h_anchor_dt_vs_spill->GetName());

            //  3-pad canvas rendering moved to ``util::qa::render_anchor_dt_canvas``
            //  (see ``include/writers/anchor_dt_canvas.h``) so the
            //  ``lightdata_writer`` can share the exact same look for
            //  its per-trigger plots — backlog row P 1.61.  All the
            //  layout / region-split / overlay logic that used to
            //  live here is now in ``src/writers/anchor_dt_canvas.cxx``;
            //  every tweak (log-z, alternate layouts, etc.) lands in
            //  one place and benefits both writers automatically.
            const std::string pulser_run_dir = data_repository + "/" + run_name;
            const auto pdf = util::qa::pdf_path(
                pulser_run_dir, "calibration", 5, "anchor_dt_vs_spill");
            util::qa::AnchorDtCanvasOpts opts;
            opts.rollover_cc = BTANA_ALCOR_ROLLOVER_TO_CC;
            opts.title =
                (cfg.anchor_fifo >= 0)
                    ? TString::Format(
                          "#Deltat_{trg} vs spill   "
                          "(channel #minus nearest anchor pulse: device %d, FIFO %d)",
                          cfg.anchor_device, cfg.anchor_fifo)
                          .Data()
                    : TString::Format(
                          "#Deltat_{trg} vs spill   "
                          "(channel #minus anchor: device %d, chip %d, channel %d)",
                          cfg.anchor_device, cfg.anchor_chip, cfg.anchor_eo_channel)
                          .Data();
            opts.pdf_path = pdf.string();
            opts.logger_prefix = "(pulser_calib_writer)";
            util::qa::render_anchor_dt_canvas(*h_anchor_dt_vs_spill, opts);

            //  Consecutive anchor-pulse Δt — the pulse cadence as a 1D
            //  Gaussian (its own PDF, 06_anchor_consecutive_dt).  The fit
            //  mean is the period; the title carries the derived average
            //  rate.  Log-Y so the missed-pulse tail stays visible.
            if (h_anchor_consecutive_dt &&
                n_anchor_consecutive_filled > 0)
            {
                diag_dir->WriteObject(h_anchor_consecutive_dt.get(),
                                      h_anchor_consecutive_dt->GetName());
                const auto pdf_cons = util::qa::pdf_path(
                    pulser_run_dir, "calibration", 6,
                    "anchor_consecutive_dt");
                TCanvas c_cons("c_anchor_consecutive_dt",
                               "consecutive anchor dt", 1000, 1000);
                c_cons.SetLogy();
                h_anchor_consecutive_dt->SetTitle(
                    TString::Format(
                        "consecutive anchor-pulse #Deltat   (device %d, "
                        "FIFO %d)   period = %.2f cc, rate = %.4f MHz;"
                        "#Deltat (cc)  = c_{i} #minus c_{i-1};pulses",
                        cfg.anchor_device, cfg.anchor_fifo, anchor_period_cc,
                        anchor_rate_hz / 1e6));
                h_anchor_consecutive_dt->SetLineColor(kAzure + 1);
                h_anchor_consecutive_dt->Draw("HIST");
                if (auto *fn = h_anchor_consecutive_dt->GetFunction("gaus"))
                {
                    fn->SetLineColor(kRed + 1);
                    fn->Draw("SAME");
                }
                c_cons.SaveAs(pdf_cons.string().c_str());
                //  ROOT wraps the canvas in A4 regardless of TCanvas size;
                //  crop the MediaBox to the content box so the QA gallery
                //  tiles uniformly (square, matching 05).
                util::qa::crop_pdf_inplace(pdf_cons);
            }

            //  1D channel−anchor Δt (07) — the coincidence distribution.
            //  A prompt peak near 0 ⇒ channels coincident with the laser
            //  pulse (real light), sitting on a flat DCR pedestal; a flat
            //  band ⇒ free-running anchor (no per-pulse coincidence).
            //  We fit prompt Gaussian + flat pedestal, report the
            //  coincidence rate as a fraction of anchor pulses, and scan
            //  for an afterpulse secondary peak (a physical-light signature).
            if (h_anchor_dt_1d && h_anchor_dt_1d->GetEntries() > 0)
            {
                diag_dir->WriteObject(h_anchor_dt_1d.get(),
                                      h_anchor_dt_1d->GetName());
                //  Fixed ±250 cc, 1-cc bins; the peak is delay-shifted into it.
                TH1F *h07 = h_anchor_dt_1d.get();
                const double peak_seed =
                    h07->GetBinCenter(h07->GetMaximumBin());

                //  FULL model: pol0 (DCR floor) + gaus1 (prompt coincidence)
                //  + gaus2 (afterpulse).  1-cc bins over ±100 cc — no comb, so
                //  the fitted Gaussian AREAS give the counts directly
                //  (area = amp·σ·√2π).  Seeds: prompt = tallest bin (delay-
                //  centred ≈ 0); afterpulse = tallest bin OUTSIDE the prompt
                //  ±5 cc core; pedestal = histogram minimum.
                const double kSqrt2Pi =
                    std::sqrt(2.0 * 3.14159265358979323846);
                const double prompt_seed = peak_seed;
                double after_seed = 0.0, after_amp = 0.0;
                for (int b = 1; b <= h07->GetNbinsX(); ++b)
                {
                    const double c = h07->GetBinCenter(b);
                    if (std::abs(c - prompt_seed) < 5.0)
                        continue;
                    if (h07->GetBinContent(b) > after_amp)
                    {
                        after_amp = h07->GetBinContent(b);
                        after_seed = c;
                    }
                }
                const double ped_seed = std::max(1.0, h07->GetMinimum());
                const double xlo = h07->GetXaxis()->GetXmin();
                const double xhi = h07->GetXaxis()->GetXmax();

                TF1 f_full("f_full", "gaus(0)+gaus(3)+pol0(6)", xlo, xhi);
                f_full.SetParameters(h07->GetMaximum(), prompt_seed, 2.0,
                                     std::max(after_amp - ped_seed, 1.0),
                                     after_seed, 2.0, ped_seed);
                f_full.SetParLimits(1, prompt_seed - 10.0, prompt_seed + 10.0);
                f_full.SetParLimits(2, 0.3, 15.0); // prompt σ
                f_full.SetParLimits(4, after_seed - 10.0, after_seed + 10.0);
                f_full.SetParLimits(5, 0.3, 15.0); // afterpulse σ
                f_full.SetParLimits(6, 0.0, h07->GetMaximum());
                f_full.SetNpx(2000);
                h07->Fit(&f_full, "Q0");

                const double ped = std::max(0.0, f_full.GetParameter(6));
                //  Both fitted Gaussians + their areas (1-cc bins → area =
                //  amp·σ·√2π).  PROMPT = the LARGER-area peak; AFTERPULSE =
                //  the smaller.  Afterpulse probability = smaller area /
                //  larger area (≤ 1 by construction — the fit may label
                //  either gaus as the bigger one).
                const double a_g1 = std::abs(f_full.GetParameter(0)) *
                                    std::abs(f_full.GetParameter(2)) * kSqrt2Pi;
                const double a_g2 = std::abs(f_full.GetParameter(3)) *
                                    std::abs(f_full.GetParameter(5)) * kSqrt2Pi;
                const bool g1_bigger = (a_g1 >= a_g2);
                const double amp_p = std::abs(
                    f_full.GetParameter(g1_bigger ? 0 : 3));
                const double mu_p = f_full.GetParameter(g1_bigger ? 1 : 4);
                const double sig_p = std::abs(
                    f_full.GetParameter(g1_bigger ? 2 : 5));
                const double amp_a = std::abs(
                    f_full.GetParameter(g1_bigger ? 3 : 0));
                const double mu_a = f_full.GetParameter(g1_bigger ? 4 : 1);
                const double sig_a = std::abs(
                    f_full.GetParameter(g1_bigger ? 5 : 2));

                const double n_prompt = std::max(a_g1, a_g2); // larger area
                const double n_after = std::min(a_g1, a_g2);  // smaller area
                const double coinc_frac =
                    (total_anchor_hits > 0)
                        ? n_prompt / static_cast<double>(total_anchor_hits)
                        : 0.0;
                //  Afterpulse probability = smaller integral / larger integral.
                const double after_frac =
                    (n_prompt > 0.0) ? n_after / n_prompt : 0.0;

                const bool clean_peak =
                    (sig_p > 0.3 && sig_p < 14.0 && ped > 0.0 &&
                     amp_p > 5.0 * std::sqrt(ped));
                const bool clean_after =
                    (clean_peak && sig_a < 14.0 &&
                     amp_a > 5.0 * std::sqrt(ped) && after_frac > 0.001);

                if (clean_peak)
                    mist::logger::info(TString::Format(
                                           "(pulser_calib_writer) coincidence: prompt #mu=%.2f ns "
                                           "#sigma=%.2f ns, %.3f%% of %ld pulses; afterpulse %s "
                                           "#mu=%.2f ns #sigma=%.2f ns, prob=%.3f%% (smaller/"
                                           "larger area); DCR pedestal=%.1f/bin",
                                           (anchor_delay_used + mu_p) * CC_TO_NS, sig_p * CC_TO_NS,
                                           100.0 * coinc_frac, total_anchor_hits,
                                           clean_after ? "" : "(weak)",
                                           (anchor_delay_used + mu_a) * CC_TO_NS,
                                           sig_a * CC_TO_NS, 100.0 * after_frac, ped)
                                           .Data());
                else
                    mist::logger::info(
                        "(pulser_calib_writer) coincidence: no clean prompt "
                        "peak in the channel-summed #Deltat (per-channel delay "
                        "spread)");

                //  Persist the scalars next to the histogram.
                TParameter<double> p_cf("coincidence_fraction", coinc_frac);
                TParameter<double> p_mu("coincidence_peak_ns",
                                        (anchor_delay_used + mu_p) * CC_TO_NS);
                TParameter<double> p_sg("coincidence_sigma_ns",
                                        sig_p * CC_TO_NS);
                TParameter<double> p_af("afterpulse_probability", after_frac);
                TParameter<double> p_am("afterpulse_peak_ns",
                                        (anchor_delay_used + mu_a) * CC_TO_NS);
                diag_dir->WriteObject(&p_cf, p_cf.GetName());
                diag_dir->WriteObject(&p_mu, p_mu.GetName());
                diag_dir->WriteObject(&p_sg, p_sg.GetName());
                diag_dir->WriteObject(&p_af, p_af.GetName());
                diag_dir->WriteObject(&p_am, p_am.GetName());

                h07->SetTitle(
                    clean_peak
                        ? TString::Format(
                              "calibration anchor #Deltat (device %d, FIFO %d) "
                              "  prompt @ %.1f ns (#sigma %.1f ns) %.2f%% · "
                              "afterpulse @ %.1f ns %.2f%%;"
                              "#Deltat (cc)  = c_{ch} #minus c_{anchor} "
                              "#minus delay;hits",
                              cfg.anchor_device, cfg.anchor_fifo,
                              (anchor_delay_used + mu_p) * CC_TO_NS,
                              sig_p * CC_TO_NS, 100.0 * coinc_frac,
                              (anchor_delay_used + mu_a) * CC_TO_NS,
                              100.0 * after_frac)
                        : TString::Format(
                              "calibration anchor #Deltat (device %d, FIFO %d) "
                              "  no clean coincidence peak;"
                              "#Deltat (cc)  = c_{ch} #minus c_{anchor} "
                              "#minus delay;hits",
                              cfg.anchor_device, cfg.anchor_fifo));

                //  Individual components (each Gaussian on the DCR pedestal,
                //  plus the bare pedestal) so the parts are visible on log-Y.
                TF1 g_prompt("g_prompt", "gaus(0)+pol0(3)", xlo, xhi);
                g_prompt.SetParameters(amp_p, mu_p, sig_p, ped);
                g_prompt.SetNpx(2000);
                TF1 g_after("g_after", "gaus(0)+pol0(3)", xlo, xhi);
                g_after.SetParameters(amp_a, mu_a, sig_a, ped);
                g_after.SetNpx(2000);
                TF1 g_ped("g_ped", "pol0", xlo, xhi);
                g_ped.SetParameter(0, ped);

                const auto pdf_1d = util::qa::pdf_path(
                    pulser_run_dir, "calibration", 7, "anchor_dt_1d");
                TCanvas c_1d("c_anchor_dt_1d", "anchor dt 1d", 1000, 1000);
                c_1d.SetLogy();
                gStyle->SetOptStat(0);
                gStyle->SetOptFit(0);
                h07->SetStats(0); // no stat/fit box on the fit plot
                h07->SetLineColor(kAzure + 1);
                h07->SetMinimum(0.5);
                h07->Draw("HIST");
                f_full.SetLineColor(kRed + 1);
                f_full.SetLineWidth(2);
                f_full.Draw("SAME");
                g_prompt.SetLineColor(kGreen + 2);
                g_prompt.SetLineStyle(2);
                g_prompt.Draw("SAME");
                g_after.SetLineColor(kMagenta + 1);
                g_after.SetLineStyle(2);
                g_after.Draw("SAME");
                g_ped.SetLineColor(kGray + 2);
                g_ped.SetLineStyle(3);
                g_ped.Draw("SAME");
                TLegend leg(0.55, 0.68, 0.88, 0.88);
                leg.SetBorderSize(0);
                leg.SetFillStyle(0);
                leg.AddEntry(&f_full, "full: pol0 + 2 gaus", "l");
                leg.AddEntry(&g_prompt,
                             TString::Format("prompt (gaus 1): %.2f%% of pulses",
                                             100.0 * coinc_frac),
                             "l");
                //  Afterpulse PROBABILITY = gaus2 area / gaus1 area (from fit).
                leg.AddEntry(&g_after,
                             TString::Format(
                                 "afterpulse (gaus 2): P = %.2f%%",
                                 100.0 * after_frac),
                             "l");
                leg.AddEntry(&g_ped, "DCR pedestal (pol0)", "l");
                leg.Draw();
                c_1d.SaveAs(pdf_1d.string().c_str());
                util::qa::crop_pdf_inplace(pdf_1d);
            }

            //  Coincidence hitmap (08) — laser spot: per-pixel count of hits
            //  in the shifted coincidence window.  COLZ readout map.
            if (h_coinc_map && h_coinc_map->GetEntries() > 0)
            {
                diag_dir->WriteObject(h_coinc_map.get(),
                                      h_coinc_map->GetName());
                const auto pdf_map = util::qa::pdf_path(
                    pulser_run_dir, "calibration", 8, "coincidence_map");
                TCanvas c_map("c_coinc_map", "coincidence map", 1000, 1000);
                c_map.SetRightMargin(0.15);
                h_coinc_map->SetStats(0);
                h_coinc_map->Draw("COLZ");
                c_map.SaveAs(pdf_map.string().c_str());
                util::qa::crop_pdf_inplace(pdf_map);
            }
        }
    }

    //  ── Skipped-TDC list — lives under RunSummary/ next to its
    //  count (skipped_count was written there above; keeping the list
    //  alongside avoids the inconsistency of one in RunSummary and
    //  one at top level).
    if (auto *rs = qa->Get<TDirectory>("RunSummary"))
    {
        rs->cd();
        std::ostringstream ss;
        long n = 0;
        for (const auto &pt : published)
        {
            if (pt.ok)
                continue;
            const int fifo = 4 * pt.key.chip + ((pt.key.eo_channel & 31) >> 3);
            const auto gi = ::GlobalIndex::try_from_components(
                pt.key.device, fifo, pt.key.chip, pt.key.eo_channel, pt.tdc_idx);
            if (!gi)
                continue;
            if (n)
                ss << ",";
            ss << gi->raw();
            ++n;
        }
        TNamed n_skipped("skipped_global_indices", ss.str().c_str());
        n_skipped.Write();
    }

    //  ── Config/ ──────────────────────────────────────────────────
    //  Anchored at the very end so it sits LAST in the TFile's key
    //  list (alongside the convention recodata / recotrack /
    //  lightdata follow): all the physics subdirs first, the self-
    //  describing parameter dump after.  Makes a `TBrowser` view
    //  read top-to-bottom from "what was measured" to "how it was
    //  measured".  Routed through util::ConfigDump
    //  (include/utility/config_dump.h) for uniformity with the other
    //  writers — every writer emits the same Config/ schema so the
    //  QA dashboard reads them with one code path.
    {
        util::ConfigDump dump(qa.get());
        dump.add("anchor_device", cfg.anchor_device)
            .add("anchor_chip", cfg.anchor_chip)
            .add("anchor_eo_channel", cfg.anchor_eo_channel)
            .add("min_hits_per_tdc", cfg.min_hits_per_tdc)
            .add("min_hits_per_tdc_per_spill", cfg.min_hits_per_tdc_per_spill)
            .add("fine_min_valid", cfg.fine_min_valid)
            .add("fine_max_valid", cfg.fine_max_valid)
            .add("slope_fit_min_fine_span", cfg.slope_fit_min_fine_span)
            .add("default_slope_cc_per_bin", cfg.default_slope_cc_per_bin)
            .add("slope_min", cfg.slope_min)
            .add("slope_max", cfg.slope_max)
            .add("b_min", cfg.b_min)
            .add("b_max", cfg.b_max)
            .add("pulser_period_cc", cfg.pulser_period_cc)
            .add("consecutive_pair_tolerance_cc", cfg.consecutive_pair_tolerance_cc)
            .add("slip_confidence_cc", cfg.slip_confidence_cc)
            .add("slip_max_snap_fraction", cfg.slip_max_snap_fraction);
        //  Runtime flags + the conf-file paths used at load time.
        dump.add("force_rebuild", cfg.force_rebuild)
            .add_path("override_path", cfg.override_path)
            .add_path("default_path", cfg.default_path);
        //  Verbatim TOML body of whichever calibration conf was picked
        //  up (override takes precedence over the default).  Was
        //  missing in v1 — the dashboard now has the same "[toml
        //  payloads]" section it shows for lightdata.root.
        const std::string calib_conf_path =
            !cfg.override_path.empty() ? cfg.override_path : cfg.default_path;
        dump.add_conf_file("calibration_conf", calib_conf_path);
    }

    qa->Write();
    qa->Close();
    mist::logger::info("(pulser_calib_writer) wrote QA to " + qa_root_path);

    //  ---
    //  --- Publish cross-run scalars to AnalysisResults.
    //
    //  Same dual-backend (.root + .toml) store the other writers feed.
    //  Sensor key is "all" — pulser_calib spans every TDC the run had
    //  hits on, and the per-TDC fine-table itself already lives on
    //  disk as the canonical artefact.  These are the run-level
    //  summary numbers the dashboard scoreboard needs.
    {
        //  Cross-run aggregate next to the run directories
        //  (``<data_repository>/standard_results.toml`` = ``Data/``).
        //  Same convention as the other three writers — stale
        //  ``extData/`` hard-code was failing because the dashboard
        //  launches with cwd at the git repo root; the store lives
        //  under data_repository (``Data/``), NOT the repo root.
        AnalysisResults ar(data_repository + "/standard_results.toml");
        ar.update(ResultMap{
                      {{run_name, "all", "calibration.total_hits_read"},
                       {static_cast<double>(total_hits_read), 0.0}},
                      {{run_name, "all", "calibration.spills_seen"},
                       {static_cast<double>(spills_seen), 0.0}},
                      {{run_name, "all", "calibration.n_published_tdcs"},
                       {static_cast<double>(published.size()), 0.0}},
                  },
                  /*source=*/"calibration");
    }

    mist::logger::info("(pulser_calib_writer) === DONE ===");
}

} // namespace btana
