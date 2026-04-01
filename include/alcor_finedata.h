#pragma once

#include <mist/ring_finding/hough_transform.h>
#include <sstream>
#include <cmath>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <array>
#include "TH2F.h"
#include "TF1.h"
#include "TCanvas.h"
#include "alcor_data.h"

/**
     * @brief Raw decoded hit data from an ALCOR TDC channel.
     *
     * Holds the timing components (rollover, coarse, fine) and the calibration
     * index used to look up the corresponding fine-time calibration parameters.
     *
     * @todo Implement bit-wise manipulation for rollover, fine, and coarse encoding.
     */
struct alcor_finedata_struct
{
    /// @name Timing Components
    /// @{

    /** @brief Rollover counter (most-significant timing word). */
    uint32_t rollover;

    /** @brief Coarse timestamp (clock-cycle counter). */
    uint16_t coarse;

    /** @brief Fine timestamp (TDC interpolation bin within a clock cycle). */
    uint8_t fine;

    /** @brief X-axis position from mapping. */
    float hit_x;

    /** @brief Y-axis position from mapping. */
    float hit_y;

    /// @}

    /// @name Indexing & Masking
    /// @{

    /** @brief Global calibration index identifying the TDC channel. */
    uint32_t global_index;

    /** @brief Bitmask encoding hit channel/pixel information. */
    uint32_t hit_mask;

    /// @}

    /** @brief Default constructor. */
    alcor_finedata_struct() = default;

    /**
     * @brief Constructor from individual values.
     */
    alcor_finedata_struct(uint32_t rollover_,
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
          global_index(global_index_),
          hit_mask(hit_mask_)
    {
    }

    /**
     * @brief Construct from a raw @ref alcor_data_struct.
     * @param d Raw data word to decode into timing components.
     */
    alcor_finedata_struct(const alcor_data_struct &d);
};

// -------------------------------------------------------------------------
// Calibration method enum
// -------------------------------------------------------------------------

/**
 * @brief Selects the algorithm used to compute the fine-time phase correction.
 *
 * _ALCOR_v2_BASE_CALIB_  — baseline linear interpolation between the two
 *                          sigmoid inflection points (parameters 0 and 1).
 * _ALCOR_v2_FIT_CALIB_   — reserved for a future fit-based correction.
 */
enum class calibration_method_t : uint8_t
{
    _ALCOR_v2_BASE_CALIB_ = 0, ///< Default: linear interpolation (sigmoid edges).
    _ALCOR_v2_FIT_CALIB_ = 1,  ///< Fit-based phase correction (not yet implemented).
};

// =============================================================================

/**
     * @class alcor_finedata
     * @brief Represents a single calibrated ALCOR TDC hit with fine-time correction.
     *
     * Wraps an @ref alcor_finedata_struct and provides:
     * - Accessors for raw timing fields and derived channel-address fields.
     * - A static calibration table (shared across all instances) mapping global
     *   TDC indices to a 3-parameter calibration array.
     * - Methods to generate, persist, and load the calibration from ROOT histograms
     *   or plain files.
     *
     * The fine-time phase is computed from the calibration parameters stored in
     * the static @c calibration_parameters map.
     */
class alcor_finedata
{
public:
    // -------------------------------------------------------------------------
    // Constructors
    // -------------------------------------------------------------------------

    /** @brief Default constructor. Initialises all fields to zero. */
    alcor_finedata() {}

    /**
     * @brief Construct from a decoded @ref alcor_finedata_struct.
     * @param s Pre-filled struct with rollover, coarse, fine, and mask.
     */
    alcor_finedata(const alcor_finedata_struct &s)
        : internal_data(s) {}

    /**
     * @brief Construct directly from a raw @ref alcor_data_struct.
     * @param d Raw data word; decoded internally via @ref alcor_finedata_struct.
     */
    alcor_finedata(const alcor_data_struct &d)
        : internal_data(d) {}

    /**
     * @brief Copy constructor.
     * @param o Source object to copy from.
     */
    alcor_finedata(const alcor_finedata &o)
        : internal_data(o.get_data()) {}

