#include "utility/config_reader.h"
#include <mist/logger/logger.h>

// --- ReadoutConfigStruct -----------------------------------------------

ReadoutConfigStruct::ReadoutConfigStruct(std::string _name, std::map<uint16_t, std::vector<uint16_t>> _device_chip)
    : name(_name), device_chip(_device_chip) {}

void ReadoutConfigStruct::add_device_chip(uint16_t device, uint16_t chip)
{
    if (device_chip.find(device) == device_chip.end())
        device_chip[device] = std::vector<uint16_t>();
    device_chip[device].push_back(chip);
}

void ReadoutConfigStruct::add_device(uint16_t device)
{
    if (device_chip.find(device) == device_chip.end())
        device_chip[device] = std::vector<uint16_t>();
    for (auto i_chip = 0; i_chip < 8; ++i_chip)
        device_chip[device].push_back(i_chip);
}

// --- ReadoutConfigList -------------------------------------------------

ReadoutConfigList::ReadoutConfigList(std::vector<ReadoutConfigStruct> vec)
    : configs(std::move(vec)) {}

ReadoutConfigStruct *ReadoutConfigList::find_by_name(const std::string &name)
{
    auto it = std::find_if(configs.begin(), configs.end(),
                           [&](const ReadoutConfigStruct &cfg)
                           { return cfg.name == name; });
    return (it != configs.end()) ? &(*it) : nullptr;
}

const ReadoutConfigStruct *ReadoutConfigList::find_by_name(const std::string &name) const
{
    auto it = std::find_if(configs.begin(), configs.end(),
                           [&](const ReadoutConfigStruct &cfg)
                           { return cfg.name == name; });
    return (it != configs.end()) ? &(*it) : nullptr;
}

ReadoutConfigStruct *ReadoutConfigList::find_by_device(uint16_t device)
{
    auto it = std::find_if(configs.begin(), configs.end(),
                           [&](const ReadoutConfigStruct &cfg)
                           { return cfg.device_chip.count(device) > 0; });
    return (it != configs.end()) ? &(*it) : nullptr;
}

std::vector<ReadoutConfigStruct *> ReadoutConfigList::find_all_by_device(uint16_t device)
{
    std::vector<ReadoutConfigStruct *> out;
    for (auto &cfg : configs)
        if (cfg.device_chip.count(device))
            out.push_back(&cfg);
    return out;
}

std::vector<std::string> ReadoutConfigList::find_by_device_and_chip(uint16_t device, uint16_t chip)
{
    std::vector<std::string> out;
    for (auto &cfg : configs)
    {
        auto it = cfg.device_chip.find(device);
        if (it != cfg.device_chip.end())
        {
            const auto &chips = it->second;
            if (std::find(chips.begin(), chips.end(), chip) != chips.end())
                out.push_back(cfg.name);
        }
    }
    return out;
}

bool ReadoutConfigList::has_name(const std::string &name) const
{
    return std::any_of(configs.begin(), configs.end(),
                       [&](const ReadoutConfigStruct &cfg)
                       { return cfg.name == name; });
}

bool ReadoutConfigList::has_cherenkov() { return find_by_name("cherenkov"); }
bool ReadoutConfigList::has_timing() { return find_by_name("timing"); }
bool ReadoutConfigList::has_tracking() { return find_by_name("tracking"); }

// --- free utility --------------------------------------------------------

std::vector<std::string> find_by_device_and_chip(
    const std::map<std::string, ReadoutConfigStruct> &readout_config_utility,
    uint16_t device, uint16_t chip)
{
    std::vector<std::string> names;
    for (const auto &[cfg_name, cfg] : readout_config_utility)
    {
        auto it = cfg.device_chip.find(device);
        if (it != cfg.device_chip.end())
        {
            const auto &chips = it->second;
            if (std::find(chips.begin(), chips.end(), chip) != chips.end())
                names.push_back(cfg_name);
        }
    }
    return names;
}

// --- readout_config_reader -----------------------------------------------

