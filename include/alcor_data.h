#pragma once

/**
 * @file AlcorData.h
 * @brief Data structures and utilities for ALCOR Hit-level data handling.
 *
 * This header defines:
 * * A base, retro compatible data struct (@ref AlcorDataStruct) that reads data from ALCOR decoder
 * * Enumerations describing Hit types and bit-level Hit masks.
 * * A lightweight wrapper class (@ref AlcorData) that provides:
 *  - Safe accessors and mutators
 *  - Derived quantities (indices, global time, etc.)
 *  - Utility predicates (Hit type checks)
 *  - ROOT I/O helpers
 *
 * The design intentionally separates *storage* (struct) from *logic*
 * (class) to keep ROOT I/O simple while allowing richer semantics
 * at the analysis level.
 *
 * @todo Implement bit-wise manipulation for global index encoding.
 * global index is a 32-bit integer, which count be used to contain:
 * device   (10-bit) [0-1023]
 * fifo     (7-bit) [0-127]
 * channel  (6-bit) [0-63]
 * tdc      (2-bit) [0-3]
 * unused   (7-bit)
 */

#include "TTree.h"
#include "utility.h"

/**
 * @def *ALCOR_CC_TO_NS*
 * @brief Conversion factor from coarse clock units to nanoseconds.
 *
 * One coarse clock count corresponds to 3.125 ns.
 */
#define BTANA_ALCOR_CC_TO_NS 3.125
/**
 * @def *BTANA_ALCOR_ROLLOVER_TO_CC*
 * @brief Conversion factor from rollover to coarse clock units.
 *
 * One rollover count corresponds to 32768 coarse clock counts.
 */
#define BTANA_ALCOR_ROLLOVER_TO_CC 32768

/**
 * @struct AlcorDataStruct
 * @brief Plain data container for a single ALCOR Hit.
 *
 * This struct is intentionally kept trivial:
 * * No logic
 * * No ownership semantics
 * * ROOT-friendly layout
 *
 * It represents exactly what is stored/read from a ROOT TTree
 * or decoded from the DAQ stream.
 */
struct AlcorDataStruct
{
    int device;        ///< Readout device ID (192–207 for known PDUs)
    int fifo;          ///< FIFO number within the device
    int type;          ///< Hit type code — see @ref AlcorHitStruct
    int counter;       ///< Event counter from the DAQ stream
    int column;        ///< Column address within the ALCOR chip (0–7)
    int pixel;         ///< Pixel address within the column (0–3)
    int tdc;           ///< TDC sub-channel index (0–3)
    int rollover;      ///< Coarse-clock rollover count (each rollover = 32 768 cc)
    int coarse;        ///< Coarse timestamp within the current rollover (cc)
    int fine;          ///< Fine-time bin from the TDC (converted to ns via calibration)
    uint32_t HitMask; ///< Bit-field of processing flags — see @ref HitMask

    AlcorDataStruct() = default;
};

/**
 * @enum AlcorHitStruct
 * @brief Encodes the logical type of an ALCOR Hit.
 *
 * Let the user know if the Hit in the stream is a simple Hit, start or end of spill signal or a trigger Hit
 */
enum AlcorHitStruct
{
    alcor_hit = 1,
    trigger_tag = 9,
    start_spill = 7,
    end_spill = 15
};

/**
 * @enum HitMask
 * @brief Bit positions used inside @c AlcorDataStruct::HitMask.
 *
 * Each enumerator corresponds to a single bit index.
 */
enum HitMask
{
    HitmaskStreamingRingTrigger = 1,
    HitmaskRingTagFirst = 2,
    HitmaskRingTagSecond = 3,
    HitmaskHoughRingTagFirst = 11,
    HitmaskHoughRingTagSecond = 12,
    HitmaskAfterpulseNear = 26, ///< Same-channel Hit with Δt inside the QA near window (afterpulse signal region).
    HitmaskAfterpulseFar  = 27, ///< Same-channel Hit with Δt inside the QA far  window (DCR sideband region).
    HitmaskCrossTalk = 28,
    HitmaskAfterpulse = 29,      ///< Same-channel Hit with Δt below the framer's `afterpulse_deadtime` (used to gate downstream CT/recodata logic).
    _HITMASK_part_lane = 30,
    HitmaskDeadLane = 31
};

