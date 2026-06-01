/**
 * @file macros/utilities/pulser_calib_writer.cpp
 * @brief CLI driver for @ref btana::pulser_calib_writer.
 *
 * Usage:
 *   pulser_calib_writer <repo> <run>
 *     [--calib-conf conf/calib/calibration_conf.toml]
 *     [--force-rebuild]
 *     [--max-spill N]
 *     [--anchor-device D] [--anchor-chip C] [--anchor-eo-channel E]
 *     [--pulser-frequency-hz F]
 *
 * Anchor overrides: each ``--anchor-*`` flag, when set ≥ 0, overrides
 * the corresponding field in ``calibration_conf.toml`` for this launch
 * only.  The dashboard's Run Manager card exposes these so an operator
 * can switch the anchor channel mid-shift (e.g. 192/0/ch0 hardware
 * fails) without rewriting the persistent TOML.  Sentinel -1 means
 * "use the TOML value" (the default).
 *
 * Pulser-frequency override: ``--pulser-frequency-hz`` takes the
 * generator setting in Hz (e.g. ``1000`` for 1 kHz) and converts it
 * to the internal ``pulser_period_cc`` using the 320 MHz clock
 * (``cc = 320e6 / Hz``).  This matches what an operator dials up on
 * the lab pulser generator without having to compute the cc value.
 * Sentinel -1 means "use the TOML value".  Positive ⇒ override.
 *
 * Pipeline (see include/writers/pulser_calib.h for the design notes):
 *   1. `lightdata_writer --calib <repo> <run>` produces lightdata.root
 *      with the anchor channel flowing as a normal hit.
 *   2. `pulser_calib_writer <repo> <run>` (this binary) produces
 *      <repo>/<run>/fine_calib.toml + <repo>/<run>/pulser_calib_qa.root.
 *   3. Subsequent `recodata_writer` / `lightdata_writer` calls pick up
 *      the new fine_calib.toml automatically via AlcorFinedata.
 */

#include "utility/conf_path.h"
#include "writers/pulser_calib.h"

#include <CLI/CLI.hpp>
#include <mist/logger/logger.h>

#include <cmath>
#include <string>

#include <TROOT.h>

int main(int argc, char **argv)
{
    //  Force ROOT batch mode before any TCanvas is created: the writer
    //  renders QA PDFs via off-screen canvases.  Without this, ROOT opens a
    //  blank Cocoa/X window per canvas at finalize, which steals OS focus
    //  when the run finishes.
    gROOT->SetBatch(kTRUE);

    CLI::App app{"Pulser-driven fine-time calibration writer"};

    std::string data_repository;
    std::string run_name;
    std::string calib_config_file;
    bool force_rebuild = false;
    int max_spill = -1;
    //  Sentinel -1 = "no CLI override"; ≥ 0 wins over calibration_conf.toml.
    //  CLI11 leaves these untouched when the user doesn't pass the flag,
    //  so the writer impl can do a single ``if (override >= 0)`` check.
    int anchor_device_override     = -1;
    int anchor_chip_override       = -1;
    int anchor_eo_channel_override = -1;
    //  Pulser frequency override (Hz).  -1.0 = no override; positive
    //  values are converted to ``pulser_period_cc`` below using the
    //  320 MHz ALCOR clock.  Mirrors what the operator types on the
    //  generator (e.g. ``1000`` for 1 kHz).
    double pulser_frequency_hz_override = -1.0;

    app.add_option("data_repository", data_repository, "Top-level data directory")->required();
    app.add_option("run_name", run_name, "Run sub-directory under data_repository")->required();
    auto *p_conf = app.add_option("--calib-conf", calib_config_file,
                                  "Calibration TOML (default: conf/calib/calibration_conf.toml)");
    app.add_flag("--force-rebuild", force_rebuild,
                 "Overwrite an existing fine_calib.toml in the run dir");
    app.add_option("--max-spill", max_spill,
                   "Cap on spills processed (default: -1 = all)");
    app.add_option("--anchor-device", anchor_device_override,
                   "Override calibration_conf.toml's anchor_device for this "
                   "launch only.  -1 (default) keeps the TOML value.");
    app.add_option("--anchor-chip", anchor_chip_override,
                   "Override calibration_conf.toml's anchor_chip.  "
                   "-1 keeps the TOML value.");
    app.add_option("--anchor-eo-channel", anchor_eo_channel_override,
                   "Override calibration_conf.toml's anchor_eo_channel.  "
                   "-1 keeps the TOML value.");
    app.add_option("--pulser-frequency-hz", pulser_frequency_hz_override,
                   "Override calibration_conf.toml's pulser period from a "
                   "frequency in Hz (e.g. 1000 for 1 kHz).  Converted "
                   "internally to pulser_period_cc using the 320 MHz clock "
                   "(cc = 320e6 / Hz).  -1 keeps the TOML value.");

    CLI11_PARSE(app, argc, argv);

    //  Unset --calib-conf falls through to the conventional path; route
    //  through util::conf_path so a user with conf/calib/ overrides
    //  picks up local edits without an explicit --calib-conf.
    if (p_conf->count() == 0)
        calib_config_file = util::conf_path("calibration_conf.toml", std::string{"calib"});

    //  Hz → cc conversion.  Lives here (the CLI driver, not the
    //  writer fn) so the writer's signature stays at the level of the
    //  underlying conf field (``pulser_period_cc``) — same currency
    //  the TOML uses.  Constant repeated rather than imported because
    //  there's no system-wide clock-rate header today; if one
    //  appears, swap it in.  320 MHz matches ``CC_TO_NS = 3.125``
    //  used elsewhere in the writer impl.
    constexpr double kAlcorClockHz = 320.0e6;
    double pulser_period_cc_override = -1.0;
    if (pulser_frequency_hz_override > 0.0)
    {
        pulser_period_cc_override = std::round(kAlcorClockHz /
                                               pulser_frequency_hz_override);
        mist::logger::info("(pulser_calib_writer_main) --pulser-frequency-hz=" +
                           std::to_string(pulser_frequency_hz_override) +
                           " -> pulser_period_cc=" +
                           std::to_string(pulser_period_cc_override));
    }

    btana::pulser_calib_writer(data_repository, run_name,
                               calib_config_file, force_rebuild, max_spill,
                               anchor_device_override,
                               anchor_chip_override,
                               anchor_eo_channel_override,
                               pulser_period_cc_override);
    return 0;
}
