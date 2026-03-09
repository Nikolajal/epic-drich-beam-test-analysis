#pragma once

#include <iostream>
#include <vector>
#include <array>
#include <map>
#include <cmath>
#include <sstream>
#include "TTree.h"
#include "triggers.h"
#include "alcor_spilldata.h"
#include "parallel_streaming_framer.h"

/**
 * @file alcor_recodata.h
 * @brief Reconstructed hit data structures and analysis utilities for the ALCOR detector.
 *
 * This file defines the @ref alcor_recodata_struct POD type and the @ref alcor_recodata
 * management class. Together they provide the full pipeline from raw hit storage through
 * coordinate transforms, mask manipulation, trigger handling, ROOT tree I/O, and ring-finding
 * algorithms (DBSCAN and Hough transform).
 */

// ============================================================
//  alcor_recodata_struct
// ============================================================

/**
 * @brief Plain-old-data (POD) representation of a single reconstructed detector hit.
 *
 * Each hit carries a global channel identifier, three-dimensional coordinates (spatial + time),
 * and a bitmask that encodes hit quality flags set during or after reconstruction
 * (e.g. afterpulse, cross-talk, ring-tagged).
 *
 * @note Coordinates use the detector-local frame.  Time @p hit_t is expressed in nanoseconds
 *       relative to the event reference.
 *
 * @see alcor_recodata for the container class that manages collections of these structs.
 */
struct alcor_recodata_struct
{
    uint32_t global_index; ///< Flat channel index encoding device / FIFO / chip / pixel address.
    float hit_x;           ///< Hit position along the x-axis [mm].
    float hit_y;           ///< Hit position along the y-axis [mm].
    float hit_t;           ///< Hit time relative to the event reference time [ns].
    uint32_t hit_mask;     ///< Bitmask of quality and classification flags (afterpulse, cross-talk, ring, …).

    /// @brief Default constructor — leaves all members value-initialised to zero.
    alcor_recodata_struct() = default;

    /**
     * @brief Construct a hit with explicit field values.
     *
     * @param gi    Global channel index (see @p global_index).
     * @param x     Hit x-coordinate [mm].
     * @param y     Hit y-coordinate [mm].
     * @param mask  Initial hit quality bitmask.
     * @param ht    Hit time [ns].
     */
    alcor_recodata_struct(uint32_t gi, float x, float y, uint32_t mask, float ht)
        : global_index(gi), hit_x(x), hit_y(y), hit_t(ht), hit_mask(mask) {}
};

// ============================================================
//  alcor_recodata
// ============================================================

/**
 * @brief Container and analysis engine for a collection of reconstructed ALCOR hits.
 *
 * @p alcor_recodata owns (or can borrow via pointer) a vector of @ref alcor_recodata_struct
 * entries and an associated vector of @ref trigger_event objects for the same event.
 * It exposes:
 *
 *  - **Pure getters** – read-only access to individual hit fields and derived quantities.
 *  - **Reference getters** – mutable references for in-place modification.
 *  - **Coordinate utilities** – Cartesian, polar (r, φ), and pixel-randomised variants.
 *  - **Channel decode helpers** – extract device, FIFO, chip, column, and pixel from the
 *    packed @p global_index.
 *  - **Setters / mutators** – write individual fields or replace the entire collection.
 *  - **Hit / trigger builders** – append new hits or triggers to the container.
 *  - **Boolean checks** – test mask bits, spill flags, trigger availability, and ring results.
 *  - **ROOT I/O** – bind to or write into a @p TTree for file-based storage.
 *  - **Ring-finding algorithms** – DBSCAN-based and Hough-transform-based ring reconstruction.
 *
 * ### Ownership and pointer semantics
 * By default the class owns its data through internal `std::vector` members.  The
 * `set_*_ptr` / `set_*_link` family of methods allows an external vector to be adopted,
 * which is useful when the data are managed by a ROOT branch.  After calling
 * `link_to_tree()` the internal pointers are re-targeted to ROOT's branch buffers and
 * must not be invalidated while the tree is in use.
 *
 * @warning Mixing owned and borrowed storage (i.e. calling `set_recodata_ptr` after
 *          `link_to_tree`) leads to undefined behaviour.
 */
