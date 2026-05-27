#pragma once

/**
 * @file types.h
 * @brief Per-frame and per-ring result types shared across the recodata
 *        writer's split translation units.
 *
 * These types were originally defined locally inside `recodata_writer()`
 * (anonymous namespace + function-scope structs) before the writer was
 * split into orchestrator + per-frame compute + finalize-QA translation
 * units.  Lifting them to a header lets the split units share the same
 * type definitions without resorting to template metaprogramming or
 * void*.
 *
 * Pipeline overview (DISCUSSION § 2.6 / § 2.7):
 *
 *   per-frame compute (parallel)
 *       │  produces FrameResult { RingFitResult first, second, ... }
 *       ▼
 *   per-frame drain     (serial, in frame order)
 *       │  reads FrameResult + writes via RingFillHists into the
 *       │  per-ring output histogram bundle
 *       ▼
 *   finalize-QA         (single-threaded post-loop)
 */

#include <map>
#include <utility>
#include <vector>

#include "alcor_recodata.h" // TriggerEvent

class TH1F;
class TH2F;

namespace btana::recodata
{

// ─────────────────────────────────────────────────────────────────────
//  Per-ring fit result — produced by the pure compute pass.
//
//  All members live in one POD-ish struct so that a FrameResult can
//  hold two of them (ring 1 / ring 2) by value and be moved cheaply
//  between worker threads and the drain.
// ─────────────────────────────────────────────────────────────────────
struct RingFitResult
{
    bool fit_ok = false;
    int n_hits = 0;
    float cx = 0.f;
    float cy = 0.f;
    float R = 0.f;
    float sigma_r = 0.f; ///< per-ring RMS of radial residuals (un-smeared)
    float f_coverage = 0.f;

    //  Un-smeared (pixel-centre) per-hit observables.
    //  `radial_per_hit[i]`  = |hit_i − fit-centre|     using pixel-centre positions.
    //  `loo_residuals[i]`   = r_hit_i − R_{−i}         using pixel-centre positions.
    std::vector<float> radial_per_hit;
    std::vector<float> loo_residuals;

    //  Smeared (pixel-jittered, ±half-pitch uniform) siblings.
    //  Filled in parallel with the un-smeared versions in
    //  `compute_ring_fit`, so the two histograms can be cross-checked.
    //  Variance subtraction recipe:
    //
    //      σ²_intrinsic  =  σ²_observed  −  k · (pitch² / 12)
    //
    //  with k = 1 for the unsmeared distribution (residual already
    //  carries one pixel-pitch uniform quantisation from the readout),
    //  and k = 2 for the smeared distribution (one quantisation from
    //  the readout + one independent quantisation added by
    //  `get_hit_*_rnd`).  At 3 mm pitch, k=1 ⇒ 0.75 mm², k=2 ⇒ 1.50 mm².
    std::vector<float> radial_per_hit_smeared;
    std::vector<float> loo_residuals_smeared;
};

// ─────────────────────────────────────────────────────────────────────
//  Per-frame compute output — drained in frame order on a single
//  thread (DISCUSSION § 2.7).
// ─────────────────────────────────────────────────────────────────────
struct FrameResult
{
    int i_frame = -1;      ///< index in frames_in_spill
    bool accepted = false; ///< neither rejected nor edge-only
    bool rejected = false; ///< duplicate trigger detected → drop frame
    bool had_edge = false; ///< at least one edge-rejected trigger

    //  Hist-fill payloads — recorded in compute, played back in drain.
    //  `(reg_bin_centre, value)` tuples; the drain just does
    //  `hist->Fill(reg_bin_centre, value)`.
    std::vector<std::pair<float, float>> edge_fills;       ///< h_edge_trigger_position
    std::vector<std::pair<float, float>> trigger_qa_fills; ///< h_trigger_qa (the 1.5 / 2.5 y values)

