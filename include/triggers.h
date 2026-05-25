/**
 * @file triggers.h
 * @brief Trigger definitions and runtime registry for the ALCOR DAQ.
 *
 * Provides:
 * - @ref TriggerNumber       : enum of known hardware trigger types
 * - @ref TriggerEvent        : per-event trigger data (index, coarse time, fine time)
 * - @ref DeviceTrigger       : hardware-tagged trigger source — one per device
 * - @ref ChannelTrigger      : data channel forced into the trigger path
 * - @ref TriggerConfigSet    : both lookup maps + ordered (index,name) list, returned by the reader
 * - @ref TriggerRegistry     : runtime lookup table mapping trigger values to names
 * - @ref trigger_conf_reader : declared here; implementation lives in `src/triggers.cxx`
 *   (keeps toml++ and `<mist/logger>` out of the public header).
 *
 * ### TOML config format
 *
 * Each `[[trigger]]` entry is **either device-mode or channel-mode** — these
 * are the only two valid shapes.  See @ref DeviceTrigger and
 * @ref ChannelTrigger for details.
 *
 * @code{.toml}
 * # Device-mode: hardware already tags the word as a trigger.
 * # The config just attaches a name, index, and delay offset.
 * [[trigger]]
 * name   = "luca_and_finger"
 * index  = 0
 * device = 196
 * delay  = 117
 *
 * # Channel-mode: a normal data channel is forced into the trigger path.
 * # (column, pixel) and `eo_channel` are equivalent — pick whichever reads better.
 * [[trigger]]
 * name   = "finger_chip1"
 * index  = 2
 * device = 196
 * fifo   = 5
 * column = 3        # together with pixel pins one channel
 * pixel  = 0
 * delay  = 80
 *
 * [[trigger]]
 * name       = "finger_chip1_alt"
 * index      = 3
 * device     = 196
 * fifo       = 5
 * eo_channel = 12   # = column*4 + pixel, range [0, 31]
 * delay      = 80
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
#include <unordered_map>
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
 * @brief Device-mode trigger configuration.
 *
 * Used when the hardware already tags the data word as a trigger
 * (`AlcorData::is_trigger_tag() == true`).  In that case the device alone
 * identifies which configured trigger fired — there is at most **one
 * `DeviceTrigger` per device**.  The config exists purely to attach a
 * human-readable name, the trigger's logical index, and the delay offset
 * used for frame-time recalculation.
 *
 * If a tagged word arrives from a device without a matching `DeviceTrigger`,
 * the framer falls back to the @ref _TRIGGER_UNKNOWN_ path.
 */
struct DeviceTrigger
{
    std::string name;       ///< Human-readable trigger label (e.g. `"luca_and_finger"`).
    uint8_t  index  = 0;    ///< Trigger index in [0, 99] used in the data stream.
    uint16_t delay  = 0;    ///< Trigger delay (DAQ clock cycles).
    uint16_t device = 0;    ///< Hardware device ID that produces this trigger.
};

/**
 * @brief Channel-mode trigger configuration.
 *
 * Promotes a single data channel into the trigger path: a word arrives
 * tagged as a normal ALCOR Hit, the framer checks the
 * `(device, fifo, column, pixel)` key, and if it matches, emits a
 * @ref TriggerEvent instead of storing the word as a data hit.
 *
 * One `ChannelTrigger` per `(device, fifo, column, pixel)` tuple.
 *
 * @note  The target channel must also be present in the readout config —
 *        otherwise the framer's per-chip readout filter
 *        ([`parallel_streaming_framer.cxx`](src/parallel_streaming_framer.cxx))
 *        breaks out of the stream loop before the hit ever reaches the
 *        channel-mode check.
 */
struct ChannelTrigger
{
    std::string name;        ///< Human-readable trigger label.
    uint8_t  index  = 0;     ///< Trigger index in [0, 99] used in the data stream.
    uint16_t delay  = 0;     ///< Trigger delay (DAQ clock cycles).
    uint16_t device = 0;     ///< Hardware device ID.
    uint16_t fifo   = 0;     ///< FIFO/lane index.
    uint8_t  column = 0;     ///< Column address within the chip (0–7).
    uint8_t  pixel  = 0;     ///< Pixel address within the column (0–3).

    /// @brief Linear ALCOR channel index `column * 4 + pixel` (0–31).
    [[nodiscard]] constexpr uint8_t eo_channel() const noexcept
    {
        return static_cast<uint8_t>(column * 4 + pixel);
    }
};

