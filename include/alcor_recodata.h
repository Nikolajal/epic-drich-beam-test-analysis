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
#include "alcor_finedata.h"
#include "parallel_streaming_framer.h"

/**
 * @file alcor_recodata.h
 * @brief Reconstructed hit data and analysis utilities for the ALCOR detector.
 *
 * This file defines the @ref alcor_recodata management class.  It provides the full
 * pipeline from calibrated hit storage through coordinate transforms, mask manipulation,
 * trigger handling, ROOT tree I/O, and ring-finding algorithms (DBSCAN and Hough transform).
 *
 * ### Data model
 * Hits are stored directly as @ref alcor_finedata_struct PODs — the same type used by
 * the Lightdata layer — eliminating the former @c alcor_recodata_struct wrapper.
 * A full @ref alcor_finedata object (with calibration/timing/channel-decode methods)
 * is constructed on demand via @ref get_finedata; because @ref alcor_finedata has no
 * heap members this is essentially a free struct copy.
 *
 * Calibrated timing (@ref get_hit_t) depends on the static calibration table loaded via
 * @ref alcor_finedata::read_calib_from_file.  Without a loaded calibration the method
 * returns 0 by design, which allows re-calibration without re-processing raw data.
 */

// ============================================================
//  alcor_recodata
// ============================================================

