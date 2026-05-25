#include "writers/recotrackdata.h"
#include "alcor_recodata.h"
#include "alcor_recotrackdata.h"
#include <memory>
// TFilePtr is available via the alcor_recodata.h → utility.h → util/root_io.h chain.

void recotrackdata_writer(
    std::string data_repository,
    std::string run_name,
    std::string track_data_repository,
    std::string track_run_name,
    int max_frames,
    bool force_recodata_rebuild,
    bool force_lightdata_rebuild)
//  TODO Add force rebuild for recotrackdata
{
    //  Input recodata files
    std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";

    //  Load recodata, return if not available.  TFilePtr: closes + deletes on
    //  scope exit; ROOT objects attached to the TFile are freed by Close().
    TFilePtr input_file_recodata(TFile::Open(input_filename_recodata.c_str()));
    if (!input_file_recodata || input_file_recodata->IsZombie())
    {
        mist::logger::warning("(recotrackdata_writer) Could not find recodata, making it");
        return;
    }

    //  Link recodata tree locally
    auto *recodata_tree = input_file_recodata->Get<TTree>("recodata");
    if (!recodata_tree)
    {
        mist::logger::error(TString::Format("(recotrackdata_writer) 'recodata' tree missing in %s",
                                 input_filename_recodata.c_str()).Data());
        return;
    }
    auto recodata = std::make_unique<AlcorRecodata>();
    recodata->link_to_tree(recodata_tree);

    //  Input recotrackdata
    std::string input_filename_recotrackdata = data_repository + "/" + run_name + "/ALTAI/tracks.txt";

    //  Load recotrackdata
    TrackingAltai current_tracking(input_filename_recotrackdata);

    //  Prepare output file
    std::string outname = data_repository + "/" + run_name + "/recotrackdata.root";

    //  Link recodata tree locally
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
    // dangle once D-08's no-copy contract lands.  Documented; fix follows in D-08.
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
