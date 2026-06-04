#include "parallel_streaming_framer.h"
#include <mist/logger/logger.h>
#include "alcor_data.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <tuple>
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
{
}

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
    // Create streams.  Invalid streamers are dropped HERE (in the ctor) so
    // the data_streams indices stay stable for the lifetime of the framer.
    // The previous per-spill erase in next_spill() would
    // have re-indexed streams between spills, which is incompatible with
    // rollover_correction_per_stream_and_spill — that table is built once
    // by resolve_rollover_offsets() against the ORIGINAL ordering.
    //
    // Construct in-place via emplace_back (NOT push_back(std::move(...))) so
    // the AlcorDataStreamer is never moved.  AlcorDataStreamer's ctor calls
    // data.link_to_tree(tree), binding ROOT branch addresses to the object's
    // own `data` fields; a subsequent move would leave those addresses
    // pointing at the moved-from source's freed memory — fatal at the next
    // tree->GetEntry().  AlcorDataStreamer's move-ctor now re-links to fix
    // that, but constructing in place is the cheaper and clearer pattern.
    data_streams.reserve(filenames.size());
    for (const auto &current_filename : filenames)
    {
        data_streams.emplace_back(current_filename);
        if (!data_streams.back().is_valid())
        {
            mist::logger::warning("Failed to open streamer: " + current_filename);
            data_streams.pop_back(); // drop invalid streamers up front
        }
    }

    // Load trigger configurations (read-only during processing; O(1) lookup via
    // trigger_config.by_device and trigger_config.by_channel — see ).
    trigger_config = trigger_conf_reader(trigger_config_file);

    // Readout configuration
    readout_config = ReadoutConfigList(readout_config_reader(readout_config_file));

    // Channel-mode triggers must also be registered in the readout config — the
    // per-chip readout filter further down breaks out of the stream loop before
    // the channel-mode lookup is even reached if the source chip isn't enrolled.
    // Warn at startup so silently-disabled triggers don't surprise the user.
    for (const auto &[key, ct] : trigger_config.by_channel)
    {
        const bool registered =
            !readout_config.find_by_device_and_chip(ct.device, ct.fifo / 4).empty();
        if (!registered)
            mist::logger::warning(
                "(ParallelStreamingFramer) Channel-mode trigger \"" + ct.name +
                "\" targets device=" + std::to_string(ct.device) +
                " fifo=" + std::to_string(ct.fifo) +
                " (chip=" + std::to_string(ct.fifo / 4) + ")"
                                                          " which is not registered in the readout config — trigger will never fire.");
    }

    // Initialize spill counter
    _current_spill = -1;

    // Initialise the fine tune container.  RootHist<T> ownership: closes
    // §B-1 from the post-migration audit — the histogram is now RAII-owned
    // so the dtor body is empty and any exception between ctor and dtor no
    // longer leaks.
    h2_fine_tune_distribution = RootHist<TH2F>("h2_fine_tune_distribution",
                                               ";global tdc index;fine parameter",
                                               1e4, -0.5, 1e4 - 0.5, 256, 0, 256);
    // Same-channel Δt diagnostic — one entry per Hit (skipping each channel's
    // first Hit).  Range chosen to span ~100 µs (32768 cc) so DCR-driven
    // long-Δt entries land in the in-range part of the histogram rather than
    // overflowing the previous narrow [0, 1024] axis (post-migration audit
    // Bin width = 32 cc ≈ 100 ns — coarse enough for the long tail,
    // fine enough to resolve the QA near (1–64 cc) and far (256–319 cc)
    // windows.
    h_afterpulse_dt = RootHist<TH1F>("h_afterpulse_dt",
                                     ";#Delta_{t} to previous same-channel Hit (cc);entries",
                                     1024, 0, 32768);
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
                mist::logger::warning(TString::Format(
                                          "(ParallelStreamingFramer::resolve_rollover_offsets) "
                                          "Stream %s spill %zu: rollover offset %.0f exceeds 1 tick "
                                          "(stream = %.0f, reference = %.0f) — skipping correction",
                                          current_stream.get_filename().c_str(),
                                          i_spill,
                                          rollover_offset,
                                          rollover_vector[i_spill],
                                          reference_rollover_per_spill[i_spill])
                                          .Data());
                continue;
            }

            const uint64_t rollover_correction_cc =
                static_cast<uint64_t>(rollover_offset) *
                static_cast<uint64_t>(BTANA_ALCOR_ROLLOVER_TO_CC);
            rollover_correction_per_stream_and_spill[i_stream][i_spill] = rollover_correction_cc;
            ++n_corrections_applied;

            mist::logger::warning(TString::Format(
                                      "(ParallelStreamingFramer::resolve_rollover_offsets) "
                                      "Stream %s spill %zu: rollover = %.0f, reference = %.0f, correction = +%.0f ticks",
                                      current_stream.get_filename().c_str(),
                                      i_spill,
                                      rollover_vector[i_spill],
                                      reference_rollover_per_spill[i_spill],
                                      rollover_offset)
                                      .Data());
        }
    }

    mist::logger::info("(ParallelStreamingFramer::resolve_rollover_offsets) Applied " +
                       std::to_string(n_corrections_applied) +
                       " non-zero corrections across " +
                       std::to_string(data_streams.size()) + " streams and " +
                       std::to_string(n_spills_aligned) + " spills");
}

