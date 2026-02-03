#ifdef __ROOTCLING__

#include "alcor_data.h"
#pragma link C++ struct alcor_data_struct + ;
#pragma link C++ class std::vector<alcor_data_struct> + ;

#include "alcor_finedata.h"
#pragma link C++ struct alcor_finedata_struct + ;
#pragma link C++ class std::vector<alcor_finedata_struct> + ;

#include "alcor_lightdata.h"
#pragma link C++ struct trigger_struct + ;
#pragma link C++ struct alcor_lightdata_struct + ;
#pragma link C++ class std::vector<trigger_struct> + ;
#pragma link C++ class std::vector<alcor_lightdata_struct> + ;

#include "alcor_recodata.h"
#pragma link C++ struct alcor_recodata_struct + ;
#pragma link C++ class std::vector<alcor_recodata_struct> + ;

#include "alcor_spilldata.h"
#pragma link C++ struct data_mask_struct + ;
#pragma link C++ struct alcor_spilldata_struct + ;
#pragma link C++ class std::vector<data_mask_struct> + ;
#pragma link C++ class std::vector<uint32_t> + ;
#pragma link C++ class std::unordered_map<uint8_t, uint32_t> + ;
#pragma link C++ class std::unordered_map<uint32_t, alcor_lightdata_struct> + ;

#endif
