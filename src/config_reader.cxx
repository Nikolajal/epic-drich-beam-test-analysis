#include "util/config_reader.h"

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
                        mist::logger::warning(TString::Format("(readout_config_reader) Unknown chips token '%s' for device %d",
                                                   chips_str->c_str(), device).Data());
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
                                                         device, chip, conflict_name.c_str(), cfg_name.c_str()).Data());
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
                                   config_file.c_str(), std::string(err.description()).c_str()).Data());
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
                                   config_file.c_str(), std::string(err.description()).c_str()).Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format("(FramerConfReader) Error reading '%s': %s — using defaults.",
                                   config_file.c_str(), err.what()).Data());
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
                                   config_file.c_str(), std::string(err.description()).c_str()).Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format("(qa_conf_reader) Error reading '%s': %s — using defaults.",
                                   config_file.c_str(), err.what()).Data());
    }
    return cfg;
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
                                   filename.c_str(), std::string(err.description()).c_str()).Data());
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
                                   runlist_file.c_str(), std::string(err.description()).c_str()).Data());
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
            config_file.c_str()).Data());

        if (auto v = (*st_table)["time_window_ns"].value<double>())
            cfg.time_window_ns = static_cast<float>(*v);
        if (auto v = (*st_table)["n_sigma_threshold"].value<double>())
            cfg.n_sigma_threshold = static_cast<float>(*v);
        if (auto v = (*st_table)["min_noise_hits"].value<double>())
            cfg.min_noise_hits = *v;

        if (cfg.time_window_ns <= 0.f)
            mist::logger::warning("(streaming_trigger_conf_reader) time_window_ns must be > 0 — "
                                  "reverting to default 5 ns.");
        if (cfg.min_noise_hits < 1.0)
            mist::logger::warning("(streaming_trigger_conf_reader) min_noise_hits < 1 admits "
                                  "channels with zero or one observed hits — rate estimate "
                                  "is unreliable and the noise-score tail will inflate.");
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format(
            "(streaming_trigger_conf_reader) TOML parse error in '%s': %s — using defaults.",
            config_file.c_str(), std::string(err.description()).c_str()).Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format(
            "(streaming_trigger_conf_reader) Error reading '%s': %s — using defaults.",
            config_file.c_str(), err.what()).Data());
    }
    return cfg;
}

// --- streaming_hough_conf_reader ----------------------------------------

