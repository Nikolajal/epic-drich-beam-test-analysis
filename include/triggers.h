/**
 * @file triggers.h
 * @brief Trigger definitions and runtime registry for the ALCOR DAQ.
 *
 * Provides:
 * - @ref TriggerNumber   : enum of known hardware trigger types
 * - @ref TriggerEvent    : per-event trigger data (index, coarse time, fine time)
 * - @ref TriggerConfig   : per-trigger configuration loaded from a TOML file
 * - @ref TriggerRegistry : runtime lookup table mapping trigger values to names
 * - @ref trigger_conf_reader : declared here; implementation lives in `src/triggers.cxx`
 *   (keeps toml++ and `<mist/logger>` out of the public header).
 *
 * ### TOML config format
 * @code{.toml}
 * [[trigger]]
 * name   = "luca_and_finger"
 * index  = 0
 * device = 196
 * fifo   = 2        # exact lane — omit to accept any lane
 * delay  = 117
 *
 * [[trigger]]
 * name   = "finger_chip1"
 * index  = 2
 * device = 196
 * chip   = 1        # matches all four lanes on chip 1 (fifos 4-7)
 * column = 3
 * pixel  = 0
 * delay  = 80
 *
 * [[trigger]]
 * name    = "broad_scintillator"
 * index   = 1
 * device  = 197
 * delay   = 117
 * use_hit = true    # re-route trigger-tagged words as standard ALCOR hits
 * @endcode
 *
 * ### Dependencies
 * - ROOT: `TH2.h` for histogram axis labelling in @ref TriggerRegistry::label_axes.
 * - toml++ and `<mist/logger>` are pulled in only by `src/triggers.cxx`.
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <iostream>
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
enum TriggerNumber : uint8_t
{
    TriggerFirstFrames = 100,         ///< First frames of a run
    TriggerTiming = 101,               ///< Timing trigger
    TriggerTracking = 102,             ///< Tracking trigger
    TriggerRingFound = 103,           ///< Ring-finding trigger
    _TRIGGER_STREAMING_RING_FOUND_ = 104, ///< Streaming ring-finding trigger
    _TRIGGER_HOUGH_RING_FOUND_ = 105,     ///< Hough-transform ring-finding trigger
    TriggerStartOfSpill = 200,       ///< Start-of-spill signal
    _TRIGGER_UNKNOWN_ = 255               ///< Unknown / unmapped trigger
};

/// @cond INTERNAL
/// Ordered list of all default trigger enum values (mirrors @ref default_names).
constexpr TriggerNumber all_default_triggers[] = {
    TriggerFirstFrames,
    TriggerTiming,
    TriggerTracking,
    TriggerRingFound,
    _TRIGGER_STREAMING_RING_FOUND_,
    _TRIGGER_HOUGH_RING_FOUND_,
    TriggerStartOfSpill,
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
constexpr int default_trigger_index(TriggerNumber t)
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
 *
 * The @c is_secondary flag is set by the framer when this firing occurs
 * within @c BTANA_TRIGGER_SECONDARY_WINDOW clock cycles of the previous firing
 * of the same trigger index, analogous to the afterpulse check for ALCOR hits.
 */
struct TriggerEvent
{
    uint8_t  index;        ///< Hardware trigger index
    uint16_t coarse;       ///< Coarse timestamp (DAQ clock ticks)
    float    fine_time;    ///< Fine timestamp correction (ns)
    bool     is_secondary; ///< True if this firing follows another within the secondary window

    TriggerEvent() = default;

    /**
     * @brief Construct with index only; timestamps default to zero.
     * @param idx  Hardware trigger index.
     */
    TriggerEvent(uint8_t idx)
        : index(idx), coarse(0), fine_time(0.f), is_secondary(false) {}

    /**
     * @brief Construct with index and coarse timestamp.
     * @param idx  Hardware trigger index.
     * @param crs  Coarse timestamp.
     */
    TriggerEvent(uint8_t idx, uint16_t crs)
        : index(idx), coarse(crs), fine_time(0.f), is_secondary(false) {}

    /**
     * @brief Construct with all fields.
     * @param idx   Hardware trigger index.
     * @param crs   Coarse timestamp.
     * @param fine  Fine time correction (ns).
     */
    TriggerEvent(uint8_t idx, uint16_t crs, float fine)
        : index(idx), coarse(crs), fine_time(fine), is_secondary(false) {}
};

/**
 * @brief Static configuration for a single trigger channel, loaded from TOML.
 *
 * Each entry corresponds to one `[[trigger]]` block in the config file.
 * Lives in the @ref TriggerRegistry; not created per-event.
 */
struct TriggerConfig
{
    std::string name;        ///< Human-readable trigger label (e.g. `"luca_and_finger"`)
    uint8_t  index  = 0;     ///< Trigger index in [0, 99] used in the data stream
    uint16_t delay  = 0;     ///< Delay applied to this trigger channel (DAQ units)
    uint16_t device = 0;     ///< Hardware device ID that produces this trigger

    // Optional sub-device source selectors — -1 means "accept any".
    // `fifo` and `chip` are hierarchically related (chip = fifo/4): specifying
    // `fifo` selects one lane precisely; `chip` selects all four lanes on a chip.
    // Specifying both is valid and both constraints must be satisfied.
    int16_t fifo   = -1; ///< Exact FIFO/lane index (-1 = any)
    int16_t chip   = -1; ///< Chip filter: matches fifos chip*4 … chip*4+3 (-1 = any)
    int16_t column = -1; ///< Column address within the chip (-1 = any)
    int16_t pixel  = -1; ///< Pixel address within the column (-1 = any)

    bool use_hit = false; ///< If true, route trigger-tagged words through the standard ALCOR Hit path instead of storing a TriggerEvent

