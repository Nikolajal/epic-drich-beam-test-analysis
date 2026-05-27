#pragma once

/**
 * @file config_reader.h
 * @brief Run configuration and readout Mapping utilities for the ePIC dRICH beam test.
 *
 * Provides:
 * - @ref ReadoutConfigStruct  – per-named-role (device, chip) assignment.
 * - @ref ReadoutConfigList   – searchable container of readout configurations.
 * - @ref readout_config_reader – TOML file parser populating the above.
 * - @ref RunInfoStruct       – per-run beam, DAQ, sensor, and optics metadata.
 * - @ref RunInfo              – static database of run metadata and run lists.
 *
 * @todo Re-write legacy text-based sections for full TOML configuration files.
 */

#include <fstream>
#include <sstream>
#include <set>
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>
#include <algorithm>
#include <iostream>
#include <cstdint>
#include <utility.h>
#include <toml++/toml.h>
#include "utility/toml_utils.h"

// =========================================================================
//  Core tag set
// =========================================================================

/// Named roles that are mutually exclusive per (device, chip) pair.
/// `inline` (C++17) gives a single shared definition across translation
/// units — the previous `static` declaration produced one copy per TU
///
inline const std::set<std::string> lightdata_core_tags = {"timing", "tracking", "cherenkov"};

// =========================================================================
//  Readout configuration
// =========================================================================

/**
 * @brief Associates a named readout role with a set of (device, chip) pairs.
 *
 * The @c device_chip map keys are ALCOR device indices; values are the list
 * of chip indices active under that device for the given role.
 */
struct ReadoutConfigStruct
{
    std::string name;                                      ///< Human-readable role name (e.g. @c "cherenkov").
    std::map<uint16_t, std::vector<uint16_t>> device_chip; ///< Active chips per device.

    ReadoutConfigStruct() = default;

    /**
     * @brief Construct with an explicit name and device–chip map.
     * @param _name       Role name.
     * @param _device_chip Initial device–chip assignment.
     */
    ReadoutConfigStruct(std::string _name, std::map<uint16_t, std::vector<uint16_t>> _device_chip);

    /**
     * @brief Register a single (device, chip) pair.
     * @param device ALCOR device index.
     * @param chip   Chip index within the device.
     */
    void add_device_chip(uint16_t device, uint16_t chip);

    /**
     * @brief Register all 8 chips of a device.
     * @param device ALCOR device index.
     */
    void add_device(uint16_t device);
};

// =========================================================================

/**
 * @brief Searchable container of @ref ReadoutConfigStruct entries.
 *
 * All find methods return raw pointers into the internal vector; the pointers
 * remain valid as long as the list is not modified.
 */
class ReadoutConfigList
{
public:
    // -------------------------------------------------------------------------
    /** @name Construction */
    /// @{

    ReadoutConfigList() = default;

    /// @brief Construct from an existing vector of configs (moved in).
    explicit ReadoutConfigList(std::vector<ReadoutConfigStruct> vec);

    /// @}

    // -------------------------------------------------------------------------
    /** @name Search */
    /// @{

    /// @brief First config whose @c name matches @p name, or @c nullptr.
    ReadoutConfigStruct *find_by_name(const std::string &name);
    const ReadoutConfigStruct *find_by_name(const std::string &name) const;

    /// @brief First config that contains @p device, or @c nullptr.
    ReadoutConfigStruct *find_by_device(uint16_t device);

    /// @brief All configs that contain @p device.
    std::vector<ReadoutConfigStruct *> find_all_by_device(uint16_t device);

    /// @brief Names of all configs that contain the (device, chip) pair.
    std::vector<std::string> find_by_device_and_chip(uint16_t device, uint16_t chip);

    /// @brief @c true if any config has @c name equal to @p name.
    bool has_name(const std::string &name) const;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Role presence flags */
    /// @{

    /// @brief @c true if a @c "cherenkov" role is present.
    bool has_cherenkov();
    /// @brief @c true if a @c "timing" role is present.
    bool has_timing();
    /// @brief @c true if a @c "tracking" role is present.
    bool has_tracking();

    /// @}

    /// @brief Read-only view of the underlying configs.  Use this instead of
    ///        the (now-private) member when iterating; the const-ref return
    ///        keeps callers from accidentally mutating mid-loop and
    ///        invalidating pointers returned by @ref find_by_device etc.
    const std::vector<ReadoutConfigStruct> &all() const noexcept { return configs; }

private:
    /// Ordered list of readout role assignments.  Private so external code can
    /// neither `push_back` (which would invalidate previously returned
    /// pointers from `find_*`) nor mutate entries without going through the
    /// class API  No `.configs.push_back(...)` callers
    /// exist today; if a future need arises, add a guarded `add_config(...)`
    /// method here rather than re-exposing the vector.
    std::vector<ReadoutConfigStruct> configs;

public:
};

// =========================================================================
//  Free utility functions
// =========================================================================

/**
 * @brief Names of all configs in @p readout_config_utility containing (device, chip).
 * @param readout_config_utility Map of role name → config struct.
 * @param device                 ALCOR device index.
 * @param chip                   Chip index.
 * @return Vector of matching role names.
 */
