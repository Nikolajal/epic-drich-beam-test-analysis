#include "lightdata_writer.h"
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
    int max_spill = 1000;
    bool force_lightdata_rebuild = false;
    int n_requested_threads = -1;
    std::string trigger_config_file = "conf/trigger_conf.toml";
    std::string readout_config_file = "conf/readout_config.txt";
    std::string mapping_config_file = "conf/mapping_conf.toml";

    app.add_option("data_repository", data_repository)->required();
    app.add_option("run_name", run_name)->required();
    app.add_option("--max-spill", max_spill);
    app.add_option("--threads", n_requested_threads);
    app.add_option("--trigger-conf", trigger_config_file);
    app.add_option("--readout-conf", readout_config_file);
    app.add_option("--mapping-conf", mapping_config_file);
    app.add_flag("--force-rebuild", force_lightdata_rebuild);

    CLI11_PARSE(app, argc, argv);

    auto start = std::chrono::high_resolution_clock::now();
    lightdata_writer(data_repository, run_name, max_spill, force_lightdata_rebuild, n_requested_threads, trigger_config_file, readout_config_file, mapping_config_file);
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    mist::logger::info(Form("Total time taken: %f seconds", elapsed.count()));

    return 0;
}