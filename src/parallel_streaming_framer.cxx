#include "parallel_streaming_framer.h"
#include <algorithm>
#include <iostream>
#include <vector>
#include <string>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <execution>
#include <future>
#include <chrono>
#include <thread>
using namespace std;

//  Constructors
parallel_streaming_framer::parallel_streaming_framer(std::vector<std::string> filenames,
                                                     uint16_t frame_size)
    : parallel_streaming_framer(filenames, "", "", frame_size) {}
parallel_streaming_framer::parallel_streaming_framer(std::vector<std::string> filenames,
                                                     std::string trigger_config_file,
                                                     uint16_t frame_size)
    : parallel_streaming_framer(filenames, trigger_config_file, "", frame_size) {}
parallel_streaming_framer::parallel_streaming_framer(std::vector<std::string> filenames,
                                                     std::string trigger_config_file,
                                                     std::string readout_config_file,
                                                     uint16_t frame_size)
    : _frame_size(frame_size), n_threads_requested(0)
{
    // Create streams
    data_streams.reserve(filenames.size());
    for (const auto &current_filename : filenames)
    {
        data_streams.emplace_back(current_filename);
        if (!data_streams.back().is_valid())
            logger::log_warning("[WARNING] Failed to open streamer: " + current_filename);
    }

    // QA plots
    init_QA_plots();

    // Trigger map initialization
    triggers = trigger_conf_reader(trigger_config_file);
    for (auto current_trigger : triggers)
        triggers_map[current_trigger.device] = current_trigger;

    // Readout configuration
    readout_config = readout_config_list(readout_config_reader(readout_config_file));

    // Initialize spill counter
    _current_spill = -1;
}

// Getters
alcor_spilldata parallel_streaming_framer::get_spilldata() const { return spilldata; }
alcor_spilldata &parallel_streaming_framer::get_spilldata_link() { return spilldata; }
std::map<std::string, TH1 *> parallel_streaming_framer::get_QA_plots() { return {}; }

// Setters
void parallel_streaming_framer::set_spilldata(alcor_spilldata v) { spilldata = v; }
void parallel_streaming_framer::set_spilldata_link(alcor_spilldata &v) { spilldata = v; }
void parallel_streaming_framer::set_parallel_cores(uint16_t v) { n_threads_requested = v; }

// Initialize QA plots
void parallel_streaming_framer::init_QA_plots() {}

