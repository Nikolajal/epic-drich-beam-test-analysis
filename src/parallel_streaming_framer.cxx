#include "parallel_streaming_framer.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>
#include <string>
#include <thread>
#include <execution>
#include <future>
#include <chrono>
using namespace std;

//  Constructor (frame_size overload — delegates to the FramerConfigStruct overload)
ParallelStreamingFramer::ParallelStreamingFramer(std::vector<std::string> filenames,
                                                     std::string trigger_config_file,
                                                     std::string readout_config_file,
                                                     uint16_t frame_size)
    : ParallelStreamingFramer(filenames, trigger_config_file, readout_config_file,
                                FramerConfigStruct{frame_size,
                                                     BTANA_FIRST_FRAMES_TRIGGER,
                                                     BTANA_AFTERPULSE_DEADTIME,
                                                     BTANA_TRIGGER_SECONDARY_WINDOW})
{}

//  Constructor (FramerConfigStruct overload — canonical implementation)
ParallelStreamingFramer::ParallelStreamingFramer(std::vector<std::string> filenames,
                                                     std::string trigger_config_file,
                                                     std::string readout_config_file,
                                                     FramerConfigStruct framer_cfg)
    : _frame_size(framer_cfg.frame_size),
      _first_frames_trigger(framer_cfg.first_frames_trigger),
      _afterpulse_deadtime(framer_cfg.afterpulse_deadtime),
      _trigger_secondary_window(framer_cfg.trigger_secondary_window),
      n_threads_requested(0), assigned_bar_(std::monostate{})
{
    // Create streams
    data_streams.reserve(filenames.size());
    for (const auto &current_filename : filenames)
    {
        data_streams.emplace_back(current_filename);
        if (!data_streams.back().is_valid())
            mist::logger::warning("Failed to open streamer: " + current_filename);
    }

    // Load trigger configurations (read-only during processing; looked up via find_best_trigger).
    triggers = trigger_conf_reader(trigger_config_file);

    // Readout configuration
    readout_config = ReadoutConfigList(readout_config_reader(readout_config_file));

    // Warn for every use_hit trigger whose source channel is absent from the readout config.
    // Without a readout entry the Hit path produces no output (current_readout_tag_list is empty).
    for (const auto &trig : triggers)
    {
        if (!trig.use_hit)
            continue;

        bool registered = false;
        if (trig.fifo >= 0)
        {
            // Exact lane known — check the specific chip.
            registered = !readout_config.find_by_device_and_chip(trig.device, trig.fifo / 4).empty();
        }
        else if (trig.chip >= 0)
        {
            // Chip-level filter — check that chip.
            registered = !readout_config.find_by_device_and_chip(trig.device, trig.chip).empty();
        }
        else
        {
            // Device-only trigger — any chip on the device would do.
            registered = (readout_config.find_by_device(trig.device) != nullptr);
        }

        if (!registered)
            mist::logger::warning(
                "(ParallelStreamingFramer) Trigger \"" + trig.name +
                "\" has use_hit=true but device=" + std::to_string(trig.device) +
                (trig.fifo >= 0 ? " fifo=" + std::to_string(trig.fifo) : "") +
                (trig.chip >= 0 ? " chip=" + std::to_string(trig.chip) : "") +
                " is not registered in the readout config — hits will be silently discarded.");
    }

    // Initialize spill counter
    _current_spill = -1;

    // Initialise the fine tune container
    h2_fine_tune_distribution = new TH2F("h2_fine_tune_distribution", ";global tdc index;fine parameter", 1e4, -0.5, 1e4 - 0.5, 256, 0, 256);
    // Same-channel Δt diagnostic — one entry per Hit (skipping each channel's
    // first Hit).  Range chosen to span both the QA near and far windows for
    // any reasonable QA configuration; bin = 1 cc for direct readout.
    h_afterpulse_dt = new TH1F("h_afterpulse_dt",
                               ";#Delta_{t} to previous same-channel Hit (cc);entries",
                               1024, 0, 1024);
}

