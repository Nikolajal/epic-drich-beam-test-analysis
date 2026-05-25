#pragma once

/**
 * @file AlcorFinedata.h
 * @brief Fine-time-resolved ALCOR Hit with per-channel calibration table.
 *
 * Extends @ref AlcorData with three-parameter sigmoid calibration applied
 * to the raw 0..127 fine-time bin to yield a fractional clock-cycle phase.
 * The class also owns the global calibration store (per-channel parameter
 * maps + the per-channel calibration-method registry); accessors and
 * mutators on those static maps are guarded by an internal
 * @c std::shared_mutex so the framer can build calibrations on one thread
 * while readers query phases on another.
 *
 * @par Thread safety
 * The static @c calibration_parameters, @c channel_calibration_method, and
 * @c default_calibration_method maps are protected by @c calibration_mutex.
 * Public getters take a shared lock; setters and @ref generate_calibration
 * take an exclusive lock.  Do not call public locking accessors while already
 * holding the mutex — @c std::shared_mutex is not reentrant.
 */

#include <mist/ring_finding/hough_transform.h>
#include <mist/rnd.h>
#include <sstream>
#include <cmath>
#include <iostream>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include "TH2F.h"
#include "TF1.h"
#include "TCanvas.h"
#include "alcor_data.h"

/**
     * @brief Raw decoded Hit data from an ALCOR TDC channel.
     *
     * Holds the timing components (rollover, coarse, fine) and the calibration
     * index used to look up the corresponding fine-time calibration parameters.
     *
     * @todo Implement bit-wise manipulation for rollover, fine, and coarse encoding.
     */
struct AlcorFinedataStruct
{
    /// @name Timing Components
    /// @{

    /** @brief Rollover counter (most-significant timing word). */
    uint32_t rollover;

    /** @brief Coarse timestamp (clock-cycle counter). */
    uint16_t coarse;

    /** @brief Fine timestamp (TDC interpolation bin within a clock cycle). */
    uint8_t fine;

    /** @brief X-axis position from Mapping. */
    float hit_x;

    /** @brief Y-axis position from Mapping. */
    float hit_y;

    /// @}

    /// @name Indexing & Masking
    /// @{

    /** @brief Global calibration index identifying the TDC channel. */
    uint32_t GlobalIndex;

    /** @brief Bitmask encoding Hit channel/pixel information. */
    uint32_t HitMask;

    /// @}

    /** @brief Default constructor. */
    AlcorFinedataStruct() = default;

    /**
     * @brief Constructor from individual values.
     */
    AlcorFinedataStruct(uint32_t rollover_,
                          uint16_t coarse_,
                          uint8_t fine_,
                          float hit_x_,
                          float hit_y_,
                          uint32_t global_index_,
                          uint32_t hit_mask_)
        : rollover(rollover_),
          coarse(coarse_),
          fine(fine_),
          hit_x(hit_x_),
          hit_y(hit_y_),
          GlobalIndex(global_index_),
          HitMask(hit_mask_)
    {
    }

    /**
     * @brief Construct from a raw @ref AlcorDataStruct.
     * @param d Raw data word to decode into timing components.
     */
    AlcorFinedataStruct(const AlcorDataStruct &d);
};

// -------------------------------------------------------------------------
// Calibration method enum
// -------------------------------------------------------------------------

/**
 * @brief Selects the algorithm used to compute the fine-time phase correction.
 *
 * AlcorV2BaseCalib  — baseline linear interpolation between the two
 *                          sigmoid inflection points (parameters 0 and 1).
 * AlcorV2FitCalib   — reserved for a future fit-based correction.
 */
enum class CalibrationMethod : uint8_t
{
    AlcorV2BaseCalib = 0, ///< Default: linear interpolation (sigmoid edges).
    AlcorV2FitCalib = 1,  ///< Fit-based phase correction.
};

// =============================================================================

/**
     * @class AlcorFinedata
     * @brief Represents a single calibrated ALCOR TDC Hit with fine-time correction.
     *
     * Wraps an @ref AlcorFinedataStruct and provides:
     * - Accessors for raw timing fields and derived channel-address fields.
     * - A static calibration table (shared across all instances) Mapping global
     *   TDC indices to a 3-parameter calibration array.
     * - Methods to generate, persist, and load the calibration from ROOT histograms
     *   or plain files.
     *
     * The fine-time phase is computed from the calibration parameters stored in
     * the static @c calibration_parameters map.
     */
