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

struct alcor_recodata_struct
{
    //  Channel w/ TDC info
    uint32_t global_index;

    //  X, Y hit position
    float hit_x;
    float hit_y;

    //  Mask to characterise the hit
    uint32_t hit_mask;

    //  Clearly something is wrong...
    //  TODO: Clean-up
    float hit_t;
    float time;
    float fine_tune;

    alcor_recodata_struct() = default;
    alcor_recodata_struct(uint32_t gi, float x, float y, uint32_t mask, float ht)
        : global_index(gi), hit_x(x), hit_y(y), hit_t(ht), hit_mask(mask) {}
};

class alcor_recodata
{
private:
    //  Fix this I/O ROOT Hack
    //  Implement std::unique_ptr for safe memory management
    std::vector<trigger_struct> triggers;
    std::vector<trigger_struct> *triggers_ptr = &triggers;
    std::vector<alcor_recodata_struct> recodata;
    std::vector<alcor_recodata_struct> *recodata_ptr = &recodata;

public:
    // Constructors
    alcor_recodata() = default;
    explicit alcor_recodata(const std::vector<alcor_recodata_struct> &d);

    // Getters
    //  --- Pure getters
    std::vector<alcor_recodata_struct> get_recodata() const;
    alcor_recodata_struct get_recodata(int i) const;
    std::vector<trigger_struct> get_triggers() const;
    int get_global_index(int i) const;
    float get_hit_x(int i) const;
    float get_hit_y(int i) const;
    uint32_t get_hit_mask(int i) const;
    float get_hit_t(int i) const;

    //  --- Reference getters
    std::vector<alcor_recodata_struct> &get_recodata_link();
    alcor_recodata_struct &get_recodata_link(int i);
    std::vector<trigger_struct> &get_triggers_link();

    //  --- Derived
    //  --- --- Polar coordinates
    float get_hit_r(int i) const;
    float get_hit_r(int i, std::array<float, 2> v) const;
    float get_hit_phi(int i) const;
    float get_hit_phi(int i, std::array<float, 2> v) const;
    //  --- --- Randomised coordinates
    float get_hit_x_rnd(int i) const;
    float get_hit_y_rnd(int i) const;
    float get_hit_r_rnd(int i) const;
    float get_hit_r_rnd(int i, std::array<float, 2> v) const;
    float get_hit_phi_rnd(int i) const;
    float get_hit_phi_rnd(int i, std::array<float, 2> v) const;
    //  --- --- Reconstruction info
    int get_hit_tdc(int i) const;
    int get_device(int i) const;
    int get_fifo(int i) const;
    int get_chip(int i) const;
    int get_eo_channel(int i) const;
    int get_column(int i) const;
    int get_pixel(int i) const;
    int get_calib_index(int i) const;
    int get_device_index(int i) const;

    // Setters
    //  --- Pure setters
    void set_recodata(std::vector<alcor_recodata_struct> v);
    void set_recodata(int i, alcor_recodata_struct v);
    void set_triggers(const std::vector<trigger_struct> v);
    void set_global_index(int i, uint32_t v);
    void set_hit_x(int i, float v);
    void set_hit_y(int i, float v);
    void set_hit_mask(int i, uint32_t v);
    void set_hit_t(int i, float v);

    //  --- Reference getters
    void set_recodata_link(std::vector<alcor_recodata_struct> &v);
    void set_triggers_link(std::vector<trigger_struct> &v);

    //  Add utilities
    void add_hit_mask(int i, uint32_t v);
    void add_trigger(uint8_t index, uint16_t coarse, float fine_time = 0.);
    void add_trigger(trigger_struct hit);
    void add_hit(uint32_t gi, float x, float y, uint32_t mask, float ht);
    void add_hit(alcor_recodata_struct hit);

    //  Bool checks
    bool check_hit_mask(int i, uint32_t v);
    bool is_afterpulse(int i);
    bool is_ring_tagged(int i);

    //  I/O utilities
    void clear();
    void link_to_tree(TTree *input_tree);
    void write_to_tree(TTree *output_tree);

    //  Analysis utilities
    void find_rings(float_t distance_length_cut, float_t distance_time_cut);
};
