#pragma once

#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <optional>
#include <cmath>
#include <limits>
#include "utility.h"

/**
 * @file hough-transform.h
 * @brief Circular Hough-transform ring-finder
 *
 * The class operates on @ref hough_hit — a plain POD struct that the caller
 * populates from their hit representation.
 *
 * ### Two-phase workflow
 *  1. **Setup** (once per run / geometry change) — call @ref build_lut with
 *     the channel-to-position map to pre-compute which accumulator cells each
 *     LUT key votes for at every candidate radius.
 *  2. **Per-event processing** — call @ref find_rings with a vector of
 *     @ref hough_hit.  The method votes, finds the best peak, removes the
 *     tagged hits from the active set, resets the accumulator, and re-votes
 *     for the next ring.  Returns a summary of all rings found.
 *
 * ### Ring-extraction logic
 * After the first ring is found its contributing hits are removed from the
 * active set and the accumulator is reset to zero before searching for the
 * next ring.  This avoids the spatial-suppression artefacts that arise when
 * two rings are close together.  The acceptance threshold for each pass is
 * re-evaluated against the number of hits still active at that point.
 *
 * ### Typical usage
 * @code{.cpp}
 * hough_transform ht;
 *
 * // --- Once per run ---
 * std::map<int, std::array<float,2>> geometry = load_geometry();
 * ht.build_lut(geometry, 30.f, 80.f, 1.f, 3.2f);
 *
 * // --- Per event (generic) ---
 * std::vector<hough_hit> hits = make_hough_hits(raw_hits);
 * auto rings = ht.find_rings(hits, 0.3f, 5);
 *
 * @endcode
 */

// ============================================================
//  Generic hit type
// ============================================================

/**
 * @brief Minimal hit descriptor consumed by @ref hough_transform.
 *
 * The caller is responsible for populating this struct from whatever
 * detector-specific hit representation is in use.  The @c lut_key must
 * match the keys used when building the LUT via @ref hough_transform::build_lut.
 */
struct hough_hit
{
    float x;     ///< Hit x-position in the detector plane [mm].
    float y;     ///< Hit y-position in the detector plane [mm].
    float time;  ///< Calibrated hit time [ns].
    int lut_key; ///< Key into the LUT — typically `global_channel_index / 4`.
};

// ============================================================
//  Result type
// ============================================================

/**
 * @brief Describes a single ring candidate found by @ref hough_transform::find_rings.
 */
struct hough_ring_result
{
    float cx;        ///< x-coordinate of the reconstructed ring centre [mm].
    float cy;        ///< y-coordinate of the reconstructed ring centre [mm].
    float radius;    ///< Reconstructed ring radius [mm].
    int peak_votes;  ///< Number of votes in the winning accumulator cell.
    float mean_time; ///< Mean hit time of the hits associated with this ring [ns].

    /// @brief Indices into the input @ref hough_hit vector of hits assigned to this ring.
    std::vector<int> hit_indices;
};

// ============================================================
//  hough_transform
// ============================================================

/**
 * @brief Circular Hough-transform ring-finder operating on @ref hough_hit collections.
 *
 * The algorithm works in the (x, y) detector plane.  For a given candidate
 * radius R and a hit at position (hx, hy), the set of possible ring centres
 * lies on a circle of radius R centred at (hx, hy).  The accumulator counts
 * how many hit arcs pass through each (cx, cy, R) cell; a high-count cell
 * indicates a real ring.
 *
 * To avoid per-event arc-drawing the class pre-computes a look-up table (LUT)
 * that maps each integer LUT key to the flat accumulator cell indices it votes
 * for at every R bin.  The LUT depends only on detector geometry and must be
 * rebuilt with @ref build_lut whenever the geometry changes.
 *
 * ### Thread safety
 * The LUT is immutable after @ref build_lut returns and may be read from
 * multiple threads concurrently.  The per-event accumulator @ref hough_accum
 * is mutated during @ref find_rings and is **not** thread-safe; use separate
 * @ref hough_transform instances per thread.
 */
class hough_transform
{
public:
    // ================================================================
    //  Constructors
    // ================================================================

    /// @brief Default constructor — creates an uninitialised finder.
    /// @note @ref build_lut must be called before @ref find_rings.
    hough_transform() = default;

    /**
     * @brief Convenience constructor that immediately builds the LUT.
     *
     * @param index_to_hit_xy  Map from LUT key to (x, y) [mm].
     * @param r_min            Minimum ring radius [mm].
     * @param r_max            Maximum ring radius [mm].
     * @param r_step           Radial bin step [mm].
     * @param cell_size        Accumulator cell size in (x, y) [mm].
     */
    hough_transform(const std::map<int, std::array<float, 2>> &index_to_hit_xy,
                    float r_min, float r_max, float r_step, float cell_size);

    // ================================================================
    /** @name LUT Construction */
    ///@{