    TriggerConfig() = default;

    /**
     * @brief Construct with all fields.
     *
     * All selector fields default to -1 (wildcard).  Only set the fields
     * needed to identify the desired source channel.
     */
    TriggerConfig(const std::string &_name,
                   uint8_t   _index,
                   uint16_t  _delay,
                   uint16_t  _device,
                   int16_t   _fifo   = -1,
                   int16_t   _chip   = -1,
                   int16_t   _column = -1,
                   int16_t   _pixel  = -1,
                   bool      _use_hit = false)
        : name(_name), index(_index), delay(_delay), device(_device),
          fifo(_fifo), chip(_chip), column(_column), pixel(_pixel),
          use_hit(_use_hit) {}
};

/**
 * @brief Encodes a (device, fifo, column, pixel) tuple as a 64-bit key for deduplication.
 *
 * Used only to track which source tuples have already been logged as unknown triggers.
 * Field widths: device 16 b | fifo 8 b | column 8 b | pixel 8 b (remaining bits unused).
 * -1 values are stored as 0xFF in their respective byte.
 */
inline uint64_t trigger_map_key(uint16_t device, int fifo, int column = -1, int pixel = -1)
{
    auto to_byte = [](int v) -> uint64_t { return v < 0 ? 0xFF : static_cast<uint8_t>(v); };
    return (uint64_t(device) << 32)
         | (to_byte(fifo)   << 16)
         | (to_byte(column) <<  8)
         | (to_byte(pixel));
}

/**
 * @brief Finds the best-matching trigger configuration for a given Hit.
 *
 * Scans @p configs for every entry whose constraints are all satisfied by
 * the supplied Hit fields.  Among the matching entries, the one with the
 * highest *specificity score* wins:
 *
 * | Specified field | Score |
 * |-----------------|-------|
 * | fifo            |   8   |
 * | chip            |   4   |
 * | column          |   2   |
 * | pixel           |   1   |
 *
 * @c device is always required and does not contribute to the score.
 * Returns @c nullptr when no entry matches.
 *
 * @param configs  Trigger configurations to search (usually the framer's @c triggers vector).
 * @param device   Hardware device ID of the incoming Hit.
 * @param fifo     FIFO/lane index of the Hit.
 * @param column   Column address of the Hit.
 * @param pixel    Pixel address of the Hit.
 */
inline const TriggerConfig *
find_best_trigger(const std::vector<TriggerConfig> &configs,
                  int device, int fifo, int column, int pixel)
{
    const TriggerConfig *best = nullptr;
    int best_score = -1;

    for (const auto &cfg : configs)
    {
        if (cfg.device != static_cast<uint16_t>(device)) continue;
        if (cfg.fifo   >= 0 && cfg.fifo   != fifo)        continue;
        if (cfg.chip   >= 0 && cfg.chip   != fifo / 4)    continue;
        if (cfg.column >= 0 && cfg.column != column)       continue;
        if (cfg.pixel  >= 0 && cfg.pixel  != pixel)        continue;

        int score = (cfg.fifo   >= 0 ?  8 : 0)
                  + (cfg.chip   >= 0 ?  4 : 0)
                  + (cfg.column >= 0 ?  2 : 0)
                  + (cfg.pixel  >= 0 ?  1 : 0);

        if (score > best_score)
        {
            best_score = score;
            best       = &cfg;
        }
    }
    return best;
}

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
struct TriggerRegistry
{
    /// Ordered list of `{trigger value, name}` pairs.
    std::vector<std::pair<uint8_t, std::string>> triggers;

    TriggerRegistry() = default;

    /**
     * @brief Build the registry from a list of config-defined triggers.
     *
     * Config triggers are inserted first (preserving TOML declaration order),
     * followed by all entries in @ref all_default_triggers. Duplicate values
     * are not filtered — config entries take priority by appearing first.
     *
     * @param config_triggers  Triggers parsed from the TOML config file.
     */
    explicit TriggerRegistry(const std::vector<TriggerConfig> &config_triggers)
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

        std::cerr << "[WARN] TriggerRegistry: unknown trigger value "
                  << static_cast<int>(trigger_value)
                  << " — falling back to UNKNOWN\n";

        // Scan the registry for _TRIGGER_UNKNOWN_ rather than using
        // default_trigger_index(), which returns a position in all_default_triggers[]
        // and is wrong when config triggers are prepended to the registry.
        for (int i = 0; i < static_cast<int>(triggers.size()); ++i)
            if (triggers[i].first == static_cast<uint8_t>(_TRIGGER_UNKNOWN_))
                return i;

        return static_cast<int>(triggers.size()) - 1; // last-resort: final entry
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
 * | name    | string  | Human-readable trigger label                                        |
 * | index   | integer | Trigger index in data stream                                        |
 * | device  | integer | Source hardware device ID                                           |
 * | delay   | integer | Trigger delay (DAQ units)                                           |
 * | fifo    | integer | (optional) Exact FIFO/lane index; absent = any                      |
 * | chip    | integer | (optional) Chip filter (matches fifos chip×4 … chip×4+3)           |
 * | column  | integer | (optional) Column address within the chip                           |
 * | pixel   | integer | (optional) Pixel address within the column                          |
 * | use_hit | bool    | (optional) If true, route as standard ALCOR Hit, not TriggerEvent  |
 *
 * Entries missing any required key are skipped with a warning.
 * If the file cannot be opened or parsed, an empty vector is returned.
 *
 * ### Index range and uniqueness rules
 * Config-defined triggers must use indices in **[0, 99]** — the range [100, 255]
 * is reserved for the built-in @ref TriggerNumber defaults.
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
std::vector<TriggerConfig>
trigger_conf_reader(const std::string &config_file = "Data/triggers.toml");