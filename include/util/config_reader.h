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
#include "util/toml_utils.h"

// =========================================================================
//  Core tag set
// =========================================================================

/// Named roles that are mutually exclusive per (device, chip) pair.
static const std::set<std::string> lightdata_core_tags = {"timing", "tracking", "cherenkov"};

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

    std::vector<ReadoutConfigStruct> configs; ///< Ordered list of readout role assignments.
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
    uint16_t frame_size               = 1024; ///< Clock cycles per frame (320 MHz → 3.125 ns/cc).
    int      first_frames_trigger     = 5000; ///< Start-of-spill frames reserved for noise measurement.
    uint16_t afterpulse_deadtime      = 64;   ///< Afterpulse suppression deadtime (cc, ~200 ns).
    uint16_t trigger_secondary_window = 200;  ///< Secondary-trigger detection window (cc, ~625 ns).

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

    static std::unordered_map<std::string, RunInfoStruct> run_info_database;          ///< run_id → metadata.
    static std::unordered_map<std::string, std::vector<std::string>> run_list_database; ///< list_name → run IDs.

    /// @}
};