#include "alcor_finedata.h"
#include <mist/logger/logger.h>
#include "alcor_data.h"           // HitMask, BTANA_ALCOR_{ROLLOVER_TO_CC,CC_TO_NS}
#include "utility/global_index.h" // ::GlobalIndex full definition
#include "utility/toml_utils.h"   // toml_parse_with_cutoff for TOML calib reader
#include "TROOT.h"

#include <algorithm> // std::transform for the .toml extension check
#include <cctype>    // std::tolower
#include <fstream>
#include <memory>
#include <sstream>   // legacy text-format reader/writer
#include <stdexcept> // std::runtime_error — C4.4 schema/empty hard errors

// ---------------------------------------------------------------------------
// Calibration file format selection.
//
// New (v3) format: TOML.  Self-describing, parseable by any toml++ consumer.
// Legacy (v2) format: whitespace-separated "key method a -b sigma" per line.
// Both encode identical content; selection is by file-name extension.
// ---------------------------------------------------------------------------
namespace
{
constexpr const char *kCalibTomlSchema = "fine_calib.v3";

bool path_has_toml_extension(const std::string &path)
{
    if (path.size() < 5)
        return false;
    std::string tail = path.substr(path.size() - 5);
    std::transform(tail.begin(), tail.end(), tail.begin(),
                   [](unsigned char c)
                   { return std::tolower(c); });
    return tail == ".toml";
}
} // namespace

//  Open design items tracked in DISCUSSION.md:
//   - Merge AlcorFinedata with AlcorData (compress dual-struct overhead) —
//     deferred; see DISCUSSION § "merge AlcorFinedata + AlcorData".
//   - generate_calibration: existing implementation has unresolved
//     convergence issues; superseded for pulser runs by
//     pulser_calib_writer.  See DISCUSSION § "generate_calibration".

// =============================================================================
// AlcorFinedataStruct
// =============================================================================

AlcorFinedataStruct::AlcorFinedataStruct(const AlcorDataStruct &d)
{
    // Store the @ref GlobalIndex raw in TDC-level form with the validity
    // bit set.  Split-in-two trick is applied here at the conversion
    // boundary: hardware (chip_raw 0–7, channel_raw 0–31) ↦ logical
    // (chip 0–3, channel 0–63) for the current detector; identity
    // transform for the final 64-ch chip (gated by ::gidx::kUsesSplitInTwo).
    const int chip_raw = d.fifo / 4;
    const int channel_raw = d.pixel + 4 * d.column;
    const int chip_logical = ::gidx::kUsesSplitInTwo ? chip_raw / 2
                                                     : chip_raw;
    const int channel_log = ::gidx::kUsesSplitInTwo
                                ? channel_raw + 32 * (chip_raw % 2)
                                : channel_raw;
    GlobalIndex = ::GlobalIndex::from_components(
                      d.device, d.fifo, chip_logical, channel_log, d.tdc)
                      .raw();

    rollover = static_cast<uint32_t>(d.rollover);
    coarse = static_cast<uint16_t>(d.coarse);
    fine = static_cast<uint8_t>(d.fine);
    HitMask = d.HitMask;

    //  Position fields are NOT touched by this ctor — geometry is wired
    //  in later via Mapping::assign_position (src/mapping.cxx:103).  Until
    //  then, default to the same -999.f sentinel Mapping itself uses for
    //  unmapped channels.  Without this init, both floats are
    //  indeterminate (struct ctor is = default) and propagate UB into the
    //  TTree for timing_hits / tracking_hits / cherenkov_hits whenever
    //  the framer emplaces a hit before assign_position has run.
    hit_x = -999.f;
    hit_y = -999.f;
}

// =============================================================================
// AlcorFinedata — Derived timing getters
// =============================================================================

