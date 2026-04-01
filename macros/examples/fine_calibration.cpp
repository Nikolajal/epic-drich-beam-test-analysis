#include "../lib_loader.h"
#include <mist/mist.h>

/**
 * @file fina_calibration.cpp
 * @brief Examine the limits of calibration 
 *
 * @details
 * This exercise is still under development
 *
 * @author Nicola Rubini
 */

void fine_calibration(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
    //  --- --- --- --- --- ---
    //  Input files
    //  ---
    std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";
    TFile *input_file_recodata = new TFile(input_filename_recodata.c_str());
    if (!input_file_recodata || input_file_recodata->IsZombie())
    {
        mist::logger::error("recodata file not found: " +
                            input_filename_recodata +
                            " does not exist");
        return;
    }
    //  Link recodata tree locally
    TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
    if (!recodata_tree)
    {
        mist::logger::error("recodata tree not found in file: " +
                            input_filename_recodata);
        return;
    }
    alcor_recodata *recodata = new alcor_recodata();
    recodata->link_to_tree(recodata_tree);
    alcor_finedata::read_calib_from_file(data_repository + "/" + run_name + "/timing_fine_calib.txt");
    //  Get number of frames, limited to maximum requested frames
    auto n_frames = recodata_tree->GetEntries();
    auto all_frames = min((int)n_frames, (int)max_frames);
    //  ---
    //  End: Input files
    //  --- --- --- --- --- ---

    //  --- --- --- --- --- ---
    //  Output definition
    //  ---
    TH2F *h_time_diff_to_channel = new TH2F("h_time_diff_to_channel", ";channel index;#Delta_{t} (t_{hit} - t_{trigger})", 2048, -0.5, 2047.5, 400, -20, 20);
    TH2F *h_time_diff_to_device = new TH2F("h_time_diff_to_device", ";device index;#Delta_{t} (t_{hit} - t_{trigger})", 8, 191.5, 199.5, 2e3, -200, 200);
    TH2F *h_time_diff_to_device_channel = new TH2F("h_time_diff_to_device", ";device index;#Delta_{t} (t_{hit} - t_{trigger})", 64, -0.5, 63.5, 2e3, -200, 200);
    /*
    //  Time distribution
    TH1F *h_t_distribution = new TH1F("h_t_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    TH1F *h_t_AP_distribution = new TH1F("h_t_AP_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    TH1F *h_t_noAP_distribution = new TH1F("h_t_noAP_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    //  Time distribution
    TH1F *h_t_detector_0 = new TH1F("h_t_detector_0", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    TH1F *h_t_detector_1 = new TH1F("h_t_detector_1", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
*/
    //  ---
    //  End: Output definition
    //  --- --- --- --- --- ---

    //  --- --- --- --- --- ---
    //  Loop on data
    //  ---
    auto i_spill = -1;
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        //  Load data for current frame
        recodata_tree->GetEntry(i_frame);

        //  Takes note of spill evolution
        if (recodata->is_start_of_spill())
        {
            //  You can internally keep track of spills
            i_spill++;

            //  This event is not of physical interest, skip it
            continue;
        }

        //  Select Luca AND trigger (0) or timing trigger (101)
        auto default_hardware_trigger = recodata->get_trigger_by_index(101);
        if (default_hardware_trigger)
        {
            //  Loop on hits
            std::vector<std::array<float, 2>> selected_points;
            for (auto current_hit = 0; current_hit < recodata->get_recodata().size(); current_hit++)
            {
                //  Avoid afterpulse
                if (recodata->is_afterpulse(current_hit))
                    continue;

                //  Fill histograms
                h_time_diff_to_device->Fill(recodata->get_device(current_hit), recodata->get_hit_t(current_hit) - default_hardware_trigger->fine_time);
                h_time_diff_to_device_channel->Fill(recodata->get_eo_channel(current_hit), recodata->get_hit_t(current_hit) - default_hardware_trigger->fine_time);
                h_time_diff_to_channel->Fill(recodata->get_global_channel_index(current_hit), recodata->get_hit_t(current_hit) - default_hardware_trigger->fine_time);
            }
        }
    }
    //  ---
    //  End: Loop on data
    //  --- --- --- --- --- ---

    //  --- --- --- --- --- ---
    //  Results plots
    //  ---
    new TCanvas();
    h_time_diff_to_channel->Draw("COLZ");
    new TCanvas();
    h_time_diff_to_device->Draw("COLZ");
    new TCanvas();
    h_time_diff_to_device_channel->Draw("COLZ");
    //  ---
    //  End: Results plots
    //  --- --- --- --- --- ---
}
