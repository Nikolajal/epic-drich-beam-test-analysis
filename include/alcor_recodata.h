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

/**
 * @brief Structure representing a single hit in the reco data.
 *
 * Contains the global channel index, hit coordinates (x, y, t),
 * and a mask describing hit characteristics.
 */
struct alcor_recodata_struct
{
    uint32_t global_index;
    float hit_x;
    float hit_y;
    float hit_t;
    uint32_t hit_mask;

    /// Default constructor
    alcor_recodata_struct() = default;
    alcor_recodata_struct(uint32_t gi, float x, float y, uint32_t mask, float ht)
        : global_index(gi), hit_x(x), hit_y(y), hit_t(ht), hit_mask(mask) {}
};

/**
 * @brief Class managing a collection of reconstructed hits and triggers.
 *
 * Provides getters, setters, derived coordinates (polar, randomized),
 * utilities to manipulate hit masks and triggers, and ROOT I/O helpers.
 */
class alcor_recodata
{
private:
    std::vector<trigger_struct> triggers;
    std::vector<trigger_struct> *triggers_ptr = &triggers;
    std::vector<alcor_recodata_struct> recodata;
    std::vector<alcor_recodata_struct> *recodata_ptr = &recodata;

public:
    alcor_recodata() = default;
    explicit alcor_recodata(const std::vector<alcor_recodata_struct> &d);

    /** @name Pure Getters */
    ///@{
    std::vector<alcor_recodata_struct> get_recodata() const;
    std::vector<alcor_recodata_struct> *get_recodata_ptr();
    alcor_recodata_struct get_recodata(int i) const;
    std::vector<trigger_struct> get_triggers() const;
    std::vector<trigger_struct> *get_triggers_ptr();
    int get_global_index(int i) const;
    float get_hit_x(int i) const;
    float get_hit_y(int i) const;
    float get_hit_t(int i) const;
    uint32_t get_hit_mask(int i) const;
    ///@}

    /** @name Reference Getters */
    ///@{
    std::vector<alcor_recodata_struct> &get_recodata_link();
    alcor_recodata_struct &get_recodata_link(int i);
    std::vector<trigger_struct> &get_triggers_link();
    ///@}

    /** @name Derived / Coordinate Utilities */
    ///@{
    float get_hit_r(int i) const;
    float get_hit_r(int i, std::array<float, 2> v) const;
    float get_hit_phi(int i) const;
    float get_hit_phi(int i, std::array<float, 2> v) const;

    float get_hit_x_rnd(int i) const;
    float get_hit_y_rnd(int i) const;
    float get_hit_r_rnd(int i) const;
    float get_hit_r_rnd(int i, std::array<float, 2> v) const;
    float get_hit_phi_rnd(int i) const;
    float get_hit_phi_rnd(int i, std::array<float, 2> v) const;

    int get_hit_tdc(int i) const;
    int get_device(int i) const;
    int get_fifo(int i) const;
    int get_chip(int i) const;
    int get_eo_channel(int i) const;
    int get_column(int i) const;
    int get_pixel(int i) const;
    int get_calib_index(int i) const;
    int get_device_index(int i) const;

    std::optional<trigger_struct> get_trigger_by_index(uint8_t index) const;
    ///@}

    /** @name Pure Setters */
    ///@{
    void set_recodata(std::vector<alcor_recodata_struct> v);
    void set_recodata(int i, alcor_recodata_struct v);
    void set_triggers(const std::vector<trigger_struct> v);
    void set_recodata_ptr(std::vector<alcor_recodata_struct> *v);
    void set_triggers_ptr(std::vector<trigger_struct> *v);
    void set_global_index(int i, uint32_t v);
    void set_hit_x(int i, float v);
    void set_hit_y(int i, float v);
    void set_hit_t(int i, float v);
    void set_hit_mask(int i, uint32_t v);
    ///@}

    /** @name Reference Setters */
    ///@{
    void set_recodata_link(std::vector<alcor_recodata_struct> &v);
    void set_triggers_link(std::vector<trigger_struct> &v);
    ///@}

    /** @name Hit Utilities */
    ///@{
    void add_hit_mask(int i, uint32_t v);
    void add_trigger(uint8_t index, uint16_t coarse, float fine_time = 0.);
    void add_trigger(trigger_struct hit);
    void add_hit(uint32_t gi, float x, float y, uint32_t mask, float ht);
    void add_hit(alcor_recodata_struct hit);
    ///@}

    /** @name Boolean Checks */
    ///@{
    bool check_trigger(uint8_t v);
    /**
     * @brief Return if the event is flagged as start of spill.
     * Start of spill events list all available channels, so they can be used to keep track of spills and channel availability.
     * @return Flag stating if the event is start of spill
     */
    bool is_start_of_spill();
    bool is_first_frames();
    bool is_timing_available();
    bool is_embedded_tracking_available();
    bool is_ring_found();
    bool check_hit_mask(int i, uint32_t v);
    /**
     * @brief Return if the hit is flagged as afterpulse.
     * During reconstruction a check is performed to flag the second hit of consequent hits as afterpulse if its time is 200ns or less w.r.t. the previous hit.
     * @return Flag stating if the hit is an afterpulse or not
     */
    bool is_afterpulse(int i);

    /**
     * @brief Return if the hit is flagged as afterpulse.
     * During reconstruction a check is performed to flag the second hit of consequent hits as afterpulse if its time is 200ns or less w.r.t. the previous hit.
     * @return Flag stating if the hit is an afterpulse or not
     */
    bool is_ring_tagged(int i);
    ///@}

    /** @name I/O Utilities */
    ///@{
    void clear();
    void link_to_tree(TTree *input_tree);
    void write_to_tree(TTree *output_tree);
    ///@}

    /** @name Analysis Utilities */
    ///@{

    /**
     * @brief Runs a custom-made DBSCAN (https://it.wikipedia.org/wiki/DBSCAN) to cluster hits in R and time.
     */
    void find_rings(float_t distance_length_cut, float_t distance_time_cut);
    ///@}
};
