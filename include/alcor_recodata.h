#pragma once

#include <iostream>
#include <vector>
#include <array>
#include <map>
#include <cmath>
#include "TTree.h"
#include "triggers.h"
#include "alcor_spilldata.h"

struct alcor_recodata_struct
{
    float hit_x;
    float hit_y;
    float hit_t;
    uint32_t channel;
    float time;
    float fine_tune;
    uint32_t hit_mask;

    alcor_recodata_struct() = default;
};

class alcor_recodata
{
private:
    std::vector<trigger_struct> triggers;
    std::vector<trigger_struct> *triggers_ptr = &triggers;

    std::vector<alcor_recodata_struct> recodata;
    std::vector<alcor_recodata_struct> *recodata_ptr = &recodata;

public:
    // Constructors
    alcor_recodata() = default;
    explicit alcor_recodata(const std::vector<alcor_recodata_struct> &d);

    // Getters
    std::vector<alcor_recodata_struct> get() const;
    alcor_recodata_struct get(int i) const;
    float get_hit_x(int i) const;
    float get_hit_x_rnd(int i) const;
    float get_hit_y(int i) const;
    float get_hit_y_rnd(int i) const;
    float get_hit_t(int i) const;
    uint32_t get_channel(int i) const;
    float get_hit_time(int i) const;
    uint32_t get_hit_mask(int i) const;
    std::vector<trigger_struct> get_triggers() const;

    // Reference getters
    alcor_recodata_struct &get_link(int i);
    std::vector<trigger_struct> &get_triggers_link();

    // Derived
    int get_hit_tdc(int i) const;
    int get_device(int i) const;
    int get_fifo(int i) const;
    int get_chip(int i) const;
    int get_eo_channel(int i) const;
    int get_column(int i) const;
    int get_pixel(int i) const;
    int get_calib_index(int i) const;
    int get_device_index(int i) const;
    int get_global_index(int i) const;
    float get_hit_r(int i) const;
    float get_hit_r(int i, std::array<float, 2> v) const;
    float get_hit_r_rnd(int i, std::array<float, 2> v) const;
    float get_hit_phi(int i) const;
    float get_hit_phi(int i, std::array<float, 2> v) const;
    float get_hit_phi_rnd(int i, std::array<float, 2> v) const;

    // Setters
    void set_recodata(std::vector<alcor_recodata_struct> v);
    void set_hit_x(int i, float v);
    void set_hit_y(int i, float v);
    void set_hit_t(int i, float v);
    void set_channel(int i, uint32_t v);
    void set_hit_mask(int i, uint32_t v);
    void add_hit_mask(int i, uint32_t v);
    void set_time(int i, float v);
    void set_triggers(const std::vector<trigger_struct> &v);
    void set_recodata_link(std::vector<alcor_recodata_struct> &v);
    void set_triggers_link(std::vector<trigger_struct> &v);

    // I/O utilities
    void add_trigger(uint8_t index, uint16_t coarse, float fine_time = 0.);
    void add_trigger(trigger_struct hit);
    void add_hit(float hit_x, float hit_y, float hit_t, uint32_t channel, float time, uint32_t mask = 0);
    void add_hit(alcor_recodata_struct hit);
    void clear();
    void link_to_tree(TTree *input_tree);
    void write_to_tree(TTree *output_tree);
    void find_rings(float_t distance_length_cut, float_t distance_time_cut);
};