class alcor_recodata
{
private:
    std::vector<trigger_event> triggers;                  ///< Owned trigger collection.
    std::vector<trigger_event> *triggers_ptr = &triggers; ///< Active pointer (may point to external storage).

    std::vector<alcor_recodata_struct> recodata;                  ///< Owned hit collection.
    std::vector<alcor_recodata_struct> *recodata_ptr = &recodata; ///< Active pointer (may point to external storage).

    // ---- Hough-transform internals ----------------------------------------
    std::vector<float> hough_r_bins; ///< Radial bin centres used during Hough voting.
    float hough_cell_size = 3.2f;    ///< Accumulator cell size in the (x, y) plane [mm].
    float hough_x_min;               ///< Lower x boundary of the Hough accumulator [mm].
    float hough_x_max;               ///< Upper x boundary of the Hough accumulator [mm].
    float hough_y_min;               ///< Lower y boundary of the Hough accumulator [mm].
    float hough_y_max;               ///< Upper y boundary of the Hough accumulator [mm].
    int hough_nx;                    ///< Number of accumulator cells along x.
    int hough_ny;                    ///< Number of accumulator cells along y.
    std::vector<int> hough_accum;    ///< Accumulator for votes.
    /// Pre-computed look-up table: global_index → per-R-bin → flat accumulator cell indices.
    std::unordered_map<int, std::vector<std::vector<int>>> hough_lut;

public:
    // ================================================================
    //  Constructors
    // ================================================================

    /// @brief Default constructor — creates an empty container.
    alcor_recodata() = default;

    /**
     * @brief Construct a container pre-filled with an existing hit vector.
     * @param d  Vector of hits to copy into the container.
     */
    explicit alcor_recodata(const std::vector<alcor_recodata_struct> &d);

    // ================================================================
    /** @name Pure Getters
     *  Read-only access to the underlying data.  All index parameters @p i are
     *  zero-based and must satisfy <tt>0 <= i < get_recodata().size()</tt>.
     */
    ///@{

    /// @brief Return a copy of the full hit vector.
    std::vector<alcor_recodata_struct> get_recodata() const;

    /// @brief Return the raw pointer to the active hit vector (may be external).
    std::vector<alcor_recodata_struct> *get_recodata_ptr();

    /**
     * @brief Return a copy of the hit at index @p i.
     * @param i  Zero-based hit index.
     */
    alcor_recodata_struct get_recodata(int i) const;

    /// @brief Return a copy of the full trigger vector.
    std::vector<trigger_event> get_triggers() const;

    /// @brief Return the raw pointer to the active trigger vector (may be external).
    std::vector<trigger_event> *get_triggers_ptr();

    /**
     * @brief Return the packed global channel index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_global_index(int i) const;

    /**
     * @brief Return the x-coordinate of hit @p i [mm].
     * @param i  Zero-based hit index.
     */
    float get_hit_x(int i) const;

    /**
     * @brief Return the y-coordinate of hit @p i [mm].
     * @param i  Zero-based hit index.
     */
    float get_hit_y(int i) const;

    /**
     * @brief Return the time of hit @p i relative to the event reference [ns].
     * @param i  Zero-based hit index.
     */
    float get_hit_t(int i) const;

    /**
     * @brief Return the quality bitmask of hit @p i.
     * @param i  Zero-based hit index.
     */
    uint32_t get_hit_mask(int i) const;

    ///@}

    // ================================================================
    /** @name Reference Getters
     *  Mutable references into the container.  These allow in-place
     *  modification without copying.
     */
    ///@{

    /// @brief Return a mutable reference to the full hit vector.
    std::vector<alcor_recodata_struct> &get_recodata_link();

    /**
     * @brief Return a mutable reference to the hit at index @p i.
     * @param i  Zero-based hit index.
     */
    alcor_recodata_struct &get_recodata_link(int i);

