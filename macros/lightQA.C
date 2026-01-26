#include "../lib/lightio.h"
#include "../lib/data.h"
#include "../lib/utility.h"

//#define TESTBEAM2023
#define TESTBEAM2024

#ifdef TESTBEAM2023
int TRIGGER0_device = 192;
int TRIGGER0_offset = 112;
#elifdef TESTBEAM2024
int TRIGGER0_device = 193;
int TRIGGER0_offset = 0;
#endif

#ifdef TESTBEAM2023
int TIMING1_device = 207, TIMING1_chip = 4;
int TIMING2_device = 207, TIMING2_chip = 5;
#elifdef TESTBEAM2024
int TIMING1_device = 200, TIMING1_chip = 2;
int TIMING2_device = 200, TIMING2_chip = 4;
#else
int TIMING1_device = 200, TIMING1_chip = 5;
int TIMING2_device = 201, TIMING2_chip = 5;
#endif

int TRACKING1_device = 200, TRACKING1_chip = 3;
int TRACKING2_device = 200, TRACKING2_chip = 5;

const float min_ntiming = 32; // [s]

const float min_tspill = -0.1; // [s]
const float max_tspill = 0.7;  // [s]
const int max_nspill = 100;

const float min_tdelta = -25; // [clock]
const float max_tdelta = 25;  // [clock]

