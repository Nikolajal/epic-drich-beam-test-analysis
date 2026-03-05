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
  int n_requested_threads = -1;
  bool force_lightdata_rebuild = false;

  app.add_option("data_repository", data_repository)->required();
  app.add_option("run_name", run_name)->required();
  app.add_option("--max-spill", max_spill);
  app.add_option("--threads", n_requested_threads);
  app.add_flag("--force-rebuild", force_lightdata_rebuild);

  CLI11_PARSE(app, argc, argv);

  auto start = std::chrono::high_resolution_clock::now();
  lightdata_writer(data_repository, run_name, max_spill, force_lightdata_rebuild, n_requested_threads);
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed = end - start;
  logger::log_info(Form("Total time taken: %f seconds", elapsed.count()));

  return 0;
}