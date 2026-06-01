#pragma once

/**
 * @file include/utility/radiator_efficiency.h
 * @brief Geometric coverage map + radial efficiency helpers for the
 *        Cherenkov-radiator photon-counting pipeline.
 *
 * Live-at-beam-test QA: given the channel-to-position map already used
 * by `lightdata_writer` and `recodata_writer`, compute:
 *
 *   1. A 2D `(φ, R)` coverage map of the channel geometry —
 *      `build_coverage_map`.
 *   2. A 1D radial efficiency `eff(R)` derived from the map by averaging
 *      over φ — `radial_efficiency`.
 *   3. A per-ring scalar `f_coverage(cx, cy, R)` = fraction of the ring's
 *      `2π` arc that intersects active channels — `azimuthal_coverage_fraction`.
 *
 * The first two are run-level (geometry-only, computed once at writer
 * init from the same `index_to_hit_xy` map both writers already build).
 * The third is per-ring and uses the per-event Hough/fit centre so the
 * resulting `N_photons = N_hits / f_coverage` is exact per event.
 *
 * Centre conventions (see DISCUSSION.md § 2.6):
 *   - `eff(R)` uses a **fixed nominal centre** (typically the detector
 *     centre `(0, 0)` or the beam-axis projection).  The macro's
 *     historic convention, preserved for direct comparison with offline
 *     analyses.
 *   - `(R, φ)` of an individual hit and `azimuthal_coverage_fraction`
 *     use the **per-event ring centre** (fit-refined Hough centre, or
 *     the Hough peak when no fit was run).
 *
 * Ported from the offline `photon_number_new.cpp` —
 * specifically the `radial_efficiency` function and the start-of-spill
 * coverage-map fill loop.  No φ-gap masking (kPhiGapRanges in the macro)
 * — that's deferred to a finer-analysis follow-up.
 */

#include <array>
#include <map>
#include <vector>

class TAxis;
class TH1F;
class TH2F;

