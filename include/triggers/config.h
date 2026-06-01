#pragma once

/**
 * @file triggers/config.h
 * @brief Trigger config schema + TOML reader prototype.
 *
 * The two-mode trigger configuration model (device-mode vs channel-mode),
 * the bundled result type returned by the loader, and the key-packing
 * helper used by the framer's channel-mode lookup map.
 *
 * The implementation of @ref trigger_conf_reader lives in
 * `src/triggers/config.cxx` (keeps toml++ and `<mist/logger>` out of the
 * public header).
 *
 * See [`triggers/DISCUSSION.md`](DISCUSSION.md) § 1 for the design
 * rationale of the two-mode split, and the in-tree TOML examples in
 * `conf/trigger_conf.*.toml` for live configuration shapes.
 *
 * Pairs with:
 * - [`triggers/events.h`](events.h) — runtime value types.
 * - [`triggers/registry.h`](registry.h) — consumes @ref TriggerConfigSet.
 * - [`triggers.h`](../triggers.h) — umbrella.
 */

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
 * the framer falls back to the `_TRIGGER_UNKNOWN_` path.
 */
struct DeviceTrigger
{
    std::string name;    ///< Human-readable trigger label (e.g. `"luca_and_finger"`).
    uint8_t index = 0;   ///< Trigger index in [0, 99] used in the data stream.
    uint16_t delay = 0;  ///< Trigger delay (DAQ clock cycles).
    uint16_t device = 0; ///< Hardware device ID that produces this trigger.
};

/**
 * @brief Channel-mode trigger configuration.
 *
 * Promotes a single data channel into the trigger path: a word arrives
 * tagged as a normal ALCOR Hit, the framer checks the
 * `(device, fifo, column, pixel)` key, and if it matches, emits a
 * `TriggerEvent` instead of storing the word as a data hit.
 *
 * One `ChannelTrigger` per `(device, fifo, column, pixel)` tuple.
 *
 * @note  The target channel must also be present in the readout config —
 *        otherwise the framer's per-chip readout filter
 *        ([`parallel_streaming_framer.cxx`](../../src/parallel_streaming_framer.cxx))
 *        breaks out of the stream loop before the hit ever reaches the
 *        channel-mode check.
 */
struct ChannelTrigger
{
    std::string name;    ///< Human-readable trigger label.
    uint8_t index = 0;   ///< Trigger index in [0, 99] used in the data stream.
    uint16_t delay = 0;  ///< Trigger delay (DAQ clock cycles).
    uint16_t device = 0; ///< Hardware device ID.
    uint16_t fifo = 0;   ///< FIFO/lane index.
    uint8_t column = 0;  ///< Column address within the chip (0–7).
    uint8_t pixel = 0;   ///< Pixel address within the column (0–3).

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
    return (uint64_t(device) << 32) | (uint64_t(fifo) << 16) | (uint64_t(column) << 8) | uint64_t(pixel);
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
    std::unordered_map<uint16_t, DeviceTrigger> by_device;

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
 * [100, 255] is reserved for the built-in `TriggerNumber` defaults.
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
