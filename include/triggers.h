#pragma once

#include <string>
#include <vector>
#include <cstdint>

enum trigger_number
{
    _TRIGGER_FIRST_FRAMES_ = 100,
    _TRIGGER_TIMING_ = 101,
    _TRIGGER_TRACKING_ = 102,
    _TRIGGER_RING_FOUND_ = 103,
    _TRIGGER_START_OF_SPILL_ = 200
};

struct trigger_struct
{
    uint8_t index;
    uint16_t coarse;
    float fine_time;

    trigger_struct() = default;
    trigger_struct(uint8_t idx);
    trigger_struct(uint8_t idx, uint16_t crs);
    trigger_struct(uint8_t idx, uint16_t crs, float fine);
};

struct trigger_config_struct
{
    std::string name;
    uint16_t index;
    uint16_t delay;
    uint16_t device;

    trigger_config_struct() = default;
    trigger_config_struct(const std::string& _name, uint16_t _index, uint16_t _delay, uint16_t _device);
};

// Declaration only
 std::vector<trigger_config_struct> trigger_conf_reader(const std::string& config_file = "Data/test.txt");