/**
 * @class AlcorData
 * @brief First-level I/O helper class from the ALCOR decoder
 *
 * This class wraps @ref AlcorDataStruct and provides:
 * * Strongly-typed getters and setters
 * * Derived indices and global time computation
 * * Comparison operators for sorting
 * * Convenience checks on Hit type and flags
 * * ROOT TTree input/output helpers
 *
 * The internal storage can either be copied or linked, depending
 * on how the setters are used.
 */
class AlcorData
{
private:
    AlcorDataStruct data;
    static constexpr int rollover_to_clock = BTANA_ALCOR_ROLLOVER_TO_CC;
    static constexpr double coarse_to_ns = BTANA_ALCOR_CC_TO_NS;
    static constexpr int rollover_to_ns = rollover_to_clock * coarse_to_ns;

public:
    /** @name Constructors */
    ///@{
    AlcorData() = default; ///< Default constructor — all fields left uninitialised.

    /// @brief Construct from a pre-filled storage struct.
    explicit AlcorData(const AlcorDataStruct &data_struct) : data(data_struct) {}

    /// @brief Construct field-by-field.
    explicit AlcorData(int device, int fifo, int type, int counter,
                        int column, int pixel, int tdc, int rollover,
                        int coarse, int fine, uint32_t mask)
    {
        data.device = device;
        data.fifo = fifo;
        data.type = type;
        data.counter = counter;
        data.column = column;
        data.pixel = pixel;
        data.tdc = tdc;
        data.rollover = rollover;
        data.coarse = coarse;
        data.fine = fine;
        data.HitMask = mask;
    }
    ///@}

    /** @name Raw field getters */
    ///@{
    AlcorDataStruct get_data() const { return data; } ///< Return a copy of the underlying storage struct.
    int get_device() const { return data.device; }             ///< Device ID (192+)
    int get_fifo() const { return data.fifo; }                 ///< FIFO number
    int get_type() const { return data.type; }                 ///< Hit type — see @ref AlcorHitStruct
    int get_counter() const { return data.counter; }           ///< Event counter
    int get_column() const { return data.column; }             ///< Column address (0–7)
    int get_pixel() const { return data.pixel; }               ///< Pixel address (0–3)
    int get_tdc() const { return data.tdc; }                   ///< TDC sub-channel (0–3)
    int get_rollover() const { return data.rollover; }         ///< Rollover count
    int get_coarse() const { return data.coarse; }             ///< Coarse timestamp (cc within rollover)
    int get_fine() const { return data.fine; }                 ///< Raw fine-time bin
    uint32_t get_mask() const { return data.HitMask; }        ///< Processing flag bit-mask — see @ref HitMask
    ///@}

    /** @name Derived getters
     *  Computed from the raw fields — not stored in the struct. */
    ///@{
    int get_chip() const { return data.fifo / 4; }                                              ///< Chip number on the device (FIFO / 4)
    int get_eo_channel() const { return data.pixel + 4 * data.column + 32 * (get_chip() % 2); } ///< Even/odd channel index within the chip
    int get_calib_index() const { return data.tdc + 4 * get_eo_channel() + 128 * get_chip(); }  ///< Calibration look-up index (TDC + channel + chip encoding)
    int get_device_index() const { return get_eo_channel() + 64 * (get_chip() / 2); }           ///< Flat per-device pixel index used for Mapping

    /// @brief Packed **TDC-level** global index — new-layout `GlobalIndex::raw()`.
    /// Phase 5: switched from the legacy arithmetic packing to
    /// `GlobalIndex::from_components(device, fifo, chip_logical, channel_logical, tdc).raw()`.
    /// The split-in-two trick (`chip_logical = chip_raw / 2`,
    /// `channel_logical = channel_raw + 32 * (chip_raw % 2)`) is applied here
    /// at the conversion boundary.
    uint32_t get_global_tdc_index() const {
        return ::GlobalIndex::from_components(
            get_device(),
            get_fifo(),
            get_chip() / 2,
            get_eo_channel(),
            get_tdc()).raw();
    }

