/**
 * @file triggers.h
 * @brief Header-only trigger definitions, registry, and TOML-based configuration reader.
 *
 * Provides:
 * - @ref trigger_number   : enum of known hardware trigger types
 * - @ref trigger_event    : per-event trigger data (index, coarse time, fine time)
 * - @ref trigger_config   : per-trigger configuration loaded from a TOML file
 * - @ref trigger_registry : runtime lookup table mapping trigger values to names
 * - @ref trigger_conf_reader : reads a TOML config file and returns a list of trigger configs
 *
 * ### TOML config format
 * @code{.toml}
 * [[trigger]]
 * name   = "luca_and_finger"
 * index  = 0
 * device = 196
 * delay  = 117
 *
 * [[trigger]]
 * name   = "broad_scintillator"
 * index  = 1
 * device = 197
 * delay  = 117
 * @endcode
 *
 * ### Dependencies
 * - [toml++](https://github.com/marzer/tomlplusplus) (single-header, C++17): `toml.hpp` must be on the include path.
 * - ROOT: `TH2.h` for histogram axis labelling in @ref trigger_registry::label_axes.
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
#include <toml++/toml.h>
#include "TH2.h"

// ============================================================
//  Trigger enum & compile-time metadata
// ============================================================

/**
 * @brief Enumeration of known hardware trigger types.
 *
 * Values in the 100–199 range are physics/DAQ triggers;
 * 200+ are spill-level signals; 255 is the catch-all unknown.
 *
 * Config-defined triggers must use indices in [0, 99] — this range
 * is intentionally left out of the enum and managed at runtime.
 */
enum trigger_number : uint8_t
{
    _TRIGGER_FIRST_FRAMES_ = 100,         ///< First frames of a run
    _TRIGGER_TIMING_ = 101,               ///< Timing trigger
    _TRIGGER_TRACKING_ = 102,             ///< Tracking trigger
    _TRIGGER_RING_FOUND_ = 103,           ///< Ring-finding trigger
    _TRIGGER_STREAMING_RING_FOUND_ = 104, ///< Streaming ring-finding trigger
    _TRIGGER_HOUGH_RING_FOUND_ = 105,     ///< Hough-transform ring-finding trigger
    _TRIGGER_START_OF_SPILL_ = 200,       ///< Start-of-spill signal
    _TRIGGER_UNKNOWN_ = 255               ///< Unknown / unmapped trigger
};

/// @cond INTERNAL
/// Ordered list of all default trigger enum values (mirrors @ref default_names).
constexpr trigger_number all_default_triggers[] = {
    _TRIGGER_FIRST_FRAMES_,
    _TRIGGER_TIMING_,
    _TRIGGER_TRACKING_,
    _TRIGGER_RING_FOUND_,
    _TRIGGER_STREAMING_RING_FOUND_,
    _TRIGGER_HOUGH_RING_FOUND_,
    _TRIGGER_START_OF_SPILL_,
    _TRIGGER_UNKNOWN_};

/// Human-readable names matching @ref all_default_triggers entry-for-entry.
constexpr const char *default_names[] = {
    "FIRST_FRAMES", "TIMING", "TRACKING",
    "RING_FOUND", "STREAMING_RING_FOUND",
    "HOUGH_RING_FOUND",
    "START_OF_SPILL", "UNKNOWN"};

/// Total number of built-in default triggers.
constexpr int n_default_triggers = std::size(all_default_triggers);
/// @endcond

/**
 * @brief Returns the index of a trigger in the default table at compile time.
 * @param t  Trigger enum value to look up.
 * @return   Index in @ref all_default_triggers, or -1 if not found.
 */
constexpr int default_trigger_index(trigger_number t)
{
    for (int i = 0; i < n_default_triggers; ++i)
        if (all_default_triggers[i] == t)
            return i;
    return -1;
}

// ============================================================
//  Data structs
// ============================================================

/**
 * @brief Per-event trigger data attached to a decoded data frame.
 *
 * Lightweight struct created once per event by the DAQ readout loop.
 * Carries only the fields present in the raw data stream: the hardware
 * trigger index and the two-component timestamp (coarse + fine).
 */
struct trigger_event
{
    uint8_t index;   ///< Hardware trigger index
    uint16_t coarse; ///< Coarse timestamp (DAQ clock ticks)
    float fine_time; ///< Fine timestamp correction (ns)

    trigger_event() = default;

    /**
     * @brief Construct with index only; timestamps default to zero.
     * @param idx  Hardware trigger index.
     */
    trigger_event(uint8_t idx)
        : index(idx), coarse(0), fine_time(0.f) {}

