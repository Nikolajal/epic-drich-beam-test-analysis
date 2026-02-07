#pragma once

#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <cstdint>

// Core tags
static const std::set<std::string> lightdata_core_tags = {
    "timing",
    "tracking",
    "cherenkov"};

// --- Readout configuration structures ---
struct readout_config_struct
{
    std::string name;
    std::map<uint16_t, std::vector<uint16_t>> device_chip;

    readout_config_struct() = default;
    readout_config_struct(std::string _name, std::map<uint16_t, std::vector<uint16_t>> _device_chip);

    void add_device_chip(uint16_t device, uint16_t chip);
    void add_device(uint16_t device);
};

// --- List of readout configs ---
class readout_config_list
{
public:
    std::vector<readout_config_struct> configs;

    readout_config_list() = default;
    explicit readout_config_list(std::vector<readout_config_struct> vec);

    // Search methods
    readout_config_struct *find_by_name(const std::string &name);
    readout_config_struct *find_by_device(uint16_t device);
    std::vector<readout_config_struct *> find_all_by_device(uint16_t device);
    std::vector<std::string> find_by_device_and_chip(uint16_t device, uint16_t chip);
    bool has_name(const std::string &name) const;

    // Provided config flags
    bool has_cherenkov();
    bool has_timing();
    bool has_tracking();
};

// --- Utility functions ---
 std::vector<std::string> find_by_device_and_chip(
    const std::map<std::string, readout_config_struct> &readout_config_utility,
    uint16_t device,
    uint16_t chip);

 std::vector<readout_config_struct> readout_config_reader(std::string config_file = "conf/readout_config.txt");
