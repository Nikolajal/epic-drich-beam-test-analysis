#pragma once

#include <fstream>
#include <sstream>

enum trigger_number
{
  _TRIGGER_FIRST_FRAMES_ = 100,
  _TRIGGER_TIMING_ = 101
};

struct trigger_struct
{
  uint8_t index;
  uint16_t coarse;
  float fine_time;

  trigger_struct() = default;
  trigger_struct(uint8_t idx)
      : index(idx), coarse(0), fine_time(0.) {}
  trigger_struct(uint8_t idx, uint16_t crs)
      : index(idx), coarse(crs), fine_time(0.) {}
  trigger_struct(uint8_t idx, uint16_t crs, float fine)
      : index(idx), coarse(crs), fine_time(fine) {}
};

struct trigger_config_struct
{
  std::string name;
  uint16_t index;
  uint16_t delay;
  uint16_t device;
  trigger_config_struct() = default;
  trigger_config_struct(std::string _name, uint16_t _index, uint16_t _delay, uint16_t _device)
      : name(_name), delay(_delay), index(_index), device(_device) {}
};

std::vector<trigger_config_struct> trigger_conf_reader(std::string config_file = "Data/test.txt")
{
  std::vector<trigger_config_struct> triggers;

  //  Reading configuration file
  std::ifstream infile(config_file);
  if (!infile.is_open())
    return triggers;

  std::cout << "[INFO] Provided trigger file: " << config_file << endl;

  //  Reading from file
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

  std::cout << "[INFO] Succesfully logged triggers: " << endl;
  for (auto current_trigger : triggers)
    std::cout << "[INFO] " << current_trigger.name << " set to index " << current_trigger.index << " from device " << current_trigger.device << " with delay " << current_trigger.delay << endl;

  infile.close();
  return triggers;
}