namespace util::radiator_efficiency
{

/**
     * @brief Build a `(φ, R)` coverage map from the channel-position map.
     *
     * Each channel at `(xc, yc)` is mapped to its `(R, φ)` centre
     * relative to `(centre_x, centre_y)`, and the `(φ, R)` bins inside
     * its `(2 × channel_half_width_mm)²` footprint receive +1.  The
     * result is a 2D histogram where bin content = number of channels
     * whose pixel overlaps that `(φ, R)` cell — a pure-geometry coverage
     * map, no spill weighting, no dead-channel masking.
     *
     * Bin value of 0 = no detector at that `(φ, R)`; positive values
     * indicate active geometry (typical values: 1, occasionally 2 at
     * pixel-tile boundaries).
     *
     * @param channel_xy            Map from LUT key to channel `(x, y)` [mm].
     *                              The same map both writers already build.
     * @param n_phi_bins            Azimuthal bin count over `[-π, π]`.
     *                              Typical: 360 (1°/bin).
     * @param r_min_mm              Lower R edge of the map [mm].
     * @param r_max_mm              Upper R edge of the map [mm].
     * @param n_r_bins              Radial bin count over `[r_min, r_max]`.
     * @param channel_half_width_mm Half-side of the channel pixel
     *                              footprint [mm]; default 1.5 (3 mm
     *                              pitch).
     * @param centre_x              Nominal centre X for `(R, φ)`
     *                              computation [mm].  Default 0.
     * @param centre_y              Nominal centre Y [mm].  Default 0.
     * @param min_channel_r_mm      Skip channels whose own
     *                              `r_channel < this` from the
     *                              nominal centre.  Default 0 = no
     *                              filter.  Use this to drop bogus
     *                              positions from `Mapping` that
     *                              would otherwise produce a
     *                              "low-R bump" in the coverage map.
     * @param channel_weights       Optional per-channel weight (lookup
     *                              by the same key as `channel_xy`).
     *                              If supplied AND the channel's key
     *                              is in this map, each (φ, R) bin
     *                              the channel covers is incremented
     *                              by `channel_weights[key]` instead
     *                              of by 1.  Channels NOT in this
     *                              map are silently skipped (so a
     *                              partial map functions as a
     *                              channel-mask).  If `nullptr`
     *                              (default), every channel
     *                              contributes +1 per covered bin
     *                              (legacy V1 behaviour — geometric
     *                              max coverage).
     *
     *                              Spill-by-spill active-channel
     *                              weighting uses
     *                              this parameter: for each channel,
     *                              `weight = Σ over spills active in
     *                              of (n_physics_spill / n_physics_total)`.
     * @return New `TH2F` (owned by the caller — typically pushed into a
     *         `RootHist` wrapper for output).  Title axes:
     *         `";#phi (rad);R (mm)"`.
     */
TH2F *build_coverage_map(
    const std::map<int, std::array<float, 2>> &channel_xy,
    int n_phi_bins,
    float r_min_mm,
    float r_max_mm,
    int n_r_bins,
    float channel_half_width_mm = 1.5f,
    float centre_x = 0.f,
    float centre_y = 0.f,
    float min_channel_r_mm = 0.f,
    const std::map<int, float> *channel_weights = nullptr);

/**
     * @brief Collapse a `(φ, R)` coverage map to a 1D `eff(R)` curve.
     *
     * Averages the coverage map over **all** φ bins (no in-gap / ex-gap
     * split in V1 — the macro's `kPhiGapRanges` are a finer-analysis
     * concern, deferred).  Each output R bin gets the mean coverage of
     * the φ slice at that R, normalised so a fully-covered radius
     * returns ~1.
     *
     * @param coverage_map           2D `(φ, R)` map from
     *                               @ref build_coverage_map.
     * @param radial_reference_axis  Output R binning.  Typically the
     *                               x-axis of the per-ring radial hist
     *                               (so `eff(R)` and the radial hist
     *                               share binning and can be `Divide`d).
     * @return Owned `TH1F` of `eff(R)`.  Bin value in `[0, 1]` for
     *         standard coverage; >1 only at pixel-tile boundaries
     *         where channels physically overlap (rare).
     */
TH1F *radial_efficiency(
    const TH2F *coverage_map,
    const TAxis *radial_reference_axis);

/**
 * @brief Cartesian (x, y) sibling of @ref build_coverage_map.
 *
 * Rasterises the same per-channel pixel footprints (square of
 * `±channel_half_width_mm` around each channel's `(x, y)`), weighted by
 * the same optional per-channel `channel_weights` (spill-activity
 * fraction ∈ [0, 1]; unmapped/dead channels skipped) onto a cartesian
 * `(c_x, c_y)` grid instead of the polar `(φ, R)` one.  No Jacobian, so
 * the footprint test is a plain bounding-box containment.  Bin value =
 * Σ weight of channels covering that bin — a per-bin coverage/readiness
 * fraction (≈ activity fraction when one channel maps to a bin).  Title
 * axes: `";c_{x} (mm);c_{y} (mm)"`.  Caller owns the returned `TH2F`.
 */
TH2F *build_coverage_map_xy(
    const std::map<int, std::array<float, 2>> &channel_xy,
    int n_x_bins,
    float x_min_mm,
    float x_max_mm,
    int n_y_bins,
    float y_min_mm,
    float y_max_mm,
    float channel_half_width_mm = 1.5f,
    const std::map<int, float> *channel_weights = nullptr);

/**
     * @brief Per-ring azimuthal coverage fraction `f ∈ [0, 1]`.
     *
     * For a ring of radius `R` centred at `(cx, cy)`, returns the
     * fraction of its `2π` arc that intersects active channels (within
     * `±delta_r_mm` of the arc).  Use as
     *
     *     N_photons = N_hits / f_coverage
     *
     * to recover the geometry-corrected photon count per ring.
     *
     * Implementation: iterates the channel map, accumulates the
     * azimuthal segments where each channel covers the arc, merges
     * overlapping segments, returns `total_covered_phi / (2π)`.  Cost
     * `O(N_channels × log N_channels)` per ring per event; negligible
     * at typical scales (~1 µs per ring at 2000 channels).
     *
     * @param channel_xy  Channel-position map.
     * @param cx          Ring centre X [mm] (per-event, e.g. fit-refined).
     * @param cy          Ring centre Y [mm].
     * @param R           Ring radius [mm].
     * @param delta_r_mm  Channel-on-arc bandwidth [mm].  Typically
     *                    matches `collection_radius` from the upstream
     *                    Hough config.
     * @param channel_half_width_mm  Pixel half-width [mm] used to convert a
     *                    channel into an angular φ-span at its radius.  Pass
     *                    `RecodataConfigStruct::channel_half_width_mm` so
     *                    f_coverage and `build_coverage_map` share one value.
     * @return Coverage fraction in `[0, 1]`.  Returns 0 if `R <= 0`,
     *         `delta_r_mm <= 0`, or no channel intersects the arc.
     */
float azimuthal_coverage_fraction(
    const std::map<int, std::array<float, 2>> &channel_xy,
    float cx,
    float cy,
    float R,
    float delta_r_mm,
    float channel_half_width_mm = 1.5f);

} // namespace util::radiator_efficiency
