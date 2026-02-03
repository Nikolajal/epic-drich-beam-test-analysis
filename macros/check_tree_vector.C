#include <vector>
#include "../lib/alcor_spilldata.h"
#include "../lib/mapping.h"
#include "../lib/utility.h"
#include "../lib/alcor_recodata.h"

gSystem->Load("alcor_recodata_h.so");

void check_tree_vector(std::string run_name, int max_spill = 1000)
{
  //  Input files
  std::string input_filename = "./Data/" + run_name + "/lightdata.root";
  TFile *input_file = new TFile(input_filename.c_str());
  TTree *lightdata_tree = (TTree *)input_file->Get("lightdata");
  alcor_spilldata *spilldata = new alcor_spilldata();
  spilldata->link_to_tree(lightdata_tree);
  auto n_spills = lightdata_tree->GetEntries();
  auto all_spills = min((int)n_spills, (int)max_spill);

  //  Prepare output tree
  TFile *output_file = TFile::Open("check_tree_vector_output.root", "RECREATE");
  TTree *recodata_tree = new TTree("recodata", "Recodata tree");
  alcor_recodata recodata;
  recodata.write_to_tree(recodata_tree);

  //  Loop over spills
  for (int ispill = 0; ispill < all_spills; ++ispill)
  {
    std::cout << "[INFO] Spill " << ispill << std::endl;
    lightdata_tree->GetEntry(ispill);
    spilldata->get_entry();
    auto frames_in_spill = spilldata->get_frame_list_link();
    auto lanes_participating = spilldata->get_not_dead_participants();

    //  Loop over frames
    for (auto &current_lightdata_struct : frames_in_spill)
    {
      //  Get lightdata
      alcor_lightdata current_lightdata(current_lightdata_struct);
      
      //  Trigger
      auto frame_triggers = current_lightdata.get_triggers();
      for (auto current_trigger : frame_triggers)
      {
        //  Skip the unknown trigger tag
        if (current_trigger.index == 255)
        {
          h_unknown_trigger_devices->Fill(i_spill, current_trigger.coarse);
          continue;
        }

        //  Trimm trigger coincidences on edges
        trigger_edge_rejection[current_trigger.index] = false;
        if ((current_trigger.coarse < _FRAME_SIZE_ * 0.1) || (current_trigger.coarse > _FRAME_SIZE_ * 0.9))
          trigger_edge_rejection[current_trigger.index] = false;

        //  Save triggers in frame
        trigger_in_frame[current_trigger.index] = current_trigger.coarse * _ALCOR_CC_TO_NS_; // ns
       }

      auto cherenkov_hits = current_lightdata.get_cherenkov_hits_link();
      //  Loop over hits
      for (auto current_cherenkov_hit_struct : cherenkov_hits)
      {
        alcor_finedata current_cherenkov_hit(current_cherenkov_hit_struct);
        auto global_index = current_cherenkov_hit.get_global_index();
        auto current_geo = sipm4eic::get_geo(current_cherenkov_hit_struct);
        auto current_position = sipm4eic::get_position(current_geo);

        //  Fill recodata
        recodata.channel = global_index * 4 + current_cherenkov_hit.get_tdc();
        recodata.x = current_position[0];
        recodata.y = current_position[1];
        recodata.t = current_cherenkov_hit.get_coarse() * _ALCOR_CC_TO_NS_; // ns
        recodata.channel


        auto x_dist = current_position[0] + (_rnd_(_global_gen_) * 3.0 - 1.5);
        auto y_dist = current_position[1] + (_rnd_(_global_gen_) * 3.0 - 1.5);
        h_hitmap->Fill(x_dist, y_dist);
      }
    }
  }

  input_file->Close();
  output_file->Close();
}
