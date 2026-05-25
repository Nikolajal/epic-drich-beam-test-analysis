#include "writers/recodata.h"
#include "util/config_reader.h"
#include "utility.h"
#include <stdio.h>
#include <CLI/CLI.hpp>

int main(int argc, char **argv)
{
    CLI::App app{"Recodata writer"};

    std::string data_repository;
    std::string run_name;
    std::string mapping_conf = "conf/mapping_conf.2025.toml";
    std::string trigger_config_file = "conf/trigger_conf.toml";
    std::string RunList;

    int max_spill = 1000;
    bool force_recodata_rebuild = false;
    bool force_lightdata_rebuild = false;

    app.add_option("data_repository", data_repository)->required();
    app.add_option("run_name", run_name)->required();
    app.add_option("--run-list", RunList, "Name of run list (required if run_name is a .toml runlist)");
    app.add_option("--max-spill", max_spill);
    app.add_option("--Mapping-conf", mapping_conf);
    app.add_option("--trigger-conf", trigger_config_file);
    app.add_flag("--force-recodata", force_recodata_rebuild);
    app.add_flag("--force-lightdata", force_lightdata_rebuild);

    try
    {
        CLI11_PARSE(app, argc, argv);

        bool is_runlist = false;

        if (run_name.size() >= 5 && run_name.substr(run_name.size() - 5) == ".toml")
            is_runlist = true;

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
                mist::logger::info(TString::Format("(recodata_writer) Starting writing recodata for run '%s'", current_run_name.c_str()).Data());
                recodata_writer(data_repository, current_run_name, max_spill, force_recodata_rebuild, force_lightdata_rebuild, mapping_conf, trigger_config_file);
                auto end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed = end - start;
                mist::logger::info(TString::Format("(recodata_writer) Total time taken: %f seconds", elapsed.count()).Data());
            }
            auto list_end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> list_elapsed = list_end - list_start;
            mist::logger::info(TString::Format("(recodata_writer) Total time taken: %f seconds", list_elapsed.count()).Data());
        }
        else
        {
            auto start = std::chrono::high_resolution_clock::now();
            recodata_writer(data_repository, run_name, max_spill, force_recodata_rebuild, force_lightdata_rebuild, mapping_conf, trigger_config_file);
            auto end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end - start;
            mist::logger::info(TString::Format("(recodata_writer) Total time taken: %f seconds", elapsed.count()).Data());
        }
    }
    catch (const CLI::ParseError &e)
    {
        return app.exit(e);
    }

    return 0;
}