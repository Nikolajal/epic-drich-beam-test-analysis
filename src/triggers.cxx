#include "triggers.h"
#include <fstream>
#include <sstream>
#include <iostream>

// Constructors
trigger_struct::trigger_struct(uint8_t idx) : index(idx), coarse(0), fine_time(0.) {}
trigger_struct::trigger_struct(uint8_t idx, uint16_t crs) : index(idx), coarse(crs), fine_time(0.) {}
trigger_struct::trigger_struct(uint8_t idx, uint16_t crs, float fine) : index(idx), coarse(crs), fine_time(fine) {}

trigger_config_struct::trigger_config_struct(const std::string& _name, uint16_t _index, uint16_t _delay, uint16_t _device)
    : name(_name), index(_index), delay(_delay), device(_device) {}

// Function definition
 std::vector<trigger_config_struct> trigger_conf_reader(const std::string& config_file)
{
    std::vector<trigger_config_struct> triggers;

    // Reading configuration file
    std::ifstream infile(config_file);
    if (!infile.is_open())
        return triggers;

    std::cout << "[INFO] Provided trigger file: " << config_file << std::endl;

    std::string line;
    while (std::getline(infile, line))
    {
        // Skip empty lines or comments
        if (line.empty() || line[0] == '#')
            continue;

        std::istringstream iss(line);
        trigger_config_struct input_config;
        if (iss >> input_config.name >> input_config.index >> input_config.device >> input_config.delay)
            triggers.push_back(input_config);
    }

    std::cout << "[INFO] Successfully logged triggers: " << std::endl;
    for (auto& current_trigger : triggers)
        std::cout << "[INFO] " << current_trigger.name << " set to index " << current_trigger.index
                  << " from device " << current_trigger.device << " with delay " << current_trigger.delay << std::endl;

    infile.close();
    return triggers;
}