std::vector<std::string> find_by_device_and_chip(
    const std::map<std::string, ReadoutConfigStruct> &readout_config_utility,
    uint16_t device,
    uint16_t chip);

/**
 * @brief Parse a TOML readout configuration file and return the role list.
 *
 * Reads the @c [readout] table, expands @c "*" chip wildcards, and enforces
 * mutual exclusion among @ref lightdata_core_tags.  Conflicts are logged as
 * errors and the offending (device, chip) pair is silently skipped.
 *
 * @param config_file Path to the TOML configuration file.
 * @return Vector of @ref ReadoutConfigStruct, one per named role.
 */
std::vector<ReadoutConfigStruct> readout_config_reader(std::string config_file = "conf/readout_config.toml");

// =========================================================================
//  Framer configuration
// =========================================================================

/**
 * @brief Framing and timing constants for the parallel streaming framer.
 *
 * All values default to the same constants previously hard-coded as macros in
 * @c ParallelStreamingFramer.h so that existing code is unaffected when no
 * config file is provided.
 */
struct FramerConfigStruct
{
    uint16_t frame_size = 1024;              ///< Clock cycles per frame (320 MHz → 3.125 ns/cc).
    int first_frames_trigger = 5000;         ///< Start-of-spill frames reserved for noise measurement.
    uint16_t afterpulse_deadtime = 64;       ///< Afterpulse suppression deadtime (cc, ~200 ns).
    uint16_t trigger_secondary_window = 200; ///< Secondary-trigger detection window (cc, ~625 ns).

    /// @brief Frame duration in nanoseconds.
    float frame_length_ns() const { return frame_size * 3.125f; }
};

/**
 * @brief Parse a TOML framer configuration file.
 *
 * Reads the @c [framer] table.  Missing keys fall back to the defaults in
 * @ref FramerConfigStruct, so a minimal or even empty file is valid.
 *
 * @param config_file Path to the TOML configuration file.
 * @return Populated @ref FramerConfigStruct.
 */
FramerConfigStruct FramerConfReader(std::string config_file = "conf/framer_conf.toml");

// =========================================================================
//  QA configuration — windows used by QA histograms (afterpulse, cross-talk)
// =========================================================================

/**
 * @brief Per-window timing constants used by the QA pipeline.
 *
 * Two semantically different families live here:
 *
 *  - **Afterpulse sideband** (consumed by @ref ParallelStreamingFramer to
 *    tag @c HitmaskAfterpulseNear / @c HitmaskAfterpulseFar on every
 *    Hit).  Near and far windows are the same width — subtraction in the
 *    QA profiles gives the DCR-corrected afterpulse probability.
 *
 *  - **Cross-talk scan & signal windows** (consumed by @ref lightdata_writer
 *    in the per-spill CT scan loop).  Lift here so the same source of truth
 *    drives both the diagnostic histograms (h_*_ct_dt, h_*_ct_dchannel_dt)
 *    and the per-channel CT-probability profiles.
 *
 *  Defaults reproduce the previously hard-coded constants in
 *  @c lightdata_writer.cxx exactly, so existing analyses are unaffected when
 *  no @c [qa] section is provided in @c framer_conf.toml.
 */
struct QaConfigStruct
{
    // -------- Afterpulse sideband (clock cycles) --------
    /// @brief Near window lower edge (Δt cc, inclusive).  Excludes Δt = 0 (the Hit itself).
    int afterpulse_near_lo = 1;
    /// @brief Near window upper edge (Δt cc, inclusive).  Default mirrors @c afterpulse_deadtime.
    int afterpulse_near_hi = 64;
    /// @brief Far (sideband) window start (Δt cc, inclusive).  Width matches the near window.
    int afterpulse_sideband_offset = 256;

    // -------- Cross-talk scan & signal windows (clock cycles) --------
    /// @brief Outer Δt cutoff for the CT scan loop (inclusive lower bound).
    /// Default extends to -10 cc so the diagnostic Δt histograms (both physical and
    /// electrical) include the symmetric negative-Δt region — useful to verify that
    /// CT really clusters near Δt = 0 rather than leaking into the sideband.
    int ct_scan_dt_min = -10;
    /// @brief Outer Δt cutoff for the CT scan loop (exclusive upper bound).
    int ct_scan_dt_max = 200;
    /// @brief Physical-CT signal window lower edge (Δt cc, inclusive).
    int ct_phys_signal_lo = 0;
    /// @brief Physical-CT signal window upper edge (Δt cc, inclusive).
    int ct_phys_signal_hi = 10;
    /// @brief Electrical-CT signal window lower edge (Δt cc, inclusive).
    int ct_elec_signal_lo = -2;
    /// @brief Electrical-CT signal window upper edge (Δt cc, inclusive).
    int ct_elec_signal_hi = 10;

    // -------- QA-mode behavior toggles --------
    //  Section holds boolean knobs (not windows) that flip lightdata_writer
    //  behavior under `--QA`.  Lives in QaConfigStruct rather than its own
    //  struct because (a) it's read from the same `[qa]` TOML section and
    //  (b) creating a dedicated struct for a handful of bools doesn't carry
    //  its weight.  Recodata-side QA toggles use the same pattern via
    //  @ref RecodataConfigStruct::skip_loo_residuals.