class AlcorFinedata
{
public:
    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------

    /** @brief Default constructor. Initialises all fields to zero. */
    AlcorFinedata() {}

    /**
     * @brief Construct from a decoded @ref AlcorFinedataStruct.
     * @param s Pre-filled struct with rollover, coarse, fine, and mask.
     */
    AlcorFinedata(const AlcorFinedataStruct &s)
        : internal_data(s) {}

    /**
     * @brief Construct directly from a raw @ref AlcorDataStruct.
     * @param d Raw data word; decoded internally via @ref AlcorFinedataStruct.
     */
    AlcorFinedata(const AlcorDataStruct &d)
        : internal_data(d) {}

    /**
     * @brief Copy constructor.
     * @param o Source object to copy from.
     */
    AlcorFinedata(const AlcorFinedata &o)
        : internal_data(o.get_data()) {}

    /**
     * @brief Construct from individual timing and channel fields.
     * @param rollover_      Rollover counter (most-significant timing word).
     * @param coarse_        Coarse timestamp (clock-cycle counter).
     * @param fine_          Fine timestamp (TDC interpolation bin).
     * @param hit_x_         X-axis position from Mapping.
     * @param hit_y_         Y-axis position from Mapping.
     * @param global_index_  Global calibration index identifying the TDC channel.
     * @param hit_mask_      Bitmask encoding Hit channel/pixel information.
     */
    AlcorFinedata(
        uint32_t rollover_,
        uint16_t coarse_,
        uint8_t fine_,
        float hit_x_,
        float hit_y_,
        uint32_t global_index_,
        uint32_t hit_mask_)
        : internal_data(AlcorFinedataStruct(
              rollover_,
              coarse_,
              fine_,
              hit_x_,
              hit_y_,
              global_index_,
              hit_mask_)) {}

    // -------------------------------------------------------------------------
    // Getters — Raw fields
    // -------------------------------------------------------------------------

    /// @name Raw Field Getters
    /// @{

    /** @brief Returns a copy of the underlying @ref AlcorFinedataStruct. */
    AlcorFinedataStruct get_data() const { return internal_data; }

    /** @brief Returns a reference of the underlying @ref AlcorFinedataStruct. */
    AlcorFinedataStruct &get_data_link()  { return internal_data; }

    /** @brief Returns the calibration index identifying the TDC channel. */
    uint32_t get_global_index() const { return internal_data.GlobalIndex; }

    /** @brief Returns the rollover counter. */
    uint32_t get_rollover() const { return internal_data.rollover; }

    /** @brief Returns the coarse timestamp (clock-cycle count). */
    uint16_t get_coarse() const { return internal_data.coarse; }

    /** @brief Returns the fine timestamp (TDC bin within a clock cycle). */
    uint8_t get_fine() const { return internal_data.fine; }

    /** @brief Returns the x-axis position from Mapping. */
    float get_hit_x() const { return internal_data.hit_x; }

    /** @brief Returns the y-axis position from Mapping. */
    float get_hit_y() const { return internal_data.hit_y; }

    /** @brief Returns the Hit bitmask. */
    uint32_t get_mask() const { return internal_data.HitMask; }

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Derived timing
    // -------------------------------------------------------------------------

    /// @name Derived Timing Getters
    /// @{

    /**
     * @brief Returns the calibrated fine-time phase in clock cycles.
     * Computed from the 3-parameter calibration stored in @c calibration_parameters.
     */
    float get_phase() const;

    /**
     * @brief Returns the calibrated Hit time in clock cycles.
     * Combines rollover, coarse, and the fine-time phase correction.
     * All unsigned fields are promoted to float before arithmetic to avoid
     * unsigned underflow when subtracting the (potentially non-zero) phase.
     */
    float get_time() const { return static_cast<float>(BTANA_ALCOR_ROLLOVER_TO_CC) * static_cast<float>(get_rollover()) + static_cast<float>(get_coarse()) - get_phase(); }

    /**
     * @brief Returns the calibrated Hit time in nanoseconds.
     */
    float get_time_ns() const { return BTANA_ALCOR_CC_TO_NS * get_time(); }

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Derived channel address
    // -------------------------------------------------------------------------

