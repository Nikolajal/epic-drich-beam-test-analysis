#include "writers/lightdata.h"
#include <mist/logger/logger.h>
#include "utility/config_reader.h"
#include "utility/conf_path.h"
#include "utility.h"
#include <stdio.h>
#include <chrono>
#include <iostream>
#include <CLI/CLI.hpp>
#include <filesystem>
#include <TROOT.h>

int main(int argc, char **argv)
{
    //  Force ROOT batch mode before any TCanvas is created: the writer
    //  renders QA PDFs via off-screen canvases.  Without this, ROOT opens a
    //  blank Cocoa/X window per canvas at finalize, which steals OS focus
    //  when the run finishes.
    gROOT->SetBatch(kTRUE);

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
    //  Default empty — the writer is calibration-optional.  When the
    //  operator passes ``--fine-calib-conf`` the explicit path wins;
    //  otherwise we try to auto-resolve a ``fine_calibration.toml``
    //  in the run directory AFTER CLI parsing (the previous default
    //  composed the path here using data_repository/run_name which
    //  are still empty at this point — that bug always produced
    //  ``//fine_calibration.txt`` and made the writer crash on the
    //  legacy-.txt-extension guard).  Falls back to the empty string
    //  if no run-local calibration exists, which the writer treats
    //  as "skip fine corrections" (every channel → phase = 0).
    std::string fine_calibration_config_file;
    std::string framer_config_file;
    std::string streaming_config_file;
    //  Optional per-run database for the streaming-trigger threshold
    //  override (DISCUSSION §1.5.2 Option A).  When supplied, the
    //  driver looks up `streaming_n_sigma_threshold` in this TOML for
    //  the active run_name and forwards it as the per-call override.
    std::string run_database_file;
    //  Direct CLI override for the same field — bypasses the run
    //  database lookup.  Useful for ad-hoc operator tuning without
    //  editing the rundb file.  Takes priority over --run-database
    //  when both are supplied.
    float n_sigma_threshold_cli = 0.f;

    app.add_option("data_repository", data_repository)->required();
    app.add_option("run_name", run_name)->required();
    app.add_option("--run-list", RunList, "Name of run list (required if run_name is a .toml runlist)");
    app.add_option("--max-spill", max_spill);
    app.add_option("--threads", n_requested_threads);
    auto *p_trigger = app.add_option("--trigger-conf", trigger_config_file);
    auto *p_readout = app.add_option("--readout-conf", readout_config_file);
    auto *p_mapping = app.add_option("--Mapping-conf", mapping_config_file);
    auto *p_fine_calib = app.add_option(
        "--fine-calib-conf", fine_calibration_config_file,
        "Path to fine_calibration.toml (v3 schema).  If omitted, the "
        "writer auto-detects <data>/<run>/fine_calibration.toml and "
        "falls back to running without fine corrections if absent.");
    auto *p_framer = app.add_option("--framer-conf", framer_config_file);
    auto *p_streaming = app.add_option("--streaming-conf", streaming_config_file);
    app.add_option("--run-database", run_database_file,
        "Per-run metadata TOML (e.g. run-lists/2026.database.toml).  "
        "When supplied, the streaming-trigger n_sigma threshold for "
        "the active run is pulled from this file's "
        "`streaming_n_sigma_threshold` field and overrides the "
        "streaming-conf default.");
    app.add_option("--n-sigma-threshold", n_sigma_threshold_cli,
        "Direct override for `[streaming_trigger].n_sigma_threshold`.  "
        "Takes priority over --run-database; 0 disables this override "
        "path (legacy behaviour).");
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
        //  Auto-detect a run-local fine_calibration.toml only when
        //  the operator didn't pass --fine-calib-conf explicitly.
        //  We CANNOT default this above because data_repository and
        //  run_name are populated by CLI parsing, not before — the
        //  earlier ``data_repository + "/" + run_name + ...`` default
        //  always produced ``//fine_calibration.txt`` and crashed
        //  the writer on its v3-schema gate.  Empty path falls
        //  through to the writer's "skip fine corrections" branch.
        if (p_fine_calib->count() == 0 && !run_name.empty())
        {
            const std::string candidate =
                data_repository + "/" + run_name + "/fine_calibration.toml";
            if (std::filesystem::exists(candidate))
                fine_calibration_config_file = candidate;
        }
        if (qa_mode)
            mist::logger::info(TString::Format(
                                   "(lightdata_writer) --QA mode: streaming-conf=%s  framer-conf=%s",
                                   streaming_config_file.c_str(), framer_config_file.c_str())
                                   .Data());

        //  Resolve the per-run streaming-n_σ override.  Resolution order:
        //    1. --n-sigma-threshold X  (CLI direct, highest priority)
        //    2. --run-database PATH    (lookup streaming_n_sigma_threshold
        //                               for run_name; needs run_name to be
        //                               a single run id, not a runlist)
        //    3. 0                      (no override; streaming-conf wins)
        //
        //  --QA mode SKIPS both lookups.  In QA mode the operator wants
        //  the QA streaming-conf's behaviour (disable firing, accumulate
        //  score hists for offline n_σ picking) — a per-run override
        //  would clobber the 1e9 disable-sentinel and re-introduce
        //  firing cost on every QA run.  The whole point of QA mode is
        //  that the operator is STILL deciding the per-run threshold;
        //  they haven't committed to it yet.  Production launches
        //  (without --QA) pick the rundb value up naturally.
        //
        //  The lookup against the run database happens once here; per-run
        //  values inside a runlist are handled by re-doing the lookup
        //  inside the runlist loop below.
        if (!qa_mode && !run_database_file.empty())
            RunInfo::read_database(run_database_file);

        auto resolve_override_for_run =
            [&](const std::string &rid) -> float
        {
            if (qa_mode)
                return 0.f;  // QA mode: never override (see comment above)
            if (n_sigma_threshold_cli > 0.f)
                return n_sigma_threshold_cli;
            if (run_database_file.empty())
                return 0.f;
            auto info = RunInfo::get_run_info(rid);
            return info ? info->streaming_n_sigma_threshold : 0.f;
        };

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
                const float per_run_override = resolve_override_for_run(current_run_name);
                lightdata_writer(data_repository, current_run_name, max_spill, force_rebuild, n_requested_threads, trigger_config_file, readout_config_file, mapping_config_file, fine_calibration_config_file, framer_config_file, streaming_config_file, per_run_override);
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
            const float per_run_override = resolve_override_for_run(run_name);
            lightdata_writer(data_repository, run_name, max_spill, force_rebuild, n_requested_threads, trigger_config_file, readout_config_file, mapping_config_file, fine_calibration_config_file, framer_config_file, streaming_config_file, per_run_override);
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