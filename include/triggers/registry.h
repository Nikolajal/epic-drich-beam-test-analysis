#pragma once

/**
 * @file triggers/registry.h
 * @brief Runtime lookup table mapping trigger values to dense (value, name)
 *        positions — used for histogram bin labelling.
 *
 * Pulls in ROOT's `TH2.h` for the @ref TriggerRegistry::label_axes helper.
 * Consumers that don't need axis labelling can skip this header entirely
 * (e.g. the framer's hot loop, which only needs the value types in
 * [`events.h`](events.h) and the config schema in [`config.h`](config.h)).
 */

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "TH2.h"

#include "triggers/config.h"
#include "triggers/events.h"

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
