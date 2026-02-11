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

//  TODO: this could probably be a derivate class (?)

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
    : _frame_size(frame_size)
{
    // Create streams
    data_streams.reserve(filenames.size());
    for (const auto &current_filename : filenames)
    {
        data_streams.emplace_back(current_filename);
        if (!data_streams.back().is_valid())
            cerr << "[WARNING] Failed to open streamer: " << current_filename << std::endl;
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

// Initialize QA plots
void parallel_streaming_framer::init_QA_plots() {}

// I/O operations
alcor_spilldata parallel_streaming_framer::process(alcor_data_streamer &current_stream, int _frame_size)
{
    //  Local istance of result
    alcor_spilldata current_spilldata;
    auto &frame_map = current_spilldata.get_frame_link();
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

        // Stop streamer reading if not tagged for readout
        //  TODO: understand the issue to make it work again, if borken at all
        // if (current_readout_tag_list.size() == 0 && (current_chip != (99 / 4)))
        //    break;

        // Refactoring time to relate to frame reference
        uint32_t hit_frame_index = current_data.get_coarse_global_time() / (_frame_size * 1.);
        uint64_t hit_frame_coarse = current_data.get_coarse_global_time() % static_cast<uint64_t>(_frame_size);
        uint64_t hit_frame_coarse_global = current_data.get_coarse_global_time();
        auto &current_lightdata = frame_map[hit_frame_index];

        //  ----    ----    ----    ALCOR hit  ----    ----    ----
        if (current_data.is_alcor_hit())
        {
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
            //  TODO: re-build QA plots infrastructure
            //  QA_plots["TH2F_fine_calib_global_index"]->Fill(current_data.get_global_tdc_index(), current_data.get_fine());
            //  TODO: if something else is defined, add. make all "classes" variable. Might make some issues in the reading... Maybe you can ask root to dynamically generate something to read?
            continue;
        }

        //  ----    ----    ----    Trigger hit  ----    ----    ----
        if (current_data.is_trigger_tag())
        {
            //  Look for this trigger in the provided trigger list, if not found assign 255 index (unknown)
            if (!triggers_map.count(current_device))
            {
                //  If unknown, the coarse will hold the device number and the fine_time the hit time in ns (coarse)
                triggers_map[current_device] = {"UNDF", 255, 0, static_cast<uint16_t>(current_device)};
                current_lightdata.trigger_hits.emplace_back(static_cast<uint8_t>(255), static_cast<uint16_t>(current_device), static_cast<float>(hit_frame_coarse * _ALCOR_CC_TO_NS_));
                continue;
            }

            //  Current trigger settings
            auto current_setting = triggers_map[current_device];

            //  The trigger is scaled by the given delay, re-calcualte the frame accordingly
            hit_frame_index = (hit_frame_coarse_global - current_setting.delay) / (_frame_size * 1.);
            hit_frame_coarse = (hit_frame_coarse_global - current_setting.delay) % static_cast<uint64_t>(_frame_size);
            frame_map[hit_frame_index].trigger_hits.emplace_back(static_cast<uint8_t>(current_setting.index), static_cast<uint16_t>(hit_frame_coarse), static_cast<float>(_ALCOR_CC_TO_NS_ * hit_frame_coarse));
            continue;
        }

        //  ----    ----    ----    Start of spill  ----    ----    ----
        if (current_data.is_start_spill())
        {
            //  Gather infos on the fifo
            auto current_fifo = current_data.get_fifo();

            //  Encode participation of lane
            auto &current_participants_mask = current_spilldata.get_participants_mask_link();
            current_participants_mask[static_cast<uint8_t>(current_device)] += encode_bits({(uint8_t)current_fifo});

            //  Encode dead lane
            if (current_data.coarse_time_clock() != 0)
            {
                auto &current_dead_mask = current_spilldata.get_dead_mask_link();
                current_dead_mask[static_cast<uint8_t>(current_device)] += encode_bits({(uint8_t)current_fifo});
            }
            continue;
        }

        //  ----    ----    ----    End of spill  ----    ----    ----
        if (current_data.is_end_spill())
            break;
    }
    return current_spilldata;
}

bool parallel_streaming_framer::next_spill()
{
    spilldata.clear();
    _current_spill++;
    auto &frame_list = spilldata.get_frame_link();

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
    /*
    TODO: Why doesn't it work?
    auto it = std::remove_if(data_streams.begin(), data_streams.end(), [](const alcor_data_streamer &stream)
                             { return !stream.is_valid(); });
    for (auto it_ = it; it_ != data_streams.end(); it_++)
        std::cout << "[WARNING] Invalid datastream discarded: " << it_->get_filename() << endl;
    data_streams.erase(it, data_streams.end());
    */

    //  Check machine cores for safe threading
    unsigned int n_threads = std::thread::hardware_concurrency();
    n_threads = 8 * std::min<size_t>(n_threads - 2, 2); //  Leave 2 cores free for general machine work, 2 streams per core are manageable

    //  Generate result vector
    std::vector<std::future<alcor_spilldata>> async_processing_results;
    async_processing_results.reserve(n_threads);
    std::vector<alcor_spilldata> processing_results;
    processing_results.reserve(data_streams.size());

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
            processing_results.push_back(current_async_thread_result.get()); //    Get forces the current thread to end working before next batch

        async_processing_results.clear(); //    Ready for next batch
        next_streamer += batch_size;      //    Start over with next batch
    }
    async_processing_results.clear();

    //  Finished Processing spills
    std::cout << "\33[2K\r[INFO] Spill " << (int)_current_spill << " processing finished! Successfully processed " << data_streams.size() << " data streams" << std::endl;

    // Divide processing_results into roughly equal chunks
    size_t base_size = processing_results.size() / n_threads; //    minimum number of chunks per thread
    size_t remainder = processing_results.size() % n_threads; //    number of threads that get 1 extra chunk
    size_t start = 0;                                         //    starting chunk
    std::cout << "\33[2K\r[INFO] Spill " << (int)_current_spill << " merging jobs in " << remainder << " batches of " << base_size << " data streams and " << n_threads - remainder << " batches of " << base_size + 1 << " data streams" << flush;
    for (size_t current_thread = 0; current_thread < n_threads; ++current_thread)
    {
        size_t chunk_size = base_size + (current_thread < remainder ? 1 : 0);
        size_t end = start + chunk_size;

        //  Process merging
        async_processing_results.push_back(
            std::async(
                std::launch::async,
                [start, end, &processing_results]() -> alcor_spilldata
                {
                    alcor_spilldata local_acc;
                    for (size_t i = start; i < end; ++i)
                    {
                        merge(local_acc, std::move(processing_results[i]));
                    }
                    return local_acc;
                }));
        start = end;
    }

    // Collect results
    auto i_chunk = 0;
    for (auto &current_async_processing_results : async_processing_results)
    {
        i_chunk++;
        merge(spilldata, current_async_processing_results.get());
        std::cout << "\33[2K\r[INFO] Spill " << (int)_current_spill << " last merge round file " << i_chunk << "/" << async_processing_results.size() << flush;
    }

    /*
    //---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//
    while (processing_results.size() > 1)
    {
        std::vector<alcor_spilldata> next_round;
        size_t i = 0;

        std::cout << "\33[2K\r[INFO] Spill " << (int)_current_spill << " next merge round has " << processing_results.size() << " files"<< flush;
        while (i + 1 < processing_results.size())
        {
            size_t batch_end = std::min(i + n_threads * 2, processing_results.size());
            size_t batch_size = batch_end - i;

            // For each batch, launch threads for merging pairs
            std::vector<std::thread> threads;
            std::vector<alcor_spilldata> temp_results(batch_size / 2);

            for (size_t j = 0; j + 1 < batch_size; j += 2)
            {
                threads.emplace_back([&temp_results, j, &processing_results, i]()
                                     {
                    alcor_spilldata temp;
                    merge(temp, std::move(processing_results[i + j]));
                    merge(temp, std::move(processing_results[i + j + 1]));
                    temp_results[j / 2] = std::move(temp); });
            }

            // Wait for threads to finish
            for (auto &t : threads)
                t.join();

            // Collect results
            for (auto &res : temp_results)
                next_round.push_back(std::move(res));

            // If odd number of chunks in batch, carry the last one
            if (batch_size % 2 == 1)
                next_round.push_back(std::move(processing_results[batch_end - 1]));

            i = batch_end;
        }

        // If total chunks in round is odd, carry the last one
        if (processing_results.size() % 2 == 1 && i == processing_results.size() - 1)
            next_round.push_back(std::move(processing_results.back()));

        processing_results = std::move(next_round);
    }
    //---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//---//

    spilldata = std::move(processing_results[0]);

    /*
    // Collect results
    auto i_chunk = 0;
    for (auto &current_processing_results : processing_results)
    {
        i_chunk++;
        merge(spilldata, std::move(current_processing_results));
        std::cout << "\33[2K\r[INFO] Spill " << (int)_current_spill << " last merge round file " << i_chunk << "/" << processing_results.size() << flush;
    }
    */

    //  Finished Merging spills
    std::cout << "\33[2K\r[INFO] Spill merging finished! Successfully merged " << data_streams.size() << " data streams results" << std::endl;

    return spilldata.has_data();
}