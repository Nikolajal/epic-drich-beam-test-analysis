#include "recotrackdata_writer.h"
#include "alcor_recodata.h"
#include "alcor_recotrackdata.h"

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

    //  Load recodata, return if not available
    TFile *input_file_recodata = new TFile(input_filename_recodata.c_str());
    if (!input_file_recodata || input_file_recodata->IsZombie())
    {
        std::cerr << "[WARNING] Could not find recodata, making it" << std::endl;
        return;
    }

    //  Link recodata tree locally
    TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
    alcor_recodata *recodata = new alcor_recodata();
    recodata->link_to_tree(recodata_tree);

    //  Input recotrackdata
    std::string input_filename_recotrackdata = data_repository + "/" + run_name + "/ALTAI/tracks.txt";

    //  Load recotrackdata
    tracking_altai current_tracking(input_filename_recotrackdata);

    //  Prepare output file
    std::string outname = data_repository + "/" + run_name + "/recotrackdata.root";
    /*
    if (std::filesystem::exists(outname) && !force_recodata_rebuild)
    {
        std::cout << "[INFO] Output file already exists, skipping: " << outname << std::endl;
        return; // or continue;
    }
    */

    //  Link recodata tree locally
    //  TODO safer implementation: recodata MUST be initialised earlier, may break
    TFile *output_file = TFile::Open(outname.c_str(), "RECREATE");
    TTree *recotrackdata_tree = new TTree("recotrackdata", "Recotrackdata tree");
    alcor_recotrackdata *recotrackdata = new alcor_recotrackdata(*recodata); // TODO internal linking
    recotrackdata->write_to_tree(recotrackdata_tree);

    //  Get number of frames, limited to maximum requested frames
    auto n_frames = recodata_tree->GetEntries();
    auto all_frames = n_frames; // std::min((int)n_frames, (int)max_frames); // TODO: understand this

    //  Loop over frames
    auto i_spill = -1;
    auto n_spils = 0;
    auto altai_events_counter = -1;
    auto recotrack_events_counter = 0;
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        //  Load data for current frame
        recodata_tree->GetEntry(i_frame);

        //  Select Luca AND trigger (0) or timing trigger (101)
        auto current_trigger = recodata->get_triggers();
        auto it = std::find_if(current_trigger.begin(), current_trigger.end(), [](const trigger_struct &t)
                               { return t.index == 0; });
        if (it != current_trigger.end())
        {
            //  Keep track of the actual number of frames used in the analysis
            altai_events_counter++;

            //  Exclude events with 0 or multiple tracks
            if (!current_tracking.event_has_one_track(altai_events_counter))
                continue;

            //  Recotrack events counter
            recotrack_events_counter++;
            recotrackdata->import_event(current_tracking.get_event_tracks(altai_events_counter));

            recotrackdata_tree->Fill();
            recotrackdata->clear();
        }
    }
    logger::log_info(Form("(recotrackdata_writer) Matched %d frames to tracking trigger", recotrack_events_counter));
    recotrackdata_tree->Write();
    output_file->Close();
}