void lightQA(std::string input_file = "lightdata.root", std::string output_file = "out.root", std::string save_dir = "./images/")
{
  //  Define output objects
  //  === Trigger
  auto hTriggerHitsTimeInSpill = new TH1F("hTriggerHitsTimeInSpill", "TRIGGER readout;time (s);entries", 1000 * (max_tspill - min_tspill), min_tspill, max_tspill);
  auto hTriggerChannelInFrameIntegrated = new TH1F("hTriggerChannelInFrame", "TRIGGER readout;number of triggers;number of frames", 10, 0, 10);

  //  === Tracking
  auto hTrackingHitsTimeInSpill = new TH1F("hTrackingHitsTimeInSpill", "TRACKING readout;time (s);entries", 1000 * (max_tspill - min_tspill), min_tspill, max_tspill);
  auto hTrackingChannelInFrameIntegrated = new TH1F("hTrackingChannelInFrame", "TRACKING readout;number of channels;number of frames", 10, 0, 10);
  auto hTrackingChannelMap = new TH2F("hTrackingChannelMap", "TRACKING readout;number of channels (TRACKING 1);number of channels (TRACKING 2)", 5, 0, 5, 5, 0, 5);

  //  === Timing
  auto hTimingHitsTimeInSpill = new TH1F("hTimingHitsTimeInSpill", "TIMING readout;time (s);entries", 1000 * (max_tspill - min_tspill), min_tspill, max_tspill);
  auto hTimingChannelInFrameIntegrated = new TH1F("hTimingChannelInFrame", "TIMING readout;number of channels;number of frames", 80, 0, 80);
  auto hTimingChannelMap = new TH2F("hTimingChannelMap", "TIMING readout;number of channels (TIMING 1);number of channels (TIMING 2)", 40, 0, 40, 40, 0, 40);
  auto hTimingTimeResolution = new TH1F("hTimingTimeResolution", "TIMING readout;TIMING 1 - TIMING 2 (clock cycles);", max_tdelta - min_tdelta, min_tdelta, max_tdelta);

  //  === Cherenkov
  auto hCherenkovHitsTimeInSpill = new TH1F("hCherenkovHitsTimeInSpill", "CHERENKOV readout;time (s);entries", 1000 * (max_tspill - min_tspill), min_tspill, max_tspill);
  auto hCherenkovChannelInFrameIntegrated = new TH1F("hCherenkovChannelInFrame", "CHERENKOV readout;number of channels;number of frames", 200, 0, 200);

  //  === General
  std::map<std::array<int, 2>, TH1F *> hGenericCoincidenceMapwTrigger;
  std::map<std::array<int, 2>, TH1F *> hGenericCoincidenceMapwTiming;
  for (auto [device_id, enumerator] : devices_enum)
  {
    for (auto iChip = 0; iChip < 6; iChip++)
    {
      hGenericCoincidenceMapwTrigger[{enumerator, iChip}] = new TH1F(Form("hGenericCoincidenceMapTrigger_d%i_c%i", device_id, iChip), Form("Time coincidences (kc705-%i, chip-%i);hit - trigger time (clock cycles);entries", device_id, iChip), max_tdelta - min_tdelta, min_tdelta, max_tdelta);
      hGenericCoincidenceMapwTiming[{enumerator, iChip}] = new TH1F(Form("hGenericCoincidenceMapTiming_d%i_c%i", device_id, iChip), Form("Time coincidences (kc705-%i, chip-%i);hit - timing time (clock cycles);entries", device_id, iChip), max_tdelta - min_tdelta, min_tdelta, max_tdelta);
    }
  }

  cout << "[INFO] Starting the light QA: reading " << input_file << endl;
  sipm4eic::lightio *io = new sipm4eic::lightio();
  io->read_from_tree(input_file);
  cout << "[INFO] Finished reading from tree in file " << input_file << endl;

  //  Loop on spills
  auto full_frames = 0;
  auto current_spill = 0;
  while (io->next_spill())
  {
    current_spill++;
    cout << "[INFO] Start spill: " << current_spill << endl;

    //  Loop on frames
    auto current_frame = 0;
    while (io->next_frame())
    {
      //  === Frame info
      current_frame++;
      auto frame_id = io->frame[current_frame];
      //  === === Trigger
      bool trigger_available = true;
      auto reference_trigger = 0.;
      //  === === Timing
      bool timing_available = true;
      auto reference_timing = 0.;

      // === Trigger reference definiton
      auto trigger0_vector = io->get_trigger0_vector();
      trigger_available = trigger0_vector.size() > 0;
      hTriggerChannelInFrameIntegrated->Fill(trigger0_vector.size());
      if (trigger_available)
      {
        reference_trigger = trigger0_vector[0].time();
        hTriggerHitsTimeInSpill->Fill( (reference_trigger + 256 * frame_id) * sipm4eic::data::coarse_to_ns * 1.e-9);
      }

      // === Timing reference definition
      auto timing_vector = io->get_timing_vector();
      std::map<int, std::vector<float>> timing_first_channels_times, timing_second_channels_times;
      auto contributors_first_chip = 0;
      auto contributors_second_chip = 0;
      auto time_first_chip = 0.;
      auto time_second_chip = 0.;
      for (auto &timing : timing_vector)
      {
        auto timing_coarse = timing.coarse;
        auto timing_time = timing.time();
        auto timing_device = devices_enum[timing.device];
        auto timing_chip = timing.chip();
        auto timing_channel = timing.eoch();
        if (timing.device == TIMING1_device && timing.chip() == TIMING1_chip)
          timing_first_channels_times[timing_channel].push_back(timing_time);
        else if (timing.device == TIMING2_device && timing.chip() == TIMING2_chip)
          timing_second_channels_times[timing_channel].push_back(timing_time);
        hTimingHitsTimeInSpill->Fill( (timing_time + 256 * frame_id) * sipm4eic::data::coarse_to_ns * 1.e-9);
      }
      for (auto [timing_channel, timing_time] : timing_first_channels_times)
      {
        time_first_chip += timing_time[0];
        contributors_first_chip++;
      }
      for (auto [timing_channel, timing_time] : timing_second_channels_times)
      {
        time_second_chip += timing_time[0];
        contributors_second_chip++;
      }
      if (contributors_second_chip == 0 ||
          contributors_first_chip == 0 ||
          contributors_first_chip < min_ntiming ||
          contributors_second_chip < min_ntiming)
        timing_available = false;
      if (timing_available)
      {
        reference_timing = 0.5 * (time_first_chip / contributors_first_chip) + 0.5 * (time_second_chip / contributors_second_chip);
        hTimingTimeResolution->Fill((time_first_chip / contributors_first_chip) - (time_second_chip / contributors_second_chip));
      }
      hTimingChannelInFrameIntegrated->Fill(timing_first_channels_times.size() + timing_second_channels_times.size());
      hTimingChannelMap->Fill(contributors_first_chip, contributors_second_chip);

      // === Trigger
      //  ---

      // === Timing
      for (auto &timing : timing_vector)
      {
        auto timing_coarse = timing.coarse;
        auto timing_time = timing.time();
        auto timing_device = devices_enum[timing.device];
        auto timing_chip = timing.chip();
        auto timing_channel = timing.eoch();
        if (timing_available)
          hGenericCoincidenceMapwTiming[{timing_device, timing_chip}]->Fill(timing_time - reference_timing);
        if (trigger_available)
          hGenericCoincidenceMapwTrigger[{timing_device, timing_chip}]->Fill(timing_time - reference_trigger);
      }

      // === Tracking
      auto tracking_vector = io->get_tracking_vector();
      std::map<int, std::vector<float>> tracking_first_channels_times, tracking_second_channels_times;
      auto trk_contributors_first_chip = 0;
      auto trk_contributors_second_chip = 0;
      std::map<int, std::vector<float>> tracking_channels_times;
      for (auto &tracking : tracking_vector)
      {
        auto tracking_coarse = tracking.coarse;
        auto tracking_time = tracking.time();
        auto tracking_device = devices_enum[tracking.device];
        auto tracking_chip = tracking.chip();
        auto tracking_channel = tracking.eoch();
        if (tracking.device == TRACKING1_device && tracking.chip() == TRACKING1_chip)
          tracking_first_channels_times[tracking_channel].push_back(tracking_time);
        else if (tracking.device == TRACKING2_device && tracking.chip() == TRACKING2_chip)
          tracking_second_channels_times[tracking_channel].push_back(tracking_time);
        tracking_channels_times[tracking_channel].push_back(tracking_coarse);
        if (timing_available)
          hGenericCoincidenceMapwTiming[{tracking_device, tracking_chip}]->Fill(tracking_time - reference_timing);
        if (trigger_available)
          hGenericCoincidenceMapwTrigger[{tracking_device, tracking_chip}]->Fill(tracking_time - reference_trigger);
        hTrackingHitsTimeInSpill->Fill( (tracking_time + 256 * frame_id) * sipm4eic::data::coarse_to_ns * 1.e-9);
      }
      for (auto [tracking_channel, tracking_time] : tracking_first_channels_times)
      {
        trk_contributors_first_chip++;
      }
      for (auto [tracking_channel, tracking_time] : tracking_second_channels_times)
      {
        trk_contributors_second_chip++;
      }
      hTrackingChannelInFrameIntegrated->Fill(tracking_channels_times.size());
      hTrackingChannelMap->Fill(trk_contributors_first_chip, trk_contributors_second_chip);

      // === Cherenkov
      auto cherenkov_vector = io->get_cherenkov_vector();
      std::map<int, std::vector<float>> cherenkov_channels_times;
      for (auto &cherenkov : cherenkov_vector)
      {
        auto cherenkov_coarse = cherenkov.coarse;
        auto cherenkov_time = cherenkov.time();
        auto cherenkov_device = devices_enum[cherenkov.device];
        auto cherenkov_chip = cherenkov.chip();
        auto cherenkov_channel = cherenkov.eoch();
        cherenkov_channels_times[cherenkov_channel].push_back(cherenkov_coarse);
        if (timing_available)
          hGenericCoincidenceMapwTiming[{cherenkov_device, cherenkov_chip}]->Fill(cherenkov_time - reference_timing);
        if (trigger_available)
          hGenericCoincidenceMapwTrigger[{cherenkov_device, cherenkov_chip}]->Fill(cherenkov_time - reference_trigger);
        hCherenkovHitsTimeInSpill->Fill( (cherenkov_coarse + 256 * frame_id) * sipm4eic::data::coarse_to_ns * 1.e-9);
      }
      hCherenkovChannelInFrameIntegrated->Fill(cherenkov_channels_times.size());
    }
  }

  //  === Graphics
  gROOT->SetBatch();
  system(Form("mkdir -p %s/", save_dir.c_str()));

  //  gStyle->SetPalette(kInvertedDarkBodyRadiator);

  //  === === Trigger
  auto current_canvas = get_std_canvas();
  //  gPad->SetLogy();
  hTriggerHitsTimeInSpill->Draw();
  current_canvas->SaveAs(Form("%s/hTriggerHitsTimeInSpill.png", save_dir.c_str()));

  current_canvas = get_std_canvas();
  gPad->SetLogy();
  hTriggerChannelInFrameIntegrated->Draw();
  current_canvas->SaveAs(Form("%s/hTriggerChannelInFrameIntegrated.png", save_dir.c_str()));

  //  === === Tracking
  current_canvas = get_std_canvas();
  //  gPad->SetLogy();
  hTrackingHitsTimeInSpill->Draw();
  current_canvas->SaveAs(Form("%s/hTrackingHitsTimeInSpill.png", save_dir.c_str()));

  current_canvas = get_std_canvas();
  gPad->SetLogy();
  hTrackingChannelInFrameIntegrated->Draw();
  current_canvas->SaveAs(Form("%s/hTrackingChannelInFrameIntegrated.png", save_dir.c_str()));

  current_canvas = get_std_canvas();
  gPad->SetLogz();
  hTrackingChannelMap->Draw("col,text");
  current_canvas->SaveAs(Form("%s/hTrackingChannelMap.png", save_dir.c_str()));

  //  === === Timing
  current_canvas = get_std_canvas();
  //  gPad->SetLogy();
  hTimingHitsTimeInSpill->Draw();
  current_canvas->SaveAs(Form("%s/hTimingHitsTimeInSpill.png", save_dir.c_str()));

  current_canvas = get_std_canvas();
  gPad->SetLogy();
  hTimingChannelInFrameIntegrated->Draw();
  current_canvas->SaveAs(Form("%s/hTimingChannelInFrameIntegrated.png", save_dir.c_str()));

  current_canvas = get_std_canvas();
  gPad->SetLogz();
  hTimingChannelMap->Draw();
  current_canvas->SaveAs(Form("%s/hTimingChannelMap.png", save_dir.c_str()));

  current_canvas = get_std_canvas();
  gPad->SetLogy();
  hTimingTimeResolution->Draw();
  current_canvas->SaveAs(Form("%s/hTimingTimeResolution.png", save_dir.c_str()));

  //  === === Cherenkov
  current_canvas = get_std_canvas();
  //  gPad->SetLogy();
  hCherenkovHitsTimeInSpill->Draw();
  current_canvas->SaveAs(Form("%s/hCherenkovHitsTimeInSpill.png", save_dir.c_str()));

  current_canvas = get_std_canvas();
  gPad->SetLogy();
  hCherenkovChannelInFrameIntegrated->Draw();
  current_canvas->SaveAs(Form("%s/hCherenkovChannelInFrameIntegrated.png", save_dir.c_str()));

  for (auto [iCoordinate, object] : hGenericCoincidenceMapwTrigger)
  {
    auto iDevice = iCoordinate[0];
    auto iChip = iCoordinate[1];
    current_canvas = get_std_canvas();
    gPad->SetLogy();
    object->Draw();
    current_canvas->SaveAs(Form("%s/%s.png", save_dir.c_str(), object->GetName()));
  }

  for (auto [iCoordinate, object] : hGenericCoincidenceMapwTiming)
  {
    auto iDevice = iCoordinate[0];
    auto iChip = iCoordinate[1];
    current_canvas = get_std_canvas();
    gPad->SetLogy();
    object->Draw();
    current_canvas->SaveAs(Form("%s/%s.png", save_dir.c_str(), object->GetName()));
  }

  TFile *out = new TFile(output_file.c_str(), "RECREATE");
  hTriggerHitsTimeInSpill->Write();
  hTriggerChannelInFrameIntegrated->Write();
  hTimingHitsTimeInSpill->Write();
  hCherenkovHitsTimeInSpill->Write();
  for (auto [iCoordinate, object] : hGenericCoincidenceMapwTrigger)
  {
    object->Write();
  }
  out->Close();

  gROOT->SetBatch(kFALSE);
}
