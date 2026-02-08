#pragma once

#include <vector>
#include <iostream>
#include <cstdint>
#include <unordered_map>
#include <map>

#include "alcor_lightdata.h"
#include "TH1.h"
#include "TTree.h"
#include "triggers.h"

struct data_mask_struct
{
    uint8_t device;
    uint32_t mask;
};

struct alcor_spilldata_struct
{
    //  Constructors
    //  Default constructor
    alcor_spilldata_struct() noexcept = default;

    //  --- Move constructor
    alcor_spilldata_struct(alcor_spilldata_struct &&) noexcept = default;
    alcor_spilldata_struct &operator=(alcor_spilldata_struct &&) noexcept = default;

    //  --- Copy constructor / assignment
    alcor_spilldata_struct(const alcor_spilldata_struct &) = default;
    alcor_spilldata_struct &operator=(const alcor_spilldata_struct &) = default;

    std::unordered_map<uint8_t, uint32_t> dead_mask;
    std::unordered_map<uint8_t, uint32_t> participants_mask;
    std::unordered_map<uint32_t, alcor_lightdata_struct> frame_and_lightdata;

    std::vector<data_mask_struct> dead_mask_list;
    std::vector<data_mask_struct> participants_mask_list;
    std::vector<uint32_t> frame_reference;
    std::vector<alcor_lightdata_struct> lightdata_list_in_frame;
    std::vector<data_mask_struct> *dead_mask_list_ptr;
    std::vector<data_mask_struct> *participants_mask_list_ptr;
    std::vector<uint32_t> *frame_reference_ptr;
    std::vector<alcor_lightdata_struct> *lightdata_list_in_frame_ptr;

    void clear();
};

class alcor_spilldata : public alcor_lightdata
{
private:
    alcor_spilldata_struct spilldata;
    std::unordered_map<uint32_t, bool> frame_reference_for_deletion;

public:
    //  Constructors
    alcor_spilldata();
    alcor_spilldata(const alcor_spilldata_struct &v);

    // Getters (copied value)
    alcor_spilldata_struct get_spilldata() const;
    std::unordered_map<uint32_t, alcor_lightdata_struct> get_frame() const;
    std::unordered_map<uint8_t, uint32_t> get_participants_mask() const;
    std::unordered_map<uint8_t, uint32_t> get_dead_mask() const;
    std::vector<alcor_lightdata_struct> get_frame_list() const;
    std::vector<uint32_t> get_frame_reference_list() const;

    // Getters (linked reference)
    alcor_spilldata_struct &get_spilldata_link();
    std::unordered_map<uint32_t, alcor_lightdata_struct> &get_frame_link();
    std::unordered_map<uint8_t, uint32_t> &get_participants_mask_link();
    std::unordered_map<uint8_t, uint32_t> &get_dead_mask_link();
    std::vector<alcor_lightdata_struct> &get_frame_list_link();
    std::vector<uint32_t> &get_frame_reference_list_link();

    // Setters (copied value)
    void set_spilldata(alcor_spilldata_struct v);
    void set_frame(std::unordered_map<uint32_t, alcor_lightdata_struct> v);
    void set_participants_mask(std::unordered_map<uint8_t, uint32_t> v);
    void set_dead_mask(std::unordered_map<uint8_t, uint32_t> v);

    // Setters (linked reference)
    void set_spilldata_link(alcor_spilldata_struct &v);
    void set_frame_link(std::unordered_map<uint32_t, alcor_lightdata_struct> &v);
    void set_participants_mask_link(std::unordered_map<uint8_t, uint32_t> &v);
    void set_dead_mask_link(std::unordered_map<uint8_t, uint32_t> &v);

    // Utilities
    void clear();
    bool has_trigger(uint32_t index_of_frame);
    void add_trigger_to_frame(uint32_t index_of_frame, trigger_struct trg);
    void do_not_write_frame(uint32_t index_of_frame);
    std::map<uint32_t, std::vector<uint8_t>> get_not_dead_participants();
    bool has_data();

    // I/O operations
    void link_to_tree(TTree *input_tree);
    void write_to_tree(TTree *output_tree);
    void prepare_tree_fill();
    void get_entry();
};

//  Reduce utilities
//  TODO move this and implement for all data structs
void merge_lightdata(alcor_lightdata_struct &lhs, alcor_lightdata_struct &&rhs);
alcor_spilldata_struct operator+(alcor_spilldata_struct lhs, alcor_spilldata_struct rhs);
void merge(alcor_spilldata_struct &lhs, alcor_spilldata_struct &&rhs);
alcor_spilldata operator+(alcor_spilldata lhs, const alcor_spilldata &rhs);
void merge(alcor_spilldata &lhs, alcor_spilldata &&rhs);