float AlcorFinedata::get_phase() const
{
    //  Fast path: once calibration is frozen (via freeze_calibration()),
    //  the table is immutable for the rest of the process — every reader
    //  can skip the std::shared_mutex acquisition entirely.  Per-Hit
    //  shared_lock was a real contention point on the 16-thread framer
    //  even with no contention against writers, because shared_mutex
    //  still does atomic RMW on its reader count
    //
    //  Slow path: during setup the table is still mutable, so we take
    //  the shared lock as before.
    //
    //  We access calibration_table_ DIRECTLY (not through get_param* /
    //  get_calibration_method) to avoid recursive locking on the
    //  non-reentrant std::shared_mutex.
    std::shared_lock<std::shared_mutex> lock(calibration_mutex, std::defer_lock);
    if (!is_calibration_frozen())
        lock.lock();

    //  Single fused lookup — was two finds (calibration_parameters +
    //  channel_calibration_method); fusing them is the whole point of this
    //  table (the per-hit cost floor, ~100M hits/run).
    auto calib_it = calibration_table_.find(get_global_index());
    if (calib_it == calibration_table_.end())
        return 0.f;

    const auto &calibration_parameter = calib_it->second.params;
    const auto method = calib_it->second.method;

    switch (method)
    {
    case CalibrationMethod::AlcorV2BaseCalib:
    {
        auto current_fine_value = static_cast<float>(get_fine());
        if (calibration_parameter[1] == calibration_parameter[0])
            return 0.f;
        if ((calibration_parameter[1] < current_fine_value) && (calibration_parameter[0] > current_fine_value))
            return -9999.f;
        auto phase = (current_fine_value - calibration_parameter[0]) / (calibration_parameter[1] - calibration_parameter[0]);
        phase -= calibration_parameter[2];
        return phase;
    }

    case CalibrationMethod::AlcorV2FitCalib:
    {
        auto current_fine_value = static_cast<float>(get_fine());
        return (current_fine_value * calibration_parameter[0] - calibration_parameter[1]);
    }

    default:
        return 0.f;
    }
}

// =============================================================================
// AlcorFinedata — Spatial getters (randomised)
// =============================================================================

float AlcorFinedata::get_hit_r_rnd(std::array<float, 2> v) const
{
    const float x = get_hit_x_rnd();
    const float y = get_hit_y_rnd();
    return std::hypot(x - v[0], y - v[1]);
}

float AlcorFinedata::get_hit_phi_rnd(std::array<float, 2> v) const
{
    const float x = get_hit_x_rnd();
    const float y = get_hit_y_rnd();
    return std::atan2(y - v[1], x - v[0]);
}

// =============================================================================
// AlcorFinedata — Calibration I/O
// =============================================================================

void AlcorFinedata::write_calib_to_file(const std::string &filename)
{
    //  TOML v3 is the ONLY supported on-disk format.  Reject anything
    //  that isn't ``*.toml`` up-front so callers can't silently emit
    //  the legacy text-v2 layout (which downstream readers now
    //  reject too).  Hard cut — see task #172 / DISCUSSION.md.
    if (!path_has_toml_extension(filename))
        throw std::runtime_error(
            "(AlcorFinedata::write_calib_to_file) refuse to write '" +
            filename + "': only the .toml v3 schema is supported. "
                       "Pass a path ending in .toml.");

    mist::logger::info(
        "(AlcorFinedata::write_calib_to_file) writing calibration to " +
        filename + "  [TOML v3]");

    std::ofstream calib_file(filename);
    if (!calib_file)
        throw std::runtime_error(
            "(AlcorFinedata::write_calib_to_file) cannot open " + filename);

    std::shared_lock<std::shared_mutex> lock(calibration_mutex);

    //  TOML v3 schema.  One [[entry]] table per GlobalIndex.  Field
    //  meanings (kept stable from the legacy text format so old
    //  parsers in plotting macros still recognise the values, only
    //  the surrounding syntax changed):
    //    key      = uint32_t GlobalIndex::raw()
    //    method   = CalibrationMethod enum int
    //    a        = slope (cc / fine bin)
    //    minus_b  = -intercept (so downstream's get_phase =
    //               fine*a - minus_b recovers c - (f*a + b))
    //    sigma    = per-pair residual sigma (cc)
    calib_file << "# fine_calib.toml — generated by AlcorFinedata::write_calib_to_file\n";
    calib_file << "# Schema documented at: include/alcor_finedata.h (read_calib_from_file)\n\n";
    calib_file << "schema = \"" << kCalibTomlSchema << "\"\n\n";
    for (const auto &[key, entry] : calibration_table_)
    {
        calib_file << "[[entry]]\n"
                   << "key     = " << key << "\n"
                   << "method  = " << static_cast<int>(entry.method) << "\n"
                   << "a       = " << entry.params[0] << "\n"
                   << "minus_b = " << entry.params[1] << "\n"
                   << "sigma   = " << entry.params[2] << "\n\n";
    }
}

