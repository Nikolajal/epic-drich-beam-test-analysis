#pragma once

/**
 * @file triggers/events.h
 * @brief Runtime trigger types — what the data stream carries.
 *
 * The smallest leaf header of the trigger subsystem: enums and per-event
 * value types, no ROOT and no toml++.  Suitable for inclusion in performance-
 * sensitive translation units (the framer hot loop, the streaming trigger)
 * that only need the value types and don't want to pull in the config-schema
 * or registry headers.
 *
 * Pairs with:
 * - [`triggers/config.h`](config.h) — config schema + reader (loaded once at startup).
 * - [`triggers/registry.h`](registry.h) — runtime bin-label lookup.
 * - [`triggers.h`](../triggers.h) — umbrella header re-exporting all three.
 */

#include <cstdint>

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
    TriggerFirstFrames = 100,             ///< First frames of a run
    TriggerTiming = 101,                  ///< Timing trigger
    TriggerTracking = 102,                ///< Tracking trigger
    TriggerRingFound = 103,               ///< Ring-finding trigger
    _TRIGGER_STREAMING_RING_FOUND_ = 104, ///< Streaming ring-finding trigger
    _TRIGGER_HOUGH_RING_FOUND_ = 105,     ///< Hough-transform ring-finding trigger
    TriggerStartOfSpill = 200,            ///< Start-of-spill signal
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
//  Per-event runtime struct
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
    uint8_t index;     ///< Hardware trigger index
    uint16_t coarse;   ///< Coarse timestamp (DAQ clock ticks)
    float fine_time;   ///< Fine timestamp correction (ns)
    bool is_secondary; ///< True if this firing follows another within the secondary window

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