    /// @brief Run `spilldata.update_calibration(framer.get_fine_tune_distribution())`
    /// at the head of every spill iteration.
    ///
    /// Off by default: the canonical calibration path is the offline pass
    /// via `macros/examples/fine_calibration_timing.cpp`.  Set true in
    /// `conf/QA/framer_conf.toml` so each spill seeds its calibration table
    /// from the channels that became active in the previous spill — useful
    /// only for an online-only mode where no offline calibration is available.
    bool per_spill_calibration_update = false;
};

/**
 * @brief Parse the @c [qa] table from a TOML configuration file.
 *
 * Missing keys fall back to the defaults in @ref QaConfigStruct (which
 * reproduce the legacy hard-coded values), so a file with no @c [qa] section
 * still yields valid configuration.
 *
 * @param config_file Path to the TOML configuration file (typically the same
 *                    file used for @ref FramerConfReader).
 * @return Populated @ref QaConfigStruct.
 */
QaConfigStruct qa_conf_reader(std::string config_file = "conf/framer_conf.toml");

// =========================================================================
//  Pulser-calibration configuration
// =========================================================================

/**
 * @brief Knobs for the pulser-driven fine-time calibration pipeline.
 *
 * Consumed by @ref pulser_calib_writer.  The anchor channel is the
 * channel that's pulsed and used as the per-pulse coincidence reference;
 * it must NOT be declared as a `[[trigger]]` in the trigger config used
 * for the `--calib` lightdata pass (otherwise the framer strips its
 * fine value when emitting the @c TriggerEvent — see
 * @c parallel_streaming_framer.cxx).  Anchor flows as a normal hit and
 * is identified by these (device, chip, eo_channel) fields.
 *
 * Defaults reflect the dRICH 2026 test-beam pulser run setup.
 */
struct CalibConfigStruct
{
    /// @brief Anchor channel — ALCOR device id.  Default 192 (test-beam dRICH).
    int anchor_device = 192;
    /// @brief Anchor channel — chip index within the device (0..7).
    int anchor_chip = 0;
    /// @brief Anchor channel — even-odd channel index within the chip (0..31).
    int anchor_eo_channel = 0;

    /// @brief Minimum cumulative hits (across all spills) on a single
    /// GlobalIndex before its `(a, b)` fit is published.  GlobalIndex's
    /// below threshold are omitted from `fine_calib.txt` and listed in
    /// the QA root file's skipped-channel TNamed.  Default 250.
    int min_hits_per_tdc = 250;

    /// @brief Minimum hits in a single spill before a per-spill fit is
    /// attempted (the building block for the cross-spill weighted-mean
    /// aggregation).  Below this, the spill's contribution to that
    /// TDC's aggregate is skipped (but other spills still contribute).
    /// Default 4 — the minimum a 2-param fit needs to leave any
    /// degrees of freedom for a residual-sigma estimate.  Pulser
    /// runs typically have ~5 hits/TDC/spill (5000 pulses/spill ÷ 4
    /// TDCs round-robin, often less); raising this floor would
    /// silently drop most spills from the aggregate.
    int min_hits_per_tdc_per_spill = 4;

    /// @brief Inclusive lower edge of the valid-fine band.  Hits with
    /// `fine < fine_min_valid` are discarded at FIFO ingestion in
    /// @ref pulser_calib_writer and never enter the per-channel
    /// buckets, the fit, or the slip-correction pass.  ALCOR's fine
    /// bins typically populate ~[30, 130]; values well outside this
    /// range are pathological (early-pickup, end-of-cycle wrap, or
    /// readout noise).  Default 20 gives a safety margin below the
    /// populated band.
    ///
    /// Design note: ideally the fit itself would identify such hits
    /// as outliers without an explicit filter.  Tracked as a
    /// follow-up in `include/writers/DISCUSSION.md`.
    int fine_min_valid = 20;
    /// @brief Inclusive upper edge of the valid-fine band.  See
    /// @ref fine_min_valid.  Default 160.
    int fine_max_valid = 160;

    /// @brief Slope-fit guard: refuse to fit the per-TDC slope when
    /// the TDC's fine-bin range is narrower than this.  Pulser data
    /// hits a tight cluster of fine bins per TDC per spill (typically
    /// 3-4 bins) because the pulser-clock-vs-DAQ-clock phase is
    /// nearly stable within a spill.  With < this many distinct bins
    /// the regression has no x-axis lever arm and returns numerical
    /// noise (often unphysical negative slopes).  When the guard
    /// fires, fall back to @ref default_slope_cc_per_bin and fit only
    /// the intercept.  Default 5.
    int slope_fit_min_fine_span = 5;

    /// @brief Fallback slope (cc per fine bin) when the slope-fit
    /// guard fires.  Roughly 3.125/256 cc/bin (the uniform
    /// fine-distribution assumption), nudged toward 0.015 by typical
    /// ALCOR characterisation.  Default 0.015.
    double default_slope_cc_per_bin = 0.015;

    /// @brief Physical lower bound on per-TDC slope (cc/bin).  Fitted
    /// slopes below this are unphysical — typical ALCOR slope sits in
    /// [0.01, 0.02] centred at the default.  Slopes outside the
    /// [@ref slope_min, @ref slope_max] band are replaced by
    /// @ref default_slope_cc_per_bin at publish time.  Default 0.01.
    double slope_min = 0.01;