    /// @name Derived Address Getters
    /// @{

    // Phase 5: every derived accessor delegates to the @ref GlobalIndex
    // value type, decoding the stored new-layout raw via direct construction
    // (no @c from_legacy — the storage is now the new layout natively).

    /** @brief Returns the TDC index decoded from the stored global index. */
    int get_tdc() const { return ::GlobalIndex(get_global_index()).tdc(); }

    /** @brief Returns the readout device ID decoded from the stored global index. */
    int get_device() const { return ::GlobalIndex(get_global_index()).device(); }

    /** @brief Returns the FIFO number decoded from the stored global index. */
    int get_fifo() const { return ::GlobalIndex(get_global_index()).fifo(); }

    /** @brief Returns the lane number decoded from the stored global index. */
    int get_lane() const { return get_fifo(); }

    /** @brief Returns the **physical hardware** chip ID (0–7) decoded from the stored global index. */
    int get_chip() const { return ::GlobalIndex(get_global_index()).real_chip(); }

    /** @brief Returns the even/odd channel index (0–63) decoded from the stored global index. */
    int get_eo_channel() const { return ::GlobalIndex(get_global_index()).eo_channel(); }

    /** @brief Returns the column address decoded from the stored global index. */
    int get_column() const { return ::GlobalIndex(get_global_index()).column(); }

    /** @brief Returns the pixel address decoded from the stored global index. */
    int get_pixel() const { return ::GlobalIndex(get_global_index()).pixel(); }

    /** @brief Returns the per-device flat index 0..255. */
    int get_device_index() const { return ::GlobalIndex(get_global_index()).device_index(); }

    /** @brief Returns the dense, counter-style channel ordinal — suitable for
     *         histogram axes that bin one channel per bin.  Matches the legacy
     *         `tdc_raw / 4` value bit-exact for the current detector.
     *
     *  @note  This now returns @ref GlobalIndex::channel_ordinal rather than
     *         the raw `stored / 4` value.  The semantic is identical (dense
     *         per-channel counter); the integer value matches what the legacy
     *         `tdc_raw / 4` pattern returned, so existing hitmap histograms
     *         that bin on this axis carry over unchanged. */
    int get_global_channel_index() const { return ::GlobalIndex(get_global_index()).channel_ordinal(); }

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Spatial (randomised)
    // -------------------------------------------------------------------------

    /// @name Spatial Getters
    /// @{

    /** @brief Returns the pixel-randomised x-coordinate, uniform within ±1.5 mm of the Hit position.
     *
     *  The engine is a `thread_local mist::Rnd` (per-thread safe).  The distribution
     *  object is cached `static thread_local` because this getter is called inside
     *  per-Hit hot loops in `lightdata_writer.cxx` (≥20 sites); constructing a fresh
     *  `std::uniform_real_distribution<float>` per call was measurable in framer-pipeline
     *  runtime.  Hoisting it removes that overhead while keeping the per-thread engine.
     */
    float get_hit_x_rnd() const {
        thread_local mist::Rnd rng;
        static thread_local std::uniform_real_distribution<float> pixel_jitter(-1.5f, 1.5f);
        return internal_data.hit_x + pixel_jitter(rng.engine());
    }

    /** @brief Returns the pixel-randomised y-coordinate, uniform within ±1.5 mm of the Hit position. */
    float get_hit_y_rnd() const {
        thread_local mist::Rnd rng;
        static thread_local std::uniform_real_distribution<float> pixel_jitter(-1.5f, 1.5f);
        return internal_data.hit_y + pixel_jitter(rng.engine());
    }

    /** @brief Returns the radial distance from the origin using a freshly randomised position. */
    float get_hit_r_rnd() const { return get_hit_r_rnd({0.f, 0.f}); }

    /** @brief Returns the azimuthal angle from the origin using a freshly randomised position [rad]. */
    float get_hit_phi_rnd() const { return get_hit_phi_rnd({0.f, 0.f}); }

    /**
     * @brief Returns the radial distance from a custom centre using a freshly randomised position.
     * @param v Centre coordinates {x, y}.
     */
    float get_hit_r_rnd(std::array<float, 2> v) const;

