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
 * @file AlcorRecodata.h
 * @brief Reconstructed Hit data and analysis utilities for the ALCOR detector.
 *
 * This file defines the @ref AlcorRecodata management class.  It provides the full
 * pipeline from calibrated Hit storage through coordinate transforms, mask manipulation,
 * trigger handling, ROOT tree I/O, and ring-finding algorithms (DBSCAN and Hough transform).
 *
 * ### Data model
 * Hits are stored directly as @ref AlcorFinedataStruct PODs — the same type used by
 * the Lightdata layer — eliminating the former @c AlcorRecodataStruct wrapper.
 * A full @ref AlcorFinedata object (with calibration/timing/channel-decode methods)
 * is constructed on demand via @ref get_finedata; because @ref AlcorFinedata has no
 * heap members this is essentially a free struct copy.
 *
 * Calibrated timing (@ref get_hit_t) depends on the static calibration table loaded via
 * @ref AlcorFinedata::read_calib_from_file.  Without a loaded calibration the method
 * returns 0 by design, which allows re-calibration without re-processing raw data.
 */

// ============================================================
//  AlcorRecodata
// ============================================================

/**
 * @brief Container and analysis engine for a collection of reconstructed ALCOR hits.
 *
 * ### Ownership and pointer semantics
 * The class owns its data through internal `std::vector` members.  After
 * @ref link_to_tree the @c recodata_ptr / @c triggers_ptr slots hold the
 * address of those owned vectors; ROOT's `TTree::SetBranchAddress` is bound
 * to the address of the *_ptr slot (`&recodata_ptr`).
 *
 * ### Non-copyable, non-movable
 * ROOT branches bind to the address of the wrapper's @c _ptr slots.  Any
 * copy or move of @c AlcorRecodata would either duplicate the slot value
 * (pointing into the source's freed memory after destruction) or relocate
 * the slot (leaving ROOT's branch binding dangling).  Hold by reference,
 * by @c std::unique_ptr, or as a class member.  Same contract as
 * @ref AlcorSpilldata.
 */
class AlcorRecodata
{
public:
    AlcorRecodata(const AlcorRecodata &) = delete;
    AlcorRecodata &operator=(const AlcorRecodata &) = delete;
    AlcorRecodata(AlcorRecodata &&) = delete;
    AlcorRecodata &operator=(AlcorRecodata &&) = delete;

private:
    std::vector<TriggerEvent> triggers;                  ///< Owned trigger collection.
    std::vector<TriggerEvent> *triggers_ptr = &triggers; ///< Branch-address pointer slot — points at the owned vector for the wrapper's lifetime (non-movable class).

    std::vector<AlcorFinedataStruct> recodata;                  ///< Owned Hit collection.
    std::vector<AlcorFinedataStruct> *recodata_ptr = &recodata; ///< Branch-address pointer slot — points at the owned vector for the wrapper's lifetime.

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
    /// Pre-computed LUT: GlobalIndex → per-R-bin → flat accumulator cell indices.
    std::unordered_map<int, std::vector<std::vector<int>>> hough_lut;

public:
    // ================================================================
    //  Constructors
    // ================================================================

    /// @brief Default constructor — creates an empty container.
    AlcorRecodata() = default;

    /**
     * @brief Construct a container pre-filled with an existing Hit vector.
     * @param d  Vector of @ref AlcorFinedataStruct hits to copy.
     */
    explicit AlcorRecodata(const std::vector<AlcorFinedataStruct> &d);

    // ================================================================
    /** @name Core Access
     *  Direct access to the underlying storage and the finedata factory.
     */
    ///@{

    /// @brief Read-only access to the full Hit vector (by reference — no copy).
    inline const std::vector<AlcorFinedataStruct> &get_recodata() const { return recodata; }

    /// @brief Return the raw pointer slot used by ROOT for branch binding.
    inline std::vector<AlcorFinedataStruct> *get_recodata_ptr() { return recodata_ptr; }

    /// @brief Read-only access to the Hit struct at index @p i (by reference — no copy).
    inline const AlcorFinedataStruct &get_recodata(int i) const { return recodata[i]; }

    /// @brief Return a mutable reference to the full Hit vector.
    inline std::vector<AlcorFinedataStruct> &get_recodata_link() { return recodata; }

    /// @brief Return a mutable reference to the Hit struct at index @p i.
    inline AlcorFinedataStruct &get_recodata_link(int i) { return recodata[i]; }

    /**
     * @brief Construct and return a full @ref AlcorFinedata object for Hit @p i.
     *
     * Construction from @ref AlcorFinedataStruct is a plain struct copy — no heap
     * allocation.  Use this whenever calibrated timing or channel-decode methods are
     * needed.  Timing results depend on the static calibration table; without it the
     * methods return 0 (see class-level note on re-calibration).
     *
     * @param i  Zero-based Hit index.
     */
    inline AlcorFinedata get_finedata(int i) const { return AlcorFinedata(recodata[i]); }