    /// @brief Return a mutable reference to the full trigger vector.
    std::vector<trigger_event> &get_triggers_link();

    ///@}

    // ================================================================
    /** @name Derived / Coordinate Utilities
     *  Functions that compute derived quantities from stored hit coordinates,
     *  including polar coordinates and pixel-randomised variants.
     */
    ///@{

    /**
     * @brief Return the radial distance of hit @p i from the detector origin.
     * @param i  Zero-based hit index.
     * @return   r = sqrt(x² + y²) [mm].
     */
    float get_hit_r(int i) const;

    /**
     * @brief Return the radial distance of hit @p i from a custom centre.
     * @param i  Zero-based hit index.
     * @param v  Centre coordinates {x₀, y₀} [mm].
     * @return   r = sqrt((x-x₀)² + (y-y₀)²) [mm].
     */
    float get_hit_r(int i, std::array<float, 2> v) const;

    /**
     * @brief Return the azimuthal angle φ of hit @p i from the detector origin.
     * @param i  Zero-based hit index.
     * @return   φ = atan2(y, x) [rad], in the range (−π, π].
     */
    float get_hit_phi(int i) const;

    /**
     * @brief Return the azimuthal angle φ of hit @p i from a custom centre.
     * @param i  Zero-based hit index.
     * @param v  Centre coordinates {x₀, y₀} [mm].
     * @return   φ = atan2(y−y₀, x−x₀) [rad].
     */
    float get_hit_phi(int i, std::array<float, 2> v) const;

    /**
     * @brief Return the pixel-randomised x-coordinate of hit @p i.
     *
     * Adds a uniform random offset within the physical pixel cell to break
     * the discrete lattice structure when plotting or fitting.
     * @param i  Zero-based hit index.
     */
    float get_hit_x_rnd(int i) const;

    /**
     * @brief Return the pixel-randomised y-coordinate of hit @p i.
     * @param i  Zero-based hit index.
     */
    float get_hit_y_rnd(int i) const;

    /**
     * @brief Return the radial distance computed from randomised coordinates.
     * @param i  Zero-based hit index.
     */
    float get_hit_r_rnd(int i) const;

    /**
     * @brief Return the radial distance from @p v computed from randomised coordinates.
     * @param i  Zero-based hit index.
     * @param v  Centre coordinates {x₀, y₀} [mm].
     */
    float get_hit_r_rnd(int i, std::array<float, 2> v) const;

    /**
     * @brief Return the azimuthal angle from randomised coordinates.
     * @param i  Zero-based hit index.
     * @return   φ [rad].
     */
    float get_hit_phi_rnd(int i) const;

    /**
     * @brief Return the azimuthal angle from @p v using randomised coordinates.
     * @param i  Zero-based hit index.
     * @param v  Centre coordinates {x₀, y₀} [mm].
     * @return   φ [rad].
     */
    float get_hit_phi_rnd(int i, std::array<float, 2> v) const;

    /**
     * @brief Return the TDC sub-channel index encoded in @p global_index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_hit_tdc(int i) const;

    /**
     * @brief Return the device number decoded from @p global_index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_device(int i) const;

    /**
     * @brief Return the FIFO number decoded from @p global_index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_fifo(int i) const;

    /**
     * @brief Return the chip number decoded from @p global_index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_chip(int i) const;

    /**
     * @brief Return the even/odd channel index decoded from @p global_index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_eo_channel(int i) const;

    /**
     * @brief Return the column address decoded from @p global_index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_column(int i) const;

    /**
     * @brief Return the pixel address decoded from @p global_index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_pixel(int i) const;

    /**
     * @brief Return the calibration look-up index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_calib_index(int i) const;

    /**
     * @brief Return the per-device flat index for hit @p i.
     * @param i  Zero-based hit index.
     */
    int get_device_index(int i) const;