void AlcorFinedata::read_calib_from_file(const std::string &filename, bool clear_first, bool overwrites)
{
    //  TOML v3 is the only supported on-disk format — see
    //  ``write_calib_to_file`` for the matching emit-side guard.
    //  Anything else (legacy ``fine_calib.txt``) is rejected up-front
    //  so a stale file path can't silently produce zero calibration
    //  entries downstream.  Operators with old .txt files must
    //  regenerate via ``pulser_calib_writer`` or the offline timing
    //  macro.  Hard cut — see task #172 / DISCUSSION.md.
    if (!path_has_toml_extension(filename))
        throw std::runtime_error(
            "(AlcorFinedata::read_calib_from_file) refuse to read '" +
            filename + "': only the .toml v3 schema is supported. "
                       "Regenerate this calibration with pulser_calib_writer.");

    std::unique_lock<std::shared_mutex> lock(calibration_mutex);
    if (clear_first)
        calibration_table_.clear();

    //  TOML v3 reader.  Looks for ``schema = "fine_calib.v3"`` plus a
    //  flat ``[[entry]]`` array.  Tolerates the schema key being
    //  missing (parses on a best-effort basis) but logs a warning.
    toml::table parsed;
    try
    {
        parsed = toml_parse_with_cutoff(filename);
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(
            "(AlcorFinedata::read_calib_from_file) cannot parse " + filename +
            " as TOML: " + err.what() + " — skipping load calibration");
        return;
    }

    //  C4.4 — promote schema-mismatch + zero-entry from warning to
    //  hard error.  `pulser_calib_writer` is the only emitter of v3 in
    //  this codebase, so a wrong schema means somebody hand-edited the
    //  file or copied in foreign data — silent best-effort parsing
    //  there has historically produced calibration tables that load
    //  but match nothing at lookup time (the published keys are wrong),
    //  which is worse than failing fast.  Missing `schema` key is
    //  still tolerated as a warning for old pre-v3 files.
    if (auto schema = parsed["schema"].value<std::string>())
    {
        if (*schema != kCalibTomlSchema)
            throw std::runtime_error(
                "(AlcorFinedata::read_calib_from_file) " + filename +
                " declares schema '" + *schema + "' — expected '" +
                kCalibTomlSchema + "'.  Refusing to load (C4.4).");
    }
    else
    {
        mist::logger::warning(
            "(AlcorFinedata::read_calib_from_file) " + filename +
            " has no `schema` key — best-effort parse "
            "(pre-v3 forward-compat).");
    }

    const auto *entries = parsed["entry"].as_array();
    if (!entries || entries->empty())
    {
        throw std::runtime_error(
            "(AlcorFinedata::read_calib_from_file) " + filename +
            " has no [[entry]] tables — refusing to load an empty "
            "calibration (C4.4).");
    }
    for (const auto &node : *entries)
    {
        const auto *entry = node.as_table();
        if (!entry)
            continue;
        const auto key = (*entry)["key"].value<int64_t>();
        const auto method_in = (*entry)["method"].value<int64_t>();
        const auto a = (*entry)["a"].value<double>();
        const auto minus_b = (*entry)["minus_b"].value<double>();
        const auto sigma = (*entry)["sigma"].value<double>();
        if (!key || !method_in || !a || !minus_b || !sigma)
            continue;
        const auto key_u32 = static_cast<uint32_t>(*key);
        if (calibration_table_.count(key_u32) && !overwrites)
            continue;
        calibration_table_[key_u32] = CalibrationEntry{
            {static_cast<float>(*a),
             static_cast<float>(*minus_b),
             static_cast<float>(*sigma)},
            static_cast<CalibrationMethod>(*method_in)};
    }
}

