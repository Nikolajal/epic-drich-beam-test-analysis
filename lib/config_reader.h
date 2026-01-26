#pragma once

#include <fstream>
#include <sstream>
#include <set>

static const std::set<std::string> lightdata_core_tags = {
    "timing",
    "tracking",
    "cherenkov"};

struct readout_config_struct
{
  std::string name;
  std::map<uint16_t, std::vector<uint16_t>> device_chip;
  readout_config_struct() = default;
  readout_config_struct(std::string _name, std::map<uint16_t, std::vector<uint16_t>> _device_chip)
      : name(_name), device_chip(_device_chip) {}
  void add_device_chip(uint16_t device, uint16_t chip)
  {
    if (device_chip.find(device) == device_chip.end())
      device_chip[device] = std::vector<uint16_t>();
    device_chip[device].push_back(chip);
  }
  void add_device(uint16_t device)
  {
    if (device_chip.find(device) == device_chip.end())
      device_chip[device] = std::vector<uint16_t>();
    for (auto i_chip = 0; i_chip < 8; ++i_chip)
      device_chip[device].push_back(i_chip);
  }
};

class readout_config_list
{
public:
  std::vector<readout_config_struct> configs;

  readout_config_list() = default;

  explicit readout_config_list(std::vector<readout_config_struct> vec)
      : configs(std::move(vec)) {}

  // --- SEARCH METHODS ---

  // Find config by name (returns nullptr if not found)
  readout_config_struct *find_by_name(const std::string &name)
  {
    auto it = std::find_if(configs.begin(), configs.end(),
                           [&](const readout_config_struct &cfg)
                           {
                             return cfg.name == name;
                           });
    return (it != configs.end()) ? &(*it) : nullptr;
  }

  // Find first config containing a device
  readout_config_struct *find_by_device(uint16_t device)
  {
    auto it = std::find_if(configs.begin(), configs.end(),
                           [&](const readout_config_struct &cfg)
                           {
                             return cfg.device_chip.count(device) > 0;
                           });
    return (it != configs.end()) ? &(*it) : nullptr;
  }

  // Find all configs containing a device
  std::vector<readout_config_struct *> find_all_by_device(uint16_t device)
  {
    std::vector<readout_config_struct *> out;
    for (auto &cfg : configs)
      if (cfg.device_chip.count(device))
        out.push_back(&cfg);
    return out;
  }

  std::vector<std::string> find_by_device_and_chip(
      uint16_t device,
      uint16_t chip)
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

  // Convenience: check if name exists
  bool has_name(const std::string &name) const
  {
    return std::any_of(configs.begin(), configs.end(),
                       [&](const readout_config_struct &cfg)
                       { return cfg.name == name; });
  }

  //  provided config flags
  bool has_cherenkov() { return find_by_name("cherenkov"); };
  bool has_timing() { return find_by_name("timing"); };
  bool has_tracking() { return find_by_name("tracking"); };

  //  provided config flags
  // TODO std::vector<int> get_global_channels_by_tag
};

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

std::vector<readout_config_struct> readout_config_reader(std::string config_file = "conf/readout_config.txt")
{
  //  Result and utility map
  std::vector<readout_config_struct> readout_config;
  std::map<std::string, readout_config_struct> readout_config_utility;

  //  Lambda to expand chip list from string
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

  //  Reading configuration file
  std::ifstream infile(config_file);
  if (!infile.is_open())
    return readout_config;

  std::cout << "[INFO] Provided configuration file: " << config_file << endl;

  //  Reading from file
  std::string line;
  while (std::getline(infile, line))
  {
    // Skip empty lines or comments
    if (line.empty() || line[0] == '#')
      continue;

    std::istringstream iss(line);
    std::string name;
    uint16_t current_device;
    std::string chip_token;
    if (iss >> name >> current_device >> chip_token)
    {
      //  Validate chip token and expand to list
      std::vector<uint16_t> valid_chips;
      auto requested_chips = expand_chips(chip_token);
      if (requested_chips.empty())
      {
        std::cerr << "[ERROR] Invalid chip token '" << chip_token
                  << "' on line: " << line << std::endl;
        continue;
      }

      // check mutual-exclusion only when the current name is one of lightdata_core_tags
      bool is_special = (lightdata_core_tags.count(name) > 0);
      if (is_special)
      {
        //  Check for conflicts with other tags
        for (uint16_t chip : requested_chips)
        {
          bool conflict_found = false;
          auto list_of_names_of_target_chip = find_by_device_and_chip(readout_config_utility, current_device, chip);
          for (auto current_name : list_of_names_of_target_chip)
            if (!lightdata_core_tags.count(current_name))
              continue;
            else
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

      //  If the tag is not stored yet, store it
      if (!readout_config_utility.count(name))
        readout_config_utility[name] = readout_config_struct(name, {});

      //  Add device and chips
      for (auto chip : valid_chips)
        readout_config_utility[name].add_device_chip(current_device, chip);
    }
  }

  std::cout << "[INFO] Succesfully logged readout config: " << endl;
  for (auto [name, current_config] : readout_config_utility)
  {
    std::cout << "[" << name << "]" << " Listed devices & chips: " << endl;
    for (auto [device, chips] : current_config.device_chip)
    {
      std::cout << "  --- Device " << device << " Chips: ";
      for (auto chip : chips)
        std::cout << chip << " ";
      std::cout << endl;
    }
    readout_config.push_back(current_config);
  }

  infile.close();
  return readout_config;
}