// Getters
// Simple getters live inline in the header. Only the non-trivial ones remain here.
std::vector<std::string>
ParallelStreamingFramer::get_stream_filenames() const
{
    std::vector<std::string> result;
    result.reserve(data_streams.size());
    for (const auto &current_stream : data_streams)
        result.push_back(current_stream.get_filename());
    return result;
}

// Internal bar update — no-op if nothing assigned.
// ProgressBar and SubtaskProgressBar are both internally thread-safe, so no
// additional locking is needed here beyond what the bars already do.
void ParallelStreamingFramer::_update_bar(int64_t current, int64_t total)
{
    std::visit([&](auto &bar_or_none)
               {
                   using T = std::decay_t<decltype(bar_or_none)>;
                   if constexpr (!std::is_same_v<T, std::monostate>)
                       bar_or_none->update(current, total); }, assigned_bar_);
}

// Rollover-offset resolution.
// For each spill index, the reference is the max rollover count across streams;
// lanes below reference missed ticks and get a positive clock-cycle correction.
// The resulting table is read-only for the rest of the framer's lifetime.
void ParallelStreamingFramer::resolve_rollover_offsets()
{
    rollover_correction_per_stream_and_spill.clear();
    rollover_correction_per_stream_and_spill.resize(data_streams.size());

    // Align over the minimum number of spills reported by any valid stream with
    // a non-empty gRollover. Streams missing the graph contribute no points
    // and leave the alignment window unchanged.
    size_t n_spills_aligned = std::numeric_limits<size_t>::max();
    for (const auto &current_stream : data_streams)
    {
        if (!current_stream.is_valid())
            continue;
        const auto &rollover_vector = current_stream.get_rollover_count_per_spill();
        if (!rollover_vector.empty())
            n_spills_aligned = std::min(n_spills_aligned, rollover_vector.size());
    }
    if (n_spills_aligned == std::numeric_limits<size_t>::max())
    {
        mist::logger::warning("(ParallelStreamingFramer::resolve_rollover_offsets) "
                              "No streams with gRollover found, skipping correction");
        return;
    }

    // Per-spill reference = majority-vote (mode) rollover count across streams.
    // The mode is more robust than the max: a single stream with a corrupted or
    // anomalously large gRollover endpoint no longer pulls the reference for the
    // entire spill.  Streams deviating from the mode by more than 1 tick are
    // treated as unreliable and receive no correction (entry left at zero).
    std::vector<double> reference_rollover_per_spill(n_spills_aligned, 0.);
    for (size_t i_spill = 0; i_spill < n_spills_aligned; ++i_spill)
    {
        // Tally rollover values across streams for this spill
        std::map<int, int> rollover_vote_tally;
        for (const auto &current_stream : data_streams)
        {
            if (!current_stream.is_valid())
                continue;
            const auto &rollover_vector = current_stream.get_rollover_count_per_spill();
            if (i_spill < rollover_vector.size() && rollover_vector[i_spill] > 0.)
                rollover_vote_tally[static_cast<int>(rollover_vector[i_spill])]++;
        }
        // Pick the mode (value with the most votes)
        int mode_rollover_value = 0;
        int mode_rollover_count = 0;
        for (const auto &[rollover_value, vote_count] : rollover_vote_tally)
        {
            if (vote_count > mode_rollover_count)
            {
                mode_rollover_count = vote_count;
                mode_rollover_value = rollover_value;
            }
        }
        reference_rollover_per_spill[i_spill] = static_cast<double>(mode_rollover_value);
    }

    // Fill the correction table and log any non-zero entries.
    size_t n_corrections_applied = 0;
    for (size_t i_stream = 0; i_stream < data_streams.size(); ++i_stream)
    {
        rollover_correction_per_stream_and_spill[i_stream].assign(n_spills_aligned, 0);
        const auto &current_stream = data_streams[i_stream];
        if (!current_stream.is_valid())
            continue;

        const auto &rollover_vector = current_stream.get_rollover_count_per_spill();
        for (size_t i_spill = 0; i_spill < n_spills_aligned; ++i_spill)
        {
            if (i_spill >= rollover_vector.size())
                continue;

            // Skip if this stream reports no rollover for this spill — treat as
            // missing data, not as a lagging stream. Leaves the entry at zero.
            if (rollover_vector[i_spill] <= 0.)
                continue;

            const double rollover_offset =
                reference_rollover_per_spill[i_spill] - rollover_vector[i_spill];
            if (rollover_offset == 0.)
                continue;

            // Discard streams deviating by more than 1 rollover tick from the
            // majority — likely a corrupted gRollover endpoint.  Leave the
            // correction at zero (no realignment) and warn.
            if (rollover_offset > 1.)
            {
                mist::logger::warning(Form(
                    "(ParallelStreamingFramer::resolve_rollover_offsets) "
                    "Stream %s spill %zu: rollover offset %.0f exceeds 1 tick "
                    "(stream = %.0f, reference = %.0f) — skipping correction",
                    current_stream.get_filename().c_str(),
                    i_spill,
                    rollover_offset,
                    rollover_vector[i_spill],
                    reference_rollover_per_spill[i_spill]));
                continue;
            }

            const uint64_t rollover_correction_cc =
                static_cast<uint64_t>(rollover_offset) *
                static_cast<uint64_t>(BTANA_ALCOR_ROLLOVER_TO_CC);
            rollover_correction_per_stream_and_spill[i_stream][i_spill] = rollover_correction_cc;
            ++n_corrections_applied;

            mist::logger::warning(Form(
                "(ParallelStreamingFramer::resolve_rollover_offsets) "
                "Stream %s spill %zu: rollover = %.0f, reference = %.0f, correction = +%.0f ticks",
                current_stream.get_filename().c_str(),
                i_spill,
                rollover_vector[i_spill],
                reference_rollover_per_spill[i_spill],
                rollover_offset));
        }
    }

    mist::logger::info("(ParallelStreamingFramer::resolve_rollover_offsets) Applied " +
                       std::to_string(n_corrections_applied) +
                       " non-zero corrections across " +
                       std::to_string(data_streams.size()) + " streams and " +
                       std::to_string(n_spills_aligned) + " spills");
}

