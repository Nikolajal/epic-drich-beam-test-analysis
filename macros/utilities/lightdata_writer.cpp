#include "lightdata_writer.h"
#include "config_reader.h"
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
    std::string run_list;
    int max_spill = 1000;
    bool force_lightdata_rebuild = false;
    int n_requested_threads = -1;
    std::string trigger_config_file = "conf/trigger_conf.toml";
    std::string readout_config_file = "conf/readout_config.toml";
    std::string mapping_config_file = "conf/mapping_conf.toml";
    std::string fine_calibration_config_file = data_repository + "/" + run_name + "/fine_calibration.txt";

    app.add_option("data_repository", data_repository)->required();
    app.add_option("run_name", run_name)->required();
    app.add_option("--run-list", run_list, "Name of run list (required if run_name is a .toml runlist)");
    app.add_option("--max-spill", max_spill);
    app.add_option("--threads", n_requested_threads);
    app.add_option("--trigger-conf", trigger_config_file);
    app.add_option("--readout-conf", readout_config_file);
    app.add_option("--mapping-conf", mapping_config_file);
    app.add_option("--fine-calib-conf", fine_calibration_config_file);
    app.add_flag("--force-rebuild", force_lightdata_rebuild);

    try
    {
        CLI11_PARSE(app, argc, argv);

        bool is_runlist = run_name.size() >= 5 && run_name.substr(run_name.size() - 5) == ".toml";

        if (is_runlist && run_list.empty())
            throw CLI::ValidationError("--run-list", "Option --run-list is REQUIRED when providing a runlist");

        if (!is_runlist && !run_list.empty())
            throw CLI::ValidationError("--run-list", "Option --run-list is only allowed when providing a runlist");

        if (is_runlist)
        {
            run_info::read_runslists(run_name);
            auto recovered_run_list = run_info::get_run_list(run_list);
            if (!recovered_run_list)
            {
                mist::logger::error(Form("Run list '%s' not found in database", run_list.c_str()));
                throw CLI::ValidationError("--run-list", Form("Run list '%s' not found in database", run_list.c_str()));
            }

            auto list_start = std::chrono::high_resolution_clock::now();
            for (const auto &current_run_name : *recovered_run_list)
            {
                auto start = std::chrono::high_resolution_clock::now();
                mist::logger::info(Form("(lightdata_writer) Starting writing lightdata for run '%s'", current_run_name.c_str()));
                lightdata_writer(data_repository, current_run_name, max_spill, force_lightdata_rebuild, n_requested_threads, trigger_config_file, readout_config_file, mapping_config_file, fine_calibration_config_file);
                auto end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed = end - start;
                mist::logger::info(Form("(lightdata_writer) Total time taken: %f seconds", elapsed.count()));
            }
            auto list_end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> list_elapsed = list_end - list_start;
            mist::logger::info(Form("(lightdata_writer) Total list time taken: %f seconds", list_elapsed.count()));
        }
        else
        {
            auto start = std::chrono::high_resolution_clock::now();
            lightdata_writer(data_repository, run_name, max_spill, force_lightdata_rebuild, n_requested_threads, trigger_config_file, readout_config_file, mapping_config_file, fine_calibration_config_file);
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            mist::logger::info(Form("(lightdata_writer) Total time taken: %f seconds", elapsed.count()));
        }
    }
    catch (const CLI::ParseError &e)
    {
        return app.exit(e);
    }

    return 0;
}