    /**
     * @brief Look up the first trigger whose index field matches @p index.
     *
     * @param index  Trigger index to search for.
     * @return       The matching @ref trigger_event wrapped in `std::optional`,
     *               or `std::nullopt` if no match was found.
     *
     * @todo Return a vector containing **all** triggers with matching index.
     * @todo Add an overload supporting (anti-)coincidence conditions.
     */
    std::optional<trigger_event> get_trigger_by_index(uint8_t index) const;

    /**
     * @brief Return the dedicated timing trigger for this event, if present.
     *
     * Searches the trigger vector for the trigger designated as the timing
     * reference.
     * @return  The timing trigger wrapped in `std::optional`,
     *          or `std::nullopt` if it has not been recorded for this event.
     */
    std::optional<trigger_event> get_timing_trigger() const;

    ///@}

    // ================================================================
    /** @name Pure Setters
     *  Replace stored values without returning references.
     */
    ///@{

    /**
     * @brief Replace the entire hit collection with @p v.
     * @param v  New hit vector (copied).
     */
    void set_recodata(std::vector<alcor_recodata_struct> v);

    /**
     * @brief Replace the hit at index @p i with @p v.
     * @param i  Zero-based hit index.
     * @param v  Replacement hit struct.
     */
    void set_recodata(int i, alcor_recodata_struct v);

    /**
     * @brief Replace the entire trigger collection with @p v.
     * @param v  New trigger vector (copied).
     */
    void set_triggers(const std::vector<trigger_event> v);

    /**
     * @brief Redirect the active hit pointer to an external vector.
     *
     * After this call the container no longer owns the hit data.
     * The caller is responsible for keeping @p v alive as long as the container is in use.
     * @param v  Pointer to an externally managed hit vector.
     */
    void set_recodata_ptr(std::vector<alcor_recodata_struct> *v);

    /**
     * @brief Redirect the active trigger pointer to an external vector.
     *
     * @param v  Pointer to an externally managed trigger vector.
     */
    void set_triggers_ptr(std::vector<trigger_event> *v);

    /**
     * @brief Overwrite the @p global_index field of hit @p i.
     * @param i  Zero-based hit index.
     * @param v  New packed global channel index.
     */
    void set_global_index(int i, uint32_t v);

    /**
     * @brief Overwrite the x-coordinate of hit @p i.
     * @param i  Zero-based hit index.
     * @param v  New x-coordinate [mm].
     */
    void set_hit_x(int i, float v);

    /**
     * @brief Overwrite the y-coordinate of hit @p i.
     * @param i  Zero-based hit index.
     * @param v  New y-coordinate [mm].
     */
    void set_hit_y(int i, float v);

    /**
     * @brief Overwrite the time of hit @p i.
     * @param i  Zero-based hit index.
     * @param v  New time [ns].
     */
    void set_hit_t(int i, float v);

    /**
     * @brief Overwrite the quality bitmask of hit @p i.
     * @param i  Zero-based hit index.
     * @param v  New bitmask (replaces the existing value entirely).
     */
    void set_hit_mask(int i, uint32_t v);

    ///@}

    // ================================================================
    /** @name Reference Setters
     *  Rebind the active data pointer to an existing vector by reference.
     */
    ///@{

    /**
     * @brief Rebind the active hit pointer to @p v.
     * @param v  Reference to an existing hit vector; lifetime must exceed the container's.
     */
    void set_recodata_link(std::vector<alcor_recodata_struct> &v);

    /**
     * @brief Rebind the active trigger pointer to @p v.
     * @param v  Reference to an existing trigger vector; lifetime must exceed the container's.
     */
    void set_triggers_link(std::vector<trigger_event> &v);

    ///@}

    // ================================================================
    /** @name Hit and Trigger Builders
     *  Append new entries to the active collections.
     */
    ///@{

    /**
     * @brief Apply a bitwise OR of @p v to the bitmask of hit @p i.
     *
     * Use this to set multiple flag bits at once.
     * @param i  Zero-based hit index.
     * @param v  Mask bits to OR in.
     */
    void add_hit_mask(int i, uint32_t v);