    //  Time-diff fills: (trigger_index, Δt_ns).  Drain looks up /
    //  lazily creates the per-trigger hist before filling.
    std::vector<std::pair<uint8_t, float>> time_diff_fills;

    std::map<uint8_t, TriggerEvent> accepted_triggers;
    bool frame_is_physics = false; ///< increments n_physics_per_spill
    bool frame_has_second_ring = false;

    RingFitResult first;
    RingFitResult second;
};

// ─────────────────────────────────────────────────────────────────────
//  Result of the Crystal-Ball + pol3 radial fit (DISCUSSION § 2.6).
//
//  Produced by `fit_radial_distribution()` (radial_fit.cxx), collected
//  by the finalize block into a vector that drives the per-run summary
//  TH1Fs (`h_N_gamma_per_ring_summary`, `h_peak_mu_summary`,
//  `h_peak_sigma_summary`).
// ─────────────────────────────────────────────────────────────────────
struct RadialFitResult
{
    std::string name;
    double n_gamma = 0.;
    double peak_mu = 0.;
    double peak_mu_err = 0.;
    double peak_sigma = 0.;
    double peak_sigma_err = 0.;
};

// ─────────────────────────────────────────────────────────────────────
//  Result of the one-parameter σ(N) = σ_photon·√(N/(N-3)) LOO fit.
//
//  Produced by `fit_sigma_vs_n()` (sigma_vs_n_fit.cxx).  `is_residual`
//  partitions the collector for the per-source summary plot
//  (`h_sigma_photon_summary`).
// ─────────────────────────────────────────────────────────────────────
struct VsNFitResult
{
    std::string name;
    double sigma_photon = 0.;
    double sigma_photon_err = 0.;
    bool is_residual = true; ///< always true (only residual hists fitted)
};

// ─────────────────────────────────────────────────────────────────────
//  Per-ring fill target — pointer-bundle of the output histograms that
//  a single ring slot writes to.  Any pointer may be nullptr to skip
//  that fill.  Lets the drain layer pick the matching bundle (first /
//  second / dual / solo) without branching on ring identity.
// ─────────────────────────────────────────────────────────────────────
struct RingFillHists
{
    TH1F *h_nhits = nullptr;
    TH1F *h_nphotons = nullptr;
    TH1F *h_fcov = nullptr;
    TH1F *h_radial = nullptr;        ///< pixel-centre radii — consistency check
    TH1F *h_R = nullptr;             ///< fitted ring radius
    TH1F *h_sigma = nullptr;         ///< per-ring RMS of radial residuals (biased — see DISCUSSION § 2.6)
    TH2F *h_R_vs_nhits = nullptr;    ///< correlation
    TH2F *h_centre_xy = nullptr;     ///< fit centre map
    TH2F *h_residual_vs_n = nullptr; ///< per-hit LOO residual (mm) vs N_hits — pixel-centre

    // Optional dual/solo-split twins for vs_n observables.  Caller
    // sets to either the _dual or _solo hist of the appropriate ring
    // slot based on the (frame_has_second_ring) predicate.
    TH2F *h_R_vs_nhits_split = nullptr;
    TH2F *h_residual_vs_n_split = nullptr;
    TH1F *h_radial_split = nullptr; ///< dual/solo split of h_radial
    TH1F *h_R_split = nullptr;      ///< dual/solo split of h_R

    //  Smeared (pixel-jittered) sibling histograms — physics path.
    //  When non-null, the corresponding observable is filled twice per
    //  hit: once into the pixel-centre hist (above) and once into the
    //  smeared sibling (below).  Smeared fills smooth out the discrete
    //  pixel-lattice "comb" in the radial distribution and give the
    //  CB+pol3 / σ-vs-N fits a continuous distribution to converge on.
    TH1F *h_radial_smeared = nullptr;
    TH1F *h_radial_split_smeared = nullptr;
    TH2F *h_residual_vs_n_smeared = nullptr;
    TH2F *h_residual_vs_n_split_smeared = nullptr;
};

} // namespace btana::recodata
