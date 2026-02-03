#pragma once

#include "TTree.h"
#include "utility.h"

#define _ALCOR_CC_TO_NS_ 3.125

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

    alcor_data_struct() = default;
};

enum alcor_hit_struct
{
    alcor_hit = 1,
    trigger_tag = 9,
    start_spill = 7,
    end_spill = 15
};

class alcor_data
{
private:
    alcor_data_struct data;
    static constexpr int rollover_to_clock = 32768;
    static constexpr double coarse_to_ns = _ALCOR_CC_TO_NS_;
    static constexpr int rollover_to_ns = rollover_to_clock * coarse_to_ns;

public:
    // Constructors
    alcor_data() = default;
    explicit alcor_data(const alcor_data_struct &data_struct);
    alcor_data(int device, int fifo, int type, int counter,
               int column, int pixel, int tdc, int rollover,
               int coarse, int fine);

    // Getters
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

    // Derived getters
    int get_chip() const;
    int get_eo_channel() const;
    int get_calib_index() const;
    int get_device_index() const;
    int get_global_index() const;
    int get_global_tdc_index() const;
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

    // Hit checks
    bool is_alcor_hit() const;
    bool is_trigger_tag() const;
    bool is_start_spill() const;
    bool is_end_spill() const;

    // Time
    int coarse_time_clock() const;
    double coarse_time_ns() const;

    // Comparison operators
    bool operator<(const alcor_data &comparing_hit) const;
    bool operator<=(const alcor_data &comparing_hit) const;
    bool operator>(const alcor_data &comparing_hit) const;
    bool operator>=(const alcor_data &comparing_hit) const;

    // ROOT tree I/O
    void link_to_tree(TTree *input_tree);
    void write_to_tree(TTree *output_tree);
};
