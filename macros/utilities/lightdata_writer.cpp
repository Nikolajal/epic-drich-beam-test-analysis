#include "writers/lightdata.h"
#include <mist/logger/logger.h>
#include "util/config_reader.h"
#include "util/conf_path.h"
#include "utility.h"
#include <stdio.h>
#include <chrono>
#include <iostream>
#include <CLI/CLI.hpp>

int main(int argc, char **argv)
{
    CLI::App app{"Beam test analysis"};

    std::string data_repository;
    std::string run_name;
    std::string RunList;
    int max_spill = 1000;
    bool force_rebuild = false;
    bool qa_mode = false;
    int n_requested_threads = -1;
    //  Config-file paths are resolved AFTER CLI parsing: if the user
    //  did not pass an explicit --xxx-conf, the path falls through to
    //  `util::conf_path(<basename>, qa_mode)`, which redirects to
    //  `conf/QA/<basename>` when --QA is set and that file exists.
    std::string trigger_config_file;
    std::string readout_config_file;
    std::string mapping_config_file;
    std::string fine_calibration_config_file = data_repository + "/" + run_name + "/fine_calibration.txt";
    std::string framer_config_file;
    std::string streaming_config_file;

    app.add_option("data_repository", data_repository)->required();
    app.add_option("run_name", run_name)->required();
    app.add_option("--run-list", RunList, "Name of run list (required if run_name is a .toml runlist)");
    app.add_option("--max-spill", max_spill);
    app.add_option("--threads", n_requested_threads);
    auto *p_trigger = app.add_option("--trigger-conf", trigger_config_file);
    auto *p_readout = app.add_option("--readout-conf", readout_config_file);
    auto *p_mapping = app.add_option("--Mapping-conf", mapping_config_file);
    app.add_option("--fine-calib-conf", fine_calibration_config_file);
    auto *p_framer = app.add_option("--framer-conf", framer_config_file);
    auto *p_streaming = app.add_option("--streaming-conf", streaming_config_file);
    app.add_flag("--force-rebuild", force_rebuild);
    //  Fast-feedback QA mode.  Looks for tuned overrides under conf/QA/
    //  (currently: conf/QA/streaming.toml with raised Hough thresholds,
    //  which biases N_γ upward but keeps σ_photon ~invariant; see the
    //  header of conf/QA/streaming.toml for the bias direction).
    app.add_flag("--QA", qa_mode);

    try
    {
        CLI11_PARSE(app, argc, argv);

        //  Resolve unset --xxx-conf options after parse.  Anything the
        //  user left at default falls through to conf_path(), which
        //  honours --QA by redirecting to `conf/QA/<basename>` when
        //  the override exists.
        const std::string mode = qa_mode ? std::string{"QA"} : std::string{};
        if (p_trigger->count() == 0)
            trigger_config_file = util::conf_path("trigger_conf.toml", mode);
        if (p_readout->count() == 0)
            readout_config_file = util::conf_path("readout_config.toml", mode);
        if (p_mapping->count() == 0)
            mapping_config_file = util::conf_path("mapping_conf.toml", mode);
        if (p_framer->count() == 0)
            framer_config_file = util::conf_path("framer_conf.toml", mode);
        if (p_streaming->count() == 0)
            streaming_config_file = util::conf_path("streaming.toml", mode);
        if (qa_mode)
            mist::logger::info(TString::Format(
                                   "(lightdata_writer) --QA mode: streaming-conf=%s  framer-conf=%s",
                                   streaming_config_file.c_str(), framer_config_file.c_str())
                                   .Data());

        bool is_runlist = run_name.size() >= 5 && run_name.substr(run_name.size() - 5) == ".toml";

        if (is_runlist && RunList.empty())
            throw CLI::ValidationError("--run-list", "Option --run-list is REQUIRED when providing a runlist");

        if (!is_runlist && !RunList.empty())
            throw CLI::ValidationError("--run-list", "Option --run-list is only allowed when providing a runlist");

        if (is_runlist)
        {
            RunInfo::read_runslists(run_name);
            auto recovered_run_list = RunInfo::get_run_list(RunList);
            if (!recovered_run_list)
            {
                mist::logger::error(TString::Format("Run list '%s' not found in database", RunList.c_str()).Data());
                throw CLI::ValidationError("--run-list", TString::Format("Run list '%s' not found in database", RunList.c_str()).Data());
            }

            auto list_start = std::chrono::high_resolution_clock::now();
            for (const auto &current_run_name : *recovered_run_list)
            {
                auto start = std::chrono::high_resolution_clock::now();
                mist::logger::info(TString::Format("(lightdata_writer) Starting writing lightdata for run '%s'", current_run_name.c_str()).Data());
                lightdata_writer(data_repository, current_run_name, max_spill, force_rebuild, n_requested_threads, trigger_config_file, readout_config_file, mapping_config_file, fine_calibration_config_file, framer_config_file, streaming_config_file);
                auto end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed = end - start;
                mist::logger::info(TString::Format("(lightdata_writer) Total time taken: %f seconds", elapsed.count()).Data());
            }
            auto list_end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> list_elapsed = list_end - list_start;
            mist::logger::info(TString::Format("(lightdata_writer) Total list time taken: %f seconds", list_elapsed.count()).Data());
        }
        else
        {
            auto start = std::chrono::high_resolution_clock::now();
            lightdata_writer(data_repository, run_name, max_spill, force_rebuild, n_requested_threads, trigger_config_file, readout_config_file, mapping_config_file, fine_calibration_config_file, framer_config_file, streaming_config_file);
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            mist::logger::info(TString::Format("(lightdata_writer) Total time taken: %f seconds", elapsed.count()).Data());
        }
    }
    catch (const CLI::ParseError &e)
    {
        return app.exit(e);
    }

    return 0;
}