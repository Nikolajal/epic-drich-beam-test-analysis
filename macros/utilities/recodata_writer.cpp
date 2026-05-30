#include "writers/recodata.h"
#include <mist/logger/logger.h>
#include "utility/config_reader.h"
#include "utility/conf_path.h"
#include "utility.h"
#include <stdio.h>
#include <CLI/CLI.hpp>

int main(int argc, char **argv)
{
    CLI::App app{"Recodata writer"};

    std::string data_repository;
    std::string run_name;
    //  Mapping default is the active master `conf/mapping_conf.toml` —
    //  a symlink managed by the QA dashboard's "setting set" picker
    //  (defaults / 2023 / 2024 / 2025 / working).  Pass --Mapping-conf
    //  to override against a specific file (e.g. when running outside
    //  the dashboard).
    std::string mapping_conf = "conf/mapping_conf.toml";
    std::string trigger_config_file;
    std::string framer_config_file;
    std::string recodata_config_file;
    std::string streaming_config_file;
    std::string RunList;

    int max_spill = 1000;
    bool force_rebuild = false;
    bool force_upstream = false;
    bool qa_mode = false;
    //  Sweep audit (2026-05-30): accept --threads as a no-op so the
    //  uniform qa_pipeline.py invocation
    //  (`writer ... --threads N`) doesn't reject this stage.  Recodata
    //  doesn't yet plumb thread parallelism through, but the CLI
    //  needs to swallow the flag.  Same parking pattern in recotrack.
    int n_threads_unused = 0;

    app.add_option("data_repository", data_repository)->required();
    app.add_option("run_name", run_name)->required();
    app.add_option("--run-list", RunList, "Name of run list (required if run_name is a .toml runlist)");
    app.add_option("--max-spill", max_spill);
    app.add_option("--threads", n_threads_unused,
                   "[ACCEPTED, IGNORED] reserved for future per-stage "
                   "thread plumbing — the qa_pipeline.py orchestrator "
                   "passes this uniformly to all stages.");
    app.add_option("--Mapping-conf", mapping_conf);
    auto *p_trigger = app.add_option("--trigger-conf", trigger_config_file);
    auto *p_framer = app.add_option("--framer-conf", framer_config_file);
    auto *p_recodata = app.add_option("--recodata-conf", recodata_config_file);
    //  Recodata never reads this file directly, but it's forwarded into
    //  the lightdata cascade when --force-upstream is set so that --QA
    //  Hough thresholds propagate through the pipeline in one command.
    auto *p_streaming = app.add_option("--streaming-conf", streaming_config_file);
    //  Uniform force-flag contract across all writers (see
    //  include/writers/*.h docstrings):
    //    --force-rebuild   → overwrite THIS writer's output (recodata.root).
    //    --force-upstream  → cascade: also rebuild upstream writers
    //                        (lightdata, in this case).
    app.add_flag("--force-rebuild", force_rebuild);
    app.add_flag("--force-upstream", force_upstream);
    //  Fast-feedback QA mode.  Reads tuned conf/QA/*.toml overrides
    //  when present (e.g. raised Hough thresholds → biases N_γ up
    //  but keeps σ_photon ~invariant).  See conf/QA/streaming.toml.
    app.add_flag("--QA", qa_mode);

    try
    {
        CLI11_PARSE(app, argc, argv);

        //  Resolve any unset --xxx-conf option through util::conf_path,
        //  which redirects to conf/<mode>/<basename> when the override
        //  exists.  Subdir-string form (the bool form is `[[deprecated]]`).
        const std::string mode = qa_mode ? std::string{"QA"} : std::string{};
        if (p_trigger->count() == 0)
            trigger_config_file = util::conf_path("trigger_conf.toml", mode);
        if (p_framer->count() == 0)
            framer_config_file = util::conf_path("framer_conf.toml", mode);
        if (p_recodata->count() == 0)
            recodata_config_file = util::conf_path("recodata.toml", mode);
        if (p_streaming->count() == 0)
            streaming_config_file = util::conf_path("streaming.toml", mode);
        if (qa_mode)
            mist::logger::info(TString::Format(
                                   "(recodata_writer) --QA mode: trigger-conf=%s  framer-conf=%s  "
                                   "recodata-conf=%s  streaming-conf=%s",
                                   trigger_config_file.c_str(), framer_config_file.c_str(),
                                   recodata_config_file.c_str(), streaming_config_file.c_str())
                                   .Data());

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
                recodata_writer(data_repository, current_run_name, max_spill, force_rebuild, force_upstream, mapping_conf, trigger_config_file, framer_config_file, recodata_config_file, streaming_config_file);
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
            recodata_writer(data_repository, run_name, max_spill, force_rebuild, force_upstream, mapping_conf, trigger_config_file, framer_config_file, recodata_config_file, streaming_config_file);
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