    /**
     * @brief Construct with index and coarse timestamp.
     * @param idx  Hardware trigger index.
     * @param crs  Coarse timestamp.
     */
    trigger_event(uint8_t idx, uint16_t crs)
        : index(idx), coarse(crs), fine_time(0.f) {}

    /**
     * @brief Construct with all fields.
     * @param idx   Hardware trigger index.
     * @param crs   Coarse timestamp.
     * @param fine  Fine time correction (ns).
     */
    trigger_event(uint8_t idx, uint16_t crs, float fine)
        : index(idx), coarse(crs), fine_time(fine) {}
};

/**
 * @brief Static configuration for a single trigger channel, loaded from TOML.
 *
 * Each entry corresponds to one `[[trigger]]` block in the config file.
 * Lives in the @ref trigger_registry; not created per-event.
 */
struct trigger_config
{
    std::string name; ///< Human-readable trigger label (e.g. `"luca_and_finger"`)
    uint8_t index;    ///< Trigger index in [0, 99] used in the data stream
    uint16_t delay;   ///< Delay applied to this trigger channel (DAQ units)
    uint16_t device;  ///< Hardware device ID that produces this trigger

    trigger_config() = default;

    /**
     * @brief Construct from all fields.
     * @param _name    Trigger name.
     * @param _index   Trigger index.
     * @param _delay   Trigger delay.
     * @param _device  Source device ID.
     */
    trigger_config(const std::string &_name,
                   uint8_t _index,
                   uint16_t _delay,
                   uint16_t _device)
        : name(_name), index(_index), delay(_delay), device(_device) {}
};

// ============================================================
//  Runtime trigger registry
// ============================================================

/**
 * @brief Runtime lookup table that maps trigger values to names and positions.
 *
 * Built once after the TOML config is loaded. Config-defined triggers occupy
 * the first slots (in declaration order); the built-in defaults follow.
 * This ordering governs histogram bin layout when calling @ref label_axes.
 */
struct trigger_registry
{
    /// Ordered list of `{trigger value, name}` pairs.
    std::vector<std::pair<uint8_t, std::string>> triggers;

    trigger_registry() = default;

    /**
     * @brief Build the registry from a list of config-defined triggers.
     *
     * Config triggers are inserted first (preserving TOML declaration order),
     * followed by all entries in @ref all_default_triggers. Duplicate values
     * are not filtered — config entries take priority by appearing first.
     *
     * @param config_triggers  Triggers parsed from the TOML config file.
     */
    explicit trigger_registry(const std::vector<trigger_config> &config_triggers)
    {
        for (const auto &t : config_triggers)
            triggers.push_back({t.index, t.name});
        for (int i = 0; i < n_default_triggers; ++i)
            triggers.push_back({all_default_triggers[i], default_names[i]});
    }

    /**
     * @brief Returns the registry position for a given trigger value.
     *
     * If the value is not found, emits a warning and falls back to the
     * position of @ref _TRIGGER_UNKNOWN_ in the default table.
     *
     * @param trigger_value  Raw 8-bit trigger value from the data stream.
     * @return               Position in @ref triggers.
     */
    int index_of(uint8_t trigger_value) const
    {
        for (int i = 0; i < static_cast<int>(triggers.size()); ++i)
            if (triggers[i].first == trigger_value)
                return i;

        std::cerr << "[WARN] trigger_registry: unknown trigger value "
                  << static_cast<int>(trigger_value)
                  << " — falling back to UNKNOWN\n";
        return default_trigger_index(_TRIGGER_UNKNOWN_);
    }

    /**
     * @brief Returns the name associated with a trigger value.
     * @param trigger_value  Raw 8-bit trigger value from the data stream.
     * @return               Name string, or `"UNKNOWN"` if not found.
     */
    std::string name_of(uint8_t trigger_value) const
    {
        return triggers[index_of(trigger_value)].second;
    }

    /// @return Total number of triggers in the registry (config + defaults).
    int size() const { return static_cast<int>(triggers.size()); }

    /**
     * @brief Labels both axes of a 2D ROOT histogram with trigger names.
     *
     * Bin @c i+1 on both X and Y axes is set to the name of the i-th
     * trigger in the registry. Intended for trigger-vs-trigger correlation plots.
     *
     * @param h  Pointer to the histogram whose axes will be labelled.
     */
    void label_axes(TH2F *h) const
    {
        for (int i = 0; i < static_cast<int>(triggers.size()); ++i)
        {
            h->GetXaxis()->SetBinLabel(i + 1, triggers[i].second.c_str());
            h->GetYaxis()->SetBinLabel(i + 1, triggers[i].second.c_str());
        }
    }
};

