/**
 * @file triggers/config.cxx
 * @brief Out-of-line implementation of @ref trigger_conf_reader.
 *
 * Keeps the toml++ parser and `<mist/logger>` dependencies behind the .cxx
 * boundary so [`triggers/config.h`](../../include/triggers/config.h) stays
 * cheap for callers that only need the schema types and reader prototype.
 *
 * ## Design notes
 *
 * Two trigger modes, no scoring, no wildcards (see
 * [`include/triggers/DISCUSSION.md`](../../include/triggers/DISCUSSION.md) § 1):
 *
 * - **Device-mode**: the hardware tag is the discriminator.  A `[[trigger]]`
 *   entry with no source-selector keys (no `fifo`, no `column`, no `pixel`)
 *   becomes a @ref DeviceTrigger indexed by `device`.
 *
 * - **Channel-mode**: a normal data channel is forced into the trigger path.
 *   A `[[trigger]]` entry with `fifo` AND a channel position
 *   (`column + pixel`, or `eo_channel`) becomes a @ref ChannelTrigger
 *   indexed by `pack_channel_key(device, fifo, column, pixel)`.
 *
 * Anything else (partial channel-mode, lingering `chip` / `use_hit` from the
 * old schema) is rejected with a clear error pointing at the entry's name.
 */

#include "triggers/config.h"

#include <toml++/toml.h>
#include "util/toml_utils.h"
#include <mist/logger/logger.h>

namespace
{
/// @brief Classification of a `[[trigger]]` TOML entry.
enum class TriggerMode
{
    Device,  ///< No source selectors → DeviceTrigger.
    Channel, ///< fifo + (column+pixel | eo_channel) → ChannelTrigger.
    Invalid  ///< Partial / mixed / unrecognised — rejected.
};

/// @brief Snapshot of which selector keys are present on a TOML entry.
struct EntryShape
{
    bool has_fifo = false;
    bool has_column = false;
    bool has_pixel = false;
    bool has_eo_channel = false;
    bool has_chip = false;    ///< Legacy, rejected.
    bool has_use_hit = false; ///< Legacy, rejected.
};

EntryShape inspect_entry(const toml::table &entry)
{
    EntryShape s;
    s.has_fifo = entry.contains("fifo");
    s.has_column = entry.contains("column");
    s.has_pixel = entry.contains("pixel");
    s.has_eo_channel = entry.contains("eo_channel");
    s.has_chip = entry.contains("chip");
    s.has_use_hit = entry.contains("use_hit");
    return s;
}

/// @brief Decide which mode an entry belongs to (or that it's malformed).
///
/// The classification is purely structural — index/device/delay sanity
/// is checked by the caller after the mode is known.
TriggerMode classify(const EntryShape &s)
{
    const bool any_channel_selector = s.has_fifo || s.has_column || s.has_pixel || s.has_eo_channel;
    if (!any_channel_selector)
        return TriggerMode::Device;

    // Channel-mode must have fifo + a channel position.
    if (!s.has_fifo)
        return TriggerMode::Invalid;

    const bool has_col_pix = s.has_column && s.has_pixel;
    if (!has_col_pix && !s.has_eo_channel)
        return TriggerMode::Invalid;

    return TriggerMode::Channel;
}
} // namespace