/**
 * @brief Container and analysis engine for a collection of reconstructed ALCOR hits.
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

    std::vector<alcor_finedata_struct> recodata;                  ///< Owned hit collection.
    std::vector<alcor_finedata_struct> *recodata_ptr = &recodata; ///< Active pointer (may point to external storage).

    // ---- Hough-transform internals ----------------------------------------
    std::vector<float> hough_r_bins; ///< Radial bin centres used during Hough voting.
    float hough_cell_size = 3.2f;    ///< Accumulator cell size in the (x, y) plane [mm].
    float hough_x_min;               ///< Lower x boundary of the Hough accumulator [mm].
    float hough_x_max;               ///< Upper x boundary of the Hough accumulator [mm].
    float hough_y_min;               ///< Lower y boundary of the Hough accumulator [mm].
    float hough_y_max;               ///< Upper y boundary of the Hough accumulator [mm].
    int hough_nx;                    ///< Number of accumulator cells along x.
    int hough_ny;                    ///< Number of accumulator cells along y.
    std::vector<int> hough_accum;    ///< Flat 3-D accumulator [iR * nx*ny + iy * nx + ix].
    /// Pre-computed LUT: global_index → per-R-bin → flat accumulator cell indices.
    std::unordered_map<int, std::vector<std::vector<int>>> hough_lut;

public:
    // ================================================================
    //  Constructors
    // ================================================================

    /// @brief Default constructor — creates an empty container.
    alcor_recodata() = default;

    /**
     * @brief Construct a container pre-filled with an existing hit vector.
     * @param d  Vector of @ref alcor_finedata_struct hits to copy.
     */
    explicit alcor_recodata(const std::vector<alcor_finedata_struct> &d);

    // ================================================================
    /** @name Core Access
     *  Direct access to the underlying storage and the finedata factory.
     */
    ///@{

    /// @brief Return a copy of the full hit vector.
    inline std::vector<alcor_finedata_struct> get_recodata() const { return recodata; }

    /// @brief Return the raw pointer to the active hit vector (may be external).
    inline std::vector<alcor_finedata_struct> *get_recodata_ptr() { return recodata_ptr; }

    /// @brief Return a copy of the hit struct at index @p i.
    inline alcor_finedata_struct get_recodata(int i) const { return recodata[i]; }

    /// @brief Return a mutable reference to the full hit vector.
    inline std::vector<alcor_finedata_struct> &get_recodata_link() { return recodata; }

    /// @brief Return a mutable reference to the hit struct at index @p i.
    inline alcor_finedata_struct &get_recodata_link(int i) { return recodata[i]; }

    /**
     * @brief Construct and return a full @ref alcor_finedata object for hit @p i.
     *
     * Construction from @ref alcor_finedata_struct is a plain struct copy — no heap
     * allocation.  Use this whenever calibrated timing or channel-decode methods are
     * needed.  Timing results depend on the static calibration table; without it the
     * methods return 0 (see class-level note on re-calibration).
     *
     * @param i  Zero-based hit index.
     */
    inline alcor_finedata get_finedata(int i) const { return alcor_finedata(recodata[i]); }

    /// @brief Return a copy of the full trigger vector.
    inline std::vector<trigger_event> get_triggers() const { return triggers; }

    /// @brief Return the raw pointer to the active trigger vector (may be external).
    inline std::vector<trigger_event> *get_triggers_ptr() { return triggers_ptr; }

    /// @brief Return a mutable reference to the full trigger vector.
    inline std::vector<trigger_event> &get_triggers_link() { return triggers; }

    ///@}

    // ================================================================
    /** @name Per-hit Field Getters
     *  Convenience wrappers forwarding to the stored @ref alcor_finedata_struct
     *  fields or constructing a temporary @ref alcor_finedata for derived quantities.
     */
    ///@{

    /// @brief Global channel index for hit @p i.
    inline uint32_t get_global_index(int i) const { return recodata[i].global_index; }

    /// @brief Hit x-coordinate from channel mapping [mm].
    inline float get_hit_x(int i) const { return recodata[i].hit_x; }

    /// @brief Hit y-coordinate from channel mapping [mm].
    inline float get_hit_y(int i) const { return recodata[i].hit_y; }

    /// @brief Hit quality / classification bitmask.
    inline uint32_t get_hit_mask(int i) const { return recodata[i].hit_mask; }

    /**
     * @brief Calibrated hit time [ns] for hit @p i.
     *
     * Delegates to @ref alcor_finedata::get_time_ns.  Returns 0 if no calibration
     * is loaded — intentional, to allow re-calibration without re-processing.
     */
    inline float get_hit_t(int i) const { return get_finedata(i).get_time_ns(); }

    /// @brief Radial distance from the detector origin [mm].
    inline float get_hit_r(int i) const { return get_hit_r(i, {0.f, 0.f}); }

    /// @brief Radial distance from a custom centre @p v [mm].
    inline float get_hit_r(int i, std::array<float, 2> v) const { return std::hypot(get_hit_x(i) - v[0], get_hit_y(i) - v[1]); }

    /// @brief Azimuthal angle from the detector origin [rad].
    inline float get_hit_phi(int i) const { return get_hit_phi(i, {0.f, 0.f}); }

    /// @brief Azimuthal angle from a custom centre @p v [rad].
    inline float get_hit_phi(int i, std::array<float, 2> v) const { return std::atan2(get_hit_y(i) - v[1], get_hit_x(i) - v[0]); }

    /// @brief Pixel-randomised x-coordinate (uniform ±1.5 mm jitter within the pixel cell).
    inline float get_hit_x_rnd(int i) const { return recodata[i].hit_x + (_rnd_(_global_gen_) * 3.0f - 1.5f); }

    /// @brief Pixel-randomised y-coordinate (uniform ±1.5 mm jitter within the pixel cell).
    inline float get_hit_y_rnd(int i) const { return recodata[i].hit_y + (_rnd_(_global_gen_) * 3.0f - 1.5f); }

    /// @brief Radial distance from the origin using randomised coordinates.
    inline float get_hit_r_rnd(int i) const { return get_hit_r_rnd(i, {0.f, 0.f}); }

    /// @brief Radial distance from @p v using randomised coordinates.
    inline float get_hit_r_rnd(int i, std::array<float, 2> v) const { return std::hypot(get_hit_x_rnd(i) - v[0], get_hit_y_rnd(i) - v[1]); }

    /// @brief Azimuthal angle from the origin using randomised coordinates [rad].
    inline float get_hit_phi_rnd(int i) const { return get_hit_phi_rnd(i, {0.f, 0.f}); }

    /// @brief Azimuthal angle from @p v using randomised coordinates [rad].
    inline float get_hit_phi_rnd(int i, std::array<float, 2> v) const { return std::atan2(get_hit_y_rnd(i) - v[1], get_hit_x_rnd(i) - v[0]); }

    ///@}

    // ================================================================
    /** @name Channel Decode Getters
     *  All forward to the corresponding @ref alcor_finedata methods via @ref get_finedata.
     */
    ///@{

    /// @brief TDC sub-channel index for hit @p i.
    inline int get_hit_tdc(int i) const { return get_finedata(i).get_tdc(); }

    /// @brief Readout device ID for hit @p i.
    inline int get_device(int i) const { return get_finedata(i).get_device(); }

    /// @brief FIFO number for hit @p i.
    inline int get_fifo(int i) const { return get_finedata(i).get_fifo(); }

    /// @brief Lane number for hit @p i.
    inline int get_lane(int i) const { return get_fifo(i); }

    /// @brief Chip ID for hit @p i.
    inline int get_chip(int i) const { return get_finedata(i).get_chip(); }

    /// @brief Even/odd channel index for hit @p i.
    inline int get_eo_channel(int i) const { return get_finedata(i).get_eo_channel(); }

    /// @brief Column address for hit @p i.
    inline int get_column(int i) const { return get_finedata(i).get_column(); }

    /// @brief Pixel address for hit @p i.
    inline int get_pixel(int i) const { return get_finedata(i).get_pixel(); }

    /// @brief Per-device flat index for hit @p i.
    inline int get_device_index(int i) const { return get_finedata(i).get_device_index(); }

    /// @brief Global channel index stripped of TDC info for hit @p i.
    inline int get_global_channel_index(int i) const { return get_finedata(i).get_global_channel_index(); }

    ///@}

    // ================================================================
    /** @name Trigger Access
     */
    ///@{

    /**
     * @brief Look up the first trigger whose index field matches @p index.
     * @return The matching @ref trigger_event wrapped in @c std::optional,
     *         or @c std::nullopt if absent.
     */
    std::optional<trigger_event> get_trigger_by_index(uint8_t index) const;

    /// @brief Return the timing trigger for this event, if present.
    inline std::optional<trigger_event> get_timing_trigger() const { return get_trigger_by_index(_TRIGGER_TIMING_); }

    ///@}

    // ================================================================
    /** @name Setters
     */
    ///@{

    /// @brief Replace the entire hit collection.
    inline void set_recodata(std::vector<alcor_finedata_struct> v) { recodata = v; }

    /// @brief Replace the hit at index @p i.
    inline void set_recodata(int i, alcor_finedata_struct v) { recodata[i] = v; }

    /// @brief Replace the entire trigger collection.
    inline void set_triggers(const std::vector<trigger_event> v) { triggers = v; }

    /// @brief Redirect the active hit pointer to an external vector.
    inline void set_recodata_ptr(std::vector<alcor_finedata_struct> *v) { recodata_ptr = v; }

    /// @brief Redirect the active trigger pointer to an external vector.
    inline void set_triggers_ptr(std::vector<trigger_event> *v) { triggers_ptr = v; }

    /// @brief Overwrite the global channel index of hit @p i.
    inline void set_global_index(int i, uint32_t v) { recodata[i].global_index = v; }

    /// @brief Overwrite the x-coordinate of hit @p i [mm].
    inline void set_hit_x(int i, float v) { recodata[i].hit_x = v; }

    /// @brief Overwrite the y-coordinate of hit @p i [mm].
    inline void set_hit_y(int i, float v) { recodata[i].hit_y = v; }

    /// @brief Overwrite the quality bitmask of hit @p i (full replacement).
    inline void set_hit_mask(int i, uint32_t v) { recodata[i].hit_mask = v; }

    /// @brief Rebind the active hit pointer to @p v (copies vector and rebinds pointer).
    void set_recodata_link(std::vector<alcor_finedata_struct> &v);

    /// @brief Rebind the active trigger pointer to @p v (copies vector and rebinds pointer).
    void set_triggers_link(std::vector<trigger_event> &v);

    ///@}

    // ================================================================
    /** @name Hit and Trigger Builders
     */
    ///@{

    /// @brief OR @p v into the bitmask of hit @p i.
    inline void add_hit_mask(int i, uint32_t v) { recodata[i].hit_mask |= v; }

    /// @brief Set the bit at position @p v in the bitmask of hit @p i.
    inline void add_hit_mask_bit(int i, uint32_t v) { recodata[i].hit_mask |= encode_bit(v); }

    /// @brief Append a trigger from its constituent fields.
    inline void add_trigger(uint8_t index, uint16_t coarse, float fine_time = 0.) { triggers.emplace_back(index, coarse, fine_time); }

    /// @brief Append a pre-built @ref trigger_event.
    inline void add_trigger(trigger_event hit) { triggers.push_back(hit); }

    /// @brief Append a hit from an @ref alcor_finedata_struct.
    inline int add_hit(const alcor_finedata_struct &hit)
    {
        recodata_ptr->push_back(hit);
        return recodata_ptr->size() - 1;
    }

    /// @brief Append a hit from an @ref alcor_finedata object (stores its underlying struct).
    inline int add_hit(const alcor_finedata &hit)
    {
        recodata_ptr->push_back(hit.get_data());
        return recodata_ptr->size() - 1;
    }

    /// @brief Append a hit from individual fields (constructed in-place).
    inline int add_hit(uint32_t rollover,
                       uint16_t coarse,
                       uint8_t fine,
                       float hit_x,
                       float hit_y,
                       uint32_t global_index,
                       uint32_t hit_mask)
    {
        recodata_ptr->emplace_back(rollover, coarse, fine, hit_x, hit_y, global_index, hit_mask);
        return recodata_ptr->size() - 1;
    }

    ///@}

    // ================================================================
    /** @name Boolean Checks
     */
    ///@{

    /// @brief True if a trigger with index @p v exists for this event.
    inline bool check_trigger(uint8_t v) { return get_trigger_by_index(v).has_value(); }

    /// @brief True if the start-of-spill trigger is present.
    inline bool is_start_of_spill() { return check_trigger(_TRIGGER_START_OF_SPILL_); }

    /// @brief True if the first-frames trigger is present.
    inline bool is_first_frames() { return check_trigger(_TRIGGER_FIRST_FRAMES_); }

    /// @brief True if a timing trigger is present.
    inline bool is_timing_available() { return check_trigger(_TRIGGER_TIMING_); }

    /// @brief True if embedded tracking data are attached to this event.
    inline bool is_embedded_tracking_available() { return check_trigger(_TRIGGER_TRACKING_); }

    /// @brief True if at least one ring has been reconstructed.
    inline bool is_ring_found() { return check_trigger(_TRIGGER_RING_FOUND_); }

    /// @brief True if any bit of @p v is set in the mask of hit @p i.
    inline bool check_hit_mask(int i, uint32_t v) { return (get_hit_mask(i) & v) != 0; }

    /// @brief True if hit @p i is flagged as an afterpulse (delegates to @ref alcor_finedata).
    inline bool is_afterpulse(int i) { return get_finedata(i).is_afterpulse(); }

    /// @brief True if hit @p i is flagged as optical cross-talk (delegates to @ref alcor_finedata).
    inline bool is_cross_talk(int i) { return get_finedata(i).is_cross_talk(); }

    /// @brief True if hit @p i has been associated with a reconstructed ring.
    inline bool is_ring_tagged(int i) { return check_hit_mask(i, encode_bits({_HITMASK_ring_tag_first, _HITMASK_ring_tag_second})); }

    ///@}

    // ================================================================
    /** @name I/O Utilities
     */
    ///@{

    /**
     * @brief Clear all hits and triggers, resetting the container to an empty state.
     * Does not reset the Hough LUT or accumulator configuration.
     */
    void clear();

    /**
     * @brief Attach the container to branches of an existing input @p TTree.
     * @param input_tree  Source tree; must remain valid for the lifetime of any GetEntry calls.
     */
    bool link_to_tree(TTree *input_tree);

    /**
     * @brief Create branches in @p output_tree for hits and triggers.
     * @param output_tree  Destination tree.
     */
    void write_to_tree(TTree *output_tree);

    ///@}

    // ================================================================
    /** @name Analysis Utilities
     */
    ///@{

    /**
     * @brief Cluster hits into ring candidates using DBSCAN in the (R, t) plane.
     *
     * DBSCAN (Density-Based Spatial Clustering of Applications with Noise) groups
     * hits that are close in both radial distance R and time t.  Hits assigned to a
     * cluster are tagged with the ring flag; if any cluster is found the event-level
     * ring trigger is added.
     *
     * @param distance_length_cut  Maximum ΔR between neighbouring hits [mm].
     * @param distance_time_cut    Maximum Δt between neighbouring hits [ns].
     */
    void find_rings(float_t distance_length_cut, float_t distance_time_cut);

    /**
     * @brief Pre-compute the Hough-transform look-up table (LUT).
     *
     * The Hough transform votes for ring centre candidates: for each hit position
     * and candidate radius R, the set of accumulator cells consistent with that
     * (hit, R) pair is pre-computed once here and reused per event.  Call once per
     * run (or whenever the geometry changes) before @ref find_rings_hough.
     *
     * @param index_to_hit_xy  Map from global channel index to (x, y) position [mm].
     * @param r_min            Minimum candidate ring radius [mm].
     * @param r_max            Maximum candidate ring radius [mm].
     * @param r_step           Radius step size [mm].
     * @param cell_size        Linear size of each accumulator cell [mm].
     */
    void build_hough_lut(const std::map<int, std::array<float, 2>> &index_to_hit_xy,
                         float r_min, float r_max, float r_step, float cell_size);

    /**
     * @brief Find ring candidates using the pre-computed Hough LUT.
     *
     * Each hit votes for the accumulator cells stored in the LUT.  Cells exceeding
     * the threshold are declared ring candidates; contributing hits and the
     * event-level ring trigger are updated accordingly.
     *
     * @pre @ref build_hough_lut must have been called with geometry consistent with
     *      the current event data.
     *
     * @param threshold_fraction  Minimum fraction of active hits required in a peak
     *                            cell to be accepted as a ring centre (range 0–1).
     * @param min_hits            Minimum absolute vote count for acceptance.
     */
    void find_rings_hough(float threshold_fraction, int min_hits);

    ///@}
};