void AlcorFinedata::switch_to_fit_v2(uint32_t GlobalIndex, CalibrationMethod calibration_type, float angular_coeff, float offset, float sigma)
{
    set_calibration_method(GlobalIndex, CalibrationMethod::AlcorV2FitCalib);
    auto prev_min = get_param0(GlobalIndex) < 1 ? 30 : get_param0(GlobalIndex);
    auto prev_max = get_param1(GlobalIndex) < 1 ? 100 : get_param1(GlobalIndex);
    set_param0(GlobalIndex, angular_coeff);
    set_param1(GlobalIndex, -(angular_coeff * (prev_max + prev_min) * 0.5 + offset));
    set_param2(GlobalIndex, sigma);
}

void AlcorFinedata::generate_calibration(TH2F *calibration_histogram, bool overwrite_calibration)
{
    //  Restore the global ROOT batch state on every return path.
    const auto previous_batch_state = gROOT->IsBatch();
    gROOT->SetBatch(true);
    struct BatchStateGuard
    {
        bool previous;
        ~BatchStateGuard() { gROOT->SetBatch(previous); }
    } batch_guard{previous_batch_state};

    if (!calibration_histogram)
    {
        mist::logger::warning(
            "(AlcorFinedata::generate_calibration) invalid histogram, aborting calibration");
        return;
    }

    mist::logger::info(
        "(AlcorFinedata::generate_calibration) starting fine data calibration from: " +
        std::string(calibration_histogram->GetName()));

    std::unique_lock<std::shared_mutex> lock(calibration_mutex);
    if (overwrite_calibration)
    {
        mist::logger::info(
            "(AlcorFinedata::generate_calibration) clearing previous calibration");
        calibration_table_.clear();
        //  A full overwrite invalidates the low-stats cache: callers
        //  passing `overwrite=true` (offline `recodata_writer`, fresh
        //  pulser-calib regeneration) expect every channel re-examined
        //  from scratch, NOT inherited from a prior partial run.  Also
        //  resets the retry counter so the first non-overwrite call
        //  after this starts the retry-every-N clock at zero.
        low_stats_keys_.clear();
        low_stats_call_count_ = 0;
        low_stats_cached_skips_ = 0;
    }
    else
    {
        //  Retry policy: drop the cache every N-th call so channels
        //  that crossed the >=250 gate since their last skip get
        //  re-examined.  `period == 0` disables retries (the default —
        //  see header).  Increment first so the check below reads
        //  "this is call number `count`": with period=N, clearing
        //  happens at the start of calls N, 2N, 3N, ...  period=1
        //  therefore means "always retry" (every call clears).
        ++low_stats_call_count_;
        if (low_stats_retry_period_ > 0 &&
            (low_stats_call_count_ % low_stats_retry_period_) == 0)
        {
            mist::logger::info(
                "(AlcorFinedata::generate_calibration) low-stats retry — "
                "clearing " +
                std::to_string(low_stats_keys_.size()) +
                " cached channels (call " +
                std::to_string(low_stats_call_count_) + ", period " +
                std::to_string(low_stats_retry_period_) + ")");
            low_stats_keys_.clear();
        }
    }

    //  Two-sigmoid edge model: rising edge at p[2], falling edge at p[3];
    //  p[0] is the plateau amplitude, p[1] is the (shared) edge sharpness.
    //  Stored as the V2 BASE calibration:  param0 = first_edge (fine bin),
    //  param1 = last_edge,  param2 = offset (default 0).  Consumers in
    //  AlcorFinedata::get_phase compute
    //     phase = (fine - p0) / (p1 - p0) - p2
    //  for the AlcorV2BaseCalib method.
    TF1 fine_dist_fit_function(
        "fine_dist_fit_function",
        "[0]*((1./(1+TMath::Exp(-[1]*(x-[2]))))-(1./(1+TMath::Exp(-[1]*(x-[3])))))",
        0, 256);
    fine_dist_fit_function.SetParLimits(0, 0., 1e6);
    fine_dist_fit_function.SetParLimits(1, 0.5, 5.);
    fine_dist_fit_function.SetParLimits(2, 10., 50.);
    fine_dist_fit_function.SetParLimits(3, 80., 120.);

    const auto calibrated_channels_before = calibration_table_.size();
    long n_skipped_low_stats = 0;
    long n_skipped_low_stats_cached = 0;
    long n_skipped_unconverged = 0;
    long n_skipped_no_global_index = 0;
    long n_fit_exceptions = 0; // C4.5

    //  The histogram's x-axis is filled via `GlobalIndex::tdc_ordinal()`
    //  in parallel_streaming_framer.cxx, so xbin-1 is the dense
    //  counter-style integer (per-TDC bin).  Reconstruct the packed
    //  `GlobalIndex::raw()` value so downstream consumers — which key
    //  the calibration table by the packed raw, NOT the legacy ordinal
    //  — can look this entry up.  Until this fix, the calibration
    //  written here used the bare xbin-1 as the key and never matched
    //  any production query.
    const int n_bins_x = calibration_histogram->GetNbinsX();
    for (int xbin = 1; xbin <= n_bins_x; ++xbin)
    {
        const int ordinal = xbin - 1;
        const auto global_index = ::GlobalIndex::try_from_tdc_ordinal(ordinal);
        if (!global_index)
        {
            ++n_skipped_no_global_index;
            continue;
        }
        const uint32_t key = global_index->raw();

        if (!overwrite_calibration && calibration_table_.count(key))
            continue;

        //  Low-stats cache short-circuit — skip the 256-bin ProjectionY
        //  allocation for channels we already know to be below the
        //  >=250 gate.  This is the whole point of the cache; the
        //  ProjectionY was the dominant cost on the per-spill --QA
        //  cascade (see header §Low-stats channel cache).
        if (!overwrite_calibration && low_stats_keys_.count(key))
        {
            ++n_skipped_low_stats_cached;
            ++low_stats_cached_skips_;
            continue;
        }

        //  Per-channel projection of the (channel, fine) plane.
        //  unique_ptr handles every continue path including the
        //  convergence-skip branch below.
        std::unique_ptr<TH1D> projection(
            calibration_histogram->ProjectionY(
                TString::Format("tmp_%i", xbin).Data(), xbin, xbin));
        projection->SetDirectory(nullptr); // don't pollute gDirectory
        if (projection->GetEntries() < 250)
        {
            ++n_skipped_low_stats;
            //  Cache the channel so future calls skip the ProjectionY.
            //  `overwrite=true` callers populate too: even though the
            //  cache was just cleared above, the next non-overwrite
            //  call benefits from a hot cache.
            low_stats_keys_.insert(key);
            continue;
        }

        //  Crude edge finder for the fit seed: first fine bin > 5
        //  entries on the rising side, first 0-entry bin after that
        //  on the falling side.
        double first_edge_seed = 0;
        double last_edge_seed = 0;
        for (int ibin = 1; ibin <= projection->GetNbinsX(); ++ibin)
        {
            if (projection->GetBinContent(ibin) > 5 && first_edge_seed < 1)
                first_edge_seed = projection->GetBinCenter(ibin);
            if (projection->GetBinContent(ibin) == 0 &&
                first_edge_seed > 0 && last_edge_seed < 1)
            {
                last_edge_seed = projection->GetBinCenter(ibin);
                break;
            }
        }
        fine_dist_fit_function.SetParameter(0, projection->GetMaximum());
        fine_dist_fit_function.SetParameter(1, 2.5);
        fine_dist_fit_function.SetParameter(2, first_edge_seed);
        fine_dist_fit_function.SetParLimits(2, first_edge_seed - 3, first_edge_seed + 3);
        fine_dist_fit_function.SetParameter(3, last_edge_seed);
        fine_dist_fit_function.SetParLimits(3, last_edge_seed - 3, last_edge_seed + 3);
        //  C4.5 — wrap TF1::Fit in try/catch.  ROOT can throw on a
        //  degenerate projection (e.g. a histogram with a single
        //  populated bin survives the >=250-entries gate via
        //  pathological overflow but the fit still fails to set up).
        //  An uncaught throw aborts the whole calibration; here we
        //  drop just this channel and keep going.
        try
        {
            projection->Fit(&fine_dist_fit_function, "Q");
        }
        catch (const std::exception &e)
        {
            ++n_fit_exceptions;
            continue;
        }

        //  Iterate until the recovered edge span is close to the
        //  expected 62.5 fine bins.  Each retry JITTERS the edge seeds
        //  in opposite directions (first_edge_seed + δ, last_edge_seed
        //  − δ) so Minuit lands in a different starting basin instead
        //  of re-converging to the same point — the cap of 5 was a
        //  no-op before this change because the fit is deterministic
        //  from a fixed start.  Tightened cap to 3 after instrumented
        //  measurement showed the jittered iterations
        //  converge within 2–3 attempts in the cold-spill regime that
        //  dominates total fit cost (see BACKLOG P 0.35).  Saves
        //  ~1.5–2 s on the cold spill, negligible on warm spills.
        constexpr float kExpectedEdgeSpanFineBins = 62.5f;
        constexpr float kEdgeSpanTolerance = 10.0f;
        constexpr float kRetrySeedJitterFineBins = 1.5f;
        constexpr int kFitRetryCap = 3;
        float first_edge = static_cast<float>(fine_dist_fit_function.GetParameter(2));
        float last_edge = static_cast<float>(fine_dist_fit_function.GetParameter(3));
        for (int retry = 0; retry < kFitRetryCap; ++retry)
        {
            if (std::fabs(last_edge - first_edge - kExpectedEdgeSpanFineBins) < kEdgeSpanTolerance)
                break;
            //  Deterministic seed jitter — alternating sign per retry
            //  drives the fit to opposite sides of the original seed
            //  basin.  Bounded ParLimits keep the perturbation valid.
            const float sign = (retry % 2 == 0) ? +1.f : -1.f;
            const float dx = sign * static_cast<float>(retry + 1) * kRetrySeedJitterFineBins;
            fine_dist_fit_function.SetParameter(2, first_edge_seed + dx);
            fine_dist_fit_function.SetParameter(3, last_edge_seed - dx);
            //  C4.5 — same try/catch as the initial fit; on exception
            //  abandon the retry loop, treat as unconverged.
            try
            {
                projection->Fit(&fine_dist_fit_function, "Q");
            }
            catch (const std::exception &e)
            {
                ++n_fit_exceptions;
                break;
            }
            first_edge = static_cast<float>(fine_dist_fit_function.GetParameter(2));
            last_edge = static_cast<float>(fine_dist_fit_function.GetParameter(3));
        }
        if (std::fabs(last_edge - first_edge - kExpectedEdgeSpanFineBins) > kEdgeSpanTolerance)
        {
            ++n_skipped_unconverged;
            continue;
        }

        calibration_table_[key] = CalibrationEntry{
            {first_edge, last_edge, 0.0f}, CalibrationMethod::AlcorV2BaseCalib};
    }

    const auto calibrated_channels_after = calibration_table_.size();
    mist::logger::info(
        "(AlcorFinedata::generate_calibration) finished — updated " +
        std::to_string(calibrated_channels_after - calibrated_channels_before) +
        " channels  (before=" + std::to_string(calibrated_channels_before) +
        " after=" + std::to_string(calibrated_channels_after) +
        ")  skipped: " + std::to_string(n_skipped_low_stats) + " low-stats (fresh), " +
        std::to_string(n_skipped_low_stats_cached) + " low-stats (cached), " +
        std::to_string(n_skipped_unconverged) + " edge-span unconverged, " +
        std::to_string(n_fit_exceptions) + " TF1::Fit exceptions, " +
        std::to_string(n_skipped_no_global_index) + " out-of-range ordinal" +
        "  [cache_size=" + std::to_string(low_stats_keys_.size()) + "]");
}

