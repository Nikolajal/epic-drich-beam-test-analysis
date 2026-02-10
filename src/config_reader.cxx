#include "config_reader.h"

//  TODO: use TOML files
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

    auto expand_chips = [](const std::string &token) -> std::vector<uint16_t>
    {
        std::vector<uint16_t> chips;
        if (token == "*")
        {
            chips.reserve(8);
            for (uint16_t c = 0; c < 8; ++c)
                chips.push_back(c);
        }
        else
            chips.push_back(static_cast<uint16_t>(std::stoi(token)));
        return chips;
    };

    std::ifstream infile(config_file);
    if (!infile.is_open())
        return readout_config;

    std::cout << "[INFO] Provided configuration file: " << config_file << std::endl;

    std::string line;
    while (std::getline(infile, line))
    {
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream iss(line);
        std::string name;
        uint16_t current_device;
        std::string chip_token;
        if (iss >> name >> current_device >> chip_token)
        {
            std::vector<uint16_t> valid_chips;
            auto requested_chips = expand_chips(chip_token);
            if (requested_chips.empty())
            {
                std::cerr << "[ERROR] Invalid chip token '" << chip_token << "' on line: " << line << std::endl;
                continue;
            }

            bool is_special = (lightdata_core_tags.count(name) > 0);
            if (is_special)
            {
                for (uint16_t chip : requested_chips)
                {
                    bool conflict_found = false;
                    auto list_of_names_of_target_chip = find_by_device_and_chip(readout_config_utility, current_device, chip);
                    for (auto current_name : list_of_names_of_target_chip)
                        if (lightdata_core_tags.count(current_name))
                        {
                            std::cerr << "[ERROR] Conflict: trying to assign device " << current_device
                                      << " chip " << chip << " to '" << name
                                      << "' but it is already assigned to another core tag '" << current_name << "'" << std::endl;
                            conflict_found = true;
                            break;
                        }
                    if (!conflict_found)
                        valid_chips.push_back(chip);
                }
            }
            else
                valid_chips = requested_chips;

            if (!readout_config_utility.count(name))
                readout_config_utility[name] = readout_config_struct(name, {});

            for (auto chip : valid_chips)
                readout_config_utility[name].add_device_chip(current_device, chip);
        }
    }

    for (auto [name, current_config] : readout_config_utility)
        readout_config.push_back(current_config);

    infile.close();
    return readout_config;
}
