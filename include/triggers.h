#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "TH2.h"

enum trigger_number
{
    _TRIGGER_FIRST_FRAMES_ = 100,
    _TRIGGER_TIMING_ = 101,
    _TRIGGER_TRACKING_ = 102,
    _TRIGGER_RING_FOUND_ = 103,
    _TRIGGER_STREAMING_RING_FOUND_ = 104,
    _TRIGGER_HOUGH_RING_FOUND_ = 105,
    _TRIGGER_START_OF_SPILL_ = 200,
    _TRIGGER_UNKNOWN_ = 255
};
constexpr trigger_number all_default_triggers[] = {
    _TRIGGER_FIRST_FRAMES_,
    _TRIGGER_TIMING_,
    _TRIGGER_TRACKING_,
    _TRIGGER_RING_FOUND_,
    _TRIGGER_STREAMING_RING_FOUND_,
    _TRIGGER_HOUGH_RING_FOUND_,
    _TRIGGER_START_OF_SPILL_,
    _TRIGGER_UNKNOWN_};
constexpr const char *default_names[] = {
    "FIRST_FRAMES", "TIMING", "TRACKING",
    "RING_FOUND", "STREAMING_RING_FOUND",
    "HOUGH_RING_FOUND",
    "START_OF_SPILL", "UNKNOWN"};
constexpr int n_default_triggers = std::size(all_default_triggers);
constexpr int default_trigger_index(trigger_number t)
{
    for (int i = 0; i < n_default_triggers; ++i)
        if (all_default_triggers[i] == t)
            return i;
    return -1; // not found
}

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
    trigger_config_struct(const std::string &_name, uint16_t _index, uint16_t _delay, uint16_t _device);
};

// Runtime trigger registry — built once after config is loaded
struct trigger_registry
{
    std::vector<std::pair<int, std::string>> triggers; // {enum value, name} ordered: config first, defaults after

    // Build from config + defaults
    trigger_registry() = default;
    trigger_registry(const std::vector<trigger_config_struct> &config_triggers)
    {
        // Config triggers first (indices 0,1,2,...)
        for (auto &t : config_triggers)
            triggers.push_back({t.index, t.name});
        for (int i = 0; i < n_default_triggers; ++i)
            triggers.push_back({all_default_triggers[i], default_names[i]});
    }

    int index_of(int trigger_value) const
    {
        for (int i = 0; i < (int)triggers.size(); ++i)
            if (triggers[i].first == trigger_value)
                return i;
        return default_trigger_index(_TRIGGER_UNKNOWN_);
    }

    std::string name_of(int trigger_value) const { return triggers[index_of(trigger_value)].second; }

    int size() const { return triggers.size(); }

    void label_axes(TH2F *h) const
    {
        for (int i = 0; i < (int)triggers.size(); ++i)
        {
            h->GetXaxis()->SetBinLabel(i + 1, triggers[i].second.c_str());
            h->GetYaxis()->SetBinLabel(i + 1, triggers[i].second.c_str());
        }
    }
};

// Declaration only
std::vector<trigger_config_struct> trigger_conf_reader(const std::string &config_file = "Data/test.txt");
