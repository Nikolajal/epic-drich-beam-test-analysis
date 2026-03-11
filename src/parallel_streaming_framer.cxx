#include "parallel_streaming_framer.h"
#include <algorithm>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <execution>
#include <future>
#include <chrono>
using namespace std;

//  Constructor
parallel_streaming_framer::parallel_streaming_framer(std::vector<std::string> filenames,
                                                     std::string trigger_config_file,
                                                     std::string readout_config_file,
                                                     uint16_t frame_size)
    : _frame_size(frame_size), n_threads_requested(0), assigned_bar_(std::monostate{})
{
    // Create streams
    data_streams.reserve(filenames.size());
    for (const auto &current_filename : filenames)
    {
        data_streams.emplace_back(current_filename);
        if (!data_streams.back().is_valid())
            mist::logger::warning("Failed to open streamer: " + current_filename);
    }

    // Trigger map initialization
    triggers = trigger_conf_reader(trigger_config_file);
    for (auto current_trigger : triggers)
        triggers_map[current_trigger.device] = current_trigger;

    // Readout configuration
    readout_config = readout_config_list(readout_config_reader(readout_config_file));

    // Initialize spill counter
    _current_spill = -1;

    // Initialise the fine tune container
    h2_fine_tune_distribution = new TH2F("h2_fine_tune_distribution", ";global tdc index;fine parameter", 1e4, 0, 1e4, 256, 0, 256);
}

// Getters
alcor_spilldata parallel_streaming_framer::get_spilldata() const { return spilldata; }
alcor_spilldata &parallel_streaming_framer::get_spilldata_link() { return spilldata; }
int parallel_streaming_framer::get_registered_triggers() { return triggers_map.size(); }
TH2F *parallel_streaming_framer::get_fine_tune_distribution() { return h2_fine_tune_distribution; }

// Setters
void parallel_streaming_framer::set_spilldata(alcor_spilldata v) { spilldata = v; }
void parallel_streaming_framer::set_spilldata_link(alcor_spilldata &v) { spilldata = v; }
void parallel_streaming_framer::set_parallel_cores(uint16_t v) { n_threads_requested = v; }

// Bar assignment
void parallel_streaming_framer::assign_bar(mist::logger::progress_bar &bar) { assigned_bar_ = &bar; }
void parallel_streaming_framer::assign_bar(mist::logger::subtask_progress_bar &bar) { assigned_bar_ = &bar; }
void parallel_streaming_framer::clear_bar() { assigned_bar_ = std::monostate{}; }

// Internal bar update — no-op if nothing assigned.
// progress_bar and subtask_progress_bar are both internally thread-safe, so no
// additional locking is needed here beyond what the bars already do.
void parallel_streaming_framer::_update_bar(int64_t current, int64_t total)
{
    std::visit([&](auto &bar_or_none)
               {
                   using T = std::decay_t<decltype(bar_or_none)>;
                   if constexpr (!std::is_same_v<T, std::monostate>)
                       bar_or_none->update(current, total); }, assigned_bar_);
}

void parallel_streaming_framer::_finish_bar()
{
    std::visit([&](auto &bar_or_none)
               {
                   using T = std::decay_t<decltype(bar_or_none)>;
                   if constexpr (!std::is_same_v<T, std::monostate>)
                       bar_or_none->finish(); }, assigned_bar_);
}

