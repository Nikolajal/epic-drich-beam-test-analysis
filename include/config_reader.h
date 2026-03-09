#pragma once

/**
 * @file config_reader.h
 * @brief Run configuration and readout mapping utilities for the ePIC dRICH beam test.
 *
 * Provides:
 * - @ref readout_config_struct  – per-named-role (device, chip) assignment.
 * - @ref readout_config_list   – searchable container of readout configurations.
 * - @ref readout_config_reader – TOML file parser populating the above.
 * - @ref run_info_struct       – per-run beam, DAQ, sensor, and optics metadata.
 * - @ref run_info              – static database of run metadata and run lists.
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
struct readout_config_struct
{
    std::string name;                                      ///< Human-readable role name (e.g. @c "cherenkov").
    std::map<uint16_t, std::vector<uint16_t>> device_chip; ///< Active chips per device.

    readout_config_struct() = default;

    /**
     * @brief Construct with an explicit name and device–chip map.
     * @param _name       Role name.
     * @param _device_chip Initial device–chip assignment.
     */
    readout_config_struct(std::string _name, std::map<uint16_t, std::vector<uint16_t>> _device_chip);

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
 * @brief Searchable container of @ref readout_config_struct entries.
 *
 * All find methods return raw pointers into the internal vector; the pointers
 * remain valid as long as the list is not modified.
 */
class readout_config_list
{
public:
    // -------------------------------------------------------------------------
    /** @name Construction */
    /// @{

    readout_config_list() = default;

    /// @brief Construct from an existing vector of configs (moved in).
    explicit readout_config_list(std::vector<readout_config_struct> vec);

    /// @}

    // -------------------------------------------------------------------------
    /** @name Search */
    /// @{

    /// @brief First config whose @c name matches @p name, or @c nullptr.
    readout_config_struct *find_by_name(const std::string &name);

    /// @brief First config that contains @p device, or @c nullptr.
    readout_config_struct *find_by_device(uint16_t device);

    /// @brief All configs that contain @p device.
    std::vector<readout_config_struct *> find_all_by_device(uint16_t device);

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

    std::vector<readout_config_struct> configs; ///< Ordered list of readout role assignments.
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
    const std::map<std::string, readout_config_struct> &readout_config_utility,
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
 * @return Vector of @ref readout_config_struct, one per named role.
 */
std::vector<readout_config_struct> readout_config_reader(std::string config_file = "conf/readout_config.txt");

// =========================================================================
//  Run metadata
// =========================================================================

/// @brief Optical radiator properties for one radiator layer.
struct radiator_info_struct
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
 * All fields are populated by @ref run_info::read_database().
 * Missing TOML keys are inherited from the immediately preceding run entry.
 */
struct run_info_struct
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
    std::vector<radiator_info_struct> radiators; ///< Ordered list of active radiator layers.
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
class run_info
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
     * @return The @ref run_info_struct, or @c std::nullopt if not found.
     */
    static const std::optional<run_info_struct> get_run_info(const std::string &run_id);

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

    static std::unordered_map<std::string, run_info_struct> run_info_database;          ///< run_id → metadata.
    static std::unordered_map<std::string, std::vector<std::string>> run_list_database; ///< list_name → run IDs.

    /// @}
};