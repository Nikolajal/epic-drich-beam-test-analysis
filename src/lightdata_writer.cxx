#include "parallel_streaming_framer.h"
#include <mist/logger/logger.h>
#include "writers/lightdata.h"
#include "writers/lightdata/types.h"                 // CtHit
#include "writers/lightdata/dcr_afterpulse_ct_qa.h"  // fill_dcr_afterpulse_ct_qa
#include "writers/lightdata/finalize_streaming_qa.h" // finalize_streaming_qa
#include "writers/anchor_dt_canvas.h"                // render_anchor_dt_canvas
#include "triggers/streaming/score.h"
#include "triggers/streaming/ransac.h"
#include "mapping.h"
#include <mist/ring_finding/hough_transform.h>
#include "TROOT.h"
#include "TProfile.h"
#include "TParameter.h"
#include "analysis_results.h"
#include "utility/config_dump.h"
#include "utility/qa_publish.h"
#include "TCanvas.h"
#include "TLegend.h"
#include "TLine.h"
#include "TLatex.h"
#include "TMarker.h"
#include "TStyle.h"
#include "TF1.h"
#include "TMath.h"
#include "TH1.h"
#include <algorithm>
#include <array>
#include <numeric>

// ── Timing-trigger coincidence params ─────────────────────────────────────
// The per-chip alive-channel counts (the same-channel-offset calibration +
// the timing trigger only use events where every alive channel on a chip
// fires) and the chip0−chip1 Δt acceptance window (centre, half-width, Nσ)
// now live in the readout config's [readout.timing] section
// (ReadoutConfigStruct::timing_chip{0,1}_alive_channels + timing_delta_*),
// captured into locals at the chip-id resolution block below.  Note: the
// mean-of-others calibration divides by (alive_channels − 1), so the config
// values must be ≥ 2.

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
    std::string streaming_conf_file,
    float streaming_n_sigma_threshold_override,
    int op_mode,
    bool leading_edge_only)
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
    //  Zero-data guard (added after a retention-pruned run
    //  silently produced an empty lightdata.root and reported success).
    //  No devices == no raw decoded data on disk == nothing to process.
    //  This typically means the run dir was demoted by the dashboard's
    //  retention sweep to the QA-only tier (raw decoded files removed)
    //  AND THEN the operator tried to re-run the writer.  The right move
    //  is to re-download the run from the DAQ host; fail loud so the
    //  operator catches it rather than burning time on an empty output.
    if (sorted_devices.empty())
    {
        mist::logger::error(TString::Format(
                                "(lightdata_writer) No decoded device data under %s/%s/ — "
                                "every device dir is missing OR has no decoded/*.root files.  "
                                "If the dashboard's retention sweep demoted this run to the "
                                "QA-only tier, the raw decoded data was deleted (recodata.root "
                                "+ qa/*.pdf still survive).  Re-download the run from the DAQ "
                                "host before re-running the writer.",
                                data_repository.c_str(), run_name.c_str())
                                .Data());
        return;
    }
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
        std::string fifo_list;
        for (auto n : fifo_numbers)
            fifo_list += " " + std::to_string(n);
        mist::logger::info("[Device: " + current_device + "] Found fifos:" + fifo_list);
    }
    //  ---
    //  End: Input files
    //  --- --- --- --- --- ---

    //  --- --- --- --- --- ---
    //  Framing data & output definition
    //
    //  Open follow-ups (FIFO-in-config, single/multi-core consistency
    //  test, afterpulse-fraction plot, QA restructure, config-from-
    //  outside) are tracked as a sub-roadmap in
    //  include/writers/DISCUSSION.md → "lightdata writer — 5 grouped
    //  @todos sub-roadmap".  Kept out of the source as @todo lines
    //  since the design context (which TOML knob, which dataset to
    //  diff against, etc.) doesn't fit on a per-line basis.
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
    auto &progress_framer = progress_bars.add_subtask("framer");
    auto &progress_postprocessing = progress_bars.add_subtask("post-processing");
    progress_bars.update(0, max_spill); // arms the main bar in the correct mode
    int streaming_trigger = 0;

    //  Load framer + QA configuration (both live in framer_conf_file)
    auto framer_cfg = FramerConfReader(framer_conf_file);
    auto qa_cfg = qa_conf_reader(framer_conf_file);
    //  Push the low-stats retry policy onto the AlcorFinedata static
    //  cache before the spill loop so the very first per-spill
    //  `update_calibration` sees the configured period.  Default `0`
    //  means "never retry" — see QaConfigStruct doc + the cache block
    //  in include/alcor_finedata.h.
    AlcorFinedata::set_low_stats_retry_period(
        qa_cfg.generate_calibration_low_stats_retry_period);
    //  Software trigger pipeline lives in its own conf file (streaming.toml).
    //  Both stages share that file; the RANSAC struct is consumed downstream
    //  (LUT geometry, threshold derivation, centre-XY QA range).
    auto streaming_trigger_cfg = streaming_trigger_conf_reader(streaming_conf_file);
    //  Per-run streaming threshold override (DISCUSSION §1.5.2 Option A).
    //  When the analyser has tuned a per-run cut from the score canvas
    //  and recorded it in the run database (e.g.
    //  `run-lists/2026.database.toml` → `streaming_n_sigma_threshold = X`),
    //  the CLI driver passes that value via this parameter and we
    //  override the streaming-conf default here.  0 (the default)
    //  leaves the streaming-conf value untouched, so legacy launch
    //  paths that don't supply the rundb behave identically to before.
    if (streaming_n_sigma_threshold_override > 0.f)
    {
        mist::logger::info(TString::Format(
                               "(lightdata_writer) Per-run streaming n_sigma threshold "
                               "override: %.3f (was %.3f from streaming-conf %s)",
                               streaming_n_sigma_threshold_override,
                               streaming_trigger_cfg.n_sigma_threshold,
                               streaming_conf_file.c_str())
                               .Data());
        streaming_trigger_cfg.n_sigma_threshold =
            streaming_n_sigma_threshold_override;
    }
    auto streaming_ransac_cfg = streaming_ransac_conf_reader(streaming_conf_file);

    //  Per-trigger in-window Cherenkov hitmaps use the SAME timing cut as the
    //  recodata ring reconstruction ([streaming_ransac] hardware_ring_dt_min/max_ns),
    //  so they agree with the recodata ring hitmap on which hits are "in time".
    //  Both knobs now live where the recodata pipeline reads them: the ring-reco
    //  knobs in streaming.toml's [streaming_ransac] table, the coverage geometry
    //  in mapping_conf.toml's [coverage] table — both writer params already.
    auto recodata_cfg = recodata_conf_reader(streaming_conf_file, mapping_config_file);
    const float kTrigCherDtMin = recodata_cfg.hardware_ring_dt_min_ns;
    const float kTrigCherDtMax = recodata_cfg.hardware_ring_dt_max_ns;

    //  Create streaming framer
    ParallelStreamingFramer framer(filenames, trigger_setup_file, readout_config_file, framer_cfg);
    framer.set_qa_config(qa_cfg); // enable afterpulse near/far Hit-mask tagging
    framer.set_op_mode(to_alcor_op_mode(op_mode)); // LET (default) keeps legacy path; ToT pairs edges
    framer.set_leading_edge_only(leading_edge_only); // ToT-as-LET: leading (even) TDCs only, no pairing
    framer.set_parallel_cores(requested_n_threads);
    framer.resolve_rollover_offsets();  // populates the per-stream, per-spill correction table consumed by the Rollover QA fills below
    framer.assign_bar(progress_framer); // framer drives the framer subtask automatically

    // Resolve timing chip IDs from the readout config —
    // previously these were hardcoded as kTimingChip{0,1}Id = {0, 2}, duplicating
    // information already in conf/readout_config.toml.  -1 sentinel = "no such
    // chip configured"; the same-channel-offset calibration loop below skips
    // its per-chip branch when the corresponding ID is < 0.
    int timing_chip_0_id = -1;
    int timing_chip_1_id = -1;
    //  Timing-trigger coincidence params from the readout config
    //  ([readout.timing]); defaults match the historical hard-coded
    //  32/31 channels and the −0.5 ± 0.5 ns × 3σ Δt window.
    int timing_chip0_alive = 32, timing_chip1_alive = 31;
    float timing_delta_center = -0.5f, timing_delta_window = 0.5f,
          timing_delta_nsigma = 3.0f;
    if (const auto *timing_cfg = framer.get_readout_config().find_by_name("timing");
        timing_cfg && !timing_cfg->device_chip.empty())
    {
        // The readout config conventionally lists exactly one timing device
        // (e.g. id=200) with two chips.  Take the first device's chip list.
        const auto &chips = timing_cfg->device_chip.begin()->second;
        if (chips.size() >= 1)
            timing_chip_0_id = static_cast<int>(chips[0]);
        if (chips.size() >= 2)
            timing_chip_1_id = static_cast<int>(chips[1]);
        if (chips.size() > 2)
            mist::logger::warning(TString::Format(
                                      "(lightdata_writer) Timing readout has %zu chips; only the first two "
                                      "are used by the same-channel-offset calibration. "
                                      "Extend the calibration loop if more timing chips become active.",
                                      chips.size())
                                      .Data());
        timing_chip0_alive = timing_cfg->timing_chip0_alive_channels;
        timing_chip1_alive = timing_cfg->timing_chip1_alive_channels;
        timing_delta_center = timing_cfg->timing_delta_center_ns;
        timing_delta_window = timing_cfg->timing_delta_window_ns;
        timing_delta_nsigma = timing_cfg->timing_delta_n_sigma;
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
    //  Load fine_calibration — optional.  Empty path means the
    //  operator hasn't generated a v3 fine_calibration.toml for this
    //  run yet (e.g. dashboard exploring a fresh run before the
    //  pulser writer has produced one).  ``AlcorFinedata::get_phase()``
    //  returns 0.f for channels not in the calibration table, so the
    //  framer + writer still produce meaningful coarse-domain plots
    //  (Δt_{trg} vs spill etc.); only the sub-cc fine residual is
    //  lost.  Trade-off: yes-warn, don't-crash.
    if (!fine_calibration_config_file.empty())
    {
        //  Sweep audit: wrap read_calib_from_file in a
        //  try/catch with fallback.  C4.4 promoted schema-mismatch +
        //  zero-entry from warning to std::runtime_error; an uncaught
        //  throw here aborts the entire QA cascade.  We'd rather fall
        //  back to "no calibration loaded → phase=0 per channel"
        //  (same semantics as the empty-path branch below) so the
        //  operator still gets coarse-domain output and can
        //  re-generate the calibration without restarting from the
        //  raw bag.  The error itself is still surfaced in the log.
        try
        {
            AlcorFinedata::read_calib_from_file(fine_calibration_config_file);
        }
        catch (const std::exception &e)
        {
            mist::logger::error(
                "(lightdata_writer) read_calib_from_file('" +
                fine_calibration_config_file + "') failed: " +
                std::string(e.what()) +
                " — proceeding with phase = 0 for every channel; "
                "re-generate the calibration or fix the file then "
                "re-run.  Coarse-domain plots will be correct; only "
                "the sub-cc fine residual is lost.");
        }
    }
    else
    {
        mist::logger::warning("(lightdata_writer) No fine_calibration config "
                              "supplied — running with phase = 0 for every "
                              "channel.  Coarse-domain plots are fine; "
                              "fine-time residuals will be uncorrected.");
    }
    //  Calibration table is now fully loaded; flip the immutability flag so
    //  the per-Hit AlcorFinedata::get_phase() readers can take the lock-free
    //  fast path inside the framer's worker threads
    AlcorFinedata::freeze_calibration();
    //  Link output tree.  TFilePtr is owning — closes + deletes on function
    //  exit (including early returns and exception unwind) so there is no
    //  manual outfile->Close() call at the bottom
    TFilePtr outfile(TFile::Open(outfile_name.c_str(), "RECREATE"));
    if (!outfile || outfile->IsZombie())
    {
        mist::logger::error("(lightdata_writer) Failed to create output file: " + outfile_name);
        return;
    }
    //  Reco-provenance: stamp the ALCOR operation mode this lightdata was
    //  reconstructed under, so downstream consumers know how `duration` and the
    //  edge pairing were produced.  Written into the output file root directory.
    outfile->cd();
    TParameter<int>("alcor_op_mode", op_mode).Write();
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
    RootHist<TH2F> h_rollover_correction_ticks_per_stream_and_spill; // empty until the conditional below builds it
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
    //  Per-trigger in-window Cherenkov hitmap: (x, y) of every cherenkov hit
    //  inside [kTrigCherDtMin, kTrigCherDtMax] of the trigger (the config
    //  timing cut) — the in-time trigger-Cherenkov occupancy, one map per
    //  trigger for the dashboard's per-trigger row.
    std::unordered_map<int, RootHist<TH2F>> h_trigger_window_hitmap;
    std::unordered_map<int, std::array<RootHist<TH1F>, 2>> h_trigger_hit_multiplicity;
    std::unordered_map<int, RootHist<TH2F>> h_trigger_dt;
    //  Pairwise Δt within a frame, keyed by ((lo << 8) | hi) where lo,hi
    //  are the two trigger indices with lo < hi (unordered pair).  Δt is
    //  signed as t_hi − t_lo so the sign carries which-came-first.  The
    //  (streaming, RANSAC) pair is intentionally NOT filled — RANSAC is the
    //  second stage of streaming, so the two are not independent and
    //  their Δt is dominated by the median delay, not physics.
    //  FirstFrames / StartOfSpill are excluded on both legs.
    std::unordered_map<uint16_t, RootHist<TH1F>> h_trigger_pair_dt;
    //  Per-trigger anchor-Δt vs spill (cc-domain).  Y = wrap(c_hit −
    //  c_trigger) in cc — wrapped at fill-time around ±rollover/2 so
    //  rollover-straddling frames (where the trigger and hit happen
    //  to live on opposite sides of a coarse-counter rollover within
    //  the same physical frame) get their true small Δt instead of
    //  a spurious ±rollover value.  Lazy-allocated per trigger that
    //  fires ≥ 1 time.  Variable-bin Y axis (705 bins) via the
    //  shared ``make_anchor_dt_y_edges`` helper — main pad gets
    //  1-cc resolution, the rollover zoom pads get 1-cc resolution,
    //  and the off-canvas region between them collapses into a
    //  single huge bin.  Rendered as a 3-pad zoom PDF via
    //  ``render_anchor_dt_canvas``.
    std::unordered_map<int, RootHist<TH2F>> h_trigger_anchor_dt;
    //  Y-edge array reused across all per-trigger histograms — built
    //  once here so the lazy ``h_trigger_anchor_dt[idx] =`` allocation
    //  inside the spill loop just hands over its data() pointer.
    const auto kAnchorYEdges =
        util::qa::make_anchor_dt_y_edges(BTANA_ALCOR_ROLLOVER_TO_CC);
    //  Diagnostic counter — how many (hit, trigger) pairs were
    //  rollover-straddling and needed the wrap.  Logged at the end
    //  so operators can see how often the rollover crossing happens
    //  in their run (expected ≈ frame_size / rollover ≈ 3 % of pairs).
    long long n_anchor_dt_rollover_wrap = 0;

    //  Per-trigger Δt(hit − trigger) vs ToT (mode-gated, ToT-family only).
    //  X = fine-time Δt of each paired Cherenkov hit relative to the trigger
    //  [ns]; Y = that hit's ToT (= t_secondary − t_primary) [ns].  Shows how
    //  the per-hit charge (ToT) is distributed across arrival time relative to
    //  the trigger — the prompt Cherenkov photons cluster near Δt ≈ 0 with
    //  their characteristic single-p.e. ToT, while pile-up afterpulses (the
    //  untagged "fake two-photon" hits) populate inflated-ToT bands.  Lazy-
    //  allocated per fired trigger, but only when `tot_qa` (LET runs leave the
    //  map empty → no PDF, dashboard tile auto-drops).  Y binning matches
    //  h_tot_spectrum (3.125 ns = 1 coarse count, half-integer-cc edges).
    std::unordered_map<int, RootHist<TH2F>> h_trigger_dt_vs_tot;
    //  X window [ns] for the Δt-vs-ToT plot: a touch before the trigger
    //  (−20 ns covers readout jitter on the prompt peak) out to +200 ns,
    //  matching the ToT Y range so the prompt peak and the afterpulse /
    //  pile-up tail both sit on screen.  1 ns bins resolve the few-ns prompt
    //  peak.  Fixed diagnostic axis (like the afterpulse hitmaps' −99..99) —
    //  not a physics tunable.
    constexpr double kDtVsTotXLo = -20.0;
    constexpr double kDtVsTotXHi = 200.0;
    constexpr int kDtVsTotNBinsX = 220;

    //  In-beam QA-sample accounting: how many hardware triggers were
    //  eligible for an in-beam pre-trigger sample vs how many were dropped
    //  because the band ran off the front of the frame.  Surfaced on the
    //  streaming-score plot so the operator sees the missed fraction.
    long long n_inbeam_triggers_total = 0;
    long long n_inbeam_triggers_skipped = 0;

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

    //  C6 addendum: track whether the run actually saw any
    //  decoded timing data so we can skip the Timing/ directory + the
    //  timing_alignment canvas when it'd be empty.  Today an empty
    //  card on the dashboard reads as a broken run; better to omit it
    //  altogether and emit one INFO line saying why.
    //
    //  Note on ALCOR `tracking`: that role is 2024-only legacy (the
    //  external CMOS/LGAD ALTAI tracker replaced it from 2025 onward).
    //  No dedicated tracking-QA hists live in this writer; the
    //  per-trigger QA cards (h_trigger_*_tracking) are LAZILY created
    //  on first encounter of a `tracking` trigger, so if no such
    //  trigger fires they self-skip — no separate guard needed here.
    bool timing_data_seen = false;
    //  ---
    //  Per-channel QA axis: one bin per Cherenkov channel ordinal.  The
    //  ordinal packs `kChansPerCherenkovDevice` channels per device across
    //  the Cherenkov device span [kFirstDevice, kTimingDeviceLo) — derived
    //  here from the gidx device bounds rather than a bare literal so it
    //  tracks the split-in-two → flat layout flip automatically.
    constexpr int kChansPerCherenkovDevice = gidx::kUsesSplitInTwo ? 256 : 512;
    constexpr int kCherenkovDeviceCount = gidx::kTimingDeviceLo - gidx::kFirstDevice;
    constexpr int kMaxCherenkovChannelOrdinal = kCherenkovDeviceCount * kChansPerCherenkovDevice;
    //  --- DCR
    RootHist<TProfile> h_dcr_per_channel("h_dcr_per_channel", ";channel;DCR [kHz];", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal);
    //  --- Timing-sensor DCR (device 200, chips 0/1).  Count noise-window
    //  (first-frames) hits per timing channel; rate = hits / (n_noise_frames ×
    //  frame_length).  Axis: chip0 → bins 0-31, chip1 → bins 32-63 (eo_channel
    //  within chip).  Finalised + averaged after the spill loop.
    RootHist<TH1F> h_timing_dcr_per_channel(
        "h_timing_dcr_per_channel",
        "timing-sensor DCR;timing channel (chip0:0-31 | chip1:32-63);DCR [kHz]",
        64, 0, 64);
    std::array<long, 64> timing_dcr_counts{};
    long n_timing_noise_frames = 0;
    double timing_dcr_mean_khz = 0.0;  // total average over all timing channels
    double timing_dcr_chip0_khz = 0.0; // chip-0 average (bins 0-31)
    double timing_dcr_chip1_khz = 0.0; // chip-1 average (bins 32-63)
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
                                                     ";channel;Near-window same-channel probability (%);", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal);
    RootHist<TProfile> h_afterpulse_far_per_channel("h_afterpulse_far_per_channel",
                                                    ";channel;Far-window same-channel probability (%);", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal);
    RootHist<TProfile> h_afterpulse_per_channel("h_afterpulse_per_channel",
                                                ";channel;Afterpulse probability (DCR-subtracted) (%);", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal);
    //  --- ToT QA (mode-gated): only filled / written / rendered for ToT-family
    //  runs.  Declared unconditionally (cheap); wired into the QA bundle and
    //  emitted only when `tot_qa` is true, so LET runs produce no ToT artefacts
    //  and the dashboard's ToT overview tiles auto-drop.
    const bool tot_qa = alcor_mode_pairs_edges(to_alcor_op_mode(op_mode));
    //  3.125 ns bins (= 1 coarse count) on half-integer-cc edges — see h_tot_vs_channel.
    RootHist<TH1F> h_tot_spectrum("h_tot_spectrum",
                                  "ToT spectrum;ToT = t_{secondary} - t_{primary} [ns];hits", 65, -1.5625, 201.5625);
    //  Y: 3.125 ns bins (= 1 coarse count) with edges on half-integer cc so each
    //  integer-cc duration sits at a bin centre — the coarse-only (no fine-calib)
    //  duration then doesn't smear across bin edges.  Range 0–200 ns captures the
    //  pile-up tail.
    RootHist<TH2F> h_tot_vs_channel("h_tot_vs_channel",
                                    ";channel;ToT [ns];", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal, 65, -1.5625, 201.5625);
    RootHist<TProfile> h_tot_secondary_orphan_per_channel("h_tot_secondary_orphan_per_channel",
                                                          ";channel;secondary-orphan (missing stop) probability (%);", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal);
    RootHist<TProfile> h_tot_leading_orphan_per_channel("h_tot_leading_orphan_per_channel",
                                                        ";channel;leading-orphan (missing start) probability (%);", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal);
    //  Per-sensor ToT spectra: split by each hit's sensor via the readout config's
    //  sensor_for() (e.g. 1350 / 1375).  Raw TH1F in a map (not RootHist) keeps the
    //  per-sensor set cleanly; written + rendered (with per-peak fit) only for ToT.
    std::map<std::string, std::unique_ptr<TH1F>> tot_spectrum_by_sensor;
    std::unordered_map<int, TH1F *> tot_spectrum_by_device;
    if (tot_qa)
        for (const auto &rc : framer.get_readout_config().all())
            for (const auto &dc : rc.device_chip)
            {
                const std::string sen = rc.sensor_for(dc.first);
                if (sen.empty())
                    continue;
                auto &slot = tot_spectrum_by_sensor[sen];
                if (!slot)
                {
                    slot = std::make_unique<TH1F>(
                        ("h_tot_spectrum_" + sen).c_str(),
                        ("ToT spectrum (" + sen + ");ToT = t_{secondary} - t_{primary} [ns];hits").c_str(),
                        65, -1.5625, 201.5625);
                    slot->SetDirectory(nullptr);
                }
                tot_spectrum_by_device[dc.first] = slot.get();
            }
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
    h_afterpulse_hitmap->Sumw2(); // signed-weight fills → needs squared-weight tracking
    //  --- Cross-talk per-channel profiles
    RootHist<TProfile> h_phys_ct_per_channel("h_phys_ct_per_channel",
                                             ";channel;Physical CT probability (%);", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal);
    RootHist<TProfile> h_elec_ct_per_channel("h_elec_ct_per_channel",
                                             ";channel;Electrical CT probability (%);", kMaxCherenkovChannelOrdinal, 0, kMaxCherenkovChannelOrdinal);
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
    //  The Cherenkov-hit hitmaps cover the score → RANSAC pipeline:
    //    full_hitmap     hits flagged by stage 1 (score's cluster mask)
    //    time_cut_hitmap hits surviving |t − t_streaming| < time_window_ns
    //    ring_finder_*   hits the RANSAC tagged as belonging to a ring
    RootHist<TH2F> h_streaming_trigger_full_hitmap("h_streaming_trigger_full_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_streaming_trigger_time_cut_hitmap("h_streaming_trigger_time_cut_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_streaming_trigger_ring_finder_hitmap("h_streaming_trigger_ring_finder_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH1F> h_streaming_trigger_ring_finder_nrings("h_streaming_trigger_ring_finder_nrings", ";timing chip 0 - timing chip 1", 3, -.5, 2.5);
    RootHist<TH2F> h_streaming_trigger_ring_finder_first_hitmap("h_streaming_trigger_ring_finder_first_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_streaming_trigger_ring_finder_second_hitmap("h_streaming_trigger_ring_finder_second_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    //  Per-ring centre / radius hists.  Binning AND limits are derived
    //  from streaming_ransac_cfg so a TOML edit propagates straight
    //  through — one QA bin ↔ one RANSAC accumulator cell.
    //
    //    R   : range = [cfg.r_min, cfg.r_max],   bin width = cfg.r_step
    //    X/Y : range = [-cfg.centre_xy_half_range_mm,
    //                   +cfg.centre_xy_half_range_mm],
    //          bin width = cfg.cell_size   (rounded so n_bins × cell_size
    //          covers the full half-range symmetrically around 0)
    //
    //  Note: `centre_xy_half_range_mm` is a writer-side QA knob, not a
    //  ring-finder bound — the RANSAC's X/Y space is set by the active
    //  geometry, not by a user limit.  Set it wider than the expected
    //  centre spread for your beam line so legitimate rings don't fall
    //  in overflow; tighter to zoom in once you know where they land.
    const int ringXY_nbins =
        std::max(1, static_cast<int>(std::round(
                        2.f * streaming_ransac_cfg.centre_xy_half_range_mm /
                        streaming_ransac_cfg.cell_size)));
    const float ringXY_half = 0.5f * ringXY_nbins * streaming_ransac_cfg.cell_size;
    const int ringR_nbins =
        std::max(1, static_cast<int>(std::round(
                        (streaming_ransac_cfg.r_max - streaming_ransac_cfg.r_min) /
                        streaming_ransac_cfg.r_step)));
    const float ringR_lo = streaming_ransac_cfg.r_min;
    const float ringR_hi = streaming_ransac_cfg.r_min +
                           ringR_nbins * streaming_ransac_cfg.r_step;
    //  Per-ring **RANSAC peak** outputs (centre X, Y, radius taken
    //  straight from `RingResult::{cx, cy, radius}` before the
    //  fit_circle refinement).  Written first in the output (see the
    //  `RANSAC rings/` subfolder below) so the upstream → downstream
    //  chain is visually obvious.
    RootHist<TH1F> h_streaming_trigger_ring_X_first_ransac("h_streaming_trigger_ring_X_first_ransac", ";x (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_Y_first_ransac("h_streaming_trigger_ring_Y_first_ransac", ";y (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_R_first_ransac("h_streaming_trigger_ring_R_first_ransac", ";R (mm)", ringR_nbins, ringR_lo, ringR_hi);
    RootHist<TH1F> h_streaming_trigger_ring_X_second_ransac("h_streaming_trigger_ring_X_second_ransac", ";x (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_Y_second_ransac("h_streaming_trigger_ring_Y_second_ransac", ";y (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_R_second_ransac("h_streaming_trigger_ring_R_second_ransac", ";R (mm)", ringR_nbins, ringR_lo, ringR_hi);
    //  Per-ring `fit_circle` outputs intentionally absent here: the
    //  lightdata-side fit was QA-only and `recodata_writer` re-fits
    //  the mask-tagged hits with full LOO + dual/solo splits + CB+pol3
    //  radial fit.  All fit-derived observables live in
    //  recodata.root's `Rings/` subfolder.

    //  RANSAC-knob calibration QA (see § 2.5 in the streaming DISCUSSION).
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
    //  Bin width = collection_radius / 30  (~0.067 mm at the default
    //  2 mm collection_radius).
    constexpr int kArcDistBinsPerSide = 30;
    const int ringArc_nbins = 2 * kArcDistBinsPerSide;
    const float ringArc_hi = 2.f * streaming_ransac_cfg.collection_radius;
    RootHist<TH1F> h_streaming_trigger_ring_hit_arc_dist_first("h_streaming_trigger_ring_hit_arc_dist_first", ";|r_{hit} - R_{ring}| (mm)", ringArc_nbins, 0.f, ringArc_hi);
    RootHist<TH1F> h_streaming_trigger_ring_hit_arc_dist_second("h_streaming_trigger_ring_hit_arc_dist_second", ";|r_{hit} - R_{ring}| (mm)", ringArc_nbins, 0.f, ringArc_hi);

    //  Dual-ring sample QA — mirrors of the first-ring hists above
    //  restricted to events where a second ring was *also* found.
    //  Lets you A/B the first ring between the full sample and the
    //  cleaner 2-ring subset (interpretation in `StreamingRansacQA`).
    //  Same binning / axes as the unsuffixed twins for direct overlay.
    RootHist<TH2F> h_streaming_trigger_ring_finder_first_hitmap_dual("h_streaming_trigger_ring_finder_first_hitmap_dual", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH1F> h_streaming_trigger_ring_X_first_ransac_dual("h_streaming_trigger_ring_X_first_ransac_dual", ";x (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_Y_first_ransac_dual("h_streaming_trigger_ring_Y_first_ransac_dual", ";y (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_R_first_ransac_dual("h_streaming_trigger_ring_R_first_ransac_dual", ";R (mm)", ringR_nbins, ringR_lo, ringR_hi);
    RootHist<TH2F> h_streaming_trigger_ring_peak_votes_vs_active_first_dual("h_streaming_trigger_ring_peak_votes_vs_active_first_dual", ";|active| hits;peak votes", 100, 0, 100, 50, 0, 50);
    RootHist<TH1F> h_streaming_trigger_ring_hit_arc_dist_first_dual("h_streaming_trigger_ring_hit_arc_dist_first_dual", ";|r_{hit} - R_{ring}| (mm)", ringArc_nbins, 0.f, ringArc_hi);

    //  Solo-ring sample QA — complement of the _dual set: same first-ring
    //  hists restricted to events where *no* second ring was found.
    //  Together (_solo + _dual) they partition the first-ring sample.
    RootHist<TH2F> h_streaming_trigger_ring_finder_first_hitmap_solo("h_streaming_trigger_ring_finder_first_hitmap_solo", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH1F> h_streaming_trigger_ring_X_first_ransac_solo("h_streaming_trigger_ring_X_first_ransac_solo", ";x (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_Y_first_ransac_solo("h_streaming_trigger_ring_Y_first_ransac_solo", ";y (mm)", ringXY_nbins, -ringXY_half, ringXY_half);
    RootHist<TH1F> h_streaming_trigger_ring_R_first_ransac_solo("h_streaming_trigger_ring_R_first_ransac_solo", ";R (mm)", ringR_nbins, ringR_lo, ringR_hi);
    RootHist<TH2F> h_streaming_trigger_ring_peak_votes_vs_active_first_solo("h_streaming_trigger_ring_peak_votes_vs_active_first_solo", ";|active| hits;peak votes", 100, 0, 100, 50, 0, 50);
    RootHist<TH1F> h_streaming_trigger_ring_hit_arc_dist_first_solo("h_streaming_trigger_ring_hit_arc_dist_first_solo", ";|r_{hit} - R_{ring}| (mm)", ringArc_nbins, 0.f, ringArc_hi);
    const float time_window_ns = streaming_trigger_cfg.time_window_ns;
    //  QA score histograms.  Always filled — the noise hist accumulates
    //  during the first-frames window of every spill, the data hist during
    //  the rest of the spill.  Same n_σ axis so the misfire and acceptance
    //  integrals at any threshold are directly comparable.  See
    //  include/triggers/DISCUSSION.md § 2.3.  Binning history: 500 → 250
    //  (Q2/2026 first pass) → 125 bins / [0,50] (0.4 n_σ/bin) → 200 bins /
    //  [0,1000] (1 n_σ/bin, linear) → 200 LOG-spaced bins / [0.1,1000]
    //  (equal-width on the log-x axis the canvas draws on).  The score
    //  distributions are log-normal in n_σ (Gaussian in log10), so
    //  log-spaced bins sample the bump and its heavy right tail evenly
    //  and keep the per-bin probabilities comparable across all three
    //  decades the log-Gaussian overlay fit lives on.
    //  Both hists are filled with the SAME standardised n_σ
    //  (`n_sigma = (S - E[S]) / σ_S`, see score.cxx::run_streaming_
    //  trigger_weighted) computed against the noise-built weight
    //  bundle — so the X-axis variable is identical for both
    //  samples; the per-sample parenthetical that used to appear in
    //  these titles was misleading on the overlay canvas (only one
    //  title can render).  Per-sample identification lives in the
    //  legend.
    //  Log-spaced edges: 200 bins over [0.1, 1000] — FOUR decades
    //  (log10: −1 → 3), each equal-width in log10 so the bins render
    //  uniform on the canvas's log-x axis.  Shared by all three score
    //  hists.  (The span factor is 4.0, not 3.0: 0.1→1000 is four
    //  decades; a 3.0 factor capped the top edge at 100 and the axis
    //  rendered [0.1,100] instead of the requested [0.1,1000].)
    double kScoreEdges[201];
    for (int i = 0; i <= 200; ++i)
        kScoreEdges[i] = 0.1 * std::pow(10.0, 4.0 * i / 200.0);
    RootHist<TH1F> h_streaming_score_noise(
        "h_streaming_score_noise",
        ";n_{#sigma};probability per bin",
        200, kScoreEdges);
    RootHist<TH1F> h_streaming_score_data(
        "h_streaming_score_data",
        ";n_{#sigma};probability per bin",
        200, kScoreEdges);
    //  Third score sample — the in-beam background.  Scored on a
    //  fixed window positioned just before each hardware trigger
    //  (see the fill site below): in-beam-but-pre-signal activity,
    //  distinct from the first-frames DCR (noise) and the
    //  post-first-frames data samples.  Lets the analyser read the
    //  in-beam baseline separately when picking the production cut.
    RootHist<TH1F> h_streaming_score_inbeam(
        "h_streaming_score_inbeam",
        ";n_{#sigma};probability per bin",
        200, kScoreEdges);
    //  Colour scheme for the overlay canvas written further down:
    //  DCR / noise (first-frames) = blue, signal / data-taking = red,
    //  in-beam bkg (pre-trigger) = violet.  Set at hist creation so the
    //  colours stick whether you draw the individual hists or the overlay.
    h_streaming_score_noise->SetLineColor(kBlue);
    h_streaming_score_data->SetLineColor(kRed);
    h_streaming_score_inbeam->SetLineColor(kViolet);
    h_streaming_score_noise->SetLineWidth(2);
    h_streaming_score_data->SetLineWidth(2);
    h_streaming_score_inbeam->SetLineWidth(2);
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
    //  No accumulator/LUT to pre-build: the ring finder is the grid-free
    //  RANSAC (mist::ring_finding::find_rings_ransac), which works directly on
    //  the per-frame hit (x, y) positions inside run_streaming_ransac_trigger.

    //  ---
    //  Loop over spills
    if (max_spill != 1000)
        mist::logger::info("(ParallelStreamingFramer::next_spill) Requested to stop at spill : " +
                           std::to_string(max_spill));

    // ── Streaming-trigger weights ─────────────────────────────────
    // The bundle persists across spills (cumulative DCR via h_dcr_per_channel
    // → newer spills supersede older builds, but a freshly-rebuilt-empty
    // bundle would lose spill 0's data when spill 1's noise frames fire).
    // The rebuild itself happens once per spill, at the noise → data
    // boundary inside the per-frame loop — see the `built_for_spill` flag
    // reset at the start of each spill and the in-loop rebuild block.
    StreamingTriggerWeights streaming_weights;

    //  ── Readout-resilience accumulators (lane_failure_rate) ──────────────
    //  Detector-wide dead-lane fraction = (dead cherenkov lane-spills) /
    //  (participant cherenkov lane-spills).  A lane is a "participant" in a
    //  spill if it sent a start-of-spill marker; "dead" if that marker carried
    //  a non-zero coarse time.  Exposure-normalised (denominator = participant
    //  lane × spill), so lanes that come online / drop out across the fill are
    //  handled without a fixed lane count.  Timing device(s) excluded.
    long long dead_lane_spills = 0;
    long long participant_lane_spills = 0;

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

        //  --- Per-spill online calibration update ---
        //
        //  The canonical calibration path is the offline pass via the
        //  fine_calibration_timing.cpp macro.  When the QA toggle
        //  `per_spill_calibration_update` is set (typically in
        //  `conf/QA/framer_conf.toml`), this call seeds each spill's
        //  calibration table from the channels that became active during the
        //  previous spill — useful for an online-only mode where no offline
        //  calibration is available.  Default-off so production runs are
        //  unaffected.
        if (qa_cfg.per_spill_calibration_update)
            spilldata.update_calibration(framer.get_fine_tune_distribution());

        //  Calculate participants channel
        // The "active sensors" set is keyed by GlobalIndex::channel_ordinal().
        // The lookup at line ~1044 below (DCR-QA per-channel count map) uses
        // the same expression, keeping the set ↔ count-map keys in sync.
        std::set<uint32_t> active_sensors;
        std::unordered_map<uint32_t, uint16_t> active_sensors_count;
        auto lanes_participating = spilldata.get_not_dead_participants();
        int n_active_cherenkov_channels = 0;
        for (auto [device, lanes] : lanes_participating)
            if (device < ::gidx::kTimingDeviceLo)
                for (auto current_lane : lanes)
                    for (auto i_channel = 0; i_channel < 8; ++i_channel)
                    {
                        // Construct the GlobalIndex directly from the hardware
                        // identifiers; apply the split-in-two trick at the
                        // conversion boundary.
                        const int chip_raw = current_lane / 4;
                        const int channel_raw = 8 * (current_lane % 4) + i_channel;
                        const int chip_logical = ::gidx::kUsesSplitInTwo
                                                     ? chip_raw / 2
                                                     : chip_raw;
                        const int channel_log = ::gidx::kUsesSplitInTwo
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

        //  ── Readout-resilience tally ─────────────────────────────────────
        //  Count cherenkov (device < timing) dead vs participant lanes this
        //  spill.  ``lanes_participating`` holds the alive lanes; participants
        //  = alive + dead, so the denominator below is the full reported set.
        {
            auto lanes_dead = spilldata.get_dead_participants();
            long long alive_cher = 0, dead_cher = 0;
            for (const auto &[device, lanes] : lanes_participating)
                if (device < ::gidx::kTimingDeviceLo)
                    alive_cher += static_cast<long long>(lanes.size());
            for (const auto &[device, lanes] : lanes_dead)
                if (device < ::gidx::kTimingDeviceLo)
                    dead_cher += static_cast<long long>(lanes.size());
            dead_lane_spills += dead_cher;
            participant_lane_spills += alive_cher + dead_cher;
        }

        //  Streaming-trigger weights
        //  The bundle itself is run-scope (declared above the spill loop),
        //  so spill N's noise frames see spill N-1's already-built weights.
        //  We only reset the per-spill "have we rebuilt yet" flag here;
        //  the actual rebuild happens once, at the noise → data boundary
        //  inside the per-frame loop.  Rebuilding per spill (rather than
        //  once per run) tracks channels that come online / drop out across
        //  spills (e.g. an RDO that was off in spill 0 starts contributing
        //  from spill 1) and channels whose rates drift over the fill.
        bool streaming_weights_built_for_spill = false;
        //  (The RANSAC `min_active` / DCR-adaptive floor is gone — the RANSAC
        //  finder gates on inlier significance, not an accumulator vote floor.)

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
            //  Iterate in ascending frame_id order — the math here is per-channel
            //  accumulation (order-independent in principle), but determinism across
            //  runs requires a stable iteration order over the (unordered_map)
            //  frame_link.  Sort once and reuse.
            const auto calib_sorted_keys = sorted_frame_ids(spilldata.get_frame_link());
            for (uint32_t frame_id : calib_sorted_keys)
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
                    seen_channels_0.size() == static_cast<size_t>(timing_chip0_alive))
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
                    seen_channels_1.size() == static_cast<size_t>(timing_chip1_alive))
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

            //  TODO: dynamically determine the timing cuts — pick the
            //  most-populated non-zero bin of the per-TDC delta-time
            //  distribution and use its position to centre the
            //  acceptance window.  Tracked in
            //  include/writers/DISCUSSION.md.
        }

        mist::logger::info("(lightdata_writer) Starting processing data streams in frames");
        const auto total_frames = static_cast<int>(spilldata.get_frame_link().size());
        // Last global clock cycle seen per trigger index — reset each spill so
        // the last trigger of one spill never contributes a delta with the next.
        std::unordered_map<int, uint64_t> trigger_last_global_cc;

        //  Per-frame CT scratch buffers — hoisted out of the inner loop
        //   Before this change each frame allocated a
        //  fresh std::vector<CtHit> + std::vector<std::size_t>, even though
        //  the capacities settle quickly after the first few frames.  Now we
        //  reuse the same storage across frames within a spill and .clear()
        //  at the top of each frame: typical capacity stabilises within a
        //  spill, eliminating the realloc churn on the hot path.
        //  `CtHit` is defined in `include/writers/lightdata/types.h` so
        //  the per-frame QA helper (`fill_dcr_afterpulse_ct_qa`) can
        //  use the same record type without dragging the writer's
        //  internals into its signature.
        using ::btana::lightdata::CtHit;
        std::vector<CtHit> ct_hits;
        std::vector<std::size_t> sorted_by_time;

        //  Iterate in ascending frame_id order.  CRITICAL for two state
        //  carriers built up across this loop:
        //   - `trigger_last_global_cc[index]`: bookkeeping for the
        //     consecutive-Δt fills into h_trigger_dt_*.  Out-of-order frame
        //     ids would yield negative Δts and saturate the log-binned axis.
        //   - `carry_over_hits`: per-frame state passed forward through
        //     `run_streaming_trigger_weighted` (frame N's tail informs frame
        //     N+1's window).  Hash-map iteration order would scramble that.
        //  The underlying frame_link is an unordered_map; build the sorted
        //  key vector once per spill (O(N log N) over a few thousand frames,
        //  negligible) and iterate it.
        const auto main_sorted_keys = sorted_frame_ids(spilldata.get_frame_link());
        //  C6.1: local counter for progress-bar throttling.  Using
        //  `frame_id % 100000` was a sparse-modulo bug: framer frame_ids
        //  carry gaps (skipped frames) and aren't dense multiples of any
        //  fixed period, so the throttle fired arbitrarily — sometimes
        //  on every frame (when many ids landed on 100k boundaries),
        //  sometimes never.  Local counter gives true 1-in-100k cadence
        //  and also matches the bar's "processed/total" semantic.
        std::size_t postproc_progress = 0;

        //  C6.3: hoist StreamingRansacQA construction above the per-frame
        //  loop.  Every field points at one of the h_streaming_trigger_*
        //  RootHist<T>'s declared at function scope; their `.get()`
        //  returns a stable raw pointer, so re-constructing the bundle
        //  per saved frame paid 24 redundant assignments × N frames.
        //  Build once, pass the same object every iteration.
        StreamingRansacQA hough_qa;
        hough_qa.full_hitmap = h_streaming_trigger_full_hitmap.get();
        hough_qa.time_cut_hitmap = h_streaming_trigger_time_cut_hitmap.get();
        hough_qa.nrings = h_streaming_trigger_ring_finder_nrings.get();
        hough_qa.ring_finder_hitmap = h_streaming_trigger_ring_finder_hitmap.get();
        hough_qa.first_hitmap = h_streaming_trigger_ring_finder_first_hitmap.get();
        hough_qa.second_hitmap = h_streaming_trigger_ring_finder_second_hitmap.get();
        //  RANSAC-seed QA assignments only; the per-ring fit
        //  belongs to recodata_writer (see lines ~430 above).
        hough_qa.ring_X_first_ransac = h_streaming_trigger_ring_X_first_ransac.get();
        hough_qa.ring_Y_first_ransac = h_streaming_trigger_ring_Y_first_ransac.get();
        hough_qa.ring_R_first_ransac = h_streaming_trigger_ring_R_first_ransac.get();
        hough_qa.ring_X_second_ransac = h_streaming_trigger_ring_X_second_ransac.get();
        hough_qa.ring_Y_second_ransac = h_streaming_trigger_ring_Y_second_ransac.get();
        hough_qa.ring_R_second_ransac = h_streaming_trigger_ring_R_second_ransac.get();
        hough_qa.ring_peak_votes_vs_active_first = h_streaming_trigger_ring_peak_votes_vs_active_first.get();
        hough_qa.ring_peak_votes_vs_active_second = h_streaming_trigger_ring_peak_votes_vs_active_second.get();
        hough_qa.ring_hit_arc_dist_first = h_streaming_trigger_ring_hit_arc_dist_first.get();
        hough_qa.ring_hit_arc_dist_second = h_streaming_trigger_ring_hit_arc_dist_second.get();
        //  Dual-ring mirror — gated inside the trigger on found_rings.size() > 1.
        hough_qa.first_hitmap_dual = h_streaming_trigger_ring_finder_first_hitmap_dual.get();
        hough_qa.ring_X_first_ransac_dual = h_streaming_trigger_ring_X_first_ransac_dual.get();
        hough_qa.ring_Y_first_ransac_dual = h_streaming_trigger_ring_Y_first_ransac_dual.get();
        hough_qa.ring_R_first_ransac_dual = h_streaming_trigger_ring_R_first_ransac_dual.get();
        hough_qa.ring_peak_votes_vs_active_first_dual = h_streaming_trigger_ring_peak_votes_vs_active_first_dual.get();
        hough_qa.ring_hit_arc_dist_first_dual = h_streaming_trigger_ring_hit_arc_dist_first_dual.get();
        //  Solo-ring mirror — gated inside the trigger on found_rings.size() == 1.
        hough_qa.first_hitmap_solo = h_streaming_trigger_ring_finder_first_hitmap_solo.get();
        hough_qa.ring_X_first_ransac_solo = h_streaming_trigger_ring_X_first_ransac_solo.get();
        hough_qa.ring_Y_first_ransac_solo = h_streaming_trigger_ring_Y_first_ransac_solo.get();
        hough_qa.ring_R_first_ransac_solo = h_streaming_trigger_ring_R_first_ransac_solo.get();
        hough_qa.ring_peak_votes_vs_active_first_solo = h_streaming_trigger_ring_peak_votes_vs_active_first_solo.get();
        hough_qa.ring_hit_arc_dist_first_solo = h_streaming_trigger_ring_hit_arc_dist_first_solo.get();

        for (uint32_t frame_id : main_sorted_keys)
        {
            //  Update post-processing subtask bar periodically to avoid render overhead
            if (postproc_progress % 100000 == 0)
                progress_postprocessing.update(
                    static_cast<int>(postproc_progress), total_frames);
            ++postproc_progress;

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

            //  Timing-sensor DCR: this frame is part of the noise sample
            //  (first-frames window) → its timing hits are dark counts.
            const bool timing_dcr_noise_frame =
                (static_cast<int>(frame_id) < framer_cfg.first_frames_trigger);
            if (timing_dcr_noise_frame)
                ++n_timing_noise_frames;

            //  Loop over timing hits
            for (const auto &raw_hit : timing_hits)
            {
                AlcorFinedata Hit(raw_hit);
                const int chip = Hit.get_chip();
                const int channel = Hit.get_global_channel_index();
                const float time_ns = Hit.get_time_ns();

                //  Count noise-window hits per timing channel for the DCR.
                if (timing_dcr_noise_frame)
                {
                    //  get_eo_channel() spans a chip PAIR (0-31 even chip,
                    //  32-63 odd chip), so reduce to the within-chip channel
                    //  (0-31) first, then place the first timing chip in
                    //  bins 0-31 and the second in 32-63.  This works whether
                    //  the two timing chips are an even/odd pair (2026: 0,1)
                    //  or both even (2025: 0,2 → both eo 0-31).
                    const int ch_in_chip = Hit.get_eo_channel() % 32;
                    const int loc =
                        (chip == timing_chip_0_id)   ? ch_in_chip
                        : (chip == timing_chip_1_id) ? 32 + ch_in_chip
                                                     : -1;
                    if (loc >= 0 && loc < 64)
                        ++timing_dcr_counts[static_cast<size_t>(loc)];
                }

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

            //  Occupancy map + "timing seen" flag publish for ANY timing
            //  activity.  The map is the per-frame (n_chip0, n_chip1) pair
            //  distribution — NOT the strict alive-channel trigger
            //  coincidence — so a frame that fired only one chip is a real
            //  point on that chip's axis and must be shown.  Gating this on
            //  both chips hid every single-chip frame, leaving a live-but-
            //  uncorrelated run (chips never coincident) with an empty map
            //  and a skipped Timing/ directory.  A truly absent / powered-off
            //  timing detector still fires neither chip, so the empty-card
            //  guard (timing_data_seen) is preserved.
            if (timing_hits_0 > 0 || timing_hits_1 > 0)
            {
                timing_data_seen = true;
                h_timing_hit_map->Fill(timing_hits_0, timing_hits_1);
            }

            //  Δt reference + strict (alive-channel) coincidence need BOTH
            //  chips: mean0/mean1 divide by the per-chip counts, and the
            //  timing trigger is defined by the chip0–chip1 time delta.
            if (timing_hits_0 > 0 && timing_hits_1 > 0)
            {
                const float mean0 = timing_sum_0 / timing_hits_0;
                const float mean1 = timing_sum_1 / timing_hits_1;
                const float ref_timing = (mean0 + mean1) / 2.f;
                const float delta_timing = mean1 - mean0;

                h_timing_ref_delta->Fill(delta_timing);

                const bool timing_available =
                    (timing_hits_0 == timing_chip0_alive) &&
                    (timing_hits_1 == timing_chip1_alive) &&
                    (std::fabs(delta_timing - timing_delta_center) < timing_delta_nsigma * timing_delta_window);

                if (timing_available)
                {
                    h_timing_ref_delta_sel->Fill(delta_timing);
                    spilldata.add_trigger_to_frame(frame_id, {static_cast<uint8_t>(TriggerTiming),
                                                              static_cast<uint16_t>(framer_cfg.frame_size / 2),
                                                              ref_timing});
                }
            }

            //  --- Cherenkov sliding window trigger
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
                //  In-beam sideband baseline.  Anchor on every "real"
                //  hardware trigger fired so far in the spill (FirstFrames
                //  and StartOfSpill are synthetic markers; streaming /
                //  RANSAC triggers don't exist yet at this point — the
                //  score loop below is what emits them — so the exclusion
                //  list mostly guards against future re-call paths).
                //  Window [-300 ns, -50 ns] is 250 ns wide with a 50 ns
                //  guard band against the trigger edge, on the LEFT side
                //  only because the right side has the afterpulse tail.
                static const std::set<uint8_t> kInBeamExclude = {
                    TriggerFirstFrames,
                    TriggerStartOfSpill,
                    _TRIGGER_STREAMING_RING_FOUND_,
                    _TRIGGER_RANSAC_RING_FOUND_,
                };
                StreamingInBeamRates in_beam_rates =
                    compute_streaming_inbeam_rates(
                        spilldata,
                        /*sideband_lo_ns=*/-300.f,
                        /*sideband_hi_ns=*/-50.f,
                        framer_cfg.frame_length_ns(),
                        kInBeamExclude);

                streaming_weights = build_streaming_trigger_weights(
                    h_dcr_per_channel.get(),
                    streaming_trigger_cfg.time_window_ns,
                    framer_cfg.frame_length_ns(),
                    streaming_trigger_cfg.min_noise_hits,
                    &active_sensors,      // restrict to this spill's participants
                    in_beam_rates.empty() // no in-beam anchors → DCR-only
                        ? nullptr
                        : &in_beam_rates);
                //  C7.6 — surface the operator's multiplicity cap (0 =
                //  disabled, fully backwards-compatible) to the trigger
                //  hot loop via the bundle.  `build_streaming_trigger_
                //  weights` doesn't know about config (it operates on
                //  histograms + scalars), so the caller wires it.
                streaming_weights.max_hits_per_window =
                    streaming_trigger_cfg.max_hits_per_window;
                streaming_weights_built_for_spill = true;

                //  C3.3: clear carry-over from the previous bundle's
                //  running_score.  Any hits that crossed the spill
                //  boundary were weighted against the OLD E[S] / σ_S;
                //  mixing them into the new bundle's window biases the
                //  first frames of this spill (typically a >5σ outlier
                //  stripe at frame_id == first_frames_trigger).  Cheap
                //  to clear — the next call to
                //  run_streaming_trigger_weighted repopulates it.
                carry_over_hits.clear();
                //  Sanity log — confirms the active-channel filter is firing:
                //  n_modelled should equal min(N_active_this_spill, N_measured).
                //  If it equals N_measured even when some RDOs are off this
                //  spill, the filter isn't being applied.  Now also logs the
                //  in-beam baseline channel count so the operator sees
                //  whether the sideband bundle is contributing.
                mist::logger::info("(streaming_trigger) Spill " +
                                   std::to_string(ispill) +
                                   ": active=" + std::to_string(active_sensors.size()) +
                                   ", modelled=" + std::to_string(streaming_weights.n_channels_modelled) +
                                   ", in_beam_ch=" + std::to_string(in_beam_rates.size()) +
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

            //  ── In-beam background score sample ──────────────────────
            //  For each HARDWARE trigger in this frame, score a fixed
            //  window of width time_window_ns ending 50 ns before the
            //  trigger time — the in-beam-but-pre-signal activity just
            //  ahead of a real external trigger.  Fills the green curve
            //  on the score canvas, distinct from the first-frames DCR
            //  (red) and post-first-frames data (blue) samples.  Only
            //  genuine external triggers anchor the window: synthetic
            //  markers (FirstFrames / StartOfSpill / UNKNOWN) and the
            //  derived ring triggers (streaming / RANSAC) are skipped.
            {
                //  Sampling band: ends inbeam_offset before the trigger,
                //  inbeam_width wide (both config knobs), scored PER HIT with
                //  the SAME sliding window (win_w) + SAME bundle
                //  (streaming_weights) as the noise/data curves — so all three
                //  share one n_σ axis and the recommended cut applies.  Per-hit
                //  conditioning means empty windows aren't counted, so the
                //  in-beam curve sits at/above the first-frames DCR baseline as
                //  it physically must (in-beam = DCR + beam-induced ≥ DCR).
                const float inbeam_offset =
                    streaming_trigger_cfg.inbeam_pretrigger_offset_ns;
                const float inbeam_width =
                    streaming_trigger_cfg.inbeam_sample_width_ns;
                const float win_w = streaming_trigger_cfg.time_window_ns;
                for (const auto &trg : spilldata.get_frame_trigger_hits(frame_id))
                {
                    const uint8_t tidx = trg.index;
                    if (tidx == TriggerFirstFrames ||
                        tidx == TriggerStartOfSpill ||
                        tidx == static_cast<uint8_t>(_TRIGGER_UNKNOWN_) ||
                        tidx == static_cast<uint8_t>(_TRIGGER_STREAMING_RING_FOUND_) ||
                        tidx == static_cast<uint8_t>(_TRIGGER_RANSAC_RING_FOUND_))
                        continue;
                    ++n_inbeam_triggers_total;
                    const float t_hi = trg.fine_time - inbeam_offset;
                    const float t_lo = t_hi - inbeam_width;
                    if (t_lo < 0.f)
                    {
                        //  Band runs off the front of the frame — this
                        //  trigger's in-beam sample is dropped.  Counted so the
                        //  operator sees how many events are missed.
                        ++n_inbeam_triggers_skipped;
                        continue;
                    }
                    fill_window_score_samples(
                        spilldata, frame_id, streaming_weights,
                        t_lo, t_hi, win_w, h_streaming_score_inbeam.get());
                }
            }

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
                //  --- Streaming Trigger — stage 2 (RANSAC ring finder).
                //  Implementation in triggers/streaming/ransac.cxx.
                //  `hough_qa` is constructed above the per-frame loop
                //  (C6.3) — same pointers every iteration.
                run_streaming_ransac_trigger(
                    spilldata, frame_id,
                    streaming_trigger, ispill,
                    streaming_trigger_cfg.time_window_ns,
                    streaming_ransac_cfg,
                    hough_qa,
                    streaming_weights.weight_by_channel);

                //  ---
                //  --- Trigger QA
                //
                //  FirstFrames (100) is the per-frame synthetic marker
                //  emitted by the framer for every frame inside the
                //  first-frames noise window (frame_id <
                //  framer_cfg.first_frames_trigger); it is NOT a physics
                //  firing.  StartOfSpill (200) is the spill boundary
                //  marker.  Excluding both from the trigger-QA plots
                //  keeps the Δt distributions free of the fixed-cadence
                //  ridge that the markers would otherwise carve through
                //  every signal panel.
                //
                //  UNKNOWN (255) is the registry's fallback for any
                //  trigger value not enumerated in the config — it
                //  silently aggregates everything we couldn't name, so
                //  a per-trigger plot for it would be a mixture and
                //  read as noise.  STREAMING_RING_FOUND (104) is a
                //  derived trigger fired by the streaming pipeline
                //  itself rather than a hardware/external one — its
                //  per-trigger plots double-account the same physics
                //  that RANSAC_RING_FOUND already plots.  Both excluded
                //  from the per-trigger fan-out by operator request
                //  alongside the synthetic markers.  TODO(operator-
                //  review): revisit once the streaming-vs-RANSAC
                //  separation is more clearly defined.
                //  NOTE: TIMING (and TRACKING) are HARDWARE triggers and
                //  are deliberately NOT listed here — they get the full
                //  per-trigger QA treatment + in-beam-window anchoring
                //  like any external trigger.  Only the synthetic frame
                //  markers and the derived streaming-ring trigger are
                //  filtered out of the per-trigger plots.
                auto is_synthetic_marker = [](uint8_t idx)
                {
                    return idx == TriggerFirstFrames ||
                           idx == TriggerStartOfSpill ||
                           idx == static_cast<uint8_t>(_TRIGGER_UNKNOWN_) ||
                           idx == static_cast<uint8_t>(_TRIGGER_STREAMING_RING_FOUND_);
                };
                // Collect unique trigger types in this frame
                std::set<int> fired_trigger_types;
                for (auto &t : triggers_in_frame)
                    fired_trigger_types.insert(
                        registry.index_of(static_cast<TriggerNumber>(t.index)));
                // Fill matrix — each pair filled exactly once
                for (auto i : fired_trigger_types)
                    for (auto j : fired_trigger_types)
                        h2_trigger_matrix->Fill(i, j);

                //  Δt range for the Cherenkov-Δt and pair-Δt plots: bound
                //  by the frame length, so the per-frame uniform-x-uniform
                //  pair distribution is exactly a triangle on
                //  [-frame_length, +frame_length] peaking at 0.  The
                //  triangle-acceptance correction below (at write time)
                //  flattens that envelope so signal peaks read against a
                //  flat DCR background instead of a sloped one.
                const double frame_length_ns_q = framer_cfg.frame_length_ns();
                //  Loop on all triggers
                for (auto current_trigger : triggers_in_frame)
                {
                    //  Skip the synthetic markers from every per-trigger
                    //  plot below.  Pair-Δt loop (further down) does the
                    //  same check on both elements before pairing.
                    if (is_synthetic_marker(current_trigger.index))
                        continue;
                    //  Skip secondary firings (within the per-trigger
                    //  secondary window of the previous firing on the
                    //  same index).  Operator-chosen v1 default so the
                    //  per-trigger plots (frame-pop, Δt-vs-cherenkov,
                    //  anchor-Δt incl. the new ±1 rollover away-side
                    //  fills, hitmap, multiplicity) aren't double-
                    //  counted by closely-spaced re-fires of the same
                    //  hardware trigger.  TODO(operator-review): revisit
                    //  this gate — some downstream studies (e.g. burst
                    //  characterisation) might want the secondaries IN
                    //  on a dedicated overlay.  Tracked alongside the
                    //  consecutive-hit-Δt standardisation task.
                    if (current_trigger.is_secondary)
                        continue;
                    if (!h_trigger_frame_population.count(current_trigger.index))
                    {
                        h_trigger_frame_population[current_trigger.index] = RootHist<TH1F>(TString::Format("h_trigger_frame_population_%s", registry.name_of(current_trigger.index).c_str()).Data(), TString::Format(";frame number; %s;", registry.name_of(current_trigger.index).c_str()).Data(), 5e3, 0, 5e6);
                        h_trigger_time_diff_w_cherenkov[current_trigger.index] = RootHist<TH1F>(TString::Format("h_trigger_time_diff_w_cherenkov_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";#Delta_{t} (t_{Hit} - t_{trigger}) ns;Normalised entries / acceptance", 5e3, -frame_length_ns_q, frame_length_ns_q);
                        h_trigger_full_hitmap[current_trigger.index] = RootHist<TH2F>(TString::Format("h_trigger_full_hitmap_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
                        h_trigger_window_hitmap[current_trigger.index] = RootHist<TH2F>(TString::Format("h_trigger_window_hitmap_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
                        h_trigger_hit_multiplicity[current_trigger.index][0] = RootHist<TH1F>(TString::Format("h_trigger_hit_multiplicity_in_time_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";n_{Hit}; events;", 100, 0, 100);
                        h_trigger_hit_multiplicity[current_trigger.index][1] = RootHist<TH1F>(TString::Format("h_trigger_hit_multiplicity_out_of_time_%s", registry.name_of(current_trigger.index).c_str()).Data(), ";n_{Hit}; events;", 100, 0, 100);
                        //  Sweep audit: X axis ranges over
                        //  the spill indices the loop actually visits —
                        //  `for (ispill = 0; ispill < max_spill; ++ispill)`
                        //  yields spill values 0…max_spill-1.  Previously
                        //  the histograms were allocated with `max_spill + 1`
                        //  bins on `[-0.5, max_spill + 0.5]`, leaving the
                        //  rightmost bin (centered at `max_spill`) PERMANENTLY
                        //  EMPTY on every per-trigger card — the
                        //  off-by-one C5.6 already fixed pulser-side.
                        //  `max_spill` is guaranteed >= 1 here because
                        //  this lazy-create branch only runs inside the
                        //  per-frame loop, which is inside the per-spill
                        //  loop, so at least one spill has been visited.
                        //
                        //  Validation-run regression:
                        //  recotrack's `--force-upstream` cascade invokes
                        //  lightdata with the framework default `max_spill =
                        //  INT_MAX` (no cap).  Without the same bin cap the
                        //  anchor-dt histogram uses, this allocates ~2 × 10⁹
                        //  bins of TH2F → SIGBUS on histogram construction.
                        //  Shared `kTriggerDtMaxXBins = 256` ceiling makes
                        //  it match anchor-dt's behaviour; under high
                        //  max_spill the "spill" axis becomes coarser bins
                        //  spanning multiple spills each, but the writer
                        //  doesn't crash.
                        constexpr int kTriggerDtMaxXBins = 256;
                        const int n_trigger_dt_x_bins =
                            std::min(max_spill, kTriggerDtMaxXBins);
                        h_trigger_dt[current_trigger.index] = RootHist<TH2F>(
                            TString::Format("h_trigger_dt_%s", registry.name_of(current_trigger.index).c_str()).Data(),
                            //  No Z-axis title — a bare "entries" colour-bar
                            //  label carries no information the operator
                            //  needs.  X / Y titles only.
                            TString::Format(";spill index;#Delta_{t} between consecutive %s triggers (ns)",
                                            registry.name_of(current_trigger.index).c_str())
                                .Data(),
                            n_trigger_dt_x_bins, -0.5, max_spill - 0.5,
                            kTriggerDtNBinsY, trigger_dt_log_edges.data());
                        //  Anchor-Δt vs spill — variable-bin Y axis
                        //  via the shared helper (705 bins instead of
                        //  65 737, ~93× smaller).  Layout: 101 1-cc
                        //  bins per rollover zoom + one giant gap bin
                        //  between zoom and main + 501 1-cc main
                        //  bins.  X is capped at ``kAnchorMaxXBins``
                        //  so memory stays bounded regardless of
                        //  ``--max-spill``.
                        constexpr int kAnchorMaxXBins = 256;
                        const int n_anchor_x_bins =
                            std::min(max_spill, kAnchorMaxXBins);
                        //  C6.4: warn once when the cap kicks in.  At
                        //  max_spill > 256 each X bin spans multiple
                        //  spills, so the "spill" axis label is no
                        //  longer per-unit-spill; operators reading the
                        //  hist need to know.  One warn per first such
                        //  trigger keyed on `triggers_in_frame` keeps
                        //  the log quiet on long runs.
                        static thread_local bool warned_anchor_dt_bin_cap = false;
                        if (!warned_anchor_dt_bin_cap &&
                            max_spill > kAnchorMaxXBins)
                        {
                            mist::logger::warning(TString::Format(
                                                      "(lightdata_writer) h_trigger_anchor_dt_* X-axis "
                                                      "capped at %d bins (max_spill = %d).  Each X "
                                                      "bin spans %.2f spills — interpret accordingly.",
                                                      kAnchorMaxXBins, max_spill,
                                                      static_cast<double>(max_spill) / kAnchorMaxXBins)
                                                      .Data());
                            warned_anchor_dt_bin_cap = true;
                        }
                        h_trigger_anchor_dt[current_trigger.index] = RootHist<TH2F>(
                            TString::Format("h_trigger_anchor_dt_%s",
                                            registry.name_of(current_trigger.index).c_str())
                                .Data(),
                            TString::Format(
                                "#Deltat_{trg} vs spill (channel - trigger %s);"
                                "spill;#Deltat_{trg} (cc)  = c_{ch} - c_{trg}",
                                registry.name_of(current_trigger.index).c_str())
                                .Data(),
                            n_anchor_x_bins, -0.5, max_spill - 0.5,
                            static_cast<int>(kAnchorYEdges.size() - 1),
                            kAnchorYEdges.data());
                        h_trigger_anchor_dt[current_trigger.index]->SetDirectory(nullptr);
                        //  Δt-vs-ToT — booked only for ToT-family runs so LET
                        //  runs carry no ToT artefacts and the dashboard tile
                        //  auto-drops (same mode-gating as the other ToT QA).
                        if (tot_qa)
                        {
                            h_trigger_dt_vs_tot[current_trigger.index] = RootHist<TH2F>(
                                TString::Format("h_trigger_dt_vs_tot_%s",
                                                registry.name_of(current_trigger.index).c_str())
                                    .Data(),
                                TString::Format(
                                    "ToT vs #Deltat_{trg} (%s);"
                                    "#Delta_{t} (t_{Hit} - t_{trigger}) [ns];"
                                    "ToT = t_{secondary} - t_{primary} [ns]",
                                    registry.name_of(current_trigger.index).c_str())
                                    .Data(),
                                kDtVsTotNBinsX, kDtVsTotXLo, kDtVsTotXHi,
                                65, -1.5625, 201.5625);
                            h_trigger_dt_vs_tot[current_trigger.index]->SetDirectory(nullptr);
                        }
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
                    //  Centre offset [ns] of the out-of-time multiplicity
                    //  bucket relative to the in-time bucket (partner fills
                    //  hit_counter[1]).
                    constexpr double kOutOfTimeOffsetNs = 100.;
                    auto window_size = (current_trigger.index == _TRIGGER_RANSAC_RING_FOUND_) || (current_trigger.index == _TRIGGER_STREAMING_RING_FOUND_) ? time_window_ns : streaming_trigger_cfg.default_trigger_window_ns;
                    for (const auto &current_cherenkov_hit_struct : cherenkov_hits)
                    {
                        AlcorFinedata current_hit(current_cherenkov_hit_struct);
                        if (!current_hit.is_afterpulse())
                        {
                            auto current_delta_time = current_hit.get_time_ns() - current_trigger.fine_time;
                            h_trigger_time_diff_w_cherenkov[current_trigger.index]->Fill(current_delta_time);
                            //  Δt-vs-ToT (ToT-family only): only paired hits
                            //  carry a duration ≥ 0; orphans (−1) are excluded
                            //  so they don't pile a spurious band at the axis
                            //  floor.  Pile-up afterpulses (untagged, inflated
                            //  ToT) survive this non-afterpulse guard and show
                            //  up as the high-ToT population.
                            if (tot_qa)
                            {
                                const float hit_tot = current_hit.get_duration();
                                if (hit_tot >= 0.f &&
                                    h_trigger_dt_vs_tot.count(current_trigger.index))
                                    h_trigger_dt_vs_tot[current_trigger.index]->Fill(
                                        current_delta_time, hit_tot);
                            }
                            //  In-window hitmap: hits inside the config timing
                            //  cut [kTrigCherDtMin, kTrigCherDtMax] (asymmetric,
                            //  the same cut recodata uses for ring reco).
                            if (current_delta_time >= kTrigCherDtMin && current_delta_time <= kTrigCherDtMax)
                                h_trigger_window_hitmap[current_trigger.index]->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                            if (fabs(current_delta_time) < window_size)
                            {
                                hit_counter[0]++;
                                h_trigger_full_hitmap[current_trigger.index]->Fill(current_hit.get_hit_x_rnd(), current_hit.get_hit_y_rnd());
                            }
                            else if (fabs(current_delta_time + kOutOfTimeOffsetNs) < window_size)
                            {
                                hit_counter[1]++;
                            }
                            //  Anchor-Δt cc-domain fill with rollover
                            //  wrap.  Raw (c_hit − c_trigger) lands
                            //  near ±rollover whenever the frame
                            //  straddles a coarse-counter rollover
                            //  (≈ frame_size / rollover ≈ 3 % of
                            //  frames at the default 1024 cc / 32768
                            //  cc).  Wrap collapses that artifact —
                            //  every same-frame pair lands at its
                            //  physical Δt, bounded by ±frame_size.
                            //  Wrap counter logged at end so operators
                            //  see how often the rollover crossing
                            //  shows up in their run.
                            constexpr int kRollover =
                                BTANA_ALCOR_ROLLOVER_TO_CC;
                            int dc_cc =
                                static_cast<int>(current_hit.get_coarse()) - static_cast<int>(current_trigger.coarse);
                            if (dc_cc > +kRollover / 2)
                            {
                                dc_cc -= kRollover;
                                ++n_anchor_dt_rollover_wrap;
                            }
                            else if (dc_cc < -kRollover / 2)
                            {
                                dc_cc += kRollover;
                                ++n_anchor_dt_rollover_wrap;
                            }
                            h_trigger_anchor_dt[current_trigger.index]->Fill(
                                static_cast<double>(ispill),
                                static_cast<double>(dc_cc));
                        }
                    }

                    //  ── Away-side fills: ±1 rollover lookup ─────────────────────
                    //
                    //  The anchor-Δt 2D hist's Y-axis already reserves
                    //  ±rollover±50 cc zoom pads (see
                    //  ``make_anchor_dt_y_edges``) — but the same-frame
                    //  loop above can only fill the central ±frame_size
                    //  cc region.  The away sides (where the DCR /
                    //  background population lives one rollover before
                    //  and after the trigger) stay empty until we
                    //  actually iterate hits from the neighbouring
                    //  frames at ± exactly one rollover offset.
                    //
                    //  Frame offset = rollover_cc / frame_size — at the
                    //  default 32768 / 1024 that's 32 frames.  Both
                    //  hit.coarse and trigger.coarse remain bounded to
                    //  [0, frame_size) after the framer's per-frame
                    //  bucketing, so the raw (hit.coarse − trigger.coarse)
                    //  for a hit one rollover later has the SAME range
                    //  as the in-frame case.  Adding ±kRollover lands
                    //  the fill in the matching zoom pad.
                    //
                    //  Thread safety: this whole writer-side block runs
                    //  in the serial post-framer consumer loop (see
                    //  ``for (uint32_t frame_id : main_sorted_keys)`` at
                    //  the top of this scope), so reading other frames'
                    //  cherenkov_hits via spilldata.get_frame_link is
                    //  lock-free.  The framer's per-spill worker pool
                    //  has already drained by the time we get here.
                    {
                        constexpr int kRollover =
                            BTANA_ALCOR_ROLLOVER_TO_CC;
                        //  Frame offset spanning exactly one rollover.
                        //  Integer division is EXACT only when frame_size
                        //  divides the rollover (true for the default
                        //  1024 = 2^10 into 32768 = 2^15).  For a
                        //  frame_size that doesn't divide evenly the
                        //  away-side frames would land a fraction of a
                        //  frame off the true ±rollover boundary,
                        //  smearing the diagnostic zoom pads — so we skip
                        //  the away-side fills entirely in that case
                        //  rather than fill them at a wrong offset.
                        const int frame_size_i =
                            static_cast<int>(framer_cfg.frame_size);
                        const bool rollover_divides_frame =
                            frame_size_i > 0 &&
                            (kRollover % frame_size_i) == 0;
                        const int kFrameOffsetForRollover =
                            rollover_divides_frame
                                ? kRollover / frame_size_i
                                : 0;
                        const auto &frame_link = spilldata.get_frame_link();

                        auto fill_away_side =
                            [&](int32_t neighbour_frame_id, int rollover_sign)
                        {
                            //  Negative neighbour ids are pre-spill —
                            //  no data, skip silently.  Missing entries
                            //  are also fine: not every frame has hits
                            //  on disk.
                            if (neighbour_frame_id < 0)
                                return;
                            auto it = frame_link.find(
                                static_cast<uint32_t>(neighbour_frame_id));
                            if (it == frame_link.end())
                                return;
                            for (const auto &neighbour_hit_struct :
                                 it->second.cherenkov_hits)
                            {
                                AlcorFinedata neighbour_hit(neighbour_hit_struct);
                                if (neighbour_hit.is_afterpulse())
                                    continue;
                                const int dc_cc_raw =
                                    static_cast<int>(neighbour_hit.get_coarse()) - static_cast<int>(current_trigger.coarse);
                                h_trigger_anchor_dt[current_trigger.index]->Fill(
                                    static_cast<double>(ispill),
                                    static_cast<double>(
                                        dc_cc_raw + rollover_sign * kRollover));
                            }
                        };

                        //  Skip when the offset is 0 (frame_size does not
                        //  divide the rollover) — a 0 offset would point
                        //  back at the trigger's own frame and double-fill
                        //  the central pad instead of the away sides.
                        if (kFrameOffsetForRollover > 0)
                        {
                            fill_away_side(
                                static_cast<int32_t>(frame_id) - kFrameOffsetForRollover, -1);
                            fill_away_side(
                                static_cast<int32_t>(frame_id) + kFrameOffsetForRollover, +1);
                        }
                    }

                    h_trigger_hit_multiplicity[current_trigger.index][0]->Fill(hit_counter[0]);
                    h_trigger_hit_multiplicity[current_trigger.index][1]->Fill(hit_counter[1]);
                }

                //  ── Pairwise Δt(trigger_i, trigger_j) within this frame ──
                //
                //  Iterates unordered pairs over triggers_in_frame and
                //  fills Δt = t_hi − t_lo for each (lo, hi) with lo<hi.
                //  Skips:
                //    - synthetic markers (FirstFrames / StartOfSpill) on
                //      either leg;
                //    - the (streaming, RANSAC) pair specifically, because
                //      RANSAC is downstream of streaming so their Δt is
                //      a near-deterministic stage delay, not physics.
                //  Range is ±frame_length_ns_q — the bound any pair can
                //  achieve within a frame.  Triangle acceptance is applied
                //  at write time (see ~line 1370 sweep below).
                for (size_t ia = 0; ia < triggers_in_frame.size(); ++ia)
                {
                    const auto &tra = triggers_in_frame[ia];
                    if (is_synthetic_marker(tra.index))
                        continue;
                    for (size_t ib = ia + 1; ib < triggers_in_frame.size(); ++ib)
                    {
                        const auto &trb = triggers_in_frame[ib];
                        if (is_synthetic_marker(trb.index))
                            continue;
                        const bool stream_ransac_pair =
                            (tra.index == _TRIGGER_STREAMING_RING_FOUND_ &&
                             trb.index == _TRIGGER_RANSAC_RING_FOUND_) ||
                            (tra.index == _TRIGGER_RANSAC_RING_FOUND_ &&
                             trb.index == _TRIGGER_STREAMING_RING_FOUND_);
                        if (stream_ransac_pair)
                            continue;
                        const uint8_t i_lo = std::min(tra.index, trb.index);
                        const uint8_t i_hi = std::max(tra.index, trb.index);
                        const float t_lo = (tra.index == i_lo) ? tra.fine_time : trb.fine_time;
                        const float t_hi = (tra.index == i_hi) ? tra.fine_time : trb.fine_time;
                        const uint16_t key = (static_cast<uint16_t>(i_lo) << 8) | i_hi;
                        auto it = h_trigger_pair_dt.find(key);
                        if (it == h_trigger_pair_dt.end())
                        {
                            const std::string name_lo = registry.name_of(i_lo);
                            const std::string name_hi = registry.name_of(i_hi);
                            it = h_trigger_pair_dt.emplace(key, RootHist<TH1F>(
                                                                    TString::Format("h_trigger_pair_dt_%s_vs_%s",
                                                                                    name_hi.c_str(), name_lo.c_str())
                                                                        .Data(),
                                                                    TString::Format(
                                                                        ";#Delta_{t} (t_{%s} - t_{%s}) ns;Normalised entries / acceptance",
                                                                        name_hi.c_str(), name_lo.c_str())
                                                                        .Data(),
                                                                    2000, -frame_length_ns_q, frame_length_ns_q))
                                     .first;
                        }
                        it->second->Fill(static_cast<double>(t_hi - t_lo));
                    }
                }
                //  ---
                //  --- DCR + afterpulse + cross-talk QA
                //  Gated on the first-frames trigger.  Fill body lives
                //  in `src/writers/lightdata/dcr_afterpulse_ct_qa.cxx`
                //  (the per-frame QA helper) to keep this writer
                //  focused on orchestration.
                if (fired_trigger_types.count(registry.index_of(static_cast<TriggerNumber>(TriggerFirstFrames))))
                {
                    ::btana::lightdata::DcrAfterpulseCtHists qa_hists;
                    qa_hists.h_dcr_per_channel = h_dcr_per_channel.get();
                    qa_hists.h_dcr_hitmap = h_dcr_hitmap.get();
                    qa_hists.h_afterpulse_near_per_channel = h_afterpulse_near_per_channel.get();
                    qa_hists.h_afterpulse_far_per_channel = h_afterpulse_far_per_channel.get();
                    qa_hists.h_afterpulse_per_channel = h_afterpulse_per_channel.get();
                    if (tot_qa) // ToT-family run → enable the ToT QA fills
                    {
                        qa_hists.h_tot_spectrum = h_tot_spectrum.get();
                        qa_hists.h_tot_vs_channel = h_tot_vs_channel.get();
                        qa_hists.h_tot_secondary_orphan_per_channel = h_tot_secondary_orphan_per_channel.get();
                        qa_hists.h_tot_leading_orphan_per_channel = h_tot_leading_orphan_per_channel.get();
                        qa_hists.tot_spectrum_by_device = &tot_spectrum_by_device;
                    }
                    qa_hists.h_afterpulse_near_hitmap = h_afterpulse_near_hitmap.get();
                    qa_hists.h_afterpulse_far_hitmap = h_afterpulse_far_hitmap.get();
                    qa_hists.h_afterpulse_hitmap = h_afterpulse_hitmap.get();
                    qa_hists.h_phys_ct_per_channel = h_phys_ct_per_channel.get();
                    qa_hists.h_elec_ct_per_channel = h_elec_ct_per_channel.get();
                    qa_hists.h_phys_ct_hitmap = h_phys_ct_hitmap.get();
                    qa_hists.h_elec_ct_hitmap = h_elec_ct_hitmap.get();
                    qa_hists.h_phys_ct_dt = h_phys_ct_dt.get();
                    qa_hists.h_elec_ct_dt = h_elec_ct_dt.get();
                    qa_hists.h_elec_ct_dchannel_dt = h_elec_ct_dchannel_dt.get();
                    qa_hists.h_phys_ct_dchannel_dt = h_phys_ct_dchannel_dt.get();
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
    //  Diagnostic — surface the rollover-straddling-frame rate.
    //  Expected ≈ frame_size / rollover (3 % at default settings)
    //  *of pairs that crossed*.  A radically different number is a
    //  smoke signal (framer config drift, anchor mis-assignment, …).
    mist::logger::info(TString::Format(
                           "(lightdata_writer) anchor-Δt rollover wraps: %lld pairs",
                           n_anchor_dt_rollover_wrap)
                           .Data());

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
    //  TOML v3 only — see task #172.  ``write_calib_to_file`` will
    //  hard-error if the path doesn't end in ``.toml``.
    AlcorFinedata::write_calib_to_file((base_dir / "fine_calib.toml").string());
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
    //
    //  Layout:
    //    Triggers/
    //      h2_trigger_matrix                 ← aggregate, top-level
    //      <trigger-name>/                   ← one sub-dir per trigger
    //        h_trigger_frame_population_<name>
    //        h_trigger_time_diff_w_cherenkov_<name>
    //        h_trigger_full_hitmap_<name>
    //        h_trigger_hit_multiplicity_in_time_<name>
    //        h_trigger_hit_multiplicity_out_of_time_<name>
    //        h_trigger_dt_<name>
    //
    //  The dashboard's QA Lightdata sub-tab mirrors this tree
    //  automatically via the schema-mirror layout, so each trigger
    //  gets a collapsible sub-card with all its control histograms.
    //  Aggregate histograms stay at the Triggers/ level so they're
    //  visible without expanding any per-trigger card.
    TDirectory *trigger_dir = outfile->mkdir("Triggers");
    trigger_dir->cd();
    h2_trigger_matrix->Write();

    //  Per-trigger sub-folder helper — same trigger index keys all
    //  the maps below, so we cd() into the right sub-dir once and
    //  write each per-trigger histogram from there.  Pre-scales any
    //  ratios that used to happen in-line (time-diff normalisation
    //  by trigger-matrix diagonal; dt scaling by bin width).
    std::map<int, TDirectory *> per_trigger_dirs;
    auto get_dir_for = [&](int idx) -> TDirectory *
    {
        auto it = per_trigger_dirs.find(idx);
        if (it != per_trigger_dirs.end())
            return it->second;
        const std::string name = registry.name_of(idx);
        TDirectory *d = trigger_dir->mkdir(name.c_str());
        per_trigger_dirs[idx] = d;
        return d;
    };
    auto write_in = [&](int idx, auto *h)
    {
        if (!h)
            return;
        auto *d = get_dir_for(idx);
        d->cd();
        h->Write();
    };

    for (auto &[key, val] : h_trigger_frame_population)
        write_in(key, val.get());

    //  Triangle acceptance correction.
    //
    //  For two times uniformly distributed in a frame of length L, the
    //  distribution of their difference is a triangle on [-L, +L] with
    //  density (L - |Δt|)/L² (peaks at 0, goes to 0 at ±L).  Dividing
    //  each bin by (L - |Δt|)/L undoes the geometric acceptance so a
    //  flat random-pair distribution (e.g. uncorrelated DCR background)
    //  becomes flat.  Signal peaks at small |Δt| then read against a
    //  flat background instead of a sloped one.
    //
    //  Edge bins are capped to a minimum 1 % acceptance to keep their
    //  errors finite — bins outside ±L are forced to zero (no pair can
    //  physically achieve Δt > L within a single frame).
    const double tri_L_ns = framer_cfg.frame_length_ns();
    auto apply_triangle_correction = [&](TH1F *h)
    {
        if (!h)
            return;
        for (int b = 1; b <= h->GetNbinsX(); ++b)
        {
            const double dt = h->GetBinCenter(b);
            const double abs_dt = std::abs(dt);
            if (abs_dt >= tri_L_ns)
            {
                h->SetBinContent(b, 0.);
                h->SetBinError(b, 0.);
                continue;
            }
            const double accept = std::max(0.01, (tri_L_ns - abs_dt) / tri_L_ns);
            h->SetBinContent(b, h->GetBinContent(b) / accept);
            h->SetBinError(b, h->GetBinError(b) / accept);
        }
    };

    for (auto &[key, val] : h_trigger_time_diff_w_cherenkov)
    {
        const double n_trig = h2_trigger_matrix->GetBinContent(
            registry.index_of(key) + 1,
            registry.index_of(key) + 1);
        if (n_trig > 0.)
            val->Scale(1. / n_trig);
        apply_triangle_correction(val.get());
        write_in(key, val.get());
    }
    for (auto &[key, val] : h_trigger_hit_multiplicity)
    {
        write_in(key, val[0].get());
        write_in(key, val[1].get());
    }
    for (auto &[key, val] : h_trigger_full_hitmap)
        write_in(key, val.get());
    for (auto &[key, val] : h_trigger_window_hitmap)
        write_in(key, val.get());
    for (auto &[key, val] : h_trigger_dt)
    {
        val->Scale(1., "width");
        write_in(key, val.get());
    }
    //  Anchor-Δt cc-domain TH2F per fired trigger — written to the
    //  same per-trigger TDirectory as the other per-trigger plots
    //  so a TBrowser dive lands all related histograms together.
    for (auto &[key, val] : h_trigger_anchor_dt)
        write_in(key, val.get());
    //  Δt-vs-ToT per fired trigger (ToT-family runs only; map empty in LET).
    for (auto &[key, val] : h_trigger_dt_vs_tot)
        write_in(key, val.get());

    //  Pairwise Δt(trigger_i, trigger_j) — lives in Triggers/Pairs/ so
    //  the dashboard's QA Lightdata sub-tab gets a separate collapsible
    //  group for the pair panel without polluting any per-trigger
    //  sub-folder.  Triangle-acceptance corrected in place before write.
    if (!h_trigger_pair_dt.empty())
    {
        TDirectory *pair_dir = trigger_dir->mkdir("Pairs");
        pair_dir->cd();
        for (auto &[key, val] : h_trigger_pair_dt)
        {
            apply_triangle_correction(val.get());
            val->Write();
        }
        trigger_dir->cd();
    }
    //  ---
    //  --- Timing
    //  C6 addendum: skip the Timing/ directory + canvas when no
    //  timing data was seen in this run.  Empty hists on the dashboard
    //  read as a "broken" run; an absent directory is cleaner and
    //  matches the lazy-create pattern used for the per-trigger cards.
    if (timing_data_seen)
    {
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
    }
    else
    {
        mist::logger::info("(lightdata_writer) Skipping Timing/ directory — "
                           "no decoded timing data seen this run "
                           "(C6 addendum: avoids empty-card 'broken run' on the dashboard).");
    }
    //  ---
    //  --- DCR
    TDirectory *DCR_dir = outfile->mkdir("Single-Pixel Noise");
    DCR_dir->cd();
    h_dcr_per_channel->Scale(1. / (framer_cfg.frame_length_ns() * 1.e-6));
    h_dcr_per_channel->Write();

    //  --- Timing-sensor DCR: total noise-window counts → kHz per channel.
    //  rate = n_hits / (n_noise_frames × frame_length_ns) × 1e6.  Average over
    //  channels that fired at least once.
    if (n_timing_noise_frames > 0 && framer_cfg.frame_length_ns() > 0.f)
    {
        const double inv_khz =
            1.e6 / (static_cast<double>(n_timing_noise_frames) *
                    static_cast<double>(framer_cfg.frame_length_ns()));
        double sum_khz = 0.0, sum0 = 0.0, sum1 = 0.0;
        int n_live = 0, n0 = 0, n1 = 0;
        for (int i = 0; i < 64; ++i)
            if (timing_dcr_counts[static_cast<size_t>(i)] > 0)
            {
                const double khz =
                    static_cast<double>(timing_dcr_counts[static_cast<size_t>(i)]) *
                    inv_khz;
                h_timing_dcr_per_channel->SetBinContent(i + 1, khz);
                sum_khz += khz;
                ++n_live;
                if (i < 32)
                {
                    sum0 += khz;
                    ++n0;
                }
                else
                {
                    sum1 += khz;
                    ++n1;
                }
            }
        timing_dcr_mean_khz = (n_live > 0) ? sum_khz / n_live : 0.0;
        timing_dcr_chip0_khz = (n0 > 0) ? sum0 / n0 : 0.0;
        timing_dcr_chip1_khz = (n1 > 0) ? sum1 / n1 : 0.0;
        h_timing_dcr_per_channel->SetTitle(TString::Format(
            "timing-sensor DCR  —  total avg %.2f kHz  (chip0 %.2f, chip1 "
            "%.2f)  [%ld noise frames];"
            "timing channel (chip0:0-31 | chip1:32-63);DCR [kHz]",
            timing_dcr_mean_khz, timing_dcr_chip0_khz, timing_dcr_chip1_khz,
            n_timing_noise_frames));
        mist::logger::info(TString::Format(
                               "(lightdata_writer) timing-sensor DCR: total avg %.3f kHz "
                               "(chip0 %.3f, chip1 %.3f) over %d channels (%ld noise frames)",
                               timing_dcr_mean_khz, timing_dcr_chip0_khz, timing_dcr_chip1_khz,
                               n_live, n_timing_noise_frames)
                               .Data());
    }
    else
        mist::logger::info("(lightdata_writer) timing-sensor DCR: no noise "
                           "frames / timing hits — plot empty");
    h_timing_dcr_per_channel->Write();
    h_dcr_hitmap->Write();
    h_afterpulse_near_per_channel->Write();
    h_afterpulse_near_hitmap->Write();
    h_afterpulse_far_per_channel->Write();
    h_afterpulse_far_hitmap->Write();
    h_afterpulse_per_channel->Write();
    h_afterpulse_hitmap->Write();
    if (tot_qa) // ToT-family run → persist the ToT QA hists for Full-plots discovery
    {
        h_tot_spectrum->Write();
        h_tot_vs_channel->Write();
        h_tot_secondary_orphan_per_channel->Write();
        h_tot_leading_orphan_per_channel->Write();
        for (auto &[sen, hist] : tot_spectrum_by_sensor)
            hist->Write();
    }
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
    //  Finalize body (overlay canvases + RANSAC-rings TDirectories)
    //  lives in `src/writers/lightdata/finalize_streaming_qa.cxx`.
    {
        const auto streaming_ring_index =
            registry.index_of(static_cast<TriggerNumber>(_TRIGGER_STREAMING_RING_FOUND_));
        ::btana::lightdata::StreamingTriggerFinalizeContext ctx{};
        ctx.outfile = outfile.get();
        ctx.h_streaming_score_noise = h_streaming_score_noise.get();
        ctx.h_streaming_score_data = h_streaming_score_data.get();
        ctx.h_streaming_score_inbeam = h_streaming_score_inbeam.get();
        ctx.h_streaming_trigger_full_hitmap = h_streaming_trigger_full_hitmap.get();
        ctx.h_streaming_trigger_time_cut_hitmap = h_streaming_trigger_time_cut_hitmap.get();
        ctx.h_streaming_trigger_ring_finder_nrings = h_streaming_trigger_ring_finder_nrings.get();
        ctx.h_streaming_trigger_ring_finder_hitmap = h_streaming_trigger_ring_finder_hitmap.get();
        ctx.h_streaming_trigger_ring_finder_first_hitmap = h_streaming_trigger_ring_finder_first_hitmap.get();
        ctx.h_streaming_trigger_ring_finder_second_hitmap = h_streaming_trigger_ring_finder_second_hitmap.get();
        ctx.h_ring_X_first_ransac = h_streaming_trigger_ring_X_first_ransac.get();
        ctx.h_ring_Y_first_ransac = h_streaming_trigger_ring_Y_first_ransac.get();
        ctx.h_ring_R_first_ransac = h_streaming_trigger_ring_R_first_ransac.get();
        ctx.h_ring_X_second_ransac = h_streaming_trigger_ring_X_second_ransac.get();
        ctx.h_ring_Y_second_ransac = h_streaming_trigger_ring_Y_second_ransac.get();
        ctx.h_ring_R_second_ransac = h_streaming_trigger_ring_R_second_ransac.get();
        ctx.h_ring_peak_votes_vs_active_first = h_streaming_trigger_ring_peak_votes_vs_active_first.get();
        ctx.h_ring_peak_votes_vs_active_second = h_streaming_trigger_ring_peak_votes_vs_active_second.get();
        ctx.h_ring_hit_arc_dist_first = h_streaming_trigger_ring_hit_arc_dist_first.get();
        ctx.h_ring_hit_arc_dist_second = h_streaming_trigger_ring_hit_arc_dist_second.get();
        ctx.h_ring_finder_first_hitmap_dual = h_streaming_trigger_ring_finder_first_hitmap_dual.get();
        ctx.h_ring_X_first_ransac_dual = h_streaming_trigger_ring_X_first_ransac_dual.get();
        ctx.h_ring_Y_first_ransac_dual = h_streaming_trigger_ring_Y_first_ransac_dual.get();
        ctx.h_ring_R_first_ransac_dual = h_streaming_trigger_ring_R_first_ransac_dual.get();
        ctx.h_ring_peak_votes_vs_active_first_dual = h_streaming_trigger_ring_peak_votes_vs_active_first_dual.get();
        ctx.h_ring_hit_arc_dist_first_dual = h_streaming_trigger_ring_hit_arc_dist_first_dual.get();
        ctx.h_ring_finder_first_hitmap_solo = h_streaming_trigger_ring_finder_first_hitmap_solo.get();
        ctx.h_ring_X_first_ransac_solo = h_streaming_trigger_ring_X_first_ransac_solo.get();
        ctx.h_ring_Y_first_ransac_solo = h_streaming_trigger_ring_Y_first_ransac_solo.get();
        ctx.h_ring_R_first_ransac_solo = h_streaming_trigger_ring_R_first_ransac_solo.get();
        ctx.h_ring_peak_votes_vs_active_first_solo = h_streaming_trigger_ring_peak_votes_vs_active_first_solo.get();
        ctx.h_ring_hit_arc_dist_first_solo = h_streaming_trigger_ring_hit_arc_dist_first_solo.get();
        //  Streaming-ring trigger map lookups — caller resolves the
        //  registry-indexed lookup; pass nullptr if no entry.
        ctx.streaming_ring_frame_population =
            h_trigger_frame_population.count(streaming_ring_index)
                ? h_trigger_frame_population[streaming_ring_index].get()
                : nullptr;
        ctx.streaming_ring_time_diff_w_cherenkov =
            h_trigger_time_diff_w_cherenkov.count(streaming_ring_index)
                ? h_trigger_time_diff_w_cherenkov[streaming_ring_index].get()
                : nullptr;
        ctx.streaming_ring_full_hitmap =
            h_trigger_full_hitmap.count(streaming_ring_index)
                ? h_trigger_full_hitmap[streaming_ring_index].get()
                : nullptr;
        ::btana::lightdata::finalize_streaming_qa(ctx);
    }
    //  ---
    //  --- Config — write all processing settings for reproducibility
    //
    //  Routed through util::ConfigDump (include/utility/config_dump.h)
    //  so every writer in the project shares one ``Config/`` layout
    //  and the QA dashboard's DataInspectPane reads them uniformly.
    //  The on-disk schema is unchanged from the hand-rolled version
    //  this replaces — same key names, same TParameter/TNamed kinds.
    {
        util::ConfigDump cfg(outfile.get());

        //  Framer numeric knobs.
        cfg.add("frame_size", framer_cfg.frame_size)
            .add("first_frames_trigger", framer_cfg.first_frames_trigger)
            .add("afterpulse_deadtime", framer_cfg.afterpulse_deadtime)
            .add("trigger_secondary_window", framer_cfg.trigger_secondary_window)
            .add("frame_length_ns", framer_cfg.frame_length_ns());

        //  QA windows used by the afterpulse sideband subtraction and the CT scan.
        cfg.add("qa_afterpulse_near_lo", qa_cfg.afterpulse_near_lo)
            .add("qa_afterpulse_near_hi", qa_cfg.afterpulse_near_hi)
            .add("qa_afterpulse_sideband_offset", qa_cfg.afterpulse_sideband_offset)
            .add("qa_ct_scan_dt_min", qa_cfg.ct_scan_dt_min)
            .add("qa_ct_scan_dt_max", qa_cfg.ct_scan_dt_max)
            .add("qa_ct_phys_signal_lo", qa_cfg.ct_phys_signal_lo)
            .add("qa_ct_phys_signal_hi", qa_cfg.ct_phys_signal_hi)
            .add("qa_ct_elec_signal_lo", qa_cfg.ct_elec_signal_lo)
            .add("qa_ct_elec_signal_hi", qa_cfg.ct_elec_signal_hi);

        //  Conf-file paths + verbatim TOML snapshots.  ``add_conf_file``
        //  writes both ``<key>_file`` (the path as TNamed) and
        //  ``<key>_toml`` (the file content as TNamed) — matches the
        //  previous hand-rolled emission exactly.
        cfg.add_conf_file("trigger_conf", trigger_setup_file)
            .add_conf_file("readout_conf", readout_config_file)
            .add_conf_file("mapping_conf", mapping_config_file)
            .add_conf_file("framer_conf", framer_conf_file)
            .add_conf_file("streaming_conf", streaming_conf_file)
            //  Fine-calib has no TOML body to snapshot in v1 (it's a
            //  text file produced by pulser_calib_writer); only the
            //  path is recorded so downstream knows which calib was used.
            .add_path("fine_calib_conf_file", fine_calibration_config_file);
    }

    //  ---
    //  --- Curated QA PDFs for the dashboard
    //
    //  The QA tab's "Lightdata" sub-tab looks for *.pdf under
    //  ``Data/<run>/qa/lightdata/`` and renders them inline.  Emit a
    //  small, curated set here — these are the plots a shifter looks
    //  at first; the full set still lives in lightdata.root for
    //  deep dives via the Inspect → TBrowser path.
    //
    //  Order matters: the 2-digit filename prefix governs the order
    //  the dashboard renders them in (alphabetical by basename).
    //
    //  Failures are quiet — TCanvas::SaveAs reports its own warnings
    //  to ROOT's logger and we don't want a broken plot to tank the
    //  whole writer run.
    {
        const std::string run_dir = data_repository + "/" + run_name;

        //  Global stat-box policy for the curated PDF set: OFF.
        //
        //  Rule (project-wide): a PDF emitted from here is a CURATED
        //  output — its purpose is to convey a specific message with
        //  controlled layout (titles, overlays, annotations).  Stat
        //  boxes overlap the curated geometry and pull the eye away
        //  from the intended takeaway.  Operators who want raw stats
        //  use the dashboard's Inspect button on the ROOT object — the
        //  full TFile is always available for deep dives.
        //
        //  Applied here once for the writer-process — every save_one
        //  call below and the inline streaming-score block honour it
        //  without per-histogram opt-outs.  Re-enable explicitly with
        //  h->SetStats(1) before Draw() if a specific PDF needs them.
        gStyle->SetOptStat(0);

        //  PDF page geometry: ROOT 6.40 / macOS ignores
        //  gStyle->SetPaperSize() — the MediaBox is ALWAYS A4 portrait
        //  (595×842 pt) regardless of TCanvas size, so every PDF gets
        //  a square plot embedded in a portrait page with whitespace
        //  below.  Defeats the equal-aspect design.  Fix: post-process
        //  each emitted PDF through util::qa::crop_pdf_inplace()
        //  (shells out to pdfcrop) which rewrites MediaBox to the
        //  content bounding box.  Best-effort: if pdfcrop isn't on
        //  PATH the call is a silent no-op.

        auto save_one = [&run_dir](int order, const std::string &name,
                                   TH1 *h, const char *draw_opt)
        {
            if (!h)
                return;
            // Compose a unique canvas name per plot so simultaneous
            // saves don't collide on the gROOT canvas registry.
            //
            //  Square canvas — matches the anchor-Δt PDFs the
            //  render_anchor_dt_canvas helper emits, so the dashboard's
            //  responsive QA grid tiles every lightdata PDF at the same
            //  aspect ratio and the gallery doesn't shimmer with
            //  alternating landscape / portrait cards.
            TCanvas c(TString::Format("c_qa_lightdata_%02d_%s", order, name.c_str()),
                      "", 1000, 1000);
            h->Draw(draw_opt);
            const auto path = util::qa::pdf_path(run_dir, "lightdata", order, name);
            c.SaveAs(path.string().c_str());
            //  Crop ROOT's A4-portrait wrapper away — see the
            //  comment block at the top of this PDF emission scope.
            util::qa::crop_pdf_inplace(path);
        };

        // ToT QA (only emitted for ToT-family runs → LET produces no ToT PDFs,
        // so the dashboard's ToT overview tiles auto-drop).
        if (tot_qa)
        {
            //  ToT spectrum render: log-Y, the 1 p.e./2 p.e. threshold (spectrum
            //  valley between the main peak and ~2.4x it) and a Gaussian fit to
            //  EACH of the 1 p.e. and 2 p.e. peaks → centre (mu) + sigma annotated.
            //  Used for the combined spectrum and each per-sensor spectrum.
            //  TODO(config): promote the threshold to a run-db / conf field.
            auto render_tot_spectrum = [&run_dir](int order, const std::string &name,
                                                  TH1F *hs, const std::string &sensor_label)
            {
                if (!hs || hs->GetEntries() < 50)
                    return;
                const int pk = hs->GetMaximumBin();
                const double pk_ns = hs->GetBinCenter(pk);
                //  1 p.e. Gaussian (±12 ns window around the main peak) — a seed.
                TF1 f1("f_tot_1pe", "gaus");
                f1.SetRange(std::max(0.0, pk_ns - 12.0), pk_ns + 12.0);
                hs->Fit(&f1, "RQ0");
                double amp1 = f1.GetParameter(0), mu1 = f1.GetParameter(1), sig1 = f1.GetParameter(2);

                //  2 p.e. peak: a genuine local maximum above the 1 p.e. valley
                //  (interior of [mu1 + 1.5sigma, 4*mu1]).  The peaks are CLOSE and
                //  the 2 p.e. sits on the 1 p.e. tail, so once a second peak is
                //  found we fit a DOUBLE Gaussian (1 p.e. + 2 p.e.) seeded from both
                //  — a lone Gaussian on the 2 p.e. window is dragged by the tail.
                //  Threshold = valley (minimum) between the peaks.  No second peak
                //  (no pile-up) -> 1 p.e.-only, no threshold line.
                TF1 f2("f_tot_2pe", "gaus");
                bool have2 = false;
                double amp2 = 0, mu2 = 0, sig2 = 0, thr_ns = -1;
                {
                    //  Walk up from the 1 p.e. peak to the valley (last falling
                    //  bin); the 2 p.e. peak is the maximum above it.  No magic
                    //  offset — works for close peaks where the valley sits just
                    //  above the 1 p.e. tail.
                    const int pkbin = hs->GetMaximumBin();
                    const int hi2 = std::min(hs->FindBin(4.0 * mu1), hs->GetNbinsX());
                    int vb = pkbin;
                    for (int b = pkbin + 1; b <= hi2; ++b)
                    {
                        if (hs->GetBinContent(b) <= hs->GetBinContent(b - 1))
                            vb = b; // still falling
                        else
                            break; // started rising → valley reached
                    }
                    int pk2 = vb;
                    double pk2v = 0;
                    for (int b = vb; b <= hi2; ++b)
                        if (hs->GetBinContent(b) > pk2v)
                        {
                            pk2v = hs->GetBinContent(b);
                            pk2 = b;
                        }
                    if (pk2 > vb && pk2v >= 10) // a genuine rise above the valley
                    {
                        thr_ns = hs->GetBinCenter(vb);
                        const double pk2_ns = hs->GetBinCenter(pk2);
                        TF1 fdg("f_tot_dg", "gaus(0)+gaus(3)");
                        fdg.SetParameters(amp1, mu1, sig1, pk2v, pk2_ns, sig1);
                        fdg.SetRange(std::max(0.0, mu1 - 2.0 * sig1), pk2_ns + 3.0 * sig1);
                        hs->Fit(&fdg, "RQ0");
                        amp1 = fdg.GetParameter(0);
                        mu1 = fdg.GetParameter(1);
                        sig1 = fdg.GetParameter(2);
                        amp2 = fdg.GetParameter(3);
                        mu2 = fdg.GetParameter(4);
                        sig2 = fdg.GetParameter(5);
                        have2 = (sig2 > 0 && mu2 > thr_ns && mu2 > mu1);
                    }
                }
                //  Component curves for drawing, from the (possibly joint) fit.
                f1.SetParameters(amp1, mu1, sig1);
                f1.SetRange(mu1 - 3.0 * sig1, mu1 + 3.0 * sig1);
                if (have2)
                {
                    f2.SetParameters(amp2, mu2, sig2);
                    f2.SetRange(mu2 - 3.0 * sig2, mu2 + 3.0 * sig2);
                }
                TCanvas c(TString::Format("c_qa_lightdata_%02d_%s", order, name.c_str()), "", 1000, 1000);
                c.SetLogy();
                hs->SetMinimum(0.5);
                hs->Draw("hist");
                f1.SetLineColor(kAzure + 2);
                f1.SetNpx(400);
                f1.Draw("same");
                TLine thr(thr_ns, 0.5, thr_ns, hs->GetMaximum());
                thr.SetLineColor(kRed + 1);
                thr.SetLineStyle(2);
                thr.SetLineWidth(2);
                if (have2)
                {
                    f2.SetLineColor(kGreen + 2);
                    f2.SetNpx(400);
                    f2.Draw("same");
                    thr.Draw();
                }
                //  Stats right-aligned in the top-right — the peaks sit on the left,
                //  so the high-ToT region there is empty and won't overlap.
                TLatex lbl;
                lbl.SetNDC();
                lbl.SetTextSize(0.028);
                lbl.SetTextAlign(31); // right-aligned
                lbl.SetTextColor(kAzure + 2);
                lbl.DrawLatex(0.90, 0.88, TString::Format("%s   1 p.e.:  #mu = %.1f,  #sigma = %.1f ns", sensor_label.c_str(), mu1, sig1));
                if (have2)
                {
                    lbl.SetTextColor(kGreen + 2);
                    lbl.DrawLatex(0.90, 0.84, TString::Format("2 p.e.:  #mu = %.1f,  #sigma = %.1f ns", mu2, sig2));
                    lbl.SetTextColor(kRed + 1);
                    lbl.DrawLatex(0.90, 0.80, TString::Format("threshold = %.1f ns", thr_ns));
                }
                const auto path = util::qa::pdf_path(run_dir, "lightdata", order, name);
                c.SaveAs(path.string().c_str());
                util::qa::crop_pdf_inplace(path);
            };
            render_tot_spectrum(20, "tot_spectrum", h_tot_spectrum.get(), "all sensors");
            {
                int order = 23;
                for (auto &[sen, hist] : tot_spectrum_by_sensor)
                    render_tot_spectrum(order++, "tot_spectrum_" + sen, hist.get(), "sensor " + sen);
            }
            //  ToT vs channel: bespoke so we can set log-Z (occupancy spans orders
            //  of magnitude across channels).
            {
                TCanvas c("c_qa_lightdata_21_tot_vs_channel", "", 1000, 1000);
                c.SetLogz();
                h_tot_vs_channel->Draw("colz");
                const auto path = util::qa::pdf_path(run_dir, "lightdata", 21, "tot_vs_channel");
                c.SaveAs(path.string().c_str());
                util::qa::crop_pdf_inplace(path);
            }
            //  Pairing health: secondary- + leading-orphan rates per channel, overlaid.
            auto *sec = h_tot_secondary_orphan_per_channel.get();
            auto *led = h_tot_leading_orphan_per_channel.get();
            if (sec && led)
            {
                sec->SetStats(0);
                sec->SetLineColor(kRed + 1);
                sec->SetMarkerColor(kRed + 1);
                led->SetLineColor(kAzure + 2);
                led->SetMarkerColor(kAzure + 2);
                sec->SetTitle("ToT pairing health;channel;orphan probability (%)");
                TCanvas c("c_qa_lightdata_22_tot_pairing_health", "", 1000, 1000);
                sec->Draw("hist");
                led->Draw("hist same");
                TLegend lg(0.45, 0.80, 0.88, 0.90);
                lg.AddEntry(sec, "secondary orphan (missing stop)", "l");
                lg.AddEntry(led, "leading orphan (missing start)", "l");
                lg.Draw();
                const auto path = util::qa::pdf_path(run_dir, "lightdata", 22, "tot_pairing_health");
                c.SaveAs(path.string().c_str());
                util::qa::crop_pdf_inplace(path);
            }
        }

        // 01 Trigger coincidence matrix — which triggers fire together.
        //  Draw with "colz text" so the bin contents (the number of
        //  (i, j) coincidences) are printed in each cell.  Without the
        //  numbers, the colour scale alone is hard to read for the
        //  matrix entries that matter most (the diagonal counts and the
        //  off-diagonal coincidences are what the operator needs to
        //  compare exactly, not as colour bands).
        h2_trigger_matrix->SetMarkerSize(1.4);
        gStyle->SetPaintTextFormat(".5g"); // scientific when > 99999
        //  Bespoke save (not via the generic save_one lambda) so we
        //  can rotate the X-axis labels and breathe out the pad
        //  margins.  Default ROOT auto-rotation crowds long trigger
        //  names on the X side and clips them on the Y side; with
        //  ~12 entries like ``RANSAC_RING_FOUND`` and
        //  ``broad_scintillator`` it gets unreadable.
        //
        //  ``LabelsOption("v")`` is the binned-axis rotation idiom
        //  that survives across ROOT 6 versions — ``SetLabelAngle``
        //  lives on TGaxis only, not on TH1's TAxis, so it can't be
        //  called here.  Y-axis stays horizontal because vertical Y
        //  labels overlap the bin grid; the wider left margin gives
        //  the longest entry breathing room instead.
        {
            auto *xax = h2_trigger_matrix->GetXaxis();
            auto *yax = h2_trigger_matrix->GetYaxis();
            xax->LabelsOption("v");
            xax->SetLabelSize(0.028);
            yax->SetLabelSize(0.028);
            xax->SetLabelOffset(0.005);
            yax->SetLabelOffset(0.005);
            TCanvas c("c_qa_lightdata_01_trigger_matrix", "", 1000, 1000);
            //  Wide margins for the rotated labels.  Top is generous
            //  so the colour-axis text doesn't crowd; right is left
            //  modest because the palette eats some of it anyway.
            c.SetLeftMargin(0.24);
            c.SetBottomMargin(0.22);
            c.SetTopMargin(0.08);
            c.SetRightMargin(0.14);
            h2_trigger_matrix->Draw("colz text");
            const auto path = util::qa::pdf_path(run_dir, "lightdata", 1, "trigger_matrix");
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);
        }
        // 02 Timing chip-0 vs chip-1 alignment — health of the timing reference.
        //  C6 addendum: skip when no timing data was seen — an empty PDF
        //  reads as "broken card" on the dashboard.  Matched against the
        //  same `timing_data_seen` flag that gates the Timing/ ROOT
        //  directory above.
        if (timing_data_seen)
        {
            save_one(2, "timing_alignment", h_timing_ref_delta.get(), "hist");
            // 02 Timing chip-coincidence map: # channels firing on chip 0 vs
            //    chip 1 per frame.  The timing trigger accepts only the
            //    (alive_chip0, alive_chip1) corner — this map shows how close
            //    the data sits to that corner (and where the dead channel pulls
            //    chip 1 down).
            //
            //  Bespoke save: scan for the highest-occupancy cell EXCLUDING the
            //  (0,0) no-coincidence corner (which always dominates), drop a
            //  marker on it, and label it "chip 0 = N ch / chip 1 = M ch".
            //  That cell is the working point the timing trigger should be
            //  configured for — i.e. the value to put in [readout.timing]
            //  alive_channels.
            {
                TH2 *hm = h_timing_hit_map.get();
                int best_ix = 0, best_iy = 0;
                double best_c = -1.0;
                for (int ix = 1; ix <= hm->GetNbinsX(); ++ix)
                    for (int iy = 1; iy <= hm->GetNbinsY(); ++iy)
                    {
                        if (ix == 1 && iy == 1)
                            continue; // skip the (0,0) bin
                        const double c = hm->GetBinContent(ix, iy);
                        if (c > best_c)
                        {
                            best_c = c;
                            best_ix = ix;
                            best_iy = iy;
                        }
                    }
                const double bx = hm->GetXaxis()->GetBinCenter(best_ix);
                const double by = hm->GetYaxis()->GetBinCenter(best_iy);
                const int best_n0 = static_cast<int>(bx); // bin centre .5 → channel count
                const int best_n1 = static_cast<int>(by);

                TCanvas c("c_qa_lightdata_02_timing_hit_map", "", 1000, 1000);
                c.SetRightMargin(0.14);
                hm->Draw("colz");
                TMarker mk(bx, by, 29 /*filled star*/);
                mk.SetMarkerColor(kRed + 1);
                mk.SetMarkerSize(2.4);
                if (best_c > 0)
                    mk.Draw();
                TLatex txt;
                txt.SetNDC();
                txt.SetTextSize(0.030);
                txt.SetTextColor(kRed + 1);
                if (best_c > 0)
                    txt.DrawLatex(0.16, 0.92,
                                  TString::Format("best: chip 0 = %d ch,  chip 1 = %d ch  (%.0f frames)",
                                                  best_n0, best_n1, best_c));
                const auto path = util::qa::pdf_path(run_dir, "lightdata", 2, "timing_hit_map");
                c.SaveAs(path.string().c_str());
                util::qa::crop_pdf_inplace(path);
            }
        }
        // 03 Single-pixel DCR hitmap — surfaces hot / dead channels.
        save_one(3, "dcr_hitmap", h_dcr_hitmap.get(), "colz");
        //  Timing-sensor average DCR (device 200, chips 0/1) — per-channel
        //  bars + horizontal average lines: total (green, full range), chip0
        //  (blue, bins 0-31), chip1 (red, bins 32-63).
        {
            TCanvas c("c_qa_lightdata_07_timing_dcr_per_channel", "",
                      1000, 1000);
            c.SetLogy(); // DCR spans orders of magnitude across channels
            h_timing_dcr_per_channel->SetStats(0);
            h_timing_dcr_per_channel->Draw("hist");
            TLine l_tot(0, timing_dcr_mean_khz, 64, timing_dcr_mean_khz);
            l_tot.SetLineColor(kGreen + 2);
            l_tot.SetLineWidth(2);
            l_tot.Draw();
            TLine l0(0, timing_dcr_chip0_khz, 32, timing_dcr_chip0_khz);
            l0.SetLineColor(kAzure + 1);
            l0.SetLineStyle(2);
            l0.Draw();
            TLine l1(32, timing_dcr_chip1_khz, 64, timing_dcr_chip1_khz);
            l1.SetLineColor(kRed + 1);
            l1.SetLineStyle(2);
            l1.Draw();
            const auto path = util::qa::pdf_path(
                run_dir, "lightdata", 7, "timing_dcr_per_channel");
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);
        }
        // 06 Per-channel DCR rate (kHz) + averages over all channels,
        //    per device (rdo), and per sensor.  Surfaced for the
        //    General overview's Sensor-health row.  Numbered 06 so it
        //    doesn't collide with the per-trigger fan-out (now 07+).
        //    Device for channel ordinal o is 192 + o/256 (the
        //    GlobalIndex channel-ordinal layout); sensor comes from the
        //    readout config's per-device sensor_type (e.g. 1350 / Quetta).
        {
            TProfile *dcr = h_dcr_per_channel.get();
            //  device → sensor from the readout config (const view).
            //  Uses the per-device sensor_for() so a single cherenkov
            //  role spanning two SiPM models (1350 / 1375) maps each
            //  device to its own sensor.
            std::map<int, std::string> device_to_sensor;
            for (const auto &cfg : framer.get_readout_config().all())
                for (const auto &dc : cfg.device_chip)
                {
                    const std::string sen = cfg.sensor_for(dc.first);
                    if (!sen.empty())
                        device_to_sensor[dc.first] = sen;
                }

            //  Channels per device follows the GlobalIndex layout
            //  (256 under the split-in-two packing, 512 otherwise) so
            //  the per-device grouping stays correct if the detector
            //  flips ``gidx::kUsesSplitInTwo``.
            constexpr int kChansPerDevice = gidx::kUsesSplitInTwo ? 256 : 512;
            double all_sum = 0.0;
            long all_n = 0;
            std::map<int, std::pair<double, long>> by_device;
            std::map<std::string, std::pair<double, long>> by_sensor;
            for (int b = 1; b <= dcr->GetNbinsX(); ++b)
            {
                if (dcr->GetBinEntries(b) <= 0)
                    continue; // dead / absent channel — excluded
                const double rate = dcr->GetBinContent(b);
                const int ordinal = b - 1;
                const int device = ::gidx::kFirstDevice + ordinal / kChansPerDevice;
                all_sum += rate;
                ++all_n;
                auto &d = by_device[device];
                d.first += rate;
                ++d.second;
                const auto sit = device_to_sensor.find(device);
                const std::string sensor =
                    (sit != device_to_sensor.end()) ? sit->second : "?";
                auto &s = by_sensor[sensor];
                s.first += rate;
                ++s.second;
            }

            TCanvas c("c_qa_lightdata_06_dcr_per_channel", "", 1400, 900);
            c.SetLogy(); // DCR spans orders of magnitude across channels
            c.SetLeftMargin(0.10);
            c.SetRightMargin(0.04);
            c.SetBottomMargin(0.12);
            dcr->Draw("hist");
            TLatex txt;
            txt.SetNDC();
            txt.SetTextFont(42);
            txt.SetTextSize(0.022);
            txt.SetTextColor(kBlack);
            //  Two-column tabular layout (label left, value right at a fixed
            //  offset so the rates align as a table).  Left column = overall
            //  + per-sensor averages; right column = per-device averages.
            //  NDC-anchored in the upper band so the table position is
            //  independent of the (log) Y scale.
            constexpr double kColAlbl = 0.13, kColAval = 0.27; // left column
            constexpr double kColBlbl = 0.46, kColBval = 0.60; // right column
            constexpr double kRowDy = 0.030;
            const double y_top = 0.88;
            TLatex hdr;
            hdr.SetNDC();
            hdr.SetTextFont(62); // bold for the category headers
            hdr.SetTextSize(0.022);

            //  ── Column A: all + per-sensor ──
            double ya = y_top;
            hdr.DrawLatex(kColAlbl, ya, "avg DCR (all)");
            txt.DrawLatex(kColAval, ya, TString::Format("%.2f kHz", all_n ? all_sum / all_n : 0.0));
            ya -= kRowDy * 1.5;
            hdr.DrawLatex(kColAlbl, ya, "per sensor");
            ya -= kRowDy;
            for (const auto &[sen, sn] : by_sensor)
            {
                txt.DrawLatex(kColAlbl + 0.02, ya, sen.c_str());
                txt.DrawLatex(kColAval, ya, TString::Format("%.2f kHz", sn.second ? sn.first / sn.second : 0.0));
                ya -= kRowDy;
            }

            //  ── Column B: per-device ──
            double yb = y_top;
            hdr.DrawLatex(kColBlbl, yb, "per device");
            yb -= kRowDy;
            for (const auto &[dev, sn] : by_device)
            {
                txt.DrawLatex(kColBlbl + 0.02, yb, TString::Format("rdo-%d", dev));
                txt.DrawLatex(kColBval, yb, TString::Format("%.2f kHz", sn.second ? sn.first / sn.second : 0.0));
                yb -= kRowDy;
            }
            const auto path = util::qa::pdf_path(
                run_dir, "lightdata", 6, "dcr_per_channel");
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);
        }
        // 06 Per-channel afterpulse probability (DCR-subtracted, %) +
        //    averages over all channels / per device / per sensor — same
        //    treatment as the DCR per-channel plot above.  Linear Y (the
        //    subtracted value can dip negative on low-stats channels, so
        //    log won't take it).  Surfaced in the Sensor-health row between
        //    the two hitmaps.
        {
            TProfile *ap = h_afterpulse_per_channel.get();
            std::map<int, std::string> device_to_sensor;
            for (const auto &cfg : framer.get_readout_config().all())
                for (const auto &dc : cfg.device_chip)
                {
                    const std::string sen = cfg.sensor_for(dc.first);
                    if (!sen.empty())
                        device_to_sensor[dc.first] = sen;
                }
            constexpr int kChansPerDevice = gidx::kUsesSplitInTwo ? 256 : 512;
            double all_sum = 0.0;
            long all_n = 0;
            std::map<int, std::pair<double, long>> by_device;
            std::map<std::string, std::pair<double, long>> by_sensor;
            for (int b = 1; b <= ap->GetNbinsX(); ++b)
            {
                if (ap->GetBinEntries(b) <= 0)
                    continue; // dead / absent channel — excluded
                const double val = ap->GetBinContent(b);
                const int ordinal = b - 1;
                const int device = ::gidx::kFirstDevice + ordinal / kChansPerDevice;
                all_sum += val;
                ++all_n;
                auto &d = by_device[device];
                d.first += val;
                ++d.second;
                const auto sit = device_to_sensor.find(device);
                const std::string sensor =
                    (sit != device_to_sensor.end()) ? sit->second : "?";
                auto &s = by_sensor[sensor];
                s.first += val;
                ++s.second;
            }

            TCanvas c("c_qa_lightdata_06_afterpulse_per_channel", "", 1400, 900);
            c.SetLeftMargin(0.10);
            c.SetRightMargin(0.04);
            c.SetBottomMargin(0.12);
            ap->Draw("hist");
            TLatex txt;
            txt.SetNDC();
            txt.SetTextFont(42);
            txt.SetTextSize(0.022);
            txt.SetTextColor(kBlack);
            constexpr double kColAlbl = 0.13, kColAval = 0.27; // left column
            constexpr double kColBlbl = 0.46, kColBval = 0.60; // right column
            constexpr double kRowDy = 0.030;
            const double y_top = 0.88;
            TLatex hdr;
            hdr.SetNDC();
            hdr.SetTextFont(62);
            hdr.SetTextSize(0.022);

            //  ── Column A: all + per-sensor ──
            double ya = y_top;
            hdr.DrawLatex(kColAlbl, ya, "avg AP (all)");
            txt.DrawLatex(kColAval, ya, TString::Format("%.2f %%", all_n ? all_sum / all_n : 0.0));
            ya -= kRowDy * 1.5;
            hdr.DrawLatex(kColAlbl, ya, "per sensor");
            ya -= kRowDy;
            for (const auto &[sen, sn] : by_sensor)
            {
                txt.DrawLatex(kColAlbl + 0.02, ya, sen.c_str());
                txt.DrawLatex(kColAval, ya, TString::Format("%.2f %%", sn.second ? sn.first / sn.second : 0.0));
                ya -= kRowDy;
            }

            //  ── Column B: per-device ──
            double yb = y_top;
            hdr.DrawLatex(kColBlbl, yb, "per device");
            yb -= kRowDy;
            for (const auto &[dev, sn] : by_device)
            {
                txt.DrawLatex(kColBlbl + 0.02, yb, TString::Format("rdo-%d", dev));
                txt.DrawLatex(kColBval, yb, TString::Format("%.2f %%", sn.second ? sn.first / sn.second : 0.0));
                yb -= kRowDy;
            }
            const auto path = util::qa::pdf_path(
                run_dir, "lightdata", 6, "afterpulse_per_channel");
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);
        }
        // 04 Afterpulse hitmap (sideband-subtracted) — diagnostic for AP
        //    cleaning.  The map is a sideband subtraction: near-window
        //    hits fill +100, the DCR far-sideband fills −100, so a bin
        //    holds 100·(N_near − N_far).  In low-stats bins the far
        //    sideband can statistically exceed the near window, driving
        //    the bin NEGATIVE — that's an estimator fluctuation, not a
        //    physical negative afterpulse rate (which can't exist).  We
        //    clamp those to 0 FOR DISPLAY ONLY: a non-destructive clone
        //    has its negative bins zeroed and its colour floor pinned at
        //    0, so the QA plot never shows the unphysical negative band.
        //    The RAW subtracted hist is still written to the ROOT file
        //    and still drives the n_afterpulse_hits integral below
        //    (clamping there would bias the afterpulse count high).
        {
            std::unique_ptr<TH2F> ap_disp(
                static_cast<TH2F *>(h_afterpulse_hitmap->Clone(
                    "h_afterpulse_hitmap_display")));
            ap_disp->SetDirectory(nullptr);
            const int n_cells = ap_disp->GetNcells();
            for (int b = 0; b < n_cells; ++b)
                if (ap_disp->GetBinContent(b) < 0.0)
                    ap_disp->SetBinContent(b, 0.0);
            ap_disp->SetMinimum(0.0); // colour floor at 0 too
            save_one(4, "afterpulse_hitmap", ap_disp.get(), "colz");
        }

        // 05 Streaming-trigger score: bkg (noise, red) vs sig (data, blue)
        //    + recommendation overlay (vertical line at FP = 1e-6
        //    target, annotated with noise/data tail integrals and
        //    S/N at the line).
        //    — the plot the analyser reads to pick the production
        //    ``n_sigma_threshold``.  In QA mode (streaming disabled by
        //    ``conf/QA/streaming.toml``) both curves still fill, so the
        //    analyser sees the full distribution without paying for
        //    the RANSAC cascade.  In production (streaming firing), the
        //    plot is the post-hoc audit of whether the threshold the
        //    operator picked is where they wanted it.  The
        //    recommendation is a STARTING POINT — the shifter looks at
        //    the noise/signal tails, the S/N at the line, and the
        //    expected misfire count, and decides where to actually
        //    place the production threshold.
        {
            auto *h_noise = h_streaming_score_noise.get();
            auto *h_data = h_streaming_score_data.get();
            auto *h_inbeam = h_streaming_score_inbeam.get();
            const bool have_inbeam =
                h_inbeam && h_inbeam->GetEntries() > 0;
            if (h_noise && h_data)
            {
                TCanvas c("c_qa_lightdata_05_streaming_score",
                          "Streaming-trigger score: noise vs data",
                          1000, 1000);
                c.SetLogy();
                //  Log X too: the score hists now span [0.1,1000] n_σ
                //  on 200 log-spaced bins and the interesting structure —
                //  the noise bump near a few n_σ and the signal bump out
                //  past 40 — is bunched at low n_σ on a linear axis.
                //  Displayed range pinned to [0.1,1000] matching the bin
                //  edges; the log-spaced binning has no n_σ≈0 turn-on bin
                //  to hide, so the full support renders on the log-x axis.
                c.SetLogx();
                //  Sweep audit: give the X-axis title room
                //  to render — operator screenshot showed the
                //  "n_{#sigma} (first-frames sample)" label cropped at
                //  the bottom edge.  Y axis margin nudged similarly so
                //  the "probability per bin" label clears the pad edge.
                c.SetBottomMargin(0.13);
                c.SetLeftMargin(0.13);
                //  Log Y so the tails of both distributions remain
                //  readable — the separation between noise and data at
                //  the tail is exactly where the threshold sits.  Y
                //  range pinned to the actual smallest-positive bin
                //  content so the recommendation line spans the full
                //  visible pad regardless of how the histograms are
                //  normalised (integer counts at low statistics vs
                //  probability-per-bin once the score gets renormalised).
                double max_content = std::max(h_noise->GetMaximum(),
                                              h_data->GetMaximum());
                if (have_inbeam)
                    max_content = std::max(max_content, h_inbeam->GetMaximum());
                const double min_pos_noise = h_noise->GetMinimum(1e-300);
                const double min_pos_data = h_data->GetMinimum(1e-300);
                const double min_pos = std::min(min_pos_noise, min_pos_data);
                const double y_min = (min_pos > 0. && min_pos < max_content)
                                         ? 0.3 * min_pos
                                         : 1e-3 * std::max(1.0, max_content);
                const double y_max = 1.5 * max_content;
                h_noise->SetMinimum(y_min);
                h_noise->SetMaximum(y_max);
                //  Displayed X range [0.1,1000] n_σ (log-x; matches the
                //  log-spaced bin edges, no turn-on bin to hide).
                h_noise->GetXaxis()->SetRangeUser(0.1, 1000.0);
                h_noise->Draw("HIST");
                h_data->Draw("HIST SAME");
                //  In-beam (pre-trigger) sample overlaid in violet.
                if (have_inbeam)
                    h_inbeam->Draw("HIST SAME");

                //  Component-fit block (GATED OFF — see kEnableScoreFit below).
                //  Models the score curves as a MIXED 3-population overlay:
                //    DCR / null      = 1 LOG-Gaussian  [0..2] — dark counts are
                //                      log-normal in n_σ: A·exp(−½·((log10 x −
                //                      m)/s)²), bump at n_σ = 10^m.
                //    in-beam, signal = LINEAR Gaussians [3..5], [6..8] — Poisson
                //                      photon-count populations, Gaussian in
                //                      LINEAR n_σ: A·exp(−½·((x − μ)/σ)²), bump
                //                      at n_σ = μ; the signal runs broad (σ ≳ μ)
                //                      as the pedestal under the whole tail.
                //  Cascade: fit DCR on the noise sample (focused [0.3,30]
                //  window) → carry its SHAPE (mean+width) FIXED, norm ±10%, into
                //  the in-beam fit (+ one free linear bump) → carry both shapes
                //  fixed (in-beam norm FREE) into the data fit (+ one free broad
                //  signal bump).  Bump means are data-driven (tail local maxima)
                //  and floored ≫ the population below.  Full rationale + why this
                //  is parked: include/triggers/streaming/DISCUSSION.md § 1.4.1.
                //
                //  TF1s are stack-allocated at this scope so they outlive the
                //  SaveAs below (no ->Write(); the only consumer is the PDF
                //  render).  Drawn "SAME" over the (already-drawn) HIST curves;
                //  fit "RQ0".  Fit range = full log-spaced support [0.1,1000].
                constexpr double kFitLo = 0.1;
                constexpr double kFitHi = 1000.0;
                //  Per-component single Gaussians drawn (dotted, same colour as
                //  the parent curve) on top of each summed fit so the operator
                //  can SEE the DCR (log-Gaussian) + in-beam/signal (linear-
                //  Gaussian) sub-components.
                //  Held here, reserved for the worst case (1+2+3 = 6), so
                //  every component outlives the SaveAs at the end of the
                //  block — a TF1 destroyed before the canvas write vanishes
                //  from the PDF.  Reserve up front so no reallocation
                //  invalidates an already-Draw()n component's address.
                std::vector<TF1> score_components;
                score_components.reserve(6);
                //  Fitted noise (DCR) peak amplitude — the downstream DCR term's
                //  normalisation is leashed to ±10% of this.  NaN until the
                //  noise fit lands.
                double a_noise =
                    std::numeric_limits<double>::quiet_NaN();
                //  Bump width band (log10 units) shared by the in-beam and
                //  signal log-Gaussians: wide enough for the broad signal bump,
                //  floored so a term can't collapse into a delta-spike (s→0).
                //  The DCR term's own width is fit on the clean noise sample
                //  (stage 1) and then held FIXED in the downstream fits.
                constexpr double kWidthSeed = 0.25;
                constexpr double kWidthLo = 0.10;
                constexpr double kWidthHi = 0.55;
                //  Fitted log10-means surfaced to the annotation block as
                //  n_σ = 10^m.  NaN / false means "fit unavailable" →
                //  annotation skipped (no NaN displayed, no crash).
                double noise_m0 = std::numeric_limits<double>::quiet_NaN();
                //  Fitted noise WIDTH (log10 units) — the DCR component is
                //  pinned wholesale (mean + width) into the downstream in-beam
                //  / data fits; only its normalisation floats there.  NaN until
                //  the noise fit lands.
                double noise_s0 = std::numeric_limits<double>::quiet_NaN();
                double inbeam_m1 = std::numeric_limits<double>::quiet_NaN();
                double inbeam_m2 = std::numeric_limits<double>::quiet_NaN();
                double data_m1 = std::numeric_limits<double>::quiet_NaN();
                double data_m2 = std::numeric_limits<double>::quiet_NaN();
                double data_m3 = std::numeric_limits<double>::quiet_NaN();
                double sig_m = std::numeric_limits<double>::quiet_NaN();
                double sig_s = std::numeric_limits<double>::quiet_NaN();
                double sig_loss_frac =
                    std::numeric_limits<double>::quiet_NaN();
                bool have_noise_fit = false;
                bool have_inbeam_fit = false;
                bool have_data_fit = false;

                //  TFormula strings (serialisation-safe) summing
                //  A_i·exp(-0.5·((log10(x)−m_i)/s_i)^2) terms.  Param order
                //  per term: [A, m, s].
                //  MIXED model (physically motivated):
                //    DCR / null     = LOG-Gaussian   [0..2]: random dark counts
                //                     are log-normal in n_σ (Gaussian in
                //                     log10 x); m,s are in log10 space, bump at
                //                     n_σ = 10^m.
                //    in-beam, signal = LINEAR Gaussians [3..5], [6..8]: these
                //                     are Poisson photon-count populations, so
                //                     Gaussian in LINEAR n_σ; μ,σ are in n_σ
                //                     directly (bump at n_σ = μ).  The signal
                //                     term runs broad (σ ≳ μ) and acts as the
                //                     pedestal under the whole high-n_σ tail.
                //  DCR kernel uses log10(x) − log10([1]) (NOT log10(x) − [1]):
                //  this makes the DCR mean param [1] read DIRECTLY as the peak
                //  position in n_σ (peak where x = [1]), instead of [1] being a
                //  log you must exponentiate.  ([2] stays a log10-space width =
                //  decades.)  The in-beam/signal terms are LINEAR Gaussians, so
                //  their means [4]/[7] are already n_σ directly.
                TF1 f_noise("f_qa_noise",
                            "[0]*exp(-0.5*((log10(x)-log10([1]))/[2])^2)",
                            kFitLo, kFitHi);
                TF1 f_inbeam("f_qa_inbeam",
                             "[0]*exp(-0.5*((log10(x)-log10([1]))/[2])^2) + "
                             "[3]*exp(-0.5*((x-[4])/[5])^2)",
                             kFitLo, kFitHi);
                TF1 f_data("f_qa_data",
                           "[0]*exp(-0.5*((log10(x)-log10([1]))/[2])^2) + "
                           "[3]*exp(-0.5*((x-[4])/[5])^2) + "
                           "[6]*exp(-0.5*((x-[7])/[8])^2)",
                           kFitLo, kFitHi);

                //  ── Score component-fit: GATED OFF ─────────────────────────
                //  The 3-population overlay (DCR log-Gaussian + in-beam & signal
                //  LINEAR Gaussians) is preserved but DISABLED: with the flag
                //  false it neither runs nor draws anything.  The model is not
                //  yet robust enough to ship — it does not converge onto the
                //  three populations reliably across runs/normalisations (full
                //  writeup: include/triggers/streaming/DISCUSSION.md, "Streaming-
                //  score overlay fit").  The recommendation cut line further
                //  down is independent of this block and still drawn.  Flip to
                //  true only for offline experimentation.
                constexpr bool kEnableScoreFit = false;

                //  In-beam bump width — carried from stage 2 into stage 3's
                //  FIXED in-beam shape.  NaN until the in-beam lands.
                [[maybe_unused]] double inbeam_s2 =
                    std::numeric_limits<double>::quiet_NaN();

                //  Locate the most prominent log-normal bump (local maximum of
                //  the lightly-smoothed hist) at n_σ ≥ floor_nsig; returns its
                //  log10(n_σ), or `fallback_log10` if none is found.  Smoothing
                //  tames per-bin noise without erasing the ~0.3-decade bumps;
                //  the local-max test (taller than the bins ±W away on BOTH
                //  sides) ignores the monotone DCR tail and keys on genuine
                //  turning points regardless of absolute height — so the data-
                //  driven seed lands on the real bump, not the DCR shoulder.
                [[maybe_unused]] auto find_bump_log10 =
                    [](TH1F *h, double floor_nsig, double fallback_log10) -> double
                {
                    if (!h || h->GetEntries() <= 0)
                        return fallback_log10;
                    std::unique_ptr<TH1F> sm(static_cast<TH1F *>(
                        h->Clone(TString::Format("%s_qa_bump_smooth", h->GetName()))));
                    sm->SetDirectory(nullptr);
                    sm->Smooth(3);
                    const int nb = sm->GetNbinsX();
                    const int first =
                        std::max(1, sm->GetXaxis()->FindBin(floor_nsig));
                    constexpr int W = 8; // ≈0.16 decade at 50 bins/decade
                    double best_c = 0.0;
                    int best_b = -1;
                    for (int b = first; b <= nb; ++b)
                    {
                        const double c = sm->GetBinContent(b);
                        if (c <= 0.0)
                            continue;
                        const double lft = sm->GetBinContent(std::max(1, b - W));
                        const double rgt = sm->GetBinContent(std::min(nb, b + W));
                        if (c > lft && c > rgt && c > best_c)
                        {
                            best_c = c;
                            best_b = b;
                        }
                    }
                    if (best_b < 0)
                        return fallback_log10;
                    return std::log10(sm->GetXaxis()->GetBinCenter(best_b));
                };

                //  --- stage 1: DCR on the clean noise sample (1 log-Gaussian).
                //      Fixes the DCR SHAPE (mean+width) reused downstream. ---
                if (kEnableScoreFit && h_noise->GetEntries() > 0)
                {
                    const double m0_seed = 2.0; // DCR peak, n_σ (linear)
                    const double a0_seed = std::max(
                        h_noise->GetBinContent(h_noise->FindBin(m0_seed)),
                        1e-12);
                    f_noise.SetParameters(a0_seed, m0_seed, 0.28);
                    f_noise.SetParLimits(0, 0.0, 1e12);
                    f_noise.SetParLimits(1, 1.0, 10.0);  // DCR mean, n_σ (linear)
                    f_noise.SetParLimits(2, 0.12, 0.60); // DCR width (log10 decades)
                    //  Fit the DCR on a window around its bump — [0.3,30] n_σ —
                    //  NOT the full [0.1,1000] support.  The noise sample carries
                    //  a sparse high-n_σ tail that, if included, pulls a single
                    //  log-Gaussian off the bump (mean drifts low, width wrong),
                    //  which then poisons the whole cascade (the in-beam term
                    //  rushes to fill the resulting shoulder).  This window lands
                    //  on the known-good DCR shape (≈0.24 mean, ≈0.28 width on
                    //  this detector); both sit inside the par limits above.
                    //  Accept on finite params rather than a strict MINUIT
                    //  status==0 — these fits routinely return status 3
                    //  (HESSE not pos-def / param near a limit) while
                    //  yielding usable means.  A NaN / out-of-range mean
                    //  (genuine failure) still bails out.
                    h_noise->Fit(&f_noise, "RQ0", "", 0.3, 30.0);
                    const double m0 = f_noise.GetParameter(1);
                    if (std::isfinite(m0))
                    {
                        noise_m0 = m0; // DCR peak in n_σ (linear) — [1] reads direct
                        //  Fitted DCR width — pinned (with the mean) into the
                        //  downstream fits' first term.
                        noise_s0 = f_noise.GetParameter(2);
                        //  Fitted DCR amplitude — the downstream DCR term's norm
                        //  is leashed to ±10% of this.
                        a_noise = f_noise.GetParameter(0);
                        have_noise_fit = true;
                        f_noise.SetLineColor(kBlue);
                        f_noise.SetLineStyle(2);
                        f_noise.SetLineWidth(2);
                        f_noise.Draw("SAME");
                        //  Single-component overlay (noise has 1 term, so
                        //  this duplicates the summed fit, but drawing it
                        //  dotted keeps the visual scheme uniform across
                        //  the three curves).
                        score_components.emplace_back(
                            "f_qa_noise_comp0",
                            "[0]*exp(-0.5*((log10(x)-log10([1]))/[2])^2)",
                            kFitLo, kFitHi);
                        TF1 &comp = score_components.back();
                        comp.SetParameters(f_noise.GetParameter(0),
                                           f_noise.GetParameter(1),
                                           f_noise.GetParameter(2));
                        comp.SetLineColor(kBlue);
                        comp.SetLineStyle(3);
                        comp.SetLineWidth(1);
                        comp.Draw("SAME");
                    }
                }

                //  --- stage 2: in-beam = DCR(shape FIXED, norm ±10%) + one
                //      FREE bump seeded on the in-beam tail's local maximum. ---
                if (kEnableScoreFit && have_inbeam && h_inbeam->GetEntries() > 0)
                {
                    //  Tail floor: 3 DCR widths above the DCR mean — above this
                    //  the DCR is spent, so any bump is the in-beam population.
                    //  noise_m0 is now the DCR mean in linear n_σ, noise_s0 a
                    //  log10 width, so the floor is mean × 10^(3·width).
                    const double floor_ns = have_noise_fit
                                                ? noise_m0 * std::pow(10.0, 3.0 * noise_s0)
                                                : 10.0;
                    const double m1_seed = have_noise_fit ? noise_m0 : 2.0;
                    //  Data-driven seed for the in-beam bump (local maximum of
                    //  the smoothed tail), with a sensible fallback.
                    const double m2_seed = find_bump_log10(
                        h_inbeam, floor_ns,
                        have_noise_fit ? noise_m0 + 1.0 : 1.4);
                    //  Floor the in-beam bump a full decade (×10) above the DCR
                    //  mean — the DCR→in-beam separation is ~20× on this
                    //  detector, so a 10× floor keeps the free term off the
                    //  low DCR shoulder (where it otherwise rails to a few n_σ)
                    //  while still leaving it free to land on the real bump.
                    const double m2_lo = have_noise_fit ? noise_m0 + 1.0 : 1.2;
                    const double a1_seed =
                        (have_noise_fit && std::isfinite(a_noise) && a_noise > 0.0)
                            ? a_noise
                            : 1e-3;
                    const double a2_seed = std::max(
                        h_inbeam->GetBinContent(
                            h_inbeam->FindBin(std::pow(10.0, m2_seed))),
                        1e-12);
                    f_inbeam.SetParameters(a1_seed, m1_seed, kWidthSeed,
                                           a2_seed, m2_seed, kWidthSeed);
                    //  Term 1 = DCR: SHAPE (mean+width) FIXED to the noise fit;
                    //  normalisation free only within ±10% of the noise norm
                    //  (the dark-count rate barely moves between samples).  Fall
                    //  back to a free first term if the noise stage failed.
                    if (have_noise_fit)
                    {
                        f_inbeam.FixParameter(1, noise_m0);
                        f_inbeam.FixParameter(2, noise_s0);
                        f_inbeam.SetParLimits(0, 0.9 * a_noise, 1.1 * a_noise);
                    }
                    else
                    {
                        f_inbeam.SetParLimits(0, 0.0, 1e12);
                        f_inbeam.SetParLimits(1, 0.0, 1.0);
                        f_inbeam.SetParLimits(2, 0.12, 0.60);
                    }
                    //  Term 2 = in-beam bump: fully free above the DCR.
                    f_inbeam.SetParLimits(3, 0.0, 1e12);
                    f_inbeam.SetParLimits(4, m2_lo, 3.0);
                    f_inbeam.SetParLimits(5, kWidthLo, kWidthHi);
                    h_inbeam->Fit(&f_inbeam, "RQ0");
                    const double m1 = f_inbeam.GetParameter(1);
                    const double m2 = f_inbeam.GetParameter(4);
                    if (std::isfinite(m1) && std::isfinite(m2))
                    {
                        inbeam_m1 = m1;
                        inbeam_m2 = m2;
                        inbeam_s2 = f_inbeam.GetParameter(5);
                        have_inbeam_fit = true;
                        f_inbeam.SetLineColor(kViolet);
                        f_inbeam.SetLineStyle(2);
                        f_inbeam.SetLineWidth(2);
                        f_inbeam.Draw("SAME");
                        //  Per-component overlay: the in-beam fit is a SUM of
                        //  2 log-Gaussians but the summed curve alone hides
                        //  that structure.  Draw each term (params [0..2] and
                        //  [3..5]) as a dotted single log-Gaussian in the same
                        //  violet so the operator can SEE the two bumps.  Held
                        //  in `score_components` (reserved up front) so each
                        //  outlives the SaveAs below.
                        for (int t = 0; t < 2; ++t)
                        {
                            const int p = 3 * t; // term param base
                            score_components.emplace_back(
                                TString::Format("f_qa_inbeam_comp%d", t),
                                "[0]*exp(-0.5*((log10(x)-[1])/[2])^2)",
                                kFitLo, kFitHi);
                            TF1 &comp = score_components.back();
                            comp.SetParameters(f_inbeam.GetParameter(p),
                                               f_inbeam.GetParameter(p + 1),
                                               f_inbeam.GetParameter(p + 2));
                            comp.SetLineColor(kViolet);
                            comp.SetLineStyle(3);
                            comp.SetLineWidth(1);
                            comp.Draw("SAME");
                        }
                    }
                }

                //  --- stage 3: data = DCR(shape FIXED, norm ±10%) + in-beam
                //      (shape FIXED, norm FREE) + one FREE signal bump seeded on
                //      the data tail's local maximum above the in-beam.  The
                //      in-beam norm floats here because its fraction differs
                //      between the pre-trigger window and the full spill, while
                //      its SHAPE (position+width in n_σ) is universal. ---
                if (kEnableScoreFit && h_data->GetEntries() > 0)
                {
                    const double m1_seed = have_noise_fit ? noise_m0 : 0.4;
                    const double m2_seed = have_inbeam_fit ? inbeam_m2 : 1.4;
                    const double s2_fix =
                        std::isfinite(inbeam_s2) ? inbeam_s2 : kWidthSeed;
                    //  Signal floor: above the in-beam bump (2 in-beam widths
                    //  up) so the finder keys on the signal bump, not the
                    //  in-beam one.
                    const double sig_floor_ns = have_inbeam_fit
                                                    ? std::pow(10.0, inbeam_m2 + 2.0 * s2_fix)
                                                    : 60.0;
                    const double m3_seed = find_bump_log10(
                        h_data, sig_floor_ns,
                        have_inbeam_fit ? inbeam_m2 + 0.6 : 1.8);
                    const double m3_lo =
                        have_inbeam_fit ? inbeam_m2 + 0.3 : 1.3;
                    const double a1_seed =
                        (have_noise_fit && std::isfinite(a_noise) && a_noise > 0.0)
                            ? a_noise
                            : 1e-3;
                    const double a2_seed = std::max(
                        h_data->GetBinContent(
                            h_data->FindBin(std::pow(10.0, m2_seed))),
                        1e-12);
                    const double a3_seed = std::max(
                        h_data->GetBinContent(
                            h_data->FindBin(std::pow(10.0, m3_seed))),
                        1e-12);
                    f_data.SetParameters(a1_seed, m1_seed, kWidthSeed,
                                         a2_seed, m2_seed, s2_fix,
                                         a3_seed, m3_seed, kWidthSeed);
                    //  Term 1 = DCR: SHAPE FIXED, norm ±10% of the noise norm.
                    if (have_noise_fit)
                    {
                        f_data.FixParameter(1, noise_m0);
                        f_data.FixParameter(2, noise_s0);
                        f_data.SetParLimits(0, 0.9 * a_noise, 1.1 * a_noise);
                    }
                    else
                    {
                        f_data.SetParLimits(0, 0.0, 1e12);
                        f_data.SetParLimits(1, 0.0, 1.0);
                        f_data.SetParLimits(2, 0.12, 0.60);
                    }
                    //  Term 2 = in-beam: SHAPE (mean+width) FIXED from stage 2,
                    //  normalisation FREE (its fraction differs from the in-beam
                    //  sample).
                    f_data.SetParLimits(3, 0.0, 1e12);
                    if (have_inbeam_fit)
                    {
                        f_data.FixParameter(4, inbeam_m2);
                        f_data.FixParameter(5, s2_fix);
                    }
                    else
                    {
                        f_data.SetParLimits(4, 0.0, 3.0);
                        f_data.SetParLimits(5, kWidthLo, kWidthHi);
                    }
                    //  Term 3 = signal: fully free above the in-beam.
                    f_data.SetParLimits(6, 0.0, 1e12);
                    f_data.SetParLimits(7, m3_lo, 3.0);
                    f_data.SetParLimits(8, kWidthLo, kWidthHi);
                    h_data->Fit(&f_data, "RQ0");
                    const double m1 = f_data.GetParameter(1);
                    const double m2 = f_data.GetParameter(4);
                    const double m3 = f_data.GetParameter(7);
                    const double s3 = f_data.GetParameter(8);
                    if (std::isfinite(m1) && std::isfinite(m2) &&
                        std::isfinite(m3) && std::isfinite(s3))
                    {
                        data_m1 = m1;
                        data_m2 = m2;
                        data_m3 = m3;
                        //  Signal = the THIRD (free) log-Gaussian.
                        sig_m = m3;
                        sig_s = s3;
                        have_data_fit = true;
                        f_data.SetLineColor(kRed);
                        f_data.SetLineStyle(2);
                        f_data.SetLineWidth(2);
                        f_data.Draw("SAME");
                        //  Per-component overlay: the data fit is a SUM of 3
                        //  log-Gaussians (terms [0..2], [3..5], [6..8] — the
                        //  third is the free SIGNAL bump).  Draw each as a
                        //  dotted single log-Gaussian in the same red so the
                        //  operator can SEE all three bumps, not just the
                        //  envelope.  Held in `score_components` (reserved up
                        //  front) so each outlives the SaveAs below.
                        for (int t = 0; t < 3; ++t)
                        {
                            const int p = 3 * t; // term param base
                            score_components.emplace_back(
                                TString::Format("f_qa_data_comp%d", t),
                                "[0]*exp(-0.5*((log10(x)-[1])/[2])^2)",
                                kFitLo, kFitHi);
                            TF1 &comp = score_components.back();
                            comp.SetParameters(f_data.GetParameter(p),
                                               f_data.GetParameter(p + 1),
                                               f_data.GetParameter(p + 2));
                            comp.SetLineColor(kRed);
                            comp.SetLineStyle(3);
                            comp.SetLineWidth(1);
                            comp.Draw("SAME");
                        }
                    }
                }

                //  Recommendation block — place the recommended cut where the
                //  in-beam background histogram first runs out (its first empty
                //  bin above the peak), then read the noise/data tail
                //  populations above the cut to surface the trade-off.
                //
                //  Inlined here (not in score.cxx) per design call:
                //  this is a QA-overlay-only computation, not the
                //  firing decision.  The shifter looks at the line
                //  and decides whether to ship that value as the
                //  production ``n_sigma_threshold`` (or pick a
                //  different one based on the visible S/N).
                constexpr double kTargetFpPerWindow = 1e-6;
                double rec_score = std::numeric_limits<double>::quiet_NaN();
                //  Doubles, NOT long long: the score hists are
                //  normalised to probability-per-bin, so each bin
                //  content is < 1.0.  Accumulating the tail into a
                //  long long truncated every contribution to 0 — that's
                //  why the "above the line" tallies read 0/N even when
                //  the data curve clearly has content above the cut.
                double noise_tail_count = 0.0;
                double data_tail_count = 0.0;
                double noise_total = 0.0;
                double data_total = 0.0;
                {
                    const int n_bins = h_noise->GetNbinsX();
                    //  Background the trigger must not fire on = DCR
                    //  (first-frames) + in-beam pre-trigger.  Combine in
                    //  RAW counts (un-normalise via GetEntries) so the two
                    //  populations are weighted by their true window counts,
                    //  then compute the recommended cut + FP on the union.
                    TH1F *h_bg = static_cast<TH1F *>(
                        h_noise->Clone("h_streaming_score_bg_tmp"));
                    h_bg->SetDirectory(nullptr);
                    h_bg->Scale(h_noise->GetEntries()); // → raw DCR counts
                    if (have_inbeam && h_inbeam->GetEntries() > 0)
                        h_bg->Add(h_inbeam, h_inbeam->GetEntries()); // + raw in-beam

                    for (int b = 0; b <= n_bins + 1; ++b)
                    {
                        noise_total += h_bg->GetBinContent(b);
                        data_total += h_data->GetBinContent(b);
                    }
                    if (noise_total > 0.0)
                    {
                        //  Primary: the recommended cut = the FIRST EMPTY BIN of
                        //  the in-beam background histogram, scanning rightward
                        //  from its peak (the DCR bulk).  Physics: above the
                        //  highest-n_σ in-beam-background window there is no
                        //  beam-correlated background left, so a cut placed
                        //  where the in-beam distribution first hits zero fires
                        //  only on genuine signal.  Directly interpretable and
                        //  operator-facing, and it replaces the FP-extrapolation
                        //  as the primary.  Scanning from the peak (not bin 1)
                        //  skips the sparse sub-1-n_σ region, whose low stats can
                        //  otherwise leave isolated empty bins.  ``rec_score`` is
                        //  the low edge of that first empty bin — i.e. just past
                        //  the last populated in-beam bin.
                        if (have_inbeam && h_inbeam->GetEntries() > 0)
                        {
                            const int ib_bins = h_inbeam->GetNbinsX();
                            const int ib_peak = h_inbeam->GetMaximumBin();
                            for (int b = ib_peak + 1; b <= ib_bins; ++b)
                                if (h_inbeam->GetBinContent(b) <= 0.0)
                                {
                                    rec_score =
                                        h_inbeam->GetXaxis()->GetBinLowEdge(b);
                                    break;
                                }
                        }
                        //  Fallback (no in-beam sample, or no empty bin found):
                        //  an empirical right-to-left walk on the combined
                        //  background to a FP = 1e-6 target.  Coarse — it can
                        //  pin to a far-tail outlier — but only reached when the
                        //  in-beam-derived cut is unavailable.
                        if (!std::isfinite(rec_score))
                        {
                            const double target_cumulative =
                                kTargetFpPerWindow * noise_total;
                            double cumulative = 0.0;
                            for (int b = n_bins + 1; b >= 0; --b)
                            {
                                cumulative += h_bg->GetBinContent(b);
                                if (cumulative >= target_cumulative)
                                {
                                    rec_score = (b == 0)
                                                    ? h_bg->GetXaxis()->GetXmin()
                                                    : h_bg->GetXaxis()->GetBinLowEdge(b);
                                    break;
                                }
                            }
                        }
                    }
                    //  Tail integrals above the recommended cut — bins whose
                    //  low edge ≥ rec_score are above the line.  noise_* are
                    //  now RAW combined-background counts; data_* stay as the
                    //  normalised data fractions.
                    if (std::isfinite(rec_score))
                    {
                        const int cut_bin = h_bg->GetXaxis()->FindBin(rec_score);
                        for (int b = cut_bin; b <= n_bins + 1; ++b)
                        {
                            noise_tail_count += h_bg->GetBinContent(b);
                            data_tail_count += h_data->GetBinContent(b);
                        }
                    }
                    delete h_bg;
                }

                //  CRITICAL: the histograms are filled with the
                //  already-standardised n_σ value (see
                //  score.cxx::run_streaming_trigger_weighted line ~492:
                //  `h_score_for_qa->Fill(n_sigma)`), NOT the raw score
                //  S = Σ w_i.  The X-axis variable IS the n_σ.  Hence
                //  `rec_score` extracted above is ALREADY in n_σ
                //  units — a previous "convert via the bundle" step
                //  was a spurious double-conversion that produced
                //  meaningless negative numbers when the cut landed
                //  above E[S]+σ_S.  Renamed locally to make the unit
                //  unambiguous; the variable identity is preserved.
                const double rec_n_sigma = rec_score;

                //  Cut-loss readout for the data SIGNAL log-Gaussian (the
                //  third, free term of the data fit).  Fraction below the
                //  cut c is ∫₁^c term / ∫₁^1000 term (the amplitude
                //  cancels).  Build a single-term log-Gaussian TF1 with the
                //  fitted signal mean + width and integrate 1→c over
                //  1→1000.  This is a MODEL extrapolation of the signal bump
                //  below the cut, not a direct count.
                if (have_data_fit && std::isfinite(rec_n_sigma) &&
                    rec_n_sigma > 0.0 && std::isfinite(sig_m) &&
                    std::isfinite(sig_s) && sig_s > 0.0)
                {
                    //  Signal is a LINEAR Gaussian (μ=sig_m, σ=sig_s in n_σ),
                    //  so its cut-loss kernel is linear too.
                    TF1 f_sig("f_qa_sig_g1",
                              "exp(-0.5*((x-[0])/[1])^2)",
                              1.0, 1000.0);
                    f_sig.SetParameter(0, sig_m);
                    f_sig.SetParameter(1, sig_s);
                    const double full = f_sig.Integral(1.0, 1000.0);
                    const double below = f_sig.Integral(1.0, rec_n_sigma);
                    if (std::isfinite(full) && full > 0.0 &&
                        std::isfinite(below))
                        sig_loss_frac = below / full;
                }
                //  One-line echo of every fitted mean (as n_σ = 10^m) + the
                //  loss %, gated so unavailable fits print NaN-free.
                if (have_noise_fit || have_inbeam_fit || have_data_fit)
                    mist::logger::info(TString::Format(
                                           "(lightdata_writer) 05_streaming_score fits (n_σ): "
                                           "noise=%.2f  in-beam=%.2f,%.2f  "
                                           "data=%.2f,%.2f,%.2f  signal=%.2f  "
                                           "signal lost below cut (n_sigma=%.2f) = %.1f%%",
                                           noise_m0, inbeam_m1, inbeam_m2,
                                           data_m1, data_m2, data_m3, sig_m,
                                           rec_n_sigma, 100.0 * sig_loss_frac)
                                           .Data());

                //  Vertical dashed line at the recommended cut.  Stops
                //  BELOW the annotation/legend block instead of running
                //  the full height — otherwise it cuts straight through
                //  the text.  On the log-Y axis NDC fraction f maps to
                //  y = y_min·(y_max/y_min)^f.  The annotation text runs
                //  down to NDC ≈ 0.55 (legend 0.78–0.90 sits above it), so
                //  stop the line at f = 0.52 — just under the lowest text
                //  row — so it never crosses the box whatever the cut x.
                if (std::isfinite(rec_score) && y_min > 0.0 && y_max > y_min)
                {
                    const double y_line_top =
                        y_min * std::pow(y_max / y_min, 0.52);
                    TLine *line = new TLine(
                        rec_score, y_min, rec_score, y_line_top);
                    line->SetLineColor(kBlack);
                    line->SetLineStyle(2);
                    line->SetLineWidth(2);
                    line->Draw();
                }

                //  Inline legend so the operator doesn't have to flip
                //  to the TBrowser to know which colour is which.
                //  Sweep audit: legend + annotation block
                //  shifted left by 0.15 NDC (X anchor 0.65 → 0.50;
                //  inner-indent 0.67 → 0.52) — operator screenshot
                //  showed the right column being clipped at the pad
                //  edge.  Y range / dimensions unchanged.
                TLegend leg(0.50, 0.78, 0.77, 0.90);
                leg.SetBorderSize(0);
                leg.SetFillStyle(0);
                leg.AddEntry(h_noise, "noise (first-frames window)", "l");
                leg.AddEntry(h_data, "data (post first-frames)", "l");
                if (have_inbeam)
                    leg.AddEntry(h_inbeam,
                                 "in-beam bkg (20 ns win, #minus50 ns)", "l");
                if (have_noise_fit)
                    leg.AddEntry(&f_noise, "DCR fit (log-Gauss)", "l");
                if (have_inbeam_fit)
                    leg.AddEntry(&f_inbeam, "in-beam fit (log-G + linear-G)", "l");
                if (have_data_fit)
                    leg.AddEntry(&f_data,
                                 "data fit (log-G + 2 linear-G)", "l");
                //  C6.2: stack-allocated TLine.  The legend stores a
                //  raw pointer for its swatch; the line just needs to
                //  outlive `leg.Draw()` and the canvas write below.
                //  Declared at the same scope as `leg` so lifetimes
                //  match.  Previously this leaked on every frame the
                //  block fired.
                TLine legline;
                if (std::isfinite(rec_score))
                {
                    legline.SetLineColor(kBlack);
                    legline.SetLineStyle(2);
                    legline.SetLineWidth(2);
                    leg.AddEntry(&legline,
                                 "recommended cut (in-beam bkg #rightarrow 0)", "l");
                }
                leg.Draw();

                //  Annotation block — surface the trade-off numbers.
                //  Per design call: n_σ + noise tail + data tail + S/N
                //  (all of the above).  Placed top-right under the
                //  legend so it doesn't overlap the score curves.
                //
                //  Sweep audit: the previous tail
                //  computation used `noise_total = Σ bin_content` and
                //  cast bin contents to `long long`.  If the
                //  histogram was normalised (probability per bin) the
                //  denominator collapsed to 1.0 and the per-bin
                //  contents to <1.0 → all cast to 0.  Display read
                //  "0 / 1 windows" regardless of the real entry count.
                //  Fix: compute fractions from the bin contents, then
                //  multiply by `GetEntries()` to recover the actual
                //  window counts.  Works for both normalised and
                //  unnormalised histograms.
                if (std::isfinite(rec_score))
                {
                    //  noise_* are RAW combined-background (DCR + in-beam)
                    //  counts; data_* are the normalised data fraction × the
                    //  data window count.
                    const double bg_windows = noise_total; // N_DCR + N_in-beam
                    const double data_entries = h_data->GetEntries();
                    const double noise_frac_above = (noise_total > 0.0)
                                                        ? static_cast<double>(noise_tail_count) / noise_total
                                                        : 0.0;
                    const double data_frac_above = (data_total > 0.0)
                                                       ? static_cast<double>(data_tail_count) / data_total
                                                       : 0.0;
                    const double noise_above_n = noise_tail_count; // raw count
                    const double data_above_n = data_frac_above * data_entries;

                    //  Compact annotation — kept terse so the longest line
                    //  (with scientific-notation counts) clears the right
                    //  pad edge.  Starts at NDC x=0.45, small font.
                    TLatex txt;
                    txt.SetNDC();
                    txt.SetTextFont(42);
                    txt.SetTextSize(0.020);
                    txt.SetTextColor(kBlack);
                    double y_cursor = 0.73; // below the legend
                    const double y_step = 0.030;
                    txt.DrawLatex(0.45, y_cursor, TString::Format("rec. n_{#sigma} = %.2f   (in-beam #rightarrow 0)", rec_n_sigma));
                    y_cursor -= y_step;
                    txt.DrawLatex(0.45, y_cursor, "above cut:");
                    y_cursor -= y_step;
                    txt.DrawLatex(0.47, y_cursor, TString::Format("bkg  %.2g / %.2g  (%.1e)", noise_above_n, bg_windows, noise_frac_above));
                    y_cursor -= y_step;
                    txt.DrawLatex(0.47, y_cursor, TString::Format("data %.2g / %.2g  (%.2f%%)", data_above_n, data_entries, 100.0 * data_frac_above));
                    y_cursor -= y_step;
                    //  S/N from the FRACTIONS (count-based goes to ∞ when the
                    //  noise count rounds to 0 at a high cut).
                    if (noise_frac_above > 0.0)
                        txt.DrawLatex(0.47, y_cursor, TString::Format("S/N %.0f", data_frac_above / noise_frac_above));
                    else
                        txt.DrawLatex(0.47, y_cursor, "S/N #infty");
                    y_cursor -= y_step;
                    txt.DrawLatex(0.45, y_cursor, TString::Format("in-beam %.2g trig (%.2g dropped)", static_cast<double>(n_inbeam_triggers_total - n_inbeam_triggers_skipped), static_cast<double>(n_inbeam_triggers_skipped)));
                    y_cursor -= y_step;
                    //  Signal log-Gaussian position (n_σ = 10^m) +
                    //  model-based cut loss (the third, free term of the
                    //  data 3-logGauss fit).
                    if (have_data_fit && std::isfinite(sig_loss_frac))
                    {
                        txt.DrawLatex(0.45, y_cursor, TString::Format("m_{sig} = %.1f n_{#sigma}", sig_m));
                        y_cursor -= y_step;
                        txt.DrawLatex(0.45, y_cursor, TString::Format("signal lost below cut: %.1f%%", 100.0 * sig_loss_frac));
                    }
                }

                const auto path = util::qa::pdf_path(
                    run_dir, "lightdata", 5, "streaming_score");
                c.SaveAs(path.string().c_str());
                util::qa::crop_pdf_inplace(path);
            }
        }

        //  06+ Per-trigger anchor-Δt vs spill — one PDF per trigger
        //  that fired ≥ 1 time, rendered through the shared
        //  ``render_anchor_dt_canvas`` helper.  The trigger index
        //  order in the unordered_map isn't stable across runs;
        //  enumerate so the file order is deterministic by
        //  registry order (matches the trigger-matrix axis).
        std::vector<int> fired_trigger_ids;
        fired_trigger_ids.reserve(h_trigger_anchor_dt.size());
        for (const auto &[k, _] : h_trigger_anchor_dt)
            fired_trigger_ids.push_back(k);
        std::sort(fired_trigger_ids.begin(), fired_trigger_ids.end());

        int trg_order = 7; // 06 is taken by dcr_per_channel above
        for (int trigger_idx : fired_trigger_ids)
        {
            auto &hist = h_trigger_anchor_dt[trigger_idx];
            if (!hist)
                continue;
            //  Skip FirstFrames + StartOfSpill anchor-Δt PDFs — these
            //  are synthetic markers (FirstFrames is the per-frame
            //  noise-window tag, StartOfSpill is the boundary signal),
            //  not physics anchors.  Their Δt plots carry no
            //  information beyond "the marker fires every frame" and
            //  burn a PDF slot the shifter has to skim past.  The
            //  matching channel timing for the DCR workflow lives
            //  elsewhere (Single-Pixel Noise sub-tab).
            if (trigger_idx ==
                    registry.index_of(static_cast<TriggerNumber>(TriggerFirstFrames)) ||
                trigger_idx ==
                    registry.index_of(static_cast<TriggerNumber>(TriggerStartOfSpill)))
                continue;
            const std::string trig_name = registry.name_of(trigger_idx);
            const auto path = util::qa::pdf_path(
                run_dir, "lightdata", trg_order++,
                std::string("anchor_dt_") + trig_name);
            util::qa::AnchorDtCanvasOpts opts;
            opts.rollover_cc = BTANA_ALCOR_ROLLOVER_TO_CC;
            opts.title = TString::Format(
                             "#Deltat_{trg} vs spill   (channel #minus trigger %s)",
                             trig_name.c_str())
                             .Data();
            opts.pdf_path = path.string();
            opts.logger_prefix = "(lightdata_writer)";
            util::qa::render_anchor_dt_canvas(*hist.get(), opts);
        }

        //  Per-trigger consecutive-Δt vs spill — the SAME standardised
        //  TH2F (X = spill index, Y = Δt between consecutive firings of
        //  this trigger, log-spaced from 1 ns to 1e10 ns) for every
        //  hardware trigger, so the dashboard tiles them uniformly.
        //  ``h_trigger_dt`` is only booked inside the per-trigger loop's
        //  is_secondary + is_synthetic_marker gate, so this map already
        //  excludes secondaries, the FirstFrames/StartOfSpill markers,
        //  and the UNKNOWN / STREAMING_RING_FOUND derived triggers —
        //  no extra filtering needed here.  Enumerate + sort so the file
        //  order is deterministic by registry index (continues the
        //  ``trg_order`` counter so these land after the anchor-Δt PDFs).
        std::vector<int> dt_trigger_ids;
        dt_trigger_ids.reserve(h_trigger_dt.size());
        for (const auto &[k, _] : h_trigger_dt)
            dt_trigger_ids.push_back(k);
        std::sort(dt_trigger_ids.begin(), dt_trigger_ids.end());
        for (int trigger_idx : dt_trigger_ids)
        {
            auto &hist = h_trigger_dt[trigger_idx];
            if (!hist)
                continue;
            const std::string trig_name = registry.name_of(trigger_idx);
            TCanvas c(TString::Format("c_qa_lightdata_trigdt_%s",
                                      trig_name.c_str()),
                      "", 1000, 1000);
            //  Log Y so the log-spaced Δt bins render at uniform width;
            //  log Z for the wide per-bin dynamic range.  Right margin
            //  widened to seat the colour palette.
            c.SetLeftMargin(0.14);
            c.SetRightMargin(0.16);
            c.SetLogy();
            c.SetLogz();
            hist->Draw("colz");

            //  Per-spill Poisson-rate annotation.  The Δt between
            //  consecutive firings of a Poisson process is exponentially
            //  distributed: p(Δt) = λ·e^{−λΔt}.  So for each spill
            //  column we project the Δt distribution and fit ``expo``
            //  (= e^{p0+p1·Δt}); the decay constant λ = −p1 is the
            //  Poisson firing rate.  The fit slope is normalisation-
            //  independent, so the earlier ``Scale(1,"width")`` (which
            //  un-distorts the log-spaced Y bins into a true density)
            //  doesn't bias it.  λ is in 1/ns; ×1e9 → Hz.  Printed at
            //  90° at the top of each spill's column.
            std::vector<std::unique_ptr<TLatex>> rate_labels; // keep alive until SaveAs
            {
                TH2F *h2 = hist.get();
                const int nx = h2->GetNbinsX();
                //  Anchor the rate labels at 65 % up the (log) Y axis —
                //  lower than the previous near-top placement, sitting
                //  in the mid-upper headroom where they read clearly
                //  without crowding the top edge.  On a log axis, the
                //  value at fraction f is y_min·(y_max/y_min)^f.
                const double y_axis_lo = h2->GetYaxis()->GetXmin();
                const double y_axis_hi = h2->GetYaxis()->GetXmax();
                const double y_label =
                    (y_axis_lo > 0.0 && y_axis_hi > y_axis_lo)
                        ? y_axis_lo * std::pow(y_axis_hi / y_axis_lo, 0.80)
                        : y_axis_hi * 0.30;
                constexpr double kMinEntriesForFit = 10.0;
                for (int xb = 1; xb <= nx; ++xb)
                {
                    std::unique_ptr<TH1D> proj(
                        h2->ProjectionY(
                            TString::Format("_py_%s_%d", trig_name.c_str(), xb),
                            xb, xb));
                    if (!proj || proj->GetEntries() < kMinEntriesForFit)
                        continue;
                    //  The parent h2 was Scale(1,"width")'d into a density for
                    //  display, which leaves the projection with non-integer
                    //  contents and broken per-bin errors — a chi-square
                    //  ``expo`` fit on it mostly fails (status != 0) and no rate
                    //  is printed.  Recover the raw per-bin counts (×bin width
                    //  exactly inverts the density scaling) and restore Poisson
                    //  errors so the fit is well-posed again.
                    for (int b = 1; b <= proj->GetNbinsX(); ++b)
                    {
                        const double cnt = proj->GetBinContent(b) * proj->GetBinWidth(b);
                        proj->SetBinContent(b, cnt);
                        proj->SetBinError(b, cnt > 0.0 ? std::sqrt(cnt) : 0.0);
                    }
                    //  Quiet, no-draw, no-store Poisson-likelihood fit ("L") —
                    //  robust on the wide-dynamic-range, log-spaced counts where
                    //  the chi-square fit struggled.
                    const int fit_status = proj->Fit("expo", "Q0L");
                    TF1 *fexpo = proj->GetFunction("expo");
                    double lambda_per_ns = -1.0;
                    if (fit_status == 0 && fexpo)
                    {
                        const double p1 = fexpo->GetParameter(1); // = −λ (1/ns)
                        if (p1 < 0.0 && std::isfinite(p1))
                            lambda_per_ns = -p1;
                    }
                    //  Fallback when the fit still doesn't converge: the mean
                    //  inter-arrival time of a Poisson process is 1/λ, so
                    //  λ = 1/⟨Δt⟩ from the (count-restored) projection — always
                    //  defined when the column has hits, so a rate is shown.
                    if (!(lambda_per_ns > 0.0))
                    {
                        const double mean_dt = proj->GetMean();
                        if (mean_dt > 0.0 && std::isfinite(mean_dt))
                            lambda_per_ns = 1.0 / mean_dt;
                    }
                    if (!(lambda_per_ns > 0.0) || !std::isfinite(lambda_per_ns))
                        continue;
                    const double rate_hz = lambda_per_ns * 1.0e9;
                    //  Human-friendly unit.
                    TString rate_txt;
                    if (rate_hz >= 1.0e6)
                        rate_txt = TString::Format("%.2f MHz", rate_hz / 1.0e6);
                    else if (rate_hz >= 1.0e3)
                        rate_txt = TString::Format("%.2f kHz", rate_hz / 1.0e3);
                    else
                        rate_txt = TString::Format("%.1f Hz", rate_hz);
                    const double x_centre = h2->GetXaxis()->GetBinCenter(xb);
                    auto lbl = std::make_unique<TLatex>(
                        x_centre, y_label, rate_txt.Data());
                    lbl->SetTextAngle(90);
                    lbl->SetTextAlign(12); // left-bottom along the rotated axis
                    lbl->SetTextSize(0.018);
                    lbl->SetTextColor(kBlack);
                    lbl->Draw();
                    rate_labels.push_back(std::move(lbl));
                }
            }

            const auto path = util::qa::pdf_path(
                run_dir, "lightdata", trg_order++,
                std::string("trigger_dt_") + trig_name);
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);
        }

        //  Per-trigger Δt-vs-ToT (ToT-family runs only) — one PDF per fired
        //  hardware trigger, X = Δt(hit − trigger) [ns], Y = ToT [ns], log Z
        //  for the wide per-bin dynamic range.  ``h_trigger_dt_vs_tot`` is
        //  only booked under the same is_secondary + is_synthetic_marker gate
        //  as the other per-trigger maps AND only when ``tot_qa``, so this
        //  loop is empty for LET runs and already excludes secondaries /
        //  synthetic markers.  Continues the trg_order counter so these tile
        //  after the consecutive-Δt PDFs.
        std::vector<int> dt_vs_tot_trigger_ids;
        dt_vs_tot_trigger_ids.reserve(h_trigger_dt_vs_tot.size());
        for (const auto &[k, _] : h_trigger_dt_vs_tot)
            dt_vs_tot_trigger_ids.push_back(k);
        std::sort(dt_vs_tot_trigger_ids.begin(), dt_vs_tot_trigger_ids.end());
        for (int trigger_idx : dt_vs_tot_trigger_ids)
        {
            auto &hist = h_trigger_dt_vs_tot[trigger_idx];
            if (!hist || hist->GetEntries() <= 0)
                continue;
            const std::string trig_name = registry.name_of(trigger_idx);
            TCanvas c(TString::Format("c_qa_lightdata_dtvstot_%s",
                                      trig_name.c_str()),
                      "", 1000, 1000);
            c.SetLeftMargin(0.14);
            c.SetRightMargin(0.16);
            c.SetLogz(); // wide per-bin dynamic range (prompt peak ≫ tail)
            hist->Draw("colz");
            const auto path = util::qa::pdf_path(
                run_dir, "lightdata", trg_order++,
                std::string("dt_vs_tot_") + trig_name);
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);
        }

        //  Per-trigger trigger↔cherenkov coincidence — Δt of cherenkov hits
        //  relative to each hardware trigger (the in-time peak shows the
        //  trigger fires together with cherenkov light).  One PDF per
        //  trigger, same enumerate+sort so the dashboard tiles them
        //  uniformly; continues the trg_order counter so they land after
        //  the consecutive-Δt PDFs.
        std::vector<int> cher_trigger_ids;
        cher_trigger_ids.reserve(h_trigger_time_diff_w_cherenkov.size());
        for (const auto &[k, _] : h_trigger_time_diff_w_cherenkov)
            cher_trigger_ids.push_back(k);
        std::sort(cher_trigger_ids.begin(), cher_trigger_ids.end());
        for (int trigger_idx : cher_trigger_ids)
        {
            auto &hist = h_trigger_time_diff_w_cherenkov[trigger_idx];
            if (!hist || hist->GetEntries() <= 0)
                continue;
            const std::string trig_name = registry.name_of(trigger_idx);
            TCanvas c(TString::Format("c_qa_lightdata_trigcher_%s",
                                      trig_name.c_str()),
                      "", 1000, 1000);
            c.SetLeftMargin(0.13);
            c.SetBottomMargin(0.12);
            c.SetLogy(); // tails span orders of magnitude below the peak
            hist->SetLineColor(kAzure + 1);
            hist->SetLineWidth(2);
            //  Zoom to the in-time coincidence region; the full frame range
            //  buries the peak.
            hist->GetXaxis()->SetRangeUser(-100., 100.);
            hist->Draw("hist");
            const auto path = util::qa::pdf_path(
                run_dir, "lightdata", trg_order++,
                std::string("trigcher_dt_") + trig_name);
            c.SaveAs(path.string().c_str());
            util::qa::crop_pdf_inplace(path);

            //  Companion in-window hitmap for the same trigger: (x, y) of the
            //  cherenkov hits inside the config timing cut [kTrigCherDtMin,
            //  kTrigCherDtMax] — the in-time trigger-Cherenkov occupancy.
            auto hm_it = h_trigger_window_hitmap.find(trigger_idx);
            if (hm_it != h_trigger_window_hitmap.end() && hm_it->second &&
                hm_it->second->GetEntries() > 0)
            {
                TCanvas ch(TString::Format("c_qa_lightdata_trigcherhm_%s",
                                           trig_name.c_str()),
                           "", 1000, 1000);
                ch.SetRightMargin(0.14);
                hm_it->second->Draw("colz");
                const auto path_hm = util::qa::pdf_path(
                    run_dir, "lightdata", trg_order++,
                    std::string("trigcher_hitmap_") + trig_name);
                ch.SaveAs(path_hm.string().c_str());
                util::qa::crop_pdf_inplace(path_hm);
            }
        }
    }

    //  ---
    //  --- Publish curated scalars to AnalysisResults (cross-run store)
    //
    //  TOML-backed: AnalysisResults::update reads + writes
    //  ``<data_repository>/standard_results.toml`` (= ``Data/``, NOT the
    //  git repo root).  Dashboard consumer is
    //  ``qa_quicklook.rundb.results_load``; sibling audit log at
    //  ``standard_results.audit.toml``.
    //  (the new dashboard-friendly format).  See DISCUSSION.md.
    //
    //  Sensor tag comes from the cherenkov role's readout config — the
    //  single source of truth for SiPM type added in this campaign.
    //  Falls back to ``"all"`` when no tag is set so we never write an
    //  empty sensor key (which would defeat the slicing dimension).
    {
        std::string sensor = "all";
        if (const auto *cher = framer.get_readout_config()
                                   .find_by_name("cherenkov"))
        {
            if (!cher->sensor_type.empty())
                sensor = cher->sensor_type;
        }

        //  Cross-run aggregate lives next to the run directories
        //  (``<data_repository>/standard_results.toml`` = ``Data/``).  The legacy
        //  ``extData/`` hard-code failed whenever the writer was
        //  launched from a cwd that didn't happen to have an
        //  ``extData/`` directory — the dashboard does exactly that.
        //  ── Rate-ified QA scalars + readout-resilience metrics ───────────
        //  dcr_mean_khz: mean single-pixel dark rate, averaged over the
        //  cherenkov channels that saw noise hits.  The per-channel TProfile
        //  was scaled to kHz in the "--- DCR" finalize block above, so its bin
        //  contents are already kHz; average them with equal per-channel
        //  weight.  This is the data-rate axis for the resilience study.
        double dcr_sum_khz = 0.0;
        long dcr_channels = 0;
        for (int b = 1; b <= h_dcr_per_channel->GetNbinsX(); ++b)
            if (h_dcr_per_channel->GetBinEntries(b) > 0.0)
            {
                dcr_sum_khz += h_dcr_per_channel->GetBinContent(b);
                ++dcr_channels;
            }
        const double dcr_mean_khz =
            dcr_channels > 0 ? dcr_sum_khz / dcr_channels : 0.0;

        //  afterpulse_prob: DCR-subtracted afterpulse probability (%), averaged
        //  over channels.  The per-channel TProfile bin means are already the
        //  100·(P_near − P_far) probability; equal per-channel weighting.
        double ap_sum = 0.0;
        long ap_channels = 0;
        for (int b = 1; b <= h_afterpulse_per_channel->GetNbinsX(); ++b)
            if (h_afterpulse_per_channel->GetBinEntries(b) > 0.0)
            {
                ap_sum += h_afterpulse_per_channel->GetBinContent(b);
                ++ap_channels;
            }
        const double afterpulse_prob = ap_channels > 0 ? ap_sum / ap_channels : 0.0;

        //  lane_failure_rate: detector-wide dead-lane fraction in [0, 1],
        //  exposure-normalised over participant lane-spills (see the
        //  accumulators above the spill loop).  The resilience response axis.
        const double lane_failure_rate =
            participant_lane_spills > 0
                ? static_cast<double>(dead_lane_spills) /
                      static_cast<double>(participant_lane_spills)
                : 0.0;

        AnalysisResults ar(data_repository + "/standard_results.toml");
        ar.update(ResultMap{
                      //  n_selected_frames: trigger-matrix entries ≈ frames that
                      //  passed selection (NOT distinct physics events).  Kept as
                      //  a throughput measure; renamed from the misleading
                      //  "n_events".
                      {{run_name, sensor, "lightdata.n_selected_frames"},
                       {static_cast<double>(h2_trigger_matrix->GetEntries()), 0.0}},
                      //  afterpulse_prob: DCR-subtracted afterpulse probability (%).
                      {{run_name, sensor, "lightdata.afterpulse_prob"},
                       {afterpulse_prob, 0.0}},
                      //  dcr_mean_khz: mean single-pixel dark rate (kHz).  The
                      //  data-rate axis.  Sensor-agnostic → published under "all".
                      {{run_name, "all", "lightdata.dcr_mean_khz"},
                       {dcr_mean_khz, 0.0}},
                      //  lane_failure_rate: detector-wide dead-lane fraction.  The
                      //  resilience response axis.  Sensor-agnostic → "all".
                      {{run_name, "all", "lightdata.lane_failure_rate"},
                       {lane_failure_rate, 0.0}},
                  },
                  /*source=*/"lightdata");
    }
    //  outfile closed automatically by TFilePtr dtor
    //  End: QA plots
    //  --- --- --- --- --- ---
}
