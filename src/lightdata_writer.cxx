#include "parallel_streaming_framer.h"
#include "writers/lightdata.h"
#include "writers/lightdata/types.h"                 // CtHit
#include "writers/lightdata/dcr_afterpulse_ct_qa.h"  // fill_dcr_afterpulse_ct_qa
#include "triggers/streaming/score.h"
#include "triggers/streaming/hough.h"
#include "mapping.h"
#include <mist/ring_finding/hough_transform.h>
#include "TROOT.h"
#include "TProfile2D.h"
#include "TNamed.h"
#include "TParameter.h"
#include "TCanvas.h"
#include "TLegend.h"
#include <algorithm>
#include <numeric>

// ── Same-channel-offset calibration: per-chip alive-channel counts ─────────
//
// The same-channel-offset calibration (further below) only uses events where
// every alive channel on a given timing chip fires.  These two constants
// encode the "expected alive channel count per chip" for the current beam-
// test detector:
//
//   - kTimingChip0AliveChannels = 32 → all 32 channels of the first timing
//     chip are alive.
//   - kTimingChip1AliveChannels = 31 → one known-dead channel on the second
//     timing chip; coincidence threshold lowered accordingly.
//
// Hardcoded here because the dead-channel layout changes ~once per
// beam-test campaign — a TOML schema bump for a single int per chip is not
// worth the indirection.  The chip IDs themselves (0 and 2) are NOT
// hardcoded any more: they are resolved at runtime from the readout config
// via framer.get_readout_config().find_by_name("timing")  (CODE_REVIEW
// §D-07).  Update these two constants directly when the dead-channel map
// changes.
//
// Guard: the mean-of-others formula divides by (alive_channels - 1), so
// both constants must be >= 2.
constexpr int kTimingChip0AliveChannels = 32;
constexpr int kTimingChip1AliveChannels = 31;
static_assert(kTimingChip0AliveChannels >= 2, "kTimingChip0AliveChannels must be >= 2 to avoid divide-by-zero");
static_assert(kTimingChip1AliveChannels >= 2, "kTimingChip1AliveChannels must be >= 2 to avoid divide-by-zero");

// ── Calibration-window tuning knobs (NOT geometry) ─────────────────────────
// Centre, half-width and Nσ for the same-channel-offset Δt cut.  These are
// analysis tuning, not detector layout, so they do not belong in the
// readout config.  Move to a dedicated calibration block (or pass-through
// arg) only if more knobs accumulate.
constexpr float kDeltaTimingCenter = -0.5f;
constexpr float kDeltaTimingWindow = 0.5f;
constexpr float kDeltaTimingNSigma = 3.0f;

