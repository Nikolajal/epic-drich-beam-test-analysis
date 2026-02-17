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
#include <cstdint>
#include <utility.h>
#include <toml++/toml.h>

//  TODO: re-write for TOML configuration files

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

class config_reader
{
public:
    //  Utility to read runlists
    static inline void runlists(std::string runlist_file)
    {
        logger::log_info(Form("(readout_config_list::runlists) reading run list: %s", runlist_file.c_str()));
    }
};

struct radiator_info_struct
{
    std::string name;
    double refindex;
    std::string tag;
    double depth;
    std::string side; // optional
};

struct run_info_struct
{
    int aerogel_mirror;
    std::string beam_polarity;
    int beam_energy;
    std::string rdo_firmware;
    std::string timing_firmware;
    double temperature;
    double v_bias;
    int n_spills;
    bool timing_on_axis;
    std::vector<radiator_info_struct> radiators;
};

class run_info
{
public:
    static void read_database(std::string filename)
    {
        logger::log_info(Form("(run_info::read_database) Reading run list: %s", filename.c_str()));

        //  Parse data base
        auto loaded_tables = toml::parse_file(filename);

        try
        {
            // Load TOML file
            auto tbl = toml::parse_file(filename);

            auto runs_table = tbl["runs"].as_table();
            if (!runs_table)
                logger::log_warning("(run_info::read_database) No [runs] table found in TOML file. Skipping run info loading.");
            else
                logger::log_info("(run_info::read_database) Found runs table, loading contents");

            // Iterate over each run
            for (auto &[run_id, run_entry] : *runs_table)
            {
                if (auto *run_tbl = run_entry.as_table())
                {
                    auto &current_run_info = run_info_database[std::string(run_id)];
                    if (auto *n = run_tbl->get("aerogel_mirror"))
                        current_run_info.aerogel_mirror = n->value_or(0);
                    else
                        current_run_info.aerogel_mirror = 0;
                    if (auto *n = run_tbl->get("beam_polarity"))
                        current_run_info.beam_polarity = n->value_or("");
                    else
                        current_run_info.beam_polarity = "";
                    if (auto *n = run_tbl->get("beam_energy"))
                        current_run_info.beam_energy = n->value_or(0);
                    else
                        current_run_info.beam_energy = 0;
                    if (auto *n = run_tbl->get("rdo_firmware"))
                        current_run_info.rdo_firmware = n->value_or("");
                    else
                        current_run_info.rdo_firmware = "";
                    if (auto *n = run_tbl->get("timing_firmware"))
                        current_run_info.timing_firmware = n->value_or("");
                    else
                        current_run_info.timing_firmware = "";
                    if (auto *n = run_tbl->get("temperature"))
                        current_run_info.temperature = n->value_or(0.0);
                    else
                        current_run_info.temperature = 0.0;
                    if (auto *n = run_tbl->get("v_bias"))
                        current_run_info.v_bias = n->value_or(0.0);
                    else
                        current_run_info.v_bias = 0.0;
                    if (auto *n = run_tbl->get("n_spills"))
                        current_run_info.n_spills = n->value_or(0);
                    else
                        current_run_info.n_spills = 0;
                    if (auto *n = run_tbl->get("timing_on_axis"))
                        current_run_info.timing_on_axis = n->value_or(true);
                    else
                        current_run_info.timing_on_axis = false;

                    // Parse radiators
                    if (auto rad_node = run_tbl->get("radiators"))
                    {
                        if (auto rad_array = rad_node->as_array())
                        {
                            for (auto &r : *rad_array)
                            {
                                if (auto r_tbl = r.as_table())
                                {
                                    auto &rad = current_run_info.radiators.emplace_back();
                                    if (auto *n = r_tbl->get("name"))
                                        rad.name = n->value_or("");
                                    if (auto *n = r_tbl->get("refindex"))
                                        rad.refindex = n->value_or(0.0);
                                    if (auto *n = r_tbl->get("tag"))
                                        rad.tag = n->value_or("");
                                    if (auto *n = r_tbl->get("depth"))
                                        rad.depth = n->value_or(0.0);
                                    if (auto *n = r_tbl->get("side"))
                                        rad.side = n->value_or("");
                                }
                            }
                        }
                    }
                }
            }
        }
        catch (const toml::parse_error &err)
        {
            logger::log_warning(Form("(run_info::read_database) File for run info \"%s\" does not contain the TOML tables expected, skipping info loading.", filename.c_str()));
        }
    }
    static void clear_database()
    {
        run_info_database.clear();
    }
    static const run_info_struct *get_run_info(const std::string &run_id)
    {
        auto it = run_info_database.find(run_id);
        if (it != run_info_database.end())
            return &it->second;
        return nullptr;
    }
private:
    static std::unordered_map<std::string, run_info_struct> run_info_database;
};