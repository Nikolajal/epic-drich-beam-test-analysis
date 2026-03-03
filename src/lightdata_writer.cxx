#include "parallel_streaming_framer.h"
#include "streaming_framer.h"
#include "lightdata_writer.h"

//  TODO: make the CLI multithred flag
//  TODO: cache mapping positions
void lightdata_writer(
    const std::string &data_repository,
    const std::string &run_name,
    int max_spill)
{
  //  Do not make ownership of histograms to current directory
  TH1::AddDirectory(false);

  //  Input files
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

  //  Create streaming framer
  //  TODO: Add FIFO to the config file (2024-2023 have FIFO 24 the triggers.)
  parallel_streaming_framer framer(filenames, "conf/trigger_setup.txt", "conf/readout_config.txt");

  //  TODO: Set this from outside
  //  TODO: Check if one core has a good behaviour, especially in merging: possibility to lock writing in the same place to avoid merging
  //  TODO: Add a plot to evaluate how many consecutive hits are flagged as afterpulse
  //  framer.set_parallel_cores(1);

  //  Prepare output tree
  TFile *outfile = TFile::Open((data_repository + "/" + run_name + "/lightdata.root").c_str(), "RECREATE");
  auto &spilldata = framer.get_spilldata_link();
  TTree *lightdata_tree = new TTree("lightdata", "Lightdata tree");
  spilldata.write_to_tree(lightdata_tree);

  //  QA Plots
  /*
  TODO: re-structure the QA
  */
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

      //  Link locally hits structure
      auto &triggers_in_frame = current_lightdata.get_triggers_link();
      auto &timing_hits = current_lightdata.get_timing_hits_link();
      auto &cherenkov_hits = current_lightdata.get_cherenkov_hits_link();

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
      float clock_cycles = 10.f;
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
