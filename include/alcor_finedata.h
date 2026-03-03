#pragma once

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
    int rollover;

    /** @brief Coarse timestamp (clock-cycle counter). */
    int coarse;

    /** @brief Fine timestamp (TDC interpolation bin within a clock cycle). */
    int fine;

    /// @}

    /// @name Indexing & Masking
    /// @{

    /** @brief Global calibration index identifying the TDC channel. */
    uint32_t calib_index;

    /** @brief Bitmask encoding hit channel/pixel information. */
    uint32_t hit_mask;

    /// @}

    /** @brief Default constructor. */
    alcor_finedata_struct() = default;

    /**
     * @brief Construct from a raw @ref alcor_data_struct.
     * @param d Raw data word to decode into timing components.
     */
    alcor_finedata_struct(const alcor_data_struct &d);
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
    alcor_finedata();

    /**
     * @brief Construct from a decoded @ref alcor_finedata_struct.
     * @param s Pre-filled struct with rollover, coarse, fine, and mask.
     */
    alcor_finedata(const alcor_finedata_struct &s);

    /**
     * @brief Construct directly from a raw @ref alcor_data_struct.
     * @param d Raw data word; decoded internally via @ref alcor_finedata_struct.
     */
    alcor_finedata(const alcor_data_struct &d);

    /**
     * @brief Copy constructor.
     * @param o Source object to copy from.
     */
    alcor_finedata(const alcor_finedata &o);

    // -------------------------------------------------------------------------
    // Getters — Raw fields
    // -------------------------------------------------------------------------

    /// @name Raw Field Getters
    /// @{

    /** @brief Returns a copy of the underlying @ref alcor_finedata_struct. */
    alcor_finedata_struct get_data_struct() const;

    /** @brief Returns the calibration index identifying the TDC channel. */
    uint32_t get_calib_index() const;

    /** @brief Returns the rollover counter. */
    int get_rollover() const;

    /** @brief Returns the coarse timestamp (clock-cycle count). */
    int get_coarse() const;

    /** @brief Returns the fine timestamp (TDC bin within a clock cycle). */
    int get_fine() const;

    /** @brief Returns the hit bitmask. */
    uint32_t get_mask() const;

    /**
     * @brief Returns the calibrated fine-time phase in nanoseconds.
     * Computed from the 3-parameter calibration stored in @c calibration_parameters.
     */
    float get_phase() const;

    /**
     * @brief Returns the calibrated fine-time in nanoseconds.
     * Computed from the phase, using the 3-parameter calibration stored in @c calibration_parameters.
     */
    float get_time() const;

    /// @}

    // -------------------------------------------------------------------------
    // Getters — Derived channel address
    // -------------------------------------------------------------------------

    /// @name Derived Address Getters
    /// @{

    /** @brief Returns the TDC index decoded from the calibration index. */
    int get_tdc() const;

    /** @brief Returns the readout device ID decoded from the calibration index. */
    int get_device() const;

    /** @brief Returns the FIFO number decoded from the calibration index. */
    int get_fifo() const;

    /** @brief Returns the chip ID decoded from the calibration index. */
    int get_chip() const;

    /** @brief Returns the even/odd channel flag decoded from the calibration index. */
    int get_eo_channel() const;

    /** @brief Returns the column address decoded from the calibration index. */
    int get_column() const;

    /** @brief Returns the pixel address decoded from the calibration index. */
    int get_pixel() const;

    /** @brief Returns the per-device TDC index. */
    int get_device_index() const;

    /** @brief Returns the global TDC index across all devices and FIFOs. */
    int get_global_index() const;

    /// @}

    // -------------------------------------------------------------------------
    // Setters — Pure
    // -------------------------------------------------------------------------

    /// @name Raw Field Setters
    /// @{

    /**
     * @brief Replaces the underlying data struct.
     * @param d New @ref alcor_finedata_struct to assign.
     */
    void set_data_struct(const alcor_finedata_struct &d);

    /**
     * @brief Sets the calibration index.
     * @param calib New calibration index value.
     */
    void set_calib_index(uint32_t calib);

    /**
     * @brief Sets the rollover counter.
     * @param r New rollover value.
     */
    void set_rollover(int r);

    /**
     * @brief Sets the coarse timestamp.
     * @param c New coarse value.
     */
    void set_coarse(int c);

    /**
     * @brief Sets the fine timestamp.
     * @param f New fine value.
     */
    void set_fine(int f);

    /**
     * @brief Sets the hit bitmask.
     * @param mask New mask value.
     */
    void set_mask(uint32_t mask);

    /// @}

    // -------------------------------------------------------------------------
    // Setters — Calibration parameters
    // -------------------------------------------------------------------------

    /// @name Calibration Parameter Setters
    /// @{

    /**
     * @brief Sets calibration parameter 0 for a given TDC channel.
     * @param global_tdc_index Global TDC index to update.
     * @param value            New value for parameter 0.
     */
    void set_param0(int global_tdc_index, float value);

    /**
     * @brief Sets calibration parameter 1 for a given TDC channel.
     * @param global_tdc_index Global TDC index to update.
     * @param value            New value for parameter 1.
     */
    void set_param1(int global_tdc_index, float value);

    /**
     * @brief Sets calibration parameter 2 for a given TDC channel.
     * @param global_tdc_index Global TDC index to update.
     * @param value            New value for parameter 2.
     */
    void set_param2(int global_tdc_index, float value);

    /// @}

    // -------------------------------------------------------------------------
    // Comparison operators
    // -------------------------------------------------------------------------

    /// @name Comparison Operators
    /// @brief Compare hits by their absolute timestamp (rollover + coarse).
    /// @{

    /** @brief Less-than comparison against an @ref alcor_finedata hit. */
    bool operator<(const alcor_finedata &comparing_hit) const;

    /** @brief Less-than-or-equal comparison against an @ref alcor_finedata hit. */
    bool operator<=(const alcor_finedata &comparing_hit) const;

    /** @brief Greater-than comparison against an @ref alcor_finedata hit. */
    bool operator>(const alcor_finedata &comparing_hit) const;

    /** @brief Greater-than-or-equal comparison against an @ref alcor_finedata hit. */
    bool operator>=(const alcor_finedata &comparing_hit) const;

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
     */
    void generate_calibration(TH2F *calibration_histogram);

    /**
     * @brief Writes the current calibration parameters to a plain-text file.
     * @param filename Path to the output calibration file.
     */
    void write_calib_to_file(const std::string &filename);

    /**
     * @brief Loads calibration parameters from a plain-text file.
     * @param filename     Path to the calibration file to read.
     * @param clear_first  If @c true, clears existing parameters before loading (default: @c true).
     * @param overwrites   If @c true, existing entries are overwritten by file values (default: @c true).
     */
    void read_calib_from_file(const std::string &filename, bool clear_first = true, bool overwrites = true);

    /// @}

private:
    /// @name Private Data
    /// @{

    /** @brief Decoded timing and mask data for this hit. */
    alcor_finedata_struct data;

    /**
     * @brief Shared calibration table mapping global TDC index to 3 fit parameters.
     *
     * Declared @c inline @c static so a single table is shared across all
     * @ref alcor_finedata instances without requiring a separate definition.
     */
    inline static std::unordered_map<int, std::array<float, 3>> calibration_parameters = {};

    /// @}

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    /** @brief Populates the standard calibration function used during fitting. */
    void set_standard_function();
};