    /// @brief Physical upper bound on per-TDC slope (cc/bin).
    /// Default 0.02.  See @ref slope_min.
    double slope_max = 0.02;

    /// @brief Optional fixed pulser period in cc.  When > 0, the
    /// per-channel period fit is SKIPPED and this value is used
    /// directly across every channel, every spill — the pulser
    /// produces ONE signal seen by all TDCs simultaneously, so
    /// fixing the period from external knowledge (e.g. "we set the
    /// generator to 1 kHz") removes one fit parameter.
    /// Default 0 = fit per channel.
    /// Example: for a 1 kHz pulser at 320 MHz clock, set 320000.0.
    double pulser_period_cc = 0.0;

    /// @brief Physical lower bound on per-TDC intercept b (cc).
    /// After Stage 2 (cross-channel mod-T) + Stage 3 (mean-subtraction
    /// centring), published b values are clamped to
    /// [@ref b_min, @ref b_max] — anything outside is the cross-chip
    /// clock-start mod-T residue, which is NOT a physical channel
    /// delay (cabling can't exceed ~20 cc = ~60 ns of propagation
    /// without making the experiment infeasible).  Default -20.
    double b_min = -20.0;

    /// @brief Physical upper bound on per-TDC intercept b (cc).
    /// Default +20.  See @ref b_min.
    double b_max = +20.0;

    /// @brief Half-width of the "consecutive-pair" tolerance window
    /// around `pulser_period_cc` (cc).  Hit pairs whose
    /// `|coarse_diff − pulser_period_cc| > consecutive_pair_tolerance_cc`
    /// are EXCLUDED from the closed-form chi² — they're either
    /// missed-pulse jumps (≥2T), spill-boundary leak, or wild outliers.
    /// Default 10 cc (~31 ps) — tight to reject anything but
    /// genuine consecutive-pulse pairs.  The `Diagnostics/` QA
    /// histograms show the actual `c_h − c_p` distribution; consult
    /// before widening.
    double consecutive_pair_tolerance_cc = 10.0;

    /// @brief Slip-correction confidence threshold (cc).  After the
    /// first closed-form fit, each hit's within-pulse phase
    /// deviation from its (spill, TDC) median is checked: a hit is
    /// snapped by an integer number of cc only if
    /// `|deviation − round(deviation)| < slip_confidence_cc`.
    /// Tight values reject the natural per-hit scatter and only
    /// catch genuine integer slips; loose values over-snap.
    /// Default 0.1 cc.
    double slip_confidence_cc = 0.1;

    /// @brief Slip-correction safety cap (fraction).  If more than
    /// this fraction of hits in a given (spill, TDC) would be
    /// snapped, the entire (spill, TDC) is left alone — too many
    /// candidates means the distribution is too noisy / wide for
    /// slip detection to be reliable.  Default 0.30 (30%).
    double slip_max_snap_fraction = 0.30;

    //  Regime-1 (whole-TDC permanent slip) knobs intentionally
    //  REMOVED 2026-05-27.  Permanent slip is naturally absorbed by
    //  the fit's per-TDC intercept b — publishing the fitted b
    //  carries the slip into downstream's get_phase() correction
    //  exactly.  An earlier regime-1 implementation modified the
    //  per-hit coarse counter before re-fitting and then published
    //  the zero-residue b, which silently broke the calibration
    //  for any channel with a hardware-permanent slip (the
    //  calibration file then had b≈0 while production hits still
    //  carried the slip).  The QA hist for h_fit_intercept_b shows
    //  satellites at the slip lattice points (integer cc for full
    //  coarse-counter slips, half-integer cc for ALCOR-640 MHz
    //  slips, etc.) — those satellites are real and reflect the
    //  calibration honestly.

    // -------- Calibration-file resolution policy (3-tier) ----------
    //  Both producer (`pulser_calib_writer`) and consumers
    //  (`recodata_writer`, …) honour the same resolution policy so
    //  there's one source of truth for "which fine_calib does this
    //  run use?":
    //
    //    1. If `override_path` is non-empty AND exists → use it.
    //       For analysts who've curated a calibration outside the run
    //       directory.
    //
    //    2. Else `default_path` (compulsory; relative to the run dir
    //       <data_repository>/<run_name>/) → use it if it exists.
    //       This is where `pulser_calib_writer` writes its output and
    //       where the consumers read from in the normal flow.
    //
    //    3. Else trigger rebuild — for `pulser_calib_writer` that
    //       means "run the pipeline".  For consumers it means "error
    //       out with a clear pointer to pulser_calib_writer".
    //
    //  `force_rebuild` short-circuits all of the above for the
    //  producer (always re-runs, overwrites `default_path`).  No
    //  effect on consumers (they always need a file to read).

    /// @brief Optional explicit path to a vetted calibration file.
    /// Takes priority when set and exists.  Default empty = skip.
    std::string override_path = "";

    /// @brief Compulsory default calibration path, relative to
    /// `<data_repository>/<run_name>/`.  Where `pulser_calib_writer`
    /// writes its output and where consumers read from when
    /// `override_path` is unset.  Default `"fine_calib.txt"`.
    std::string default_path = "fine_calib.txt";

