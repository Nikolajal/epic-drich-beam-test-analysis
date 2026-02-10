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
  static std::map<int, std::vector<int>> matrix_to_do_channel;
  //  Loaded from calib file
  static std::map<int, bool> pdu_rotation;
  static std::map<int, std::array<float, 2>> pdu_xy_position;
  static std::map<std::array<int, 2>, std::array<int, 2>> device_chip_to_pdu_matrix;
  // constexpr static std::array<float, 2> position_offset = {1.7, 1.7}; // the centre of the SiPM in the bottom-left corner (A1)
  // constexpr static std::array<float, 2> position_pitch = {3.2, 3.2};  // the distance between the SiPM cetres
};

// #define TESTBEAM2023
// #define TESTBEAM2024
#define TESTBEAM2025

#if defined TESTBEAM2023
// maps device and chip to pdu and matrix, generated from /etc/drich/drich_readout.conf
std::map<std::array<int, 2>, std::array<int, 2>> pdu_matrix_map = {
    {{195, 0}, {1, 3}},
    {{195, 1}, {1, 3}},
    {{195, 2}, {1, 4}},
    {{195, 3}, {1, 4}},
    {{193, 0}, {2, 1}},
    {{193, 1}, {2, 1}},
    {{193, 2}, {2, 2}},
    {{193, 3}, {2, 2}},
    {{192, 0}, {2, 3}},
    {{192, 1}, {2, 3}},
    {{192, 2}, {2, 4}},
    {{192, 3}, {2, 4}},
    {{192, 4}, {3, 1}},
    {{192, 5}, {3, 1}},
    {{193, 4}, {3, 2}},
    {{193, 5}, {3, 2}},
    {{194, 4}, {3, 3}},
    {{194, 5}, {3, 3}},
    {{194, 1}, {1, 1}},
    {{194, 2}, {1, 2}},
    {{195, 4}, {3, 4}},
    {{195, 5}, {3, 4}},
    {{196, 0}, {4, 1}},
    {{196, 1}, {4, 1}},
    {{196, 2}, {4, 2}},
    {{196, 3}, {4, 2}},
    {{197, 0}, {4, 3}},
    {{197, 1}, {4, 3}},
    {{197, 2}, {4, 4}},
    {{197, 3}, {4, 4}},
    {{196, 4}, {5, 2}},
    {{196, 5}, {5, 2}},
    {{197, 4}, {6, 2}},
    {{197, 5}, {6, 2}},
    {{198, 0}, {7, 4}},
    {{198, 1}, {7, 4}},
    {{198, 2}, {8, 4}},
    {{198, 3}, {8, 4}}};
#elif defined TESTBEAM2024
// automatically generated
// cat /etc/drich/drich_readout.conf | grep -v "^#" | awk {'print "{ { " substr($4,7,3) " , " $5 " } ,  {" $1 " , " $3 " } } ," '}
// cat /etc/drich/drich_readout.conf | grep -v "^#" | awk {'print "{ { " substr($4,7,3) " , " $6 " } ,  {" $1 " , " $3 " } } ," '}
std::map<std::array<int, 2>, std::array<int, 2>> pdu_matrix_map = {
    {{193, 0}, {1, 1}},
    {{193, 2}, {1, 2}},
    {{194, 0}, {1, 3}},
    {{194, 2}, {1, 4}},
    {{195, 0}, {2, 1}},
    {{195, 2}, {2, 2}},
    {{196, 0}, {2, 3}},
    {{196, 2}, {2, 4}},
    {{197, 0}, {3, 1}},
    {{197, 2}, {3, 2}},
    {{198, 0}, {3, 3}},
    {{198, 2}, {3, 4}},
    {{193, 4}, {4, 1}},
    {{194, 4}, {4, 2}},
    {{195, 4}, {4, 3}},
    {{196, 4}, {4, 4}},
    {{197, 4}, {5, 1}},
    {{198, 4}, {5, 2}},
    {{199, 0}, {5, 3}},
    {{199, 2}, {5, 4}},
    {{201, 0}, {6, 1}},
    {{201, 2}, {6, 2}},
    {{202, 0}, {6, 3}},
    {{202, 2}, {6, 4}},
    {{203, 0}, {7, 1}},
    {{203, 2}, {7, 2}},
    {{192, 0}, {7, 3}},
    {{192, 2}, {7, 4}},
    {{201, 4}, {8, 1}},
    {{202, 4}, {8, 2}},
    {{203, 4}, {8, 3}},
    {{192, 4}, {8, 4}},
    {{193, 1}, {1, 1}},
    {{193, 3}, {1, 2}},
    {{194, 1}, {1, 3}},
    {{194, 3}, {1, 4}},
    {{195, 1}, {2, 1}},
    {{195, 3}, {2, 2}},
    {{196, 1}, {2, 3}},
    {{196, 3}, {2, 4}},
    {{197, 1}, {3, 1}},
    {{197, 3}, {3, 2}},
    {{198, 1}, {3, 3}},
    {{198, 3}, {3, 4}},
    {{193, 5}, {4, 1}},
    {{194, 5}, {4, 2}},
    {{195, 5}, {4, 3}},
    {{196, 5}, {4, 4}},
    {{197, 5}, {5, 1}},
    {{198, 5}, {5, 2}},
    {{199, 1}, {5, 3}},
    {{199, 3}, {5, 4}},
    {{201, 1}, {6, 1}},
    {{201, 3}, {6, 2}},
    {{202, 1}, {6, 3}},
    {{202, 3}, {6, 4}},
    {{203, 1}, {7, 1}},
    {{203, 3}, {7, 2}},
    {{192, 1}, {7, 3}},
    {{192, 3}, {7, 4}},
    {{201, 5}, {8, 1}},
    {{202, 5}, {8, 2}},
    {{203, 5}, {8, 3}},
    {{192, 5}, {8, 4}}};
