#include "utility.h"
#include "TH1.h"
#include "TH2.h"
#include "TTree.h"
#include "TFile.h"
#include "math.h"
#include "TProfile.h"
#include "streaming_framer.h"
#include "mapping.h"
#include "alcor_recodata.h"
#include "alcor_spilldata.h"
#include "lightdata_writer.h"
#include "recodata_writer.h"

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill,
    bool force_recodata_rebuild,
    bool force_lightdata_rebuild)
{
  //  Input files
  //  --- Check if recodata has already been done
  std::string input_filename = data_repository + "/" + run_name + "/lightdata.root";
  TFile *input_file = open_or_build_rootfile(input_filename, lightdata_writer, data_repository, run_name, max_spill, force_lightdata_rebuild);
  if (!input_file)
    return;

  //  Link lightdata tree locally
  TTree *lightdata_tree = (TTree *)input_file->Get("lightdata");
  alcor_spilldata *spilldata = new alcor_spilldata();
  spilldata->link_to_tree(lightdata_tree);

  //  Get calibration
  TH2F *h_calibration_data = (TH2F *)input_file->Get("TH2F_fine_calib_global_index");
  spilldata->generate_calibration(h_calibration_data);
  // spilldata->read_calib_from_file("alcor_fine_calibration.txt");

  //  Get number of spills, limited to maximum requested spills
  auto n_spills = lightdata_tree->GetEntries();
  auto all_spills = std::min((int)n_spills, (int)max_spill);

  //  Prepare output file
  std::string outname = data_repository + "/" + run_name + "/recodata.root";
  if (std::filesystem::exists(outname) && !force_recodata_rebuild)
  {
    std::cout << "[INFO] Output file already exists, skipping: " << outname << std::endl;
    return; // or continue;
  }

  //  Link recodata tree locally
  TFile *output_file = TFile::Open(outname.c_str(), "RECREATE");
  TTree *recodata_tree = new TTree("recodata", "Recodata tree");
  alcor_recodata recodata;
  recodata.write_to_tree(recodata_tree);

  //  QA plots
  //  --- Utility
  std::map<int, std::vector<float>> participants_channel;
  std::map<int, std::vector<float>> hit_time_per_channel;
  std::map<int, float> trigger_in_frame;
  std::map<int, bool> trigger_edge_rejection;
  //  --- Avg hits
  TProfile *h_dcr_rate_start_of_spill = new TProfile("h_dcr_rate_start_of_spill", ";global channel;DCR (kHz)", 2056, 0, 2056);
  TProfile *h_hit_rate_triggered = new TProfile("h_hit_rate_triggered", ";global channel;DCR (kHz)", 2056, 0, 2056);
  //  --- Trigger checks
  TH2F *h_unknown_trigger_devices = new TH2F("h_unknown_trigger_devices", ";spill;device ID", all_spills, 0, all_spills, 30, 190, 220);
  TH2F *h_trigger_occupancy = new TH2F("h_trigger_occupancy", ";;saved frame ID + 10k*spill", 1000 * all_spills, 0, 1000000 * all_spills, 256, 0, 256);
  std::map<uint8_t, TH2F *> h_hitmap_per_trigger;
  std::map<uint8_t, TH2F *> h_time_delta_triggers;
  std::map<uint8_t, TH1F *> h_triggers_per_spill;
  // --- General QA
  TH2F *h_full_hitmap = new TH2F("h_full_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH1F *h_selected_radius_dist = new TH1F("h_selected_radius_dist", ";x (mm);y (mm)", 600, 0, 150);
  TH2F *h_full_hitmap_selected = new TH2F("h_full_hitmap_selected", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_selected_2 = new TH2F("h_full_hitmap_selected_2", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH1F *h_delta_time_selected = new TH1F("h_delta_time_selected", ";delta time (ns)", 2 * _FRAME_SIZE_, -_ALCOR_CC_TO_NS_ * 2 * _FRAME_SIZE_, _ALCOR_CC_TO_NS_ * 2 * _FRAME_SIZE_);
  TH1F *h_active_participants_per_channel = new TH1F("h_active_participants_per_channel", ";global index", 2048, 0, 2048);
  //  --- Lightdata calibration
  TH2F *h_delta_trigger_fine_calib = new TH2F("h_delta_trigger_fine_calib", ";fine tdc;#Delta t (ns)", 100, 20, 120, 1000, -200, 200);
  TH1F *h_delta_trigger_fine_calib_1d = new TH1F("h_delta_trigger_fine_calib_1d", ";#Delta t (ns)", 1000, -200, 200);
  TH1F *h_delta_trigger_fine_nocalib_1d = new TH1F("h_delta_trigger_fine_nocalib_1d", ";#Delta t (ns)", 100, -200, 200);
  TH2F *h_delta_trigger_timing_fine_calib = new TH2F("h_delta_trigger_timing_fine_calib", ";fine tdc;#Delta t (ns)", 100, 20, 120, 1000, -200, 200);
  TH1F *h_delta_trigger_timing_fine_calib_1d = new TH1F("h_delta_trigger_timing_fine_calib_1d", ";#Delta t (ns)", 100, -200, 200);
  TH1F *h_delta_trigger_timing_fine_nocalib_1d = new TH1F("h_delta_trigger_timing_fine_nocalib_1d", ";#Delta t (ns)", 1000, -200, 200);
  //  --- Timing
  TH1F *h_available_channel_chip_0_timing = new TH1F("h_available_channel_chip_0_timing", "", 32, 0, 32);
  TH1F *h_available_channel_chip_1_timing = new TH1F("h_available_channel_chip_1_timing", "", 32, 0, 32);
  TH2F *h_timing_hit_map = new TH2F("h_timing_hit_map", ";chip 0 hits;chip 1 hits", 33, 0, 33, 33, 0, 33);
  TH1F *h_timing_delta_time = new TH1F("h_timing_delta_time", "", 5000, -100, 100);
  TH1F *h_timing_cherenkov_delta_time = new TH1F("h_timing_cherenkov_delta_time", "", 2000, -200, 600);
  TH1F *h_timing_trigger_0_delta_time = new TH1F("h_timing_trigger_0_delta_time", "", 1000, -200, 200);
  TH2F *h_timing_cherenkov_delta_time_device = new TH2F("h_timing_cherenkov_delta_time_device", ";device ID;#Delta t (ns)", 10000, 0, 10000, 1000, -200, 200);

  //  TODO: use cached position
  std::map<int, std::array<float, 2>> index_to_hit_xy;
  std::map<std::array<float, 2>, int> hit_to_index_xy;
  for (auto i_index = 0; i_index < 2048 * 4; i_index++)
  {
    auto position = sipm4eic::get_position(i_index);
    if (fabs(position[0]) < 5 && fabs(position[1]) < 5)
      continue;
    index_to_hit_xy[i_index] = position;
    hit_to_index_xy[position] = i_index;
  }

  //  Loop over spills
  auto all_frames = 0;
  for (int i_spill = 0; i_spill < all_spills; ++i_spill)
  {
    //  Load data for current spill
    lightdata_tree->GetEntry(i_spill);
    spilldata->get_entry();
    auto &frames_in_spill = spilldata->get_frame_list_link();
    auto &frame_reference = spilldata->get_frame_reference_list_link();
    std::cout << "[INFO] Spill " << i_spill << " with " << frames_in_spill.size() << " frames " << std::endl;

    //  Get participating channels fill dummy event for recodata analysis
    participants_channel.clear();
    trigger_edge_rejection.clear();
    auto lanes_participating = spilldata->get_not_dead_participants();
    for (auto [device, lanes] : lanes_participating)
      for (auto current_lane : lanes)
        for (auto i_channel = 0; i_channel < 8; ++i_channel)
        {
          auto global_index = 256 * (device - 192) + 8 * (int)current_lane + i_channel;
          participants_channel[global_index] = {};
          h_active_participants_per_channel->Fill(global_index, 1. / all_spills);

          //  Hit particiapnt mask
          alcor_recodata_struct current_recodata_event;
          current_recodata_event.global_index = global_index;
          current_recodata_event.hit_x = index_to_hit_xy[global_index][0];
          current_recodata_event.hit_y = index_to_hit_xy[global_index][1];
          current_recodata_event.hit_t = 0.; // ns
          current_recodata_event.hit_mask = encode_bit(_HITMASK_dead_lane);
          recodata.add_hit(current_recodata_event);
        }
    recodata_tree->Fill();
    recodata.clear();

    //  Loop over frames
    auto i_saved_frame = -1;
    for (auto &current_lightdata_struct : frames_in_spill)
    {
      i_saved_frame++;
      auto current_frame = frame_reference[i_saved_frame];
      alcor_lightdata current_lightdata(current_lightdata_struct);

      if (i_saved_frame % 1000 == 0)
        std::cout << "\33[2K\r[INFO] Processing frame " << i_saved_frame << std::flush;

      //  Trigger
      std::vector<trigger_struct> current_triggers;
      auto frame_triggers = current_lightdata.get_triggers();
      for (auto current_trigger : frame_triggers)
      {
        //  Skip the unknown trigger tag
        if (current_trigger.index == 255)
        {
          h_unknown_trigger_devices->Fill(i_spill, current_trigger.coarse);
          continue;
        }

        //  Trimm trigger 0 coincidences
        trigger_edge_rejection[current_trigger.index] = false;
        if ((current_trigger.coarse < _FRAME_SIZE_ * 0.1) || (current_trigger.coarse > _FRAME_SIZE_ * 0.9))
          trigger_edge_rejection[current_trigger.index] = false;

        //  Save triggers in frame
        trigger_in_frame[current_trigger.index] = current_trigger.coarse;
        recodata.add_trigger(current_trigger);

        //  QA for triggers
        h_trigger_occupancy->Fill(current_frame + i_spill * 10000, current_trigger.index);
        if (!h_time_delta_triggers.count(current_trigger.index))
          h_time_delta_triggers[current_trigger.index] = new TH2F(Form("h_time_delta_triggers_%i", current_trigger.index), Form(";spill;t_{trigger%i} - t_{cherenkov}", current_trigger.index), n_spills, 0, n_spills, 1024 * 2, -1024, 1024);
        if (!h_triggers_per_spill.count(current_trigger.index))
          h_triggers_per_spill[current_trigger.index] = new TH1F(Form("h_triggers_per_spill_%i", current_trigger.index), Form(";spill;triggers"), n_spills, 0, n_spills);
        if (!h_hitmap_per_trigger.count(current_trigger.index))
          h_hitmap_per_trigger[current_trigger.index] = new TH2F(Form("h_hitmap_per_trigger_%i", current_trigger.index), ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
        h_triggers_per_spill[current_trigger.index]->Fill(i_spill);
      }

      hit_time_per_channel.clear();

      //  Loop over timing hits
      auto timing_hits = current_lightdata.get_timing_hits_link();
      int timing_hit_chip_0 = 0;
      int timing_hit_chip_1 = 0;
      float timing_ref_chip_0 = 0;
      float timing_ref_chip_1 = 0;
      std::map<int, bool> map_timing_hit_chip_0;
      std::map<int, bool> map_timing_hit_chip_1;
      for (auto current_timing_hit_struct : timing_hits)
      {
        alcor_finedata current_timing_hit(current_timing_hit_struct);
        if (current_timing_hit.get_chip() == 0)
        {
          if (!map_timing_hit_chip_0[current_timing_hit.get_global_index()])
          {
            timing_hit_chip_0++;
            timing_ref_chip_0 += (current_timing_hit.get_coarse() - current_timing_hit.get_phase());
            map_timing_hit_chip_0[current_timing_hit.get_global_index()] = true;
          }
        }
        if (current_timing_hit.get_chip() == 2)
        {
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
      auto timing_available = ((timing_hit_chip_0 == 32) && (timing_hit_chip_1 == 31)) && (fabs(delta_timing + 0.15) < 3 * 0.180);
      if (timing_available)
      {
        if (trigger_in_frame.count(0))
          h_timing_trigger_0_delta_time->Fill((ref_timing - trigger_in_frame[0]) * _ALCOR_CC_TO_NS_);
        h_timing_delta_time->Fill(delta_timing * _ALCOR_CC_TO_NS_);
        for (auto current_timing_hit_struct : timing_hits)
        {
          alcor_finedata current_timing_hit(current_timing_hit_struct);
          if (current_timing_hit.get_chip() == 0)
            h_available_channel_chip_0_timing->Fill(current_timing_hit.get_eo_channel() % 32);
          if (current_timing_hit.get_chip() == 2)
            h_available_channel_chip_1_timing->Fill(current_timing_hit.get_eo_channel() % 32);
          for (auto [trigger_index, hit_time] : trigger_in_frame)
            if (!trigger_edge_rejection[trigger_index])
            {
              if (trigger_index == 0)
              {
                h_delta_trigger_timing_fine_calib->Fill(current_timing_hit.get_fine(), (hit_time - current_timing_hit.get_phase() - current_timing_hit.get_coarse()) * _ALCOR_CC_TO_NS_);
                h_delta_trigger_timing_fine_calib_1d->Fill((hit_time - current_timing_hit.get_phase() - current_timing_hit.get_coarse()));
                h_delta_trigger_timing_fine_nocalib_1d->Fill((hit_time - current_timing_hit.get_coarse()));
              }
            }
        }
      }

      //  Loop over cherenkov hits
      auto cherenkov_hits = current_lightdata.get_cherenkov_hits_link();
      for (auto current_cherenkov_hit_struct : cherenkov_hits)
      {
        alcor_finedata current_cherenkov_hit(current_cherenkov_hit_struct);
        auto current_device = current_cherenkov_hit.get_device();
        auto current_fifo = current_cherenkov_hit.get_fifo();
        auto global_index = current_cherenkov_hit.get_global_index();
        auto hit_position = sipm4eic::get_position(sipm4eic::get_geo(current_cherenkov_hit_struct)); // TODO: use cached values
        alcor_recodata_struct current_recodata_event;

        //  Fill recodata
        current_recodata_event.global_index = global_index * 4 + current_cherenkov_hit.get_tdc();
        current_recodata_event.hit_x = hit_position[0];
        current_recodata_event.hit_y = hit_position[1];
        current_recodata_event.hit_t = (current_cherenkov_hit.get_coarse() - current_cherenkov_hit.get_phase()) * _ALCOR_CC_TO_NS_; // ns
        //  *** hack, to be fixed ASAP ***
        current_recodata_event.hit_t += current_recodata_event.global_index > 4096 ? 2.9196200 : 0.;
        //  TODO: Fix this
        current_recodata_event.hit_mask = current_cherenkov_hit.get_mask();

        auto hit_x_rnd = hit_position[0] + (_rnd_(_global_gen_) * 3.0 - 1.5);
        auto hit_y_rnd = hit_position[1] + (_rnd_(_global_gen_) * 3.0 - 1.5);

        if (timing_available)
        {
          h_timing_cherenkov_delta_time->Fill(current_recodata_event.hit_t - ref_timing * _ALCOR_CC_TO_NS_);
          h_timing_cherenkov_delta_time_device->Fill(current_recodata_event.global_index, current_recodata_event.hit_t - ref_timing * _ALCOR_CC_TO_NS_);
        }

        //  QA plots
        hit_time_per_channel[global_index].push_back(current_cherenkov_hit.get_coarse());
        for (auto [trigger_index, hit_time] : trigger_in_frame)
          if (!trigger_edge_rejection[trigger_index])
          {
            auto delta_t = (hit_time - current_cherenkov_hit.get_coarse());
            h_time_delta_triggers[trigger_index]->Fill(i_spill, delta_t);
            if (trigger_index == 0)
            {
              h_delta_trigger_fine_calib->Fill(current_cherenkov_hit.get_fine(), (current_cherenkov_hit.get_coarse() - current_cherenkov_hit.get_phase() - hit_time) * _ALCOR_CC_TO_NS_);
              h_delta_trigger_fine_calib_1d->Fill((current_cherenkov_hit.get_coarse() - current_cherenkov_hit.get_phase() - hit_time) * _ALCOR_CC_TO_NS_);
              h_delta_trigger_fine_nocalib_1d->Fill((current_cherenkov_hit.get_coarse() - hit_time) * _ALCOR_CC_TO_NS_);
            }
            if (fabs(delta_t) < 25)
              h_hitmap_per_trigger[trigger_index]->Fill(hit_x_rnd, hit_y_rnd);
          }
        h_full_hitmap->Fill(hit_x_rnd, hit_y_rnd);
        recodata.add_hit(current_recodata_event);
      }

      //  Find ring
      recodata.find_rings(1.0, 5.0);
      auto ring_found = false;
      for (auto current_hit = 0; current_hit < recodata.get_recodata().size(); current_hit++)
      {
        auto hit_x_rnd = recodata.get_hit_x(current_hit) + (_rnd_(_global_gen_) * 3.0 - 1.5);
        auto hit_y_rnd = recodata.get_hit_y(current_hit) + (_rnd_(_global_gen_) * 3.0 - 1.5);
        auto hit_t = recodata.get_hit_t(current_hit);
        if (decode_bits(recodata.get_hit_mask(current_hit))[0] == 1)
        {
          ring_found = true;
          h_delta_time_selected->Fill(hit_t - trigger_in_frame[0] * _ALCOR_CC_TO_NS_);

          auto delta_x = (recodata.get_hit_x(current_hit) - 1.75) * (recodata.get_hit_x(current_hit) - 1.75);
          auto delta_y = (recodata.get_hit_y(current_hit) - 0.75) * (recodata.get_hit_y(current_hit) - 0.75);
          auto current_radius = sqrt(delta_x + delta_y);
          h_selected_radius_dist->Fill(current_radius);
          if (fabs(hit_t - trigger_in_frame[0] * _ALCOR_CC_TO_NS_) > 50)
            h_full_hitmap_selected->Fill(hit_x_rnd, hit_y_rnd);
          else
            h_full_hitmap_selected_2->Fill(hit_x_rnd, hit_y_rnd);
        }
      }

      if (ring_found)
        recodata.add_trigger({102, _FRAME_SIZE_ / 2});

      //  Fill recodata tree
      recodata_tree->Fill();
      recodata.clear();

      //  Fill plots per channel
      for (auto [global_index, hits] : participants_channel)
      {
        if (trigger_in_frame.count(100))
          h_dcr_rate_start_of_spill->Fill(global_index, hit_time_per_channel[global_index].size() + hits.size());
        if (trigger_in_frame.count(0))
          h_hit_rate_triggered->Fill(global_index, hit_time_per_channel[global_index].size() + hits.size());
      }
    }
    std::cout << std::endl;
    std::cout << "[INFO] Finished processing spill " << i_spill << std::endl;
    all_frames += frames_in_spill.size();
  }

  //  Loop to determine offset
  TF1 *f_utility_gaussian = new TF1("f_utility_gaussian", "[2]*TMath::Gaus(x,[0],[1],true)", -100., 100.);
  h_timing_cherenkov_delta_time_device->GetYaxis()->SetRangeUser(-25., 25.);
  for (auto x_bin = 1; x_bin <= h_timing_cherenkov_delta_time_device->GetXaxis()->GetNbins(); x_bin++)
  {
    auto current_offset = 0.;
    auto h_projection = h_timing_cherenkov_delta_time_device->ProjectionY(Form("h_timing_cherenkov_delta_time_device_py_%d", x_bin), x_bin, x_bin);
    if (h_projection->GetEntries() < 15)
      continue;

    f_utility_gaussian->SetParameters(h_projection->GetMean(), h_projection->GetRMS(), h_projection->GetMaximum());
    f_utility_gaussian->SetParLimits(0, h_projection->GetMean() - 1, h_projection->GetMean() + 1);
    h_projection->Fit("gaus", "Q");
    current_offset = h_projection->GetFunction("gaus")->GetParameter(1);
    // spilldata->set_param2(x_bin - 1, current_offset);
  }

  //  Save output
  recodata_tree->Write();

  //  Normalise & polish plots
  h_dcr_rate_start_of_spill->Scale(
      (1. / 1000.) *            // kHz
      (1. / (_FRAME_SIZE_ *     // Integrated time window
             _ALCOR_CC_TO_NS_ * // clock cycles to ns
             1e-9)));           // ns to s to have Hz
  h_hit_rate_triggered->Scale((1. / 1000.) * (1. / (_FRAME_SIZE_ * _ALCOR_CC_TO_NS_ * 1e-9)));

  TCanvas *c_dcr_and_hit_rate = new TCanvas("c_dcr_and_hit_rate", "", 1500, 500);
  c_dcr_and_hit_rate->Divide(3, 1);
  c_dcr_and_hit_rate->cd(1);
  h_dcr_rate_start_of_spill->Draw("");
  c_dcr_and_hit_rate->cd(2);
  h_hit_rate_triggered->Draw("");
  auto h_ratio_hitrate_dcr = (TProfile *)h_hit_rate_triggered->Clone("h_ratio_hitrate_dcr");
  h_ratio_hitrate_dcr->Divide(h_dcr_rate_start_of_spill);
  c_dcr_and_hit_rate->cd(3);
  h_ratio_hitrate_dcr->Draw("HIST");
  new TCanvas();
  h_delta_trigger_fine_calib->Draw("COLZ");
  new TCanvas();
  h_delta_trigger_fine_calib_1d->Draw("HIST");
  h_delta_trigger_fine_calib_1d->Scale(1. / h_delta_trigger_fine_calib_1d->Integral());
  h_delta_trigger_fine_calib_1d->Scale(1., "width");
  h_delta_trigger_fine_nocalib_1d->SetLineColor(kRed);
  h_delta_trigger_fine_nocalib_1d->Draw("SAME HIST");
  h_delta_trigger_fine_nocalib_1d->Scale(1. / h_delta_trigger_fine_nocalib_1d->Integral());
  h_delta_trigger_fine_nocalib_1d->Scale(1., "width");
  new TCanvas();
  h_delta_trigger_timing_fine_calib->Draw("COLZ");
  new TCanvas();
  h_delta_trigger_timing_fine_calib_1d->Draw("HIST");
  h_delta_trigger_timing_fine_calib_1d->Scale(1. / h_delta_trigger_timing_fine_calib_1d->Integral());
  h_delta_trigger_timing_fine_calib_1d->Scale(1., "width");
  h_delta_trigger_timing_fine_nocalib_1d->SetLineColor(kRed);
  h_delta_trigger_timing_fine_nocalib_1d->Draw("SAME HIST");
  h_delta_trigger_timing_fine_nocalib_1d->Scale(1. / h_delta_trigger_timing_fine_nocalib_1d->Integral());
  h_delta_trigger_timing_fine_nocalib_1d->Scale(1., "width");
  new TCanvas();
  h_timing_hit_map->Draw("COLZ TEXT");
  new TCanvas();
  h_timing_delta_time->Draw();

  new TCanvas();
  TF1 *f_time_coincidence_profile = new TF1("f_time_coincidence_profile", "[0]+[1]  * TMath::Gaus(x, [2],  [3],  true) + [4]  * TMath::Gaus(x, [5],  [6],  true) +[7]  * TMath::Gaus(x, [8],  [9],  true) +[10] * TMath::Gaus(x, [11], [12], true) +[13] * TMath::Gaus(x, [14], [15], true)", -200, 600);
  f_time_coincidence_profile->SetNpx(1000);
  double *params = new double[16]{15., 8500., -7., 3., 32., 50., 5., 85., 95., 8., 13., 135., 8., 7., 90., 78.};
  f_time_coincidence_profile->SetParameters(params);
  h_timing_cherenkov_delta_time->Scale(1. / all_frames);
  h_timing_cherenkov_delta_time->Fit(f_time_coincidence_profile);
  h_timing_cherenkov_delta_time->Draw();

  new TCanvas();
  h_timing_cherenkov_delta_time_device->Draw("COLZ");

  new TCanvas();
  h_available_channel_chip_0_timing->Draw();
  new TCanvas();
  h_available_channel_chip_1_timing->Draw();

  new TCanvas();
  h_timing_trigger_0_delta_time->Draw();

  //  Save plots
  h_full_hitmap->Write();
  h_full_hitmap_selected->Write();
  h_full_hitmap_selected_2->Write();
  h_active_participants_per_channel->Write();
  h_selected_radius_dist->Write();
  h_delta_time_selected->Write();
  h_dcr_rate_start_of_spill->Write();
  h_hit_rate_triggered->Write();
  c_dcr_and_hit_rate->Write();
  h_trigger_occupancy->Write();
  h_unknown_trigger_devices->Write();
  h_delta_trigger_fine_calib->Write();
  for (auto [index, hist] : h_time_delta_triggers)
    hist->Write();
  for (auto [index, hist] : h_triggers_per_spill)
    hist->Write();
  for (auto [index, hist] : h_hitmap_per_trigger)
    hist->Write();

  spilldata->write_calib_to_file("alcor_fine_calibration.txt");

  input_file->Close();
  output_file->Close();
}

/*
  //TH2F *h_afterpulse = new TH2F("h_afterpulse", "", 10000, 0, 10000, _FRAME_SIZE_, 0, _FRAME_SIZE_);

  //  --- Afterpulse
  TH2F *h_afterpulse = new TH2F("h_afterpulse", "", 10000, 0, 10000, _FRAME_SIZE_, 0, _FRAME_SIZE_);
  std::map<int, std::vector<float>> participants_channel;
  std::map<int, std::vector<float>> hit_time_per_channel;
  //  --- Avg hits
  TProfile *h_dcr_rate_start_of_spill = new TProfile("h_dcr_rate_start_of_spill", ";global channel;DCR (kHz)", 2600, 0, 2600);
  TProfile *h_dcr_rate_start_of_spill_spill = new TProfile("h_dcr_rate_start_of_spill_spill", "", all_spills, 0, all_spills);

  //  Loop over spills
  for (int i_spill = 0; i_spill < all_spills; ++i_spill)
  {

    lightdata_tree->GetEntry(i_spill);
    spilldata->get_entry();
    auto frames_in_spill = spilldata->get_frame_list_link();
    std::cout << "[INFO] Spill " << i_spill << " with " << frames_in_spill.size() << " frames " << std::endl;
    auto &frame_reference = spilldata->get_frame_reference_list_link();

    //  Get participating channels
    participants_channel.clear();
    auto lanes_participating = spilldata->get_not_dead_participants();
    for (auto [device, lanes] : lanes_participating)
      for (auto current_lane : lanes)
        for (auto i_channel = 0; i_channel < 8; ++i_channel)
          participants_channel[256 * (device - 192) + 8 * (int)current_lane + i_channel] = {};

    //  Loop over frames
    auto i_saved_frame = -1;
    for (auto &current_lightdata_struct : frames_in_spill)
    {
      i_saved_frame++;
      auto current_frame = frame_reference[i_saved_frame];

      alcor_lightdata current_lightdata(current_lightdata_struct);

      hit_time_per_channel = participants_channel;
      auto cherenkov_hits = current_lightdata.get_cherenkov_hits_link();
      //  Loop over hits
      for (auto current_cherenkov_hit_struct : cherenkov_hits)
      {
        alcor_finedata current_cherenkov_hit(current_cherenkov_hit_struct);
        auto current_device = current_cherenkov_hit.get_device();
        auto global_index = current_cherenkov_hit.get_global_index();
        hit_time_per_channel[global_index].push_back(current_cherenkov_hit.get_coarse());
      }

      //  Fill plots per channel
      for (auto [global_index, hits] : hit_time_per_channel)
      {
        if (true)
        {
          h_dcr_rate_start_of_spill->Fill(global_index, hits.size());
          h_dcr_rate_start_of_spill_spill->Fill(i_spill, hits.size());
        }
        for (auto i_hit = 0; i_hit < hits.size(); i_hit++)
          for (auto j_hit = i_hit + 1; j_hit < hits.size(); j_hit++)
          {
            h_afterpulse->Fill(global_index, j_hit - i_hit);
          }
      }
    }
  }

  new TCanvas();
  h_afterpulse->Draw("COLZ");
  new TCanvas();
  h_dcr_rate_start_of_spill->Draw("COLZ");
  new TCanvas();
  h_dcr_rate_start_of_spill_spill->Draw("COLZ");

  h_dcr_rate_start_of_spill->Scale((1. / 1000.) * (1. / (1024 * 3.125 * 1e-9)));

  // input_file->Close();
  //  output_file->Close();
  */