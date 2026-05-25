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
#include "writers/lightdata.h"
#include "writers/recodata.h"

void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill,
    bool force_recodata_rebuild,
    bool force_lightdata_rebuild,
    std::string mapping_conf,
    std::string trigger_conf,
    std::string framer_conf)
{
    //  Framer configuration
    auto framer_cfg = FramerConfReader(framer_conf);

    //  Input files
    std::string input_filename = data_repository + "/" + run_name + "/lightdata.root";
    TFile *input_file = open_or_build_rootfile(input_filename, lightdata_writer, data_repository, run_name, max_spill, force_lightdata_rebuild);
    if (!input_file)
        return;

    //  Link lightdata tree locally
    TTree *lightdata_tree = (TTree *)input_file->Get("lightdata");
    AlcorSpilldata *spilldata = new AlcorSpilldata();
    spilldata->link_to_tree(lightdata_tree);

    AlcorFinedata::read_calib_from_file(data_repository + "/" + run_name + "/fine_calib.txt");

    auto fine_time_calib_th2f = (TH2F *)input_file->Get("h_fine_calib");
    AlcorFinedata::generate_calibration(fine_time_calib_th2f, true);

    //  Progress tracking — multi-bar with one subtask (per-frame post-processing).
    //  Main bar shows overall spill progress; the subtask clock is restarted
    //  at the head of each spill via progress_bars.restart() so it reflects
    //  THIS spill's reconstruction time, not the cumulative total.
    mist::logger::MultiProgressBar progress_bars(mist::logger::BarStyle::Block);
    progress_bars.set_unit("spills");
    auto &post_processing = progress_bars.add_subtask("post-processing");

    //  Generate Mapping
    Mapping current_mapping(mapping_conf);

    //  Build trigger registry from config
    //  The registry maps raw uint8 trigger values to a dense ordered list of
    //  (value, name) pairs — config-defined triggers first, built-in defaults
    //  after. This gives contiguous histogram bins with meaningful labels,
    //  avoiding the 252 empty bins you'd get from a raw 0–255 axis.
    auto trigger_configs = trigger_conf_reader(trigger_conf);
    TriggerRegistry registry(trigger_configs);
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
    AlcorRecodata recodata;
    recodata.write_to_tree(recodata_tree);

    //  Cache channel positions from Mapping

    // Phase 5: iterate (device, chip, channel) directly via the
    // GlobalIndex overload and key the cache by `4 * channel_ordinal` —
    // matches the position-cache convention in Mapping.cxx and the
    // MIST HoughTransform `lut_key`.
    std::map<int, std::array<float, 2>> index_to_hit_xy;
    {
        constexpr int kDeviceLo  = 192;
        constexpr int kDeviceHi  = 224;
        const int max_chip       = ::gidx::kUsesSplitInTwo ? 4 : 8;
        constexpr int kChannelHi = 64;
        for (int device = kDeviceLo; device < kDeviceHi; ++device)
            for (int chip = 0; chip < max_chip; ++chip)
                for (int channel = 0; channel < kChannelHi; ++channel)
                {
                    const auto gi = ::GlobalIndex::from_components(
                        device, /*fifo=*/0, chip, channel, /*tdc=*/0);
                    auto position = current_mapping.get_position_from_global_index(gi);
                    if (!position)
                        continue;
                    if (fabs((*position)[0]) < 5 && fabs((*position)[1]) < 5)
                        continue;
                    index_to_hit_xy[4 * gi.channel_ordinal()] = *position;
                }
    }

    //  Edge rejection window: 25 ns fixed, converted to clock cycles
    const float edge_rejection_cc = 25.f / BTANA_ALCOR_CC_TO_NS;

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
        500, 0, framer_cfg.frame_size);
    for (int i = 0; i < n_triggers; ++i)
        h_edge_trigger_position->GetXaxis()->SetBinLabel(i + 1, registry.triggers[i].second.c_str());

    std::unordered_map<int, TH1F *> h_trigger_time_diff_w_cherenkov;

    //  ── Loop over spills ─────────────────────────────────────────────────────
    std::map<int, std::vector<float>> map_of_offsets;
    for (int i_spill = 0; i_spill < all_spills; ++i_spill)
    {
        lightdata_tree->GetEntry(i_spill);
        spilldata->get_entry();
        auto &frames_in_spill = spilldata->get_frame_list_link();
        auto &frame_reference = spilldata->get_frame_reference_list_link();

        //  ── First loop for calibration ──────────────────────────────────────────
        for (auto &current_lightdata_struct : frames_in_spill)
        {
            AlcorLightdata current_lightdata(current_lightdata_struct);
            auto current_trigger_list = current_lightdata.get_triggers();
            auto timing_trigger = std::find_if(current_trigger_list.begin(),
                                               current_trigger_list.end(),
                                               [](const TriggerEvent &e)
                                               {
                                                   return e.index == _TRIGGER_STREAMING_RING_FOUND_;
                                               });
            if (timing_trigger != current_trigger_list.end())
            {
                for (auto current_cherenkov : current_lightdata.get_cherenkov_hits_link())
                {
                    AlcorFinedata current_hit(current_cherenkov);
                    auto index = current_hit.get_global_index();
                    map_of_offsets[index].push_back(
                        current_hit.get_time_ns() - timing_trigger->fine_time);
                }
            }
        }
    }
    for (auto &[channel_index, values_list] : map_of_offsets)
    {
        if (values_list.size() < 20)
            continue;

        auto offset_value = 0.f;
        auto offset_participants = 0;
        for (auto &value : values_list)
        {
            if (fabs(value) > 30)
                continue;
            offset_value += value;
            offset_participants++;
        }
        offset_value /= offset_participants;

        if (offset_participants < 20)
            continue;

        if (!channel_index)
        {
            AlcorFinedata temy_testt;
            temy_testt.set_global_index(0);
            temy_testt.set_rollover(0);
            temy_testt.set_coarse(0);
            temy_testt.set_fine(0);
            mist::logger::debug(Form("channel %i - offset: %f", channel_index, offset_value));
            mist::logger::debug(Form("channel %i - param0: %f", channel_index, AlcorFinedata::get_param0(channel_index)));
            mist::logger::debug(Form("channel %i - param1: %f", channel_index, AlcorFinedata::get_param1(channel_index)));
            mist::logger::debug(Form("channel %i - param2: %f", channel_index, AlcorFinedata::get_param2(channel_index)));
            mist::logger::debug(Form("channel %i - get_time_ns: %f", channel_index, temy_testt.get_time_ns()));
            AlcorFinedata::set_param2(channel_index, -offset_value / BTANA_ALCOR_CC_TO_NS);
            mist::logger::debug(Form("channel %i - param0: %f", channel_index, AlcorFinedata::get_param0(channel_index)));
            mist::logger::debug(Form("channel %i - param1: %f", channel_index, AlcorFinedata::get_param1(channel_index)));
            mist::logger::debug(Form("channel %i - param2: %f", channel_index, AlcorFinedata::get_param2(channel_index)));
            mist::logger::debug(Form("channel %i - get_time_ns: %f", channel_index, temy_testt.get_time_ns()));
        }
        AlcorFinedata::set_param2(channel_index, -offset_value / BTANA_ALCOR_CC_TO_NS);
    }
    mist::logger::debug("Save face");
    mist::logger::debug("Save face");
    mist::logger::debug("Save face");
    for (int i_spill = 0; i_spill < all_spills; ++i_spill)
    {
        //  Per-spill multi-bar reset (skip first iteration — the subtask is
        //  not yet active on the first pass through).
        if (i_spill > 0)
            progress_bars.restart(/*flush=*/false);
        progress_bars.update(i_spill, all_spills);

        lightdata_tree->GetEntry(i_spill);
        spilldata->get_entry();
        auto &frames_in_spill = spilldata->get_frame_list_link();
        auto &frame_reference = spilldata->get_frame_reference_list_link();

        //  Start-of-spill event: dead lane map
        auto lanes_participating = spilldata->get_not_dead_participants();
        for (auto [device, lanes] : lanes_participating)
            if (device < 200)
                for (auto current_lane : lanes)
                    for (auto i_channel = 0; i_channel < 8; ++i_channel)
                    {
                        // Phase 5: construct the synthetic dead-lane Hit
                        // with a new-layout GlobalIndex.  Split-in-two
                        // trick is applied here at the construction
                        // boundary.  The stored value is the new-layout
                        // raw; the position-cache lookup uses
                        // `4 * channel_ordinal` (the same dense-int key
                        // the position cache was populated with — see the
                        // top-of-function loop and `Mapping.cxx`).
                        const int chip_raw     = current_lane / 4;
                        const int channel_raw  = 8 * (current_lane % 4) + i_channel;
                        const int chip_logical = ::gidx::kUsesSplitInTwo
                                                     ? chip_raw / 2
                                                     : chip_raw;
                        const int channel_log  = ::gidx::kUsesSplitInTwo
                                                     ? channel_raw + 32 * (chip_raw % 2)
                                                     : channel_raw;
                        const auto gi = ::GlobalIndex::from_components(
                            device, current_lane, chip_logical, channel_log, 0);
                        const int pos_key = 4 * gi.channel_ordinal();
                        recodata.add_hit(
                            0., 0., 0.,
                            index_to_hit_xy[pos_key][0],
                            index_to_hit_xy[pos_key][1],
                            gi.raw(),
                            encode_bit(HitmaskDeadLane));
                    }
        recodata.add_trigger({TriggerStartOfSpill, static_cast<uint16_t>(framer_cfg.frame_size / 2)});
        recodata_tree->Fill();
        recodata.clear();

        //  ── Loop over frames ──────────────────────────────────────────────────
        int n_accepted = 0, n_edge = 0, n_duplicate = 0;
        int i_saved_frame = -1;
        i_saved_frame = -1;
        for (auto &current_lightdata_struct : frames_in_spill)
        {
            i_saved_frame++;
            AlcorLightdata current_lightdata(current_lightdata_struct);

            if (i_saved_frame % 1000 == 0)
                post_processing.update(i_saved_frame, frames_in_spill.size());

            h_frames_per_spill->Fill(i_spill, 0.5); // total

            //  ── Trigger selection ───────────────────────────────────────────────
            //  1. Triggers within 25 ns of either boundary → edge-rejected.
            //     Frame not immediately discarded; type may still have a valid instance.
            //  2. Two distinct valid instances of the same type → frame rejected.
            //  3. Two valid instances of the same type within BTANA_TRIGGER_MIN_SEPARATION
            //     cc → temporal duplicate, second dropped silently, frame kept.

            bool frame_rejected = false;
            bool had_edge = false;
            std::map<uint8_t, TriggerEvent> accepted_triggers;

            for (auto current_trigger : current_lightdata.get_triggers())
            {
                if (current_trigger.index == _TRIGGER_UNKNOWN_)
                    continue;

                accepted_triggers[current_trigger.index] = current_trigger;

                const int reg_bin = registry.index_of(current_trigger.index) + 0.5; // centre of bin

                bool is_edge = (current_trigger.fine_time < edge_rejection_cc) ||
                               (current_trigger.fine_time > framer_cfg.frame_size - edge_rejection_cc);
                if (is_edge)
                {
                    h_edge_trigger_position->Fill(reg_bin, current_trigger.fine_time);
                    h_trigger_qa->Fill(reg_bin, 1.5); // edge-rejected
                    had_edge = true;
                    continue;
                }

                if (accepted_triggers.count(current_trigger.index))
                {
                    auto &prev = accepted_triggers[current_trigger.index];
                    if (std::fabs((float)current_trigger.coarse - (float)prev.coarse) < BTANA_TRIGGER_MIN_SEPARATION)
                        continue; // temporal duplicate, drop silently

                    h_trigger_qa->Fill(reg_bin, 2.5); // duplicate-rejected
                    frame_rejected = true;
                    break;
                }

                accepted_triggers[current_trigger.index] = current_trigger;
                if (!h_trigger_time_diff_w_cherenkov.count(current_trigger.index))
                    h_trigger_time_diff_w_cherenkov[current_trigger.index] =
                        new TH1F(
                            Form("h_trigger_time_diff_w_cherenkov_%s", registry.name_of(current_trigger.index).c_str()),
                            ";#Delta_{t} (t_{Hit} - t_{trigger}) ns;Normalised entries",
                            5e3,
                            -500,
                            500);
                for (auto current_cherenkov_hit_struct : current_lightdata.get_cherenkov_hits_link())
                    h_trigger_time_diff_w_cherenkov[current_trigger.index]->Fill(
                        AlcorFinedata(current_cherenkov_hit_struct).get_time_ns() -
                        current_trigger.fine_time);
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

        //  Reflect the just-completed spill on the main bar.
        progress_bars.update(i_spill + 1, all_spills);
    } // end spill loop

    post_processing.finish(/*flush=*/false);
    progress_bars.finish();

    //  --- --- --- --- --- ---
    //  QA plots
    //  ---
    output_file->cd();
    recodata_tree->Write();
    //  ---
    //  --- Trigger QA
    TDirectory *trigger_dir = output_file->mkdir("Triggers");
    trigger_dir->cd();
    h_trigger_qa->Write();
    h_frames_per_spill->Write();
    h_edge_trigger_position->Write();
    for (auto [key, val] : h_trigger_time_diff_w_cherenkov)
        val->Write();
    //
    //  ---
    //  --- Close file
    input_file->Close();
    output_file->Close();
    //  ---
    //  End: QA plots
    //  --- --- --- --- --- ---
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
        for (auto &[position, GlobalIndex] : hit_to_index_xy)
        {
          float distance = std::hypot(position[0] - candidate[0], position[1] - candidate[1]);
          if (distance < tolerance)
          {
            current_neighbors_list.push_back(GlobalIndex);
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
  TH1F *h_delta_time_selected     = new TH1F("h_delta_time_selected",     ";delta time (ns)", 2 * framer_cfg.frame_size, -BTANA_ALCOR_CC_TO_NS * 2 * framer_cfg.frame_size, BTANA_ALCOR_CC_TO_NS * 2 * framer_cfg.frame_size);
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
  // Map keys are global_channel_raw() values — per-channel uniqueness with
  // TDC bits cleared.  Storage is the new-layout raw (Phase 5), so
  // construction is direct (no from_legacy).
  std::map<uint32_t, bool> map_timing_hit_chip_0, map_timing_hit_chip_1;
  for (auto current_timing_hit_struct : current_lightdata.get_timing_hits_link())
  {
    AlcorFinedata current_timing_hit(current_timing_hit_struct);
    const uint32_t channel_key =
        ::GlobalIndex(current_timing_hit.get_global_index()).global_channel_raw();

    if (current_timing_hit.get_chip() == 0 && !map_timing_hit_chip_0[channel_key])
    {
      timing_hit_chip_0++;
      timing_ref_chip_0 += (current_timing_hit.get_coarse() - current_timing_hit.get_phase());
      map_timing_hit_chip_0[channel_key] = true;
    }
    if (current_timing_hit.get_chip() == 2 && !map_timing_hit_chip_1[channel_key])
    {
      timing_hit_chip_1++;
      timing_ref_chip_1 += (current_timing_hit.get_coarse() - current_timing_hit.get_phase());
      map_timing_hit_chip_1[channel_key] = true;
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
  for (auto &[GlobalIndex, times] : hit_time_per_channel)
  {
    if (times.empty()) continue;
    if (!index_to_proximity_index.count(GlobalIndex)) continue;
    for (auto neighbor_index : index_to_proximity_index[GlobalIndex])
    {
      for (auto t_self : times)
        for (auto t_neighbor : hit_time_per_channel[neighbor_index])
          if ((t_neighbor - t_self) < BTANA_CROSS_TALK_DEADTIME && (t_neighbor - t_self) > 0)
            if (channel_time_to_recodata_index.count({neighbor_index, t_neighbor}))
              recodata.add_hit_mask_bit(channel_time_to_recodata_index[{neighbor_index, t_neighbor}], HitmaskCrossTalk);
      if (frames_are_adjacent)
        for (auto t_self : times)
          for (auto t_neighbor : prev_hit_time_per_channel[neighbor_index])
            if ((t_neighbor - t_self) < BTANA_CROSS_TALK_DEADTIME && (t_neighbor - t_self) > 0)
              if (channel_time_to_recodata_index.count({neighbor_index, t_neighbor}))
                recodata.add_hit_mask_bit(channel_time_to_recodata_index[{neighbor_index, t_neighbor}], HitmaskCrossTalk);
    }
  }
  prev_hit_time_per_channel = hit_time_per_channel;
  prev_frame_reference = current_frame;

  //  Per-channel DCR fill
  for (auto [GlobalIndex, hits] : participants_channel)
  {
    if (trigger_in_frame.count(100))
      h_dcr_rate_start_of_spill->Fill(GlobalIndex, hit_time_per_channel[GlobalIndex].size() + hits.size());
    if (trigger_in_frame.count(0))
      h_hit_rate_triggered->Fill(GlobalIndex, hit_time_per_channel[GlobalIndex].size() + hits.size());
  }

  //  Normalise & save QA plots
  h_dcr_rate_start_of_spill->Scale((1./1000.) * (1./(framer_cfg.frame_size * BTANA_ALCOR_CC_TO_NS * 1e-9)));
  h_hit_rate_triggered->Scale((1./1000.) * (1./(framer_cfg.frame_size * BTANA_ALCOR_CC_TO_NS * 1e-9)));
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