// ============================================================
//  TOML config reader
// ============================================================

/**
 * @brief Reads trigger configuration from a TOML file.
 *
 * Parses an array of `[[trigger]]` tables. Each table must contain:
 * | Key    | Type    | Description                  |
 * |--------|---------|------------------------------|
 * | name   | string  | Human-readable trigger label |
 * | index  | integer | Trigger index in data stream |
 * | device | integer | Source hardware device ID    |
 * | delay  | integer | Trigger delay (DAQ units)    |
 *
 * Entries missing any required key are skipped with a warning.
 * If the file cannot be opened or parsed, an empty vector is returned.
 *
 * ### Index range and uniqueness rules
 * Config-defined triggers must use indices in **[0, 99]** — the range [100, 255]
 * is reserved for the built-in @ref trigger_number defaults.
 * - An entry with an out-of-range index is reassigned to the earliest unused
 *   slot in [0, 99] and a warning is emitted.
 * - A duplicate in-range index (two entries claiming the same slot) causes the
 *   second entry to be reassigned the same way.
 * - If no free slot remains in [0, 99], the offending entry is dropped with an error.
 *
 * @param config_file  Path to the TOML configuration file.
 * @return             List of successfully parsed trigger configurations,
 *                     with out-of-range and duplicate indices corrected.
 */
inline std::vector<trigger_config>
trigger_conf_reader(const std::string &config_file = "Data/triggers.toml")
{
    std::vector<trigger_config> triggers;

    toml::table tbl;
    try
    {
        tbl = toml::parse_file(config_file);
    }
    catch (const toml::parse_error &e)
    {
        std::cerr << "[ERROR] Failed to parse trigger config \"" << config_file
                  << "\": " << e.description() << "\n";
        return triggers;
    }

    std::cout << "[INFO] Loaded trigger config: " << config_file << "\n";

    auto arr = tbl.get_as<toml::array>("trigger");
    if (!arr)
    {
        std::cerr << "[WARN] No [[trigger]] entries found in \"" << config_file << "\"\n";
        return triggers;
    }

    // Tracks which indices in [0, 99] are already claimed.
    // Flat bool array: O(1) insert/lookup, trivially sized for 100 slots.
    bool used_indices[100] = {};

    // Returns the earliest free slot in [0, 99], or -1 if all are taken.
    auto earliest_free = [&]() -> int
    {
        for (int i = 0; i < 100; ++i)
            if (!used_indices[i])
                return i;
        return -1;
    };

    for (const auto &node : *arr)
    {
        const auto *entry = node.as_table();
        if (!entry)
            continue;

        // Validate all required keys are present
        if (!entry->contains("name") || !entry->contains("index") ||
            !entry->contains("device") || !entry->contains("delay"))
        {
            std::cerr << "[WARN] Skipping incomplete [[trigger]] entry (missing required key)\n";
            continue;
        }

        trigger_config cfg;
        cfg.name = entry->at("name").value_or(std::string{});
        uint16_t raw_index = static_cast<uint16_t>(entry->at("index").value_or(0));
        cfg.device = static_cast<uint16_t>(entry->at("device").value_or(0));
        cfg.delay = static_cast<uint16_t>(entry->at("delay").value_or(0));

        // Enforce [0, 99] index range — [100, 255] is reserved for built-in triggers.
        // Also catches duplicates: an already-used in-range index is treated the same
        // as an out-of-range one and gets reassigned to the earliest free slot.
        bool out_of_range = raw_index > 99;
        bool duplicate = !out_of_range && used_indices[raw_index];

        if (out_of_range || duplicate)
        {
            int free_slot = earliest_free();
            if (free_slot == -1)
            {
                std::cerr << "[ERROR] \"" << cfg.name << "\""
                          << (duplicate ? " has duplicate index " : " has out-of-range index ")
                          << raw_index
                          << " and no free slot remains in [0, 99] — entry dropped\n";
                continue;
            }

            std::cerr << "[WARN] \"" << cfg.name << "\""
                      << (duplicate ? " has duplicate index " : " has out-of-range index ")
                      << raw_index << " (valid range: 0-99, unique)"
                      << " — reassigned to earliest available index " << free_slot << "\n";
            raw_index = static_cast<uint16_t>(free_slot);
        }

        cfg.index = static_cast<uint8_t>(raw_index);
        used_indices[cfg.index] = true;
        triggers.push_back(cfg);

        std::cout << "[INFO]  " << cfg.name
                  << " | index=" << static_cast<int>(cfg.index)
                  << " | device=" << cfg.device
                  << " | delay=" << cfg.delay << "\n";
    }

    return triggers;
}