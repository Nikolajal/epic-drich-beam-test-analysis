#pragma once

#ifdef __ROOTCLING__

#include "alcor_data.h"
#pragma link C++ struct alcor_data_struct + ;
#pragma link C++ class std::vector<alcor_data_struct> + ;

#include "alcor_finedata.h"
#pragma link C++ class alcor_finedata + ;
#pragma link C++ struct alcor_finedata_struct + ;
#pragma link C++ class std::vector<alcor_finedata_struct> + ;

#include "alcor_lightdata.h"
#pragma link C++ struct trigger_event + ;
#pragma link C++ class alcor_lightdata + ;
#pragma link C++ struct alcor_lightdata_struct + ;
#pragma link C++ class std::vector<trigger_event> + ;
#pragma link C++ class std::vector<alcor_lightdata_struct> + ;

#include "alcor_recodata.h"
#pragma link C++ class alcor_recodata + ;
#pragma link C++ class std::vector<alcor_recodata_struct> + ;

#include "alcor_recotrackdata.h"
#pragma link C++ struct alcor_recotrackdata_struct + ;
#pragma link C++ class alcor_recotrackdata + ;
#pragma link C++ class std::vector<alcor_recotrackdata_struct> + ;

#include "alcor_spilldata.h"
#pragma link C++ struct data_mask_struct + ;
#pragma link C++ class alcor_spilldata + ;
#pragma link C++ struct alcor_spilldata_struct + ;
#pragma link C++ class std::vector<data_mask_struct> + ;
#pragma link C++ class std::vector<uint32_t> + ;
#pragma link C++ class std::unordered_map<uint8_t, uint32_t> + ;
#pragma link C++ class std::unordered_map<uint32_t, alcor_lightdata_struct> + ;

#include "config_reader.h"
#pragma link C++ class run_list + ;
#pragma link C++ class run_info + ;

#include "mapping.h"
#pragma link C++ class mapping + ;

#include "alcor_data_streamer.h"
#include "alcor_data_streamer.h"
#include "lightdata_writer.h"
#include "recodata_writer.h"
#include "parallel_streaming_framer.h"
#include "triggers.h"
#include "utility.h"

#endif
