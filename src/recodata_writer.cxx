#include "utility.h"
#include "TH1.h"
#include "TH2.h"
#include "TTree.h"
#include "TFile.h"
#include "mapping.h"
#include "math.h"
#include "TProfile.h"
#include "parallel_streaming_framer.h"
#include "alcor_recodata.h"
#include "alcor_spilldata.h"
#include "lightdata_writer.h"
#include "recodata_writer.h"

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill,
    bool force_recodata_rebuild,
    bool force_lightdata_rebuild,
    std::string mapping_conf,
    std::string trigger_conf)
{
    //  Input files
    std::string input_filename = data_repository + "/" + run_name + "/lightdata.root";
    TFile *input_file = open_or_build_rootfile(input_filename, lightdata_writer, data_repository, run_name, max_spill, force_lightdata_rebuild);
    if (!input_file)
        return;

    //  Link lightdata tree locally
    TTree *lightdata_tree = (TTree *)input_file->Get("lightdata");
    alcor_spilldata *spilldata = new alcor_spilldata();
    spilldata->link_to_tree(lightdata_tree);

    //  Generate mapping
    mapping current_mapping(mapping_conf);

    //  Build trigger registry from config
    //  The registry maps raw uint8 trigger values to a dense ordered list of
    //  (value, name) pairs — config-defined triggers first, built-in defaults
    //  after. This gives contiguous histogram bins with meaningful labels,
    //  avoiding the 252 empty bins you'd get from a raw 0–255 axis.
    auto trigger_configs = trigger_conf_reader(trigger_conf);
    trigger_registry registry(trigger_configs);
    const int n_triggers = registry.size();

    //  Get number of spills, limited to maximum requested spills
    auto n_spills = lightdata_tree->GetEntries();
    auto all_spills = std::min((int)n_spills, (int)max_spill);

    //  Prepare output file
    std::string outname = data_repository + "/" + run_name + "/recodata.root";
    if (std::filesystem::exists(outname) && !force_recodata_rebuild)
    {
        mist::logger::info(Form("Output file already exists, skipping: %s", outname.c_str()));
        return;
    }

    TFile *output_file = TFile::Open(outname.c_str(), "RECREATE");
    TTree *recodata_tree = new TTree("recodata", "Recodata tree");
    alcor_recodata recodata;
    recodata.write_to_tree(recodata_tree);

    //  Cache channel positions from mapping
    std::map<int, std::array<float, 2>> index_to_hit_xy;
    for (auto i_index = 0; i_index < 2048 * 4; i_index += 4)
    {
        auto position = current_mapping.get_position_from_global_index(i_index);
        if (!position)
            continue;
        if (fabs((*position)[0]) < 5 && fabs((*position)[1]) < 5)
            continue;
        index_to_hit_xy[i_index] = (*position);
    }

    //  Edge rejection window: 25 ns fixed, converted to clock cycles
    const float edge_rejection_cc = 25.f / _ALCOR_CC_TO_NS_;

    //  ── Trigger selection QA histograms ──────────────────────────────────────
    //  X axis uses registry position (dense, named) instead of raw trigger index.
    //  registry.index_of(raw) maps any observed trigger value to its bin.
    //
    //  h_trigger_qa: per-trigger-type outcome counts
    //    Y bin 1 = accepted, 2 = edge-rejected, 3 = duplicate-rejected
    //  h_frames_per_spill: frame counts per spill per outcome category
    //  h_edge_trigger_position: coarse time of edge-rejected triggers,
    //    to verify the 25 ns cut is well placed

    TH2F *h_trigger_qa = new TH2F(
        "h_trigger_qa",
        "Trigger selection QA;trigger;outcome",
        n_triggers, 0, n_triggers,
        3, 0, 3);
    for (int i = 0; i < n_triggers; ++i)
        h_trigger_qa->GetXaxis()->SetBinLabel(i + 1, registry.triggers[i].second.c_str());
    h_trigger_qa->GetYaxis()->SetBinLabel(1, "accepted");
    h_trigger_qa->GetYaxis()->SetBinLabel(2, "edge-rejected");
    h_trigger_qa->GetYaxis()->SetBinLabel(3, "duplicate-rejected");

    TH2F *h_frames_per_spill = new TH2F(
        "h_frames_per_spill",
        "Frame counts per spill;spill;category",
        all_spills, 0, all_spills,
        4, 0, 4);
    h_frames_per_spill->GetYaxis()->SetBinLabel(1, "total");
    h_frames_per_spill->GetYaxis()->SetBinLabel(2, "accepted");
    h_frames_per_spill->GetYaxis()->SetBinLabel(3, "had edge trigger");
    h_frames_per_spill->GetYaxis()->SetBinLabel(4, "duplicate-rejected");

    TH2F *h_edge_trigger_position = new TH2F(
        "h_edge_trigger_position",
        "Position of edge-rejected triggers;trigger;coarse time (cc)",
        n_triggers, 0, n_triggers,
        500, 0, _FRAME_SIZE_);
    for (int i = 0; i < n_triggers; ++i)
        h_edge_trigger_position->GetXaxis()->SetBinLabel(i + 1, registry.triggers[i].second.c_str());

    //  ── Loop over spills ─────────────────────────────────────────────────────
    for (int i_spill = 0; i_spill < all_spills; ++i_spill)
    {
        lightdata_tree->GetEntry(i_spill);
        spilldata->get_entry();
        auto &frames_in_spill = spilldata->get_frame_list_link();
        auto &frame_reference = spilldata->get_frame_reference_list_link();
        mist::logger::info(Form("Spill %i with %zu frames", i_spill, frames_in_spill.size()));

        //  Start-of-spill event: dead lane map
        auto lanes_participating = spilldata->get_not_dead_participants();
        for (auto [device, lanes] : lanes_participating)
            if (device < 200)
                for (auto current_lane : lanes)
                    if (current_lane)
                        for (auto i_channel = 0; i_channel < 8; ++i_channel)
                        {
                            auto global_index = get_global_index(device, current_lane / 4, 8 * (current_lane % 4) + i_channel, 0);
                            recodata.add_hit(
                                0., 0., 0.,
                                index_to_hit_xy[global_index][0],
                                index_to_hit_xy[global_index][1],
                                global_index,
                                encode_bit(_HITMASK_dead_lane));
                        }
        recodata.add_trigger({_TRIGGER_START_OF_SPILL_, _FRAME_SIZE_ / 2});
        recodata_tree->Fill();
        recodata.clear();

        //  ── Loop over frames ──────────────────────────────────────────────────
        int n_accepted = 0, n_edge = 0, n_duplicate = 0;
        int i_saved_frame = -1;

        for (auto &current_lightdata_struct : frames_in_spill)
        {
            i_saved_frame++;
            alcor_lightdata current_lightdata(current_lightdata_struct);

            if (i_saved_frame % 1000 == 0)
                mist::logger::info(Form("\033[2K\rProcessing frame %i", i_saved_frame), true);

            h_frames_per_spill->Fill(i_spill, 0.5); // total

            //  ── Trigger selection ───────────────────────────────────────────────
            //  1. Triggers within 25 ns of either boundary → edge-rejected.
            //     Frame not immediately discarded; type may still have a valid instance.
            //  2. Two distinct valid instances of the same type → frame rejected.
            //  3. Two valid instances of the same type within _TRIGGER_MIN_SEPARATION_
            //     cc → temporal duplicate, second dropped silently, frame kept.

            bool frame_rejected = false;
            bool had_edge = false;
            std::map<uint8_t, trigger_event> accepted_triggers;

            for (auto current_trigger : current_lightdata.get_triggers())
            {
                if (current_trigger.index == _TRIGGER_UNKNOWN_)
                    continue;

                const int reg_bin = registry.index_of(current_trigger.index) + 0.5; // centre of bin

                bool is_edge = (current_trigger.coarse < edge_rejection_cc) ||
                               (current_trigger.coarse > _FRAME_SIZE_ - edge_rejection_cc);
                if (is_edge)
                {
                    h_edge_trigger_position->Fill(reg_bin, current_trigger.coarse);
                    h_trigger_qa->Fill(reg_bin, 1.5); // edge-rejected
                    had_edge = true;
                    continue;
                }

                if (accepted_triggers.count(current_trigger.index))
                {
                    auto &prev = accepted_triggers[current_trigger.index];
                    if (std::fabs((float)current_trigger.coarse - (float)prev.coarse) < _TRIGGER_MIN_SEPARATION_)
                        continue; // temporal duplicate, drop silently

                    h_trigger_qa->Fill(reg_bin, 2.5); // duplicate-rejected
                    frame_rejected = true;
                    break;
                }

                accepted_triggers[current_trigger.index] = current_trigger;
            }

            if (frame_rejected)
            {
                n_duplicate++;
                h_frames_per_spill->Fill(i_spill, 3.5); // duplicate-rejected
                continue;
            }

            if (had_edge)
            {
                n_edge++;
                h_frames_per_spill->Fill(i_spill, 2.5); // had edge trigger (frame kept)
            }

            for (auto &[index, trigger] : accepted_triggers)
            {
                h_trigger_qa->Fill(registry.index_of(index) + 0.5, 0.5); // accepted
                recodata.add_trigger(trigger);
            }

            //  ── Cherenkov hits ─────────────────────────────────────────────────
            for (auto current_cherenkov_hit_struct : current_lightdata.get_cherenkov_hits_link())
                recodata.add_hit(current_cherenkov_hit_struct);

            recodata_tree->Fill();
            recodata.clear();
            n_accepted++;
            h_frames_per_spill->Fill(i_spill, 1.5); // accepted
        }

        mist::logger::info(Form("Spill %i done — accepted: %i  had-edge: %i  duplicate-rejected: %i  total: %zu",
                                i_spill, n_accepted, n_edge, n_duplicate, frames_in_spill.size()));
    }

    //  Save output
    recodata_tree->Write();
    h_trigger_qa->Write();
    h_frames_per_spill->Write();
    h_edge_trigger_position->Write();
    input_file->Close();
    output_file->Close();
}