// The AlcorFinedata ring-finding adapter (`alcor_find_rings_hough`) lives
// inline in `src/triggers/streaming/hough.cxx`, the only consumer.
// See the header comment block at the top of `alcor_finedata.h` and
// `include/triggers/streaming/DISCUSSION.md` § 2 for context.
// =============================================================================
// CONVENTION-BREAK NOTICE — these methods belong inline in the header
// =============================================================================
//
// Method bodies below were moved out of `include/alcor_finedata.h` during
// the IWYU sweep on the theory that ROOT's dict autoparse
// needed the header to be self-sufficient.  That theory was a
// misdiagnosis: the actual failures it chased (`HitMask` /
// `BTANA_ALCOR_*` / `::GlobalIndex` unknown when a macro loads via
// `#include "../lib_loader.h"`) were a `dict/alcor_linkdef.h` autoload
// problem, not a header self-sufficiency problem.
//
// PROJECT CONVENTION: framework headers may include each other freely;
// macros rely on cling autoload via the dict's rootmap to pull in types
// on first reference.  Per that convention, the canonical home for
// these short one-liners is the header (where the original inline
// definitions lived).
//
// These bodies are not being moved back blindly — see the matching
// notice in `include/alcor_finedata.h`.  If you re-inline them, do it
// only after also confirming the LinkDef covers `HitMask` / `AlcorData`
// / `GlobalIndex` so `.x macro.cpp` mode keeps working.
// =============================================================================

