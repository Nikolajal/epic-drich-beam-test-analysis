#include "writers/recotrackdata.h"
#include <mist/logger/logger.h>
#include "alcor_data.h"
#include "writers/recodata.h"
#include "alcor_recodata.h"
#include "alcor_recotrackdata.h"
#include <filesystem>
#include <memory>
// TFilePtr is available via the alcor_recodata.h → utility.h → util/root_io.h chain.

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
        recodata_writer(data_repository, run_name, /*max_spill=*/max_frames,
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
    //  TODO safer implementation: recodata MUST be initialised earlier, may break
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
    // NOTE: *recodata dereference invokes AlcorRecotrackdata(const AlcorRecodata&)
    // which today copies the branch-binding *_ptr members verbatim — a latent
    // dangle once 's no-copy contract lands.  Documented; fix follows in .
    auto recotrackdata = std::make_unique<AlcorRecotrackdata>(*recodata);
    recotrackdata->write_to_tree(recotrackdata_tree);

    //  Get number of frames, limited to maximum requested frames
    auto n_frames = recodata_tree->GetEntries();
    auto all_frames = n_frames; // std::min((int)n_frames, (int)max_frames); // TODO: understand this

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
    // unique_ptr<TFile> dtor calls Close() and deletes the object — no manual
    // output_file->Close(); + leak.
}