// I/O operations
void parallel_streaming_framer::process(alcor_data_streamer &current_stream, int _frame_size)
{
    //  Local istance of result
    auto &frame_map = spilldata.get_frame_link();
    std::unordered_map<int, uint64_t> afterpulse_map;

    //  Start loop on streamer data
    while (current_stream.read_next())
    {
        //  Recover data from streamer
        auto &current_data = current_stream.current();
        //  --- Set aside useful variables
        auto current_device = current_data.get_device();
        auto current_chip = current_data.get_chip();
        auto current_readout_tag_list = readout_config.find_by_device_and_chip(current_device, current_chip);

        //  Stop streamer reading if not tagged for readout
        //  TODO: understand the issue to make it work again, if borken at all
        //  TODO: For thread balancing this should probably be done outside this function, before workload division
        //  if (current_readout_tag_list.size() == 0 && (current_chip != (99 / 4)))
        //      break;

        // Refactoring time to relate to frame reference
        uint32_t hit_frame_index = current_data.get_coarse_global_time() / (_frame_size * 1.);
        uint64_t hit_frame_coarse = current_data.get_coarse_global_time() % static_cast<uint64_t>(_frame_size);
        uint64_t hit_frame_coarse_global = current_data.get_coarse_global_time();

        //  ----    ----    ----    ALCOR hit  ----    ----    ----
        if (current_data.is_alcor_hit())
        {
            // Get or create the mutex for this frame
            std::mutex *frame_mutex;
            {
                std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                frame_mutex = &frame_mutexes[hit_frame_index]; // creates if not exists
            }

            //  Make afterpulse check
            current_data.set_mask(0);
            auto current_channel = current_data.get_global_index();
            if (auto search = afterpulse_map.find(current_channel); search != afterpulse_map.end())
                if (hit_frame_coarse_global < afterpulse_map[current_channel])
                    current_data.add_mask_bit(_HITMASK_afterpulse);
            afterpulse_map[current_channel] = hit_frame_coarse_global + _AFTERPULSE_DEADTIME_;

            //  Assigning frame-time values
            current_data.set_rollover(0);
            current_data.set_coarse(hit_frame_coarse);
            current_data.set_mask(0);

            // Lock only this frame's mutex
            {
                std::lock_guard<std::mutex> frame_lock(*frame_mutex);
                auto &current_lightdata = frame_map[hit_frame_index];
                //  Assign to correct lightdata hit label
                for (auto &tag : current_readout_tag_list)
                {
                    if (tag == "timing")
                        current_lightdata.timing_hits.emplace_back(current_data.get_data_struct());
                    else if (tag == "tracking")
                        current_lightdata.tracking_hits.emplace_back(current_data.get_data_struct());
                    else if (tag == "cherenkov")
                        current_lightdata.cherenkov_hits.emplace_back(current_data.get_data_struct());
                }
            }

            //  TODO: re-build QA plots infrastructure
            //  QA_plots["TH2F_fine_calib_global_index"]->Fill(current_data.get_global_tdc_index(), current_data.get_fine());
            //  TODO: if something else is defined, add. make all "classes" variable. Might make some issues in the reading... Maybe you can ask root to dynamically generate something to read?
            continue;
        }

        //  ----    ----    ----    Trigger hit  ----    ----    ----
        if (current_data.is_trigger_tag())
        {
            // Read trigger settings safely
            trigger_config_struct current_setting;
            bool trigger_known = false;
            {
                std::lock_guard<std::mutex> trig_lock(triggers_map_mutex);
                if (!triggers_map.count(current_device))
                    triggers_map[current_device] = {"UNDF", 255, 0, static_cast<uint16_t>(current_device)};
                current_setting = triggers_map[current_device];
                trigger_known = (current_setting.index != 255);
            }

            if (!trigger_known)
            {
                // Unknown trigger — write to original frame
                std::mutex *frame_mutex;
                {
                    std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                    frame_mutex = &frame_mutexes[hit_frame_index];
                }
                std::lock_guard<std::mutex> frame_lock(*frame_mutex);
                frame_map[hit_frame_index].trigger_hits.emplace_back(
                    static_cast<uint8_t>(255),
                    static_cast<uint16_t>(current_device),
                    static_cast<float>(hit_frame_coarse * _ALCOR_CC_TO_NS_));
                continue;
            }

            // Known trigger — recalculate frame and write there
            uint32_t new_frame_index = (hit_frame_coarse_global - current_setting.delay) / (_frame_size * 1.);
            uint64_t new_frame_coarse = (hit_frame_coarse_global - current_setting.delay) % static_cast<uint64_t>(_frame_size);

            std::mutex *frame_mutex;
            {
                std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                frame_mutex = &frame_mutexes[new_frame_index];
            }
            {
                std::lock_guard<std::mutex> frame_lock(*frame_mutex);
                frame_map[new_frame_index].trigger_hits.emplace_back(
                    static_cast<uint8_t>(current_setting.index),
                    static_cast<uint16_t>(new_frame_coarse),
                    static_cast<float>(_ALCOR_CC_TO_NS_ * new_frame_coarse));
            }
            continue;
        }

        //  ----    ----    ----    Start of spill  ----    ----    ----
        if (current_data.is_start_spill())
        {
            {
                std::lock_guard<std::mutex> lock(spilldata_masks_mutex);
                //  Gather infos on the fifo
                auto current_fifo = current_data.get_fifo();

                //  Encode participation of lane
                auto &current_participants_mask = spilldata.get_participants_mask_link();
                current_participants_mask[static_cast<uint8_t>(current_device)] += encode_bits({(uint8_t)current_fifo});

                //  Encode dead lane
                if (current_data.coarse_time_clock() != 0)
                {
                    auto &current_dead_mask = spilldata.get_dead_mask_link();
                    current_dead_mask[static_cast<uint8_t>(current_device)] += encode_bits({(uint8_t)current_fifo});
                }
            }
            continue;
        }

        //  ----    ----    ----    End of spill  ----    ----    ----
        if (current_data.is_end_spill())
            break;
    }
}