    /// @brief Channel-level global index — TDC bits zeroed.
    /// Equivalent to @ref get_global_tdc_index for the same channel-position
    /// but with TDC = 0.  Use this as a key for per-channel maps where the
    /// TDC sub-index is not meaningful.
    uint32_t get_global_index() const {
        return ::GlobalIndex(get_global_tdc_index()).global_channel_raw();
    }
    /**
     * @brief Global coarse time including rollovers.
     * @return Time in coarse clock units
     */
    uint64_t get_coarse_global_time() const { return get_coarse() + get_rollover() * rollover_to_clock; }
    ///@}

    /** @name Setters */
    ///@{
    void set_data_struct_copy(AlcorDataStruct input_data) { data = input_data; }    ///< Copy @p input_data into internal storage
    void set_data_struct_linked(AlcorDataStruct &input_data) { data = input_data; } ///< Link internal storage to an external struct (no copy)
    void set_device(int val) { data.device = val; }                                   ///< Set device ID
    void set_fifo(int val) { data.fifo = val; }                                       ///< Set FIFO number
    void set_type(int val) { data.type = val; }                                       ///< Set Hit type
    void set_counter(int val) { data.counter = val; }                                 ///< Set event counter
    void set_column(int val) { data.column = val; }                                   ///< Set column address
    void set_pixel(int val) { data.pixel = val; }                                     ///< Set pixel address
    void set_tdc(int val) { data.tdc = val; }                                         ///< Set TDC sub-channel
    void set_rollover(int val) { data.rollover = val; }                               ///< Set rollover count
    void set_coarse(int val) { data.coarse = val; }                                   ///< Set coarse timestamp
    void set_fine(int val) { data.fine = val; }                                       ///< Set fine-time bin (signed)
    void set_fine(uint32_t val) { data.fine = static_cast<int>(val); }                ///< Set fine-time bin (unsigned overload)
    void set_mask(uint32_t val) { data.HitMask = val; }                              ///< Replace the Hit mask
    ///@}

    /** @name Comparison operators
     *  Order hits by global coarse time (ascending). */
    ///@{
    bool operator<(const AlcorData &c) const { return coarse_time_ns() < c.coarse_time_ns(); }
    bool operator<=(const AlcorData &c) const { return coarse_time_ns() <= c.coarse_time_ns(); }
    bool operator>(const AlcorData &c) const { return coarse_time_ns() > c.coarse_time_ns(); }
    bool operator>=(const AlcorData &c) const { return coarse_time_ns() >= c.coarse_time_ns(); }
    ///@}

    /** @name Hit-type predicates */
    ///@{
    bool is_alcor_hit() const { return get_type() == alcor_hit; }     ///< True if this is a normal TDC Hit
    bool is_trigger_tag() const { return get_type() == trigger_tag; } ///< True if this is a trigger-tag word
    bool is_start_spill() const { return get_type() == start_spill; } ///< True if this marks the start of a spill
    bool is_end_spill() const { return get_type() == end_spill; }     ///< True if this marks the end of a spill
    ///@}

    /** @name Time utilities */
    ///@{
    int coarse_time_clock() const { return get_coarse() + get_rollover() * rollover_to_clock; }             ///< Coarse time in clock counts (rollover × 32 768 + coarse)
    double coarse_time_ns() const { return get_coarse() * coarse_to_ns + get_rollover() * rollover_to_ns; } ///< Coarse time converted to nanoseconds (× 3.125 ns/cc)
    ///@}

    /** @name Mask utilities */
    ///@{
    void add_mask(uint32_t new_mask) { data.HitMask |= new_mask; }     ///< OR @p new_mask into the current Hit mask
    void add_mask_bit(int new_mask) { add_mask(encode_bit(new_mask)); } ///< Set a single bit at position @p new_mask in the Hit mask
    ///@}

    /** @name ROOT TTree I/O */
    ///@{
    /**
     * @brief Link internal members to branches of an input TTree.
     * @param input_tree Pointer to an existing TTree
     */
    void link_to_tree(TTree *input_tree);
    /**
     * @brief Write current data as branches to an output TTree.
     * @param output_tree Pointer to the output TTree
     */
    void write_to_tree(TTree *output_tree);
    ///@}
};