#pragma once

#include "utility.h"
#include "alcor_finedata.h"
#include <toml++/toml.h>

//  TODO: Cache values to avoid re-computation
class mapping
{
public:
  //  Getters
  std::optional<int> get_do_channel(int matrix, int eo_channel) const;
  std::optional<std::array<int, 2>> get_pdu_matrix(int device, int chip) const;
  std::optional<std::array<float, 2>> get_position_from_pdu_column_row(int pdu, int column, int row) const;
  std::optional<std::array<float, 2>> get_position_from_pdu_matrix_eoch(int pdu, int matrix, int eo_channel) const;
  std::optional<std::array<float, 2>> get_position_from_device_chip_eoch(int device, int chip, int eo_channel) const;
  std::optional<std::array<float, 2>> get_position_from_finedata(alcor_finedata entry) const;
  std::optional<std::array<float, 2>> get_position_from_global_index(int entry) const;

  //  I/O
  void load_calib(std::string filename, bool verbose = false);

private:
  //  Static definition of matrix channels
  static std::map<int, std::vector<int>> matrix_to_do_channel;
  //  Loaded from calib file
  static std::map<int, bool> pdu_rotation;
  static std::map<int, std::array<float, 2>> pdu_xy_position;
  static std::map<std::array<int, 2>, std::array<int, 2>> device_chip_to_pdu_matrix;
};
