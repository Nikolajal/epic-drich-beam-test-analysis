/**
 * @file triggers.cxx
 * @brief Out-of-line implementation of @ref trigger_conf_reader.
 *
 * Keeps the toml++ parser and `<mist/logger>` dependencies behind the .cxx
 * boundary so `triggers.h` stays cheap for callers that only need the
 * @ref TriggerNumber / @ref TriggerEvent / @ref TriggerRegistry types.
 */

#include "triggers.h"

#include <toml++/toml.h>
#include "util/toml_utils.h"
#include <mist/logger/logger.h>

std::vector<TriggerConfig>
trigger_conf_reader(const std::string &config_file)
{
    std::vector<TriggerConfig> triggers;

    toml::table tbl;
    try
    {
        tbl = toml_parse_with_cutoff(config_file);
    }
    catch (const toml::parse_error &e)
    {
        mist::logger::error("(trigger_conf_reader) Failed to parse trigger config \"" +
                            config_file +
                            "\": " +
                            std::string(e.description()));
        return triggers;
    }

    mist::logger::info("(trigger_conf_reader) Loaded trigger config: " + config_file);

    auto arr = tbl.get_as<toml::array>("trigger");
    if (!arr)
    {
        mist::logger::warning("(trigger_conf_reader) No [[trigger]] entries found in \"" +
                              config_file + "\"");
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
            mist::logger::warning("(trigger_conf_reader) Skipping incomplete [[trigger]] entry (missing required key)");
            continue;
        }

        TriggerConfig cfg;
        cfg.name = entry->at("name").value_or(std::string{});
        uint16_t raw_index = static_cast<uint16_t>(entry->at("index").value_or(0));
        cfg.device = static_cast<uint16_t>(entry->at("device").value_or(0));
        cfg.delay = static_cast<uint16_t>(entry->at("delay").value_or(0));
        auto read_opt_i16 = [&](const char *key) -> int16_t
        {
            return entry->contains(key)
                       ? static_cast<int16_t>(entry->at(key).value_or(-1))
                       : int16_t(-1);
        };
        cfg.fifo   = read_opt_i16("fifo");
        cfg.chip   = read_opt_i16("chip");
        cfg.column = read_opt_i16("column");
        cfg.pixel  = read_opt_i16("pixel");
        cfg.use_hit = entry->contains("use_hit") && entry->at("use_hit").value_or(false);

        // Validate fifo/chip consistency: if both are set they must agree.
        if (cfg.fifo >= 0 && cfg.chip >= 0 && cfg.fifo / 4 != cfg.chip)
            mist::logger::warning("(trigger_conf_reader) Trigger \"" + cfg.name +
                                  "\": fifo=" + std::to_string(cfg.fifo) +
                                  " is on chip " + std::to_string(cfg.fifo / 4) +
                                  " but chip=" + std::to_string(cfg.chip) +
                                  " — these constraints are contradictory and will never match.");

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
                mist::logger::error("(trigger_conf_reader) \"" +
                                    cfg.name +
                                    (duplicate ? "\" has duplicate index " : "\" has out-of-range index ") +
                                    std::to_string(raw_index) +
                                    " and no free slot remains in [0, 99] — entry dropped");
                continue;
            }

            mist::logger::warning("(trigger_conf_reader) \"" +
                                  cfg.name +
                                  (duplicate ? "\" has duplicate index " : "\" has out-of-range index ") +
                                  std::to_string(raw_index) +
                                  " (valid range: 0-99, unique) — reassigned to earliest available index " +
                                  std::to_string(free_slot));
            raw_index = static_cast<uint16_t>(free_slot);
        }

        cfg.index = static_cast<uint8_t>(raw_index);
        used_indices[cfg.index] = true;
        triggers.push_back(cfg);

        auto sel_str = [](const char *label, int16_t val) -> std::string
        {
            return val >= 0 ? std::string(" | ") + label + "=" + std::to_string(val) : "";
        };
        mist::logger::info("(trigger_conf_reader) Loaded trigger \"" + cfg.name +
                           "\" | index=" + std::to_string(static_cast<int>(cfg.index)) +
                           " | device=" + std::to_string(cfg.device) +
                           sel_str("fifo",   cfg.fifo) +
                           sel_str("chip",   cfg.chip) +
                           sel_str("column", cfg.column) +
                           sel_str("pixel",  cfg.pixel) +
                           " | delay=" + std::to_string(cfg.delay) +
                           (cfg.use_hit ? " | use_hit=true" : ""));
    }

    return triggers;
}