/**
 * @brief Encodes `(device, fifo, column, pixel)` as a 64-bit key.
 *
 * Stable packing used both as the lookup key for @ref ChannelTrigger maps
 * and as a deduplication key for "unknown trigger" logging.  Field widths:
 * device 16 b | fifo 16 b | column 8 b | pixel 8 b (high 16 b unused).
 */
[[nodiscard]] inline constexpr uint64_t
pack_channel_key(uint16_t device, uint16_t fifo, uint8_t column, uint8_t pixel) noexcept
{
    return (uint64_t(device) << 32)
         | (uint64_t(fifo)   << 16)
         | (uint64_t(column) <<  8)
         |  uint64_t(pixel);
}

/**
 * @brief Bundle returned by @ref trigger_conf_reader.
 *
 * Holds the two O(1) lookup tables used by the framer, plus the ordered
 * `(index, name)` list that @ref TriggerRegistry needs to preserve TOML
 * declaration order in histogram bin layouts.
 */
struct TriggerConfigSet
{
    /// device → DeviceTrigger.  Duplicate device entries are a config error
    /// (rejected at load time by @ref trigger_conf_reader).
    std::unordered_map<uint16_t, DeviceTrigger>  by_device;

    /// packed `(device, fifo, column, pixel)` key → ChannelTrigger.
    /// Duplicates rejected at load time.
    std::unordered_map<uint64_t, ChannelTrigger> by_channel;

    /// `(index, name)` pairs in TOML declaration order, both modes interleaved.
    /// Consumed by @ref TriggerRegistry.
    std::vector<std::pair<uint8_t, std::string>> ordered_index_name;

    /// @return Total number of successfully loaded entries (device + channel mode).
    [[nodiscard]] std::size_t size() const noexcept
    {
        return by_device.size() + by_channel.size();
    }
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
struct TriggerRegistry
{
    /// Ordered list of `{trigger value, name}` pairs.
    std::vector<std::pair<uint8_t, std::string>> triggers;

    TriggerRegistry() = default;

    /**
     * @brief Build the registry from a parsed config set.
     *
     * Config-defined triggers are inserted first in TOML declaration order
     * (taken from @ref TriggerConfigSet::ordered_index_name), followed by
     * the built-in defaults in @ref all_default_triggers.  Duplicate values
     * are not filtered — config entries take priority by appearing first.
     *
     * @param config  Parsed configuration set from @ref trigger_conf_reader.
     */
    explicit TriggerRegistry(const TriggerConfigSet &config)
    {
        triggers = config.ordered_index_name;
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
 * Each `[[trigger]]` entry is classified as exactly one of two modes:
 *
 * **Device-mode** — required keys: `name`, `index`, `device`, `delay`.
 * No other selectors permitted.  Produces a @ref DeviceTrigger.
 *
 * **Channel-mode** — required keys: `name`, `index`, `device`, `delay`,
 * `fifo`, plus the channel position given as either:
 *   - `column` AND `pixel` (canonical), or
 *   - `eo_channel` ∈ [0, 31] (equivalent: `column = eo_channel / 4`,
 *     `pixel = eo_channel % 4`).
 *
 * Both forms together are accepted only if consistent.  Produces a @ref ChannelTrigger.
 *
 * Entries that fit neither shape are rejected with an error and skipped.
 *
 * ### Index range and uniqueness rules
 * Config-defined triggers must use indices in **[0, 99]** — the range
 * [100, 255] is reserved for the built-in @ref TriggerNumber defaults.
 * - Out-of-range or duplicate-in-range indices are reassigned to the
 *   earliest unused slot in [0, 99] and a warning is emitted.
 * - If no free slot remains, the offending entry is dropped with an error.
 *
 * ### Duplicate definitions
 * - Two device-mode entries with the same `device` → error, second dropped.
 * - Two channel-mode entries with the same
 *   `(device, fifo, column, pixel)` → error, second dropped.
 *
 * ### Legacy / dropped fields
 * `chip` and `use_hit` are no longer accepted.  Entries that set them get a
 * deprecation warning and the field is ignored.
 *
 * @param config_file  Path to the TOML configuration file.
 * @return             Parsed @ref TriggerConfigSet; empty on parse failure.
 */
TriggerConfigSet
trigger_conf_reader(const std::string &config_file = "Data/triggers.toml");