std::vector<ReadoutConfigStruct> readout_config_reader(std::string config_file)
{
    std::vector<ReadoutConfigStruct> readout_config;
    std::map<std::string, ReadoutConfigStruct> readout_config_utility;

    try
    {
        auto tbl = toml_parse_with_cutoff(config_file);

        auto readout_table = tbl["readout"].as_table();
        if (!readout_table)
        {
            mist::logger::warning("(readout_config_reader) No [readout] table found in TOML file.");
            return readout_config;
        }

        mist::logger::info(TString::Format("(readout_config_reader) Reading readout config: %s", config_file.c_str()).Data());

        for (auto &[name, entry] : *readout_table)
        {
            std::string cfg_name = std::string(name);
            auto *entry_tbl = entry.as_table();
            if (!entry_tbl)
                continue;

            if (!readout_config_utility.count(cfg_name))
                readout_config_utility[cfg_name] = ReadoutConfigStruct(cfg_name, {});

            //  Sensor type — optional string (e.g. "1350", "1375", "mixed").
            //  Single source of truth for downstream code that needs to
            //  tag results by SiPM model (e.g. AnalysisResults' sensor
            //  dimension, QA plots split by sensor).  Empty when not set
            //  in the TOML — callers should treat that as "unspecified",
            //  not as an error.
            if (auto sensor_node = entry_tbl->get("sensor_type"))
            {
                if (auto s = sensor_node->value<std::string>())
                    readout_config_utility[cfg_name].sensor_type = *s;
            }

            //  Timing-trigger coincidence params (only the "timing" role sets
            //  them; harmless no-ops on other roles).  alive_channels is a
            //  [chip0, chip1] array; the deltas are floats in ns.
            if (auto *ac_node = entry_tbl->get("alive_channels"))
                if (auto *ac_arr = ac_node->as_array())
                {
                    int idx = 0;
                    for (auto &el : *ac_arr)
                    {
                        if (auto v = el.value<int64_t>())
                        {
                            if (idx == 0)
                                readout_config_utility[cfg_name].timing_chip0_alive_channels = static_cast<int>(*v);
                            else if (idx == 1)
                                readout_config_utility[cfg_name].timing_chip1_alive_channels = static_cast<int>(*v);
                        }
                        ++idx;
                    }
                    //  The same-channel-offset calibration divides by
                    //  (alive_channels - 1), so a count < 2 divides by zero and
                    //  poisons that channel's fine-time phase with NaN.  Clamp
                    //  to 2 and warn rather than letting a misconfig through.
                    auto &rc = readout_config_utility[cfg_name];
                    for (int *p : {&rc.timing_chip0_alive_channels, &rc.timing_chip1_alive_channels})
                        if (*p < 2)
                        {
                            mist::logger::warning(TString::Format(
                                                      "(readout_config_reader) [readout.%s] alive_channels entry %d < 2 "
                                                      "would divide by zero in the same-channel calibration; clamping to 2.",
                                                      cfg_name.c_str(), *p)
                                                      .Data());
                            *p = 2;
                        }
                }
            if (auto v = entry_tbl->get("delta_center_ns"))
                if (auto d = v->value<double>())
                    readout_config_utility[cfg_name].timing_delta_center_ns = static_cast<float>(*d);
            if (auto v = entry_tbl->get("delta_window_ns"))
                if (auto d = v->value<double>())
                    readout_config_utility[cfg_name].timing_delta_window_ns = static_cast<float>(*d);
            if (auto v = entry_tbl->get("delta_n_sigma"))
                if (auto d = v->value<double>())
                    readout_config_utility[cfg_name].timing_delta_n_sigma = static_cast<float>(*d);

            auto *devices_node = entry_tbl->get("devices");
            if (!devices_node)
            {
                mist::logger::warning(TString::Format("(readout_config_reader) Entry '%s' has no 'devices' key — skipping.", cfg_name.c_str()).Data());
                continue;
            }
            auto *devices_array = devices_node->as_array();
            if (!devices_array)
                continue;

            for (auto &dev_node : *devices_array)
            {
                auto *dev_tbl = dev_node.as_table();
                if (!dev_tbl)
                    continue;

                auto *id_node = dev_tbl->get("id");
                if (!id_node)
                    continue;
                uint16_t device = static_cast<uint16_t>(id_node->value_or(0));

                //  Optional per-device sensor tag — lets one role span
                //  two SiPM models (e.g. 1350 on rdo 192-195, 1375 on
                //  196-199) without an array-of-tables.  Falls back to
                //  the role-level sensor_type when absent.
                if (auto *sensor_node = dev_tbl->get("sensor"))
                    if (auto sv = sensor_node->value<std::string>())
                        readout_config_utility[cfg_name].device_sensor[device] = *sv;

                //  Expand chips: "*" wildcard or explicit integer array
                std::vector<uint16_t> requested_chips;
                auto *chips_node = dev_tbl->get("chips");
                if (!chips_node)
                    continue;

                if (auto chips_str = chips_node->value<std::string>())
                {
                    if (*chips_str == "*")
                        for (uint16_t c = 0; c < 8; ++c)
                            requested_chips.push_back(c);
                    else
                    {
                        //  Accept a comma-separated list given as a string
                        //  (e.g. chips = "2,4") in addition to the canonical
                        //  TOML array form ([2, 4]) — both mean "these chips".
                        //  Only "*" is the wildcard.  Guards against the
                        //  silent chip-drop when a list is mistakenly quoted.
                        const std::string &s = *chips_str;
                        bool any = false;
                        for (size_t i = 0; i < s.size();)
                        {
                            while (i < s.size() &&
                                   (s[i] == ',' || s[i] == ' ' || s[i] == '\t'))
                                ++i;
                            size_t j = i;
                            while (j < s.size() && s[j] >= '0' && s[j] <= '9')
                                ++j;
                            if (j > i)
                            {
                                requested_chips.push_back(static_cast<uint16_t>(
                                    std::stoi(s.substr(i, j - i))));
                                any = true;
                                i = j;
                            }
                            else
                                break; // non-numeric, non-separator → stop
                        }
                        if (!any)
                            mist::logger::warning(TString::Format(
                                                      "(readout_config_reader) Unknown chips token '%s' for device %d",
                                                      s.c_str(), device)
                                                      .Data());
                    }
                }
                else if (auto *chips_array = chips_node->as_array())
                {
                    for (auto &c : *chips_array)
                        if (auto cv = c.value<int64_t>())
                            requested_chips.push_back(static_cast<uint16_t>(*cv));
                }

                //  Core-tag conflict check
                bool is_special = (lightdata_core_tags.count(cfg_name) > 0);
                std::vector<uint16_t> valid_chips;

                if (is_special)
                {
                    for (uint16_t chip : requested_chips)
                    {
                        bool conflict_found = false;
                        for (auto &conflict_name : find_by_device_and_chip(readout_config_utility, device, chip))
                        {
                            if (lightdata_core_tags.count(conflict_name))
                            {
                                mist::logger::error(TString::Format("(readout_config_reader) Conflict: device %d chip %d already "
                                                                    "assigned to core tag '%s', cannot assign to '%s'",
                                                                    device, chip, conflict_name.c_str(), cfg_name.c_str())
                                                        .Data());
                                conflict_found = true;
                                break;
                            }
                        }
                        if (!conflict_found)
                            valid_chips.push_back(chip);
                    }
                }
                else
                    valid_chips = requested_chips;

                for (auto chip : valid_chips)
                    readout_config_utility[cfg_name].add_device_chip(device, chip);
            }
        }
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format("(readout_config_reader) Failed to parse TOML config '%s': %s",
                                              config_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
        return readout_config;
    }

    for (auto &[name, cfg] : readout_config_utility)
        readout_config.push_back(cfg);

    return readout_config;
}

// --- FramerConfReader --------------------------------------------------

FramerConfigStruct FramerConfReader(std::string config_file)
{
    FramerConfigStruct cfg;
    try
    {
        auto tbl = toml_parse_with_cutoff(config_file);
        auto *framer_table = tbl["framer"].as_table();
        if (!framer_table)
        {
            mist::logger::warning("(FramerConfReader) No [framer] table found — using defaults.");
            return cfg;
        }
        mist::logger::info(TString::Format("(FramerConfReader) Reading framer config: %s", config_file.c_str()).Data());
        if (auto v = (*framer_table)["frame_size"].value<int64_t>())
            cfg.frame_size = static_cast<uint16_t>(*v);
        if (auto v = (*framer_table)["first_frames_trigger"].value<int64_t>())
            cfg.first_frames_trigger = static_cast<int>(*v);
        if (auto v = (*framer_table)["afterpulse_deadtime"].value<int64_t>())
            cfg.afterpulse_deadtime = static_cast<uint16_t>(*v);
        if (auto v = (*framer_table)["trigger_secondary_window"].value<int64_t>())
            cfg.trigger_secondary_window = static_cast<uint16_t>(*v);
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format("(FramerConfReader) TOML parse error in '%s': %s — using defaults.",
                                              config_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format("(FramerConfReader) Error reading '%s': %s — using defaults.",
                                              config_file.c_str(), err.what())
                                  .Data());
    }
    return cfg;
}

// --- qa_conf_reader ------------------------------------------------------

QaConfigStruct qa_conf_reader(std::string config_file)
{
    QaConfigStruct cfg;
    try
    {
        auto tbl = toml_parse_with_cutoff(config_file);
        auto *qa_table = tbl["qa"].as_table();
        if (!qa_table)
        {
            // No [qa] section — silent (defaults reproduce legacy behaviour).
            return cfg;
        }
        mist::logger::info(TString::Format("(qa_conf_reader) Reading QA config: %s", config_file.c_str()).Data());

        if (auto v = (*qa_table)["afterpulse_near_lo"].value<int64_t>())
            cfg.afterpulse_near_lo = static_cast<int>(*v);
        if (auto v = (*qa_table)["afterpulse_near_hi"].value<int64_t>())
            cfg.afterpulse_near_hi = static_cast<int>(*v);
        if (auto v = (*qa_table)["afterpulse_sideband_offset"].value<int64_t>())
            cfg.afterpulse_sideband_offset = static_cast<int>(*v);

        if (auto v = (*qa_table)["ct_scan_dt_min"].value<int64_t>())
            cfg.ct_scan_dt_min = static_cast<int>(*v);
        if (auto v = (*qa_table)["ct_scan_dt_max"].value<int64_t>())
            cfg.ct_scan_dt_max = static_cast<int>(*v);
        if (auto v = (*qa_table)["ct_phys_signal_lo"].value<int64_t>())
            cfg.ct_phys_signal_lo = static_cast<int>(*v);
        if (auto v = (*qa_table)["ct_phys_signal_hi"].value<int64_t>())
            cfg.ct_phys_signal_hi = static_cast<int>(*v);
        if (auto v = (*qa_table)["ct_elec_signal_lo"].value<int64_t>())
            cfg.ct_elec_signal_lo = static_cast<int>(*v);
        if (auto v = (*qa_table)["ct_elec_signal_hi"].value<int64_t>())
            cfg.ct_elec_signal_hi = static_cast<int>(*v);
        if (auto v = (*qa_table)["ct_phys_radius_mm"].value<double>())
            cfg.ct_phys_radius_mm = static_cast<float>(*v);
        if (auto v = (*qa_table)["ct_sideband_offset"].value<int64_t>())
            cfg.ct_sideband_offset = static_cast<int>(*v);

        // QA-mode behavior toggles.
        if (auto v = (*qa_table)["per_spill_calibration_update"].value<bool>())
        {
            cfg.per_spill_calibration_update = *v;
            if (cfg.per_spill_calibration_update)
                mist::logger::info("(qa_conf_reader) per_spill_calibration_update=true — "
                                   "lightdata_writer will seed each spill's calibration table "
                                   "from the framer's fine-tune distribution.");
        }
        //  Accept both integer types: TOML's int is int64_t, but operators
        //  may legitimately write `0` (unset) or larger periods.  Negative
        //  values fall back to 0 (never retry) with a warning so a typo
        //  doesn't silently produce undefined behaviour from an unsigned
        //  wraparound.
        if (auto v = (*qa_table)["generate_calibration_low_stats_retry_period"].value<int64_t>())
        {
            if (*v < 0)
            {
                mist::logger::warning(
                    "(qa_conf_reader) generate_calibration_low_stats_retry_period < 0 — "
                    "treating as 0 (never retry).");
                cfg.generate_calibration_low_stats_retry_period = 0;
            }
            else
            {
                cfg.generate_calibration_low_stats_retry_period =
                    static_cast<unsigned long>(*v);
            }
        }

        // Sanity warnings — windows must be non-empty and well-ordered.
        if (cfg.afterpulse_near_hi < cfg.afterpulse_near_lo)
            mist::logger::warning("(qa_conf_reader) afterpulse_near_hi < afterpulse_near_lo "
                                  "— window will never match a Hit.");
        const int near_width = cfg.afterpulse_near_hi - cfg.afterpulse_near_lo + 1;
        if (cfg.afterpulse_sideband_offset < cfg.afterpulse_near_hi)
            mist::logger::warning("(qa_conf_reader) afterpulse_sideband_offset overlaps the near "
                                  "window — sideband subtraction will under-estimate afterpulse.");
        if (near_width <= 0)
            mist::logger::warning("(qa_conf_reader) afterpulse near window has non-positive width.");
        if (cfg.ct_scan_dt_max <= cfg.ct_scan_dt_min)
            mist::logger::warning("(qa_conf_reader) ct_scan_dt_max <= ct_scan_dt_min — CT scan will be empty.");
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format("(qa_conf_reader) TOML parse error in '%s': %s — using defaults.",
                                              config_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format("(qa_conf_reader) Error reading '%s': %s — using defaults.",
                                              config_file.c_str(), err.what())
                                  .Data());
    }
    return cfg;
}

// --- CalibConfigStruct --------------------------------------------------

CalibConfigStruct calib_conf_reader(std::string config_file)
{
    CalibConfigStruct cfg;
    try
    {
        auto tbl = toml_parse_with_cutoff(config_file);
        auto *t = tbl["calibration"].as_table();
        if (!t)
        {
            mist::logger::warning(TString::Format(
                                      "(calib_conf_reader) No [calibration] table in '%s' — using defaults "
                                      "(anchor = device=%d chip=%d eo_channel=%d).",
                                      config_file.c_str(),
                                      cfg.anchor_device, cfg.anchor_chip, cfg.anchor_eo_channel)
                                      .Data());
            return cfg;
        }
        mist::logger::info(TString::Format("(calib_conf_reader) Reading calibration config: %s",
                                           config_file.c_str())
                               .Data());

        if (auto v = (*t)["anchor_device"].value<int64_t>())
            cfg.anchor_device = static_cast<int>(*v);
        if (auto v = (*t)["anchor_chip"].value<int64_t>())
            cfg.anchor_chip = static_cast<int>(*v);
        if (auto v = (*t)["anchor_eo_channel"].value<int64_t>())
            cfg.anchor_eo_channel = static_cast<int>(*v);
        if (auto v = (*t)["anchor_fifo"].value<int64_t>())
            cfg.anchor_fifo = static_cast<int>(*v);
        if (auto v = (*t)["anchor_delay_cc"].value<double>())
            cfg.anchor_delay_cc = *v;
        if (auto v = (*t)["min_hits_per_tdc"].value<int64_t>())
            cfg.min_hits_per_tdc = static_cast<int>(*v);
        if (auto v = (*t)["min_hits_per_tdc_per_spill"].value<int64_t>())
            cfg.min_hits_per_tdc_per_spill = static_cast<int>(*v);
        if (auto v = (*t)["fine_min_valid"].value<int64_t>())
            cfg.fine_min_valid = static_cast<int>(*v);
        if (auto v = (*t)["fine_max_valid"].value<int64_t>())
            cfg.fine_max_valid = static_cast<int>(*v);
        if (auto v = (*t)["slope_fit_min_fine_span"].value<int64_t>())
            cfg.slope_fit_min_fine_span = static_cast<int>(*v);
        if (auto v = (*t)["default_slope_cc_per_bin"].value<double>())
            cfg.default_slope_cc_per_bin = *v;
        if (auto v = (*t)["slope_min"].value<double>())
            cfg.slope_min = *v;
        if (auto v = (*t)["slope_max"].value<double>())
            cfg.slope_max = *v;
        if (auto v = (*t)["pulser_period_cc"].value<double>())
            cfg.pulser_period_cc = *v;
        if (auto v = (*t)["b_min"].value<double>())
            cfg.b_min = *v;
        if (auto v = (*t)["b_max"].value<double>())
            cfg.b_max = *v;
        if (auto v = (*t)["consecutive_pair_tolerance_cc"].value<double>())
            cfg.consecutive_pair_tolerance_cc = *v;
        if (auto v = (*t)["slip_confidence_cc"].value<double>())
            cfg.slip_confidence_cc = *v;
        if (auto v = (*t)["slip_max_snap_fraction"].value<double>())
            cfg.slip_max_snap_fraction = *v;
        //  regime1_confidence_cc / regime1_slip_unit_cc keys are
        //  IGNORED if present in the TOML — the regime-1 pass was
        //  removed (see CalibConfigStruct).  Silently ignore for
        //  backward compatibility with old config files.
        //  3-tier calibration-file resolution.
        if (auto v = (*t)["override_path"].value<std::string>())
            cfg.override_path = *v;
        if (auto v = (*t)["default_path"].value<std::string>())
            cfg.default_path = *v;
        if (auto v = (*t)["force_rebuild"].value<bool>())
            cfg.force_rebuild = *v;

        //  Sanity checks — the pulser pipeline tolerates a wide range
        //  but flag obvious mistakes early.
        if (cfg.anchor_device < 0 || cfg.anchor_chip < 0 || cfg.anchor_eo_channel < 0 ||
            cfg.anchor_chip >= 8 || cfg.anchor_eo_channel >= 32)
            mist::logger::warning(TString::Format(
                                      "(calib_conf_reader) anchor address (dev=%d chip=%d eo_ch=%d) is out of "
                                      "the expected ranges (chip 0..7, eo_channel 0..31). Check the TOML.",
                                      cfg.anchor_device, cfg.anchor_chip, cfg.anchor_eo_channel)
                                      .Data());
        if (cfg.min_hits_per_tdc < cfg.min_hits_per_tdc_per_spill)
            mist::logger::warning("(calib_conf_reader) min_hits_per_tdc < min_hits_per_tdc_per_spill — "
                                  "cumulative threshold below per-spill floor will short-circuit the gate.");

        //  Mirror the --QA banner pattern from lightdata/recodata/
        //  recotrackdata_writer: log every knob the writer will use,
        //  so the operator sees on stdout what's actually loaded.
        mist::logger::info(TString::Format(
                               "(calib_conf_reader) anchor   : device:%d chip:%d eo_ch:%d  fifo:%d%s  delay:%.0f cc%s",
                               cfg.anchor_device, cfg.anchor_chip, cfg.anchor_eo_channel,
                               cfg.anchor_fifo,
                               cfg.anchor_fifo >= 0 ? " (FIFO-salvage anchor)" : " (channel anchor)",
                               cfg.anchor_delay_cc,
                               cfg.anchor_delay_cc != 0.0 ? " (pinned)" : " (auto-picker)")
                               .Data());
        mist::logger::info(TString::Format(
                               "(calib_conf_reader) thresholds: cumulative>=%d   per-spill>=%d",
                               cfg.min_hits_per_tdc, cfg.min_hits_per_tdc_per_spill)
                               .Data());
        mist::logger::info(TString::Format(
                               "(calib_conf_reader) fine band: [%d, %d] inclusive   (hits outside are discarded at ingest)",
                               cfg.fine_min_valid, cfg.fine_max_valid)
                               .Data());
        mist::logger::info(TString::Format(
                               "(calib_conf_reader) slope    : guard=fine_span>=%d   "
                               "default=%.6f cc/bin   physical=[%.4f, %.4f]",
                               cfg.slope_fit_min_fine_span, cfg.default_slope_cc_per_bin,
                               cfg.slope_min, cfg.slope_max)
                               .Data());
        if (cfg.pulser_period_cc > 0.0)
            mist::logger::info(TString::Format(
                                   "(calib_conf_reader) pulser   : period FIXED at %.3f cc = %.3f ns "
                                   "(per-channel period fit disabled)",
                                   cfg.pulser_period_cc, cfg.pulser_period_cc * 3.125)
                                   .Data());
        else
            mist::logger::info("(calib_conf_reader) pulser   : period FIT per channel "
                               "(set pulser_period_cc > 0 in TOML to fix it)");
        mist::logger::info(TString::Format(
                               "(calib_conf_reader) intercept: physical band b in [%.2f, %.2f] cc "
                               "(out-of-band values clamped to nearest edge)",
                               cfg.b_min, cfg.b_max)
                               .Data());
        mist::logger::info(TString::Format(
                               "(calib_conf_reader) pair tol : |c_h - c_p - T| < %.1f cc (consecutive-pair safety filter)",
                               cfg.consecutive_pair_tolerance_cc)
                               .Data());
        if (cfg.slip_max_snap_fraction > 0.0)
            mist::logger::info(TString::Format(
                                   "(calib_conf_reader) slip     : regime-2 confidence=%.3f cc   max_snap_fraction=%.2f   (regime-1 absorbed by fitted b)",
                                   cfg.slip_confidence_cc, cfg.slip_max_snap_fraction)
                                   .Data());
        else
            mist::logger::info("(calib_conf_reader) slip     : correction DISABLED (slip_max_snap_fraction <= 0)");
        mist::logger::info(TString::Format(
                               "(calib_conf_reader) io       : override='%s'   default='%s'   force_rebuild=%s",
                               cfg.override_path.c_str(), cfg.default_path.c_str(),
                               cfg.force_rebuild ? "true" : "false")
                               .Data());
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format(
                                  "(calib_conf_reader) TOML parse error in '%s': %s — using defaults.",
                                  config_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format(
                                  "(calib_conf_reader) Error reading '%s': %s — using defaults.",
                                  config_file.c_str(), err.what())
                                  .Data());
    }
    return cfg;
}

// --- Calibration-file resolution ----------------------------------------
//
// 3-tier policy (see CalibConfigStruct doc): override > default > rebuild.
// Centralised here so both pulser_calib_writer (producer) and the
// downstream consumers (recodata_writer etc.) share one source of truth.

CalibPathResult resolve_fine_calib_path(const CalibConfigStruct &cfg,
                                        const std::string &run_dir)
{
    namespace fs = std::filesystem;
    CalibPathResult r;

    //  ForceRebuild short-circuits everything (producer-only meaning;
    //  consumers don't set this flag).
    if (cfg.force_rebuild)
    {
        r.kind = CalibPathResolution::ForceRebuildRequested;
        r.path = run_dir + "/" + cfg.default_path;
        return r;
    }
    //  Tier 1: explicit override.
    if (!cfg.override_path.empty() && fs::exists(cfg.override_path))
    {
        r.kind = CalibPathResolution::Override;
        r.path = cfg.override_path;
        return r;
    }
    //  Tier 2: default file in the run dir.
    //
    //  Only the configured ``default_path`` (TOML v3 schema:
    //  fine_calib.toml) counts as a valid existing calibration.  The
    //  earlier ``.txt ↔ .toml`` sibling-extension fallback is gone
    //  along with the legacy text reader itself — see task #172.
    //  Operators with stranded ``fine_calib.txt`` files must
    //  regenerate (the resolver returns MissingNeedsRebuild here so
    //  pulser_calib_writer will produce a fresh .toml on the next
    //  launch).
    const std::string default_full = run_dir + "/" + cfg.default_path;
    if (fs::exists(default_full))
    {
        r.kind = CalibPathResolution::Default;
        r.path = default_full;
        return r;
    }
    //  Tier 3: nothing — rebuild needed.
    r.kind = CalibPathResolution::MissingNeedsRebuild;
    r.path = default_full;
    return r;
}

// --- RunInfo ------------------------------------------------------------

void RunInfo::read_database(std::string filename)
{
    mist::logger::info(TString::Format("(RunInfo::read_database) Reading run database: %s", filename.c_str()).Data());

    try
    {
        auto tbl = toml_parse_with_cutoff(filename);

        auto runs_table = tbl["runs"].as_table();
        if (!runs_table)
        {
            mist::logger::warning("(RunInfo::read_database) No [runs] table found in TOML file.");
            return;
        }

        std::string previous_run_id;
        for (auto &[run_id, run_entry] : *runs_table)
        {
            auto *run_tbl = run_entry.as_table();
            if (!run_tbl)
                continue;

            auto &cur = run_info_database[std::string(run_id)];

            //  Helper: inherit field from previous run if absent
            auto prev = [&]() -> RunInfoStruct *
            {
                return previous_run_id.empty() ? nullptr : &run_info_database[previous_run_id];
            };

            //  Beam
            cur.beam_polarity = run_tbl->get("beam_polarity") ? run_tbl->get("beam_polarity")->value_or(std::string{}) : (prev() ? prev()->beam_polarity : "");
            cur.beam_energy = run_tbl->get("beam_energy") ? run_tbl->get("beam_energy")->value_or(0) : (prev() ? prev()->beam_energy : 0);

            //  DAQ
            cur.rdo_firmware = run_tbl->get("rdo_firmware") ? run_tbl->get("rdo_firmware")->value_or(std::string{}) : (prev() ? prev()->rdo_firmware : "");
            cur.timing_firmware = run_tbl->get("timing_firmware") ? run_tbl->get("timing_firmware")->value_or(std::string{}) : (prev() ? prev()->timing_firmware : "");
            cur.n_spills = run_tbl->get("n_spills") ? run_tbl->get("n_spills")->value_or(0) : (prev() ? prev()->n_spills : 0);
            cur.timing_on_axis = run_tbl->get("timing_on_axis") ? run_tbl->get("timing_on_axis")->value_or(true) : (prev() ? prev()->timing_on_axis : true);
            cur.op_mode = run_tbl->get("op_mode") ? run_tbl->get("op_mode")->value_or(1) : (prev() ? prev()->op_mode : 1);
            cur.delta_thr = run_tbl->get("delta_thr") ? run_tbl->get("delta_thr")->value_or(10) : (prev() ? prev()->delta_thr : 10);

            //  Sensors
            cur.temperature = run_tbl->get("temperature") ? run_tbl->get("temperature")->value_or(0.0) : (prev() ? prev()->temperature : 0.0);
            cur.v_bias = run_tbl->get("v_bias") ? run_tbl->get("v_bias")->value_or(0.0) : (prev() ? prev()->v_bias : 0.0);

            //  Optics
            cur.aerogel_mirror = run_tbl->get("aerogel_mirror") ? run_tbl->get("aerogel_mirror")->value_or(0) : (prev() ? prev()->aerogel_mirror : 0);
            cur.gas_mirror = run_tbl->get("gas_mirror") ? run_tbl->get("gas_mirror")->value_or(0) : (prev() ? prev()->gas_mirror : 0);

            //  Analyser-tuned trigger thresholds (per-run override).
            //  Inherits from the preceding run when absent so a single
            //  edit at the top of a calibration sweep carries forward
            //  to subsequent runs without re-typing.  0 (the default)
            //  means "use the streaming-conf default" — same semantic
            //  as the field being absent.
            cur.streaming_n_sigma_threshold = run_tbl->get("streaming_n_sigma_threshold")
                                                  ? static_cast<float>(run_tbl->get("streaming_n_sigma_threshold")->value_or(0.0))
                                                  : (prev() ? prev()->streaming_n_sigma_threshold : 0.f);

            //  Radiators
            if (auto rad_node = run_tbl->get("radiators"))
            {
                if (auto rad_array = rad_node->as_array())
                {
                    for (auto &r : *rad_array)
                    {
                        if (auto r_tbl = r.as_table())
                        {
                            auto &rad = cur.radiators.emplace_back();
                            if (auto *n = r_tbl->get("type"))
                                rad.type = n->value_or("");
                            if (auto *n = r_tbl->get("tag"))
                                rad.tag = n->value_or("");
                            if (auto *n = r_tbl->get("refindex"))
                                rad.refindex = n->value_or(0.0);
                            if (auto *n = r_tbl->get("depth"))
                                rad.depth = n->value_or(0.0);
                            if (auto *n = r_tbl->get("side"))
                                rad.side = n->value_or("");
                        }
                    }
                }
            }
            else if (prev())
                cur.radiators = prev()->radiators;

            previous_run_id = std::string(run_id);
        }
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format("(RunInfo::read_database) Failed to parse '%s': %s",
                                              filename.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
}

const std::optional<RunInfoStruct> RunInfo::get_run_info(const std::string &run_id)
{
    auto it = run_info_database.find(run_id);
    return (it != run_info_database.end()) ? std::optional{it->second} : std::nullopt;
}

void RunInfo::read_runslists(std::string runlist_file)
{
    mist::logger::info(TString::Format("(RunInfo::read_runslists) Reading run list: %s", runlist_file.c_str()).Data());

    try
    {
        auto tbl = toml_parse_with_cutoff(runlist_file);

        auto runlists_table = tbl["runlists"].as_table();
        if (!runlists_table)
        {
            mist::logger::warning("(RunInfo::read_runslists) No [runlists] table found in TOML file.");
            return;
        }

        for (auto &[runlist_name, runlist_entry] : *runlists_table)
        {
            auto *runlist_tbl = runlist_entry.as_table();
            if (!runlist_tbl)
                continue;

            auto &current_runlist = run_list_database[std::string(runlist_name)];
            if (auto *runs_node = runlist_tbl->get("runs"))
                if (auto *runs_array = runs_node->as_array())
                    for (auto &r : *runs_array)
                        if (auto run_str = r.value<std::string>())
                            current_runlist.push_back(*run_str);
        }
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format("(RunInfo::read_runslists) Failed to parse '%s': %s",
                                              runlist_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
}

const std::optional<std::vector<std::string>> RunInfo::get_run_list(const std::string &runlist_name)
{
    auto it = run_list_database.find(runlist_name);
    return (it != run_list_database.end()) ? std::optional{it->second} : std::nullopt;
}

// --- streaming_trigger_conf_reader --------------------------------------

StreamingTriggerConfigStruct
streaming_trigger_conf_reader(std::string config_file)
{
    StreamingTriggerConfigStruct cfg;
    try
    {
        auto tbl = toml_parse_with_cutoff(config_file);
        auto *st_table = tbl["streaming_trigger"].as_table();
        if (!st_table)
        {
            // No [streaming_trigger] section — silent (defaults are sensible).
            return cfg;
        }
        mist::logger::info(TString::Format(
                               "(streaming_trigger_conf_reader) Reading streaming-trigger config: %s",
                               config_file.c_str())
                               .Data());

        if (auto v = (*st_table)["time_window_ns"].value<double>())
            cfg.time_window_ns = static_cast<float>(*v);
        if (auto v = (*st_table)["n_sigma_threshold"].value<double>())
            cfg.n_sigma_threshold = static_cast<float>(*v);
        if (auto v = (*st_table)["min_noise_hits"].value<double>())
            cfg.min_noise_hits = *v;
        //  C7.6 — multiplicity upper-bound cut.  0 = disabled.
        if (auto v = (*st_table)["max_hits_per_window"].value<int64_t>())
            cfg.max_hits_per_window = static_cast<int>(*v);
        //  In-beam-background QA sample knobs.
        if (auto v = (*st_table)["inbeam_pretrigger_offset_ns"].value<double>())
            cfg.inbeam_pretrigger_offset_ns = static_cast<float>(*v);
        if (auto v = (*st_table)["inbeam_sample_width_ns"].value<double>())
            cfg.inbeam_sample_width_ns = static_cast<float>(*v);
        if (auto v = (*st_table)["default_trigger_window_ns"].value<double>())
            cfg.default_trigger_window_ns = static_cast<float>(*v);

        if (cfg.time_window_ns <= 0.f)
            mist::logger::warning("(streaming_trigger_conf_reader) time_window_ns must be > 0 — "
                                  "reverting to default 5 ns.");
        if (cfg.min_noise_hits < 1.0)
            mist::logger::warning("(streaming_trigger_conf_reader) min_noise_hits < 1 admits "
                                  "channels with zero or one observed hits — rate estimate "
                                  "is unreliable and the noise-score tail will inflate.");
        if (cfg.max_hits_per_window < 0)
        {
            mist::logger::warning("(streaming_trigger_conf_reader) max_hits_per_window < 0 "
                                  "is meaningless — clamping to 0 (cut disabled).");
            cfg.max_hits_per_window = 0;
        }
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format(
                                  "(streaming_trigger_conf_reader) TOML parse error in '%s': %s — using defaults.",
                                  config_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format(
                                  "(streaming_trigger_conf_reader) Error reading '%s': %s — using defaults.",
                                  config_file.c_str(), err.what())
                                  .Data());
    }
    return cfg;
}

// --- streaming_ransac_conf_reader ----------------------------------------

StreamingRansacConfigStruct
streaming_ransac_conf_reader(std::string config_file)
{
    StreamingRansacConfigStruct cfg;
    try
    {
        auto tbl = toml_parse_with_cutoff(config_file);
        auto *sh_table = tbl["streaming_ransac"].as_table();
        if (!sh_table)
        {
            // No [streaming_ransac] section — silent (defaults match the
            // hardcoded values still in lightdata_writer.cxx).
            return cfg;
        }
        mist::logger::info(TString::Format(
                               "(streaming_ransac_conf_reader) Reading streaming-RANSAC config: %s",
                               config_file.c_str())
                               .Data());

        // RANSAC accumulator geometry
        if (auto v = (*sh_table)["r_min"].value<double>())
            cfg.r_min = static_cast<float>(*v);
        if (auto v = (*sh_table)["r_max"].value<double>())
            cfg.r_max = static_cast<float>(*v);
        if (auto v = (*sh_table)["r_step"].value<double>())
            cfg.r_step = static_cast<float>(*v);
        if (auto v = (*sh_table)["cell_size"].value<double>())
            cfg.cell_size = static_cast<float>(*v);

        // Per-frame ring-finder parameters.
        // `time_cut_ns` is inherited from streaming_trigger.time_window_ns
        // and `max_rings` is hardcoded to 2 — see the struct doc and
        // include/triggers/streaming/DISCUSSION.md § 2 for the rationale.
        // If the user mistakenly sets either key in TOML, warn loudly
        // (the value is silently ignored).
        if ((*sh_table)["time_cut_ns"])
            mist::logger::warning(
                "(streaming_ransac_conf_reader) `time_cut_ns` is no longer a "
                "knob — the RANSAC time pre-cut is inherited from "
                "`streaming_trigger.time_window_ns`.  Ignoring.");
        if ((*sh_table)["max_rings"])
            mist::logger::warning(
                "(streaming_ransac_conf_reader) `max_rings` is no longer a "
                "knob — hardcoded to 2 (one ring per radiator).  Ignoring.");

        if (auto v = (*sh_table)["threshold_fraction"].value<double>())
            cfg.threshold_fraction = static_cast<float>(*v);
        if (auto v = (*sh_table)["min_hits_slack"].value<double>())
            cfg.min_hits_slack = static_cast<float>(*v);
        if (auto v = (*sh_table)["hough_threshold_fraction"].value<double>())
            cfg.hough_threshold_fraction = static_cast<float>(*v);
        if (auto v = (*sh_table)["hough_n_sigma_dcr"].value<double>())
            cfg.hough_n_sigma_dcr = static_cast<float>(*v);
        if (auto v = (*sh_table)["min_ring_votes_floor"].value<int64_t>())
            cfg.min_ring_votes_floor = static_cast<int>(*v);
        if (auto v = (*sh_table)["collection_radius"].value<double>())
            cfg.collection_radius = static_cast<float>(*v);
        if (auto v = (*sh_table)["ransac_iterations"].value<int64_t>())
            cfg.ransac_iterations = static_cast<int>(*v);
        if (auto v = (*sh_table)["ransac_min_significance"].value<double>())
            cfg.ransac_min_significance = static_cast<float>(*v);
        if (auto v = (*sh_table)["ransac_min_inliers"].value<int64_t>())
            cfg.ransac_min_inliers = static_cast<int>(*v);
        if (auto v = (*sh_table)["centre_xy_half_range_mm"].value<double>())
            cfg.centre_xy_half_range_mm = static_cast<float>(*v);
        if (auto v = (*sh_table)["aggregation_window_cells"].value<int64_t>())
            cfg.aggregation_window_cells = static_cast<int>(*v);
        if (auto v = (*sh_table)["centre_padding_mm"].value<double>())
            cfg.centre_padding_mm = static_cast<float>(*v);

        //  Wide-arc mode: auto-couple the radius range to the centre range
        //  (far-off-centre / arc reconstruction).  Capture whether the QA
        //  centre axis was pinned in TOML *before* we may widen it below.
        const bool centre_xy_half_range_set =
            static_cast<bool>((*sh_table)["centre_xy_half_range_mm"]);
        if (auto v = (*sh_table)["r_max_auto"].value<bool>())
            cfg.r_max_auto = *v;
        if (auto v = (*sh_table)["sensor_half_extent_mm"].value<double>())
            cfg.sensor_half_extent_mm = static_cast<float>(*v);
        if (auto v = (*sh_table)["r_margin_mm"].value<double>())
            cfg.r_margin_mm = static_cast<float>(*v);

        if (cfg.r_max_auto)
        {
            if (cfg.centre_padding_mm < 0.f)
            {
                mist::logger::warning(
                    "(streaming_ransac_conf_reader) r_max_auto requires "
                    "centre_padding_mm >= 0 (the -1 sentinel defines the pad AS "
                    "r_max, mutually exclusive with deriving r_max from the "
                    "pad).  Leaving auto OFF; using the explicit r_max.");
                cfg.r_max_auto = false;
            }
            else
            {
                cfg.r_max = cfg.centre_padding_mm + cfg.sensor_half_extent_mm +
                            cfg.r_margin_mm;
            }
        }

        //  Single-knob promise: when the per-ring centre QA axis isn't
        //  explicitly pinned, widen it to bracket the centre search range
        //  (centre_padding_mm) so far-off centres don't pile into overflow.
        //  Applies whenever a finite pad is given (not just r_max_auto), so
        //  the explicit-r_max coarse-scan config gets a matching QA axis too.
        //  Bin width snaps to cell_size downstream, so n_bins scales
        //  automatically.
        if (!centre_xy_half_range_set && cfg.centre_padding_mm >= 0.f)
            cfg.centre_xy_half_range_mm = cfg.centre_padding_mm;

        //  C3.5 — `fit_circle_init_{x,y,r}` are deprecated.  recodata_writer
        //  seeds the per-ring refit from the RANSAC peak (cx, cy, radius);
        //  no consumer ever read the config-supplied values.  Tolerated
        //  for one release so existing TOMLs continue to load — log one
        //  warning per key encountered.  Remove the warning (and this
        //  block) in v2.1.
        for (const char *key : {"fit_circle_init_x",
                                "fit_circle_init_y",
                                "fit_circle_init_r"})
        {
            if ((*sh_table)[key])
            {
                mist::logger::warning(TString::Format(
                                          "(streaming_ransac_conf_reader) `%s` is deprecated and "
                                          "IGNORED (recodata seeds the fit from the RANSAC peak). "
                                          "Remove the key from your config to silence this warning.",
                                          key)
                                          .Data());
            }
        }

        // Sanity warnings
        if (cfg.r_min < 0.f || cfg.r_max <= cfg.r_min)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) invalid radius range — "
                "r_min must be ≥ 0 and r_max must exceed r_min.");
        if (cfg.r_step <= 0.f)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) r_step must be > 0.");
        if (cfg.cell_size <= 0.f)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) cell_size must be > 0.");
        if (cfg.threshold_fraction <= 0.f || cfg.threshold_fraction > 1.f)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) threshold_fraction should be in (0, 1].");
        if (cfg.min_hits_slack <= 0.f || cfg.min_hits_slack > 1.f)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) min_hits_slack should be in (0, 1].");
        if (cfg.hough_threshold_fraction <= 0.f)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) hough_threshold_fraction must be > 0.");
        if (cfg.collection_radius <= 0.f)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) collection_radius must be > 0.");
        if (cfg.centre_xy_half_range_mm <= 0.f)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) centre_xy_half_range_mm must be > 0.");
        if (cfg.aggregation_window_cells < 1)
            mist::logger::warning(
                "(streaming_ransac_conf_reader) aggregation_window_cells must be ≥ 1.");

        //  Echo the loaded values back — saves a class of "did my TOML
        //  edit actually take effect?" confusion at the start of a run.
        //  One line per logical group, fixed format so it's grep-able.
        mist::logger::info(TString::Format(
                               "(streaming_ransac_conf_reader) geom: r_min=%.2f r_max=%.2f r_step=%.2f cell_size=%.2f",
                               cfg.r_min, cfg.r_max, cfg.r_step, cfg.cell_size)
                               .Data());
        if (cfg.r_max_auto)
            mist::logger::info(TString::Format(
                                   "(streaming_ransac_conf_reader) r_max AUTO-COUPLED (wide-arc): "
                                   "r_max=%.1f = pad %.1f + sensor %.1f + margin %.1f",
                                   cfg.r_max, cfg.centre_padding_mm,
                                   cfg.sensor_half_extent_mm, cfg.r_margin_mm)
                                   .Data());
        mist::logger::info(TString::Format(
                               "(streaming_ransac_conf_reader) thresholds: threshold_fraction=%.3f min_hits_slack=%.3f "
                               "hough_threshold_fraction=%.4f collection_radius=%.2f",
                               cfg.threshold_fraction, cfg.min_hits_slack,
                               cfg.hough_threshold_fraction, cfg.collection_radius)
                               .Data());
        mist::logger::info(TString::Format(
                               "(streaming_ransac_conf_reader) peak finder: aggregation_window_cells=%d %s",
                               cfg.aggregation_window_cells,
                               cfg.aggregation_window_cells > 1
                                   ? "(SLIDING-WINDOW AGGREGATION ACTIVE)"
                                   : "(legacy single-cell)")
                               .Data());
        mist::logger::info(TString::Format(
                               "(streaming_ransac_conf_reader) lut padding: centre_padding_mm=%.2f %s",
                               cfg.centre_padding_mm,
                               cfg.centre_padding_mm < 0.f
                                   ? "(default = r_max, full coverage)"
                                   : "(tight pad — accumulator shrunk)")
                               .Data());
        //  C3.5 — fit_circle_init_{x,y,r} echo dropped along with the
        //  fields themselves; centre_xy_half_range_mm now stands alone.
        mist::logger::info(TString::Format(
                               "(streaming_ransac_conf_reader) centre_xy_half_range_mm=%.2f",
                               cfg.centre_xy_half_range_mm)
                               .Data());
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format(
                                  "(streaming_ransac_conf_reader) TOML parse error in '%s': %s — using defaults.",
                                  config_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format(
                                  "(streaming_ransac_conf_reader) Error reading '%s': %s — using defaults.",
                                  config_file.c_str(), err.what())
                                  .Data());
    }
    return cfg;
}

