#include "parallel_streaming_framer.h"
#include "lightdata_writer.h"
#include "mapping.h"
#include <mist/ring_finding/hough_transform.h>

// Config (to be sourced from readout_config_file)
constexpr int kTimingChip0Id = 0, kTimingChip1Id = 2;
constexpr int kTimingChip0Expected = 32, kTimingChip1Expected = 31;
constexpr float kDeltaTimingCenter = -0.5f;
constexpr float kDeltaTimingWindow = 0.5f;
constexpr float kDeltaTimingNSigma = 3.0f;

void lightdata_writer(
    const std::string &data_repository,
    const std::string &run_name,
    int max_spill,
    bool force_lightdata_rebuild,
    int requested_n_threads,
    std::string trigger_setup_file,
    std::string readout_config_file,
    std::string mapping_config_file,
    std::string fine_calibration_config_file)
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
        mist::logger::error("(lightdata_writer) Data folder does not exist, abort");
        return;
    }
    //  Check the given folder is actually a directory
    if (!std::filesystem::is_directory(base_dir))
    {
        mist::logger::error("(lightdata_writer) Data folder is not a folder, abort");
        return;
    }
    //  Check the given folder is not empty
    if (std::filesystem::is_empty(base_dir))
    {
        mist::logger::error("(lightdata_writer) Data folder is empty, abort");
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
            mist::logger::warning(Form("(lightdata_writer) Data folder for device %s do not have decoded data, skipping", device_name.c_str()));
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
    mist::logger::info("[INFO] Found devices with files: ");
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
   * @todo Re-structure and re-evaluate needs for the QA
   * @todo Config files from outside
   */
    //  Create progress tracking —
    //  Two subtasks: one for the streaming framer, one for per-frame post-processing.
    //  The framer subtask is wired directly into the framer via assign_bar() so it
    //  updates automatically during next_spill(). The post-processing subtask is
    //  driven manually inside the frame loop below.
    mist::logger::progress_bar progress_framer("framer         ", mist::logger::bar_style::BLOCK);
    mist::logger::progress_bar progress_postprocessing("post-processing", mist::logger::bar_style::BLOCK);
    mist::logger::update("spill_counter  ", "Current spill: " +
                                                std::to_string(0) +
                                                " - framing raw data");
    int streaming_trigger = 0;

    //  Create streaming framer
    parallel_streaming_framer framer(filenames, trigger_setup_file, readout_config_file);
    framer.set_parallel_cores(requested_n_threads);
    framer.assign_bar(progress_framer); // framer drives progress_framer automatically

    auto config_triggers = trigger_conf_reader(trigger_setup_file);
    trigger_registry registry(config_triggers);
    //  Prepare output file & tree
    std::string outfile_name = data_repository + "/" + run_name + "/lightdata.root";
    if (std::filesystem::exists(outfile_name) && !force_lightdata_rebuild)
    {
        mist::logger::info("[INFO] Output file already exists, skipping: " + outfile_name);
        return;
    }
    //  Generate mapping
    mapping current_mapping(mapping_config_file);
    //  Load fine_calibration
    alcor_finedata::read_calib_from_file(fine_calibration_config_file);
    //  Link output tree
    TFile *outfile = TFile::Open(outfile_name.c_str(), "RECREATE");
    auto &spilldata = framer.get_spilldata_link();
    TTree *lightdata_tree = new TTree("lightdata", "Lightdata tree");
    spilldata.write_to_tree(lightdata_tree);
    //  ---
    //  QA Plots
    //  ---
    //  --- Triggers
    TH2F *h2_trigger_matrix = new TH2F(
        "h2_trigger_matrix", "Trigger coincidence matrix;;",
        registry.size(), -0.5, registry.size() - 0.5,
        registry.size(), -0.5, registry.size() - 0.5);
    registry.label_axes(h2_trigger_matrix);
    std::unordered_map<int, TH1F *> h_trigger_frame_population;
    std::unordered_map<int, TH1F *> h_trigger_time_diff_w_cherenkov;
    std::unordered_map<int, TH2F *> h_trigger_full_hitmap;
    std::unordered_map<int, std::array<TH1F *, 2>> h_trigger_hit_multiplicity;
    //  ---
    //  --- Timing
    TH2F *h_timing_hit_map = new TH2F("h_timing_hit_map", ";channels on chip 0; channels on chip 1", 33, 0, 33, 33, 0, 33);
    TH1F *h_timing_ref_delta = new TH1F("h_timing_ref_delta", ";timing chip 0 - timing chip 1", 250, -5, 5);
    TH1F *h_timing_ref_delta_sel = new TH1F("h_timing_ref_delta_sel", ";timing chip 0 - timing chip 1", 250, -5, 5);
    //  ---
    //  --- DCR
    TProfile *h_dcr_per_channel = new TProfile("h_dcr_per_channel", ";channel;DCR [kHz];", 2048, 0, 2048);
    //  ---
    //  --- Streaming Trigger
    TH2F *h_streaming_trigger_frames_examples = new TH2F("h_streaming_trigger_frames_examples", ";x (mm);y (mm)", 1000, 0, 1000, _FRAME_SIZE_, 0, _FRAME_SIZE_);
    TH2F *h_streaming_trigger_full_hitmap = new TH2F("h_streaming_trigger_full_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH2F *h_streaming_trigger_time_cut_hitmap = new TH2F("h_streaming_trigger_time_cut_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH2F *h_streaming_trigger_ring_finder_hitmap = new TH2F("h_streaming_trigger_ring_finder_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH1F *h_streaming_trigger_ring_finder_nrings = new TH1F("h_streaming_trigger_ring_finder_nrings", ";timing chip 0 - timing chip 1", 3, -.5, 2.5);
    TH2F *h_streaming_trigger_ring_finder_first_hitmap = new TH2F("h_streaming_trigger_ring_finder_first_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH2F *h_streaming_trigger_ring_finder_second_hitmap = new TH2F("h_streaming_trigger_ring_finder_second_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH1F *h_streaming_trigger_center_X_first = new TH1F("h_streaming_trigger_center_X_first", ";x (mm)", 100, -25, 25);
    TH1F *h_streaming_trigger_center_Y_first = new TH1F("h_streaming_trigger_center_Y_first", ";y (mm)", 100, -25, 25);
    TH1F *h_streaming_trigger_center_R_first = new TH1F("h_streaming_trigger_center_R_first", ";R (mm)", 200, 20, 120);
    TH1F *h_streaming_trigger_center_X_second = new TH1F("h_streaming_trigger_center_X_second", ";x (mm)", 100, -25, 25);
    TH1F *h_streaming_trigger_center_Y_second = new TH1F("h_streaming_trigger_center_Y_second", ";y (mm)", 100, -25, 25);
    TH1F *h_streaming_trigger_center_R_second = new TH1F("h_streaming_trigger_center_R_second", ";R (mm)", 200, 20, 120);
    const float time_window_ns = 5.f;
    TH1F *h_streaming_trigger_delta_t_leading_edge = new TH1F("h_streaming_trigger_delta_t_leading_edge", ";#Delta_{t} (t_{hit} - t_{avg}) ns", 1000 * (time_window_ns), 0., time_window_ns);
    TH1F *h_streaming_trigger_delta_t_half_centroid = new TH1F("h_streaming_trigger_delta_t_half_centroid", ";#Delta_{t} (t_{hit} - t_{avg}) ns", 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH1F *h_streaming_trigger_delta_t_first_half = new TH1F("h_streaming_trigger_delta_t_first_half", ";#Delta_{t} (t_{hit} - t_{avg}) ns", 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH1F *h_streaming_trigger_delta_t_second_half = new TH1F("h_streaming_trigger_delta_t_second_half", ";#Delta_{t} (t_{hit} - t_{avg}) ns", 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH2F *h_streaming_trigger_sigma_vs_nhits = new TH2F("h_streaming_trigger_sigma_vs_nhits", ";n_{hits} in peak window;|odd_median - even_median| ns", 50, 0, 50, 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH2F *h_streaming_trigger_median_vs_window = new TH2F("h_delta_median_vs_window", ";t_{last} - t_{first} in peak window ns;|odd_median - even_median| ns", 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns, 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH1F *h_streaming_trigger_tdc_step_sizes = new TH1F("h_streaming_trigger_tdc_step_sizes", ";t_{i+1} - t_{i} ns;entries", 1000, 0., 1.);
    TH1F *h_streaming_trigger_tdc_zero_times = new TH1F("h_streaming_trigger_tdc_zero_times", ";t_{hit} ns;entries", 10000, 0., _FRAME_LENGTH_NS_);
    TH1F *h_streaming_trigger_tdc_zero_cluster_size = new TH1F("h_streaming_trigger_tdc_zero_cluster_size", ";n_{hits} in peak;entries", 50, 0., 50.);
    //  ---
    //  End: Framing data & output definition
    //  --- --- --- --- --- ---

    //  --- --- --- --- --- ---
    //  Loop on data
    //  ---
    //  Cache positions
    std::map<int, std::array<float, 2>> index_to_hit_xy;
    std::map<std::array<float, 2>, int> hit_to_index_xy;
    for (auto i_index = 0; i_index < 2048 * 4; i_index += 4)
    {
        auto position = current_mapping.get_position_from_global_index(i_index);
        if (!position)
            continue;
        if (fabs((*position)[0]) < 5 && fabs((*position)[1]) < 5)
            continue;
        index_to_hit_xy[i_index] = (*position);
        hit_to_index_xy[(*position)] = i_index;
    }
    mist::ring_finding::hough_transform ring_finder(index_to_hit_xy, 20, 120, 1., 3.);

    //  ---
    //  Loop over spills
    if (max_spill != 1000)
        mist::logger::info("(parallel_streaming_framer::next_spill) Requested to stop at spill : " +
                           std::to_string(max_spill));

    for (int ispill = 0; ispill < max_spill && framer.next_spill(); ++ispill)
    {
        //  Signal current spill
        mist::logger::update("spill_counter  ", "Current spill: " +
                                                    std::to_string(ispill) +
                                                    " - Requested: " +
                                                    std::to_string(max_spill) +
                                                    " (may be less)");

        //  Generate the calibration at each spill if new channels get available
        //spilldata.update_calibration(framer.get_fine_tune_distribution());

        //  Calculate participants channel
        std::set<uint32_t> active_sensors;
        std::unordered_map<uint32_t, uint16_t> active_sensors_count;
        auto lanes_participating = spilldata.get_not_dead_participants();
        int n_active_cherenkov_channels = 0;
        for (auto [device, lanes] : lanes_participating)
            if (device < 200)
                for (auto current_lane : lanes)
                    if (current_lane)
                        for (auto i_channel = 0; i_channel < 8; ++i_channel)
                            active_sensors.insert(get_global_index(device, current_lane / 4, 8 * (current_lane % 4) + i_channel, 0) / 4); //  Global channel index

        n_active_cherenkov_channels = active_sensors.size();

        //  Streaming trigger utilities
        const float cherenkov_fraction = 0.004f;
        const int threshold = 5; //std::max(1, static_cast<int>(std::ceil(cherenkov_fraction * n_active_cherenkov_channels)));
        std::vector<std::pair<int, float>> carry_over_hits;

        //  Info
        mist::logger::info("(lightdata_writer) Spill " +
                           std::to_string(ispill) +
                           " has " +
                           std::to_string(n_active_cherenkov_channels) +
                           " active channels");

        //  Timing calibration at first spill
        if (ispill == 0 && false)
        {
            std::unordered_map<int, float> tdc_offset_sum_0, tdc_offset_sum_1;
            std::unordered_map<int, int> tdc_offset_count_0, tdc_offset_count_1;
            for (auto &[frame_id, current_lightdata_struct] : spilldata.get_frame_link())
            {
                //  Link locally hits structure
                auto &cherenkov_hits = spilldata.get_frame_cherenkov_hits(frame_id);
                auto &timing_hits = spilldata.get_frame_timing_hits(frame_id);
                auto &triggers_in_frame = spilldata.get_frame_trigger_hits(frame_id);

                //  ----    ----    ----    Timing hits  ----    ----    ----
                std::unordered_map<int, float> tdc_time_0, tdc_time_1;
                std::unordered_set<int> seen_channels_0, seen_channels_1;

                for (const auto &raw_hit : timing_hits)
                {
                    alcor_finedata hit(raw_hit);
                    const int chip = hit.get_chip();
                    const int global_index = hit.get_global_index();
                    const int channel = hit.get_global_channel_index();
                    const float time_cc = hit.get_time();

                    if (chip == kTimingChip0Id && !tdc_time_0.count(global_index))
                    {
                        tdc_time_0[global_index] = time_cc;
                        seen_channels_0.insert(channel);
                    }
                    if (chip == kTimingChip1Id && !tdc_time_1.count(global_index))
                    {
                        tdc_time_1[global_index] = time_cc;
                        seen_channels_1.insert(channel);
                    }
                }

                // Only process full-occupancy events
                if (seen_channels_0.size() == static_cast<size_t>(kTimingChip0Expected))
                {
                    float mean_0 = 0.f;
                    for (auto &[gi, time] : tdc_time_0)
                        mean_0 += time;
                    mean_0 /= tdc_time_0.size();

                    for (auto &[gi, time] : tdc_time_0)
                    {
                        const float mean_others = (mean_0 * tdc_time_0.size() - time) / (tdc_time_0.size() - 1);
                        tdc_offset_sum_0[gi] += (time - mean_others);
                        tdc_offset_count_0[gi]++;
                    }
                }
                if (seen_channels_1.size() == static_cast<size_t>(kTimingChip1Expected))
                {
                    float mean_1 = 0.f;
                    for (auto &[gi, time] : tdc_time_1)
                        mean_1 += time;
                    mean_1 /= tdc_time_1.size();

                    for (auto &[gi, time] : tdc_time_1)
                    {
                        const float mean_others = (mean_1 * tdc_time_1.size() - time) / (tdc_time_1.size() - 1);
                        tdc_offset_sum_1[gi] += (time - mean_others);
                        tdc_offset_count_1[gi]++;
                    }
                }
            }

            // Apply — offset is already in clock cycles, sign corrected for get_phase()
            for (auto &[gi, sum] : tdc_offset_sum_0)
                alcor_finedata::set_param2(gi, -(sum / tdc_offset_count_0[gi]));
            for (auto &[gi, sum] : tdc_offset_sum_1)
                alcor_finedata::set_param2(gi, -(sum / tdc_offset_count_1[gi]));

            //  Add the plot to dinamically determine the timing cuts > determine the highest bin excluding zeros, delta times without any further check look for
        }

        mist::logger::info("(lightdata_writer) Starting processing data streams in frames");
        const auto total_frames = static_cast<int>(spilldata.get_frame_link().size());
        for (auto &[frame_id, current_lightdata_struct] : spilldata.get_frame_link())
        {
            //  Update post-processing subtask bar periodically to avoid render overhead
            if (frame_id % 100000 == 0)
                progress_postprocessing.update(static_cast<int>(frame_id), total_frames);

            //  Link locally hits structure
            auto &cherenkov_hits = spilldata.get_frame_cherenkov_hits(frame_id);
            auto &timing_hits = spilldata.get_frame_timing_hits(frame_id);
            auto &triggers_in_frame = spilldata.get_frame_trigger_hits(frame_id);

            //  ----    ----    ----    Cherenkov hits  ----    ----    ----
            for (auto &current_cherenkov_hit_struct : cherenkov_hits)
                current_mapping.assign_position(current_cherenkov_hit_struct);

            //  ----    ----    ----    Timing hits  ----    ----    ----
            //  Utilities
            int timing_hits_0 = 0, timing_hits_1 = 0;
            float timing_sum_0 = 0.f, timing_sum_1 = 0.f;
            std::unordered_set<int> seen_channels_0, seen_channels_1;

            //  Loop over timing hits
            for (const auto &raw_hit : timing_hits)
            {
                alcor_finedata hit(raw_hit);
                const int chip = hit.get_chip();
                const int channel = hit.get_global_channel_index();
                const float time_ns = hit.get_time_ns();

                if (chip == kTimingChip0Id && seen_channels_0.insert(channel).second)
                {
                    ++timing_hits_0;
                    timing_sum_0 += time_ns;
                }
                if (chip == kTimingChip1Id && seen_channels_1.insert(channel).second)
                {
                    ++timing_hits_1;
                    timing_sum_1 += time_ns;
                }
            }

            //  Excluding events with no timing
            if (timing_hits_0 > 0 && timing_hits_1 > 0)
            {
                //  Fill occupancy matrix
                h_timing_hit_map->Fill(timing_hits_0, timing_hits_1);

                const float mean0 = timing_sum_0 / timing_hits_0;
                const float mean1 = timing_sum_1 / timing_hits_1;
                const float ref_timing = (mean0 + mean1) / 2.f;
                const float delta_timing = mean1 - mean0;

                h_timing_ref_delta->Fill(delta_timing);

                const bool timing_available =
                    (timing_hits_0 == kTimingChip0Expected) &&
                    (timing_hits_1 == kTimingChip1Expected) &&
                    (std::fabs(delta_timing - kDeltaTimingCenter) < kDeltaTimingNSigma * kDeltaTimingWindow);

                if (timing_available)
                {
                    h_timing_ref_delta_sel->Fill(delta_timing);
                    spilldata.add_trigger_to_frame(frame_id, {static_cast<uint8_t>(_TRIGGER_TIMING_),
                                                              static_cast<uint16_t>(_FRAME_SIZE_ / 2),
                                                              ref_timing});
                }
            }

            //  --- Cherenkov sliding window trigger
            run_streaming_trigger(spilldata,
                                  frame_id,
                                  time_window_ns,
                                  threshold,
                                  carry_over_hits,
                                  h_streaming_trigger_delta_t_leading_edge,
                                  h_streaming_trigger_delta_t_half_centroid,
                                  h_streaming_trigger_delta_t_first_half,
                                  h_streaming_trigger_delta_t_second_half,
                                  h_streaming_trigger_sigma_vs_nhits,
                                  h_streaming_trigger_median_vs_window,
                                  h_streaming_trigger_tdc_step_sizes,
                                  h_streaming_trigger_tdc_zero_times,
                                  h_streaming_trigger_tdc_zero_cluster_size);

            //  ---
            if (!spilldata.has_trigger(frame_id))
            {
                spilldata.do_not_write_frame(frame_id);
            }
            //  ---
            //  --- This frame will be saved, we perform saved frames QA
            else
            {
                //  ---
                //  --- Utility for QA
                //  None

                //  ---
                //  --- Streaming Trigger
                //  Loop on all triggers
                std::vector<trigger_event> hough_triggers;
                for (auto current_trigger : triggers_in_frame)
                    if (current_trigger.index == _TRIGGER_STREAMING_RING_FOUND_)
                    {
                        auto index = -1;
                        streaming_trigger++;
                        std::vector<alcor_finedata> ring_candidates;
                        std::vector<int> ring_candidates_index;
                        for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
                        {
                            index++;
                            alcor_finedata current_hit(current_cherenkov_hit_struct);
                            if (current_hit.is_afterpulse())
                                continue;

                            if (!ispill && streaming_trigger <= 1000)
                                h_streaming_trigger_frames_examples->Fill(streaming_trigger - 1, current_hit.get_time());
                            if (current_hit.has_mask_bit(_HITMASK_streaming_ring_trigger_))
                                h_streaming_trigger_full_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                            if (fabs(current_hit.get_time_ns() - current_trigger.fine_time) < 10.)
                            {
                                h_streaming_trigger_time_cut_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                                ring_candidates.push_back(current_hit);
                                ring_candidates_index.push_back(index);
                            }
                        }
                        auto found_rings = alcor_finedata::alcor_find_rings_hough(ring_finder, ring_candidates, 0.33, threshold * 0.75, threshold, 2, 7.5);
                        h_streaming_trigger_ring_finder_nrings->Fill(found_rings.size());
                        index = -1;
                        std::array<int, 2> hough_trigger_hits = {0, 0};
                        std::array<float, 2> hough_trigger_time = {0.f, 0.f};
                        std::vector<std::array<float, 2>> hough_triggered_first;
                        std::vector<std::array<float, 2>> hough_triggered_second;
                        for (auto current_hit : ring_candidates)
                        {
                            index++;
                            if (current_hit.has_mask_bit(_HITMASK_hough_ring_tag_first) || current_hit.has_mask_bit(_HITMASK_hough_ring_tag_second))
                                h_streaming_trigger_ring_finder_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                            if (current_hit.has_mask_bit(_HITMASK_hough_ring_tag_first))
                            {
                                h_streaming_trigger_ring_finder_first_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                                hough_trigger_hits[0]++;
                                hough_trigger_time[0] += current_hit.get_time_ns();
                                hough_triggered_first.push_back({current_hit.get_hit_x(), current_hit.get_hit_y()});
                            }
                            if (current_hit.has_mask_bit(_HITMASK_hough_ring_tag_second))
                            {
                                h_streaming_trigger_ring_finder_second_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                                hough_trigger_hits[1]++;
                                hough_trigger_time[1] += current_hit.get_time_ns();
                                hough_triggered_second.push_back({current_hit.get_hit_x(), current_hit.get_hit_y()});
                            }
                            cherenkov_hits[ring_candidates_index[index]].hit_mask = current_hit.get_mask();
                        }

                        if (found_rings.size() > 0)
                        {
                            hough_triggers.emplace_back(static_cast<uint8_t>(_TRIGGER_HOUGH_RING_FOUND_),
                                                        static_cast<uint16_t>(found_rings.size()),
                                                        static_cast<float>(hough_trigger_time[0] / hough_trigger_hits[0]));

                            auto fit_circle_result = fit_circle(hough_triggered_first, {0., 0., 50.}, false);
                            h_streaming_trigger_center_X_first->Fill(fit_circle_result[0][0]);
                            h_streaming_trigger_center_Y_first->Fill(fit_circle_result[1][0]);
                            h_streaming_trigger_center_R_first->Fill(fit_circle_result[2][0]);
                        }
                        if (found_rings.size() > 1)
                        {
                            hough_triggers.emplace_back(static_cast<uint8_t>(_TRIGGER_HOUGH_RING_FOUND_),
                                                        static_cast<uint16_t>(found_rings.size()),
                                                        static_cast<float>(hough_trigger_time[1] / hough_trigger_hits[1]));

                            auto fit_circle_result = fit_circle(hough_triggered_second, {0., 0., 50.}, false);
                            h_streaming_trigger_center_X_second->Fill(fit_circle_result[0][0]);
                            h_streaming_trigger_center_Y_second->Fill(fit_circle_result[1][0]);
                            h_streaming_trigger_center_R_second->Fill(fit_circle_result[2][0]);
                        }
                    }

                for (auto &t : hough_triggers)
                    spilldata.add_trigger_to_frame(frame_id, t);

                //  ---
                //  --- Trigger QA
                // Collect unique trigger types in this frame
                std::set<int> fired_trigger_types;
                for (auto &t : triggers_in_frame)
                    fired_trigger_types.insert(
                        registry.index_of(static_cast<trigger_number>(t.index)));
                // Fill matrix — each pair filled exactly once
                for (auto i : fired_trigger_types)
                    for (auto j : fired_trigger_types)
                        h2_trigger_matrix->Fill(i, j);
                //  Loop on all triggers
                for (auto current_trigger : triggers_in_frame)
                {
                    if (!h_trigger_frame_population.count(current_trigger.index))
                    {
                        h_trigger_frame_population[current_trigger.index] = new TH1F(Form("h_trigger_frame_population_%s", registry.name_of(current_trigger.index).c_str()), Form(";frame number; %s;", registry.name_of(current_trigger.index).c_str()), 5e3, 0, 5e6);
                        h_trigger_time_diff_w_cherenkov[current_trigger.index] = new TH1F(Form("h_trigger_time_diff_w_cherenkov_%s", registry.name_of(current_trigger.index).c_str()), ";#Delta_{t} (t_{hit} - t_{trigger}) ns;Normalised entries", 5e3, -500, 500);
                        h_trigger_full_hitmap[current_trigger.index] = new TH2F(Form("h_trigger_full_hitmap_%s", registry.name_of(current_trigger.index).c_str()), ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
                        h_trigger_hit_multiplicity[current_trigger.index][0] = new TH1F(Form("h_trigger_hit_multiplicity_in_time_%s", registry.name_of(current_trigger.index).c_str()), ";n_{hit}; events;", 100, 0, 100);
                        h_trigger_hit_multiplicity[current_trigger.index][1] = new TH1F(Form("h_trigger_hit_multiplicity_out_of_time_%s", registry.name_of(current_trigger.index).c_str()), ";n_{hit}; events;", 100, 0, 100);
                    }

                    //  Frame distribution of the trigger
                    h_trigger_frame_population[current_trigger.index]->Fill(frame_id);
                    //  Time difference of trigger with cherenkov hits
                    std::array<int, 2> hit_counter = {0, 0};
                    auto window_size = (current_trigger.index == _TRIGGER_HOUGH_RING_FOUND_) || (current_trigger.index == _TRIGGER_STREAMING_RING_FOUND_) ? time_window_ns : 50.;
                    for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
                    {
                        alcor_finedata current_hit(current_cherenkov_hit_struct);
                        if (!current_hit.is_afterpulse())
                        {
                            auto current_delta_time = current_hit.get_time_ns() - current_trigger.fine_time;
                            h_trigger_time_diff_w_cherenkov[current_trigger.index]->Fill(current_delta_time);
                            if (fabs(current_delta_time) < window_size)
                            {
                                hit_counter[0]++;
                                h_trigger_full_hitmap[current_trigger.index]->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                            }
                            else if (fabs(current_delta_time + 100) < window_size)
                            {
                                hit_counter[1]++;
                            }
                        }
                    }
                    h_trigger_hit_multiplicity[current_trigger.index][0]->Fill(hit_counter[0]);
                    h_trigger_hit_multiplicity[current_trigger.index][1]->Fill(hit_counter[1]);
                }
                //  ---
                //  --- DCR QA
                if (fired_trigger_types.count(registry.index_of(static_cast<trigger_number>(_TRIGGER_FIRST_FRAMES_))))
                {
                    //  Channel by channel hit counting
                    for (const auto &global_index : active_sensors)
                        active_sensors_count[global_index] = 0;
                    for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
                        active_sensors_count[(current_cherenkov_hit_struct.global_index / 4)]++;
                    //  Fill the DCR histogram
                    for (auto &[global_index, count] : active_sensors_count)
                        h_dcr_per_channel->Fill(global_index, count);
                }
            }
        }
        progress_postprocessing.update(1, 1);

        //  Signal current spill
        mist::logger::update("spill_counter  ", "Current spill: " +
                                                    std::to_string(ispill + 1) +
                                                    " - framing raw data");

        outfile->cd();
        spilldata.prepare_tree_fill();
        lightdata_tree->Fill();
        outfile->Flush();
    }
    // All spills done
    progress_framer.finish();
    progress_postprocessing.finish();
    mist::logger::end_update("spill_counter  ");
    mist::logger::info("(lightdata_writer) Finished spills loop, writing to file");

    //  ---
    //  End: Loop on data streamers
    //  --- --- --- --- --- ---

    //  --- --- --- --- --- ---
    //  QA plots
    //  ---
    outfile->cd();
    lightdata_tree->Write();
    framer.get_fine_tune_distribution()->Write("h_fine_calib");
    alcor_finedata::write_calib_to_file((base_dir / "fine_calib.txt").string());
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
    for (auto [key, val] : h_trigger_hit_multiplicity)
    {
        val[0]->Write();
        val[1]->Write();
    }
    for (auto [key, val] : h_trigger_full_hitmap)
        val->Write();
    //  ---
    //  --- Timing
    TDirectory *timing_dir = outfile->mkdir("Timing");
    timing_dir->cd();
    h_timing_hit_map->Write();
    h_timing_ref_delta->Write();
    h_timing_ref_delta_sel->Write();
    //  Repeating info for single source for check
    auto timing_index = registry.index_of(static_cast<trigger_number>(_TRIGGER_TIMING_));
    if (h_trigger_frame_population.count(timing_index))
    {
        h_trigger_frame_population[timing_index]->Write();
        h_trigger_time_diff_w_cherenkov[timing_index]->Write();
        h_trigger_full_hitmap[timing_index]->Write();
    }
    //  ---
    //  --- DCR
    TDirectory *DCR_dir = outfile->mkdir("DCR");
    DCR_dir->cd();
    h_dcr_per_channel->Scale(1. / (_FRAME_LENGTH_NS_ * 1.e-6));
    h_dcr_per_channel->Write();
    //  ---
    //  --- Streaming Trigger
    TDirectory *streaming_trigger_dir = outfile->mkdir("Streaming Trigger");
    streaming_trigger_dir->cd();
    auto streaming_ring_index = registry.index_of(static_cast<trigger_number>(_TRIGGER_STREAMING_RING_FOUND_));
    h_streaming_trigger_frames_examples->Write();
    h_streaming_trigger_full_hitmap->Write();
    h_streaming_trigger_time_cut_hitmap->Write();
    h_streaming_trigger_ring_finder_nrings->Write();
    h_streaming_trigger_ring_finder_hitmap->Write();
    h_streaming_trigger_ring_finder_first_hitmap->Write();
    h_streaming_trigger_ring_finder_second_hitmap->Write();
    h_streaming_trigger_center_X_first->Write();
    h_streaming_trigger_center_Y_first->Write();
    h_streaming_trigger_center_R_first->Write();
    h_streaming_trigger_center_X_second->Write();
    h_streaming_trigger_center_Y_second->Write();
    h_streaming_trigger_center_R_second->Write();
    h_streaming_trigger_delta_t_leading_edge->Write();
    h_streaming_trigger_delta_t_half_centroid->Write();
    h_streaming_trigger_sigma_vs_nhits->Write();
    h_streaming_trigger_median_vs_window->Write();
    h_streaming_trigger_tdc_step_sizes->Write();
    h_streaming_trigger_tdc_zero_times->Write();
    h_streaming_trigger_tdc_zero_cluster_size->Write();
    h_streaming_trigger_delta_t_first_half->Write();
    h_streaming_trigger_delta_t_second_half->Write();
    if (h_trigger_frame_population.count(streaming_ring_index))
    {
        h_trigger_frame_population[streaming_ring_index]->Write();
        h_trigger_time_diff_w_cherenkov[streaming_ring_index]->Write();
        h_trigger_full_hitmap[streaming_ring_index]->Write();
    }
    //  ---
    //  --- Close file
    outfile->Close();
    //  ---
    //  End: QA plots
    //  --- --- --- --- --- ---
}

bool run_streaming_trigger(alcor_spilldata &current_spill,
                           int frame_id,
                           const float time_window_ns,
                           const int threshold,
                           std::vector<std::pair<int, float>> &carry_over_hits,
                           TH1F *h_delta_t_leading_edge,
                           TH1F *h_delta_t_half_centroid,
                           TH1F *h_delta_t_half_center_left,
                           TH1F *h_delta_t_half_center_right,
                           TH2F *h_sigma_vs_nhits,
                           TH2F *h_median_vs_window,
                           TH1F *h_tdc_step_sizes,
                           TH1F *h_tdc_zero_times,
                           TH1F *h_tdc_zero_cluster_size)
{
    auto &cherenkov_hits = current_spill.get_frame_cherenkov_hits(frame_id);

    //  Build and sort finedata hits
    std::vector<alcor_finedata> cherenkov_finedata_hits;
    cherenkov_finedata_hits.reserve(cherenkov_hits.size());
    for (const auto &h : cherenkov_hits)
        cherenkov_finedata_hits.emplace_back(h);
    std::sort(cherenkov_finedata_hits.begin(), cherenkov_finedata_hits.end());

    //  Deque window: front = oldest hit, back = newest hit.
    //  Eviction is always from the front (O(1)), insertion always at the back (O(1)).
    //  Each entry is {original_index, time_ns}.
    std::deque<std::pair<int, float>> window;
    for (const auto &entry : carry_over_hits)
        window.push_back(entry);

    //  Snapshot of the window at peak occupancy — used for QA histograms and
    //  trigger time. Stored as a sorted vector of times for median computation.
    std::vector<float> peak_times;

    bool in_cluster = false;
    int peak_count = 0;
    bool has_fired = false;

    //  Helper: median of a sorted vector of floats.
    //  Precondition: times must be sorted, size > 0.
    auto median_of = [](std::vector<float> times) -> float
    {
        //  Already sorted
        //  std::sort(times.begin(), times.end());
        const int n = static_cast<int>(times.size());
        return (n % 2 == 1)
                   ? times[n / 2]
                   : (times[n / 2 - 1] + times[n / 2]) * 0.5f;
    };

    auto end_of_cluster = [&]()
    {
        // --- Leading edge reference
        const float t_leading = peak_times.front(); // already sorted
        for (auto i_ter = 1; i_ter < static_cast<int>(peak_times.size()); i_ter++)
            h_delta_t_leading_edge->Fill(peak_times[i_ter] - t_leading);

        auto clean_times = [](const std::vector<float> &times) -> std::vector<float>
        {
            //  Minimum-IQR recursive outlier removal.
            //  At each step, compare the two candidate ranges obtained by excluding
            //  either the first or last endpoint. Remove the endpoint that creates
            //  the larger gap relative to the inner spread — but ONLY if that gap
            //  exceeds a minimum ratio threshold, meaning it is a genuine outlier.
            //  Recurse until no outlier is found or fewer than 3 hits remain.
            //
            //  kOutlierRatio: the endpoint gap must be at least this many times
            //  larger than the other candidate range to be considered an outlier.
            //  Empirically 2.0 is a reasonable starting point — if the removed
            //  endpoint is a real Cherenkov photon, both ranges are comparable
            //  and no removal occurs.
            constexpr float kOutlierRatio = 2.0f;

            std::vector<float> result = times;
            while (true)
            {
                const int n = static_cast<int>(result.size());
                if (n <= 3)
                    break; // cannot clean further — need at least 3 hits

                const float range_excl_last = result[n - 2] - result[0];  // range excluding last
                const float range_excl_first = result[n - 1] - result[1]; // range excluding first

                if (range_excl_last <= range_excl_first && range_excl_last * kOutlierRatio < range_excl_first)
                    result.pop_back(); // last hit is a genuine outlier
                else if (range_excl_first < range_excl_last && range_excl_first * kOutlierRatio < range_excl_last)
                    result.erase(result.begin()); // first hit is a genuine outlier
                else
                    break; // no outlier found — stop
            }
            return result;
        };
        std::vector<float> cleaned_times = clean_times(peak_times);

        constexpr float kPeakSeparation_ns = 2.0f;
        constexpr float kSigma_ns = 0.3f;
        constexpr float kPopulationRatio = 20.f; // main:early

        std::vector<float> first_times;
        std::vector<float> second_times;

        if (cleaned_times.size() >= 2)
        {
            // Start: all hits assigned to main peak
            // log-likelihood ratio for hit t:
            //   LLR = log[ p(t|main) * 19 ] - log[ p(t|early) * 1 ]
            // If LLR < 0, hit is more likely from early peak.
            // We don't know the peak centers, so we bootstrap:
            // center_main  = mean of all hits (good starting point, 19/20 are main)
            // center_early = center_main - kPeakSeparation_ns

            auto sorted_for_init = cleaned_times; // already sorted
            float center_main = median_of(sorted_for_init);
            float center_early = center_main - kPeakSeparation_ns;

            // One EM pass is enough given the strong ratio prior
            for (int iter = 0; iter < 3; ++iter)
            {
                first_times.clear();
                second_times.clear();
                float sum_main = 0.f, sum_early = 0.f;
                int n_main = 0, n_early = 0;

                for (auto t : cleaned_times)
                {
                    const float d_main = (t - center_main) / kSigma_ns;
                    const float d_early = (t - center_early) / kSigma_ns;
                    // log-ratio: positive → main, negative → early
                    const float llr = 0.5f * (d_early * d_early - d_main * d_main) + std::log(kPopulationRatio);
                    if (llr >= 0.f)
                    {
                        second_times.push_back(t);
                        sum_main += t;
                        n_main++;
                    }
                    else
                    {
                        first_times.push_back(t);
                        sum_early += t;
                        n_early++;
                    }
                }

                // Update centers for next iteration
                if (n_main > 0)
                    center_main = sum_main / n_main;
                if (n_early > 0)
                    center_early = sum_early / n_early;
                // If no early hits found, fix center_early relative to main
                if (n_early == 0)
                    center_early = center_main - kPeakSeparation_ns;
            }
        }
        else
        {
            second_times = cleaned_times;
        }

        auto leave_one_out_mean = [&](const std::vector<float> &input_vec) -> std::vector<float>
        {
            std::vector<float> result;
            if (input_vec.size() < 2)
                return result;

            float full_val = 0.f;
            for (auto val : input_vec)
                full_val += val;

            const int n = static_cast<int>(input_vec.size());
            for (auto val : input_vec)
                result.push_back(val - (full_val - val) / (n - 1));

            return result;
        };

        auto clean_first_times = leave_one_out_mean(first_times);
        auto clean_second_times = leave_one_out_mean(second_times);
        for (auto val : clean_first_times)
            h_delta_t_half_center_left->Fill(val);
        for (auto val : clean_second_times)
            h_delta_t_half_center_right->Fill(val);

        // --- Split-half median
        // Odd/even split by index; each half uses the median of the *other* half
        // as reference — statistically independent, so the difference distribution
        // is unbiased. σ_measured = √2 × σ_single_photon.
        std::vector<float> even_times, odd_times;
        for (auto i_ter = 0; i_ter < static_cast<int>(cleaned_times.size()); i_ter++)
        {
            if (i_ter % 2 == 0)
                even_times.push_back(cleaned_times[i_ter]);
            else
                odd_times.push_back(cleaned_times[i_ter]);
        }

        if (!even_times.empty() && !odd_times.empty())
        {
            const float even_median = median_of(even_times);
            const float odd_median = median_of(odd_times);
            for (auto t : even_times)
            {
                h_sigma_vs_nhits->Fill(cleaned_times.size(), t - odd_median);
                h_delta_t_half_centroid->Fill(t - odd_median);
            }
            for (auto t : odd_times)
            {
                h_sigma_vs_nhits->Fill(cleaned_times.size(), t - even_median);
                h_delta_t_half_centroid->Fill(t - even_median);
            }
            h_median_vs_window->Fill(cleaned_times.back() - cleaned_times.front(), even_median - odd_median);
        }

        constexpr float kTdcEpsilon = 1e-4f; // 0.1 ps — below any physical LSB
        int n_zero_steps = 0;
        for (auto i = 1; i < static_cast<int>(cleaned_times.size()); i++)
        {
            const float step = cleaned_times[i] - cleaned_times[i - 1];
            h_tdc_step_sizes->Fill(step);
            if (std::fabs(step) < kTdcEpsilon)
            {
                n_zero_steps++;
                h_tdc_zero_times->Fill(cleaned_times[i]);
            }
        }
        if (n_zero_steps > 0)
            h_tdc_zero_cluster_size->Fill(peak_count);

        has_fired = true;

        //  Trigger time is the median of the peak window — robust to DCR outliers.
        const float trigger_time = median_of(peak_times);
        current_spill.add_trigger_to_frame(frame_id,
                                           {static_cast<uint8_t>(_TRIGGER_STREAMING_RING_FOUND_),
                                            static_cast<uint16_t>(peak_count),
                                            static_cast<float>(trigger_time)});

        in_cluster = false;
        peak_count = 0;
        peak_times.clear();
    };

    for (int ihit = 0; ihit < static_cast<int>(cherenkov_finedata_hits.size()); ++ihit)
    {
        if (cherenkov_finedata_hits[ihit].is_afterpulse())
            continue;

        const float current_time = cherenkov_finedata_hits[ihit].get_time_ns();

        //  Evict from the front of the deque — O(1) per eviction.
        while (!window.empty() && (current_time - window.front().second) > time_window_ns)
            window.pop_front();
        window.push_back({ihit, current_time});

        const int count = static_cast<int>(window.size());

        if (count >= threshold)
        {
            in_cluster = true;

            cherenkov_finedata_hits[ihit].set_streaming_ring_trigger_mask();
            cherenkov_hits[ihit].hit_mask = cherenkov_finedata_hits[ihit].get_mask();

            //  Update peak snapshot when occupancy grows.
            if (count > peak_count)
            {
                peak_count = count;
                peak_times.clear();
                for (const auto &entry : window)
                    peak_times.push_back(entry.second);
                // peak_times is already in time order since window is ordered
                // (deque front = oldest, back = newest, hits are sorted)
            }
        }
        else if (in_cluster)
        {
            end_of_cluster();
        }
    }

    if (in_cluster)
        end_of_cluster();

    //  Carry-over: hits still in the deque window at frame boundary.
    carry_over_hits.clear();
    for (const auto &entry : window)
        carry_over_hits.push_back({-1, entry.second - _FRAME_LENGTH_NS_});

    return has_fired;
}