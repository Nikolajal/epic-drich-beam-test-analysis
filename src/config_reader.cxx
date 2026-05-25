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

        mist::logger::info(Form("(readout_config_reader) Reading readout config: %s", config_file.c_str()));

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
                mist::logger::warning(Form("(readout_config_reader) Entry '%s' has no 'devices' key — skipping.", cfg_name.c_str()));
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
                        mist::logger::warning(Form("(readout_config_reader) Unknown chips token '%s' for device %d",
                                                   chips_str->c_str(), device));
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
                                mist::logger::error(Form("(readout_config_reader) Conflict: device %d chip %d already "
                                                         "assigned to core tag '%s', cannot assign to '%s'",
                                                         device, chip, conflict_name.c_str(), cfg_name.c_str()));
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
        mist::logger::warning(Form("(readout_config_reader) Failed to parse TOML config '%s': %s",
                                   config_file.c_str(), std::string(err.description()).c_str()));
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
        mist::logger::info(Form("(FramerConfReader) Reading framer config: %s", config_file.c_str()));
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
        mist::logger::warning(Form("(FramerConfReader) TOML parse error in '%s': %s — using defaults.",
                                   config_file.c_str(), std::string(err.description()).c_str()));
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(Form("(FramerConfReader) Error reading '%s': %s — using defaults.",
                                   config_file.c_str(), err.what()));
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
        mist::logger::info(Form("(qa_conf_reader) Reading QA config: %s", config_file.c_str()));

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
        mist::logger::warning(Form("(qa_conf_reader) TOML parse error in '%s': %s — using defaults.",
                                   config_file.c_str(), std::string(err.description()).c_str()));
    }
    catch (const std::exception &err)
    {
        mist::logger::warning(Form("(qa_conf_reader) Error reading '%s': %s — using defaults.",
                                   config_file.c_str(), err.what()));
    }
    return cfg;
}

// --- RunInfo ------------------------------------------------------------

void RunInfo::read_database(std::string filename)
{
    mist::logger::info(Form("(RunInfo::read_database) Reading run database: %s", filename.c_str()));

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
        mist::logger::warning(Form("(RunInfo::read_database) Failed to parse '%s': %s",
                                   filename.c_str(), std::string(err.description()).c_str()));
    }
}

const std::optional<RunInfoStruct> RunInfo::get_run_info(const std::string &run_id)
{
    auto it = run_info_database.find(run_id);
    return (it != run_info_database.end()) ? std::optional{it->second} : std::nullopt;
}

void RunInfo::read_runslists(std::string runlist_file)
{
    mist::logger::info(Form("(RunInfo::read_runslists) Reading run list: %s", runlist_file.c_str()));

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
        mist::logger::warning(Form("(RunInfo::read_runslists) Failed to parse '%s': %s",
                                   runlist_file.c_str(), std::string(err.description()).c_str()));
    }
}

const std::optional<std::vector<std::string>> RunInfo::get_run_list(const std::string &runlist_name)
{
    auto it = run_list_database.find(runlist_name);
    return (it != run_list_database.end()) ? std::optional{it->second} : std::nullopt;
}

// --- static member definitions -------------------------------------------

std::unordered_map<std::string, RunInfoStruct> RunInfo::run_info_database = {};
std::unordered_map<std::string, std::vector<std::string>> RunInfo::run_list_database = {};