    /**
     * @brief Returns the azimuthal angle from a custom centre using a freshly randomised position [rad].
     * @param v Centre coordinates {x, y}.
     */
    float get_hit_phi_rnd(std::array<float, 2> v) const;

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Hit masks
    // -------------------------------------------------------------------------

    /// @name Hit Mask Flags
    /// @{

    /**
     * @brief Sets a single bit in the Hit mask.
     * @param bit Bit position to set (from @ref HitMask enum).
     */
    void add_mask_bit(HitMask bit) { internal_data.HitMask |= (1u << bit); }

    /**
     * @brief Clears a single bit in the Hit mask.
     * @param bit Bit position to clear (from @ref HitMask enum).
     */
    void clear_mask_bit(HitMask bit) { internal_data.HitMask &= ~(1u << bit); }

    /**
     * @brief Checks whether a single bit is set in the Hit mask.
     * @param bit Bit position to check (from @ref HitMask enum).
     * @return true if the bit is set.
     */
    bool has_mask_bit(HitMask bit) const { return (internal_data.HitMask >> bit) & 1u; }

    /** @brief Checks whether the Hit is tagged as part of a ring (first pass). */
    bool is_ring_tag_first() const { return has_mask_bit(HitmaskRingTagFirst); }

    /** @brief Checks whether the Hit is tagged as part of a ring (second pass). */
    bool is_ring_tag_second() const { return has_mask_bit(HitmaskRingTagSecond); }

    /** @brief Checks whether the Hit is flagged as cross-talk. */
    bool is_cross_talk() const { return has_mask_bit(HitmaskCrossTalk); }

    /** @brief Checks whether the Hit is flagged as an afterpulse (Δt < framer @c afterpulse_deadtime). */
    bool is_afterpulse() const { return has_mask_bit(HitmaskAfterpulse); }

    /**
     * @brief True if Δt to the previous same-channel Hit lies in the QA near window
     *        (afterpulse-signal region).
     *
     * Independent of @ref is_afterpulse — that flag uses the framer deadtime and gates
     * downstream CT / recodata logic. This flag is set per the configurable QA window
     * (@ref QaConfigStruct::afterpulse_near_lo / @c afterpulse_near_hi) and is meant
     * exclusively for the sideband-subtraction afterpulse probability QA.
     */
    bool is_afterpulse_near() const { return has_mask_bit(HitmaskAfterpulseNear); }

    /**
     * @brief True if Δt to the previous same-channel Hit lies in the QA far window
     *        (DCR-only sideband).
     *
     * Paired with @ref is_afterpulse_near to subtract the random same-channel
     * coincidence baseline from the apparent afterpulse rate.
     */
    bool is_afterpulse_far() const { return has_mask_bit(HitmaskAfterpulseFar); }

    /** @brief Checks whether the Hit originates from a partially active lane. */
    bool is_part_lane() const { return has_mask_bit(_HITMASK_part_lane); }

    /** @brief Checks whether the Hit originates from a dead lane. */
    bool is_dead_lane() const { return has_mask_bit(HitmaskDeadLane); }

    /// @}

    // -------------------------------------------------------------------------
    // Setters — Raw fields
    // -------------------------------------------------------------------------

    /// @name Raw Field Setters
    /// @{

    /**
     * @brief Replaces the underlying data struct.
     * @param d New @ref AlcorFinedataStruct to assign.
     */
    void set_data_struct(const AlcorFinedataStruct &d) { internal_data = d; }

    /**
     * @brief Sets the calibration index.
     * @param calib New calibration index value.
     */
    void set_global_index(uint32_t calib) { internal_data.GlobalIndex = calib; }

    /**
     * @brief Sets the rollover counter.
     * @param r New rollover value.
     */
    void set_rollover(uint32_t r) { internal_data.rollover = r; }

    /**
     * @brief Sets the coarse timestamp.
     * @param c New coarse value.
     */
    void set_coarse(uint16_t c) { internal_data.coarse = c; }

    /**
     * @brief Sets the fine timestamp.
     * @param f New fine value.
     */
    void set_fine(uint8_t f) { internal_data.fine = f; }

    /**
     * @brief Sets the Hit bitmask.
     * @param mask New mask value.
     */
    void set_mask(uint32_t mask) { internal_data.HitMask = mask; }