// --- recodata_conf_reader -----------------------------------------------
//
// Populates RecodataConfigStruct from TWO files (the former standalone
// recodata.toml was dismembered):
//   * `[streaming_ransac]` in `streaming_file`  → the 5 ring-reco knobs,
//     so they sit next to the RANSAC geometry they must agree with.
//   * `[coverage]`        in `mapping_file`    → the 8 coverage-map
//     geometry keys, same domain as the rest of mapping_conf.toml.
// All fields are optional; missing keys (or a missing table) keep the
// defaults from RecodataConfigStruct.  Each file is parsed in its own
// try/catch so a parse failure in one doesn't lose the other's values —
// same non-fatal pattern as streaming_ransac_conf_reader.

RecodataConfigStruct
recodata_conf_reader(std::string streaming_file, std::string mapping_file)
{
    RecodataConfigStruct cfg;

    //  Effective RANSAC r_max (honouring the wide-arc auto-coupling) — peeked
    //  from the [streaming_ransac] table below and used in part (b) to warn if
    //  the radial histogram range would clip large arcs.  -1 = unknown.  Only
    //  meaningful as a clip bound when r_max is AUTO-derived (then it ≈ the
    //  reachable R); with an explicit huge coarse-scan r_max the radial range
    //  is decoupled, so the warning is suppressed.
    float hough_r_max_effective = -1.f;
    bool hough_r_max_auto_derived = false;

    //  (a) Ring-reconstruction knobs from streaming.toml's
    //      `[streaming_ransac]` table.
    try
    {
        //  C2.3: route through ``toml_parse_with_cutoff`` so a ``##``
        //  sentinel below the live config is honoured (matches every
        //  other reader — see ``streaming_ransac_conf_reader`` and
        //  ``Mapping::load_calib``).
        toml::table tbl = toml_parse_with_cutoff(streaming_file);
        mist::logger::info(TString::Format(
                               "(recodata_conf_reader) Reading ring-reco knobs from "
                               "[streaming_ransac] in: %s",
                               streaming_file.c_str())
                               .Data());

        if (auto *h_table = tbl["streaming_ransac"].as_table())
        {
            if (auto v = (*h_table)["delta_r_for_coverage_mm"].value<double>())
                cfg.delta_r_for_coverage_mm = static_cast<float>(*v);
            if (auto v = (*h_table)["min_hits_per_ring"].value<int64_t>())
                cfg.min_hits_per_ring = static_cast<int>(*v);
            if (auto v = (*h_table)["arc_span_min_rad"].value<double>())
                cfg.arc_span_min_rad = static_cast<float>(*v);

            //  Mirror the wide-arc r_max derivation from
            //  streaming_ransac_conf_reader (single source of truth there;
            //  this is a read-only peek for the radial-clip warning below).
            float h_r_max = 120.f;
            if (auto v = (*h_table)["r_max"].value<double>())
                h_r_max = static_cast<float>(*v);
            bool h_r_max_auto = false;
            if (auto v = (*h_table)["r_max_auto"].value<bool>())
                h_r_max_auto = *v;
            float pad = -1.f;
            if (auto v = (*h_table)["centre_padding_mm"].value<double>())
                pad = static_cast<float>(*v);
            //  Mirror streaming_ransac_conf_reader's mutual-exclusion guard:
            //  auto is only honoured when centre_padding_mm >= 0.
            const bool wide_arc = h_r_max_auto && pad >= 0.f;
            if (wide_arc)
            {
                float sensor = 99.f, margin = 10.f;
                if (auto v = (*h_table)["sensor_half_extent_mm"].value<double>())
                    sensor = static_cast<float>(*v);
                if (auto v = (*h_table)["r_margin_mm"].value<double>())
                    margin = static_cast<float>(*v);
                h_r_max = pad + sensor + margin;
            }
            hough_r_max_effective = h_r_max;
            hough_r_max_auto_derived = wide_arc;

            //  Per-ring eff / arc-mode reconstruction (Kasa seed + arc-span
            //  guard + per-ring coverage).  Defaults to the wide-arc
            //  derivation (so existing auto-coupled configs are unchanged),
            //  but can be set EXPLICITLY — needed by the wide coarse-scan
            //  config, which uses an explicit r_max (r_max_auto = false) yet
            //  still wants arc-mode reconstruction on.
            cfg.radial_eff_per_ring_centre = wide_arc;
            if (auto v = (*h_table)["radial_eff_per_ring_centre"].value<bool>())
                cfg.radial_eff_per_ring_centre = *v;

            if (auto v = (*h_table)["hardware_ring_dt_min_ns"].value<double>())
                cfg.hardware_ring_dt_min_ns = static_cast<float>(*v);
            if (auto v = (*h_table)["hardware_ring_dt_max_ns"].value<double>())
                cfg.hardware_ring_dt_max_ns = static_cast<float>(*v);
            if (auto v = (*h_table)["skip_loo_residuals"].value<bool>())
                cfg.skip_loo_residuals = *v;
        }
        else
        {
            mist::logger::warning(
                "(recodata_conf_reader) No `[streaming_ransac]` table found — "
                "using ring-reco defaults.");
        }
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format(
                                  "(recodata_conf_reader) TOML parse error in '%s': %s — using defaults.",
                                  streaming_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format(
                                  "(recodata_conf_reader) Error reading '%s': %s — using defaults.",
                                  streaming_file.c_str(), err.what())
                                  .Data());
    }

    //  (b) Coverage-map geometry from mapping_conf.toml's `[coverage]`
    //      table.
    try
    {
        toml::table tbl = toml_parse_with_cutoff(mapping_file);
        mist::logger::info(TString::Format(
                               "(recodata_conf_reader) Reading coverage geometry from "
                               "[coverage] in: %s",
                               mapping_file.c_str())
                               .Data());

        if (auto *r_table = tbl["coverage"].as_table())
        {
            if (auto v = (*r_table)["n_phi_bins_coverage"].value<int64_t>())
                cfg.n_phi_bins_coverage = static_cast<int>(*v);
            if (auto v = (*r_table)["n_r_bins_coverage"].value<int64_t>())
                cfg.n_r_bins_coverage = static_cast<int>(*v);
            if (auto v = (*r_table)["r_min_coverage_mm"].value<double>())
                cfg.r_min_coverage_mm = static_cast<float>(*v);
            if (auto v = (*r_table)["r_max_coverage_mm"].value<double>())
                cfg.r_max_coverage_mm = static_cast<float>(*v);
            if (auto v = (*r_table)["channel_half_width_mm"].value<double>())
                cfg.channel_half_width_mm = static_cast<float>(*v);
            if (auto v = (*r_table)["nominal_centre_x_mm"].value<double>())
                cfg.nominal_centre_x_mm = static_cast<float>(*v);
            if (auto v = (*r_table)["nominal_centre_y_mm"].value<double>())
                cfg.nominal_centre_y_mm = static_cast<float>(*v);
            if (auto v = (*r_table)["min_channel_r_for_coverage_mm"].value<double>())
                cfg.min_channel_r_for_coverage_mm = static_cast<float>(*v);
        }
        else
        {
            mist::logger::warning(
                "(recodata_conf_reader) No `[coverage]` table found — "
                "using coverage-geometry defaults.");
        }

        // Sanity warnings — same style as streaming_ransac_conf_reader.
        if (cfg.n_phi_bins_coverage <= 0 || cfg.n_r_bins_coverage <= 0)
            mist::logger::warning(
                "(recodata_conf_reader) coverage bin counts must be > 0.");
        if (cfg.r_max_coverage_mm <= cfg.r_min_coverage_mm)
            mist::logger::warning(
                "(recodata_conf_reader) r_max_coverage_mm must exceed r_min_coverage_mm.");
        if (hough_r_max_auto_derived && hough_r_max_effective > 0.f &&
            cfg.r_max_coverage_mm < hough_r_max_effective)
            mist::logger::warning(TString::Format(
                                      "(recodata_conf_reader) r_max_coverage_mm=%.1f < RANSAC r_max=%.1f "
                                      "— the radial histogram will CLIP large arcs.  Widen "
                                      "[coverage].r_max_coverage_mm to >= the RANSAC r_max.",
                                      cfg.r_max_coverage_mm, hough_r_max_effective)
                                      .Data());
        if (cfg.channel_half_width_mm <= 0.f)
            mist::logger::warning(
                "(recodata_conf_reader) channel_half_width_mm must be > 0.");
        if (cfg.delta_r_for_coverage_mm <= 0.f)
            mist::logger::warning(
                "(recodata_conf_reader) delta_r_for_coverage_mm must be > 0.");
        if (cfg.min_hits_per_ring < 1)
            mist::logger::warning(
                "(recodata_conf_reader) min_hits_per_ring must be ≥ 1.");

        // Echo loaded values — same diagnostic pattern as
        // streaming_ransac_conf_reader.  Grep-friendly fixed format.
        mist::logger::info(TString::Format(
                               "(recodata_conf_reader) coverage map: nphi=%d nR=%d  R=[%.2f, %.2f] mm  "
                               "channel_half_width=%.2f mm",
                               cfg.n_phi_bins_coverage, cfg.n_r_bins_coverage,
                               cfg.r_min_coverage_mm, cfg.r_max_coverage_mm,
                               cfg.channel_half_width_mm)
                               .Data());
        mist::logger::info(TString::Format(
                               "(recodata_conf_reader) nominal centre: (%.2f, %.2f) mm  "
                               "delta_r_for_coverage=%.2f mm  min_hits_per_ring=%d  "
                               "min_channel_r_for_coverage=%.2f mm  arc_span_min=%.2f rad",
                               cfg.nominal_centre_x_mm, cfg.nominal_centre_y_mm,
                               cfg.delta_r_for_coverage_mm, cfg.min_hits_per_ring,
                               cfg.min_channel_r_for_coverage_mm, cfg.arc_span_min_rad)
                               .Data());
        if (cfg.skip_loo_residuals)
            mist::logger::info(
                "(recodata_conf_reader) skip_loo_residuals=true — per-hit "
                "LOO residual loop disabled; h_residual_vs_n_* will be empty "
                "and no σ_photon will be measured (QA fast path).");
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format(
                                  "(recodata_conf_reader) TOML parse error in '%s': %s — using defaults.",
                                  mapping_file.c_str(), std::string(err.description()).c_str())
                                  .Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format(
                                  "(recodata_conf_reader) Error reading '%s': %s — using defaults.",
                                  mapping_file.c_str(), err.what())
                                  .Data());
    }
    return cfg;
}

// --- static member definitions -------------------------------------------

std::unordered_map<std::string, RunInfoStruct> RunInfo::run_info_database = {};
std::unordered_map<std::string, std::vector<std::string>> RunInfo::run_list_database = {};