    /// @brief Force producer (`pulser_calib_writer`) to re-run even
    /// if a calibration already exists.  No effect on consumers.
    /// Default `false`; CLI `--force-rebuild` flag overrides this
    /// to `true` for one invocation.
    bool force_rebuild = false;
};

/**
 * @brief Result of @ref resolve_fine_calib_path: which file (if any) to use.
 *
 * Distinguishes the four resolution outcomes so callers can act
 * differently — e.g. consumers error out on `MissingNeedsRebuild`
 * with a useful message, while producers just run the pipeline.
 */
enum class CalibPathResolution
{
    Override,             ///< override_path was set and exists
    Default,              ///< default_path exists in the run dir
    MissingNeedsRebuild,  ///< neither exists; producer should rebuild, consumer should error
    ForceRebuildRequested ///< force_rebuild=true (producer ignores existing files)
};

/**
 * @brief Apply the 3-tier resolution policy from @ref CalibConfigStruct.
 *
 * Caller usually wants both the resolution kind and the resolved path
 * (the file the caller should read or write).  When the resolution is
 * `MissingNeedsRebuild` or `ForceRebuildRequested`, `path` is set to
 * `<run_dir>/<default_path>` — i.e. where a producer should write,
 * and where a consumer would have found the file if it had existed.
 */
struct CalibPathResult
{
    CalibPathResolution kind;
    std::string path;
};

CalibPathResult resolve_fine_calib_path(const CalibConfigStruct &cfg,
                                        const std::string &run_dir);

/**
 * @brief Parse the @c [calibration] table from a TOML configuration file.
 *
 * Missing keys fall back to the defaults in @ref CalibConfigStruct.
 * The conventional file path is @c conf/calib/calibration_conf.toml;
 * the lookup is via @ref util::conf_path so writers honour the
 * `--calib` flag's @c conf/calib/ override.
 *
 * @param config_file Path to the TOML configuration file.
 * @return Populated @ref CalibConfigStruct.
 */
CalibConfigStruct calib_conf_reader(std::string config_file = "conf/calib/calibration_conf.toml");

// =========================================================================
//  Streaming-trigger configuration
// =========================================================================

/**
 * @brief Tuning knobs for the DCR-weighted streaming-trigger score stage
 *
 * The score stage is the first half of the two-stage software trigger
 * pipeline — a per-frame online pre-filter that gates the Hough
 * ring-finder downstream.  See
 * [`include/triggers/streaming/DISCUSSION.md`](../triggers/streaming/DISCUSSION.md)
 * for the algorithm and § 1.5 for the threshold-tuning workflow.
 *
 * The TOML loader accepts a `[streaming_trigger]` section in
 * [`conf/streaming.toml`](../../conf/streaming.toml); missing keys fall back
 * to the defaults below so an un-configured file is still valid.
 */
struct StreamingTriggerConfigStruct
{
    /// @brief Sliding-window width [ns].  Hits within this Δt window
    /// form a cluster.
    ///
    /// @warning This knob is **inherited** by the Hough stage as its
    /// hit pre-selection window — there is no separate
    /// `time_cut_ns` knob on the Hough side, by design (see
    /// @c include/triggers/streaming/DISCUSSION.md § 2.2).  Retuning
    /// `time_window_ns` here silently changes the Hough's hit pool;
    /// expect both stages' QA to move when you change this value.
    float time_window_ns = 5.f;

    /// @brief Threshold on standardised score $n_\sigma = (S - \mathbb{E}[S])/\sigma_S$.
    ///        Cluster fires when the score crosses this value.  Initial guess
    ///        suggested in the design doc is 3; tune offline from the two QA
    ///        score histograms.  Set to a very large value (e.g. 1000) to
    ///        disable firing while still accumulating QA — see § 2.4 in
    ///        `include/triggers/DISCUSSION.md`.
    float n_sigma_threshold = 3.f;

    /// @brief Minimum number of hits a channel must accumulate in the noise
    ///        sample before it enters the streaming-trigger weight bundle.
    ///        Channels firing fewer times have unreliable rate estimates
    ///        (the rare-fire end of the spectrum produces large outlier
    ///        weights that inflate the noise-score tail).  Default 5.0
    ///        gives ~45 % relative error on the rate estimate.  Increase
    ///        for stricter purity, decrease for more channels in early
    ///        spills.
    double min_noise_hits = 5.0;
};

/**
 * @brief Parse the @c [streaming_trigger] table from a TOML configuration file.
 *
 * Missing keys fall back to the defaults in @ref StreamingTriggerConfigStruct.
 *
 * @param config_file Path to the TOML configuration file
 *                    (defaults to @c "conf/streaming.toml").
 * @return Populated @ref StreamingTriggerConfigStruct.
 */
StreamingTriggerConfigStruct
streaming_trigger_conf_reader(std::string config_file = "conf/streaming.toml");

// =========================================================================
//  Streaming-Hough configuration
// =========================================================================