    /** @brief Flags the Hit as a streaming ring trigger participant. */
    void set_streaming_ring_trigger_mask() { add_mask_bit(HitmaskStreamingRingTrigger); }

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Calibration parameters
    // -------------------------------------------------------------------------

    /// @name Calibration Parameter Getters
    /// @{

    /**
     * @brief Returns calibration parameter 0 for a given TDC channel.
     * @param GlobalIndex Global TDC index to query.
     */
    static float get_param0(int GlobalIndex)
    {
        std::shared_lock<std::shared_mutex> lock(calibration_mutex);
        auto it = calibration_parameters.find(GlobalIndex);
        return (it != calibration_parameters.end()) ? it->second[0] : 0.f;
    }

    /**
     * @brief Returns calibration parameter 1 for a given TDC channel.
     * @param GlobalIndex Global TDC index to query.
     */
    static float get_param1(int GlobalIndex)
    {
        std::shared_lock<std::shared_mutex> lock(calibration_mutex);
        auto it = calibration_parameters.find(GlobalIndex);
        return (it != calibration_parameters.end()) ? it->second[1] : 0.f;
    }

    /**
     * @brief Returns calibration parameter 2 for a given TDC channel.
     * @param GlobalIndex Global TDC index to query.
     */
    static float get_param2(int GlobalIndex)
    {
        std::shared_lock<std::shared_mutex> lock(calibration_mutex);
        auto it = calibration_parameters.find(GlobalIndex);
        return (it != calibration_parameters.end()) ? it->second[2] : 0.f;
    }

    /// @}

    // -------------------------------------------------------------------------
    // Setters — Calibration parameters
    // -------------------------------------------------------------------------

    /// @name Calibration Parameter Setters
    /// @{

    /**
     * @brief Sets calibration parameter 0 for a given TDC channel.
     * @param GlobalIndex Global TDC index to update.
     * @param value            New value for parameter 0.
     */
    static void set_param0(int GlobalIndex, float value)
    {
        std::unique_lock<std::shared_mutex> lock(calibration_mutex);
        calibration_parameters[GlobalIndex][0] = value;
    }

    /**
     * @brief Sets calibration parameter 1 for a given TDC channel.
     * @param GlobalIndex Global TDC index to update.
     * @param value            New value for parameter 1.
     */
    static void set_param1(int GlobalIndex, float value)
    {
        std::unique_lock<std::shared_mutex> lock(calibration_mutex);
        calibration_parameters[GlobalIndex][1] = value;
    }

    /**
     * @brief Sets calibration parameter 2 for a given TDC channel.
     * @param GlobalIndex Global TDC index to update.
     * @param value            New value for parameter 2.
     */
    static void set_param2(int GlobalIndex, float value)
    {
        std::unique_lock<std::shared_mutex> lock(calibration_mutex);
        calibration_parameters[GlobalIndex][2] = value;
    }

    /// @}

    // -------------------------------------------------------------------------
    // Comparison operators
    // -------------------------------------------------------------------------

    /// @name Comparison Operators
    /// @brief Compare hits by their calibrated timestamp.
    /// @{

    /** @brief Less-than comparison against an @ref AlcorFinedata Hit. */
    bool operator<(const AlcorFinedata &v) const { return get_time() < v.get_time(); }

    /** @brief Less-than-or-equal comparison against an @ref AlcorFinedata Hit. */
    bool operator<=(const AlcorFinedata &v) const { return get_time() <= v.get_time(); }

    /** @brief Greater-than comparison against an @ref AlcorFinedata Hit. */
    bool operator>(const AlcorFinedata &v) const { return get_time() > v.get_time(); }

    /** @brief Greater-than-or-equal comparison against an @ref AlcorFinedata Hit. */
    bool operator>=(const AlcorFinedata &v) const { return get_time() >= v.get_time(); }

    /// @}

    // -------------------------------------------------------------------------
    // Calibration I/O
    // -------------------------------------------------------------------------

    /// @name Calibration
    /// @{

    /**
     * @brief Derives calibration parameters from a 2D fine-time histogram.
     *
     * Fits each Y-slice of @p calibration_histogram to extract the 3 calibration
     * parameters and populates the static @c calibration_parameters map.
     *
     * @param calibration_histogram 2D histogram with TDC index on X and fine
     *                              counts on Y, typically accumulated during a
     *                              dedicated calibration run.
     * @param overwrite_calibration If @c true, existing entries are replaced.
     */
    static void generate_calibration(TH2F *calibration_histogram, bool overwrite_calibration);