    /// @brief Read-only access to the full trigger vector (by reference — no copy).
    inline const std::vector<TriggerEvent> &get_triggers() const { return triggers; }

    /// @brief Return the raw pointer slot used by ROOT for branch binding.
    inline std::vector<TriggerEvent> *get_triggers_ptr() { return triggers_ptr; }

    /// @brief Return a mutable reference to the full trigger vector.
    inline std::vector<TriggerEvent> &get_triggers_link() { return triggers; }

    ///@}

    // ================================================================
    /** @name Per-Hit Field Getters
     *  Convenience wrappers forwarding to the stored @ref AlcorFinedataStruct
     *  fields or constructing a temporary @ref AlcorFinedata for derived quantities.
     */
    ///@{

    /// @brief Global channel index for Hit @p i.
    inline uint32_t get_global_index(int i) const { return recodata[i].GlobalIndex; }

    /// @brief Hit x-coordinate from channel Mapping [mm].
    inline float get_hit_x(int i) const { return recodata[i].hit_x; }

    /// @brief Hit y-coordinate from channel Mapping [mm].
    inline float get_hit_y(int i) const { return recodata[i].hit_y; }

    /// @brief Hit quality / classification bitmask.
    inline uint32_t get_hit_mask(int i) const { return recodata[i].HitMask; }

    /**
     * @brief Calibrated Hit time [ns] for Hit @p i.
     *
     * Delegates to @ref AlcorFinedata::get_time_ns.  Returns 0 if no calibration
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
    ///
    /// Distribution is cached `static thread_local` to avoid per-call construction —
    /// this getter sits in hot lightdata_writer loops; see the matching note on
    /// `AlcorFinedata::get_hit_x_rnd`.
    inline float get_hit_x_rnd(int i) const
    {
        thread_local mist::Rnd rng;
        static thread_local std::uniform_real_distribution<float> pixel_jitter(-1.5f, 1.5f);
        return recodata[i].hit_x + pixel_jitter(rng.engine());
    }

    /// @brief Pixel-randomised y-coordinate (uniform ±1.5 mm jitter within the pixel cell).
    inline float get_hit_y_rnd(int i) const
    {
        thread_local mist::Rnd rng;
        static thread_local std::uniform_real_distribution<float> pixel_jitter(-1.5f, 1.5f);
        return recodata[i].hit_y + pixel_jitter(rng.engine());
    }

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
     *  All forward to the corresponding @ref AlcorFinedata methods via @ref get_finedata.
     */
    ///@{

    /// @brief TDC sub-channel index for Hit @p i.
    inline int get_hit_tdc(int i) const { return get_finedata(i).get_tdc(); }

    /// @brief Readout device ID for Hit @p i.
    inline int get_device(int i) const { return get_finedata(i).get_device(); }

    /// @brief FIFO number for Hit @p i.
    inline int get_fifo(int i) const { return get_finedata(i).get_fifo(); }

    /// @brief Lane number for Hit @p i.
    inline int get_lane(int i) const { return get_fifo(i); }

    /// @brief Chip ID for Hit @p i.
    inline int get_chip(int i) const { return get_finedata(i).get_chip(); }

    /// @brief Even/odd channel index for Hit @p i.
    inline int get_eo_channel(int i) const { return get_finedata(i).get_eo_channel(); }

    /// @brief Column address for Hit @p i.
    inline int get_column(int i) const { return get_finedata(i).get_column(); }

    /// @brief Pixel address for Hit @p i.
    inline int get_pixel(int i) const { return get_finedata(i).get_pixel(); }

    /// @brief Per-device flat index for Hit @p i.
    inline int get_device_index(int i) const { return get_finedata(i).get_device_index(); }

    /// @brief Global channel index stripped of TDC info for Hit @p i.
    inline int get_global_channel_index(int i) const { return get_finedata(i).get_global_channel_index(); }

    ///@}

    // ================================================================
    /** @name Trigger Access
     */
    ///@{

    /**
     * @brief Look up the first trigger whose index field matches @p index.
     * @return The matching @ref TriggerEvent wrapped in @c std::optional,
     *         or @c std::nullopt if absent.
     */
    std::optional<TriggerEvent> get_trigger_by_index(uint8_t index) const;

    /// @brief Return the timing trigger for this event, if present.
    inline std::optional<TriggerEvent> get_timing_trigger() const { return get_trigger_by_index(TriggerTiming); }

    ///@}

    // ================================================================
    /** @name Setters
     *
     * The previous by-value `set_recodata(...)`, `set_triggers(...)`,
     * `set_recodata_link(...)`, `set_triggers_link(...)` methods are
     * removed — no callers, and they either copy
     * multi-MB containers per call or invite the F1/F2/F3 branch-address
     * traps now blocked by the non-copyable contract.
     *
     * The `set_*_ptr` family is **retained** because @ref AlcorRecotrackdata
     * uses it to alias its parent @ref AlcorRecodata's vectors so each
     * `tree->GetEntry()` propagates through both wrappers.  External code
     * should not call these directly — the parent-aliasing pattern is a
     * narrow internal contract enforced by the non-copyable invariant on
     * both wrappers (caller is responsible for outliving the parent).
     */
    ///@{

