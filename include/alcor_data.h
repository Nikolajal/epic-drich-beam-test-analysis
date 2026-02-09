#pragma once

/**
 * @file alcor_data.h
 * @brief Data structures and utilities for ALCOR hit-level data handling.
 *
 * This header defines:
 * * A base, retro compatible data struct (@ref alcor_data_struct) that reads data from ALCOR decoder
 * * Enumerations describing hit types and bit-level hit masks.
 * * A lightweight wrapper class (@ref alcor_data) that provides:
 *  - Safe accessors and mutators
 *  - Derived quantities (indices, global time, etc.)
 *  - Utility predicates (hit type checks)
 *  - ROOT I/O helpers
 *
 * The design intentionally separates *storage* (struct) from *logic*
 * (class) to keep ROOT I/O simple while allowing richer semantics
 * at the analysis level.
 */

#include "TTree.h"
#include "utility.h"

/**
 * @def *ALCOR_CC_TO_NS*
 * @brief Conversion factor from coarse clock units to nanoseconds.
 *
 * One coarse clock count corresponds to 3.125 ns.
 */
#define _ALCOR_CC_TO_NS_ 3.125
/**
 * @def *_ALCOR_ROLLOVER_TO_CC_*
 * @brief Conversion factor from rollover to coarse clock units.
 *
 * One rollover count corresponds to 32768 coarse clock counts.
 */
#define _ALCOR_ROLLOVER_TO_CC_ 32768

/**
 * @struct alcor_data_struct
 * @brief Plain data container for a single ALCOR hit.
 *
 * This struct is intentionally kept trivial:
 * * No logic
 * * No ownership semantics
 * * ROOT-friendly layout
 *
 * It represents exactly what is stored/read from a ROOT TTree
 * or decoded from the DAQ stream.
 */
struct alcor_data_struct
{
    int device;
    int fifo;
    int type;
    int counter;
    int column;
    int pixel;
    int tdc;
    int rollover;
    int coarse;
    int fine;
    uint32_t hit_mask;

    alcor_data_struct() = default;
};

/**
 * @enum alcor_hit_struct
 * @brief Encodes the logical type of an ALCOR hit.
 *
 * Let the user know if the hit in the stream is a simple hit, start or end of spill signal or a trigger hit
 */
enum alcor_hit_struct
{
    alcor_hit = 1,
    trigger_tag = 9,
    start_spill = 7,
    end_spill = 15
};

/**
 * @enum hit_mask
 * @brief Bit positions used inside @c alcor_data_struct::hit_mask.
 *
 * Each enumerator corresponds to a single bit index.
 */
enum hit_mask
{
    _HITMASK_ring_tag_first = 1,
    _HITMASK_ring_tag_second = 2,
    _HITMASK_afterpulse = 29,
    _HITMASK_part_lane = 30,
    _HITMASK_dead_lane = 31
};

/**
 * @class alcor_data
 * @brief Frist-level I/O helper class from alcor decoder
 *
 * This class wraps @ref alcor_data_struct and provides:
 * * Strongly-typed getters and setters
 * * Derived indices and global time computation
 * * Comparison operators for sorting
 * * Convenience checks on hit type and flags
 * * ROOT TTree input/output helpers
 *
 * The internal storage can either be copied or linked, depending
 * on how the setters are used.
 */
class alcor_data
{
private:
    alcor_data_struct data;
    static constexpr int rollover_to_clock = _ALCOR_ROLLOVER_TO_CC_;
    static constexpr double coarse_to_ns = _ALCOR_CC_TO_NS_;
    static constexpr int rollover_to_ns = rollover_to_clock * coarse_to_ns;

public:
    // Constructors
    alcor_data() = default;
    explicit alcor_data(const alcor_data_struct &data_struct);
    explicit alcor_data(int device, int fifo, int type, int counter,
                        int column, int pixel, int tdc, int rollover,
                        int coarse, int fine, uint32_t mask);

    // Getters
    //  --- Pure getters
    alcor_data_struct get_data_struct() const;
    int get_device() const;
    int get_fifo() const;
    int get_type() const;
    int get_counter() const;
    int get_column() const;
    int get_pixel() const;
    int get_tdc() const;
    int get_rollover() const;
    int get_coarse() const;
    int get_fine() const;
    uint32_t get_mask() const;

    //  --- Derived getters
    int get_chip() const;
    int get_eo_channel() const;
    int get_calib_index() const;
    int get_device_index() const;
    int get_global_index() const;
    int get_global_tdc_index() const;
    /**
     * @brief Global coarse time including rollovers.
     * @return Time in coarse clock units
     */
    uint64_t get_coarse_global_time() const;

    // Setters
    void set_data_struct_copy(alcor_data_struct input_data);
    void set_data_struct_linked(alcor_data_struct &input_data);
    void set_device(int val);
    void set_fifo(int val);
    void set_type(int val);
    void set_counter(int val);
    void set_column(int val);
    void set_pixel(int val);
    void set_tdc(int val);
    void set_rollover(int val);
    void set_coarse(int val);
    void set_fine(int val);
    void set_fine(uint32_t val);
    void set_mask(uint32_t val);

    // Comparison operators
    bool operator<(const alcor_data &comparing_hit) const;
    bool operator<=(const alcor_data &comparing_hit) const;
    bool operator>(const alcor_data &comparing_hit) const;
    bool operator>=(const alcor_data &comparing_hit) const;

    //  Utilities
    // --- Bool checks
    bool is_alcor_hit() const;
    bool is_trigger_tag() const;
    bool is_start_spill() const;
    bool is_end_spill() const;
    //  --- Time
    int coarse_time_clock() const;
    double coarse_time_ns() const;
    //  --- Time
    void add_mask(uint32_t new_mask);
    void add_mask_bit(int new_mask);

    // ROOT tree I/O
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
};