    /**
     * @brief Updates calibration without overwriting existing entries.
     * Alias for @ref generate_calibration with @c overwrite_calibration = @c false.
     * @param h 2D fine-time histogram (same format as @ref generate_calibration).
     */
    static void update_calibration(TH2F *h) { generate_calibration(h, false); }

    /**
     * @brief Writes the current calibration parameters to a plain-text file.
     * @param filename Path to the output calibration file.
     */
    static void write_calib_to_file(const std::string &filename);

    /**
     * @brief Loads calibration parameters from a plain-text file.
     * @param filename     Path to the calibration file to read.
     * @param clear_first  If @c true, clears existing parameters before loading (default: @c true).
     * @param overwrites   If @c true, existing entries are overwritten by file values (default: @c true).
     */
    static void read_calib_from_file(const std::string &filename, bool clear_first = true, bool overwrites = true);

    // Setter — per channel
    /**
     * @brief Sets the phase-correction method for a specific TDC channel.
     * @param GlobalIndex  Channel to configure.
     * @param method        One of the @ref CalibrationMethod enumerators.
     */
    static void set_calibration_method(int GlobalIndex, CalibrationMethod method)
    {
        std::unique_lock<std::shared_mutex> lock(calibration_mutex);
        channel_calibration_method[GlobalIndex] = method;
    }

    // Setter — global default
    /**
     * @brief Sets the fallback phase-correction method for channels with no explicit assignment.
     * @param method One of the @ref CalibrationMethod enumerators.
     */
    static void set_default_calibration_method(CalibrationMethod method)
    {
        std::unique_lock<std::shared_mutex> lock(calibration_mutex);
        default_calibration_method = method;
    }

    // Getters
    /**
     * @brief Returns the phase-correction method for a specific TDC channel,
     *        falling back to the global default if none is set.
     * @param GlobalIndex Channel to query.
     */
    static CalibrationMethod get_calibration_method(int GlobalIndex)
    {
        std::shared_lock<std::shared_mutex> lock(calibration_mutex);
        auto it = channel_calibration_method.find(GlobalIndex);
        return (it != channel_calibration_method.end()) ? it->second : default_calibration_method;
    }

    /**
     * @brief Returns the global default phase-correction method.
     */
    static CalibrationMethod get_default_calibration_method()
    {
        std::shared_lock<std::shared_mutex> lock(calibration_mutex);
        return default_calibration_method;
    }

    /**
     * @brief Switch a channel to the linear fit calibration method (v2) and update its parameters.
     *
     * Sets the calibration method for @p GlobalIndex to @c AlcorV2FitCalib and
     * derives the two fit parameters from the supplied linear coefficients, offsetting
     * by the midpoint of the existing [param0, param1] interval when available.
     *
     * @param GlobalIndex      TDC channel to reconfigure.
     * @param calibration_type  Calibration type selector (currently unused; reserved for future variants).
     * @param angular_coeff     Slope of the linear fine-time vs. ADC-bin relationship.
     * @param offset            Intercept offset applied after the midpoint shift.
     * @param sigma             Width parameter stored as param2 (e.g. for resolution estimates).
     */
    static void switch_to_fit_v2(int GlobalIndex, CalibrationMethod calibration_type, float angular_coeff, float offset, float sigma);

    /// @}

    // -------------------------------------------------------------------------
    // Finding rings algorithms
    // -------------------------------------------------------------------------

    /// @name Ring-finding algorithms
    /// @{