TriggerConfigSet
trigger_conf_reader(const std::string &config_file)
{
    TriggerConfigSet out;

    toml::table tbl;
    try
    {
        tbl = toml_parse_with_cutoff(config_file);
    }
    catch (const toml::parse_error &e)
    {
        mist::logger::error("(trigger_conf_reader) Failed to parse trigger config \"" +
                            config_file + "\": " +
                            std::string(e.description()));
        return out;
    }

    mist::logger::info("(trigger_conf_reader) Loaded trigger config: " + config_file);

    auto arr = tbl.get_as<toml::array>("trigger");
    if (!arr)
    {
        mist::logger::warning("(trigger_conf_reader) No [[trigger]] entries found in \"" +
                              config_file + "\"");
        return out;
    }

    // Track claimed slots in [0, 99] to enforce index uniqueness across both modes.
    bool used_indices[100] = {};

    auto earliest_free = [&]() -> int
    {
        for (int i = 0; i < 100; ++i)
            if (!used_indices[i])
                return i;
        return -1;
    };

    // Reassign or accept the index, returning -1 on hard failure (no free slot).
    auto resolve_index = [&](const std::string &name, uint16_t raw_index) -> int
    {
        const bool out_of_range = raw_index > 99;
        const bool duplicate = !out_of_range && used_indices[raw_index];

        if (!out_of_range && !duplicate)
        {
            used_indices[raw_index] = true;
            return static_cast<int>(raw_index);
        }

        const int free_slot = earliest_free();
        if (free_slot == -1)
        {
            mist::logger::error("(trigger_conf_reader) \"" + name + "\" has " +
                                (duplicate ? "duplicate" : "out-of-range") +
                                " index " + std::to_string(raw_index) +
                                " and no free slot remains in [0, 99] — entry dropped");
            return -1;
        }
        mist::logger::warning("(trigger_conf_reader) \"" + name + "\" has " +
                              (duplicate ? "duplicate" : "out-of-range") +
                              " index " + std::to_string(raw_index) +
                              " (valid range: 0–99, unique) — reassigned to " +
                              std::to_string(free_slot));
        used_indices[free_slot] = true;
        return free_slot;
    };

    for (const auto &node : *arr)
    {
        const auto *entry = node.as_table();
        if (!entry)
            continue;

        // Required keys common to both modes.
        if (!entry->contains("name") || !entry->contains("index") ||
            !entry->contains("device") || !entry->contains("delay"))
        {
            mist::logger::warning("(trigger_conf_reader) Skipping incomplete [[trigger]] "
                                  "entry (missing one of: name, index, device, delay)");
            continue;
        }

        const std::string name = entry->at("name").value_or(std::string{});
        const auto raw_index = static_cast<uint16_t>(entry->at("index").value_or(0));
        const auto device = static_cast<uint16_t>(entry->at("device").value_or(0));
        const auto delay = static_cast<uint16_t>(entry->at("delay").value_or(0));

        const EntryShape shape = inspect_entry(*entry);

        // Legacy fields: warn and ignore.
        if (shape.has_chip)
            mist::logger::warning("(trigger_conf_reader) \"" + name +
                                  "\": `chip` field is deprecated and ignored "
                                  "(use channel-mode with fifo + column/pixel or eo_channel).");
        if (shape.has_use_hit)
            mist::logger::warning("(trigger_conf_reader) \"" + name +
                                  "\": `use_hit` field is deprecated and ignored "
                                  "(channel-mode now subsumes that behaviour).");

        const TriggerMode mode = classify(shape);
        if (mode == TriggerMode::Invalid)
        {
            mist::logger::error("(trigger_conf_reader) \"" + name +
                                "\": malformed entry. Must be either:\n"
                                "  - device-mode: only {name, index, device, delay}, or\n"
                                "  - channel-mode: {name, index, device, delay, fifo} "
                                "PLUS {column, pixel} or eo_channel.");
            continue;
        }

        const int resolved = resolve_index(name, raw_index);
        if (resolved < 0)
            continue;
        const auto idx = static_cast<uint8_t>(resolved);

        if (mode == TriggerMode::Device)
        {
            // Reject duplicate device — same device cannot host two device-mode triggers.
            if (auto existing_it = out.by_device.find(device); existing_it != out.by_device.end())
            {
                mist::logger::error("(trigger_conf_reader) \"" + name +
                                    "\": device " + std::to_string(device) +
                                    " already has device-mode trigger \"" + existing_it->second.name +
                                    "\" — entry dropped.");
                // The slot was claimed — release it so other entries can use it.
                used_indices[idx] = false;
                continue;
            }

            DeviceTrigger dt;
            dt.name = name;
            dt.index = idx;
            dt.delay = delay;
            dt.device = device;
            out.by_device.emplace(device, std::move(dt));
            out.ordered_index_name.emplace_back(idx, name);

            mist::logger::info("(trigger_conf_reader) device-mode trigger \"" + name +
                               "\" | index=" + std::to_string(idx) +
                               " | device=" + std::to_string(device) +
                               " | delay=" + std::to_string(delay));
            continue;
        }

        // Channel-mode: extract fifo + channel position.
        const auto fifo = static_cast<uint16_t>(entry->at("fifo").value_or(0));

        uint8_t column = 0;
        uint8_t pixel = 0;

        if (shape.has_column && shape.has_pixel)
        {
            column = static_cast<uint8_t>(entry->at("column").value_or(0));
            pixel = static_cast<uint8_t>(entry->at("pixel").value_or(0));

            if (column > 7 || pixel > 3)
            {
                mist::logger::error("(trigger_conf_reader) \"" + name +
                                    "\": column=" + std::to_string(column) +
                                    ", pixel=" + std::to_string(pixel) +
                                    " out of range (column ∈ [0,7], pixel ∈ [0,3]) — entry dropped.");
                used_indices[idx] = false;
                continue;
            }

            // Cross-check against eo_channel if both forms were given.
            if (shape.has_eo_channel)
            {
                const int eo = static_cast<int>(entry->at("eo_channel").value_or(-1));
                const int expected = column * 4 + pixel;
                if (eo != expected)
                {
                    mist::logger::error("(trigger_conf_reader) \"" + name +
                                        "\": eo_channel=" + std::to_string(eo) +
                                        " inconsistent with column=" + std::to_string(column) +
                                        ", pixel=" + std::to_string(pixel) +
                                        " (expected eo_channel=" + std::to_string(expected) +
                                        ") — entry dropped.");
                    used_indices[idx] = false;
                    continue;
                }
            }
        }
        else // eo_channel-only path
        {
            const int eo = static_cast<int>(entry->at("eo_channel").value_or(-1));
            if (eo < 0 || eo > 31)
            {
                mist::logger::error("(trigger_conf_reader) \"" + name +
                                    "\": eo_channel=" + std::to_string(eo) +
                                    " out of range [0, 31] — entry dropped.");
                used_indices[idx] = false;
                continue;
            }
            column = static_cast<uint8_t>(eo / 4);
            pixel = static_cast<uint8_t>(eo % 4);
        }

        const uint64_t key = pack_channel_key(device, fifo, column, pixel);
        if (auto existing_it = out.by_channel.find(key); existing_it != out.by_channel.end())
        {
            mist::logger::error("(trigger_conf_reader) \"" + name +
                                "\": (device=" + std::to_string(device) +
                                ", fifo=" + std::to_string(fifo) +
                                ", column=" + std::to_string(column) +
                                ", pixel=" + std::to_string(pixel) +
                                ") already has channel-mode trigger \"" + existing_it->second.name +
                                "\" — entry dropped.");
            used_indices[idx] = false;
            continue;
        }

        ChannelTrigger ct;
        ct.name = name;
        ct.index = idx;
        ct.delay = delay;
        ct.device = device;
        ct.fifo = fifo;
        ct.column = column;
        ct.pixel = pixel;
        out.by_channel.emplace(key, std::move(ct));
        out.ordered_index_name.emplace_back(idx, name);

        mist::logger::info("(trigger_conf_reader) channel-mode trigger \"" + name +
                           "\" | index=" + std::to_string(idx) +
                           " | device=" + std::to_string(device) +
                           " | fifo=" + std::to_string(fifo) +
                           " | column=" + std::to_string(column) +
                           " | pixel=" + std::to_string(pixel) +
                           " | eo_channel=" + std::to_string(column * 4 + pixel) +
                           " | delay=" + std::to_string(delay));
    }

    return out;
}