StreamingHoughConfigStruct
streaming_hough_conf_reader(std::string config_file)
{
    StreamingHoughConfigStruct cfg;
    try
    {
        auto tbl = toml_parse_with_cutoff(config_file);
        auto *sh_table = tbl["streaming_hough"].as_table();
        if (!sh_table)
        {
            // No [streaming_hough] section — silent (defaults match the
            // hardcoded values still in lightdata_writer.cxx).
            return cfg;
        }
        mist::logger::info(TString::Format(
            "(streaming_hough_conf_reader) Reading streaming-Hough config: %s",
            config_file.c_str()).Data());

        // Hough accumulator geometry
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
                "(streaming_hough_conf_reader) `time_cut_ns` is no longer a "
                "knob — the Hough time pre-cut is inherited from "
                "`streaming_trigger.time_window_ns`.  Ignoring.");
        if ((*sh_table)["max_rings"])
            mist::logger::warning(
                "(streaming_hough_conf_reader) `max_rings` is no longer a "
                "knob — hardcoded to 2 (one ring per radiator).  Ignoring.");

        if (auto v = (*sh_table)["threshold_fraction"].value<double>())
            cfg.threshold_fraction = static_cast<float>(*v);
        if (auto v = (*sh_table)["min_hits_slack"].value<double>())
            cfg.min_hits_slack = static_cast<float>(*v);
        if (auto v = (*sh_table)["hough_threshold_fraction"].value<double>())
            cfg.hough_threshold_fraction = static_cast<float>(*v);
        if (auto v = (*sh_table)["collection_radius"].value<double>())
            cfg.collection_radius = static_cast<float>(*v);
        if (auto v = (*sh_table)["centre_xy_half_range_mm"].value<double>())
            cfg.centre_xy_half_range_mm = static_cast<float>(*v);
        if (auto v = (*sh_table)["aggregation_window_cells"].value<int64_t>())
            cfg.aggregation_window_cells = static_cast<int>(*v);
        if (auto v = (*sh_table)["centre_padding_mm"].value<double>())
            cfg.centre_padding_mm = static_cast<float>(*v);

        // fit_circle initial guess
        if (auto v = (*sh_table)["fit_circle_init_x"].value<double>())
            cfg.fit_circle_init_x = static_cast<float>(*v);
        if (auto v = (*sh_table)["fit_circle_init_y"].value<double>())
            cfg.fit_circle_init_y = static_cast<float>(*v);
        if (auto v = (*sh_table)["fit_circle_init_r"].value<double>())
            cfg.fit_circle_init_r = static_cast<float>(*v);

        // Sanity warnings
        if (cfg.r_min < 0.f || cfg.r_max <= cfg.r_min)
            mist::logger::warning(
                "(streaming_hough_conf_reader) invalid radius range — "
                "r_min must be ≥ 0 and r_max must exceed r_min.");
        if (cfg.r_step <= 0.f)
            mist::logger::warning(
                "(streaming_hough_conf_reader) r_step must be > 0.");
        if (cfg.cell_size <= 0.f)
            mist::logger::warning(
                "(streaming_hough_conf_reader) cell_size must be > 0.");
        if (cfg.threshold_fraction <= 0.f || cfg.threshold_fraction > 1.f)
            mist::logger::warning(
                "(streaming_hough_conf_reader) threshold_fraction should be in (0, 1].");
        if (cfg.min_hits_slack <= 0.f || cfg.min_hits_slack > 1.f)
            mist::logger::warning(
                "(streaming_hough_conf_reader) min_hits_slack should be in (0, 1].");
        if (cfg.hough_threshold_fraction <= 0.f)
            mist::logger::warning(
                "(streaming_hough_conf_reader) hough_threshold_fraction must be > 0.");
        if (cfg.collection_radius <= 0.f)
            mist::logger::warning(
                "(streaming_hough_conf_reader) collection_radius must be > 0.");
        if (cfg.centre_xy_half_range_mm <= 0.f)
            mist::logger::warning(
                "(streaming_hough_conf_reader) centre_xy_half_range_mm must be > 0.");
        if (cfg.aggregation_window_cells < 1)
            mist::logger::warning(
                "(streaming_hough_conf_reader) aggregation_window_cells must be ≥ 1.");

        //  Echo the loaded values back — saves a class of "did my TOML
        //  edit actually take effect?" confusion at the start of a run.
        //  One line per logical group, fixed format so it's grep-able.
        mist::logger::info(TString::Format(
            "(streaming_hough_conf_reader) geom: r_min=%.2f r_max=%.2f r_step=%.2f cell_size=%.2f",
            cfg.r_min, cfg.r_max, cfg.r_step, cfg.cell_size).Data());
        mist::logger::info(TString::Format(
            "(streaming_hough_conf_reader) thresholds: threshold_fraction=%.3f min_hits_slack=%.3f "
            "hough_threshold_fraction=%.4f collection_radius=%.2f",
            cfg.threshold_fraction, cfg.min_hits_slack,
            cfg.hough_threshold_fraction, cfg.collection_radius).Data());
        mist::logger::info(TString::Format(
            "(streaming_hough_conf_reader) peak finder: aggregation_window_cells=%d %s",
            cfg.aggregation_window_cells,
            cfg.aggregation_window_cells > 1
                ? "(SLIDING-WINDOW AGGREGATION ACTIVE)"
                : "(legacy single-cell)").Data());
        mist::logger::info(TString::Format(
            "(streaming_hough_conf_reader) lut padding: centre_padding_mm=%.2f %s",
            cfg.centre_padding_mm,
            cfg.centre_padding_mm < 0.f
                ? "(default = r_max, full coverage)"
                : "(tight pad — accumulator shrunk)").Data());
        mist::logger::info(TString::Format(
            "(streaming_hough_conf_reader) fit_circle init: x=%.2f y=%.2f r=%.2f; centre_xy_half_range=%.2f",
            cfg.fit_circle_init_x, cfg.fit_circle_init_y, cfg.fit_circle_init_r,
            cfg.centre_xy_half_range_mm).Data());
    }
    catch (const toml::parse_error &err)
    {
        mist::logger::warning(TString::Format(
            "(streaming_hough_conf_reader) TOML parse error in '%s': %s — using defaults.",
            config_file.c_str(), std::string(err.description()).c_str()).Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format(
            "(streaming_hough_conf_reader) Error reading '%s': %s — using defaults.",
            config_file.c_str(), err.what()).Data());
    }
    return cfg;
}