float AlcorFinedata::get_time() const
{
    return static_cast<float>(BTANA_ALCOR_ROLLOVER_TO_CC) *
               static_cast<float>(get_rollover()) +
           static_cast<float>(get_coarse()) - get_phase();
}

float AlcorFinedata::get_time_ns() const
{
    return BTANA_ALCOR_CC_TO_NS * get_time();
}

int AlcorFinedata::get_tdc() const { return ::GlobalIndex(get_global_index()).tdc(); }
int AlcorFinedata::get_device() const { return ::GlobalIndex(get_global_index()).device(); }
int AlcorFinedata::get_fifo() const { return ::GlobalIndex(get_global_index()).fifo(); }
int AlcorFinedata::get_chip() const { return ::GlobalIndex(get_global_index()).real_chip(); }
int AlcorFinedata::get_eo_channel() const { return ::GlobalIndex(get_global_index()).eo_channel(); }
int AlcorFinedata::get_column() const { return ::GlobalIndex(get_global_index()).column(); }
int AlcorFinedata::get_pixel() const { return ::GlobalIndex(get_global_index()).pixel(); }
int AlcorFinedata::get_device_index() const { return ::GlobalIndex(get_global_index()).device_index(); }
int AlcorFinedata::get_global_channel_index() const { return ::GlobalIndex(get_global_index()).channel_ordinal(); }