/**
 * @brief Tuning knobs for the streaming-trigger Hough ring-finder stage.
 *
 * Stage 2 of the software trigger pipeline.  Runs on every frame where
 * the score stage fired (`_TRIGGER_STREAMING_RING_FOUND_` present),
 * extracts up to `max_rings` ring candidates via Hough voting, then
 * refines each ring's centre with a least-squares `fit_circle`.
 *
 * The TOML loader accepts a `[streaming_hough]` section in
 * [`conf/streaming.toml`](../../conf/streaming.toml).  Missing keys fall
 * back to the defaults below.
 *
 * See [`include/triggers/streaming/DISCUSSION.md`](../triggers/streaming/DISCUSSION.md)
 * for the algorithm, parameter physics, and roadmap.
 *
 * @note  Phase 2 of the streaming-trigger consolidation introduces this
 *        struct + reader.  The algorithm in `src/lightdata_writer.cxx`
 *        still uses hardcoded constants — defaults here match the
 *        hardcoded values, and Phase 4 wires them into the algorithm.
 */
struct StreamingHoughConfigStruct
{
    // ── Hough accumulator geometry ──────────────────────────────────
    /// @brief Minimum ring radius scanned [mm].
    float r_min = 20.f;
    /// @brief Maximum ring radius scanned [mm].
    float r_max = 120.f;
    /// @brief Radius granularity in the accumulator [mm].
    float r_step = 1.f;
    /// @brief XY accumulator cell size [mm] — sets the discrete centre resolution.
    float cell_size = 3.f;

    // ── Per-frame ring-finder parameters ────────────────────────────
    //
    // **`time_cut_ns` is NOT a knob here.**  The Hough's time pre-cut
    // around the streaming-trigger's `fine_time` is inherited from
    // `StreamingTriggerConfigStruct::time_window_ns` — there's no
    // physical reason for the Hough hit-selection window to differ
    // from the score-stage clustering window.
    //
    // **`max_rings` is NOT a knob either.**  Hardcoded to 2 in the
    // algorithm because the detector has two Cherenkov radiators
    // (aerogel + gas); no physically realisable single-event
    // configuration produces more than two concentric rings.

    /// @brief Minimum fraction of currently-active hits in a peak accumulator cell.
    float threshold_fraction = 0.33f;

    /// @brief Slack on `min_hits` relative to `min_active`:
    /// `min_hits = min_active × this`.
    float min_hits_slack = 0.75f;

    /// @brief `min_active = ceil(this × N_active_cherenkov)`.  Both the
    /// "Hough may run at all" gate and the baseline for `min_hits_slack`.
    float hough_threshold_fraction = 0.004f;

    /// @brief Ring band width [mm] for hit assignment.
    float collection_radius = 7.5f;

    /// @brief Half-range [mm] of the X/Y axis on the per-ring centre QA
    /// histograms (the hists span @c [-this, +this]).  Not consumed by
    /// the Hough algorithm itself — purely a QA-axis knob.  Set wider
    /// than the expected centre spread; the bin width is automatically
    /// snapped to @c cell_size so n_bins × cell_size covers it exactly.
    float centre_xy_half_range_mm = 25.f;

    /// @brief Sliding-window size (in accumulator cells, per axis) used
    /// by MIST's peak finder.  Default `1` = legacy single-cell peak.
    /// Set to `2` (combined with halved `cell_size` / `r_step`) to
    /// enable sub-cell aggregation — the peak finder then sums a
    /// 2×2×2 sub-cell window at every position on the finer grid and
    /// reports the position with the maximum aggregated count.  Volume
    /// equivalence is the caller's responsibility: `aggregation_window_cells = 2`
    /// at half-sized cells covers the same physical volume as the
    /// single-cell finder at the original cell size, so `min_hits`
    /// and `threshold_fraction` retain their physical meaning.
    /// Diagnostic evidence and design in
    /// `include/triggers/streaming/DISCUSSION.md` § 2.3.1.
    int aggregation_window_cells = 1;

    /// @brief Half-width [mm] of extra (x, y) padding around the hit
    /// bounding box passed to MIST's `build_lut`.  Negative (default)
    /// keeps the legacy behaviour of padding by `r_max` — safest but
    /// bloats the accumulator for detectors whose ring centres are
    /// physically constrained near the detector plane.  Picking a
    /// smaller value shrinks `n_cells` and speeds up voting + the SAT
    /// peak finder proportionally.  Suggested rule of thumb: set this
    /// to ~1.5–2× the observed centre spread in `ring_X/Y_first` QA
    /// hists, well below `r_max`.  No effect on physics if the chosen
    /// value comfortably brackets the real ring centres.
    float centre_padding_mm = -1.f;

    // ── fit_circle initial guess ────────────────────────────────────
    /// @brief Initial centre X [mm] for the per-ring circle fit.
    float fit_circle_init_x = 0.f;
    /// @brief Initial centre Y [mm] for the per-ring circle fit.
    float fit_circle_init_y = 0.f;
    /// @brief Initial radius [mm] for the per-ring circle fit.
    float fit_circle_init_r = 50.f;
};

/**
 * @brief Parse the @c [streaming_hough] table from a TOML configuration file.
 *
 * Missing keys fall back to the defaults in @ref StreamingHoughConfigStruct.
 *
 * @param config_file Path to the TOML configuration file
 *                    (defaults to @c "conf/streaming.toml").
 * @return Populated @ref StreamingHoughConfigStruct.
 */