    /**
     * @brief Set a single flag bit identified by position @p v in the bitmask of hit @p i.
     * @param i  Zero-based hit index.
     * @param v  Bit position (0-based) to set.
     */
    void add_hit_mask_bit(int i, uint32_t v);

    /**
     * @brief Append a new trigger built from its constituent fields.
     * @param index      Trigger type / index identifier.
     * @param coarse     Coarse timestamp counter value.
     * @param fine_time  Optional fine-time correction [ns] (default 0).
     */
    void add_trigger(uint8_t index, uint16_t coarse, float fine_time = 0.);

    /**
     * @brief Append an already-constructed trigger struct to the trigger collection.
     * @param hit  Fully populated @ref trigger_event to append.
     */
    void add_trigger(trigger_event hit);

    /**
     * @brief Append a new hit built from its constituent fields.
     * @param gi    Global channel index.
     * @param x     Hit x-coordinate [mm].
     * @param y     Hit y-coordinate [mm].
     * @param mask  Initial quality bitmask.
     * @param ht    Hit time [ns].
     */
    void add_hit(uint32_t gi, float x, float y, uint32_t mask, float ht);

    /**
     * @brief Append an already-constructed hit struct to the hit collection.
     * @param hit  Fully populated @ref alcor_recodata_struct to append.
     */
    void add_hit(alcor_recodata_struct hit);

    ///@}

    // ================================================================
    /** @name Boolean Checks
     *  Predicates on the event-level and hit-level state.
     */
    ///@{

    /**
     * @brief Return whether a trigger with the given index exists for this event.
     * @param v  Trigger index to test.
     * @return   @c true if at least one trigger with index @p v is present.
     */
    bool check_trigger(uint8_t v);

    /**
     * @brief Return whether this event is flagged as the start of a spill.
     *
     * Start-of-spill events enumerate all available channels and can therefore
     * be used to track spill boundaries and monitor channel availability over time.
     *
     * @return @c true if the start-of-spill flag is set.
     */
    bool is_start_of_spill();

    /**
     * @brief Return whether this event belongs to the first frames of a spill.
     * @return @c true if the first-frames flag is set.
     */
    bool is_first_frames();

    /**
     * @brief Return whether a timing trigger is available for this event.
     * @return @c true if a timing trigger is present in the trigger collection.
     */
    bool is_timing_available();

    /**
     * @brief Return whether embedded tracking information is available for this event.
     * @return @c true if tracking data have been attached to the event.
     */
    bool is_embedded_tracking_available();

    /**
     * @brief Return whether at least one ring has been reconstructed for this event.
     * @return @c true if the ring-found flag is set (populated by @ref find_rings or
     *         @ref find_rings_hough).
     */
    bool is_ring_found();

    /**
     * @brief Test whether any of the bits in @p v are set in the bitmask of hit @p i.
     * @param i  Zero-based hit index.
     * @param v  Bitmask to test against.
     * @return   @c true if `(hit_mask[i] & v) != 0`.
     */
    bool check_hit_mask(int i, uint32_t v);

    /**
     * @brief Return whether hit @p i is flagged as an afterpulse.
     *
     * During reconstruction, consecutive hits on the same channel are compared:
     * if a hit arrives within 200 ns of the preceding hit on the same channel,
     * it is flagged as an afterpulse.
     *
     * @param i  Zero-based hit index.
     * @return   @c true if the afterpulse flag is set.
     */
    bool is_afterpulse(int i);

    /**
     * @brief Return whether hit @p i is flagged as optical cross-talk.
     * @param i  Zero-based hit index.
     * @return   @c true if the cross-talk flag is set.
     */
    bool is_cross_talk(int i);

    /**
     * @brief Return whether hit @p i has been associated with a reconstructed ring.
     *
     * This flag is set by @ref find_rings or @ref find_rings_hough when a hit
     * is assigned to a ring candidate that satisfies the reconstruction criteria.
     *
     * @param i  Zero-based hit index.
     * @return   @c true if the ring-tagged flag is set.
     */
    bool is_ring_tagged(int i);

