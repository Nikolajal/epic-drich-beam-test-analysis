#include "../lib_loader.h"
#include <mist/mist.h>

/**
 * @file dark_count_rate.cpp
 * @brief Calculate dark count rate (DCR)
 * @details
 *
 * @author Nicola Rubini
 */

void dark_count_rate(std::string data_repository = "/Users/nrubini/Analysis/ePIC/test-beam-rec/Data/", std::string run_name = "20251111-164951", int max_frames = 10000000)
{
  //  Input files
  std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";

  //  Load recodata, return if not available
  TFile *input_file_recodata = new TFile(input_filename_recodata.c_str());
  if (!input_file_recodata || input_file_recodata->IsZombie())
  {
    std::cerr << "[WARNING] Could not find recodata, making it" << std::endl;
    return;
  }

  //  Link recodata tree locally
  TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
  alcor_recodata *recodata = new alcor_recodata();
  if (!recodata->link_to_tree(recodata_tree))
    return;

  //  Get number of frames, limited to maximum requested frames
  auto n_frames = recodata_tree->GetEntries();
  auto all_frames = min((int)n_frames, (int)max_frames);
  auto used_frames = 0;

  //  Result histograms
  TH1F *h_dcr = new TH1F("h_dcr", "Dark Count Rate;DCR [kHz];Entries", 40, 0, 20);
  TProfile *h_dcr_per_channel = new TProfile("h_dcr_per_channel", ";channel;DCR [kHz];", 2048, 0, 2048);
  TH1F *h_average_dcr = new TH1F("h_average_dcr", "1350;DCR [kHz];Entries", 50, 0, 10);
  TH1F *h_average_dcr_2 = new TH1F("h_average_dcr_2", "1375;DCR [kHz];Entries", 50, 0, 10);

  //  Keep track of active sensors
  std::set<uint32_t> active_sensors;
  std::unordered_map<uint32_t, uint16_t> active_sensors_count;

  //  Loop over frames
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current frame
    recodata_tree->GetEntry(i_frame);

    //  Takes note of spill evolution
    if (recodata->is_start_of_spill())
    {
      //  Keep track of current available sensors
      active_sensors.clear();
      active_sensors_count.clear();
      for (auto hit = 0; hit < recodata->get_recodata().size(); hit++)
        active_sensors.insert(recodata->get_global_channel_index(hit));

      //  This event is not of physical interest, skip it
      continue;
    }

    //  First frames is a software trigger designed to provide the first 2500 frames in a spill.
    //  These frames are collected before the particles in the beam reach the radiator(s),
    //  so they can be used to monitor the dark count rate (DCR) of the detector.
    if (recodata->is_first_frames())
    {
      //  Counting the frames actually used in the analysis
      used_frames++;

      //  Channel by channel hit counting
      for (const auto &global_channel_index : active_sensors)
        active_sensors_count[global_channel_index] = 0;
      for (auto hit = 0; hit < recodata->get_recodata().size(); hit++)
        active_sensors_count[recodata->get_global_channel_index(hit)]++;

      //  Fill the DCR histogram
      for (auto &[global_channel_index, count] : active_sensors_count)
        h_dcr_per_channel->Fill(global_channel_index, count);

      h_dcr->Fill(recodata->get_recodata().size() * 1. / (_FRAME_LENGTH_NS_ * 1.e-6 * active_sensors.size()));
    }
  }

  //  Normalise DCR to the number of used frames
  h_dcr_per_channel->Scale(1. / (_FRAME_LENGTH_NS_ * 1.e-6));
  h_dcr->Scale(1. / used_frames);

  for (auto x_bin = 1; x_bin <= h_dcr_per_channel->GetNbinsX(); ++x_bin)
    if (h_dcr_per_channel->GetBinContent(x_bin) > 0.1)
    {
      if (x_bin < 1025)
      {
        h_average_dcr->Fill(h_dcr_per_channel->GetBinContent(x_bin));
      }
      else
      {
        h_average_dcr_2->Fill(h_dcr_per_channel->GetBinContent(x_bin));
      }
    }

  gStyle->SetOptStat(0);
  TCanvas *c_dcr = new TCanvas("c_dcr", "Dark Count Rate", 800, 600);
  h_dcr->Draw();
  TCanvas *c_test2 = new TCanvas("c_test2", "Test Histogram", 800, 600);
  h_dcr_per_channel->Draw();
  TCanvas *c_test3 = new TCanvas("c_test3", "Average DCR Histogram", 800, 600);
  h_average_dcr->SetLineWidth(2);
  h_average_dcr->SetLineColor(kRed);
  h_average_dcr->Draw();
  h_average_dcr_2->SetLineWidth(2);
  h_average_dcr_2->SetLineColor(kBlue);
  h_average_dcr_2->Draw("SAME");
  gPad->BuildLegend();
}
