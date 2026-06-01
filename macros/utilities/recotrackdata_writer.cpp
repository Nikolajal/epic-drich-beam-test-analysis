#include "writers/recotrackdata.h"
#include <mist/logger/logger.h>
#include <stdio.h>
#include <CLI/CLI.hpp>
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
    std::string track_data_repository;
    std::string track_run_name;
    int max_spill = 1000;
    bool force_rebuild = false;
    bool force_upstream = false;
    bool qa_mode = false;
    //  Sweep audit: accept --threads as a no-op so the
    //  uniform qa_pipeline.py invocation doesn't reject this stage.
    //  recotrack doesn't yet plumb thread parallelism through; the
    //  CLI just needs to swallow the flag.  Same parking pattern in
    //  recodata.  See lightdata_writer for the live --threads path.
    int n_threads_unused = 0;

    app.add_option("data_repository", data_repository)->required();
    app.add_option("run_name", run_name)->required();
    app.add_option("track_data_repository", track_data_repository);
    app.add_option("track_run_name", track_run_name);
    app.add_option("--max-spill", max_spill);
    app.add_option("--threads", n_threads_unused,
                   "[ACCEPTED, IGNORED] reserved for future per-stage "
                   "thread plumbing — the qa_pipeline.py orchestrator "
                   "passes this uniformly to all stages.");
    //  Uniform force-flag contract across all writers (see
    //  include/writers/*.h docstrings):
    //    --force-rebuild   → overwrite THIS writer's output (recotrackdata.root).
    //    --force-upstream  → cascade: also rebuild upstream writers
    //                        (recodata, which itself cascades into lightdata).
    app.add_flag("--force-rebuild", force_rebuild);
    app.add_flag("--force-upstream", force_upstream);
    //  --QA accepted for CLI uniformity.  recotrackdata itself reads no
    //  config files; the flag only affects behaviour through the
    //  --force-upstream cascade into recodata_writer (which then loads
    //  conf/QA/* overrides if --QA was set there).  Since we don't
    //  re-invoke recodata from this binary's main() — the cascade
    //  happens inside recotrackdata_writer() — there's nothing to do
    //  here yet; pass-through plumbing for --QA will land alongside a
    //  qa_mode signature parameter when first needed.
    app.add_flag("--QA", qa_mode);

    CLI11_PARSE(app, argc, argv);

    auto start = std::chrono::high_resolution_clock::now();
    recotrackdata_writer(data_repository, run_name, track_data_repository, track_run_name, max_spill, force_rebuild, force_upstream);
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    //mist::logger::info(Form("Total time taken: %d seconds", elapsed.count()));

    return 0;
}