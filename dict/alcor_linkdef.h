#pragma once

#ifdef __ROOTCLING__

#include "util/global_index.h"
#pragma link C++ class GlobalIndex + ;
#pragma link C++ class std::vector<GlobalIndex> + ;

#include "alcor_data.h"
#pragma link C++ struct AlcorDataStruct + ;
#pragma link C++ class std::vector<AlcorDataStruct> + ;

#include "alcor_finedata.h"
#pragma link C++ class AlcorFinedata + ;
#pragma link C++ struct AlcorFinedataStruct + ;
#pragma link C++ class std::vector<AlcorFinedataStruct> + ;

#include "alcor_lightdata.h"
#pragma link C++ struct TriggerEvent + ;
#pragma link C++ class AlcorLightdata + ;
#pragma link C++ struct AlcorLightdataStruct + ;
#pragma link C++ class std::vector<TriggerEvent> + ;
#pragma link C++ class std::vector<AlcorLightdataStruct> + ;

#include "alcor_recodata.h"
#pragma link C++ class AlcorRecodata + ;
#pragma link C++ class std::vector<AlcorRecodataStruct> + ;

#include "alcor_recotrackdata.h"
#pragma link C++ struct AlcorRecotrackdataStruct + ;
#pragma link C++ class AlcorRecotrackdata + ;
#pragma link C++ class std::vector<AlcorRecotrackdataStruct> + ;

#include "alcor_spilldata.h"
#pragma link C++ struct DataMaskStruct + ;
#pragma link C++ class AlcorSpilldata + ;
#pragma link C++ struct AlcorSpilldataStruct + ;
#pragma link C++ class std::vector<DataMaskStruct> + ;
#pragma link C++ class std::vector<uint32_t> + ;
#pragma link C++ class std::unordered_map<uint8_t, uint32_t> + ;
#pragma link C++ class std::unordered_map<uint32_t, AlcorLightdataStruct> + ;

#include "util/config_reader.h"
#pragma link C++ class RunList + ;
#pragma link C++ class RunInfo + ;

#include "mapping.h"
#pragma link C++ class Mapping + ;

#include "alcor_data_streamer.h"
#include "alcor_data_streamer.h"
#include "writers/lightdata.h"
#include "writers/recodata.h"
#include "parallel_streaming_framer.h"
#include "triggers.h"
#include "utility.h"

#endif
