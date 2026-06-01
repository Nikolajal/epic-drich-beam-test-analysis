#include "writers/recotrackdata.h"
#include <mist/logger/logger.h>
#include "alcor_data.h"
#include "writers/recodata.h"
#include "alcor_recodata.h"
#include "alcor_recotrackdata.h"
#include "analysis_results.h"
#include "utility/config_dump.h"
#include <filesystem>
#include <limits>
#include <memory>
// TFilePtr is available via the alcor_recodata.h → utility.h → utility/root_io.h chain.

void recotrackdata_writer(
    std::string data_repository,
    std::string run_name,
    std::string track_data_repository,
    std::string track_run_name,
    int max_frames,
    bool force_rebuild,
    bool force_upstream)
{
    //  Output recotrackdata file.  Skip the whole pipeline if it
    //  exists and the caller didn't ask for a rebuild — the uniform
    //  flag contract (this writer's --force-rebuild gates output
    //  overwrite, --force-upstream cascades to upstream writers).
    const std::string outname = data_repository + "/" + run_name + "/recotrackdata.root";
    if (std::filesystem::exists(outname) && !force_rebuild)
    {
        mist::logger::info(TString::Format(
                               "(recotrackdata_writer) %s exists and --force-rebuild not set — "
                               "skipping (use --force-rebuild to overwrite).",
                               outname.c_str())
                               .Data());
        return;
    }

    //  Input recodata file — mirror recodata_writer's auto-cascade
    //  behaviour: if recodata.root is missing/corrupt OR force_upstream
    //  is set, invoke recodata_writer (which itself cascades into
    //  lightdata_writer when needed).  Emits a warning when the cascade
    //  is triggered implicitly (missing file) so the operator notices
    //  they're rebuilding the chain rather than silently doing nothing.
    const std::string input_filename_recodata =
        data_repository + "/" + run_name + "/recodata.root";
    TFilePtr input_file_recodata(TFile::Open(input_filename_recodata.c_str()));
    const bool recodata_missing = !input_file_recodata || input_file_recodata->IsZombie();
    if (recodata_missing || force_upstream)
    {
        if (recodata_missing && !force_upstream)
            mist::logger::warning(
                "(recotrackdata_writer) " + input_filename_recodata +
                " missing or corrupt — auto-cascading into recodata_writer.  "
                "Pass --force-upstream explicitly to skip this implicit cascade "
                "(or to force a rebuild even when the file is present).");
        else
            mist::logger::info(
                "(recotrackdata_writer) --force-upstream set — rebuilding recodata "
                "(which itself cascades into lightdata).");
        //  UNIT MISMATCH — DO NOT forward max_frames here.  This writer's
        //  --max-spill knob actually caps FRAMES (recodata is per-frame; the
        //  CLI variable name is a hold-over from a pre-frame-pipeline era).
        //  recodata_writer's max_spill is a SPILL cap.  Forwarding the
        //  frame cap as a spill cap silently truncates the upstream rebuild
        //  to the wrong unit — `--max-spill 30` rebuilt 30 spills of
        //  recodata, then locally capped to 30 frames, hiding tracking
        //  failures.  Pass INT_MAX so the cascade rebuilds the full run;
        //  the local frame cap below still honours the operator's intent.
        recodata_writer(data_repository, run_name,
                        /*max_spill=*/std::numeric_limits<int>::max(),
                        /*force_rebuild=*/true,
                        /*force_upstream=*/force_upstream);
        input_file_recodata.reset(TFile::Open(input_filename_recodata.c_str()));
        if (!input_file_recodata || input_file_recodata->IsZombie())
        {
            mist::logger::error(
                "(recotrackdata_writer) " + input_filename_recodata +
                " still missing after auto-cascade — aborting.");
            return;
        }
    }

    //  Link recodata tree locally
    auto *recodata_tree = input_file_recodata->Get<TTree>("recodata");
    if (!recodata_tree)
    {
        mist::logger::error(TString::Format("(recotrackdata_writer) 'recodata' tree missing in %s",
                                            input_filename_recodata.c_str())
                                .Data());
        return;
    }
    auto recodata = std::make_unique<AlcorRecodata>();
    recodata->link_to_tree(recodata_tree);

    //  Input recotrackdata
    std::string input_filename_recotrackdata = data_repository + "/" + run_name + "/ALTAI/tracks.txt";

    //  Load recotrackdata
    TrackingAltai current_tracking(input_filename_recotrackdata);

    //  Prepare output file.  `outname` was already declared at the
    //  top of this function for the --force-rebuild guard; reuse it.
    TFilePtr output_file(TFile::Open(outname.c_str(), "RECREATE"));
    if (!output_file || output_file->IsZombie())
    {
        mist::logger::error(TString::Format("(recotrackdata_writer) Failed to open output %s for writing", outname.c_str()).Data());
        return;
    }
    // TDirectory::TContext RAII guard so every Write() below lands in
    // output_file regardless of where gDirectory might wander to in between.
    TDirectory::TContext ctx(output_file.get());

    TTree *recotrackdata_tree = new TTree("recotrackdata", "Recotrackdata tree");
    //  Defensive init-order guard — `recodata` is constructed via
    //  make_unique above (line ~81) and bound to the input tree
    //  immediately after.  An empty unique_ptr here would mean control
    //  flow took a path we don't expect (a future refactor that moves
    //  the bind earlier without re-ordering the construct, say);
    //  failing loud + early is cheaper than the silent UB the
    //  `*recodata` deref would emit.
    if (!recodata)
    {
        mist::logger::error(
            "(recotrackdata_writer) internal: recodata is null at "
            "AlcorRecotrackdata construction — init-order broke.  "
            "Aborting before deref.");
        return;
    }
    // NOTE: *recodata dereference invokes AlcorRecotrackdata(const AlcorRecodata&)
    // which today copies the branch-binding *_ptr members verbatim — a latent
    // dangle once a no-copy contract lands.  Documented; fix follows separately.
    auto recotrackdata = std::make_unique<AlcorRecotrackdata>(*recodata);
    recotrackdata->write_to_tree(recotrackdata_tree);

    //  Get number of frames, capped at the caller's --max-frames knob.
    //  Note: max_frames is also forwarded to upstream recodata_writer (as
    //  max_spill) on the cascade path above, but when recodata.root already
    //  exists we skip the cascade — without this local cap the CLI flag
    //  would silently do nothing on second-pass runs.
    auto n_frames = recodata_tree->GetEntries();
    auto all_frames = std::min<Long64_t>(n_frames, max_frames);

    //  Loop over frames
    auto i_spill = -1;
    auto n_spils = 0;
    auto altai_events_counter = -1;
    auto recotrack_events_counter = 0;
    std::map<int, int> per_spill_events_counter_recodata;
    std::map<int, int> per_spill_events_counter_trackdata;
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        //  Load data for current frame
        recodata_tree->GetEntry(i_frame);

        //  HitmaskDeadLane signals the event is start of spill, tells which channels are available
        // const& over get_triggers_link() to avoid copying the whole vector per frame.
        const auto &current_trigger = recodata->get_triggers_link();
        auto it = std::find_if(current_trigger.begin(), current_trigger.end(), [](const TriggerEvent &t)
                               { return t.index == TriggerStartOfSpill; });
        if (it != current_trigger.end())
        {
            //  Spill management
            i_spill++;
            n_spils++;

            //  This event is not of physical interest
            continue;
        }

        //  Select Luca AND trigger (0) or timing trigger (101)
        it = std::find_if(current_trigger.begin(), current_trigger.end(), [](const TriggerEvent &t)
                          { return t.index == 0; });
        if (it != current_trigger.end())
        {
            //  Trigger found, trigger 0 is the sync trigger for tracking
            per_spill_events_counter_recodata[i_spill]++;

            //  Keep track of the actual number of frames used in the analysis
            altai_events_counter++;

            //  Recotrack events counter
            recotrack_events_counter++;
            recotrackdata->import_event(current_tracking.get_event_tracks(altai_events_counter));

            recotrackdata_tree->Fill();
            recotrackdata->clear();
        }
    }

    mist::logger::info(TString::Format("(recotrackdata_writer) Matched %d frames to tracking trigger", recotrack_events_counter).Data());
    // TContext ctx (above) ensures Write lands in output_file; explicit cd
    // for redundancy in case the RAII guard's scope ever moves.
    output_file->cd();
    recotrackdata_tree->Write();

    //  ---
    //  --- Config — self-describing parameter dump.
    //
    //  Routed through util::ConfigDump for uniformity with the other
    //  writers.  recotrack reads no TOML conf files directly (its
    //  knobs are all CLI flags + the upstream recodata.root contents)
    //  so this dump is minimal — runtime flags plus the absolute
    //  paths of every input artefact so the build is reproducible
    //  from this file alone.  Outer-tab guard (TDirectory::TContext
    //  in ConfigDump's ctor) restores gDirectory on dtor.
    {
        util::ConfigDump dump(output_file.get());
        dump.add("max_frames",          max_frames)
            .add("force_rebuild",       force_rebuild)
            .add("force_upstream",      force_upstream)
            .add_path("input_recodata_root", input_filename_recodata)
            .add_path("input_tracks_txt",    input_filename_recotrackdata)
            .add_path("track_data_repository", track_data_repository)
            .add_path("track_run_name",        track_run_name)
            .add("frames_matched_to_tracks", recotrack_events_counter)
            .add("spills_seen",              n_spils);
    }

    //  ---
    //  --- Publish cross-run scalars to AnalysisResults.
    //
    //  Same dual-backend store as lightdata writes to.  Sensor key
    //  is "all" here since recotrack doesn't read the readout config
    //  directly (it inherits everything via recodata).
    {
        //  Cross-run aggregate sits next to the run directories
        //  (``<data_repository>/standard_results.toml`` = ``Data/``, NOT
        //  the git repo root) — same convention as recodata + lightdata
        //  + calibration writers.  The earlier ``extData/`` literal was
        //  a stale hard-code.
        AnalysisResults ar(data_repository + "/standard_results.toml");
        ar.update(ResultMap{
            {{run_name, "all", "recotrack.n_matched_tracks"},
             {static_cast<double>(recotrack_events_counter), 0.0}},
            {{run_name, "all", "recotrack.n_spills"},
             {static_cast<double>(n_spils), 0.0}},
            {{run_name, "all", "recotrack.n_frames"},
             {static_cast<double>(n_frames), 0.0}},
        }, /*source=*/"recotrack");
    }
    // unique_ptr<TFile> dtor calls Close() and deletes the object — no manual
    // output_file->Close(); + leak.
}