StreamingHoughConfigStruct
streaming_hough_conf_reader(std::string config_file = "conf/streaming.toml");

// =========================================================================
//  Recodata live-QA pipeline configuration (`[recodata]` block)
// =========================================================================

/**
 * @brief Knobs for the recodata-side live-QA pipeline.
 *
 * V1 minimum-viable scope (see `include/triggers/streaming/DISCUSSION.md`
 * Drives the coverage map / `eff(R)` / radial(R) / N_photons
 * machinery that runs inline in `recodata_writer` so beam-test
 * operators can see Cherenkov physics observables without a separate
 * offline analysis pass.
 *
 * Centre convention:  the coverage map and `eff(R)` are computed once
 * at writer init with a **fixed nominal centre** (the beam-axis
 * projection on the detector plane, typically `(0, 0)`).  Per-hit
 * `(R, φ)` and per-ring `f_coverage` use the **per-event fit-refined
 * Hough centre** (re-computed by recodata via `fit_circle` on the
 * mask-tagged hits — no `TriggerEvent` schema bump).  The `eff(R)`
 * discrepancy from this mismatch is < 1 % at the ~10–25 mm centre
 * wander observed in `ring_X/Y_first_hough`
 *
 * φ-gap (in_gap / ex_gap) and sensor-model (k1350 / k1375) splits
 * from the offline `photon_number_new.cpp` macro are **not** in V1.
 * Surface them when the V1 plots demand a finer breakdown.
 */
struct RecodataConfigStruct
{
    // ── Coverage map geometry ───────────────────────────────────────
    /// @brief Coverage map azimuthal binning over @c [-π, π].  Default
    /// 360 = 1°/bin, matches the offline macro.  Coarser is fine for
    /// smooth `eff(R)`; finer only helps if you also intend to read
    /// the 2D coverage map directly.
    int n_phi_bins_coverage = 360;

    /// @brief Coverage map radial bin count over `[r_min, r_max]`.
    /// 80 bins over 25–125 mm = 1.25 mm/bin, matches the macro.
    int n_r_bins_coverage = 80;

    /// @brief Coverage / radial map lower R edge [mm].
    float r_min_coverage_mm = 25.f;

    /// @brief Coverage / radial map upper R edge [mm].
    float r_max_coverage_mm = 125.f;

    /// @brief Channel pixel half-width [mm] (3 mm pitch → 1.5).  Used
    /// by both `build_coverage_map` and `azimuthal_coverage_fraction`.
    float channel_half_width_mm = 1.5f;

    /// @brief Nominal ring centre X [mm] for coverage / `eff(R)`.
    /// Beam-axis projection on the detector plane.  Default 0 = beam
    /// centred; override for off-axis beam runs.
    float nominal_centre_x_mm = 0.f;

    /// @brief Nominal ring centre Y [mm].  See @ref nominal_centre_x_mm.
    float nominal_centre_y_mm = 0.f;

    // ── Per-ring photon counting ────────────────────────────────────
    /// @brief Bandwidth [mm] used by `azimuthal_coverage_fraction`:
    /// a channel counts as "on the arc" if `||r_ch - R|| < delta_r`.
    /// Should match the upstream `collection_radius` from
    /// `[streaming_hough]` so the coverage estimate is consistent
    /// with the hit-assignment cut.
    float delta_r_for_coverage_mm = 3.f;

    /// @brief Minimum hits per ring for the re-fit to be attempted.
    /// Below this we skip the ring (and emit no N_photons / radial
    /// fill).  Matches the upstream `min_hits` floor in the Hough
    /// stage so the two are consistent.
    int min_hits_per_ring = 5;

    /// @brief Skip channels with `r_channel < this` when building the
    /// coverage map.  Default 0 = no extra filter (the loose
    /// `|x|<5 && |y|<5` filter from `index_to_hit_xy` construction
    /// remains in effect).
    ///
    /// When the coverage map shows a "low-R bump" at small `R` that
    /// doesn't correspond to any physical channel in the detector
    /// geometry — typically because `Mapping` returns slightly-bogus
    /// positions for uncalibrated / unused channel slots — bump this
    /// above the bump's R to crop it out.  The bump appears at
    /// `R ≈ √(x²+y²)` of the offending channels, so set the knob
    /// just above the smallest *legitimate* `r_channel` in your
    /// detector.
    ///
    /// Diagnostic to identify problem channels: at startup,
    /// `recodata_writer` logs the 10 smallest-r channels in
    /// `index_to_hit_xy`.  Cross-check against detector geometry.
    float min_channel_r_for_coverage_mm = 0.f;

    // ── Fast-feedback QA tuning ─────────────────────────────────────
    /// @brief Skip the leave-one-out residual loop in
    /// `compute_ring_fit_pure` when @c true.  Saves ~N extra
    /// `fit_circle` calls per ring per event — the biggest single
    /// speedup lever for QA mode.
    ///
    /// **Consequence:** the per-hit `h_residual_vs_n_*` TH2Fs stay
    /// empty, so the σ_photon LOO fit (`fit_sigma_vs_n`) skips
    /// silently and no `_sigma_photon_mm` TNamed scalars are emitted.
    /// QA-mode output therefore has **no σ_photon measurement** — the
    /// trade-off matches the rest of the QA path (rough numbers, fast
    /// feedback).
    ///
    /// Default `false` keeps production behaviour intact; set to
    /// `true` in `conf/QA/recodata.toml` to enable for `--QA` mode.
    bool skip_loo_residuals = false;
};