    /**
     * @brief Pre-compute the Hough-transform look-up table.
     *
     * For every key in @p index_to_hit_xy and every radius bin the method
     * determines which accumulator cells lie within half a cell width of the
     * corresponding arc and stores their flat indices.
     *
     * The accumulator grid is derived automatically from the bounding box of
     * all hit positions, padded by @p r_max so that ring centres outside the
     * active area are reachable.
     *
     * @param index_to_hit_xy  Map from LUT key to hit position [mm].
     * @param r_min            Minimum ring radius [mm].
     * @param r_max            Maximum ring radius [mm].
     * @param r_step           Step between candidate radii [mm].
     * @param cell_size        Linear size of each accumulator cell [mm].
     */
    void build_lut(const std::map<int, std::array<float, 2>> &index_to_hit_xy,
                   float r_min, float r_max, float r_step, float cell_size);

    /**
     * @brief Return whether the LUT has been built and is ready for use.
     */
    bool is_lut_ready() const { return !hough_lut.empty() && !hough_r_bins.empty(); }

    ///@}

    // ================================================================
    /** @name Per-event Ring Finding */
    ///@{

    /**
     * @brief Find ring candidates in a vector of @ref hough_hit.
     *
     * For each pass:
     *  1. Vote using the active hit set and the pre-computed LUT.
     *  2. Find the global accumulator peak.
     *  3. Collect hits within @p collection_radius of the ring arc.
     *  4. Remove those hits from the active set.
     *  5. Reset the accumulator and repeat from step 1 for the next ring.
     *
     * The acceptance threshold is re-evaluated against the number of hits
     * still active at the start of each pass, so a small second ring is not
     * penalised by the size of the full event.
     *
     * @param hits                  Hit vector for the current event (read-only).
     * @param threshold_fraction    Minimum fraction of currently-active hits
     *                              required in the peak cell (range 0–1).
     * @param min_hits              Minimum absolute vote count for acceptance.
     * @param max_rings             Maximum number of rings to extract (default 2).
     * @param collection_radius     Distance from the ring arc within which a hit
     *                              is assigned to the ring [mm] (default 6).
     * @return                      Vector of @ref hough_ring_result in
     *                              descending peak-vote order.
     */
    std::vector<hough_ring_result> find_rings(const std::vector<hough_hit> &hits,
                                              float threshold_fraction,
                                              int min_hits,
                                              int min_active,
                                              int max_rings = 2,
                                              float collection_radius = 6.f);

    ///@}

    // ================================================================
    /** @name Accumulator Accessors */
    ///@{

    /// @brief Flat accumulator array after the last @ref find_rings call.
    /// Layout: `accum[iR * nx * ny + iy * nx + ix]`.
    const std::vector<int> &get_accumulator() const { return hough_accum; }

    /// @brief Radial bin centres [mm].
    const std::vector<float> &get_r_bins() const { return hough_r_bins; }

    int get_nx() const { return hough_nx; }
    int get_ny() const { return hough_ny; }
    float get_x_min() const { return hough_x_min; }
    float get_y_min() const { return hough_y_min; }
    float get_cell_size() const { return hough_cell_size; }

    ///@}

private:
    // ================================================================
    //  Accumulator geometry
    // ================================================================

    float hough_cell_size = 3.2f;
    float hough_x_min = 0.f;
    float hough_x_max = 0.f;
    float hough_y_min = 0.f;
    float hough_y_max = 0.f;
    int hough_nx = 0;
    int hough_ny = 0;

    // ================================================================
    //  LUT and accumulator storage
    // ================================================================

    std::vector<float> hough_r_bins;
    std::vector<int> hough_accum;

    /**
     * @brief Pre-computed look-up table.
     *
     * `hough_lut[lut_key][r_bin_index]` → vector of flat cell indices
     * that this key votes for at that radius bin.
     */
    std::unordered_map<int, std::vector<std::vector<int>>> hough_lut;

    // ================================================================
    //  Private helpers
    // ================================================================

    /**
     * @brief Fill the accumulator from a subset of hits and return the
     *        index of the global maximum.
     *
     * Iterates over @p active_indices, looks up each hit's LUT entry, and
     * increments the corresponding accumulator cells.  Tracks the running
     * maximum so no second scan is needed.
     *
     * @param hits            Full hit vector.
     * @param active_indices  Indices into @p hits to vote with.
     * @param[out] best_iR    Radial bin index of the maximum cell.
     * @param[out] best_cell  Flat (iy * nx + ix) index of the maximum cell.
     * @return                Vote count of the maximum cell.
     */
    int vote_and_find_peak(const std::vector<hough_hit> &hits,
                           const std::vector<int> &active_indices,
                           int &best_iR, int &best_cell);

    /**
     * @brief Collect hits within @p collection_radius of a ring arc.
     *
     * @param hits              Full hit vector.
     * @param active_indices    Candidate indices to test.
     * @param cx                Ring centre x [mm].
     * @param cy                Ring centre y [mm].
     * @param R                 Ring radius [mm].
     * @param collection_radius Acceptance half-width around the arc [mm].
     * @return                  Populated @ref hough_ring_result.
     */
    hough_ring_result collect_ring_hits(const std::vector<hough_hit> &hits,
                                        const std::vector<int> &active_indices,
                                        float cx, float cy, float R,
                                        float collection_radius) const;
};