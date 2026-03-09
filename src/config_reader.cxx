#include "config_reader.h"

// --- readout_config_struct definitions ---
readout_config_struct::readout_config_struct(std::string _name, std::map<uint16_t, std::vector<uint16_t>> _device_chip)
    : name(_name), device_chip(_device_chip) {}

void readout_config_struct::add_device_chip(uint16_t device, uint16_t chip)
{
    if (device_chip.find(device) == device_chip.end())
        device_chip[device] = std::vector<uint16_t>();
    device_chip[device].push_back(chip);
}

void readout_config_struct::add_device(uint16_t device)
{
    if (device_chip.find(device) == device_chip.end())
        device_chip[device] = std::vector<uint16_t>();
    for (auto i_chip = 0; i_chip < 8; ++i_chip)
        device_chip[device].push_back(i_chip);
}

// --- readout_config_list definitions ---
readout_config_list::readout_config_list(std::vector<readout_config_struct> vec)
    : configs(std::move(vec)) {}

readout_config_struct *readout_config_list::find_by_name(const std::string &name)
{
    auto it = std::find_if(configs.begin(), configs.end(),
                           [&](const readout_config_struct &cfg)
                           { return cfg.name == name; });
    return (it != configs.end()) ? &(*it) : nullptr;
}

readout_config_struct *readout_config_list::find_by_device(uint16_t device)
{
    auto it = std::find_if(configs.begin(), configs.end(),
                           [&](const readout_config_struct &cfg)
                           { return cfg.device_chip.count(device) > 0; });
    return (it != configs.end()) ? &(*it) : nullptr;
}

std::vector<readout_config_struct *> readout_config_list::find_all_by_device(uint16_t device)
{
    std::vector<readout_config_struct *> out;
    for (auto &cfg : configs)
        if (cfg.device_chip.count(device))
            out.push_back(&cfg);
    return out;
}

std::vector<std::string> readout_config_list::find_by_device_and_chip(uint16_t device, uint16_t chip)
{
    std::vector<std::string> out;
    for (auto &cfg : configs)
    {
        auto it = cfg.device_chip.find(device);
        if (it != cfg.device_chip.end())
        {
            const auto &chips = it->second;
            if (std::find(chips.begin(), chips.end(), chip) != chips.end())
                out.push_back(cfg.name);
        }
    }
    return out;
}

bool readout_config_list::has_name(const std::string &name) const
{
    return std::any_of(configs.begin(), configs.end(),
                       [&](const readout_config_struct &cfg)
                       { return cfg.name == name; });
}

bool readout_config_list::has_cherenkov() { return find_by_name("cherenkov"); }
bool readout_config_list::has_timing() { return find_by_name("timing"); }
bool readout_config_list::has_tracking() { return find_by_name("tracking"); }

// --- Utility functions ---
std::vector<std::string> find_by_device_and_chip(
    const std::map<std::string, readout_config_struct> &readout_config_utility,
    uint16_t device,
    uint16_t chip)
{
    std::vector<std::string> names;

    for (const auto &pair : readout_config_utility)
    {
        const auto &cfg_name = pair.first;
        const auto &cfg = pair.second;

        auto it = cfg.device_chip.find(device);
        if (it != cfg.device_chip.end())
        {
            const auto &chips = it->second;
            if (std::find(chips.begin(), chips.end(), chip) != chips.end())
                names.push_back(cfg_name);
        }
    }

    return names;
}

std::vector<readout_config_struct> readout_config_reader(std::string config_file)
{
    std::vector<readout_config_struct> readout_config;
    std::map<std::string, readout_config_struct> readout_config_utility;
    
    try
    {
        auto tbl = toml::parse_file(config_file);

        auto readout_table = tbl["readout"].as_table();
        if (!readout_table)
        {
            mist::logger::warning("(readout_config_reader) No [readout] table found in TOML file.");
            return readout_config;
        }

        mist::logger::info(Form("(readout_config_reader) Reading readout config: %s", config_file.c_str()));

        for (auto &[name, entry] : *readout_table)
        {
            std::string cfg_name = std::string(name);
            auto *entry_tbl = entry.as_table();
            if (!entry_tbl)
                continue;

            if (!readout_config_utility.count(cfg_name))
                readout_config_utility[cfg_name] = readout_config_struct(cfg_name, {});

            auto *devices_array = entry_tbl->get("devices")->as_array();
            if (!devices_array)
                continue;

            for (auto &dev_node : *devices_array)
            {
                auto *dev_tbl = dev_node.as_table();
                if (!dev_tbl)
                    continue;

                auto *id_node = dev_tbl->get("id");
                if (!id_node)
                    continue;
                uint16_t device = static_cast<uint16_t>(id_node->value_or(0));

                // Expand chips: "*" wildcard or explicit integer array
                std::vector<uint16_t> requested_chips;
                auto *chips_node = dev_tbl->get("chips");
                if (!chips_node)
                    continue;

                if (auto chips_str = chips_node->value<std::string>())
                {
                    // Wildcard: all 8 chips
                    if (*chips_str == "*")
                        for (uint16_t c = 0; c < 8; ++c)
                            requested_chips.push_back(c);
                    else
                        mist::logger::warning(Form("(readout_config_reader) Unknown chips string token '%s' for device %d", chips_str->c_str(), device));
                }
                else if (auto *chips_array = chips_node->as_array())
                {
                    for (auto &c : *chips_array)
                        if (auto cv = c.value<int64_t>())
                            requested_chips.push_back(static_cast<uint16_t>(*cv));
                }

                // Core tag conflict check (same logic as before)
                bool is_special = (lightdata_core_tags.count(cfg_name) > 0);
                std::vector<uint16_t> valid_chips;

                if (is_special)
                {
                    for (uint16_t chip : requested_chips)
                    {
                        bool conflict_found = false;
                        auto conflicting = find_by_device_and_chip(readout_config_utility, device, chip);
                        for (auto &conflict_name : conflicting)
                        {
                            if (lightdata_core_tags.count(conflict_name))
                            {
                                mist::logger::error(Form("(readout_config_reader) Conflict: device %d chip %d already assigned to core tag '%s', cannot assign to '%s'",
                                                       device, chip, conflict_name.c_str(), cfg_name.c_str()));
                                conflict_found = true;
                                break;
                            }
                        }
                        if (!conflict_found)
                            valid_chips.push_back(chip);
                    }
                }
                else
                    valid_chips = requested_chips;

                for (auto chip : valid_chips)
                    readout_config_utility[cfg_name].add_device_chip(device, chip);
            }
        }
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(Form("(readout_config_reader) Failed to parse TOML config '%s': %s",
                                 config_file.c_str(), std::string(err.description()).c_str()));
        return readout_config;
    }

    for (auto &[name, cfg] : readout_config_utility)
        readout_config.push_back(cfg);

    return readout_config;
}

std::unordered_map<std::string, run_info_struct> run_info::run_info_database = {};
std::unordered_map<std::string, std::vector<std::string>> run_info::run_list_database = {};