    /**
     * @brief Convert a vector of @ref AlcorFinedata hits into @ref hough_hit,
     *        run the Hough-transform ring finder, write mask bits back onto the
     *        original hits, and return the ring results.
     *
     * All ALCOR-specific knowledge is concentrated here:
     *  - Afterpulse filtering (`is_afterpulse()`).
     *  - Device-index guard (device ≥ 200 excluded).
     *  - LUT key derivation (`get_global_index() / 4`).
     *  - Mask-bit assignment (`HitmaskHoughRingTagFirst/second`).
     *
     * @param ht                  Pre-built @ref HoughTransform instance.
     *                            @ref HoughTransform::build_lut must have been
     *                            called with geometry consistent with @p alcor_hits.
     * @param alcor_hits          Calibrated ALCOR hits for the current event.
     *                            Mask bits are written back in-place for tagged hits.
     * @param threshold_fraction  Minimum fraction of active hits required in the
     *                            peak accumulator cell (range 0–1).
     * @param min_hits            Minimum absolute vote count for a ring to be accepted.
     * @param max_rings           Maximum number of rings to extract (default 2).
     * @param collection_radius   Distance from the ring arc within which a Hit is
     *                            assigned to the ring [mm] (default 6).
     * @return                    Vector of @ref hough_ring_result (indices refer to
     *                            the @p alcor_hits vector), in descending peak-vote order.
     */
    static std::vector<mist::ring_finding::RingResult> alcor_find_rings_hough(
        mist::ring_finding::HoughTransform &ht,
        std::vector<AlcorFinedata> &alcor_hits,
        float threshold_fraction,
        int min_hits,
        int min_active,
        int max_rings = 2,
        float collection_radius = 6.f);

    /// @}

private:
    // -------------------------------------------------------------------------
    // Private data
    // -------------------------------------------------------------------------

    /** @brief Decoded timing and mask data for this Hit. */
    AlcorFinedataStruct internal_data;

    /**
     * @brief Shared calibration table Mapping global TDC index to 3 fit parameters.
     *
     * Declared @c inline @c static so a single table is shared across all
     * @ref AlcorFinedata instances without requiring a separate definition.
     *
     * @note Thread-safety: all accesses must hold @c calibration_mutex.
     *       Getters take a shared (read) lock; setters and bulk I/O methods
     *       take a unique (write) lock.  Load calibration before spawning
     *       worker threads to avoid contention on the hot read path.
     */
    inline static std::unordered_map<int, std::array<float, 3>> calibration_parameters = {};

    /** @brief Per-channel phase-correction method override.
     *  @note Protected by @c calibration_mutex. */
    inline static std::unordered_map<int, CalibrationMethod> channel_calibration_method = {};

    /** @brief Fallback method for channels absent from channel_calibration_method.
     *  @note Protected by @c calibration_mutex. */
    inline static CalibrationMethod default_calibration_method = CalibrationMethod::AlcorV2BaseCalib;

    /**
     * @brief Reader-writer lock guarding all three static calibration members.
     *
     * Use @c std::shared_lock for reads (@c get_param*, @c get_calibration_method)
     * and @c std::unique_lock for writes (@c set_param*, @c read_calib_from_file,
     * @c generate_calibration, @c write_calib_to_file).
     *
     * @note Static members can't be declared @c mutable (static state is
     *       inherently mutable from any context); @c const methods may still
     *       lock this freely because the mutex is global, not a member of
     *       the object being viewed.
     */
    inline static std::shared_mutex calibration_mutex{};

    /**
     * @brief Frozen-table fast-path flag (CODE_REVIEW §3.1).
     *
     * Once calibration loading is complete the table is immutable for the rest
     * of the run.  Setting this flag lets per-Hit readers like @ref get_phase
     * skip the @c shared_lock entirely — atomic reads only.
     *
     * The framer / writer setup calls @ref freeze_calibration after loading
     * (typically right after @ref read_calib_from_file or @ref generate_calibration
     * finishes, before workers are spawned).  Subsequent setter calls are NOT
     * blocked at compile time; they take the unique lock as before — but
     * concurrent setters and lock-free readers can race, so don't call setters
     * after freezing.
     */
    inline static std::atomic<bool> calibration_frozen_{false};

public:
    /**
     * @brief Mark the calibration table immutable so per-Hit readers can
     *        skip the shared-mutex acquisition.
     *
     * Idempotent; once set, stays set for the process lifetime.  Use only
     * after every calibration setter has finished (typically right before
     * spawning worker threads).
     */
    static void freeze_calibration() noexcept
    {
        calibration_frozen_.store(true, std::memory_order_release);
    }

    /**
     * @brief Returns whether the calibration table has been frozen.
     */
    static bool is_calibration_frozen() noexcept
    {
        return calibration_frozen_.load(std::memory_order_acquire);
    }

private:
};