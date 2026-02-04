#include "recodata_writer.h"
#include <stdio.h>
#include <CLI/CLI.hpp>

int main(int argc, char **argv)
{

  CLI::App app{"Beam test analysis"};

  std::string data_repository;
  std::string run_name;
  int max_spill = 1000;
  bool force_recodata_rebuild = false;
  bool force_lightdata_rebuild = false;

  app.add_option("data_repository", data_repository)->required();
  app.add_option("run_name", run_name)->required();
  app.add_option("--max-spill", max_spill);
  app.add_flag("--force-recodata", force_recodata_rebuild);
  app.add_flag("--force-lightdata", force_lightdata_rebuild);

  CLI11_PARSE(app, argc, argv);

  recodata_writer(data_repository, run_name, max_spill, force_recodata_rebuild, force_lightdata_rebuild);

  return 0;
}