void lightdata_writer(
    const std::string &data_repository,
    const std::string &run_name,
    int max_spill,
    bool force_rebuild,
    int requested_n_threads,
    std::string trigger_setup_file,
    std::string readout_config_file,
    std::string mapping_config_file,
    std::string fine_calibration_config_file,
    std::string framer_conf_file,
    std::string streaming_conf_file)
{
    //  ROOT thread-safety: protects TROOT/TF1/Fit::Fitter global
    //  state under the framer's multithreaded stream reads (which
    //  exercise gROOT lookups via the per-thread WorkerScratch hist
    //  cloning) and any future frame-level parallelism in this
    //  writer.  Without it, ROOT's internal mutex effectively
    //  serializes ROOT calls across workers.  Idempotent; safe to
    //  call once per lightdata_writer invocation.
    ROOT::EnableThreadSafety();

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
            mist::logger::warning(TString::Format("(lightdata_writer) Data folder for device %s do not have decoded data, skipping", device_name.c_str()).Data());
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
    //  Software trigger pipeline (Phase 2: moved out of framer_conf.toml).
    //  Both stages share the same file; the Hough struct is loaded but not
    //  yet consumed by the algorithm (Phase 4 wires it).
    auto streaming_trigger_cfg = streaming_trigger_conf_reader(streaming_conf_file);
    auto streaming_hough_cfg   = streaming_hough_conf_reader(streaming_conf_file);

    //  Create streaming framer
    ParallelStreamingFramer framer(filenames, trigger_setup_file, readout_config_file, framer_cfg);
    framer.set_qa_config(qa_cfg);       // enable afterpulse near/far Hit-mask tagging
    framer.set_parallel_cores(requested_n_threads);
    framer.resolve_rollover_offsets();   // populates the per-stream, per-spill correction table consumed by the Rollover QA fills below
    framer.assign_bar(progress_framer); // framer drives the framer subtask automatically

    // Resolve timing chip IDs from the readout config (CODE_REVIEW §D-07) —
    // previously these were hardcoded as kTimingChip{0,1}Id = {0, 2}, duplicating
    // information already in conf/readout_config.toml.  -1 sentinel = "no such
    // chip configured"; the same-channel-offset calibration loop below skips
    // its per-chip branch when the corresponding ID is < 0.
    int timing_chip_0_id = -1;
    int timing_chip_1_id = -1;
    if (const auto *timing_cfg = framer.get_readout_config().find_by_name("timing");
        timing_cfg && !timing_cfg->device_chip.empty())
    {
        // The readout config conventionally lists exactly one timing device
        // (e.g. id=200) with two chips.  Take the first device's chip list.
        const auto &chips = timing_cfg->device_chip.begin()->second;
        if (chips.size() >= 1) timing_chip_0_id = static_cast<int>(chips[0]);
        if (chips.size() >= 2) timing_chip_1_id = static_cast<int>(chips[1]);
        if (chips.size() > 2)
            mist::logger::warning(TString::Format(
                "(lightdata_writer) Timing readout has %zu chips; only the first two "
                "are used by the same-channel-offset calibration. "
                "Extend the calibration loop if more timing chips become active.",
                chips.size()).Data());
    }
    else
    {
        mist::logger::warning("(lightdata_writer) No timing detector configured in "
                              "readout config — same-channel-offset calibration "
                              "will be skipped.");
    }

    auto config_triggers = trigger_conf_reader(trigger_setup_file);
    TriggerRegistry registry(config_triggers);
    //  Prepare output file & tree
    std::string outfile_name = data_repository + "/" + run_name + "/lightdata.root";
    if (std::filesystem::exists(outfile_name) && !force_rebuild)
    {
        mist::logger::info("[INFO] Output file already exists, skipping: " + outfile_name);
        return;
    }
    //  Generate Mapping
    Mapping current_mapping(mapping_config_file);
    //  Load fine_calibration
    AlcorFinedata::read_calib_from_file(fine_calibration_config_file);
    //  Calibration table is now fully loaded; flip the immutability flag so
    //  the per-Hit AlcorFinedata::get_phase() readers can take the lock-free
    //  fast path inside the framer's worker threads (CODE_REVIEW §3.1).
    AlcorFinedata::freeze_calibration();
    //  Link output tree.  TFilePtr is owning — closes + deletes on function
    //  exit (including early returns and exception unwind) so there is no
    //  manual outfile->Close() call at the bottom (CODE_REVIEW §4.12).
    TFilePtr outfile(TFile::Open(outfile_name.c_str(), "RECREATE"));
    if (!outfile || outfile->IsZombie())
    {
        mist::logger::error("(lightdata_writer) Failed to create output file: " + outfile_name);
        return;
    }
    auto &spilldata = framer.get_spilldata_link();
    TTree *lightdata_tree = new TTree("lightdata", "Lightdata tree");
    // 30 MB auto-flush — ROOT's own default, set explicitly here so the
    // choice is visible.  Together with the removal of the per-spill
    // outfile->Flush() below, this leaves I/O scheduling to the tree's
    // buffer machinery (writes flush when the basket buffer fills) rather
    // than fsync'ing on every spill (lethal on HDD/NFS).
    lightdata_tree->SetAutoFlush(-30000000);
    spilldata.write_to_tree(lightdata_tree);
    //  ---
    //  QA Plots
    //  ---
    //  --- Rollover offset QA
    //  Per-stream, per-spill correction applied during framing, expressed in
    //  rollover ticks (clock cycles / BTANA_ALCOR_ROLLOVER_TO_CC). Bins sized to
    //  accommodate the actual table dimensions at fill time.
    RootHist<TH2F> h_rollover_correction_ticks_per_stream_and_spill;  // empty until the conditional below builds it
    RootHist<TH1F> h_rollover_correction_ticks_distribution(
        "h_rollover_correction_ticks_distribution",
        ";rollover correction (ticks);(stream,spill) entries",
        10, -0.5, 9.5);
    RootHist<TH1F> h_rollover_correction_affected_streams_per_spill(
        "h_rollover_correction_affected_streams_per_spill",
        ";spill index;streams requiring correction",
        1000, -0.5, 999.5);
    //  ---
    //  --- Triggers
    RootHist<TH2F> h2_trigger_matrix(
        "h2_trigger_matrix", "Trigger coincidence matrix;;",
        registry.size(), -0.5, registry.size() - 0.5,
        registry.size(), -0.5, registry.size() - 0.5);
    registry.label_axes(h2_trigger_matrix.get());
    std::unordered_map<int, RootHist<TH1F>> h_trigger_frame_population;
    std::unordered_map<int, RootHist<TH1F>> h_trigger_time_diff_w_cherenkov;
    std::unordered_map<int, RootHist<TH2F>> h_trigger_full_hitmap;
    std::unordered_map<int, std::array<RootHist<TH1F>, 2>> h_trigger_hit_multiplicity;
    std::unordered_map<int, RootHist<TH2F>> h_trigger_dt;

    //  Log-spaced Y-axis edges (Δt in ns) shared across all per-trigger TH2Fs.
    //  Range: 1 ns → 1e10 ns (≈10 s, comfortably covers a 5 s spill at 320 MHz).
    constexpr int kTriggerDtNBinsY = 60;
    std::vector<double> trigger_dt_log_edges(kTriggerDtNBinsY + 1);
    for (int i = 0; i <= kTriggerDtNBinsY; ++i)
        trigger_dt_log_edges[i] = std::pow(10.0, 10.0 * static_cast<double>(i) / kTriggerDtNBinsY);
    //  ---
    //  --- Timing
    RootHist<TH2F> h_timing_hit_map("h_timing_hit_map", ";channels on chip 0; channels on chip 1", 33, 0, 33, 33, 0, 33);
    RootHist<TH1F> h_timing_ref_delta("h_timing_ref_delta", ";timing chip 0 - timing chip 1", 250, -5, 5);
    RootHist<TH1F> h_timing_ref_delta_sel("h_timing_ref_delta_sel", ";timing chip 0 - timing chip 1", 250, -5, 5);
    //  ---
    //  --- DCR
    RootHist<TProfile> h_dcr_per_channel("h_dcr_per_channel", ";channel;DCR [kHz];", 2048, 0, 2048);
    //  Smeared DCR hitmap — one fill per cherenkov Hit during noise (first-frames)
    //  trigger frames, at the channel's ±1.5 mm smeared physical position.  Bin
    //  contents are total Hit counts; divide by (n_dcr_frames × frame_length × bin_area)
    //  for a rate.  Density ∝ DCR rate, as in the CT / AP smeared maps.
    RootHist<TH2F> h_dcr_hitmap("h_dcr_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    //  --- Afterpulse
    //  Per-channel afterpulse profiles.
    //  Near = afterpulse signal + DCR baseline ; far = DCR sideband only.
    //  Subtracted (true afterpulse) = signed-weight fill on the same Hit set.
    RootHist<TProfile> h_afterpulse_near_per_channel("h_afterpulse_near_per_channel",
        ";channel;Near-window same-channel probability (%);", 2048, 0, 2048);
    RootHist<TProfile> h_afterpulse_far_per_channel("h_afterpulse_far_per_channel",
        ";channel;Far-window same-channel probability (%);", 2048, 0, 2048);
    RootHist<TProfile> h_afterpulse_per_channel("h_afterpulse_per_channel",
        ";channel;Afterpulse probability (DCR-subtracted) (%);", 2048, 0, 2048);
    //  Smeared 2D hitmaps — per primary Hit we deposit 100 fills at independent
    //  ±1.5 mm smeared positions when the Hit lies in the relevant window.  Density
    //  in the resulting TH2F is therefore proportional to the corresponding
    //  probability, in the same units as the per-channel profiles.
    //
    //  The "subtracted" map uses ±1 weights so per-bin contents = (n_near − n_far),
    //  i.e. density ∝ true afterpulse probability.  May go negative in DCR-only
    //  bins by Poisson fluctuation — that's the expected zero-bias behaviour.
    RootHist<TH2F> h_afterpulse_near_hitmap("h_afterpulse_near_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_afterpulse_far_hitmap("h_afterpulse_far_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_afterpulse_hitmap("h_afterpulse_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    h_afterpulse_hitmap->Sumw2();   // signed-weight fills → needs squared-weight tracking
    //  --- Cross-talk per-channel profiles
    RootHist<TProfile> h_phys_ct_per_channel("h_phys_ct_per_channel",
        ";channel;Physical CT probability (%);", 2048, 0, 2048);
    RootHist<TProfile> h_elec_ct_per_channel("h_elec_ct_per_channel",
        ";channel;Electrical CT probability (%);", 2048, 0, 2048);
    //  Smeared CT hitmaps — n_ct_neighbours × 100 fills per primary Hit, each at
    //  an independent ±1.5 mm smeared position.  Density ∝ CT rate per spatial bin.
    RootHist<TH2F> h_phys_ct_hitmap("h_phys_ct_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_elec_ct_hitmap("h_elec_ct_hitmap",
        ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    //  --- CT neighbour-pair Δt distributions (signal peak + DCR sideband)
    //  Physical CT signal window: [0, 10] cc.
    //  Electrical CT signal window: [-2, 10] cc (small negative allowed for readout-timing jitter).
    //  Sideband for DCR baseline: beyond the signal window.
    RootHist<TH1F> h_phys_ct_dt("h_phys_ct_dt", ";#Delta_{t} (cc);physical neighbour pairs / primary Hit", 200, 0, 200);
    RootHist<TH1F> h_elec_ct_dt("h_elec_ct_dt", ";#Delta_{t} (cc);electrical neighbour pairs / primary Hit", 210, -10, 200);
    //  --- CT 2D diagnostic: (Δchannel, Δt) for every in-frame pair — no neighbour
    //  definition needed; CT clusters near the origin, DCR is flat in Δt.
    //  Per-neighbour-type (Δchannel, Δt) diagnostics.
    //
    //  Axis sizing (post-Phase-5 audit, post-migration §10):
    //  - h.channel is now `gi.channel_ordinal()` — dense small int.
    //    Under kUsesSplitInTwo the ordinal increments by 1 within a chip
    //    and by 32 across chips on the same FIFO, so within a FIFO the
    //    actual Δordinal range can be ±31 (not ±7 as the legacy formula
    //    suggested).  Old axis [-8.5, 8.5] dumped >50% of fills to
    //    overflow.  New axis [-31.5, 31.5] / 63 bins (1 bin / Δchannel).
    //  - Physical CT spans a device (no FIFO restriction), so Δordinal
    //    can reach ±255 in pathological cases.  Old axis [-32.5, 32.5]
    //    overflowed ~40% of fills.  New axis [-127.5, 127.5] / 255 bins
    //    (1 bin / Δchannel) — covers everything we observed in the post-
    //    migration data while keeping bin resolution at 1 channel.
    // Y-axis sized to match the CT scan window
    // [qa_cfg.ct_scan_dt_min, qa_cfg.ct_scan_dt_max) (defaults [-5, 200) cc
    // from conf/framer_conf.toml).  The previous [-5.5, 20.5] axis dumped
    // ~60% of fills to Y-overflow (dt > 20 cc) — see post-migration audit §10.
    RootHist<TH2F> h_elec_ct_dchannel_dt("h_elec_ct_dchannel_dt",
                                           ";#Delta channel (j #minus i, same FIFO);#Delta_{t} (cc)",
                                           63, -31.5, 31.5,
                                           205, -5.5, 199.5);
    RootHist<TH2F> h_phys_ct_dchannel_dt("h_phys_ct_dchannel_dt",
                                           ";#Delta channel (j #minus i, #leq 3.2 mm);#Delta_{t} (cc)",
                                           255, -127.5, 127.5,
                                           205, -5.5, 199.5);
    //  ---
    //  --- Streaming Trigger
    //
    //  The Cherenkov-hit hitmaps cover the score → Hough pipeline:
    //    full_hitmap     hits flagged by stage 1 (score's cluster mask)
    //    time_cut_hitmap hits surviving |t − t_streaming| < time_window_ns
    //    ring_finder_*   hits the Hough tagged as belonging to a ring
    RootHist<TH2F> h_streaming_trigger_full_hitmap("h_streaming_trigger_full_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_streaming_trigger_time_cut_hitmap("h_streaming_trigger_time_cut_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_streaming_trigger_ring_finder_hitmap("h_streaming_trigger_ring_finder_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH1F> h_streaming_trigger_ring_finder_nrings("h_streaming_trigger_ring_finder_nrings", ";timing chip 0 - timing chip 1", 3, -.5, 2.5);
    RootHist<TH2F> h_streaming_trigger_ring_finder_first_hitmap("h_streaming_trigger_ring_finder_first_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_streaming_trigger_ring_finder_second_hitmap("h_streaming_trigger_ring_finder_second_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    //  Per-ring centre / radius hists.  Binning AND limits are derived
    //  from streaming_hough_cfg so a TOML edit propagates straight
    //  through — one QA bin ↔ one Hough accumulator cell.
    //
    //    R   : range = [cfg.r_min, cfg.r_max],   bin width = cfg.r_step
    //    X/Y : range = [-cfg.centre_xy_half_range_mm,
    //                   +cfg.centre_xy_half_range_mm],
    //          bin width = cfg.cell_size   (rounded so n_bins × cell_size
    //          covers the full half-range symmetrically around 0)
    //
    //  Note: `centre_xy_half_range_mm` is a writer-side QA knob, not a
    //  ring-finder bound — the Hough's X/Y space is set by the active
    //  geometry, not by a user limit.  Set it wider than the expected
    //  centre spread for your beam line so legitimate rings don't fall
    //  in overflow; tighter to zoom in once you know where they land.
    const int   ringXY_nbins =
        std::max(1, static_cast<int>(std::round(
                       2.f * streaming_hough_cfg.centre_xy_half_range_mm /
                       streaming_hough_cfg.cell_size)));
    const float ringXY_half  = 0.5f * ringXY_nbins * streaming_hough_cfg.cell_size;
    const int   ringR_nbins =
        std::max(1, static_cast<int>(std::round(
                       (streaming_hough_cfg.r_max - streaming_hough_cfg.r_min) /
                       streaming_hough_cfg.r_step)));
    const float ringR_lo    = streaming_hough_cfg.r_min;
    const float ringR_hi    = streaming_hough_cfg.r_min +
                              ringR_nbins * streaming_hough_cfg.r_step;
    //  Per-ring **Hough peak** outputs (centre X, Y, radius taken
    //  straight from `RingResult::{cx, cy, radius}` before the
    //  fit_circle refinement).  Written first in the output (see the
    //  `Hough rings/` subfolder below) so the upstream → downstream
    //  chain is visually obvious.
    RootHist<TH1F> h_streaming_trigger_ring_X_first_hough("h_streaming_trigger_ring_X_first_hough", ";x (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_Y_first_hough("h_streaming_trigger_ring_Y_first_hough", ";y (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_R_first_hough("h_streaming_trigger_ring_R_first_hough", ";R (mm)", ringR_nbins, ringR_lo, ringR_hi);
    RootHist<TH1F> h_streaming_trigger_ring_X_second_hough("h_streaming_trigger_ring_X_second_hough", ";x (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_Y_second_hough("h_streaming_trigger_ring_Y_second_hough", ";y (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_R_second_hough("h_streaming_trigger_ring_R_second_hough", ";R (mm)", ringR_nbins, ringR_lo, ringR_hi);
    //  (Per-ring `fit_circle` outputs removed 2026-05-26: the lightdata-
    //  side fit was QA-only and recodata re-fits the mask-tagged hits
    //  with full LOO + dual/solo splits + CB+pol3 radial fit.  All
    //  fit-derived observables now live in recodata.root's `Rings/`
    //  subfolder.  Saves ~5-30 s per run and removes the architectural
    //  ambiguity of having two simultaneous fit pipelines.)

    //  Hough-knob calibration QA (see § 2.5 in the streaming DISCUSSION).
    //
    //  2D `peak_votes` (y) vs `|active|` (x) per ring slot.  Two
    //  threshold lines overlay on these axes for tuning:
    //     y = min_hits                    (absolute floor; raise via
    //                                      hough_threshold_fraction)
    //     y = threshold_fraction × x      (relative floor)
    //  Real rings cluster as a band high in y; random-coincidence
    //  peaks track the diagonal.  Bins are integer-aligned (1 hit /
    //  1 vote per bin) so the discrete vote counts map cleanly.
    //
    //  Axis ranges set from the first calibration run: |active| ≤ 100
    //  covers all observed events, peak_votes ≤ 50 covers the bulk of
    //  the population at current settings.  If the diagonal starts
    //  clipping (population pushes past the upper edge after a knob
    //  bump), widen these and rebuild.
    RootHist<TH2F> h_streaming_trigger_ring_peak_votes_vs_active_first("h_streaming_trigger_ring_peak_votes_vs_active_first", ";|active| hits;peak votes", 100, 0, 100, 50, 0, 50);
    RootHist<TH2F> h_streaming_trigger_ring_peak_votes_vs_active_second("h_streaming_trigger_ring_peak_votes_vs_active_second", ";|active| hits;peak votes", 100, 0, 100, 50, 0, 50);
    //  1D `|r_hit − R_ring|` per assigned hit, per ring slot.  Range
    //  bound to [0, 2 × collection_radius] so the threshold line at
    //  x = cfg.collection_radius sits at the midpoint of the axis.
    //  Bin width = collection_radius / 30  (~0.25 mm with the default
    //  7.5 mm collection_radius).
    constexpr int kArcDistBinsPerSide = 30;
    const int     ringArc_nbins = 2 * kArcDistBinsPerSide;
    const float   ringArc_hi    = 2.f * streaming_hough_cfg.collection_radius;
    RootHist<TH1F> h_streaming_trigger_ring_hit_arc_dist_first("h_streaming_trigger_ring_hit_arc_dist_first", ";|r_{hit} - R_{ring}| (mm)", ringArc_nbins, 0.f, ringArc_hi);
    RootHist<TH1F> h_streaming_trigger_ring_hit_arc_dist_second("h_streaming_trigger_ring_hit_arc_dist_second", ";|r_{hit} - R_{ring}| (mm)", ringArc_nbins, 0.f, ringArc_hi);

    //  Dual-ring sample QA — mirrors of the first-ring hists above
    //  restricted to events where a second ring was *also* found.
    //  Lets you A/B the first ring between the full sample and the
    //  cleaner 2-ring subset (interpretation in `StreamingHoughQA`).
    //  Same binning / axes as the unsuffixed twins for direct overlay.
    RootHist<TH2F> h_streaming_trigger_ring_finder_first_hitmap_dual("h_streaming_trigger_ring_finder_first_hitmap_dual", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH1F> h_streaming_trigger_ring_X_first_hough_dual("h_streaming_trigger_ring_X_first_hough_dual", ";x (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_Y_first_hough_dual("h_streaming_trigger_ring_Y_first_hough_dual", ";y (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_R_first_hough_dual("h_streaming_trigger_ring_R_first_hough_dual", ";R (mm)", ringR_nbins, ringR_lo, ringR_hi);
    //  (Fit-refined dual hists removed 2026-05-26 — see note above.)
    RootHist<TH2F> h_streaming_trigger_ring_peak_votes_vs_active_first_dual("h_streaming_trigger_ring_peak_votes_vs_active_first_dual", ";|active| hits;peak votes", 100, 0, 100, 50, 0, 50);
    RootHist<TH1F> h_streaming_trigger_ring_hit_arc_dist_first_dual("h_streaming_trigger_ring_hit_arc_dist_first_dual", ";|r_{hit} - R_{ring}| (mm)", ringArc_nbins, 0.f, ringArc_hi);

    //  Solo-ring sample QA — complement of the _dual set: same first-ring
    //  hists restricted to events where *no* second ring was found.
    //  Together (_solo + _dual) they partition the first-ring sample.
    RootHist<TH2F> h_streaming_trigger_ring_finder_first_hitmap_solo("h_streaming_trigger_ring_finder_first_hitmap_solo", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH1F> h_streaming_trigger_ring_X_first_hough_solo("h_streaming_trigger_ring_X_first_hough_solo", ";x (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_Y_first_hough_solo("h_streaming_trigger_ring_Y_first_hough_solo", ";y (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_R_first_hough_solo("h_streaming_trigger_ring_R_first_hough_solo", ";R (mm)", ringR_nbins, ringR_lo, ringR_hi);
    //  (Fit-refined solo hists removed 2026-05-26 — see note above.)
    RootHist<TH2F> h_streaming_trigger_ring_peak_votes_vs_active_first_solo("h_streaming_trigger_ring_peak_votes_vs_active_first_solo", ";|active| hits;peak votes", 100, 0, 100, 50, 0, 50);
    RootHist<TH1F> h_streaming_trigger_ring_hit_arc_dist_first_solo("h_streaming_trigger_ring_hit_arc_dist_first_solo", ";|r_{hit} - R_{ring}| (mm)", ringArc_nbins, 0.f, ringArc_hi);
    const float time_window_ns = streaming_trigger_cfg.time_window_ns;
    //  D-12 QA score histograms.  Always filled — the noise hist accumulates
    //  during the first-frames window of every spill, the data hist during
    //  the rest of the spill.  Same n_σ axis so the misfire and acceptance
    //  integrals at any threshold are directly comparable.  See
    //  include/triggers/DISCUSSION.md § 2.3.  Binning history: 500 → 250
    //  (Q2/2026 first pass) → 125 bins / [0,50] (0.4 n_σ/bin) — finer
    //  binning was just visual noise on the operating-point overlay.
    RootHist<TH1F> h_streaming_score_noise(
        "h_streaming_score_noise",
        ";n_{#sigma} (first-frames sample);entries",
        125, 0.f, 50.f);
    RootHist<TH1F> h_streaming_score_data(
        "h_streaming_score_data",
        ";n_{#sigma} (data-taking);entries",
        125, 0.f, 50.f);
    //  Colour scheme for the overlay canvas written further down:
    //  noise (first-frames) = red, data-taking = blue.  Set at hist
    //  creation so the colours stick whether you draw the individual
    //  hists or the overlay.
    h_streaming_score_noise->SetLineColor(kRed);
    h_streaming_score_data ->SetLineColor(kBlue);
    h_streaming_score_noise->SetLineWidth(2);
    h_streaming_score_data ->SetLineWidth(2);
    //  (No timing-QA histograms remain after the 2026-Q2 QA sweep.  The
    //  full list of dropped hists — delta_t_leading_edge / half_centroid /
    //  first_half / second_half, h_delta_median_vs_window, the
    //  h_streaming_trigger_tdc_* trio, and most recently h_sigma_vs_nhits —
    //  is preserved in DISCUSSION.md § 2.5 alongside the open items that
    //  may justify reintroducing a focused subset.)
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
    //  LUT geometry from streaming_hough_cfg (Phase 4 — was hardcoded
    //  20/120/1./3.).  build_lut is constructor-side here; the same
    //  cfg is also passed per-frame to run_streaming_hough_trigger
    //  for the per-event parameters.
    mist::ring_finding::HoughTransform ring_finder(index_to_hit_xy,
                                                   streaming_hough_cfg.r_min,
                                                   streaming_hough_cfg.r_max,
                                                   streaming_hough_cfg.r_step,
                                                   streaming_hough_cfg.cell_size,
                                                   streaming_hough_cfg.centre_padding_mm);

    //  ---
    //  Loop over spills
    if (max_spill != 1000)
        mist::logger::info("(ParallelStreamingFramer::next_spill) Requested to stop at spill : " +
                           std::to_string(max_spill));

    // ── Streaming-trigger weights (D-12) ─────────────────────────────────
    // The bundle persists across spills (cumulative DCR via h_dcr_per_channel
    // → newer spills supersede older builds, but a freshly-rebuilt-empty
    // bundle would lose spill 0's data when spill 1's noise frames fire).
    // The rebuild itself happens once per spill, at the noise → data
    // boundary inside the per-frame loop — see the `built_for_spill` flag
    // reset at the start of each spill and the in-loop rebuild block.
    StreamingTriggerWeights streaming_weights;

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

        //  --- Disabled: per-spill online calibration update ---
        //
        //  The canonical calibration path is the offline pass via
        //  macros/examples/fine_calibration_timing.cpp.  Restoring this call
        //  would seed each spill's calibration table with the channels that
        //  became active during the previous spill — useful only for an
        //  online-only mode where no offline calibration is available.
        //  Re-enable by uncommenting the line below; otherwise leave it.
        //
        //  LINT-OK: documented intentional disable (CODE_REVIEW §D-11 review).
        //  spilldata.update_calibration(framer.get_fine_tune_distribution());

        //  Calculate participants channel
        // Phase 4 internal cleanup: the "active sensors" set is keyed by
        // GlobalIndex::channel_ordinal() instead of the legacy
        // `legacy_raw / 4` pattern.  The lookup at line ~1044 below (DCR-QA
        // per-channel count map) uses the same expression, keeping the set
        // ↔ count-map keys in sync.
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
                        // Use channel_ordinal — a dense small integer suitable
                        // for histogram bins.  global_channel_raw() would carry
                        // the full packed device-bits encoding (millions/sparse)
                        // and overflow the per-channel TProfile axes downstream.
                        active_sensors.insert(static_cast<uint32_t>(gi.channel_ordinal()));
                    }

        n_active_cherenkov_channels = active_sensors.size();

        //  Streaming-trigger weights (D-12: DCR-weighted, n_σ-thresholded).
        //  The bundle itself is run-scope (declared above the spill loop),
        //  so spill N's noise frames see spill N-1's already-built weights.
        //  We only reset the per-spill "have we rebuilt yet" flag here;
        //  the actual rebuild happens once, at the noise → data boundary
        //  inside the per-frame loop.  Rebuilding per spill (rather than
        //  once per run) tracks channels that come online / drop out across
        //  spills (e.g. an RDO that was off in spill 0 starts contributing
        //  from spill 1) and channels whose rates drift over the fill.
        bool streaming_weights_built_for_spill = false;

        //  Hough ring-finder `min_active` — a *separate* knob from the
        //  streaming trigger above (the Hough operates on candidate
        //  frames the trigger already accepted, and wants a minimum hit
        //  count to seed a ring).  Was historically shared with the
        //  streaming-trigger `threshold` int; D-12 split the two.
        //  Formula: ceil(cfg.hough_threshold_fraction × N_active_Cherenkov),
        //  floored at 1.  Phase 4 wired the fraction to the config.
        const int hough_min_active = std::max(
            1, static_cast<int>(std::ceil(streaming_hough_cfg.hough_threshold_fraction *
                                          n_active_cherenkov_channels)));

        std::vector<std::tuple<int, float, float>> carry_over_hits;

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

                    if (timing_chip_0_id >= 0 && chip == timing_chip_0_id && !tdc_time_0.count(gidx_legacy))
                    {
                        tdc_time_0[gidx_legacy] = time_cc;
                        seen_channels_0.insert(channel);
                    }
                    if (timing_chip_1_id >= 0 && chip == timing_chip_1_id && !tdc_time_1.count(gidx_legacy))
                    {
                        tdc_time_1[gidx_legacy] = time_cc;
                        seen_channels_1.insert(channel);
                    }
                }

                // Only process full-occupancy events
                if (timing_chip_0_id >= 0 &&
                    seen_channels_0.size() == static_cast<size_t>(kTimingChip0AliveChannels))
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
                if (timing_chip_1_id >= 0 &&
                    seen_channels_1.size() == static_cast<size_t>(kTimingChip1AliveChannels))
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

        //  Per-frame CT scratch buffers — hoisted out of the inner loop
        //  (CODE_REVIEW §4.8).  Before this change each frame allocated a
        //  fresh std::vector<CtHit> + std::vector<std::size_t>, even though
        //  the capacities settle quickly after the first few frames.  Now we
        //  reuse the same storage across frames within a spill and .clear()
        //  at the top of each frame: typical capacity stabilises within a
        //  spill, eliminating the realloc churn on the hot path.
        //  `CtHit` was lifted to `include/writers/lightdata/types.h` in
        //  Phase F (2026-05-27) so the extracted per-frame QA helper
        //  (`fill_dcr_afterpulse_ct_qa`) can use the same record type.
        using ::btana::lightdata::CtHit;
        std::vector<CtHit> ct_hits;
        std::vector<std::size_t> sorted_by_time;

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

                if (timing_chip_0_id >= 0 && chip == timing_chip_0_id && seen_channels_0.insert(channel).second)
                {
                    ++timing_hits_0;
                    timing_sum_0 += time_ns;
                }
                if (timing_chip_1_id >= 0 && chip == timing_chip_1_id && seen_channels_1.insert(channel).second)
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
                    (timing_hits_0 == kTimingChip0AliveChannels) &&
                    (timing_hits_1 == kTimingChip1AliveChannels) &&
                    (std::fabs(delta_timing - kDeltaTimingCenter) < kDeltaTimingNSigma * kDeltaTimingWindow);

                if (timing_available)
                {
                    h_timing_ref_delta_sel->Fill(delta_timing);
                    spilldata.add_trigger_to_frame(frame_id, {static_cast<uint8_t>(TriggerTiming),
                                                              static_cast<uint16_t>(framer_cfg.frame_size / 2),
                                                              ref_timing});
                }
            }

            //  --- Cherenkov sliding window trigger (D-12: DCR-weighted score).
            //  QA score histogram destination depends on which window of the
            //  spill we're in: first-frames → noise sample; rest → data sample.
            const bool is_first_frames_window =
                (static_cast<int>(frame_id) < framer_cfg.first_frames_trigger);
            //  Build streaming weights exactly once per spill, at the moment
            //  the first data frame is encountered (i.e. immediately after
            //  the spill's 5000 first-frames trigger frames have completed
            //  their DCR fills into h_dcr_per_channel).  Cumulative across
            //  spills + this spill's own noise data — captures channels that
            //  come online late (RDO previously off) and channels that drift.
            //  During the first-frames window itself, `streaming_weights`
            //  remains empty: the noise QA hist fills at n_σ = 0 for spill 0
            //  but accumulates real distributions from spill 1 onward (where
            //  prior spills' DCR informs the bundle once it's built).
            if (!is_first_frames_window && !streaming_weights_built_for_spill)
            {
                streaming_weights = build_streaming_trigger_weights(
                    h_dcr_per_channel.get(),
                    streaming_trigger_cfg.time_window_ns,
                    framer_cfg.frame_length_ns(),
                    streaming_trigger_cfg.min_noise_hits,
                    &active_sensors);   // restrict to this spill's participants
                streaming_weights_built_for_spill = true;
                //  Sanity log — confirms the active-channel filter is firing:
                //  n_modelled should equal min(N_active_this_spill, N_measured).
                //  If it equals N_measured even when some RDOs are off this
                //  spill, the filter isn't being applied.
                mist::logger::info("(streaming_trigger) Spill " +
                                   std::to_string(ispill) +
                                   ": active=" + std::to_string(active_sensors.size()) +
                                   ", modelled=" + std::to_string(streaming_weights.n_channels_modelled) +
                                   ", E[S]=" + std::to_string(streaming_weights.expected_score_per_window) +
                                   ", σ_S=" + std::to_string(streaming_weights.sigma_score_per_window));
            }
            TH1F *h_score_for_this_frame = is_first_frames_window
                                               ? h_streaming_score_noise.get()
                                               : h_streaming_score_data.get();
            run_streaming_trigger_weighted(
                spilldata,
                frame_id,
                streaming_trigger_cfg.time_window_ns,
                streaming_weights,
                streaming_trigger_cfg.n_sigma_threshold,
                carry_over_hits,
                h_score_for_this_frame,
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
                //  --- Streaming Trigger — stage 2 (Hough ring finder).
                //  Extracted into triggers/streaming/hough.cxx (Phase 3).
                //  The QA-pointer struct holds raw `.get()`s; any field
                //  left as nullptr disables that fill.
                StreamingHoughQA hough_qa;
                hough_qa.full_hitmap        = h_streaming_trigger_full_hitmap.get();
                hough_qa.time_cut_hitmap    = h_streaming_trigger_time_cut_hitmap.get();
                hough_qa.nrings             = h_streaming_trigger_ring_finder_nrings.get();
                hough_qa.ring_finder_hitmap = h_streaming_trigger_ring_finder_hitmap.get();
                hough_qa.first_hitmap       = h_streaming_trigger_ring_finder_first_hitmap.get();
                hough_qa.second_hitmap      = h_streaming_trigger_ring_finder_second_hitmap.get();
                //  (Per-ring fit_circle QA assignments removed 2026-05-26
                //   — fit moved fully to recodata.  Hough-seed QA assignments
                //   below remain.)
                hough_qa.ring_X_first_hough   = h_streaming_trigger_ring_X_first_hough.get();
                hough_qa.ring_Y_first_hough   = h_streaming_trigger_ring_Y_first_hough.get();
                hough_qa.ring_R_first_hough   = h_streaming_trigger_ring_R_first_hough.get();
                hough_qa.ring_X_second_hough  = h_streaming_trigger_ring_X_second_hough.get();
                hough_qa.ring_Y_second_hough  = h_streaming_trigger_ring_Y_second_hough.get();
                hough_qa.ring_R_second_hough  = h_streaming_trigger_ring_R_second_hough.get();
                hough_qa.ring_peak_votes_vs_active_first  = h_streaming_trigger_ring_peak_votes_vs_active_first.get();
                hough_qa.ring_peak_votes_vs_active_second = h_streaming_trigger_ring_peak_votes_vs_active_second.get();
                hough_qa.ring_hit_arc_dist_first          = h_streaming_trigger_ring_hit_arc_dist_first.get();
                hough_qa.ring_hit_arc_dist_second         = h_streaming_trigger_ring_hit_arc_dist_second.get();
                //  Dual-ring mirror — gated inside the trigger on found_rings.size() > 1.
                hough_qa.first_hitmap_dual                    = h_streaming_trigger_ring_finder_first_hitmap_dual.get();
                hough_qa.ring_X_first_hough_dual              = h_streaming_trigger_ring_X_first_hough_dual.get();
                hough_qa.ring_Y_first_hough_dual              = h_streaming_trigger_ring_Y_first_hough_dual.get();
                hough_qa.ring_R_first_hough_dual              = h_streaming_trigger_ring_R_first_hough_dual.get();
                //  (Fit-refined dual hist assignments removed 2026-05-26.)
                hough_qa.ring_peak_votes_vs_active_first_dual = h_streaming_trigger_ring_peak_votes_vs_active_first_dual.get();
                hough_qa.ring_hit_arc_dist_first_dual         = h_streaming_trigger_ring_hit_arc_dist_first_dual.get();
                //  Solo-ring mirror — gated inside the trigger on found_rings.size() == 1.
                hough_qa.first_hitmap_solo                    = h_streaming_trigger_ring_finder_first_hitmap_solo.get();
                hough_qa.ring_X_first_hough_solo              = h_streaming_trigger_ring_X_first_hough_solo.get();
                hough_qa.ring_Y_first_hough_solo              = h_streaming_trigger_ring_Y_first_hough_solo.get();
                hough_qa.ring_R_first_hough_solo              = h_streaming_trigger_ring_R_first_hough_solo.get();
                //  (Fit-refined solo hist assignments removed 2026-05-26.)
                hough_qa.ring_peak_votes_vs_active_first_solo = h_streaming_trigger_ring_peak_votes_vs_active_first_solo.get();
                hough_qa.ring_hit_arc_dist_first_solo         = h_streaming_trigger_ring_hit_arc_dist_first_solo.get();
                run_streaming_hough_trigger(
                    spilldata, frame_id, ring_finder, hough_min_active,
                    streaming_trigger, ispill,
                    streaming_trigger_cfg.time_window_ns,
                    streaming_hough_cfg,
                    hough_qa);

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
                        h_trigger_frame_population[current_trigger.index] = RootHist<TH1F>(TString::Format("h_trigger_frame_population_%s", registry.name_of(current_trigger.index).c_str()).Data(), TString::Format(";frame number; %s;", registry.name_of(current_trigger.index).c_str()).Data(), 5e3, 0, 5e6);
                        h_trigger_time_diff_w_cherenkov[current_trigger.index] = RootHist<TH1F>(TString::Format("h_trigger_time_diff_w_cherenkov_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";#Delta_{t} (t_{Hit} - t_{trigger}) ns;Normalised entries", 5e3, -500, 500);
                        h_trigger_full_hitmap[current_trigger.index] = RootHist<TH2F>(TString::Format("h_trigger_full_hitmap_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
                        h_trigger_hit_multiplicity[current_trigger.index][0] = RootHist<TH1F>(TString::Format("h_trigger_hit_multiplicity_in_time_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";n_{Hit}; events;", 100, 0, 100);
                        h_trigger_hit_multiplicity[current_trigger.index][1] = RootHist<TH1F>(TString::Format("h_trigger_hit_multiplicity_out_of_time_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";n_{Hit}; events;", 100, 0, 100);
                        h_trigger_dt[current_trigger.index] = RootHist<TH2F>(
                            TString::Format("h_trigger_dt_%s", registry.name_of(current_trigger.index).c_str()).Data(),
                            TString::Format(";spill index;#Delta_{t} between consecutive %s triggers (ns);entries",
                                 registry.name_of(current_trigger.index).c_str()).Data(),
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
                //  --- DCR + afterpulse + cross-talk QA
                //  Gated on the first-frames trigger.  The fill body was
                //  lifted to `src/writers/lightdata/dcr_afterpulse_ct_qa.cxx`
                //  in Phase F of the lightdata modularisation (2026-05-27);
                //  ~190 lines moved out of this writer.  Behaviour is
                //  identical, verified against
                //  `Data/20251111-164951/phaseF_baseline/`.
                if (fired_trigger_types.count(registry.index_of(static_cast<TriggerNumber>(TriggerFirstFrames))))
                {
                    ::btana::lightdata::DcrAfterpulseCtHists qa_hists;
                    qa_hists.h_dcr_per_channel             = h_dcr_per_channel.get();
                    qa_hists.h_dcr_hitmap                  = h_dcr_hitmap.get();
                    qa_hists.h_afterpulse_near_per_channel = h_afterpulse_near_per_channel.get();
                    qa_hists.h_afterpulse_far_per_channel  = h_afterpulse_far_per_channel.get();
                    qa_hists.h_afterpulse_per_channel      = h_afterpulse_per_channel.get();
                    qa_hists.h_afterpulse_near_hitmap      = h_afterpulse_near_hitmap.get();
                    qa_hists.h_afterpulse_far_hitmap       = h_afterpulse_far_hitmap.get();
                    qa_hists.h_afterpulse_hitmap           = h_afterpulse_hitmap.get();
                    qa_hists.h_phys_ct_per_channel         = h_phys_ct_per_channel.get();
                    qa_hists.h_elec_ct_per_channel         = h_elec_ct_per_channel.get();
                    qa_hists.h_phys_ct_hitmap              = h_phys_ct_hitmap.get();
                    qa_hists.h_elec_ct_hitmap              = h_elec_ct_hitmap.get();
                    qa_hists.h_phys_ct_dt                  = h_phys_ct_dt.get();
                    qa_hists.h_elec_ct_dt                  = h_elec_ct_dt.get();
                    qa_hists.h_elec_ct_dchannel_dt         = h_elec_ct_dchannel_dt.get();
                    qa_hists.h_phys_ct_dchannel_dt         = h_phys_ct_dchannel_dt.get();
                    ::btana::lightdata::fill_dcr_afterpulse_ct_qa(
                        cherenkov_hits, active_sensors, active_sensors_count,
                        ct_hits, sorted_by_time, qa_cfg, qa_hists);
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
        // Per-spill outfile->Flush() removed (§4.2): it fsync'd to disk on
        // every spill, which is brutal on HDDs and lethal on networked
        // storage.  The tree's SetAutoFlush(-30000000) above lets the
        // basket buffers handle scheduling instead.
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
        h_rollover_correction_ticks_per_stream_and_spill = RootHist<TH2F>(
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
    for (auto &[key, val] : h_trigger_frame_population)
        val->Write();
    for (auto &[key, val] : h_trigger_time_diff_w_cherenkov)
    {
        val->Scale(1. / h2_trigger_matrix->GetBinContent(registry.index_of(key) + 1, registry.index_of(key) + 1));
        val->Write();
    }
    for (auto &[key, val] : h_trigger_hit_multiplicity)
    {
        val[0]->Write();
        val[1]->Write();
    }
    for (auto &[key, val] : h_trigger_full_hitmap)
        val->Write();
    for (auto &[key, val] : h_trigger_dt)
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
    //  D-12 QA score histograms — drive the threshold-tuning workflow described
    //  in include/triggers/DISCUSSION.md § 2.4.  Same n_σ axis on both so the
    //  misfire and acceptance integrals at any threshold are directly comparable.
    //  Normalised by entry count so y-axis is **probability per bin** rather
    //  than raw counts — makes noise vs data integrals above a threshold
    //  directly readable as false-positive rate vs signal acceptance.
    if (h_streaming_score_noise->GetEntries() > 0)
        h_streaming_score_noise->Scale(1.0 / h_streaming_score_noise->GetEntries());
    if (h_streaming_score_data->GetEntries() > 0)
        h_streaming_score_data->Scale(1.0 / h_streaming_score_data->GetEntries());
    h_streaming_score_noise->GetYaxis()->SetTitle("probability per bin");
    h_streaming_score_data ->GetYaxis()->SetTitle("probability per bin");
    h_streaming_score_noise->Write();
    h_streaming_score_data->Write();

    //  Pre-made overlay canvas for visual threshold tuning.
    //  Noise (first-frames) in red, data-taking in blue, log-Y so the tails
    //  separating signal from noise are visible across many decades.  The
    //  canvas is built self-contained: clones of the hists (detached from
    //  the output TDirectory and marked kCanDelete) plus a heap-allocated
    //  legend.  Without this, the reopened canvas references freed memory
    //  (stack-scoped legend, hists owned by RootHist) and segfaults inside
    //  TCanvas::Build when the browser tries to redraw it.
    {
        TCanvas *c_streaming_score_overlay = new TCanvas(
            "c_streaming_score_overlay",
            "Streaming-trigger score: noise (red) vs data (blue)",
            1600, 800);
        c_streaming_score_overlay->cd();
        c_streaming_score_overlay->SetLogy();
        c_streaming_score_overlay->SetGridx();
        c_streaming_score_overlay->SetGridy();

        //  Clone hists into the canvas: detached from any directory, with
        //  kCanDelete set so the canvas destructor frees them.  Unique names
        //  to avoid collisions with the standalone hists written elsewhere
        //  in the same TDirectory.
        TH1F *h_data_overlay = static_cast<TH1F *>(
            h_streaming_score_data->Clone("h_streaming_score_data_overlay"));
        h_data_overlay->SetDirectory(nullptr);
        h_data_overlay->SetBit(TObject::kCanDelete);
        h_data_overlay->SetTitle(
            "Streaming-trigger score;n_{#sigma};probability per bin");
        h_data_overlay->Draw("HIST");

        TH1F *h_noise_overlay = static_cast<TH1F *>(
            h_streaming_score_noise->Clone("h_streaming_score_noise_overlay"));
        h_noise_overlay->SetDirectory(nullptr);
        h_noise_overlay->SetBit(TObject::kCanDelete);
        h_noise_overlay->Draw("HIST SAME");

        TLegend *leg = new TLegend(0.65, 0.75, 0.88, 0.88);
        leg->SetBorderSize(0);
        leg->SetFillStyle(0);
        leg->AddEntry(h_data_overlay,  "Data-taking (signal + noise)", "l");
        leg->AddEntry(h_noise_overlay, "First-frames (noise only)",    "l");
        leg->SetBit(TObject::kCanDelete);
        leg->Draw();

        c_streaming_score_overlay->Modified();
        c_streaming_score_overlay->Update();
        c_streaming_score_overlay->Write();
        delete c_streaming_score_overlay;   // also deletes overlay clones + legend
    }
    h_streaming_trigger_full_hitmap->Write();
    h_streaming_trigger_time_cut_hitmap->Write();
    h_streaming_trigger_ring_finder_nrings->Write();
    h_streaming_trigger_ring_finder_hitmap->Write();
    h_streaming_trigger_ring_finder_first_hitmap->Write();
    h_streaming_trigger_ring_finder_second_hitmap->Write();

    //  Per-ring centre + radius distributions, split into two subfolders
    //  so the upstream → downstream chain (Hough seed → fit_circle refine)
    //  is visually grouped in TBrowser.  Order matters: the Hough seed is
    //  what the fit climbs from, so any bad tails in `Fit rings/` should
    //  first be checked against the same hist in `Hough rings/` — if the
    //  seed is already wrong, the fit was given a bad starting point
    //  (relevant open items in DISCUSSION.md § 2.5).
    {
        TDirectory *hough_rings_dir = streaming_trigger_dir->mkdir("Hough rings");
        hough_rings_dir->cd();
        h_streaming_trigger_ring_X_first_hough->Write();
        h_streaming_trigger_ring_Y_first_hough->Write();
        h_streaming_trigger_ring_R_first_hough->Write();
        h_streaming_trigger_ring_X_second_hough->Write();
        h_streaming_trigger_ring_Y_second_hough->Write();
        h_streaming_trigger_ring_R_second_hough->Write();
        //  Knob-calibration QA (see § 2.5 in the streaming DISCUSSION).
        h_streaming_trigger_ring_peak_votes_vs_active_first->Write();
        h_streaming_trigger_ring_peak_votes_vs_active_second->Write();
        h_streaming_trigger_ring_hit_arc_dist_first->Write();
        h_streaming_trigger_ring_hit_arc_dist_second->Write();

        //  ("Fit rings/" subfolder removed 2026-05-26 — fit_circle work
        //   moved entirely to recodata.  See recodata.root's `Rings/`
        //   subfolder for all fit-derived QA.)

        //  Dual-ring mirror — same hists as `Hough rings/`
        //  above but for the first ring, gated on a second ring also
        //  being present in the same frame.  Lets you compare ring-1
        //  properties in the full sample vs the cleaner 2-ring subset.
        TDirectory *hough_rings_dual_dir = streaming_trigger_dir->mkdir("Hough rings (dual)");
        hough_rings_dual_dir->cd();
        h_streaming_trigger_ring_finder_first_hitmap_dual->Write();
        h_streaming_trigger_ring_X_first_hough_dual->Write();
        h_streaming_trigger_ring_Y_first_hough_dual->Write();
        h_streaming_trigger_ring_R_first_hough_dual->Write();
        h_streaming_trigger_ring_peak_votes_vs_active_first_dual->Write();
        h_streaming_trigger_ring_hit_arc_dist_first_dual->Write();

        //  ("Fit rings (dual)/" subfolder removed 2026-05-26.)

        //  Solo-ring mirror — complement of (dual). Together they
        //  partition the full first-ring sample, so any systematic
        //  difference between (solo) and (dual) flags single-ring
        //  contamination that the dual requirement filters out.
        TDirectory *hough_rings_solo_dir = streaming_trigger_dir->mkdir("Hough rings (solo)");
        hough_rings_solo_dir->cd();
        h_streaming_trigger_ring_finder_first_hitmap_solo->Write();
        h_streaming_trigger_ring_X_first_hough_solo->Write();
        h_streaming_trigger_ring_Y_first_hough_solo->Write();
        h_streaming_trigger_ring_R_first_hough_solo->Write();
        h_streaming_trigger_ring_peak_votes_vs_active_first_solo->Write();
        h_streaming_trigger_ring_hit_arc_dist_first_solo->Write();

        //  ("Fit rings (solo)/" subfolder removed 2026-05-26.)

        streaming_trigger_dir->cd();   // restore parent for any later writes
    }
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
        write_toml_snapshot("streaming_conf_toml", streaming_conf_file);
    }
    //  outfile closed automatically by TFilePtr dtor (CODE_REVIEW §4.12).
    //  End: QA plots
    //  --- --- --- --- --- ---
}