/**
 * @brief Parse the @c [recodata] table from a TOML configuration file.
 *
 * Missing keys fall back to the defaults in @ref RecodataConfigStruct.
 * The parser echoes loaded values via `mist::logger::info` at startup
 * — same "did my edit take effect?" diagnostic as
 * @ref streaming_hough_conf_reader.
 *
 * @param config_file Path to the TOML configuration file
 *                    (defaults to @c "conf/recodata.toml").
 * @return Populated @ref RecodataConfigStruct.
 */
RecodataConfigStruct
recodata_conf_reader(std::string config_file = "conf/recodata.toml");

// =========================================================================
//  Run metadata
// =========================================================================

/// @brief Optical radiator properties for one radiator layer.
struct RadiatorInfoStruct
{
    std::string type; ///< Radiator material identifier (e.g. @c "aerogel").
    std::string tag;  ///< Short label used in histogram naming.
    double refindex;  ///< Refractive index at the nominal beam energy.
    double depth;     ///< Radiator depth along the beam axis [cm].
    std::string side; ///< Detector side (@c "left" / @c "right").
};

/**
 * @brief Complete per-run metadata record.
 *
 * All fields are populated by @ref RunInfo::read_database().
 * Missing TOML keys are inherited from the immediately preceding run entry.
 */
struct RunInfoStruct
{
    // -------------------------------------------------------------------------
    /** @name Beam configuration */
    /// @{
    std::string beam_polarity; ///< Beam particle sign (@c "+" or @c "-").
    int beam_energy;           ///< Nominal beam momentum [GeV/c].
    /// @}

    // -------------------------------------------------------------------------
    /** @name DAQ configuration */
    /// @{
    std::string rdo_firmware;    ///< RDO firmware version string.
    std::string timing_firmware; ///< Timing board firmware version string.
    int n_spills;                ///< Number of spills in the run.
    bool timing_on_axis;         ///< @c true if the timing channel is on the beam axis.
    int op_mode;                 ///< ALCOR operational mode index.
    int delta_thr;               ///< ALCOR Δ-threshold setting [LSB].
    /// @}

    // -------------------------------------------------------------------------
    /** @name Sensor conditions */
    /// @{
    double temperature; ///< SiPM temperature during the run [°C].
    double v_bias;      ///< SiPM bias voltage [V].
    /// @}

    // -------------------------------------------------------------------------
    /** @name Optics */
    /// @{
    int aerogel_mirror; ///< Aerogel mirror configuration index.
    int gas_mirror;     ///< Gas radiator mirror configuration index.
    /// @}

    // -------------------------------------------------------------------------
    /** @name Radiators */
    /// @{
    std::vector<RadiatorInfoStruct> radiators; ///< Ordered list of active radiator layers.
    /// @}
};

// =========================================================================

/**
 * @brief Static database of run metadata and named run lists.
 *
 * All methods are @c static; no instance is needed.  Call read_database()
 * once at startup, then query with get_run_info() or get_run_list().
 *
 * Missing fields in a run entry are inherited from the previous entry in
 * document order, allowing compact TOML files that only list deltas.
 */
class RunInfo
{
public:
    // -------------------------------------------------------------------------
    /** @name Database I/O */
    /// @{

    /**
     * @brief Parse a TOML run-database file and populate the internal map.
     *
     * Reads the @c [runs] table.  Each sub-table key becomes the run ID.
     * Fields absent from a run entry are copied from the preceding entry.
     *
     * @param filename Path to the TOML database file.
     */
    static void read_database(std::string filename);

    /// @brief Clear all entries from the run-info database.
    static void clear_database() { run_info_database.clear(); }

    /**
     * @brief Retrieve the metadata record for @p run_id.
     * @param run_id Run identifier string (must match a key in the TOML file).
     * @return The @ref RunInfoStruct, or @c std::nullopt if not found.
     */
    static const std::optional<RunInfoStruct> get_run_info(const std::string &run_id);

    /// @}

    // -------------------------------------------------------------------------
    /** @name Run list management */
    /// @{

    /**
     * @brief Parse a TOML run-list file and populate the internal list map.
     *
     * Reads the @c [runlists] table.  Each sub-table key becomes the list name
     * and its @c runs array provides the ordered run IDs.
     *
     * @param runlist_file Path to the TOML run-list file.
     */
    static void read_runslists(std::string runlist_file);

    /**
     * @brief Retrieve the ordered run-ID list for @p runlist_name.
     * @param runlist_name List identifier (must match a key in the TOML file).
     * @return The vector of run IDs, or @c std::nullopt if not found.
     */
    static const std::optional<std::vector<std::string>> get_run_list(const std::string &runlist_name);

    /// @}

private:
    // -------------------------------------------------------------------------
    /** @name Static databases */
    /// @{

    static std::unordered_map<std::string, RunInfoStruct> run_info_database;            ///< run_id → metadata.
    static std::unordered_map<std::string, std::vector<std::string>> run_list_database; ///< list_name → run IDs.

    /// @}
};