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

struct radiator_info_struct
{
    std::string type;
    std::string tag;
    double refindex;
    double depth;
    std::string side;
};

struct run_info_struct
{
    //  Beam configuration
    std::string beam_polarity;
    int beam_energy;

    //  DAQ configuration
    std::string rdo_firmware;
    std::string timing_firmware;
    int n_spills;
    bool timing_on_axis;
    //  --- ALCOR
    int op_mode;
    int delta_thr;

    //  Sensors conditions
    double temperature;
    double v_bias;

    //  Optics info
    int aerogel_mirror;
    int gas_mirror;

    //  Radiator info
    std::vector<radiator_info_struct> radiators;
};

class run_info
{
public:
    //  Pure run info
    static void read_database(std::string filename)
    {
        logger::log_info(Form("(run_info::read_database) Reading run database: %s", filename.c_str()));

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
            std::string previous_run_id = "";
            for (auto &[run_id, run_entry] : *runs_table)
            {
                if (auto *run_tbl = run_entry.as_table())
                {
                    //  Recover current run info element
                    auto &current_run_info = run_info_database[std::string(run_id)];

                    //  Beam configuration
                    //  --- Beam polarity
                    if (auto *n = run_tbl->get("beam_polarity"))
                        current_run_info.beam_polarity = n->value_or("");
                    else if (previous_run_id.empty())
                        current_run_info.beam_polarity = "";
                    else
                        current_run_info.beam_polarity = run_info_database[previous_run_id].beam_polarity;
                    //  --- Beam energy
                    if (auto *n = run_tbl->get("beam_energy"))
                        current_run_info.beam_energy = n->value_or(0);
                    else if (previous_run_id.empty())
                        current_run_info.beam_energy = 0.;
                    else
                        current_run_info.beam_energy = run_info_database[previous_run_id].beam_energy;

                    //  DAQ configuration
                    //  --- RDO Firmware
                    if (auto *n = run_tbl->get("rdo_firmware"))
                        current_run_info.rdo_firmware = n->value_or("");
                    else if (previous_run_id.empty())
                        current_run_info.rdo_firmware = "";
                    else
                        current_run_info.rdo_firmware = run_info_database[previous_run_id].rdo_firmware;
                    //  --- Timing Firmware
                    if (auto *n = run_tbl->get("timing_firmware"))
                        current_run_info.timing_firmware = n->value_or("");
                    else if (previous_run_id.empty())
                        current_run_info.timing_firmware = "";
                    else
                        current_run_info.timing_firmware = run_info_database[previous_run_id].timing_firmware;
                    //  --- Number of spills
                    if (auto *n = run_tbl->get("n_spills"))
                        current_run_info.n_spills = n->value_or(0);
                    else if (previous_run_id.empty())
                        current_run_info.n_spills = 0;
                    else
                        current_run_info.n_spills = run_info_database[previous_run_id].n_spills;
                    //  --- Timing on axis
                    if (auto *n = run_tbl->get("timing_on_axis"))
                        current_run_info.timing_on_axis = n->value_or(true);
                    else if (previous_run_id.empty())
                        current_run_info.timing_on_axis = true;
                    else
                        current_run_info.timing_on_axis = run_info_database[previous_run_id].timing_on_axis;
                    //  --- Operational mode
                    if (auto *n = run_tbl->get("op_mode"))
                        current_run_info.op_mode = n->value_or(1);
                    else if (previous_run_id.empty())
                        current_run_info.op_mode = 1;
                    else
                        current_run_info.op_mode = run_info_database[previous_run_id].op_mode;
                    //  --- Delta threshold
                    if (auto *n = run_tbl->get("delta_thr"))
                        current_run_info.delta_thr = n->value_or(10);
                    else if (previous_run_id.empty())
                        current_run_info.delta_thr = 10;
                    else
                        current_run_info.delta_thr = run_info_database[previous_run_id].delta_thr;

                    //  Sensors conditions
                    //  --- Temperature
                    if (auto *n = run_tbl->get("temperature"))
                        current_run_info.temperature = n->value_or(0.0);
                    else if (previous_run_id.empty())
                        current_run_info.temperature = 0.0;
                    else
                        current_run_info.temperature = run_info_database[previous_run_id].temperature;
                    //  --- Vbias
                    if (auto *n = run_tbl->get("v_bias"))
                        current_run_info.v_bias = n->value_or(0.0);
                    else if (previous_run_id.empty())
                        current_run_info.v_bias = 0.0;
                    else
                        current_run_info.v_bias = run_info_database[previous_run_id].v_bias;

                    //  Optics info
                    //  --- Aerogel mirror
                    if (auto *n = run_tbl->get("aerogel_mirror"))
                        current_run_info.aerogel_mirror = n->value_or(0);
                    else if (previous_run_id.empty())
                        current_run_info.aerogel_mirror = 0;
                    else
                        current_run_info.aerogel_mirror = run_info_database[previous_run_id].aerogel_mirror;
                    //  --- Gas mirror
                    if (auto *n = run_tbl->get("gas_mirror"))
                        current_run_info.gas_mirror = n->value_or(0);
                    else if (previous_run_id.empty())
                        current_run_info.gas_mirror = 0;
                    else
                        current_run_info.gas_mirror = run_info_database[previous_run_id].gas_mirror;

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
                                    if (auto *n = r_tbl->get("type"))
                                        rad.type = n->value_or("");
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
                    else
                    {
                        if (!previous_run_id.empty())
                            current_run_info.radiators = run_info_database[previous_run_id].radiators;
                    }

                    logger::log_debug("test: current_run_info.temperature = " + std::to_string(current_run_info.temperature));
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
    static const std::optional<run_info_struct> get_run_info(const std::string &run_id)
    {
        auto it = run_info_database.find(run_id);
        if (it != run_info_database.end())
            return it->second;
        return std::nullopt;
    }

    //  Run list management
    //  Utility to read runlists
    static inline void read_runslists(std::string runlist_file)
    {
        logger::log_info(Form("(run_info::read_runslists) Reading run list: %s", runlist_file.c_str()));

        try
        {
            // Parse the TOML file
            auto tbl = toml::parse_file(runlist_file);

            auto runs_table = tbl["runlists"].as_table();
            if (!runs_table)
                logger::log_warning("(run_info::read_runslists) No [runlists] table found in TOML file. Skipping run list loading.");
            else
                logger::log_info("(run_info::read_runslists) Found runlists table, loading contents");

            // Access the "runlists" table
            if (auto runlists_table = tbl["runlists"].as_table())
            {
                // Iterate over all runlists
                for (auto &[runlist_name, runlist_entry] : *runlists_table)
                {
                    if (auto *runlist_tbl = runlist_entry.as_table())
                    {
                        auto &current_runlist = run_list_database[std::string(runlist_name)];

                        // Read array of runs
                        if (auto *runs_array = runlist_tbl->get("runs")->as_array())
                        {
                            for (auto &r : *runs_array)
                            {
                                if (auto run_str = r.value<std::string>())
                                    current_runlist.push_back(*run_str);
                            }
                        }
                    }
                }
            }
        }
        catch (const toml::parse_error &err)
        {
            logger::log_warning(Form("(run_info::read_runslists) File for run list \"%s\" does not contain the TOML tables expected, skipping info loading.", runlist_file.c_str()));
        }
    }
    static const std::optional<std::vector<std::string>> get_run_list(const std::string &runlist_name)
    {
        auto it = run_list_database.find(runlist_name);
        if (it != run_list_database.end())
            return it->second;
        return std::nullopt;
    }

private:
    static std::unordered_map<std::string, run_info_struct> run_info_database;
    static std::unordered_map<std::string, std::vector<std::string>> run_list_database;
};