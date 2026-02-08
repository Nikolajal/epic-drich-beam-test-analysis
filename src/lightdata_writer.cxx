#include "parallel_streaming_framer.h"
#include "streaming_framer.h"
#include "lightdata_writer.h"

//  TODO: make the out file name/path custom

std::vector<std::string> list_of_devices = {
    "rdo-192",
    //"rdo-193",
    "rdo-194",
    "rdo-195",
    "rdo-196",
    "rdo-197",
    "rdo-198",
    "rdo-199",
    "kc705-200"
};

void lightdata_writer(
    const std::string &data_repository,
    const std::string &run_name,
    int max_spill)
{
  //  Do not make ownership of histograms to current directory
  TH1::AddDirectory(false);

  //  Input files
  std::vector<std::string> filenames;
  for (auto device : list_of_devices)
  {
    for (auto i_fifo = 0; i_fifo < (device == "kc705-200" ? 12 : 32); ++i_fifo)
      filenames.push_back(data_repository + "/" + run_name + "/" + device + "/decoded/alcdaq.fifo_" + std::to_string(i_fifo) + ".root");
    filenames.push_back(data_repository + "/" + run_name + "/" + device + "/decoded/alcdaq.fifo_99.root");
  }

  //  Create streaming framer
  parallel_streaming_framer framer(filenames, "conf/trigger_setup.txt", "conf/readout_config.txt");
  // streaming_framer framer(filenames, "conf/trigger_setup.txt", "conf/readout_config.txt");

  //  Prepare output tree
  TFile *outfile = TFile::Open((data_repository + "/" + run_name + "/lightdata.root").c_str(), "RECREATE");
  auto &spilldata = framer.get_spilldata_link();
  TTree *lightdata_tree = new TTree("lightdata", "Lightdata tree");
  spilldata.write_to_tree(lightdata_tree);

  //  QA Plots
  TH2F *h_timing_hit_map = new TH2F("h_timing_hit_map", ";channels on chip 0; channels on chip 1", 33, 0, 33, 33, 0, 33);
  TH1F *h_timing_ref_per_frame = new TH1F("h_timing_ref_per_frame", ";frame ID;Timing trigger", 2000, 0, 2000000);
  TH1F *h_timing_ref_delta = new TH1F("h_timing_ref_delta", ";timing chip 0 - timing chip 1", 200, -10, 10);
  TH1F *h_timing_ref_delta_sel = new TH1F("h_timing_ref_delta_sel", ";timing chip 0 - timing chip 1", 200, -10, 10);
  TH1F *h_timing_res = new TH1F("h_timing_res", "", 2000, 0, 2000000);
  TH1F *h_delta_time_trigger_0_timing = new TH1F("h_delta_time_trigger_0_timing", ";#Delta t (ns)", 100, -50, 50);

  //  Loop over spills
  int n_spills = 0, n_frames = 0;
  if (max_spill != 1000)
    std::cout << "[INFO] Requested to stop at spill : " << max_spill << std::endl;
  for (int ispill = 0; ispill < max_spill && framer.next_spill(); ++ispill)
  {
    std::cout << "\33[2K\r[INFO] Spill " << ispill << std::flush;
    for (auto &[frame_id, current_lightdata_struct] : spilldata.get_frame_link())
    {
      //  Build current event lightdata
      alcor_lightdata current_lightdata(current_lightdata_struct);

      //  Trigger hits
      auto &triggers_in_frame = current_lightdata.get_triggers_link();

      //  Fill timing trigger information
      auto &timing_hits = current_lightdata.get_timing_hits_link();
      int timing_hit_chip_0 = 0;
      int timing_hit_chip_1 = 0;
      int all_timing_hit_chip_0 = 0;
      int all_timing_hit_chip_1 = 0;
      float timing_ref_chip_0 = 0;
      float timing_ref_chip_1 = 0;
      std::map<int, bool> map_timing_hit_chip_0;
      std::map<int, bool> map_timing_hit_chip_1;
      for (auto current_timing_hit_struct : timing_hits)
      {
        alcor_finedata current_timing_hit(current_timing_hit_struct);
        //  TODO: fix, needs to follow the configuration file
        if (current_timing_hit.get_chip() == 0)
        {
          all_timing_hit_chip_0++;
          if (!map_timing_hit_chip_0[current_timing_hit.get_global_index()])
          {
            timing_hit_chip_0++;
            timing_ref_chip_0 += (current_timing_hit.get_coarse() - current_timing_hit.get_phase());
            map_timing_hit_chip_0[current_timing_hit.get_global_index()] = true;
          }
        }
        if (current_timing_hit.get_chip() == 2)
        {
          all_timing_hit_chip_1++;
          if (!map_timing_hit_chip_1[current_timing_hit.get_global_index()])
          {
            timing_hit_chip_1++;
            timing_ref_chip_1 += (current_timing_hit.get_coarse() - current_timing_hit.get_phase());
            map_timing_hit_chip_1[current_timing_hit.get_global_index()] = true;
          }
        }
      }
      if (timing_hit_chip_0 && timing_hit_chip_1)
        h_timing_hit_map->Fill(timing_hit_chip_0, timing_hit_chip_1);
      auto ref_timing = (timing_ref_chip_1 / timing_hit_chip_1 + timing_ref_chip_0 / timing_hit_chip_0) / 2.;
      auto delta_timing = (timing_ref_chip_1 / timing_hit_chip_1 - timing_ref_chip_0 / timing_hit_chip_0);
      //  TODO: fix, needs to be modular
      auto timing_available = ((timing_hit_chip_0 == 32) && (timing_hit_chip_1 == 31)) && (fabs(delta_timing + 0.15) < 3 * 0.180);
      if (timing_available)
      {
        if (all_timing_hit_chip_0 != 32 || all_timing_hit_chip_1 != 31)
          h_timing_ref_delta_sel->Fill(delta_timing);
        h_timing_ref_delta->Fill(delta_timing);

        //  TODO: understand why thee is a difference with the lightdata result
        spilldata.add_trigger_to_frame(frame_id, {static_cast<uint8_t>(_TRIGGER_TIMING_), static_cast<uint16_t>(_FRAME_SIZE_ / 2), static_cast<float>(ref_timing * _ALCOR_CC_TO_NS_)});
        h_timing_ref_per_frame->Fill(frame_id);
        for (auto current_trigger_hit_struct : triggers_in_frame)
        {
          //  TODO automatise to loop over all triggers
          if (current_trigger_hit_struct.index != 0)
            continue;
          h_delta_time_trigger_0_timing->Fill((ref_timing - current_trigger_hit_struct.coarse) * _ALCOR_CC_TO_NS_);
        }
      }

      if (!spilldata.has_trigger(frame_id))
        spilldata.do_not_write_frame(frame_id);
    }

    outfile->cd();
    spilldata.prepare_tree_fill();
    lightdata_tree->Fill();
    outfile->Flush();
  }
  std::cout << "\r[INFO] Finished spills loop, writing to file" << std::endl;

  //  QA plots
  outfile->cd();
  lightdata_tree->Write();
  h_timing_hit_map->Write();
  h_timing_ref_per_frame->Write();
  h_delta_time_trigger_0_timing->Write();
  h_timing_ref_delta->Write();
  h_timing_ref_delta_sel->Write();
  // auto QA_plots_map = framer.get_QA_plots();
  // for (auto [name, hist] : QA_plots_map)
  //   hist->Write(name.c_str());
  outfile->Close();
}