// I/O operations
void parallel_streaming_framer::process(alcor_data_streamer &current_stream, int _frame_size)
{
    // Local instance of result
    auto &frame_map = spilldata.get_frame_link();
    std::unordered_map<int, uint64_t> afterpulse_map;

    // Start loop on streamer data
    while (current_stream.read_next())
    {
        // Recover data from streamer
        auto &current_data = current_stream.current();
        // --- Set aside useful variables
        auto current_device = current_data.get_device();
        auto current_chip = current_data.get_chip();
        auto current_readout_tag_list = readout_config.find_by_device_and_chip(current_device, current_chip);

        // Stop streamer reading if not tagged for readout
        if (current_readout_tag_list.size() == 0 && (current_chip != (99 / 4)))
            break;

        // Refactoring time to relate to frame reference
        uint32_t hit_frame_index = current_data.get_coarse_global_time() / (_frame_size * 1.);
        uint64_t hit_frame_coarse = current_data.get_coarse_global_time() % static_cast<uint64_t>(_frame_size);
        uint64_t hit_frame_coarse_global = current_data.get_coarse_global_time();

        //  ----    ----    ----    ALCOR hit  ----    ----    ----
        if (current_data.is_alcor_hit())
        {
            // Make afterpulse check
            current_data.set_mask(0);
            auto current_channel = current_data.get_global_index();
            if (auto search = afterpulse_map.find(current_channel); search != afterpulse_map.end())
                if (hit_frame_coarse_global < search->second)
                    current_data.add_mask_bit(_HITMASK_afterpulse);
            afterpulse_map[current_channel] = hit_frame_coarse_global + _AFTERPULSE_DEADTIME_;

            // Assigning frame-time values
            current_data.set_rollover(0);
            current_data.set_coarse(hit_frame_coarse);

            // Lock only this frame's mutex
            {
                std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                auto &current_lightdata = frame_map[hit_frame_index];
                // Assign to correct lightdata hit label
                for (auto &tag : current_readout_tag_list)
                {
                    if (tag == "timing")
                        current_lightdata.timing_hits.emplace_back(current_data.get_data_struct());
                    else if (tag == "tracking")
                        current_lightdata.tracking_hits.emplace_back(current_data.get_data_struct());
                    else if (tag == "cherenkov")
                        current_lightdata.cherenkov_hits.emplace_back(current_data.get_data_struct());
                }
                h2_fine_tune_distribution->Fill(current_data.get_global_tdc_index() + 1, current_data.get_fine());
            }
            continue;
        }

        //  ----    ----    ----    Trigger hit  ----    ----    ----
        if (current_data.is_trigger_tag())
        {
            // Read trigger settings safely
            trigger_config current_setting;
            bool trigger_known = false;
            {
                std::lock_guard<std::mutex> trig_lock(triggers_map_mutex);
                if (!triggers_map.count(current_device))
                    triggers_map[current_device] = {
                        "UNDF",
                        static_cast<uint8_t>(_TRIGGER_UNKNOWN_),
                        static_cast<uint16_t>(0),
                        static_cast<uint16_t>(current_device)};
                current_setting = triggers_map[current_device];
                trigger_known = (current_setting.index != _TRIGGER_UNKNOWN_);
            }

            if (!trigger_known)
            {
                // Unknown trigger — write to original frame
                std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                frame_map[hit_frame_index].trigger_hits.emplace_back(
                    static_cast<uint8_t>(_TRIGGER_UNKNOWN_),
                    static_cast<uint16_t>(current_device),
                    static_cast<float>(hit_frame_coarse * _ALCOR_CC_TO_NS_));
                continue;
            }

            // Known trigger — recalculate frame and write there
            uint32_t new_frame_index = (hit_frame_coarse_global - current_setting.delay) / (_frame_size * 1.);
            uint64_t new_frame_coarse = (hit_frame_coarse_global - current_setting.delay) % static_cast<uint64_t>(_frame_size);
            {
                std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
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
            // Lock the spilldata info
            std::lock_guard<std::mutex> lock(spilldata_masks_mutex);

            // Gather infos on the fifo
            auto current_fifo = current_data.get_fifo();

            // Encode participation of lane
            auto &current_participants_mask = spilldata.get_participants_mask_link();
            current_participants_mask[static_cast<uint8_t>(current_device)] += encode_bits({(uint8_t)current_fifo});

            // Encode dead lane
            if (current_data.coarse_time_clock() != 0)
            {
                auto &current_dead_mask = spilldata.get_dead_mask_link();
                current_dead_mask[static_cast<uint8_t>(current_device)] += encode_bits({(uint8_t)current_fifo});
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

    // --- First frames trigger
    for (auto i_frame = 0; i_frame < _FIRST_FRAMES_TRIGGER_; ++i_frame)
    {
        auto &current_lightdata = frame_list[i_frame];
        auto &current_trigger_hits = current_lightdata.trigger_hits;
        current_trigger_hits.push_back({100, static_cast<uint16_t>(_frame_size / 2.), static_cast<float>(_ALCOR_CC_TO_NS_ * _frame_size / 2.)});
    }

    // --- Remove invalid streams before feeding to the parallel unit
    auto it = std::remove_if(data_streams.begin(), data_streams.end(), [](const alcor_data_streamer &stream)
                             { return !stream.is_valid(); });
    for (auto it_ = it; it_ != data_streams.end(); it_++)
        mist::logger::warning("Invalid datastream discarded: " + it_->get_filename());
    data_streams.erase(it, data_streams.end());

    // Check machine cores for safe threading
    unsigned int n_threads = std::thread::hardware_concurrency();
    n_threads = n_threads_requested > 0
                    ? std::min<uint16_t>(n_threads_requested, 8 * std::min<size_t>(n_threads - 2, 2))
                    : 8 * std::min<size_t>(n_threads - 2, 2);

    std::atomic<size_t> next_streamer_atomic(0);
    std::atomic<size_t> completed(0);
    const size_t total = data_streams.size();

    mist::logger::info("(parallel_streaming_framer::next_spill) Starting reading data streams");

    std::vector<std::future<void>> thread_pool;
    thread_pool.reserve(n_threads);

    for (size_t i = 0; i < n_threads; ++i)
    {
        thread_pool.push_back(std::async(std::launch::async,
                                         [this, &next_streamer_atomic, &completed, total]()
                                         {
                                             while (true)
                                             {
                                                 size_t my_stream = next_streamer_atomic.fetch_add(1);
                                                 if (my_stream >= total)
                                                     return;

                                                 process(data_streams[my_stream], this->_frame_size);

                                                 size_t done = ++completed;
                                                 _update_bar(static_cast<int64_t>(done), static_cast<int64_t>(total));
                                             }
                                         }));
    }

    for (auto &f : thread_pool)
        f.get();

    _finish_bar();

    mist::logger::info("(parallel_streaming_framer::next_spill) Spill " +
                       std::to_string(_current_spill) +
                       " processing finished! Successfully processed " +
                       std::to_string(data_streams.size()) +
                       " data streams");

    return spilldata.has_data();
}