    /// @brief Alias the branch-binding pointer slot to an external vector
    ///        (internal API used by @ref AlcorRecotrackdata to share its
    ///        parent's data).  Caller MUST ensure the source object
    ///        outlives this one.
    inline void set_recodata_ptr(std::vector<AlcorFinedataStruct> *v) { recodata_ptr = v; }

    /// @brief Same as @ref set_recodata_ptr but for the trigger vector.
    inline void set_triggers_ptr(std::vector<TriggerEvent> *v) { triggers_ptr = v; }

    /// @brief Overwrite the global channel index of Hit @p i.
    inline void set_global_index(int i, uint32_t v) { recodata[i].GlobalIndex = v; }

    /// @brief Overwrite the x-coordinate of Hit @p i [mm].
    inline void set_hit_x(int i, float v) { recodata[i].hit_x = v; }

    /// @brief Overwrite the y-coordinate of Hit @p i [mm].
    inline void set_hit_y(int i, float v) { recodata[i].hit_y = v; }

    /// @brief Overwrite the quality bitmask of Hit @p i (full replacement).
    inline void set_hit_mask(int i, uint32_t v) { recodata[i].HitMask = v; }

    ///@}

    // ================================================================
    /** @name Hit and Trigger Builders
     */
    ///@{

    /// @brief OR @p v into the bitmask of Hit @p i.
    inline void add_hit_mask(int i, uint32_t v) { recodata[i].HitMask |= v; }

    /// @brief Set the bit at position @p v in the bitmask of Hit @p i.
    inline void add_hit_mask_bit(int i, uint32_t v) { recodata[i].HitMask |= encode_bit(v); }

    /// @brief Append a trigger from its constituent fields.
    inline void add_trigger(uint8_t index, uint16_t coarse, float fine_time = 0.) { triggers.emplace_back(index, coarse, fine_time); }

    /// @brief Append a pre-built @ref TriggerEvent.
    inline void add_trigger(TriggerEvent Hit) { triggers.push_back(Hit); }

    /// @brief Append a Hit from an @ref AlcorFinedataStruct.
    inline int add_hit(const AlcorFinedataStruct &Hit)
    {
        recodata_ptr->push_back(Hit);
        return recodata_ptr->size() - 1;
    }

    /// @brief Append a Hit from an @ref AlcorFinedata object (stores its underlying struct).
    inline int add_hit(const AlcorFinedata &Hit)
    {
        recodata_ptr->push_back(Hit.get_data());
        return recodata_ptr->size() - 1;
    }

    /// @brief Append a Hit from individual fields (constructed in-place).
    inline int add_hit(uint32_t rollover,
                       uint16_t coarse,
                       uint8_t fine,
                       float hit_x,
                       float hit_y,
                       uint32_t GlobalIndex,
                       uint32_t HitMask)
    {
        recodata_ptr->emplace_back(rollover, coarse, fine, hit_x, hit_y, GlobalIndex, HitMask);
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
    inline bool is_start_of_spill() { return check_trigger(TriggerStartOfSpill); }

    /// @brief True if the first-frames trigger is present.
    inline bool is_first_frames() { return check_trigger(TriggerFirstFrames); }

    /// @brief True if a timing trigger is present.
    inline bool is_timing_available() { return check_trigger(TriggerTiming); }

    /// @brief True if embedded tracking data are attached to this event.
    inline bool is_embedded_tracking_available() { return check_trigger(TriggerTracking); }

    /// @brief True if at least one ring has been reconstructed.
    inline bool is_ring_found() { return check_trigger(TriggerRingFound); }

    /// @brief True if any bit of @p v is set in the mask of Hit @p i.
    inline bool check_hit_mask(int i, uint32_t v) { return (get_hit_mask(i) & v) != 0; }

    /// @brief True if Hit @p i is flagged as an afterpulse (delegates to @ref AlcorFinedata).
    inline bool is_afterpulse(int i) { return get_finedata(i).is_afterpulse(); }

    /// @brief True if Hit @p i is flagged as optical cross-talk (delegates to @ref AlcorFinedata).
    inline bool is_cross_talk(int i) { return get_finedata(i).is_cross_talk(); }

    /// @brief True if Hit @p i has been associated with a reconstructed ring.
    /// Body in alcor_recodata.cxx — uses HitMask enum constants from alcor_data.h.
    bool is_ring_tagged(int i);

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
     * The Hough transform votes for ring centre candidates: for each Hit position
     * and candidate radius R, the set of accumulator cells consistent with that
     * (Hit, R) pair is pre-computed once here and reused per event.  Call once per
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
     * Each Hit votes for the accumulator cells stored in the LUT.  Cells exceeding
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