    ///@}

    // ================================================================
    /** @name I/O Utilities
     *  ROOT @p TTree integration and container housekeeping.
     */
    ///@{

    /**
     * @brief Clear all hits and triggers, resetting the container to an empty state.
     *
     * Resets both the owned vectors and the active pointers to point at them.
     * Does **not** reset the Hough LUT or accumulator configuration.
     */
    void clear();

    /**
     * @brief Attach the container to branches of an existing input @p TTree.
     *
     * Sets branch addresses so that subsequent calls to `TTree::GetEntry()` populate
     * the internal hit and trigger vectors directly.
     *
     * @param input_tree  Pointer to the source @p TTree.  Must remain valid for the
     *                    lifetime of any subsequent `GetEntry` calls.
     */
    void link_to_tree(TTree *input_tree);

    /**
     * @brief Create branches in @p output_tree and register the container's vectors.
     *
     * After this call, `TTree::Fill()` will serialise the current hit and trigger
     * collections into the tree.
     *
     * @param output_tree  Pointer to the destination @p TTree.
     */
    void write_to_tree(TTree *output_tree);

    ///@}

    // ================================================================
    /** @name Analysis Utilities
     *  Ring-finding algorithms operating on the stored hit collection.
     */
    ///@{

    /**
     * @brief Cluster hits into ring candidates using a custom DBSCAN algorithm.
     *
     * Implements a density-based spatial clustering of applications with noise
     * (DBSCAN, see https://en.wikipedia.org/wiki/DBSCAN) adapted to operate in
     * the (R, t) plane, where R is the radial distance from a reference centre and
     * t is the hit time.  Hits assigned to a cluster are tagged with the ring flag
     * (see @ref is_ring_tagged); if any cluster is found the event-level ring flag
     * is set (see @ref is_ring_found).
     *
     * @param distance_length_cut  Maximum spatial separation in R between two hits
     *                             for them to be considered neighbours [mm].
     * @param distance_time_cut    Maximum temporal separation between two hits for
     *                             them to be considered neighbours [ns].
     */
    void find_rings(float_t distance_length_cut, float_t distance_time_cut);

    /**
     * @brief Pre-compute the Hough-transform look-up table (LUT).
     *
     * For each channel present in @p index_to_hit_xy, the LUT stores the set of
     * accumulator cells that would be incremented for each candidate ring radius.
     * The LUT is stored internally and consumed by @ref find_rings_hough.
     *
     * Call this once per run (or whenever the geometry changes) before processing
     * events with @ref find_rings_hough.
     *
     * @param index_to_hit_xy  Map from global channel index to hit (x, y) position [mm].
     * @param r_min            Minimum ring radius to consider [mm].
     * @param r_max            Maximum ring radius to consider [mm].
     * @param r_step           Step size between candidate radii [mm].
     * @param cell_size        Linear size of each accumulator cell in the (x, y) plane [mm].
     */
    void build_hough_lut(const std::map<int, std::array<float, 2>> &index_to_hit_xy,
                         float r_min, float r_max, float r_step, float cell_size);

    /**
     * @brief Find ring candidates using the pre-computed Hough-transform LUT.
     *
     * For every hit in the current event, increments the accumulator cells stored
     * in the LUT for the hit's global channel index.  Cells whose accumulated vote
     * count exceeds `threshold_fraction * (total hits)` and that contain at least
     * @p min_hits contributing hits are declared ring candidates.  Tagged hits and
     * the event-level ring flag are updated accordingly.
     *
     * @pre @ref build_hough_lut must have been called with a geometry that is
     *      consistent with the current event data.
     *
     * @param threshold_fraction  Minimum fraction of total hits required in a cell
     *                            for it to be accepted as a ring centre (range: 0–1).
     * @param min_hits            Minimum absolute number of contributing hits required
     *                            for a cell to be accepted as a ring candidate.
     */
    void find_rings_hough(float threshold_fraction, int min_hits);

    ///@}
};