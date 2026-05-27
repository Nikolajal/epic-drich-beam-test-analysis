/**
 * @file macros/utilities/pulser_calib_writer.cpp
 * @brief CLI driver for @ref btana::pulser_calib_writer.
 *
 * Usage:
 *   pulser_calib_writer <repo> <run>
 *     [--calib-conf conf/calib/calibration_conf.toml]
 *     [--force-rebuild]
 *     [--max-spill N]
 *
 * Pipeline (see include/writers/pulser_calib.h for the design notes):
 *   1. `lightdata_writer --calib <repo> <run>` produces lightdata.root
 *      with the anchor channel flowing as a normal hit.
 *   2. `pulser_calib_writer <repo> <run>` (this binary) produces
 *      <repo>/<run>/fine_calib.txt + <repo>/<run>/pulser_calib_qa.root.
 *   3. Subsequent `recodata_writer` / `lightdata_writer` calls pick up
 *      the new fine_calib.txt automatically via AlcorFinedata.
 */

#include "utility/conf_path.h"
#include "writers/pulser_calib.h"

#include <CLI/CLI.hpp>
#include <mist/logger/logger.h>

#include <string>

int main(int argc, char **argv)
{
    CLI::App app{"Pulser-driven fine-time calibration writer"};

    std::string data_repository;
    std::string run_name;
    std::string calib_config_file;
    bool force_rebuild = false;
    int max_spill = -1;

    app.add_option("data_repository", data_repository, "Top-level data directory")->required();
    app.add_option("run_name", run_name, "Run sub-directory under data_repository")->required();
    auto *p_conf = app.add_option("--calib-conf", calib_config_file,
                                  "Calibration TOML (default: conf/calib/calibration_conf.toml)");
    app.add_flag("--force-rebuild", force_rebuild,
                 "Overwrite an existing fine_calib.txt in the run dir");
    app.add_option("--max-spill", max_spill,
                   "Cap on spills processed (default: -1 = all)");

    CLI11_PARSE(app, argc, argv);

    //  Unset --calib-conf falls through to the conventional path; route
    //  through util::conf_path so a user with conf/calib/ overrides
    //  picks up local edits without an explicit --calib-conf.
    if (p_conf->count() == 0)
        calib_config_file = util::conf_path("calibration_conf.toml", std::string{"calib"});

    btana::pulser_calib_writer(data_repository, run_name,
                               calib_config_file, force_rebuild, max_spill);
    return 0;
}