// I/O operations
void ParallelStreamingFramer::process(size_t stream_index, int _frame_size)
{
    auto &current_stream = data_streams[stream_index];

    // Per-stream, per-spill rollover correction in clock cycles. Zero when the
    // correction table has not been built or the indices are out of range, so
    // omitting resolve_rollover_offsets() reproduces the pre-correction behaviour.
    uint64_t rollover_correction_cc = 0;
    if (stream_index < rollover_correction_per_stream_and_spill.size())
    {
        const auto &stream_corrections =
            rollover_correction_per_stream_and_spill[stream_index];
        if (static_cast<size_t>(_current_spill) < stream_corrections.size())
            rollover_correction_cc = stream_corrections[_current_spill];
    }

    // Local instance of result
    auto &frame_map = spilldata.get_frame_link();
    // Map: global channel index → previous-Hit global clock cycle for that channel.
    // Drives BOTH the framer-internal afterpulse mask (Δt < _afterpulse_deadtime)
    // and the QA sideband tagging (Δt in near / far windows from _qa_cfg).
    std::unordered_map<int, uint64_t> prev_hit_time_map;
    // Maps trigger index → first global clock cycle after which the next firing is no longer secondary.
    std::unordered_map<uint8_t, uint64_t> trigger_secondary_map;

    // Precompute the QA far-window bounds from _qa_cfg.
    // Far window mirrors the near window's width and starts at the sideband offset.
    const int64_t qa_near_lo      = _qa_cfg.afterpulse_near_lo;
    const int64_t qa_near_hi      = _qa_cfg.afterpulse_near_hi;
    const int64_t qa_far_lo       = _qa_cfg.afterpulse_sideband_offset;
    const int64_t qa_far_hi       = _qa_cfg.afterpulse_sideband_offset
                                  + (_qa_cfg.afterpulse_near_hi - _qa_cfg.afterpulse_near_lo);

    // Start loop on streamer data
    while (current_stream.read_next())
    {
        // Recover data from streamer
        auto &current_data = current_stream.current();
        // --- Set aside useful variables
        auto current_device = current_data.get_device();
        auto current_chip = current_data.get_chip();
        auto current_readout_tag_list = readout_config.find_by_device_and_chip(current_device, current_chip);

        // Stop streamer reading if not tagged for readout.
        // Chip 24 (fifo 96-99) is the hardware trigger chip — always continue for it.
        constexpr int trigger_chip = 99 / 4; // = 24
        if (current_readout_tag_list.size() == 0 && current_chip != trigger_chip)
            break;

        // Refactoring time to relate to frame reference.
        // The rollover correction is applied here once, so ALCOR hits and
        // trigger hits are both realigned uniformly downstream.
        uint64_t hit_frame_coarse_global = current_data.get_coarse_global_time() + rollover_correction_cc;
        uint32_t hit_frame_index = hit_frame_coarse_global / (_frame_size * 1.);
        uint64_t hit_frame_coarse = hit_frame_coarse_global % static_cast<uint64_t>(_frame_size);

        //  ----    ----    ----    ALCOR Hit  ----    ----    ----
        if (current_data.is_alcor_hit())
        {
            // Same-channel Δt analysis: drives both the framer's afterpulse
            // mask (Δt < deadtime, gates downstream CT/recodata logic) AND
            // the QA near/far bits used for sideband-subtracted afterpulse
            // probability profiles.  Δt is captured as @c same_channel_dt
            // (negative sentinel "no previous Hit") and forwarded into the
            // locked block below so the diagnostic h_afterpulse_dt histogram
            // can record it under the same mutex as the other QA fills.
            current_data.set_mask(0);
            auto current_channel = current_data.get_global_index();
            int64_t same_channel_dt = -1;
            if (auto search = prev_hit_time_map.find(current_channel); search != prev_hit_time_map.end())
            {
                same_channel_dt =
                    static_cast<int64_t>(hit_frame_coarse_global) - static_cast<int64_t>(search->second);
                // Framer-internal afterpulse mask (legacy semantics).
                if (same_channel_dt < static_cast<int64_t>(_afterpulse_deadtime))
                    current_data.add_mask_bit(HitmaskAfterpulse);
                // QA near window — afterpulse signal region (signal + DCR baseline).
                if (same_channel_dt >= qa_near_lo && same_channel_dt <= qa_near_hi)
                    current_data.add_mask_bit(HitmaskAfterpulseNear);
                // QA far window — DCR-only sideband (same width as near).
                if (same_channel_dt >= qa_far_lo && same_channel_dt <= qa_far_hi)
                    current_data.add_mask_bit(HitmaskAfterpulseFar);
            }
            prev_hit_time_map[current_channel] = hit_frame_coarse_global;

            // Assigning frame-time values
            current_data.set_rollover(0);
            current_data.set_coarse(hit_frame_coarse);

            // Lock only this frame's mutex
            {
                std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                auto &current_lightdata = frame_map[hit_frame_index];
                // Assign to correct lightdata Hit label
                for (auto &tag : current_readout_tag_list)
                {
                    if (tag == "timing")
                        current_lightdata.timing_hits.emplace_back(current_data.get_data());
                    else if (tag == "tracking")
                        current_lightdata.tracking_hits.emplace_back(current_data.get_data());
                    else if (tag == "cherenkov")
                        current_lightdata.cherenkov_hits.emplace_back(current_data.get_data());
                }
                h2_fine_tune_distribution->Fill(current_data.get_global_tdc_index(), current_data.get_fine());
                if (same_channel_dt >= 0)
                    h_afterpulse_dt->Fill(static_cast<double>(same_channel_dt));
            }
            continue;
        }

        //  ----    ----    ----    Trigger Hit  ----    ----    ----
        if (current_data.is_trigger_tag())
        {
            auto current_fifo   = current_data.get_fifo();
            auto current_column = current_data.get_column();
            auto current_pixel  = current_data.get_pixel();

            // Find best-matching config (most specific selector wins).
            // triggers is read-only after construction — no mutex needed for the scan.
            const TriggerConfig *best = find_best_trigger(
                triggers, current_device, current_fifo, current_column, current_pixel);

            // Log unrecognised triggers once per (device, fifo, column, pixel) tuple.
            if (!best)
            {
                uint64_t key = trigger_map_key(current_device, current_fifo, current_column, current_pixel);
                std::lock_guard<std::mutex> trig_lock(triggers_map_mutex);
                if (unknown_triggers_seen.insert(key).second)
                    mist::logger::warning("(ParallelStreamingFramer) Unknown trigger:"
                                          " device=" + std::to_string(current_device) +
                                          " fifo="   + std::to_string(current_fifo) +
                                          " column=" + std::to_string(current_column) +
                                          " pixel="  + std::to_string(current_pixel));
            }

            const bool trigger_known = best != nullptr;
            const TriggerConfig current_setting = best ? *best : TriggerConfig{};

            // use_hit bypass: treat the trigger-tagged word as a standard ALCOR Hit.
            // The trigger_tag struct carries the same channel and timing fields as a
            // normal alcor_hit, so the Hit path processes it identically.
            // Requires the channel to be registered in the readout config so that
            // current_readout_tag_list is non-empty and the Hit is categorised correctly.
            if (current_setting.use_hit)
            {
                // Same-channel Δt analysis (mirrors the ALCOR-Hit path above);
                // same_channel_dt is forwarded into the locked block below for
                // the h_afterpulse_dt diagnostic fill.
                current_data.set_mask(0);
                auto current_channel = current_data.get_global_index();
                int64_t same_channel_dt = -1;
                if (auto search = prev_hit_time_map.find(current_channel); search != prev_hit_time_map.end())
                {
                    same_channel_dt =
                        static_cast<int64_t>(hit_frame_coarse_global) - static_cast<int64_t>(search->second);
                    if (same_channel_dt < static_cast<int64_t>(_afterpulse_deadtime))
                        current_data.add_mask_bit(HitmaskAfterpulse);
                    if (same_channel_dt >= qa_near_lo && same_channel_dt <= qa_near_hi)
                        current_data.add_mask_bit(HitmaskAfterpulseNear);
                    if (same_channel_dt >= qa_far_lo && same_channel_dt <= qa_far_hi)
                        current_data.add_mask_bit(HitmaskAfterpulseFar);
                }
                prev_hit_time_map[current_channel] = hit_frame_coarse_global;
                current_data.set_rollover(0);
                current_data.set_coarse(hit_frame_coarse);
                {
                    std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                    auto &current_lightdata = frame_map[hit_frame_index];
                    for (auto &tag : current_readout_tag_list)
                    {
                        if (tag == "timing")
                            current_lightdata.timing_hits.emplace_back(current_data.get_data());
                        else if (tag == "tracking")
                            current_lightdata.tracking_hits.emplace_back(current_data.get_data());
                        else if (tag == "cherenkov")
                            current_lightdata.cherenkov_hits.emplace_back(current_data.get_data());
                    }
                    h2_fine_tune_distribution->Fill(current_data.get_global_tdc_index(), current_data.get_fine());
                    if (same_channel_dt >= 0)
                        h_afterpulse_dt->Fill(static_cast<double>(same_channel_dt));
                }
                continue;
            }

            if (!trigger_known)
            {
                // Unknown trigger — write to original frame
                const uint8_t unknown_idx = static_cast<uint8_t>(_TRIGGER_UNKNOWN_);
                bool sec = false;
                if (auto it = trigger_secondary_map.find(unknown_idx); it != trigger_secondary_map.end())
                    sec = hit_frame_coarse_global < it->second;
                trigger_secondary_map[unknown_idx] = hit_frame_coarse_global + _trigger_secondary_window;
                {
                    std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                    auto &ev = frame_map[hit_frame_index].trigger_hits.emplace_back(
                        unknown_idx,
                        static_cast<uint16_t>(current_device),
                        static_cast<float>(hit_frame_coarse * BTANA_ALCOR_CC_TO_NS));
                    ev.is_secondary = sec;
                }
                continue;
            }

            // Known trigger — check for secondary firing, then recalculate frame and write there
            {
                bool sec = false;
                if (auto it = trigger_secondary_map.find(current_setting.index); it != trigger_secondary_map.end())
                    sec = hit_frame_coarse_global < it->second;
                trigger_secondary_map[current_setting.index] = hit_frame_coarse_global + _trigger_secondary_window;

                uint32_t new_frame_index = (hit_frame_coarse_global - current_setting.delay) / (_frame_size * 1.);
                uint64_t new_frame_coarse = (hit_frame_coarse_global - current_setting.delay) % static_cast<uint64_t>(_frame_size);
                std::lock_guard<std::mutex> map_lock(frame_mutexes_access);
                auto &ev = frame_map[new_frame_index].trigger_hits.emplace_back(
                    static_cast<uint8_t>(current_setting.index),
                    static_cast<uint16_t>(new_frame_coarse),
                    static_cast<float>(BTANA_ALCOR_CC_TO_NS * new_frame_coarse));
                ev.is_secondary = sec;
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
            current_participants_mask[static_cast<uint8_t>(current_device)] |= encode_bits({(uint8_t)current_fifo});

            // Encode dead lane
            if (current_data.coarse_time_clock() != 0)
            {
                auto &current_dead_mask = spilldata.get_dead_mask_link();
                current_dead_mask[static_cast<uint8_t>(current_device)] |= encode_bits({(uint8_t)current_fifo});
            }
            continue;
        }

        //  ----    ----    ----    End of spill  ----    ----    ----
        if (current_data.is_end_spill())
            break;
    }
}

bool ParallelStreamingFramer::next_spill()
{
    spilldata.clear();
    _current_spill++;
    auto &frame_list = spilldata.get_frame_link();

    // --- First frames trigger
    for (auto i_frame = 0; i_frame < _first_frames_trigger; ++i_frame)
    {
        auto &current_lightdata = frame_list[i_frame];
        auto &current_trigger_hits = current_lightdata.trigger_hits;
        current_trigger_hits.push_back({TriggerFirstFrames, static_cast<uint16_t>(_frame_size / 2.), static_cast<float>(BTANA_ALCOR_CC_TO_NS * _frame_size / 2.)});
    }

    // --- Remove invalid streams before feeding to the parallel unit
    auto it = std::remove_if(data_streams.begin(), data_streams.end(), [](const AlcorDataStreamer &stream)
                             { return !stream.is_valid(); });
    for (auto it_ = it; it_ != data_streams.end(); it_++)
        mist::logger::warning("Invalid datastream discarded: " + it_->get_filename());
    data_streams.erase(it, data_streams.end());

    // Check machine cores for safe threading.
    // hardware_concurrency() may return 0 (unknown); guard against underflow before
    // subtracting the 2 reserved cores.
    unsigned int n_hw = std::thread::hardware_concurrency();
    unsigned int n_usable = (n_hw > 2) ? (n_hw - 2) : 0;
    unsigned int n_threads = n_threads_requested > 0
                                 ? std::min<unsigned>(n_threads_requested, 8 * std::min(n_usable, 2u))
                                 : std::max(1u, 8 * std::min(n_usable, 2u));

    std::atomic<size_t> next_streamer_atomic(0);
    std::atomic<size_t> completed(0);
    const size_t total = data_streams.size();

    mist::logger::info("(ParallelStreamingFramer::next_spill) Starting reading data streams");

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

                                                 // process() resolves the per-stream, per-spill
                                                 // rollover correction internally from the member
                                                 // table; no extra arguments are needed here.
                                                 process(my_stream, this->_frame_size);

                                                 size_t done = ++completed;
                                                 _update_bar(static_cast<int64_t>(done), static_cast<int64_t>(total));
                                             }
                                         }));
    }

    for (auto &f : thread_pool)
        f.get();

    _update_bar(static_cast<int64_t>(total), static_cast<int64_t>(total));

    mist::logger::info("(ParallelStreamingFramer::next_spill) Spill " +
                       std::to_string(_current_spill) +
                       " processing finished! Successfully processed " +
                       std::to_string(data_streams.size()) +
                       " data streams");

    return spilldata.has_data();
}