// I/O operations
void ParallelStreamingFramer::process(size_t stream_index, WorkerQA *qa)
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
        //  _current_spill is initialised to -1 and only becomes a valid
        //  index after the first start_spill — guard explicitly to
        //  avoid the implicit `static_cast<size_t>(-1) == SIZE_MAX`
        //  trap that would silently pass any signed/unsigned compare
        //  against a smaller vector.
        if (_current_spill >= 0 &&
            static_cast<size_t>(_current_spill) < stream_corrections.size())
            rollover_correction_cc = stream_corrections[_current_spill];
    }

    // Local instance of result.  When called from next_spill() each worker
    // gets its own private frame_map via qa — writes to it are lock-free.
    // The qa==nullptr fallback (only used by single-threaded test drivers)
    // writes directly into the master under the shared frame_mutexes_access
    // lock taken at each write site below.
    auto &frame_map = qa ? qa->frame_map : spilldata.get_frame_link();
    const bool use_shared_frame_map = (qa == nullptr);
    // Map: global channel index → previous-Hit global clock cycle for that channel.
    // Drives BOTH the framer-internal afterpulse mask (Δt < _afterpulse_deadtime)
    // and the QA sideband tagging (Δt in near / far windows from _qa_cfg).
    std::unordered_map<int, uint64_t> prev_hit_time_map;
    // Maps trigger index → first global clock cycle after which the next firing is no longer secondary.
    std::unordered_map<uint8_t, uint64_t> trigger_secondary_map;

    // Precompute the QA far-window bounds from _qa_cfg.
    // Far window mirrors the near window's width and starts at the sideband offset.
    const int64_t qa_near_lo = _qa_cfg.afterpulse_near_lo;
    const int64_t qa_near_hi = _qa_cfg.afterpulse_near_hi;
    const int64_t qa_far_lo = _qa_cfg.afterpulse_sideband_offset;
    const int64_t qa_far_hi = _qa_cfg.afterpulse_sideband_offset + (_qa_cfg.afterpulse_near_hi - _qa_cfg.afterpulse_near_lo);

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
        // Chip 8 (fifo 32-35) is the hardware trigger chip — always continue for it.
        // TODO: outsource this to trigger_conf (per-trigger fifo) so it isn't hardcoded.
        constexpr int trigger_chip = 32 / 4; // = 8
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
            // Channel-mode trigger: a data-tagged word on a configured channel
            // is forced into the trigger path (see DISCUSSION.md → ).
            // Check this FIRST so the channel never touches the data-hit path
            // (no afterpulse bookkeeping, no readout-tag categorisation).
            {
                const uint64_t ch_key = pack_channel_key(
                    static_cast<uint16_t>(current_device),
                    static_cast<uint16_t>(current_data.get_fifo()),
                    static_cast<uint8_t>(current_data.get_column()),
                    static_cast<uint8_t>(current_data.get_pixel()));
                if (auto it = trigger_config.by_channel.find(ch_key);
                    it != trigger_config.by_channel.end())
                {
                    const auto &ct = it->second;
                    bool sec = false;
                    if (auto sm = trigger_secondary_map.find(ct.index); sm != trigger_secondary_map.end())
                        sec = hit_frame_coarse_global < sm->second;
                    trigger_secondary_map[ct.index] = hit_frame_coarse_global + _trigger_secondary_window;

                    const uint32_t new_frame_index =
                        (hit_frame_coarse_global - ct.delay) / (_frame_size * 1.);
                    const uint64_t new_frame_coarse =
                        (hit_frame_coarse_global - ct.delay) % static_cast<uint64_t>(_frame_size);

                    std::unique_lock<std::mutex> map_lock(frame_mutexes_access, std::defer_lock);
                    if (use_shared_frame_map)
                        map_lock.lock();
                    auto &ev = frame_map[new_frame_index].trigger_hits.emplace_back(
                        ct.index,
                        static_cast<uint16_t>(new_frame_coarse),
                        static_cast<float>(BTANA_ALCOR_CC_TO_NS * new_frame_coarse));
                    ev.is_secondary = sec;
                    continue;
                }
            }

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

            // Per-worker frame_map (qa->frame_map) → no lock needed.
            // Fallback test path (qa==nullptr) still serialises via the
            // shared mutex so existing single-threaded callers keep working.
            {
                std::unique_lock<std::mutex> map_lock(frame_mutexes_access, std::defer_lock);
                if (use_shared_frame_map)
                    map_lock.lock();
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
            }
            if (qa)
            {
                // h2_fine_tune_distribution is sized [0, 10000] on the X axis
                // — the legacy compact "global tdc index" range.  Under Phase-5
                // storage, get_global_tdc_index() returns the packed 32-bit raw
                // value (millions), which would overflow the axis on every fill.
                // Use the dense ordinal helper to recover the legacy bin index.
                qa->h2_fine_tune->Fill(::GlobalIndex(current_data.get_global_tdc_index()).tdc_ordinal(),
                                       current_data.get_fine());
                if (same_channel_dt >= 0)
                    qa->h_afterpulse->Fill(static_cast<double>(same_channel_dt));
            }
            continue;
        }

        //  ----    ----    ----    Trigger Hit  ----    ----    ----
        if (current_data.is_trigger_tag())
        {
            // Device-mode lookup: the hardware tag is the discriminator —
            // the device alone identifies which configured trigger fired
            // (see DISCUSSION.md → ).  O(1) hash lookup, no scoring.
            auto it = trigger_config.by_device.find(static_cast<uint16_t>(current_device));
            const bool trigger_known = (it != trigger_config.by_device.end());

            if (!trigger_known)
            {
                // Log unrecognised tagged-trigger devices once. Set membership
                // is the only thing under the mutex — string formatting and
                // mist::logger::warning() are released to keep other workers
                // off this lock
                bool first_time = false;
                {
                    std::lock_guard<std::mutex> trig_lock(triggers_map_mutex);
                    first_time = unknown_trigger_devices_seen.insert(
                                                                 static_cast<uint16_t>(current_device))
                                     .second;
                }
                if (first_time)
                    mist::logger::warning(
                        "(ParallelStreamingFramer) Tagged trigger from "
                        "unconfigured device=" +
                        std::to_string(current_device) +
                        " — emitting as _TRIGGER_UNKNOWN_.");

                // Unknown trigger — write to original frame at the original timing.
                const uint8_t unknown_idx = static_cast<uint8_t>(_TRIGGER_UNKNOWN_);
                bool sec = false;
                if (auto sm = trigger_secondary_map.find(unknown_idx); sm != trigger_secondary_map.end())
                    sec = hit_frame_coarse_global < sm->second;
                trigger_secondary_map[unknown_idx] = hit_frame_coarse_global + _trigger_secondary_window;
                {
                    // Per-worker frame_map → no lock; fallback test path locks.
                    std::unique_lock<std::mutex> map_lock(frame_mutexes_access, std::defer_lock);
                    if (use_shared_frame_map)
                        map_lock.lock();
                    auto &ev = frame_map[hit_frame_index].trigger_hits.emplace_back(
                        unknown_idx,
                        static_cast<uint16_t>(current_device),
                        static_cast<float>(hit_frame_coarse * BTANA_ALCOR_CC_TO_NS));
                    ev.is_secondary = sec;
                }
                continue;
            }

            // Known trigger — apply delay correction and write to the corrected frame.
            const auto &cfg = it->second;
            bool sec = false;
            if (auto sm = trigger_secondary_map.find(cfg.index); sm != trigger_secondary_map.end())
                sec = hit_frame_coarse_global < sm->second;
            trigger_secondary_map[cfg.index] = hit_frame_coarse_global + _trigger_secondary_window;

            const uint32_t new_frame_index =
                (hit_frame_coarse_global - cfg.delay) / (_frame_size * 1.);
            const uint64_t new_frame_coarse =
                (hit_frame_coarse_global - cfg.delay) % static_cast<uint64_t>(_frame_size);

            {
                // Per-worker frame_map → no lock; fallback test path locks.
                std::unique_lock<std::mutex> map_lock(frame_mutexes_access, std::defer_lock);
                if (use_shared_frame_map)
                    map_lock.lock();
                auto &ev = frame_map[new_frame_index].trigger_hits.emplace_back(
                    cfg.index,
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
    //
    // frame_list is an unordered_map (hashed buckets, O(1) average insert),
    // so the previous ascending-order insertion-hint trick is unnecessary.
    // Reserve once up front so the run of 5000 inserts doesn't trigger any
    // rehash + bucket reshuffle mid-loop.
    frame_list.reserve(frame_list.size() + static_cast<size_t>(_first_frames_trigger));
    for (auto i_frame = 0; i_frame < _first_frames_trigger; ++i_frame)
    {
        auto [it, _] = frame_list.try_emplace(i_frame);
        it->second.trigger_hits.push_back({TriggerFirstFrames,
                                           static_cast<uint16_t>(_frame_size / 2.),
                                           static_cast<float>(BTANA_ALCOR_CC_TO_NS * _frame_size / 2.)});
    }

    // Invalid streams were already dropped in the constructor, so
    // data_streams here is guaranteed to contain only valid streamers and
    // its indexing is stable across spills (matches the rollover_correction
    // table;).

    // Determine worker thread count.  Clamped by three independent factors:
    //
    //  - kMaxWorkers (16): conservative ceiling — empirically chosen to avoid
    //    I/O contention on the filesystem when many readers hit gRollover and
    //    branch caches simultaneously.  Raise this constant after benchmarking
    //    if your target node has more I/O headroom.
    //
    //  - n_usable: hardware_concurrency() minus 2 reserved cores (one for the
    //    progress-bar / main thread, one for OS noise).  Underflow-guarded so
    //    machines reporting 0 or unknown hardware concurrency still get one
    //    worker.
    //
    //  - n_streams: pointless to spawn more workers than work items.  Workers
    //    feed off a single atomic queue (next_streamer_atomic); extras grab
    //    my_stream >= total immediately and exit, paying pthread create cost
    //    for no gain.  At 192 streams this rarely bites, but at small DCR
    //    configs (4–8 streams) the saving is real.
    //
    // fix: the previous formula `8 * std::min(n_usable, 2u)`
    // is algebraically equivalent to a hard ceiling of 16 on every machine
    // with ≥ 4 cores, regardless of n_usable.  64-core nodes performed
    // identically to 4-core laptops.  Replaced with an explicit
    // `min(n_usable, kMaxWorkers)` so the cap is greppable and the intermediate
    // value scales with hardware up to the cap.
    constexpr unsigned int kMaxWorkers = 16;
    const unsigned int n_hw = std::thread::hardware_concurrency();
    const unsigned int n_usable = (n_hw > 2) ? (n_hw - 2) : 0;
    const unsigned int n_streams = static_cast<unsigned int>(data_streams.size());
    const unsigned int n_threads = (n_threads_requested > 0)
                                       ? std::min({static_cast<unsigned int>(n_threads_requested), kMaxWorkers, n_streams})
                                       : std::min({std::max(1u, n_usable), kMaxWorkers, n_streams});

    std::atomic<size_t> next_streamer_atomic(0);
    std::atomic<size_t> completed(0);
    const size_t total = data_streams.size();

    mist::logger::info("(ParallelStreamingFramer::next_spill) Starting reading data streams");

    // Per-worker QA-histogram clones.  Each worker fills its own copy without
    // taking frame_mutexes_access — pulling the ROOT TH1::Fill() calls out
    // of the per-Hit critical section was the primary fix for the framer's
    // system-CPU contention.  Clones are detached from gDirectory so they
    // don't accidentally land in an open TFile; they are merged into the
    // master histograms and freed at the end of this spill.
    std::vector<WorkerQA> worker_qas(n_threads);
    for (size_t i = 0; i < n_threads; ++i)
    {
        worker_qas[i].h2_fine_tune = static_cast<TH2F *>(
            h2_fine_tune_distribution->Clone(
                TString::Format("h2_fine_tune_distribution_worker_%zu_spill_%d",
                                i, static_cast<int>(_current_spill))
                    .Data()));
        worker_qas[i].h2_fine_tune->SetDirectory(nullptr);
        worker_qas[i].h2_fine_tune->Reset();
        worker_qas[i].h_afterpulse = static_cast<TH1F *>(
            h_afterpulse_dt->Clone(
                TString::Format("h_afterpulse_dt_worker_%zu_spill_%d",
                                i, static_cast<int>(_current_spill))
                    .Data()));
        worker_qas[i].h_afterpulse->SetDirectory(nullptr);
        worker_qas[i].h_afterpulse->Reset();
    }

    // Per-spill std::async spawn cost was measured at < 0.01% of per-spill
    // work on a realistic load; no worker
    // pool needed.  Instrumentation removed.
    std::vector<std::future<void>> thread_pool;
    thread_pool.reserve(n_threads);

    for (size_t i = 0; i < n_threads; ++i)
    {
        thread_pool.push_back(std::async(std::launch::async,
                                         [this, &next_streamer_atomic, &completed, total, &worker_qas, i]()
                                         {
                                             while (true)
                                             {
                                                 size_t my_stream = next_streamer_atomic.fetch_add(1);
                                                 if (my_stream >= total)
                                                     return;

                                                 // process() reads _frame_size and per-stream
                                                 // rollover correction from members; only the
                                                 // stream index and per-worker scratch are passed.
                                                 process(my_stream, &worker_qas[i]);

                                                 size_t done = ++completed;
                                                 _update_bar(static_cast<int64_t>(done), static_cast<int64_t>(total));
                                             }
                                         }));
    }

    for (auto &f : thread_pool)
        f.get();

    // Merge per-worker frame maps into the master spilldata.frame_and_lightdata.
    // Workers wrote to their own maps without holding frame_mutexes_access —
    // pulling that lock out of the per-Hit path was the primary throughput
    // fix.  This merge step is single-threaded, runs once per spill, and
    // walks each worker's accumulated frames; duplicated frame keys are
    // combined via merge_lightdata (which concatenates the per-bucket Hit
    // vectors).  Mirrors the inner loop of ::merge(AlcorSpilldataStruct&,
    // AlcorSpilldataStruct&&) but reuses the framer's own master map so
    // any first-frames trigger seeds populated earlier in next_spill() are
    // preserved.
    auto &master_frame_map = spilldata.get_frame_link();
    for (auto &qa : worker_qas)
    {
        for (auto &[frame_index, lightdata] : qa.frame_map)
        {
            auto [it, inserted] = master_frame_map.try_emplace(frame_index, std::move(lightdata));
            if (!inserted)
                merge_lightdata(it->second, std::move(lightdata));
        }
        qa.frame_map.clear();
    }

    //  Post-merge canonicalisation pass — guarantees bit-identical
    //  output across runs regardless of how the work-stealing atomic
    //  counter (line 622) distributed streams across worker threads.
    //
    //  Why this is needed: each worker accumulates hits into its
    //  per-thread frame_map in source-stream-traversal order; at merge
    //  time the per-worker frames are concatenated onto the master
    //  frame's hit vectors via merge_lightdata.  Which streams a given
    //  worker processed depends on OS scheduling, so the per-worker
    //  block contents of each frame's final hit vector shuffle from
    //  run to run.  Downstream consumers that iterate the vector in
    //  order (notably MIST's `collect_ring_hits`, which assigns
    //  border-line hits to whichever ring is found first) then produce
    //  slightly different output.  Sorting the vectors by a stable
    //  hardware-derived key fixes the order in a way the schedule
    //  cannot perturb.
    //
    //  Cost: O(N log N) per frame on N ≤ ~50 hits / frame — negligible
    //  vs the per-stream decoding work above.  std::sort is stable
    //  enough here because the key is strict (no duplicate (device,
    //  fifo, column, pixel, tdc, rollover, coarse, fine) tuples in a
    //  spill by construction).
    auto cmp_finedata = [](const AlcorFinedataStruct &a,
                           const AlcorFinedataStruct &b)
    {
        return std::tie(a.GlobalIndex, a.rollover, a.coarse, a.fine) < std::tie(b.GlobalIndex, b.rollover, b.coarse, b.fine);
    };
    auto cmp_trigger = [](const TriggerEvent &a, const TriggerEvent &b)
    {
        return std::tie(a.index, a.coarse, a.fine_time) < std::tie(b.index, b.coarse, b.fine_time);
    };
    for (auto &[frame_index, ld] : master_frame_map)
    {
        std::sort(ld.cherenkov_hits.begin(), ld.cherenkov_hits.end(), cmp_finedata);
        std::sort(ld.timing_hits.begin(), ld.timing_hits.end(), cmp_finedata);
        std::sort(ld.tracking_hits.begin(), ld.tracking_hits.end(), cmp_finedata);
        std::sort(ld.trigger_hits.begin(), ld.trigger_hits.end(), cmp_trigger);
    }

    // Merge the per-worker QA clones into the master histograms, then free.
    // TH1::Add is serial-only — runs in the calling (writer) thread after the
    // worker barrier above, so no lock is needed.
    for (auto &qa : worker_qas)
    {
        if (qa.h2_fine_tune)
        {
            h2_fine_tune_distribution->Add(qa.h2_fine_tune);
            delete qa.h2_fine_tune;
            qa.h2_fine_tune = nullptr;
        }
        if (qa.h_afterpulse)
        {
            h_afterpulse_dt->Add(qa.h_afterpulse);
            delete qa.h_afterpulse;
            qa.h_afterpulse = nullptr;
        }
    }

    _update_bar(static_cast<int64_t>(total), static_cast<int64_t>(total));

    mist::logger::info("(ParallelStreamingFramer::next_spill) Spill " +
                       std::to_string(_current_spill) +
                       " processing finished! Successfully processed " +
                       std::to_string(data_streams.size()) +
                       " data streams");

    return spilldata.has_data();
}