/*
// ============================================================================
//  QA / CALIBRATION — retained for eventual retrieval
// ============================================================================

  //  Get calibration
  TH2F *h_calibration_data = (TH2F *)input_file->Get("TH2F_fine_calib_global_index");
  // spilldata->generate_calibration(h_calibration_data);
  // spilldata->read_calib_from_file("alcor_fine_calibration.txt");

  //  Proximity cache for cross-talk tagging
  std::map<std::array<float, 2>, int> hit_to_index_xy;
  for (auto &[i_index, pos] : index_to_hit_xy)
    hit_to_index_xy[pos] = i_index;

  const float pitch = 4.0f;
  const float tolerance = 1.f;
  std::map<int, std::vector<int>> index_to_proximity_index;
  float average_hits = 0.f;
  for (auto &[current_index, current_position] : index_to_hit_xy)
  {
    auto &current_neighbors_list = index_to_proximity_index[current_index];
    for (int x_neighbor : {-1, 0, 1})
      for (int y_neighbor : {-1, 0, 1})
      {
        if (x_neighbor == 0 && y_neighbor == 0)
          continue;
        std::array<float, 2> candidate = {
            current_position[0] + x_neighbor * pitch,
            current_position[1] + y_neighbor * pitch};
        for (auto &[position, global_index] : hit_to_index_xy)
        {
          float distance = std::hypot(position[0] - candidate[0], position[1] - candidate[1]);
          if (distance < tolerance)
          {
            current_neighbors_list.push_back(global_index);
            break;
          }
        }
      }
    average_hits += current_neighbors_list.size();
  }
  mist::logger::debug(Form("index_to_proximity_index size: %zu", index_to_proximity_index.size()));
  mist::logger::debug(Form("index_to_proximity_index avg: %f", average_hits / index_to_proximity_index.size()));

  //  QA histograms
  TProfile *h_dcr_rate_start_of_spill = new TProfile("h_dcr_rate_start_of_spill", ";global channel;DCR (kHz)", 2056, 0, 2056);
  TProfile *h_hit_rate_triggered       = new TProfile("h_hit_rate_triggered",       ";global channel;DCR (kHz)", 2056, 0, 2056);
  TH2F *h_unknown_trigger_devices      = new TH2F("h_unknown_trigger_devices",      ";spill;device ID", all_spills, 0, all_spills, 30, 190, 220);
  TH2F *h_trigger_occupancy            = new TH2F("h_trigger_occupancy",            ";;saved frame ID + 10k*spill", 1000 * all_spills, 0, 1000000 * all_spills, 256, 0, 256);
  std::map<uint8_t, TH2F *> h_hitmap_per_trigger;
  std::map<uint8_t, TH2F *> h_time_delta_triggers;
  std::map<uint8_t, TH1F *> h_triggers_per_spill;
  TH2F *h_full_hitmap             = new TH2F("h_full_hitmap",             ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH1F *h_selected_radius_dist    = new TH1F("h_selected_radius_dist",    ";x (mm);y (mm)", 600, 0, 150);
  TH2F *h_full_hitmap_selected    = new TH2F("h_full_hitmap_selected",    ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_selected_2  = new TH2F("h_full_hitmap_selected_2",  ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH1F *h_delta_time_selected     = new TH1F("h_delta_time_selected",     ";delta time (ns)", 2 * _FRAME_SIZE_, -_ALCOR_CC_TO_NS_ * 2 * _FRAME_SIZE_, _ALCOR_CC_TO_NS_ * 2 * _FRAME_SIZE_);
  TH1F *h_active_participants_per_channel = new TH1F("h_active_participants_per_channel", ";global index", 2048, 0, 2048);
  TH2F *h_delta_trigger_fine_calib              = new TH2F("h_delta_trigger_fine_calib",              ";fine tdc;#Delta t (ns)", 100, 20, 120, 1000, -200, 200);
  TH1F *h_delta_trigger_fine_calib_1d           = new TH1F("h_delta_trigger_fine_calib_1d",           ";#Delta t (ns)", 1000, -200, 200);
  TH1F *h_delta_trigger_fine_nocalib_1d         = new TH1F("h_delta_trigger_fine_nocalib_1d",         ";#Delta t (ns)", 100, -200, 200);
  TH2F *h_delta_trigger_timing_fine_calib       = new TH2F("h_delta_trigger_timing_fine_calib",       ";fine tdc;#Delta t (ns)", 100, 20, 120, 1000, -200, 200);
  TH1F *h_delta_trigger_timing_fine_calib_1d    = new TH1F("h_delta_trigger_timing_fine_calib_1d",    ";#Delta t (ns)", 100, -200, 200);
  TH1F *h_delta_trigger_timing_fine_nocalib_1d  = new TH1F("h_delta_trigger_timing_fine_nocalib_1d",  ";#Delta t (ns)", 1000, -200, 200);
  TH1F *h_available_channel_chip_0_timing = new TH1F("h_available_channel_chip_0_timing", "", 32, 0, 32);
  TH1F *h_available_channel_chip_1_timing = new TH1F("h_available_channel_chip_1_timing", "", 32, 0, 32);
  TH2F *h_timing_hit_map                  = new TH2F("h_timing_hit_map",                  ";chip 0 hits;chip 1 hits", 33, 0, 33, 33, 0, 33);
  TH1F *h_timing_delta_time               = new TH1F("h_timing_delta_time",               "", 5000, -100, 100);
  TH1F *h_timing_cherenkov_delta_time     = new TH1F("h_timing_cherenkov_delta_time",     "", 2000, -200, 600);
  TH1F *h_timing_trigger_0_delta_time     = new TH1F("h_timing_trigger_0_delta_time",     "", 1000, -200, 200);
  TH2F *h_timing_cherenkov_delta_time_device = new TH2F("h_timing_cherenkov_delta_time_device", ";device ID;#Delta t (ns)", 10000, 0, 10000, 1000, -200, 200);

  //  Participants per channel (for DCR)
  std::map<int, std::vector<float>> participants_channel;
  std::map<int, std::vector<float>> hit_time_per_channel;
  std::map<int, float> trigger_in_frame;

  //  Inside frame loop — timing reference
  int timing_hit_chip_0 = 0, timing_hit_chip_1 = 0;
  float timing_ref_chip_0 = 0, timing_ref_chip_1 = 0;
  std::map<int, bool> map_timing_hit_chip_0, map_timing_hit_chip_1;
  for (auto current_timing_hit_struct : current_lightdata.get_timing_hits_link())
  {
    alcor_finedata current_timing_hit(current_timing_hit_struct);
    if (current_timing_hit.get_chip() == 0 && !map_timing_hit_chip_0[current_timing_hit.get_global_index() / 4])
    {
      timing_hit_chip_0++;
      timing_ref_chip_0 += (current_timing_hit.get_coarse() - current_timing_hit.get_phase());
      map_timing_hit_chip_0[current_timing_hit.get_global_index() / 4] = true;
    }
    if (current_timing_hit.get_chip() == 2 && !map_timing_hit_chip_1[current_timing_hit.get_global_index() / 4])
    {
      timing_hit_chip_1++;
      timing_ref_chip_1 += (current_timing_hit.get_coarse() - current_timing_hit.get_phase());
      map_timing_hit_chip_1[current_timing_hit.get_global_index() / 4] = true;
    }
  }
  if (timing_hit_chip_0 && timing_hit_chip_1)
    h_timing_hit_map->Fill(timing_hit_chip_0, timing_hit_chip_1);
  auto ref_timing   = (timing_ref_chip_1 / timing_hit_chip_1 + timing_ref_chip_0 / timing_hit_chip_0) / 2.;
  auto delta_timing = (timing_ref_chip_1 / timing_hit_chip_1 - timing_ref_chip_0 / timing_hit_chip_0);
  auto timing_available = ((timing_hit_chip_0 == 32) && (timing_hit_chip_1 == 31)) && (fabs(delta_timing + 0.15) < 3 * 0.180);

  //  Inside frame loop — cross-talk tagging
  std::map<int, std::vector<float>> prev_hit_time_per_channel;
  int prev_frame_reference = -1;
  std::map<std::pair<int, float>, int> channel_time_to_recodata_index;
  bool frames_are_adjacent = (current_frame == prev_frame_reference + 1);
  for (auto &[global_index, times] : hit_time_per_channel)
  {
    if (times.empty()) continue;
    if (!index_to_proximity_index.count(global_index)) continue;
    for (auto neighbor_index : index_to_proximity_index[global_index])
    {
      for (auto t_self : times)
        for (auto t_neighbor : hit_time_per_channel[neighbor_index])
          if ((t_neighbor - t_self) < _CROSS_TALK_DEADTIME_ && (t_neighbor - t_self) > 0)
            if (channel_time_to_recodata_index.count({neighbor_index, t_neighbor}))
              recodata.add_hit_mask_bit(channel_time_to_recodata_index[{neighbor_index, t_neighbor}], _HITMASK_cross_talk);
      if (frames_are_adjacent)
        for (auto t_self : times)
          for (auto t_neighbor : prev_hit_time_per_channel[neighbor_index])
            if ((t_neighbor - t_self) < _CROSS_TALK_DEADTIME_ && (t_neighbor - t_self) > 0)
              if (channel_time_to_recodata_index.count({neighbor_index, t_neighbor}))
                recodata.add_hit_mask_bit(channel_time_to_recodata_index[{neighbor_index, t_neighbor}], _HITMASK_cross_talk);
    }
  }
  prev_hit_time_per_channel = hit_time_per_channel;
  prev_frame_reference = current_frame;

  //  Per-channel DCR fill
  for (auto [global_index, hits] : participants_channel)
  {
    if (trigger_in_frame.count(100))
      h_dcr_rate_start_of_spill->Fill(global_index, hit_time_per_channel[global_index].size() + hits.size());
    if (trigger_in_frame.count(0))
      h_hit_rate_triggered->Fill(global_index, hit_time_per_channel[global_index].size() + hits.size());
  }

  //  Normalise & save QA plots
  h_dcr_rate_start_of_spill->Scale((1./1000.) * (1./(_FRAME_SIZE_ * _ALCOR_CC_TO_NS_ * 1e-9)));
  h_hit_rate_triggered->Scale((1./1000.) * (1./(_FRAME_SIZE_ * _ALCOR_CC_TO_NS_ * 1e-9)));
  h_full_hitmap->Write();
  h_full_hitmap_selected->Write();
  h_full_hitmap_selected_2->Write();
  h_active_participants_per_channel->Write();
  h_selected_radius_dist->Write();
  h_delta_time_selected->Write();
  h_dcr_rate_start_of_spill->Write();
  h_hit_rate_triggered->Write();
  h_trigger_occupancy->Write();
  h_unknown_trigger_devices->Write();
  h_delta_trigger_fine_calib->Write();
  for (auto [index, hist] : h_time_delta_triggers)  hist->Write();
  for (auto [index, hist] : h_triggers_per_spill)   hist->Write();
  for (auto [index, hist] : h_hitmap_per_trigger)   hist->Write();
  spilldata->write_calib_to_file("alcor_fine_calibration.txt");

*/