#elif defined TESTBEAM2025
inline std::map<std::array<int, 2>, std::array<int, 2>> pdu_matrix_map =
    {{{192, 0}, {1, 1}},
     {{192, 1}, {1, 1}},
     {{192, 2}, {1, 2}},
     {{192, 3}, {1, 2}},
     {{192, 4}, {1, 3}},
     {{192, 5}, {1, 3}},
     {{192, 6}, {1, 4}},
     {{192, 7}, {1, 4}},
     {{194, 0}, {3, 1}},
     {{194, 1}, {3, 1}},
     {{194, 2}, {3, 2}},
     {{194, 3}, {3, 2}},
     {{194, 4}, {3, 3}},
     {{194, 5}, {3, 3}},
     {{194, 6}, {3, 4}},
     {{194, 7}, {3, 4}},
     {{195, 0}, {4, 1}},
     {{195, 1}, {4, 1}},
     {{195, 2}, {4, 2}},
     {{195, 3}, {4, 2}},
     {{195, 4}, {4, 3}},
     {{195, 5}, {4, 3}},
     {{195, 6}, {4, 4}},
     {{195, 7}, {4, 4}},
     {{196, 0}, {5, 1}},
     {{196, 1}, {5, 1}},
     {{196, 2}, {5, 2}},
     {{196, 3}, {5, 2}},
     {{196, 4}, {5, 3}},
     {{196, 5}, {5, 3}},
     {{196, 6}, {5, 4}},
     {{196, 7}, {5, 4}},
     {{197, 0}, {6, 1}},
     {{197, 1}, {6, 1}},
     {{197, 2}, {6, 2}},
     {{197, 3}, {6, 2}},
     {{197, 4}, {6, 3}},
     {{197, 5}, {6, 3}},
     {{197, 6}, {6, 4}},
     {{197, 7}, {6, 4}},
     {{198, 0}, {7, 1}},
     {{198, 1}, {7, 1}},
     {{198, 2}, {7, 2}},
     {{198, 3}, {7, 2}},
     {{198, 4}, {7, 3}},
     {{198, 5}, {7, 3}},
     {{198, 6}, {7, 4}},
     {{198, 7}, {7, 4}},
     {{199, 0}, {8, 1}},
     {{199, 1}, {8, 1}},
     {{199, 2}, {8, 2}},
     {{199, 3}, {8, 2}},
     {{199, 4}, {8, 3}},
     {{199, 5}, {8, 3}},
     {{199, 6}, {8, 4}},
     {{199, 7}, {8, 4}},
     {{200, 0}, {99, 1}},
     {{200, 2}, {99, 1}}};
#endif

/**
    the mapping is a map where the key is the matrix index (U1, U2, U3, U4) and the value is
    a vector of the detector-oriented SiPM index on the matrix for a given
    electronics-oriented channel index

    electronics-oriented channel index is defined as following in the ALCOR-dual boards

    eoch = pixel + 8 * column + 32 * chip

    where pixel = [0, 7] and column = [0, 3] are withing an ALCOR chip
    whereas chip = [0, 1] defines the chip on the ALCOR-dual board (0 = left, 1 = right chip)

    the detector-oriented channel index on the SiPM matrix is defined as follong

    doch = row + column * 8

    where row = [0, 7] and column = [0, 7] are defined starting from the bottom-left corner
**/

/**
   maps the readout configuration to the detector
   readout_map[device][chip] --> {pdu, matrix}
**/