void AlcorFinedata::add_mask_bit(HitMask bit) { internal_data.HitMask |= (1u << bit); }
void AlcorFinedata::clear_mask_bit(HitMask bit) { internal_data.HitMask &= ~(1u << bit); }
bool AlcorFinedata::has_mask_bit(HitMask bit) const { return (internal_data.HitMask >> bit) & 1u; }
bool AlcorFinedata::is_ring_tag_first() const { return has_mask_bit(HitmaskRingTagFirst); }
bool AlcorFinedata::is_ring_tag_second() const { return has_mask_bit(HitmaskRingTagSecond); }

bool AlcorFinedata::is_cross_talk() const { return has_mask_bit(HitmaskCrossTalk); }
bool AlcorFinedata::is_afterpulse() const { return has_mask_bit(HitmaskAfterpulse); }
bool AlcorFinedata::is_afterpulse_near() const { return has_mask_bit(HitmaskAfterpulseNear); }
bool AlcorFinedata::is_afterpulse_far() const { return has_mask_bit(HitmaskAfterpulseFar); }
bool AlcorFinedata::is_part_lane() const { return has_mask_bit(HitmaskPartLane); }
bool AlcorFinedata::is_dead_lane() const { return has_mask_bit(HitmaskDeadLane); }
bool AlcorFinedata::is_secondary_orphan() const { return has_mask_bit(HitmaskSecondaryOrphan); }
bool AlcorFinedata::is_leading_orphan() const { return has_mask_bit(HitmaskLeadingOrphan); }
bool AlcorFinedata::is_tot_saturated() const { return has_mask_bit(HitmaskTotSaturated); }

void AlcorFinedata::set_streaming_ring_trigger_mask() { add_mask_bit(HitmaskStreamingRingTrigger); }
