#include "parallel_streaming_framer.h"
#include "lightdata_writer.h"

//  TODO: make the CLI multithred flag
//  TODO: cache mapping positions
void lightdata_writer(
    const std::string &data_repository,
    const std::string &run_name,
    int max_spill,
    bool force_lightdata_rebuild,
    int requested_n_threads)
{
  //  Do not make ownership of histograms to current directory
  TH1::AddDirectory(false);

  //  --- --- --- --- --- ---
  //  Input files
  //  ---
  std::filesystem::path base_dir = data_repository + "/" + run_name;
  std::vector<std::string> filenames;
  std::unordered_map<std::string, std::vector<std::string>> print_found_files;
  //  Check the given folder existence
  if (!std::filesystem::exists(base_dir))
  {
    logger::log_error("(lightdata_writer) Data folder does not exist, abort");
    return;
  }
  //  Check the given folder is actually a directory
  if (!std::filesystem::is_directory(base_dir))
  {
    logger::log_error("(lightdata_writer) Data folder is not a folder, abort");
    return;
  }
  //  Check the given folder is not empty
  if (std::filesystem::is_empty(base_dir))
  {
    logger::log_error("(lightdata_writer) Data folder is empty, abort");
    return;
  }
  //  Iterate in the directory
  for (const auto &device_dir : std::filesystem::directory_iterator(base_dir))
  {
    //  Skip non directories
    if (!std::filesystem::is_directory(device_dir))
      continue;

    //  Get current device
    std::string device_name = device_dir.path().filename().string();

    //  Check there is the decoded directory
    std::filesystem::path decoded_dir = device_dir.path() / "decoded";
    if (!std::filesystem::exists(decoded_dir) || !std::filesystem::is_directory(decoded_dir))
    {
      logger::log_warning(Form("(lightdata_writer) Data folder for device %s do not have decoded data, skipping", device_name.c_str()));
      continue;
    }
    //  Loop on files in decoded
    for (const auto &file : std::filesystem::directory_iterator(decoded_dir))
    {
      if (file.path().extension() == ".root")
      {
        std::string file_name = file.path().filename().string();
        filenames.push_back(file.path());
        print_found_files[device_name].push_back(file_name);
      }
    }
  }
  // Collect and sort devices numerically by their trailing number
  std::vector<std::pair<int, std::string>> sorted_devices;
  for (auto [current_device, _] : print_found_files)
  {
    auto dash = current_device.rfind('-');
    int dev_num = (dash != std::string::npos) ? std::stoi(current_device.substr(dash + 1)) : 0;
    sorted_devices.push_back({dev_num, current_device});
  }
  std::sort(sorted_devices.begin(), sorted_devices.end());
  logger::log_info("[INFO] Found devices with files: ");
  for (auto [dev_num, current_device] : sorted_devices)
  {
    std::vector<int> fifo_numbers;
    for (auto current_file : print_found_files[current_device])
    {
      auto start = current_file.find("fifo_");
      auto end = current_file.find(".root");
      if (start != std::string::npos && end != std::string::npos)
        fifo_numbers.push_back(std::stoi(current_file.substr(start + 5, end - (start + 5))));
    }
    std::sort(fifo_numbers.begin(), fifo_numbers.end());
    std::cout << "[Device: " << current_device << "] Found fifos: ";
    for (auto n : fifo_numbers)
      std::cout << n << " ";
    std::cout << std::endl;
  }
  //  ---
  //  End: Input files
  //  --- --- --- --- --- ---

  //  --- --- --- --- --- ---
  //  Framing data & output definition
  /***
   * @todo Add FIFO to the config file (2024-2023 have FIFO 24 the triggers.)
   * @todo Test single/multiple core behaviour is consistent
   * @todo Add a plot to evaluate how many consecutive hits are flagged as afterpulse
   * @todo Re-structureand re-evaluate needs for the QA
   * @todo Config files from outside
   */
  //  Create streaming framer
  parallel_streaming_framer framer(filenames, "conf/trigger_setup.txt", "conf/readout_config.txt");
  framer.set_parallel_cores(requested_n_threads);
  auto config_triggers = trigger_conf_reader("conf/trigger_setup.txt");
  trigger_registry registry(config_triggers);
  //  Prepare output file & tree
  std::string outfile_name = data_repository + "/" + run_name + "/lightdata.root";
  if (std::filesystem::exists(outfile_name) && !force_lightdata_rebuild)
  {
    logger::log_info("[INFO] Output file already exists, skipping: " + outfile_name);
    return;
  }
  TFile *outfile = TFile::Open(outfile_name.c_str(), "RECREATE");
  auto &spilldata = framer.get_spilldata_link();
  TTree *lightdata_tree = new TTree("lightdata", "Lightdata tree");
  spilldata.write_to_tree(lightdata_tree);
  //  ---
  //  QA Plots
  //  ---
  //  --- Triggers
  std::unordered_map<int, TH1F *> h_trigger_frame_population;
  std::unordered_map<int, TH1F *> h_trigger_time_diff_w_cherenkov;
  TH2F *h2_trigger_matrix = new TH2F(
      "h2_trigger_matrix", "Trigger coincidence matrix;;",
      registry.size(), -0.5, registry.size() - 0.5,
      registry.size(), -0.5, registry.size() - 0.5);
  registry.label_axes(h2_trigger_matrix);
  //  ---
  //  --- Hit Number
  TH1F *h_hits_per_frame_sig = new TH1F("h_hits_per_frame_sig", ";;", 500, 0, 500);
  TH1F *h_hits_per_frame_bkg = new TH1F("h_hits_per_frame_bkg", ";;", 500, 0, 500);
  //  --- --- --- V Revise V
  TH2F *h_timing_hit_map = new TH2F("h_timing_hit_map", ";channels on chip 0; channels on chip 1", 33, 0, 33, 33, 0, 33);
  TH1F *h_timing_ref_per_frame = new TH1F("h_timing_ref_per_frame", ";frame ID;Timing trigger", 2000, 0, 2000000);
  TH1F *h_timing_ref_delta = new TH1F("h_timing_ref_delta", ";timing chip 0 - timing chip 1", 200, -10, 10);
  TH1F *h_timing_ref_delta_sel = new TH1F("h_timing_ref_delta_sel", ";timing chip 0 - timing chip 1", 200, -10, 10);
  TH1F *h_timing_res = new TH1F("h_timing_res", "", 2000, 0, 2000000);
  TH1F *h_delta_time_trigger_0_timing = new TH1F("h_delta_time_trigger_0_timing", ";#Delta t (ns)", 100, -50, 50);
  //  ---
  //  End: Framing data & output definition
  //  --- --- --- --- --- ---

  //  --- --- --- --- --- ---
  //  Loop on data streamers
  //  ---
  //  Loop over spills
  int n_spills = 0, n_frames = 0;
  if (max_spill != 1000)
    std::cout << "[INFO] Requested to stop at spill : " << max_spill << std::endl;
  for (int ispill = 0; ispill < max_spill && framer.next_spill(); ++ispill)
  {
    std::cout << "\33[2K\r[INFO] Spill " << ispill << std::flush;
    for (auto &[frame_id, current_lightdata_struct] : spilldata.get_frame_link())
    {
      //  Link locally hits structure
      auto &triggers_in_frame = spilldata.get_frame_trigger_hits(frame_id);
      auto &timing_hits = spilldata.get_frame_timing_hits(frame_id);
      auto &cherenkov_hits = spilldata.get_frame_cherenkov_hits(frame_id);

      //  Fill timing trigger information
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

        //  TODO: understand why there is a difference with the lightdata result > What does that even mean? > probably obsolete... keep it just in case
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

      // -------------------------------------------------------------------------
      // Cherenkov sliding window trigger
      // -------------------------------------------------------------------------
      //  TODO: make it an external repo with template structure

      //  Utility structures
      std::vector<std::pair<int, float>> current_hit_fifo;
      std::vector<std::pair<int, float>> previous_hit_fifo;
      float clock_cycles = 40.f;
      //  TODO: make the threshold dependent on participants channels (?)
      int hit_threshold = 7;

      //  Build an alcor_finedata vector and sort the hits
      std::vector<alcor_finedata> cherenkov_finedata_hits;
      for (auto current_cherenkov_hit_struct : cherenkov_hits)
        cherenkov_finedata_hits.emplace_back(current_cherenkov_hit_struct);
      std::sort(cherenkov_finedata_hits.begin(), cherenkov_finedata_hits.end());

      //  Loop over cherenkov hits
      int current_hit = -1;
      int current_hit_count = 0;
      int previous_hit_count = 0;
      for (auto current_cherenkov_hit : cherenkov_finedata_hits)
      {
        //  Next hit, get reference hit time and add to the streaming_trigger
        current_hit++;
        float current_hit_time = current_cherenkov_hit.get_time();
        current_hit_fifo.push_back({current_hit, current_hit_time});

        //  Remove hits than clock_cycles in the search array
        current_hit_fifo.erase(
            std::remove_if(current_hit_fifo.begin(),
                           current_hit_fifo.end(),
                           [&](const std::pair<int, float> &elem)
                           {
                             return (current_hit_time - elem.second) > clock_cycles;
                           }),
            current_hit_fifo.end());

        //  Check the nuber of hits to trigger
        current_hit_count = current_hit_fifo.size();
        if ((previous_hit_count >= hit_threshold) && (current_hit_count < previous_hit_count))
        {
          float trigger_time = 0.;
          for (auto i_ter : previous_hit_fifo)
            trigger_time += i_ter.second / previous_hit_count;
          spilldata.add_trigger_to_frame(frame_id, {static_cast<uint8_t>(_TRIGGER_STREAMING_RING_FOUND_), static_cast<uint16_t>(previous_hit_count), static_cast<float>(trigger_time * _ALCOR_CC_TO_NS_)});
        }
        previous_hit_fifo = current_hit_fifo;
        previous_hit_count = current_hit_count;
      }

      if (!spilldata.has_trigger(frame_id))
      {
        spilldata.do_not_write_frame(frame_id);
      }
      //  This frame will be saved, we perform saved frames QA
      else
      {
        for (auto current_trigger : triggers_in_frame)
        {
          if (!h_trigger_frame_population.count(current_trigger.index))
          {
            h_trigger_frame_population[current_trigger.index] = new TH1F(Form("h_trigger_frame_population_%s", registry.name_of(current_trigger.index).c_str()), ";frame number; trigger;", 5e3, 0, 5e6);
            h_trigger_time_diff_w_cherenkov[current_trigger.index] = new TH1F(Form("h_trigger_time_diff_w_cherenkov_%s", registry.name_of(current_trigger.index).c_str()), ";;", 5e3, -500, 500);
          }
          //  Frame distribution of the trigger
          h_trigger_frame_population[current_trigger.index]->Fill(frame_id);
          //  Time difference of trigger with cherenkov hits
          auto trigger_104_hits_signal = 0;
          auto trigger_104_hits_background = 0;
          for (auto current_cherenkov_hit_struct : cherenkov_hits)
          {
            alcor_finedata current_hit(current_cherenkov_hit_struct);
            if (!current_hit.is_afterpulse())
            {
              h_trigger_time_diff_w_cherenkov[current_trigger.index]->Fill(current_hit.get_time_ns() - current_trigger.fine_time);
              if (fabs(current_hit.get_time_ns() - current_trigger.fine_time) < 20 && current_trigger.index == 104)
                trigger_104_hits_signal++;
              if (fabs(current_hit.get_time_ns() - current_trigger.fine_time - 60) < 20 && current_trigger.index == 104)
                trigger_104_hits_background++;
            }
          }
          h_hits_per_frame_sig->Fill(trigger_104_hits_signal);
          h_hits_per_frame_bkg->Fill(trigger_104_hits_background);
        }
        // Collect unique trigger types in this frame
        std::set<int> fired_trigger_types;
        for (auto &t : triggers_in_frame)
          fired_trigger_types.insert(
              registry.index_of(static_cast<trigger_number>(t.index)));
        // Fill matrix — each pair filled exactly once
        for (auto i : fired_trigger_types)
          for (auto j : fired_trigger_types)
            h2_trigger_matrix->Fill(i, j);
      }
    }
    outfile->cd();
    spilldata.prepare_tree_fill();
    lightdata_tree->Fill();
    outfile->Flush();
  }
  std::cout << "\r[INFO] Finished spills loop, writing to file" << std::endl;
  //  ---
  //  End: Loop on data streamers
  //  --- --- --- --- --- ---

  //  --- --- --- --- --- ---
  //  QA plots
  //  ---
  outfile->cd();
  lightdata_tree->Write();
  h_hits_per_frame_sig->Write();
  h_hits_per_frame_bkg->Write();
  //  ---
  //  --- Trigger QA
  TDirectory *trigger_dir = outfile->mkdir("Triggers");
  trigger_dir->cd();
  h2_trigger_matrix->Write();
  for (auto [key, val] : h_trigger_frame_population)
    val->Write();
  for (auto [key, val] : h_trigger_time_diff_w_cherenkov)
  {
    val->Scale(1. / h2_trigger_matrix->GetBinContent(registry.index_of(key) + 1, registry.index_of(key) + 1));
    val->Write();
  }
  //  ---
  //  --- Other QA
  //  ---
  //  --- Close file
  outfile->Close();
  //  ---
  //  End: QA plots
  //  --- --- --- --- --- ---
}