    /**
     * @brief Construct from individual timing and channel fields.
     * @param rollover_      Rollover counter (most-significant timing word).
     * @param coarse_        Coarse timestamp (clock-cycle counter).
     * @param fine_          Fine timestamp (TDC interpolation bin).
     * @param hit_x_         X-axis position from mapping.
     * @param hit_y_         Y-axis position from mapping.
     * @param global_index_  Global calibration index identifying the TDC channel.
     * @param hit_mask_      Bitmask encoding hit channel/pixel information.
     */
    alcor_finedata(
        uint32_t rollover_,
        uint16_t coarse_,
        uint8_t fine_,
        float hit_x_,
        float hit_y_,
        uint32_t global_index_,
        uint32_t hit_mask_)
        : internal_data(alcor_finedata_struct(
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

    /** @brief Returns a copy of the underlying @ref alcor_finedata_struct. */
    alcor_finedata_struct get_data() const { return internal_data; }

    /** @brief Returns a reference of the underlying @ref alcor_finedata_struct. */
    alcor_finedata_struct &get_data_link()  { return internal_data; }

    /** @brief Returns the calibration index identifying the TDC channel. */
    uint32_t get_global_index() const { return internal_data.global_index; }

    /** @brief Returns the rollover counter. */
    uint32_t get_rollover() const { return internal_data.rollover; }

    /** @brief Returns the coarse timestamp (clock-cycle count). */
    uint16_t get_coarse() const { return internal_data.coarse; }

    /** @brief Returns the fine timestamp (TDC bin within a clock cycle). */
    uint8_t get_fine() const { return internal_data.fine; }

    /** @brief Returns the x-axis position from mapping. */
    float get_hit_x() const { return internal_data.hit_x; }

    /** @brief Returns the y-axis position from mapping. */
    float get_hit_y() const { return internal_data.hit_y; }

    /** @brief Returns the hit bitmask. */
    uint32_t get_mask() const { return internal_data.hit_mask; }

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
     * @brief Returns the calibrated hit time in clock cycles.
     * Combines rollover, coarse, and the fine-time phase correction.
     * All unsigned fields are promoted to float before arithmetic to avoid
     * unsigned underflow when subtracting the (potentially non-zero) phase.
     */
    float get_time() const { return static_cast<float>(_ALCOR_ROLLOVER_TO_CC_) * static_cast<float>(get_rollover()) + static_cast<float>(get_coarse()) - get_phase(); }

    /**
     * @brief Returns the calibrated hit time in nanoseconds.
     */
    float get_time_ns() const { return _ALCOR_CC_TO_NS_ * get_time(); }

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Derived channel address
    // -------------------------------------------------------------------------

    /// @name Derived Address Getters
    /// @{

    /** @brief Returns the TDC index decoded from the calibration index. */
    int get_tdc() const { return get_global_index() % 4; }

    /** @brief Returns the readout device ID decoded from the calibration index. */
    int get_device() const { return 192 + (get_global_channel_index() / 256); }

    /** @brief Returns the FIFO number decoded from the calibration index. */
    int get_fifo() const { return (get_global_channel_index() % 256) / 8; }

    /** @brief Returns the lane number decoded from the calibration index. */
    int get_lane() const { return get_fifo(); }

    /** @brief Returns the chip ID decoded from the calibration index. */
    int get_chip() const { return (get_global_channel_index() % 256) / 32; }

    /** @brief Returns the even/odd channel index decoded from the calibration index. */
    int get_eo_channel() const { return (get_global_channel_index() % 256) % 32 + 32 * (get_chip() % 2); }

    /** @brief Returns the column address decoded from the calibration index. */
    int get_column() const { return ((get_global_channel_index() % 256) % 32) / 4; }

    /** @brief Returns the pixel address decoded from the calibration index. */
    int get_pixel() const { return ((get_global_channel_index() % 256) % 32) % 4; }

    /** @brief Returns the per-device TDC index. */
    int get_device_index() const { return get_eo_channel() + 64 * (get_chip() / 2); }

    /** @brief Returns the global channel index stripped of TDC info. */
    int get_global_channel_index() const { return get_global_index() / 4; }

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Spatial (randomised)
    // -------------------------------------------------------------------------

    /// @name Spatial Getters
    /// @{

    /** @brief Returns the pixel-randomised x-coordinate, uniform within ±1.5 mm of the hit position. */
    float get_hit_x_rnd() const { return internal_data.hit_x + (_rnd_(_global_gen_) * 3.0f - 1.5f); }

    /** @brief Returns the pixel-randomised y-coordinate, uniform within ±1.5 mm of the hit position. */
    float get_hit_y_rnd() const { return internal_data.hit_y + (_rnd_(_global_gen_) * 3.0f - 1.5f); }

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
     * @brief Sets a single bit in the hit mask.
     * @param bit Bit position to set (from @ref hit_mask enum).
     */
    void add_mask_bit(hit_mask bit) { internal_data.hit_mask |= (1u << bit); }

    /**
     * @brief Clears a single bit in the hit mask.
     * @param bit Bit position to clear (from @ref hit_mask enum).
     */
    void clear_mask_bit(hit_mask bit) { internal_data.hit_mask &= ~(1u << bit); }

    /**
     * @brief Checks whether a single bit is set in the hit mask.
     * @param bit Bit position to check (from @ref hit_mask enum).
     * @return true if the bit is set.
     */
    bool has_mask_bit(hit_mask bit) const { return (internal_data.hit_mask >> bit) & 1u; }

    /** @brief Checks whether the hit is tagged as part of a ring (first pass). */
    bool is_ring_tag_first() const { return has_mask_bit(_HITMASK_ring_tag_first); }

    /** @brief Checks whether the hit is tagged as part of a ring (second pass). */
    bool is_ring_tag_second() const { return has_mask_bit(_HITMASK_ring_tag_second); }

    /** @brief Checks whether the hit is flagged as cross-talk. */
    bool is_cross_talk() const { return has_mask_bit(_HITMASK_cross_talk); }

    /** @brief Checks whether the hit is flagged as an afterpulse. */
    bool is_afterpulse() const { return has_mask_bit(_HITMASK_afterpulse); }

    /** @brief Checks whether the hit originates from a partially active lane. */
    bool is_part_lane() const { return has_mask_bit(_HITMASK_part_lane); }

    /** @brief Checks whether the hit originates from a dead lane. */
    bool is_dead_lane() const { return has_mask_bit(_HITMASK_dead_lane); }

    /// @}

    // -------------------------------------------------------------------------
    // Setters — Raw fields
    // -------------------------------------------------------------------------

    /// @name Raw Field Setters
    /// @{

    /**
     * @brief Replaces the underlying data struct.
     * @param d New @ref alcor_finedata_struct to assign.
     */
    void set_data_struct(const alcor_finedata_struct &d) { internal_data = d; }

    /**
     * @brief Sets the calibration index.
     * @param calib New calibration index value.
     */
    void set_global_index(uint32_t calib) { internal_data.global_index = calib; }

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
     * @brief Sets the hit bitmask.
     * @param mask New mask value.
     */
    void set_mask(uint32_t mask) { internal_data.hit_mask = mask; }

    /** @brief Flags the hit as a streaming ring trigger participant. */
    void set_streaming_ring_trigger_mask() { add_mask_bit(_HITMASK_streaming_ring_trigger_); }

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Calibration parameters
    // -------------------------------------------------------------------------

    /// @name Calibration Parameter Getters
    /// @{

    /**
     * @brief Returns calibration parameter 0 for a given TDC channel.
     * @param global_index Global TDC index to query.
     */
    static float get_param0(int global_index)
    {
        auto it = calibration_parameters.find(global_index);
        return (it != calibration_parameters.end()) ? it->second[0] : 0.f;
    }

    /**
     * @brief Returns calibration parameter 1 for a given TDC channel.
     * @param global_index Global TDC index to query.
     */
    static float get_param1(int global_index)
    {
        auto it = calibration_parameters.find(global_index);
        return (it != calibration_parameters.end()) ? it->second[1] : 0.f;
    }

    /**
     * @brief Returns calibration parameter 2 for a given TDC channel.
     * @param global_index Global TDC index to query.
     */
    static float get_param2(int global_index)
    {
        auto it = calibration_parameters.find(global_index);
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
     * @param global_index Global TDC index to update.
     * @param value            New value for parameter 0.
     */
    static void set_param0(int global_index, float value) { calibration_parameters[global_index][0] = value; }

    /**
     * @brief Sets calibration parameter 1 for a given TDC channel.
     * @param global_index Global TDC index to update.
     * @param value            New value for parameter 1.
     */
    static void set_param1(int global_index, float value) { calibration_parameters[global_index][1] = value; }

    /**
     * @brief Sets calibration parameter 2 for a given TDC channel.
     * @param global_index Global TDC index to update.
     * @param value            New value for parameter 2.
     */
    static void set_param2(int global_index, float value) { calibration_parameters[global_index][2] = value; }

    /// @}

    // -------------------------------------------------------------------------
    // Comparison operators
    // -------------------------------------------------------------------------

    /// @name Comparison Operators
    /// @brief Compare hits by their calibrated timestamp.
    /// @{

    /** @brief Less-than comparison against an @ref alcor_finedata hit. */
    bool operator<(const alcor_finedata &v) const { return get_time() < v.get_time(); }

    /** @brief Less-than-or-equal comparison against an @ref alcor_finedata hit. */
    bool operator<=(const alcor_finedata &v) const { return get_time() <= v.get_time(); }

    /** @brief Greater-than comparison against an @ref alcor_finedata hit. */
    bool operator>(const alcor_finedata &v) const { return get_time() > v.get_time(); }

    /** @brief Greater-than-or-equal comparison against an @ref alcor_finedata hit. */
    bool operator>=(const alcor_finedata &v) const { return get_time() >= v.get_time(); }

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
     * @param global_index  Channel to configure.
     * @param method        One of the @ref calibration_method_t enumerators.
     */
    static void set_calibration_method(int global_index, calibration_method_t method) { channel_calibration_method[global_index] = method; }

    // Setter — global default
    /**
     * @brief Sets the fallback phase-correction method for channels with no explicit assignment.
     * @param method One of the @ref calibration_method_t enumerators.
     */
    static void set_default_calibration_method(calibration_method_t method) { default_calibration_method = method; }

    // Getters
    /**
     * @brief Returns the phase-correction method for a specific TDC channel,
     *        falling back to the global default if none is set.
     * @param global_index Channel to query.
     */
    static calibration_method_t get_calibration_method(int global_index)
    {
        auto it = channel_calibration_method.find(global_index);
        return (it != channel_calibration_method.end()) ? it->second : default_calibration_method;
    }

    /**
     * @brief Returns the global default phase-correction method.
     */
    static calibration_method_t get_default_calibration_method() { return default_calibration_method; }

    static void switch_to_fit_v2(int global_index, calibration_method_t calibration_type, float angular_coeff, float offset, float sigma);

    /// @}

    // -------------------------------------------------------------------------
    // Finding rings algorithms
    // -------------------------------------------------------------------------

    /// @name Ring-finding algorithms
    /// @{

    /**
     * @brief Convert a vector of @ref alcor_finedata hits into @ref hough_hit,
     *        run the Hough-transform ring finder, write mask bits back onto the
     *        original hits, and return the ring results.
     *
     * All ALCOR-specific knowledge is concentrated here:
     *  - Afterpulse filtering (`is_afterpulse()`).
     *  - Device-index guard (device ≥ 200 excluded).
     *  - LUT key derivation (`get_global_index() / 4`).
     *  - Mask-bit assignment (`_HITMASK_hough_ring_tag_first/second`).
     *
     * @param ht                  Pre-built @ref hough_transform instance.
     *                            @ref hough_transform::build_lut must have been
     *                            called with geometry consistent with @p alcor_hits.
     * @param alcor_hits          Calibrated ALCOR hits for the current event.
     *                            Mask bits are written back in-place for tagged hits.
     * @param threshold_fraction  Minimum fraction of active hits required in the
     *                            peak accumulator cell (range 0–1).
     * @param min_hits            Minimum absolute vote count for a ring to be accepted.
     * @param max_rings           Maximum number of rings to extract (default 2).
     * @param collection_radius   Distance from the ring arc within which a hit is
     *                            assigned to the ring [mm] (default 6).
     * @return                    Vector of @ref hough_ring_result (indices refer to
     *                            the @p alcor_hits vector), in descending peak-vote order.
     */
    static std::vector<mist::ring_finding::ring_result> alcor_find_rings_hough(
        mist::ring_finding::hough_transform &ht,
        std::vector<alcor_finedata> &alcor_hits,
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

    /** @brief Decoded timing and mask data for this hit. */
    alcor_finedata_struct internal_data;

    /**
     * @brief Shared calibration table mapping global TDC index to 3 fit parameters.
     *
     * Declared @c inline @c static so a single table is shared across all
     * @ref alcor_finedata instances without requiring a separate definition.
     */
    inline static std::unordered_map<int, std::array<float, 3>> calibration_parameters = {};

    /** @brief Per-channel phase-correction method override. */
    inline static std::unordered_map<int, calibration_method_t> channel_calibration_method = {};

    /** @brief Fallback method for channels absent from channel_calibration_method. */
    inline static calibration_method_t default_calibration_method = calibration_method_t::_ALCOR_v2_BASE_CALIB_;
};