// --- recodata_conf_reader -----------------------------------------------
//
// Parses the [recodata] table.  All fields are optional; missing keys
// keep the defaults from RecodataConfigStruct (which match the offline
// macro's conventions for direct comparison).  Same parse-error pattern
// as streaming_hough_conf_reader — TOML parse failure is non-fatal
// (a missing file just means default config), but bad parses are
// logged.

RecodataConfigStruct
recodata_conf_reader(std::string config_file)
{
    RecodataConfigStruct cfg;

    try
    {
        toml::table tbl = toml::parse_file(config_file);
        mist::logger::info(TString::Format(
            "(recodata_conf_reader) Reading recodata config: %s",
            config_file.c_str()).Data());

        if (auto *r_table = tbl["recodata"].as_table())
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
            if (auto v = (*r_table)["delta_r_for_coverage_mm"].value<double>())
                cfg.delta_r_for_coverage_mm = static_cast<float>(*v);
            if (auto v = (*r_table)["min_hits_per_ring"].value<int64_t>())
                cfg.min_hits_per_ring = static_cast<int>(*v);
            if (auto v = (*r_table)["min_channel_r_for_coverage_mm"].value<double>())
                cfg.min_channel_r_for_coverage_mm = static_cast<float>(*v);
            if (auto v = (*r_table)["skip_loo_residuals"].value<bool>())
                cfg.skip_loo_residuals = *v;
        }
        else
        {
            mist::logger::warning(
                "(recodata_conf_reader) No `[recodata]` table found — using defaults.");
        }

        // Sanity warnings — same style as streaming_hough_conf_reader.
        if (cfg.n_phi_bins_coverage <= 0 || cfg.n_r_bins_coverage <= 0)
            mist::logger::warning(
                "(recodata_conf_reader) coverage bin counts must be > 0.");
        if (cfg.r_max_coverage_mm <= cfg.r_min_coverage_mm)
            mist::logger::warning(
                "(recodata_conf_reader) r_max_coverage_mm must exceed r_min_coverage_mm.");
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
        // streaming_hough_conf_reader.  Grep-friendly fixed format.
        mist::logger::info(TString::Format(
            "(recodata_conf_reader) coverage map: nphi=%d nR=%d  R=[%.2f, %.2f] mm  "
            "channel_half_width=%.2f mm",
            cfg.n_phi_bins_coverage, cfg.n_r_bins_coverage,
            cfg.r_min_coverage_mm, cfg.r_max_coverage_mm,
            cfg.channel_half_width_mm).Data());
        mist::logger::info(TString::Format(
            "(recodata_conf_reader) nominal centre: (%.2f, %.2f) mm  "
            "delta_r_for_coverage=%.2f mm  min_hits_per_ring=%d  "
            "min_channel_r_for_coverage=%.2f mm",
            cfg.nominal_centre_x_mm, cfg.nominal_centre_y_mm,
            cfg.delta_r_for_coverage_mm, cfg.min_hits_per_ring,
            cfg.min_channel_r_for_coverage_mm).Data());
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
            config_file.c_str(), std::string(err.description()).c_str()).Data());
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(TString::Format(
            "(recodata_conf_reader) Error reading '%s': %s — using defaults.",
            config_file.c_str(), err.what()).Data());
    }
    return cfg;
}

// --- static member definitions -------------------------------------------

std::unordered_map<std::string, RunInfoStruct> RunInfo::run_info_database = {};
std::unordered_map<std::string, std::vector<std::string>> RunInfo::run_list_database = {};