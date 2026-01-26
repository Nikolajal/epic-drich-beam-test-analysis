#pragma once

#include <vector>
#include "../lib_v2/alcor_spilldata.h"
#include "../lib_v2/mapping.h"
#include "../lib_v2/utility.h"
#include "../lib_v2/streaming_framer.h"
#include "../lib_v2/alcor_recodata.h"
#include "lightdata_writer.C"
#include <filesystem>

gSystem->Load("alcor_recodata_h.so");

void simple_test()
{
  alcor_spilldata spilldata;
  spilldata.read_calib_from_file("alcor_fine_calibration_doctored.txt");
  spilldata.write_calib_to_file("alcor_fine_calibration_doctored_result.txt");
  

}