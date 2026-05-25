#include "parallel_streaming_framer.h"
#include "writers/lightdata.h"
#include "mapping.h"
#include <mist/ring_finding/hough_transform.h>
#include "TProfile2D.h"
#include "TNamed.h"
#include "TParameter.h"
#include <algorithm>
#include <numeric>

// Config (to be sourced from readout_config_file)
constexpr int kTimingChip0Id = 0, kTimingChip1Id = 2;
constexpr int kTimingChip0Expected = 32, kTimingChip1Expected = 31;
// Guard: the mean-of-others formula divides by (kTimingChipXExpected - 1).
// Both constants must be >= 2.
static_assert(kTimingChip0Expected >= 2, "kTimingChip0Expected must be >= 2 to avoid divide-by-zero");
static_assert(kTimingChip1Expected >= 2, "kTimingChip1Expected must be >= 2 to avoid divide-by-zero");
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
    std::string fine_calibration_config_file,
    std::string framer_conf_file)
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
    //  Create progress tracking — single multi-bar:
    //    main bar  : overall spill progress (X / max_spill spills)
    //    subtask 1 : per-spill framing (driven by ParallelStreamingFramer)
    //    subtask 2 : per-spill post-processing (driven below in the frame loop)
    //
    //  Subtask timers are restarted at the head of every spill iteration so
    //  the elapsed columns reflect THIS spill's work, not cumulative time —
    //  using the mist::logger::MultiProgressBar::restart() primitive added
    //  in plan-(3).
    //
    //  When @p max_spill is non-positive we pass it as-is to the main bar's
    //  update() which switches to "unknown-total" mode (no percentage / ETA,
    //  just a running spill count and elapsed time).
    mist::logger::MultiProgressBar progress_bars(mist::logger::BarStyle::Block);
    progress_bars.set_unit("spills");
    auto &progress_framer         = progress_bars.add_subtask("framer");
    auto &progress_postprocessing = progress_bars.add_subtask("post-processing");
    progress_bars.update(0, max_spill); // arms the main bar in the correct mode
    int streaming_trigger = 0;

    //  Load framer + QA configuration (both live in framer_conf_file)
    auto framer_cfg = FramerConfReader(framer_conf_file);
    auto qa_cfg     = qa_conf_reader(framer_conf_file);

    //  Create streaming framer
    ParallelStreamingFramer framer(filenames, trigger_setup_file, readout_config_file, framer_cfg);
    framer.set_qa_config(qa_cfg);       // enable afterpulse near/far Hit-mask tagging
    framer.set_parallel_cores(requested_n_threads);
    //framer.resolve_rollover_offsets();
    framer.assign_bar(progress_framer); // framer drives the framer subtask automatically

    auto config_triggers = trigger_conf_reader(trigger_setup_file);
    TriggerRegistry registry(config_triggers);
    //  Prepare output file & tree
    std::string outfile_name = data_repository + "/" + run_name + "/lightdata.root";
    if (std::filesystem::exists(outfile_name) && !force_lightdata_rebuild)
    {
        mist::logger::info("[INFO] Output file already exists, skipping: " + outfile_name);
        return;
    }
    //  Generate Mapping
    Mapping current_mapping(mapping_config_file);
    //  Load fine_calibration
    AlcorFinedata::read_calib_from_file(fine_calibration_config_file);
    //  Link output tree
    TFile *outfile = TFile::Open(outfile_name.c_str(), "RECREATE");
    auto &spilldata = framer.get_spilldata_link();
    TTree *lightdata_tree = new TTree("lightdata", "Lightdata tree");
    spilldata.write_to_tree(lightdata_tree);
    //  ---
    //  QA Plots
    //  ---
    //  --- Rollover offset QA
    //  Per-stream, per-spill correction applied during framing, expressed in
    //  rollover ticks (clock cycles / BTANA_ALCOR_ROLLOVER_TO_CC). Bins sized to
    //  accommodate the actual table dimensions at fill time.
    TH2F *h_rollover_correction_ticks_per_stream_and_spill = nullptr;
    TH1F *h_rollover_correction_ticks_distribution = new TH1F(
        "h_rollover_correction_ticks_distribution",
        ";rollover correction (ticks);(stream,spill) entries",
        10, -0.5, 9.5);
    TH1F *h_rollover_correction_affected_streams_per_spill = new TH1F(
        "h_rollover_correction_affected_streams_per_spill",
        ";spill index;streams requiring correction",
        1000, -0.5, 999.5);
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
    std::unordered_map<int, TH2F *> h_trigger_dt;

    //  Log-spaced Y-axis edges (Δt in ns) shared across all per-trigger TH2Fs.
    //  Range: 1 ns → 1e10 ns (≈10 s, comfortably covers a 5 s spill at 320 MHz).
    constexpr int kTriggerDtNBinsY = 60;
    std::vector<double> trigger_dt_log_edges(kTriggerDtNBinsY + 1);
    for (int i = 0; i <= kTriggerDtNBinsY; ++i)
        trigger_dt_log_edges[i] = std::pow(10.0, 10.0 * static_cast<double>(i) / kTriggerDtNBinsY);
    //  ---
    //  --- Timing
    TH2F *h_timing_hit_map = new TH2F("h_timing_hit_map", ";channels on chip 0; channels on chip 1", 33, 0, 33, 33, 0, 33);
    TH1F *h_timing_ref_delta = new TH1F("h_timing_ref_delta", ";timing chip 0 - timing chip 1", 250, -5, 5);
    TH1F *h_timing_ref_delta_sel = new TH1F("h_timing_ref_delta_sel", ";timing chip 0 - timing chip 1", 250, -5, 5);
    //  ---
    //  --- DCR
    TProfile *h_dcr_per_channel = new TProfile("h_dcr_per_channel", ";channel;DCR [kHz];", 2048, 0, 2048);
    //  Smeared DCR hitmap — one fill per cherenkov Hit during noise (first-frames)
    //  trigger frames, at the channel's ±1.5 mm smeared physical position.  Bin
    //  contents are total Hit counts; divide by (n_dcr_frames × frame_length × bin_area)
    //  for a rate.  Density ∝ DCR rate, as in the CT / AP smeared maps.
    TH2F *h_dcr_hitmap = new TH2F("h_dcr_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    //  --- Afterpulse
    //  Per-channel afterpulse profiles.
    //  Near = afterpulse signal + DCR baseline ; far = DCR sideband only.
    //  Subtracted (true afterpulse) = signed-weight fill on the same Hit set.
    TProfile *h_afterpulse_near_per_channel = new TProfile("h_afterpulse_near_per_channel",
        ";channel;Near-window same-channel probability (%);", 2048, 0, 2048);
    TProfile *h_afterpulse_far_per_channel  = new TProfile("h_afterpulse_far_per_channel",
        ";channel;Far-window same-channel probability (%);", 2048, 0, 2048);
    TProfile *h_afterpulse_per_channel      = new TProfile("h_afterpulse_per_channel",
        ";channel;Afterpulse probability (DCR-subtracted) (%);", 2048, 0, 2048);
    //  Smeared 2D hitmaps — per primary Hit we deposit 100 fills at independent
    //  ±1.5 mm smeared positions when the Hit lies in the relevant window.  Density
    //  in the resulting TH2F is therefore proportional to the corresponding
    //  probability, in the same units as the per-channel profiles.
    //
    //  The "subtracted" map uses ±1 weights so per-bin contents = (n_near − n_far),
    //  i.e. density ∝ true afterpulse probability.  May go negative in DCR-only
    //  bins by Poisson fluctuation — that's the expected zero-bias behaviour.
    TH2F *h_afterpulse_near_hitmap = new TH2F("h_afterpulse_near_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH2F *h_afterpulse_far_hitmap  = new TH2F("h_afterpulse_far_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH2F *h_afterpulse_hitmap      = new TH2F("h_afterpulse_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    h_afterpulse_hitmap->Sumw2();   // signed-weight fills → needs squared-weight tracking
    //  --- Cross-talk per-channel profiles
    TProfile *h_phys_ct_per_channel = new TProfile("h_phys_ct_per_channel",
        ";channel;Physical CT probability (%);", 2048, 0, 2048);
    TProfile *h_elec_ct_per_channel = new TProfile("h_elec_ct_per_channel",
        ";channel;Electrical CT probability (%);", 2048, 0, 2048);
    //  Smeared CT hitmaps — n_ct_neighbours × 100 fills per primary Hit, each at
    //  an independent ±1.5 mm smeared position.  Density ∝ CT rate per spatial bin.
    TH2F *h_phys_ct_hitmap = new TH2F("h_phys_ct_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH2F *h_elec_ct_hitmap = new TH2F("h_elec_ct_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    //  --- CT neighbour-pair Δt distributions (signal peak + DCR sideband)
    //  Physical CT signal window: [0, 10] cc.
    //  Electrical CT signal window: [-2, 10] cc (small negative allowed for readout-timing jitter).
    //  Sideband for DCR baseline: beyond the signal window.
    TH1F *h_phys_ct_dt = new TH1F("h_phys_ct_dt", ";#Delta_{t} (cc);physical neighbour pairs / primary Hit", 200, 0, 200);
    TH1F *h_elec_ct_dt = new TH1F("h_elec_ct_dt", ";#Delta_{t} (cc);electrical neighbour pairs / primary Hit", 210, -10, 200);
    //  --- CT 2D diagnostic: (Δchannel, Δt) for every in-frame pair — no neighbour
    //  definition needed; CT clusters near the origin, DCR is flat in Δt.
    //  Per-neighbour-type (Δchannel, Δt) diagnostics
    //  Electrical: same device+FIFO → Δchannel naturally constrained to ±7 (8 ch/FIFO).
    TH2F *h_elec_ct_dchannel_dt = new TH2F("h_elec_ct_dchannel_dt",
                                           ";#Delta channel (j #minus i, same FIFO);#Delta_{t} (cc)",
                                           17, -8.5, 8.5,
                                           26, -5.5, 20.5);
    //  Physical: distance ≤ 3.2 mm → most pairs within a chip (±32 ch) but cross-chip
    //  neighbours can land further out; range chosen to comfortably contain in-chip pairs.
    TH2F *h_phys_ct_dchannel_dt = new TH2F("h_phys_ct_dchannel_dt",
                                           ";#Delta channel (j #minus i, #leq 3.2 mm);#Delta_{t} (cc)",
                                           65, -32.5, 32.5,
                                           26, -5.5, 20.5);
    //  ---
    //  --- Streaming Trigger
    TH2F *h_streaming_trigger_frames_examples = new TH2F("h_streaming_trigger_frames_examples", ";x (mm);y (mm)", 1000, 0, 1000, framer_cfg.frame_size, 0, framer_cfg.frame_size);
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
    TH1F *h_streaming_trigger_delta_t_leading_edge = new TH1F("h_streaming_trigger_delta_t_leading_edge", ";#Delta_{t} (t_{Hit} - t_{avg}) ns", 1000 * (time_window_ns), 0., time_window_ns);
    TH1F *h_streaming_trigger_delta_t_half_centroid = new TH1F("h_streaming_trigger_delta_t_half_centroid", ";#Delta_{t} (t_{Hit} - t_{avg}) ns", 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH1F *h_streaming_trigger_delta_t_first_half = new TH1F("h_streaming_trigger_delta_t_first_half", ";#Delta_{t} (t_{Hit} - t_{avg}) ns", 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH1F *h_streaming_trigger_delta_t_second_half = new TH1F("h_streaming_trigger_delta_t_second_half", ";#Delta_{t} (t_{Hit} - t_{avg}) ns", 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH2F *h_streaming_trigger_sigma_vs_nhits = new TH2F("h_streaming_trigger_sigma_vs_nhits", ";n_{hits} in peak window;|odd_median - even_median| ns", 50, 0, 50, 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH2F *h_streaming_trigger_median_vs_window = new TH2F("h_delta_median_vs_window", ";t_{last} - t_{first} in peak window ns;|odd_median - even_median| ns", 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns, 1000 * (time_window_ns * 2), -time_window_ns, time_window_ns);
    TH1F *h_streaming_trigger_tdc_step_sizes = new TH1F("h_streaming_trigger_tdc_step_sizes", ";t_{i+1} - t_{i} ns;entries", 1000, 0., 1.);
    TH1F *h_streaming_trigger_tdc_zero_times = new TH1F("h_streaming_trigger_tdc_zero_times", ";t_{Hit} ns;entries", 10000, 0., framer_cfg.frame_length_ns());
    TH1F *h_streaming_trigger_tdc_zero_cluster_size = new TH1F("h_streaming_trigger_tdc_zero_cluster_size", ";n_{hits} in peak;entries", 50, 0., 50.);
    //  ---
    //  End: Framing data & output definition
    //  --- --- --- --- --- ---

    //  --- --- --- --- --- ---
    //  Loop on data
    //  ---
    //  Cache positions
    //  Phase 5: iterate (device, chip, channel) directly via the
    //  GlobalIndex overload.  Both caches keyed by `4 * channel_ordinal`
    //  (dense int) — same as the MIST HoughTransform `lut_key`.
    std::map<int, std::array<float, 2>> index_to_hit_xy;
    std::map<std::array<float, 2>, int> hit_to_index_xy;
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
                    const int key = 4 * gi.channel_ordinal();
                    index_to_hit_xy[key] = *position;
                    hit_to_index_xy[*position] = key;
                }
    }
    mist::ring_finding::HoughTransform ring_finder(index_to_hit_xy, 20, 120, 1., 3.);

    //  ---
    //  Loop over spills
    if (max_spill != 1000)
        mist::logger::info("(ParallelStreamingFramer::next_spill) Requested to stop at spill : " +
                           std::to_string(max_spill));

    // Use a while-loop instead of a for-loop so we can restart the multi-bar
    // BEFORE the next next_spill() call — which itself updates the framer
    // subtask, so the restart must precede it to actually reset the clock.
    for (int ispill = 0; ispill < max_spill; ++ispill)
    {
        //  ── Per-spill progress reset ─────────────────────────────────────
        //  From the second spill onward, restart the multi-bar so the two
        //  subtask clocks (framer + post-processing) begin at zero for this
        //  spill.  Without this they would accumulate elapsed time across
        //  spills (the original bug that motivated plan-(3) in mist).
        if (ispill > 0)
            progress_bars.restart(/*flush=*/false);
        //  Advance the main spill counter.  In unknown-total mode
        //  (max_spill <= 0) this just shows "N spills  elapsed: …".
        progress_bars.update(ispill, max_spill);

        //  Now drive the framer for this spill — its internal update() calls
        //  Hit the framer subtask whose clock was just reset above.
        if (!framer.next_spill())
            break;

        //  Generate the calibration at each spill if new channels get available
        //spilldata.update_calibration(framer.get_fine_tune_distribution());

        //  Calculate participants channel
        // Phase 4 internal cleanup: the "active sensors" set is keyed by
        // GlobalIndex::global_channel_raw() instead of the legacy
        // `legacy_raw / 4` pattern.  The lookup at line ~725 below uses the
        // same expression, keeping the set ↔ count-map keys in sync.
        std::set<uint32_t> active_sensors;
        std::unordered_map<uint32_t, uint16_t> active_sensors_count;
        auto lanes_participating = spilldata.get_not_dead_participants();
        int n_active_cherenkov_channels = 0;
        for (auto [device, lanes] : lanes_participating)
            if (device < 200)
                for (auto current_lane : lanes)
                    for (auto i_channel = 0; i_channel < 8; ++i_channel)
                    {
                        // Phase 5: construct the new-layout GlobalIndex
                        // directly from the hardware identifiers; apply the
                        // split-in-two trick at the conversion boundary.
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
                        active_sensors.insert(gi.global_channel_raw());
                    }

        n_active_cherenkov_channels = active_sensors.size();

        //  Streaming trigger utilities
        const float cherenkov_fraction = 0.004f;
        const int threshold = 10000; //5; //std::max(1, static_cast<int>(std::ceil(cherenkov_fraction * n_active_cherenkov_channels)));
        std::vector<std::pair<int, float>> carry_over_hits;

        //  Info
        mist::logger::info("(lightdata_writer) Spill " +
                           std::to_string(ispill) +
                           " has " +
                           std::to_string(n_active_cherenkov_channels) +
                           " active channels");

        //  Timing calibration at first spill
        if (ispill == 0)
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
                    AlcorFinedata Hit(raw_hit);
                    const int chip = Hit.get_chip();
                    // Renamed from `GlobalIndex` to avoid shadowing the new
                    // GlobalIndex value type defined in <GlobalIndex.h>.
                    // Stored value remains the legacy TDC-level raw integer.
                    const int gidx_legacy = Hit.get_global_index();
                    const int channel = Hit.get_global_channel_index();
                    const float time_cc = Hit.get_time();

                    if (chip == kTimingChip0Id && !tdc_time_0.count(gidx_legacy))
                    {
                        tdc_time_0[gidx_legacy] = time_cc;
                        seen_channels_0.insert(channel);
                    }
                    if (chip == kTimingChip1Id && !tdc_time_1.count(gidx_legacy))
                    {
                        tdc_time_1[gidx_legacy] = time_cc;
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
                AlcorFinedata::set_param2(gi, -(sum / tdc_offset_count_0[gi]));
            for (auto &[gi, sum] : tdc_offset_sum_1)
                AlcorFinedata::set_param2(gi, -(sum / tdc_offset_count_1[gi]));

            //  Add the plot to dinamically determine the timing cuts > determine the highest bin excluding zeros, delta times without any further check look for
        }

        mist::logger::info("(lightdata_writer) Starting processing data streams in frames");
        const auto total_frames = static_cast<int>(spilldata.get_frame_link().size());
        // Last global clock cycle seen per trigger index — reset each spill so
        // the last trigger of one spill never contributes a delta with the next.
        std::unordered_map<int, uint64_t> trigger_last_global_cc;
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
                AlcorFinedata Hit(raw_hit);
                const int chip = Hit.get_chip();
                const int channel = Hit.get_global_channel_index();
                const float time_ns = Hit.get_time_ns();

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
                    spilldata.add_trigger_to_frame(frame_id, {static_cast<uint8_t>(TriggerTiming),
                                                              static_cast<uint16_t>(framer_cfg.frame_size / 2),
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
                                  h_streaming_trigger_tdc_zero_cluster_size,
                                  framer_cfg.frame_length_ns());

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
                std::vector<TriggerEvent> hough_triggers;
                for (auto current_trigger : triggers_in_frame)
                    if (current_trigger.index == _TRIGGER_STREAMING_RING_FOUND_)
                    {
                        auto index = -1;
                        streaming_trigger++;
                        std::vector<AlcorFinedata> ring_candidates;
                        std::vector<int> ring_candidates_index;
                        for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
                        {
                            index++;
                            AlcorFinedata current_hit(current_cherenkov_hit_struct);
                            if (current_hit.is_afterpulse())
                                continue;

                            if (!ispill && streaming_trigger <= 1000)
                                h_streaming_trigger_frames_examples->Fill(streaming_trigger - 1, current_hit.get_time());
                            if (current_hit.has_mask_bit(HitmaskStreamingRingTrigger))
                                h_streaming_trigger_full_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                            if (fabs(current_hit.get_time_ns() - current_trigger.fine_time) < 10.)
                            {
                                h_streaming_trigger_time_cut_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                                ring_candidates.push_back(current_hit);
                                ring_candidates_index.push_back(index);
                            }
                        }
                        auto found_rings = AlcorFinedata::alcor_find_rings_hough(ring_finder, ring_candidates, 0.33, threshold * 0.75, threshold, 2, 7.5);
                        h_streaming_trigger_ring_finder_nrings->Fill(found_rings.size());
                        index = -1;
                        std::array<int, 2> hough_trigger_hits = {0, 0};
                        std::array<float, 2> hough_trigger_time = {0.f, 0.f};
                        std::vector<std::array<float, 2>> hough_triggered_first;
                        std::vector<std::array<float, 2>> hough_triggered_second;
                        for (auto current_hit : ring_candidates)
                        {
                            index++;
                            if (current_hit.has_mask_bit(HitmaskHoughRingTagFirst) || current_hit.has_mask_bit(HitmaskHoughRingTagSecond))
                                h_streaming_trigger_ring_finder_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                            if (current_hit.has_mask_bit(HitmaskHoughRingTagFirst))
                            {
                                h_streaming_trigger_ring_finder_first_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                                hough_trigger_hits[0]++;
                                hough_trigger_time[0] += current_hit.get_time_ns();
                                hough_triggered_first.push_back({current_hit.get_hit_x(), current_hit.get_hit_y()});
                            }
                            if (current_hit.has_mask_bit(HitmaskHoughRingTagSecond))
                            {
                                h_streaming_trigger_ring_finder_second_hitmap->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                                hough_trigger_hits[1]++;
                                hough_trigger_time[1] += current_hit.get_time_ns();
                                hough_triggered_second.push_back({current_hit.get_hit_x(), current_hit.get_hit_y()});
                            }
                            cherenkov_hits[ring_candidates_index[index]].HitMask = current_hit.get_mask();
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
                        registry.index_of(static_cast<TriggerNumber>(t.index)));
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
                        h_trigger_time_diff_w_cherenkov[current_trigger.index] = new TH1F(Form("h_trigger_time_diff_w_cherenkov_%s", registry.name_of(current_trigger.index).c_str()), ";#Delta_{t} (t_{Hit} - t_{trigger}) ns;Normalised entries", 5e3, -500, 500);
                        h_trigger_full_hitmap[current_trigger.index] = new TH2F(Form("h_trigger_full_hitmap_%s", registry.name_of(current_trigger.index).c_str()), ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
                        h_trigger_hit_multiplicity[current_trigger.index][0] = new TH1F(Form("h_trigger_hit_multiplicity_in_time_%s", registry.name_of(current_trigger.index).c_str()), ";n_{Hit}; events;", 100, 0, 100);
                        h_trigger_hit_multiplicity[current_trigger.index][1] = new TH1F(Form("h_trigger_hit_multiplicity_out_of_time_%s", registry.name_of(current_trigger.index).c_str()), ";n_{Hit}; events;", 100, 0, 100);
                        h_trigger_dt[current_trigger.index] = new TH2F(
                            Form("h_trigger_dt_%s", registry.name_of(current_trigger.index).c_str()),
                            Form(";spill index;#Delta_{t} between consecutive %s triggers (ns);entries",
                                 registry.name_of(current_trigger.index).c_str()),
                            max_spill + 1, -0.5, max_spill + 0.5,
                            kTriggerDtNBinsY, trigger_dt_log_edges.data());
                    }

                    //  Frame distribution of the trigger
                    h_trigger_frame_population[current_trigger.index]->Fill(frame_id);
                    //  Time difference between consecutive same-index triggers
                    const uint64_t global_cc = static_cast<uint64_t>(frame_id) * framer_cfg.frame_size + current_trigger.coarse;
                    if (trigger_last_global_cc.count(current_trigger.index))
                    {
                        const double dt_ns = static_cast<double>(global_cc - trigger_last_global_cc[current_trigger.index]) * BTANA_ALCOR_CC_TO_NS;
                        h_trigger_dt[current_trigger.index]->Fill(static_cast<double>(ispill), dt_ns);
                    }
                    trigger_last_global_cc[current_trigger.index] = global_cc;
                    //  Time difference of trigger with cherenkov hits
                    std::array<int, 2> hit_counter = {0, 0};
                    auto window_size = (current_trigger.index == _TRIGGER_HOUGH_RING_FOUND_) || (current_trigger.index == _TRIGGER_STREAMING_RING_FOUND_) ? time_window_ns : 50.;
                    for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
                    {
                        AlcorFinedata current_hit(current_cherenkov_hit_struct);
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
                if (fired_trigger_types.count(registry.index_of(static_cast<TriggerNumber>(TriggerFirstFrames))))
                {
                    //  Channel by channel Hit counting — start each frame from a clean map
                    //  so counts cannot accumulate across frames.
                    //  Phase 5: storage is new-layout raw, so the Hit-side
                    //  key is constructed via direct GlobalIndex(stored).
                    //  Loop variable `channel_key` avoids shadowing the
                    //  ::GlobalIndex type.
                    active_sensors_count.clear();
                    for (const auto &channel_key : active_sensors)
                        active_sensors_count[channel_key] = 0;
                    for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
                    {
                        const uint32_t channel_key = ::GlobalIndex(
                            current_cherenkov_hit_struct.GlobalIndex).global_channel_raw();
                        active_sensors_count[channel_key]++;
                        //  Smeared DCR hitmap: one fill per Hit at the channel's
                        //  smeared physical position.  Density ∝ accumulated DCR
                        //  Hit count; divide by exposure for rate.
                        AlcorFinedata dcr_hit_fd(current_cherenkov_hit_struct);
                        const float dx = dcr_hit_fd.get_hit_x_rnd();
                        if (dx > -990.f)
                            h_dcr_hitmap->Fill(dx, dcr_hit_fd.get_hit_y_rnd());
                    }
                    //  Fill the DCR histogram
                    for (auto &[GlobalIndex, count] : active_sensors_count)
                        h_dcr_per_channel->Fill(GlobalIndex, count);

                    //  --- Afterpulse & cross-talk QA
                    //  Pre-decode per-Hit fields needed for pairwise comparisons.
                    struct _ct_hit
                    {
                        uint64_t global_t; ///< rollover*32768 + coarse
                        uint32_t channel;  ///< GlobalIndex / 4
                        int device;        ///< 192 + channel/256
                        int fifo;          ///< block of 8 channels within device
                        float x, y;        ///< physical position (-999 if unmapped)
                    };
                    std::vector<_ct_hit> ct_hits;
                    ct_hits.reserve(cherenkov_hits.size());
                    for (const auto &s : cherenkov_hits)
                    {
                        const uint32_t chan = s.GlobalIndex / 4;
                        ct_hits.push_back({static_cast<uint64_t>(s.rollover) * 32768u + s.coarse,
                                           chan,
                                           192 + static_cast<int>(chan / 256),
                                           static_cast<int>((chan % 256) / 8),
                                           s.hit_x, s.hit_y});
                    }

                    //  Build a time-sorted index so the CT inner loop can use
                    //  binary search to restrict candidates to the [dt_min, dt_max)
                    //  window — O(N log N + N·k_win) instead of O(N²).
                    std::vector<std::size_t> sorted_by_time(ct_hits.size());
                    std::iota(sorted_by_time.begin(), sorted_by_time.end(), 0);
                    std::sort(sorted_by_time.begin(), sorted_by_time.end(),
                              [&ct_hits](std::size_t a, std::size_t b)
                              { return ct_hits[a].global_t < ct_hits[b].global_t; });

                    for (std::size_t i = 0; i < cherenkov_hits.size(); ++i)
                    {
                        const auto &s = cherenkov_hits[i];
                        const auto &h = ct_hits[i];

                        const bool is_ap      = (s.HitMask >> HitmaskAfterpulse)      & 1u;
                        const bool is_ap_near = (s.HitMask >> HitmaskAfterpulseNear) & 1u;
                        const bool is_ap_far  = (s.HitMask >> HitmaskAfterpulseFar)  & 1u;

                        //  Single AlcorFinedata view of this Hit — shared by the AP and CT
                        //  smeared-hitmap fills below.  Constructed once per primary Hit.
                        AlcorFinedata hit_fd(s);

                        //  Afterpulse QA: sideband subtraction.
                        //  Per-channel profiles: TProfile means represent P(same-channel Hit
                        //  in window). The "subtracted" one uses signed weight so its mean
                        //  yields 100 × (P_near − P_far) = true afterpulse probability in %.
                        h_afterpulse_near_per_channel->Fill(h.channel, is_ap_near ? 100.0 : 0.0);
                        h_afterpulse_far_per_channel ->Fill(h.channel, is_ap_far  ? 100.0 : 0.0);
                        h_afterpulse_per_channel->Fill(h.channel,
                            100.0 * (static_cast<int>(is_ap_near) - static_cast<int>(is_ap_far)));
                        //  Smeared 2D maps: density ∝ probability (near, far) and
                        //  ∝ (P_near − P_far) for the DCR-subtracted true-afterpulse map.
                        //  100 fills per qualifying Hit, each at an independent ±1.5 mm
                        //  smeared position, mirroring the CT pattern.
                        if (h.x > -990.f)
                        {
                            if (is_ap_near)
                                for (int k = 0; k < 100; ++k)
                                    h_afterpulse_near_hitmap->Fill(
                                        hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd());
                            if (is_ap_far)
                                for (int k = 0; k < 100; ++k)
                                    h_afterpulse_far_hitmap->Fill(
                                        hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd());
                            //  Subtracted: ±1 weighted fills.  Net bin contents =
                            //  (n_near_fills − n_far_fills) = density ∝ true AP probability.
                            if (is_ap_near)
                                for (int k = 0; k < 100; ++k)
                                    h_afterpulse_hitmap->Fill(
                                        hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd(), +1.);
                            if (is_ap_far)
                                for (int k = 0; k < 100; ++k)
                                    h_afterpulse_hitmap->Fill(
                                        hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd(), -1.);
                        }

                        //  Cross-talk: skip afterpulse hits as DUI
                        if (is_ap)
                            continue;

                        int n_phys_ct = 0, n_elec_ct = 0;
                        // hit_fd already constructed above (shared with the AP smeared maps).
                        //  Binary-search pre-filter: only iterate hits whose global_t falls
                        //  inside [h.global_t + dt_min, h.global_t + dt_max).
                        //  This reduces the inner loop from O(N) to O(log N + k_window).
                        const int64_t t_lo = static_cast<int64_t>(h.global_t) + qa_cfg.ct_scan_dt_min;
                        const int64_t t_hi = static_cast<int64_t>(h.global_t) + qa_cfg.ct_scan_dt_max;
                        const auto jt_lo = std::lower_bound(
                            sorted_by_time.begin(), sorted_by_time.end(), t_lo,
                            [&ct_hits](std::size_t idx, int64_t t)
                            { return static_cast<int64_t>(ct_hits[idx].global_t) < t; });
                        const auto jt_hi = std::lower_bound(
                            jt_lo, sorted_by_time.end(), t_hi,
                            [&ct_hits](std::size_t idx, int64_t t)
                            { return static_cast<int64_t>(ct_hits[idx].global_t) < t; });
                        for (auto jt = jt_lo; jt != jt_hi; ++jt)
                        {
                            const std::size_t j = *jt;
                            if (j == i || ct_hits[j].channel == h.channel)
                                continue;
                            //  dt is guaranteed within [ct_scan_dt_min, ct_scan_dt_max)
                            //  by the binary-search pre-filter above.
                            const int64_t dt = static_cast<int64_t>(ct_hits[j].global_t) -
                                               static_cast<int64_t>(h.global_t);
                            const bool is_elec = ct_hits[j].device == h.device &&
                                                 ct_hits[j].fifo == h.fifo;
                            //  Physical CT requires strictly positive Δt (causal optical/charge coupling)
                            const bool is_phys = dt >= 0 &&
                                                 h.x > -990.f && ct_hits[j].x > -990.f &&
                                                 std::hypot(ct_hits[j].x - h.x, ct_hits[j].y - h.y) <= 3.2f;
                            //  Fill Δt for all neighbour types — used for DCR sideband estimation
                            if (is_phys)
                                h_phys_ct_dt->Fill(static_cast<double>(dt));
                            if (is_elec)
                                h_elec_ct_dt->Fill(static_cast<double>(dt));
                            //  2D diagnostic: (Δchannel, Δt) filtered per neighbour type
                            const double dchannel = static_cast<double>(ct_hits[j].channel) -
                                                    static_cast<double>(h.channel);
                            if (is_elec)
                                h_elec_ct_dchannel_dt->Fill(dchannel, static_cast<double>(dt));
                            if (is_phys)
                                h_phys_ct_dchannel_dt->Fill(dchannel, static_cast<double>(dt));
                            //  CT signal windows from qa_cfg.  Use the wider of the two
                            //  upper bounds as the loop's early-exit gate to keep the
                            //  filtering symmetric for both neighbour types.
                            const int ct_signal_hi_any =
                                std::max(qa_cfg.ct_elec_signal_hi, qa_cfg.ct_phys_signal_hi);
                            if (dt > ct_signal_hi_any)
                                continue;
                            if (is_elec &&
                                dt >= qa_cfg.ct_elec_signal_lo && dt <= qa_cfg.ct_elec_signal_hi)
                                ++n_elec_ct;
                            if (is_phys &&
                                dt >= qa_cfg.ct_phys_signal_lo && dt <= qa_cfg.ct_phys_signal_hi)
                                ++n_phys_ct;
                        }

                        //  Per-channel probability profiles (boolean: was there any CT?).
                        h_phys_ct_per_channel->Fill(h.channel, n_phys_ct > 0 ? 100.0 : 0.0);
                        h_elec_ct_per_channel->Fill(h.channel, n_elec_ct > 0 ? 100.0 : 0.0);
                        //  Smeared CT hitmaps — n_ct_neighbours × 100 fills per primary Hit
                        //  at independent ±1.5 mm smeared positions; density ∝ CT rate.
                        if (h.x > -990.f)
                        {
                            for (int _k = 0; _k < n_phys_ct * 100; ++_k)
                                h_phys_ct_hitmap->Fill(hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd());
                            for (int _k = 0; _k < n_elec_ct * 100; ++_k)
                                h_elec_ct_hitmap->Fill(hit_fd.get_hit_x_rnd(), hit_fd.get_hit_y_rnd());
                        }
                    }
                }
            }
        }
        progress_postprocessing.update(1, 1);

        //  Reflect the just-completed spill on the main bar (ispill+1 of N).
        //  The next iteration's restart() will reset the subtask clocks.
        progress_bars.update(ispill + 1, max_spill);

        outfile->cd();
        spilldata.prepare_tree_fill();
        lightdata_tree->Fill();
        outfile->Flush();
    }
    // All spills done — finalise the multi-bar.  finish() emits the last
    // 100% frame as scrolling output (B1 fix in mist) and removes the bar
    // block from the anchored region so subsequent log lines flow normally.
    progress_framer.finish(/*flush=*/false);
    progress_postprocessing.finish(/*flush=*/false);
    progress_bars.finish();
    mist::logger::info("(lightdata_writer) Finished spills loop, writing to file");

    //  ---
    //  --- Rollover offset QA: populate from the framer's correction table
    const auto &rollover_correction_table = framer.get_rollover_correction_table();
    const auto stream_filenames = framer.get_stream_filenames();

    //  Find table dimensions for the 2D histogram.
    size_t n_streams_in_table = rollover_correction_table.size();
    size_t n_spills_in_table = 0;
    for (const auto &stream_corrections : rollover_correction_table)
        n_spills_in_table = std::max(n_spills_in_table, stream_corrections.size());

    if (n_streams_in_table > 0 && n_spills_in_table > 0)
    {
        h_rollover_correction_ticks_per_stream_and_spill = new TH2F(
            "h_rollover_correction_ticks_per_stream_and_spill",
            ";stream index;spill index;rollover correction (ticks)",
            n_streams_in_table, -0.5, n_streams_in_table - 0.5,
            n_spills_in_table, -0.5, n_spills_in_table - 0.5);

        //  Label each stream bin with the basename of its file (e.g. "rdo-192/alcdaq.fifo_14.root")
        //  so the x-axis is human-readable in the browser.
        for (size_t i_stream = 0; i_stream < n_streams_in_table; ++i_stream)
        {
            std::filesystem::path stream_path(stream_filenames[i_stream]);
            //  Use parent_dir/filename form to distinguish same-name files across devices.
            const std::string stream_label =
                stream_path.parent_path().parent_path().filename().string() + "/" +
                stream_path.filename().string();
            h_rollover_correction_ticks_per_stream_and_spill
                ->GetXaxis()
                ->SetBinLabel(i_stream + 1, stream_label.c_str());
        }

        for (size_t i_stream = 0; i_stream < n_streams_in_table; ++i_stream)
        {
            const auto &stream_corrections = rollover_correction_table[i_stream];
            for (size_t i_spill = 0; i_spill < stream_corrections.size(); ++i_spill)
            {
                const uint64_t rollover_correction_cc = stream_corrections[i_spill];
                const double rollover_correction_ticks =
                    static_cast<double>(rollover_correction_cc) /
                    static_cast<double>(BTANA_ALCOR_ROLLOVER_TO_CC);

                //  Fill the 2D map with the correction in ticks — zero entries
                //  are still set so the axis shows full coverage.
                h_rollover_correction_ticks_per_stream_and_spill->SetBinContent(
                    i_stream + 1, i_spill + 1, rollover_correction_ticks);

                //  1D distribution: one entry per (stream, spill) pair. Peak at 0
                //  is healthy; entries at 1 are the actionable ones.
                h_rollover_correction_ticks_distribution->Fill(rollover_correction_ticks);

                //  How many streams needed correction in each spill.
                if (rollover_correction_ticks > 0)
                    h_rollover_correction_affected_streams_per_spill->Fill(i_spill);
            }
        }
    }

    //  ---
    //  End: Loop on data streamers
    //  --- --- --- --- --- ---

    //  --- --- --- --- --- ---
    //  QA plots
    //  ---
    outfile->cd();
    lightdata_tree->Write();
    framer.get_fine_tune_distribution()->Write("h_fine_calib");
    AlcorFinedata::write_calib_to_file((base_dir / "fine_calib.txt").string());
    //  ---
    //  --- Rollover QA
    TDirectory *rollover_dir = outfile->mkdir("Rollover QA");
    rollover_dir->cd();
    if (h_rollover_correction_ticks_per_stream_and_spill)
        h_rollover_correction_ticks_per_stream_and_spill->Write();
    h_rollover_correction_ticks_distribution->Write();
    h_rollover_correction_affected_streams_per_spill->Write();
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
    for (auto [key, val] : h_trigger_dt)
    {
        val->Scale(1., "width");
        val->Write();
    }
    //  ---
    //  --- Timing
    TDirectory *timing_dir = outfile->mkdir("Timing");
    timing_dir->cd();
    h_timing_hit_map->Write();
    h_timing_ref_delta->Write();
    h_timing_ref_delta_sel->Write();
    //  Repeating info for single source for check
    auto timing_index = registry.index_of(static_cast<TriggerNumber>(TriggerTiming));
    if (h_trigger_frame_population.count(timing_index))
    {
        h_trigger_frame_population[timing_index]->Write();
        h_trigger_time_diff_w_cherenkov[timing_index]->Write();
        h_trigger_full_hitmap[timing_index]->Write();
    }
    //  ---
    //  --- DCR
    TDirectory *DCR_dir = outfile->mkdir("Single-Pixel Noise");
    DCR_dir->cd();
    h_dcr_per_channel->Scale(1. / (framer_cfg.frame_length_ns() * 1.e-6));
    h_dcr_per_channel->Write();
    h_dcr_hitmap->Write();
    h_afterpulse_near_per_channel->Write();
    h_afterpulse_near_hitmap     ->Write();
    h_afterpulse_far_per_channel ->Write();
    h_afterpulse_far_hitmap      ->Write();
    h_afterpulse_per_channel     ->Write();
    h_afterpulse_hitmap          ->Write();
    //  Same-channel Δt diagnostic — use it to validate that the near / far
    //  windows defined in qa_cfg actually contain the afterpulse peak and a
    //  flat DCR shelf respectively.
    if (auto *h_ap_dt = framer.get_afterpulse_dt_distribution())
        h_ap_dt->Write();
    h_phys_ct_per_channel->Write();
    h_phys_ct_hitmap->Write();
    h_elec_ct_per_channel->Write();
    h_elec_ct_hitmap->Write();
    h_phys_ct_dt->Write();
    h_elec_ct_dt->Write();
    h_elec_ct_dchannel_dt->Write();
    h_phys_ct_dchannel_dt->Write();
    //  ---
    //  --- Streaming Trigger
    TDirectory *streaming_trigger_dir = outfile->mkdir("Streaming Trigger");
    streaming_trigger_dir->cd();
    auto streaming_ring_index = registry.index_of(static_cast<TriggerNumber>(_TRIGGER_STREAMING_RING_FOUND_));
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
    //  --- Config — write all processing settings for reproducibility
    {
        TDirectory *cfg_dir = outfile->mkdir("Config");
        cfg_dir->cd();

        //  Numeric values as TParameter so they are directly readable downstream
        TParameter<int> p_frame_size("frame_size", framer_cfg.frame_size);
        TParameter<int> p_first_frames_trigger("first_frames_trigger", framer_cfg.first_frames_trigger);
        TParameter<int> p_afterpulse_deadtime("afterpulse_deadtime", framer_cfg.afterpulse_deadtime);
        TParameter<int> p_trigger_secondary_window("trigger_secondary_window", framer_cfg.trigger_secondary_window);
        TParameter<double> p_frame_length_ns("frame_length_ns", framer_cfg.frame_length_ns());
        p_frame_size.Write();
        p_first_frames_trigger.Write();
        p_afterpulse_deadtime.Write();
        p_trigger_secondary_window.Write();
        p_frame_length_ns.Write();

        //  QA windows used by the afterpulse sideband subtraction and the CT scan.
        TParameter<int> p_qa_ap_near_lo            ("qa_afterpulse_near_lo",          qa_cfg.afterpulse_near_lo);
        TParameter<int> p_qa_ap_near_hi            ("qa_afterpulse_near_hi",          qa_cfg.afterpulse_near_hi);
        TParameter<int> p_qa_ap_sideband_offset    ("qa_afterpulse_sideband_offset",  qa_cfg.afterpulse_sideband_offset);
        TParameter<int> p_qa_ct_scan_dt_min        ("qa_ct_scan_dt_min",              qa_cfg.ct_scan_dt_min);
        TParameter<int> p_qa_ct_scan_dt_max        ("qa_ct_scan_dt_max",              qa_cfg.ct_scan_dt_max);
        TParameter<int> p_qa_ct_phys_signal_lo     ("qa_ct_phys_signal_lo",           qa_cfg.ct_phys_signal_lo);
        TParameter<int> p_qa_ct_phys_signal_hi     ("qa_ct_phys_signal_hi",           qa_cfg.ct_phys_signal_hi);
        TParameter<int> p_qa_ct_elec_signal_lo     ("qa_ct_elec_signal_lo",           qa_cfg.ct_elec_signal_lo);
        TParameter<int> p_qa_ct_elec_signal_hi     ("qa_ct_elec_signal_hi",           qa_cfg.ct_elec_signal_hi);
        p_qa_ap_near_lo.Write();
        p_qa_ap_near_hi.Write();
        p_qa_ap_sideband_offset.Write();
        p_qa_ct_scan_dt_min.Write();
        p_qa_ct_scan_dt_max.Write();
        p_qa_ct_phys_signal_lo.Write();
        p_qa_ct_phys_signal_hi.Write();
        p_qa_ct_elec_signal_lo.Write();
        p_qa_ct_elec_signal_hi.Write();

        //  Config file paths as TNamed
        TNamed n_trigger_conf("trigger_conf_file", trigger_setup_file.c_str());
        TNamed n_readout_conf("readout_conf_file", readout_config_file.c_str());
        TNamed n_mapping_conf("mapping_conf_file", mapping_config_file.c_str());
        TNamed n_fine_calib_conf("fine_calib_conf_file", fine_calibration_config_file.c_str());
        TNamed n_framer_conf("framer_conf_file", framer_conf_file.c_str());
        n_trigger_conf.Write();
        n_readout_conf.Write();
        n_mapping_conf.Write();
        n_fine_calib_conf.Write();
        n_framer_conf.Write();

        //  Raw TOML content snapshots — read each config file and store verbatim
        auto write_toml_snapshot = [&](const std::string &key, const std::string &path)
        {
            std::ifstream f(path);
            if (!f.good())
                return;
            std::ostringstream ss;
            ss << f.rdbuf();
            TNamed named(key.c_str(), ss.str().c_str());
            named.Write();
        };
        write_toml_snapshot("trigger_conf_toml", trigger_setup_file);
        write_toml_snapshot("readout_conf_toml", readout_config_file);
        write_toml_snapshot("mapping_conf_toml", mapping_config_file);
        write_toml_snapshot("framer_conf_toml", framer_conf_file);
    }
    //  ---
    //  --- Close file
    outfile->Close();
    //  ---
    //  End: QA plots
    //  --- --- --- --- --- ---
}

bool run_streaming_trigger(AlcorSpilldata &current_spill,
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
                           TH1F *h_tdc_zero_cluster_size,
                           float frame_length_ns)
{
    auto &cherenkov_hits = current_spill.get_frame_cherenkov_hits(frame_id);

    //  Build and sort finedata hits
    std::vector<AlcorFinedata> cherenkov_finedata_hits;
    cherenkov_finedata_hits.reserve(cherenkov_hits.size());
    for (const auto &h : cherenkov_hits)
        cherenkov_finedata_hits.emplace_back(h);
    std::sort(cherenkov_finedata_hits.begin(), cherenkov_finedata_hits.end());

    //  Deque window: front = oldest Hit, back = newest Hit.
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
                    result.pop_back(); // last Hit is a genuine outlier
                else if (range_excl_first < range_excl_last && range_excl_first * kOutlierRatio < range_excl_last)
                    result.erase(result.begin()); // first Hit is a genuine outlier
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
            // log-likelihood ratio for Hit t:
            //   LLR = log[ p(t|main) * 19 ] - log[ p(t|early) * 1 ]
            // If LLR < 0, Hit is more likely from early peak.
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
            cherenkov_hits[ihit].HitMask = cherenkov_finedata_hits[ihit].get_mask();

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
        carry_over_hits.push_back({-1, entry.second - frame_length_ns});

    return has_fired;
}