bool parallel_streaming_framer::next_spill()
{
    spilldata.clear();
    _current_spill++;
    auto &frame_list = spilldata.get_frame_link();

    //  Pre-populate maps to avoid re-hashing
    {
        uint32_t max_frame = 5.e6;
        frame_list.reserve(max_frame);
        frame_mutexes.reserve(max_frame);
        for (uint32_t i = 0; i < max_frame; ++i)
        {
            frame_list[i];
            frame_mutexes[i];
        }
    }

    // --- First frames trigger
    for (auto i_frame = 0; i_frame < _FIRST_FRAMES_TRIGGER_; ++i_frame)
    {
        auto &current_lightdata = frame_list[i_frame];
        auto &current_trigger_hits = current_lightdata.trigger_hits;
        current_trigger_hits.push_back({100, static_cast<uint16_t>(_frame_size / 2.), static_cast<float>(_ALCOR_CC_TO_NS_ * _frame_size / 2.)});
    }

    //  Start of parallel work
    //  --- Remove invalide streams before feeding to the parallel unit
    //  Re-orders array so that last elements, from it to end, are the invalid ones
    auto it = std::remove_if(data_streams.begin(), data_streams.end(), [](const alcor_data_streamer &stream)
                             { return !stream.is_valid(); });
    for (auto it_ = it; it_ != data_streams.end(); it_++)
        std::cout << "[WARNING] Invalid datastream discarded: " << it_->get_filename() << endl;
    data_streams.erase(it, data_streams.end());

    //  Check machine cores for safe threading
    unsigned int n_threads = std::thread::hardware_concurrency();
    n_threads = n_threads_requested > 0 ? std::min<uint16_t>(n_threads_requested, 8 * std::min<size_t>(n_threads - 2, 2)) : 8 * std::min<size_t>(n_threads - 2, 2); //  Leave 2 cores free for general machine work, 8 streams per core are manageable

    //  Generate result vector
    std::vector<std::future<void>> async_processing_results;
    async_processing_results.reserve(n_threads);

    //  Loop over data streams
    //  --- Start with streamer 0
    size_t next_streamer = 0;
    while (next_streamer < data_streams.size())
    {
        //  Update on processing
        std::cout << "\33[2K\r[INFO] Spill " << (int)_current_spill << " processed files " << next_streamer << "/" << data_streams.size() << " -- parallel threads: " << n_threads << flush;

        //  Set next batch size
        size_t batch_size = std::min<size_t>(n_threads, data_streams.size() - next_streamer);

        //  Launch a batch of async tasks processing the next spill of data to generate frames
        for (size_t i = 0; i < batch_size; ++i)
        {
            auto &current_stream = data_streams[next_streamer + i];
            async_processing_results.push_back(std::async(std::launch::async, [this, &current_stream]()
                                                          { return process(current_stream, this->_frame_size); }));
        }

        //  Collect results for this batch
        for (auto &current_async_thread_result : async_processing_results)
            current_async_thread_result.get(); //    Get forces the current thread to end working before next batch

        async_processing_results.clear(); //    Ready for next batch
        next_streamer += batch_size;      //    Start over with next batch
    }
    async_processing_results.clear();

    //  Finished Processing spills
    std::cout << "\33[2K\r[INFO] Spill " << (int)_current_spill << " processing finished! Successfully processed " << data_streams.size() << " data streams" << std::endl;

    return spilldata.has_data();
}