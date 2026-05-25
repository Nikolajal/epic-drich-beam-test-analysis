#include "../lib_loader.h"
#include "util/root_io.h"
#include "util/root_hist.h"

/**
 * @file ring_spatial_resolution.cpp
 * @brief Calculate the spatial resolution of the ring.
 *
 * This exercise estimates the center and radius of a ring of hits and then
 * computes the spatial resolution using multiple methods.
 *
 * @details
 * **Workflow:**
 * 1. **Initial ring center and radius estimation**
 *    - Select points tagged as "ring-like" by a density scan algorithm
 *      (based on time coincidence and radial proximity).
 *    - Filter hits that are **not labeled as After-Pulses (APs)** and within a
 *      reasonable time window relative to the trigger.
 *    - Fit the selected points with a circle to determine the center `(x_0, y_0)`
 *      and radius `R`.
 *    - Fixing the center will aid the subsequent resolution calculation.
 *
 * 2. **Spatial resolution calculation** (three methods):
 *    - **Method 1: Variable resolution vs. participant hits**
 *      - Fit points assigned to the ring.
 *      - Plot resulting radius as a function of the number of participant hits (photo-electrons, p.e.).
 *
 *    - **Method 2: Single Photon Spatial Resolution (SPSR)**
 *      - Fit points assigned to the ring.
 *      - Remove one point at a time.
 *      - Compute the difference in radius between the fit result and the removed point.
 *
 *    - **Method 3: SPSR alternative**
 *      - Compute the difference in radius between the first-round radius result
 *        and each selected point.
 *
 * **Notes:**
 * - Method 1 provides variable resolution as a function of participants (p.e.).
 * - Methods 2 and 3 provide the spatial resolution for a single photon.
 *
 * @author Nicola Rubini
 */

std::array<float, 2> time_cut_boundaries = {-45., 20.};

void ring_spatial_resolution(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
    //  Input files
    std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";

    //  Load recodata, return if not available
    TFilePtr input_file_recodata(TFile::Open(input_filename_recodata.c_str(), "READ"));
    if (!input_file_recodata || input_file_recodata->IsZombie())
    {
        std::cerr << "[WARNING] Could not find recodata, making it" << std::endl;
        return;
    }

    //  Link recodata tree locally
    TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
    AlcorRecodata *recodata = new AlcorRecodata();
    recodata->link_to_tree(recodata_tree);

    //  Get number of frames, limited to maximum requested frames
    auto n_frames = recodata_tree->GetEntries();
    auto all_frames = min((int)n_frames, (int)max_frames);

    //  Time distribution
    RootHist<TH1F> h_t_distribution("h_t_distribution", ";t_{Hit} - t_{timing} (ns)", 200, -312.5, 312.5);
    //  First round X, Y, R
    RootHist<TH1F> h_first_round_X("h_first_round_X", ";circle center x coordinate (mm)", 120, -30, 30);
    RootHist<TH1F> h_first_round_Y("h_first_round_Y", ";circle center y coordinate (mm)", 120, -30, 30);
    RootHist<TH1F> h_first_round_R("h_first_round_R", ";circle radius (mm)", 200, 30, 130);
    //  Second round selection
    RootHist<TH2F> h_second_round_xy_map("h_second_round_xy_map", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_second_round_R_Ngamma("h_second_round_R_Ngamma", ";circle radius (mm);N_{#gamma}", 200, 30, 130, 97, 3, 100);
    RootHist<TH1F> h_second_round_R_excluded("h_second_round_R_excluded", ";circle radius - point radius (mm)", 120, -30, 30);
    RootHist<TH1F> h_second_round_R_global("h_second_round_R_global", ";circle radius - point radius (mm)", 120, -30, 30);

    //  Saving the frame numebr you can speed up secondary loops
    std::vector<int> start_of_spill_frame_ref;
    std::vector<std::pair<int, float>> frame_of_interest_ref;

    //  Loop over frames
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        //  Load data for current frame
        recodata_tree->GetEntry(i_frame);

        //  Takes note of spill evolution
        if (recodata->is_start_of_spill())
        {
            //  You can internally keep track of spills

            //  This event is not of physical interest, skip it
            continue;
        }

        //  Select Luca AND trigger (0) or timing trigger (101)
        auto default_hardware_trigger = recodata->get_trigger_by_index(0);
        if (default_hardware_trigger)
        {
            //  Save trigger frames for later, ref to the actual number of used frames in the analysis
            frame_of_interest_ref.push_back({i_frame, default_hardware_trigger->fine_time});

            //  Container for selected hits
            std::vector<std::array<float, 2>> selected_points;
            float avg_radius = 0.; // First estimate for radius

            //  Loop on hits
            for (auto current_hit = 0; current_hit < recodata->get_recodata().size(); current_hit++)
            {
                //  Remove afterpulse
                //  Ref: afterpulse_treatment.cpp
                if (recodata->is_afterpulse(current_hit))
                    continue;

                //  Fill time distribution to check
                auto time_delta_wrt_ref = recodata->get_hit_t(current_hit) - default_hardware_trigger->fine_time; //  ns
                h_t_distribution->Fill(time_delta_wrt_ref);

                //  Ask for time coincidence
                if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
                    continue;

                //  Check the Hit has been labeled as ring-belonging
                //  This is done through a simple DBSCAN implementation
                //  Density-Based Spatial Clustering of Applications with Noise > https://it.wikipedia.org/wiki/DBSCAN
                //  Clustering is done in R and t, \phi is ignored (radial simmetry of cricle)
                //  Clustering is done in AlcorRecodata::find_rings(...)
                //  TODO: add a flag for sensor type
                if (!recodata->is_ring_tagged(current_hit))
                    continue;

                //  Store selected points
                selected_points.push_back({recodata->get_hit_x(current_hit), recodata->get_hit_y(current_hit)});
                avg_radius += recodata->get_hit_r(current_hit);
            }

            //  Fit selected points, if enough for circle fit (> 3)
            if (selected_points.size() > 4)
            {
                //  Fitting the points
                //  fit_result = {{center_x_value,center_x_error}, {center_y_value,center_y_error}, {radius_value,radius_error}}
                //                fit_circle(points to fit,  starting values for the fit,  let X-Y free,  do not exclude any points)
                auto fit_result = fit_circle(selected_points, {0., 0., avg_radius / selected_points.size()}, false, {{}});

                //  Save results for later QA
                h_first_round_X->Fill(fit_result[0][0]);
                h_first_round_Y->Fill(fit_result[1][0]);
                h_first_round_R->Fill(fit_result[2][0]);
            }
        }
    }

    auto found_ring_center_x = h_first_round_X->GetMean();
    auto found_ring_center_y = h_first_round_Y->GetMean();
    auto found_ring_radius = h_first_round_R->GetMean();
    auto found_ring_radius_stddev = h_first_round_R->GetRMS();

    //  Second loop on frames of interest
    for (auto i_frame : frame_of_interest_ref)
    {
        //  Load data for current frame
        recodata_tree->GetEntry(i_frame.first);

        //  Container for selected hits
        std::vector<std::array<float, 2>> selected_points;

        //  Loop on hits
        for (auto current_hit = 0; current_hit < recodata->get_recodata().size(); current_hit++)
        {
            //  Remove afterpulse
            if (recodata->is_afterpulse(current_hit))
                continue;

            //  Ask for time coincidence
            auto time_delta_wrt_ref = recodata->get_hit_t(current_hit) - i_frame.second; //  ns
            if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
                continue;

            //  Ask the hits are within 3 \sigma of average found radius
            if (std::fabs(recodata->get_hit_r(current_hit, {(float)found_ring_center_x, (float)found_ring_center_y}) - found_ring_radius) > 3 * found_ring_radius_stddev)
                continue;

            //  Store selected points
            selected_points.push_back({(float)recodata->get_hit_x(current_hit), (float)recodata->get_hit_y(current_hit)});

            //  Plot the selection for QA
            //  *_rnd randomise the value within the sensor area, improves data visualisation
            //  Available for x, y, r, phi getters
            h_second_round_xy_map->Fill(recodata->get_hit_x_rnd(current_hit), recodata->get_hit_y_rnd(current_hit));
        }

        //  Work on second round of selected points

        //  Fitting the points
        auto fit_result = fit_circle(selected_points, {(float)found_ring_center_x, (float)found_ring_center_y, (float)found_ring_radius}, true, {{}});

        //  R vs Ngamma for resolution estimation
        h_second_round_R_Ngamma->Fill(fit_result[2][0], selected_points.size());

        //  Fitting the points, excluding one at a time
        for (auto i_ter = 0; i_ter < selected_points.size(); i_ter++)
        {
            fit_result = fit_circle(selected_points, {(float)found_ring_center_x, (float)found_ring_center_y, (float)found_ring_radius}, true, {i_ter});
            //  Temp fix
            auto radius = std::hypot(selected_points[i_ter][0] - found_ring_center_x, selected_points[i_ter][1] - found_ring_center_y);
            h_second_round_R_excluded->Fill(fit_result[2][0] - radius);
            h_second_round_R_global->Fill(found_ring_radius - radius);
        }
    }

    //  Loop on the 2D histogram to find the resolution vs p.e.
    //  Generate a TGraph to hold each Ngamma resolution
    TGraphErrors *g_resolution = new TGraphErrors();
    g_resolution->SetName("g_resolution");
    //  Hoisted out of the loop: `new TF1("fit_gaus", ...)` every iteration
    //  with the same name made ROOT silently destroy the previous one on
    //  construction; the explicit `delete` then freed the new allocation.
    //  Fragile pattern — one stack TF1 reused works the same and avoids
    //  the rename dance (CODE_REVIEW §6.9).
    TF1 fit_gaus("fit_gaus", "gaus", 0, 150);
    //  Loop over the y_bin, i.e. Ngamma
    for (auto y_bin = 1; y_bin <= h_second_round_R_Ngamma->GetNbinsY(); y_bin++)
    {
        auto n_gamma = h_second_round_R_Ngamma->GetYaxis()->GetBinCenter(y_bin);

        //  Slice to the resolution
        std::unique_ptr<TH1D> current_r_slice(
            h_second_round_R_Ngamma->ProjectionX(TString::Format("r_slice_%i", y_bin).Data(), y_bin, y_bin));
        current_r_slice->SetDirectory(nullptr);

        //  Select appropriate statistics
        if (current_r_slice->GetEntries() < 100)
            continue;

        //  Fit slice with the hoisted gaus function
        current_r_slice->Fit(&fit_gaus, "QNR");

        //  Discard if uncertainty is too high (unreliable fit)
        if (fit_gaus.GetParError(2) / fit_gaus.GetParameter(2) > 0.075)
            continue;

        //  Assign resolution value in the TGraph
        auto current_point = g_resolution->GetN();
        g_resolution->SetPoint(current_point, n_gamma, fit_gaus.GetParameter(2));
        g_resolution->SetPointError(current_point, 0., fit_gaus.GetParError(2));
        // unique_ptr frees current_r_slice at end of iteration; no manual delete.
    }

    //  Fit w/ resolution function
    TF1 *f_resolution = new TF1("f_resolution", "TMath::Sqrt([0] *[0] / x + [1] *[1])", 0, 100);
    f_resolution->SetParameters(2.5, 0.5);
    f_resolution->SetParName(0, "SPSR");
    f_resolution->SetParName(1, "constant");
    g_resolution->Fit(f_resolution);

    //  Show fit result on Canvas
    gStyle->SetOptFit(111111);

    TCanvas *c_time_delta = new TCanvas("c_time_delta", "Simple check on coincidences of timing and cherenkov sensors", 800, 800);
    gPad->SetLogy();
    h_t_distribution->SetLineColor(kBlack);
    h_t_distribution->SetLineWidth(2);
    h_t_distribution->Draw();

    TCanvas *c_first_round = new TCanvas("c_first_round", "First round fit on ring-like tagged points", 1200, 400);
    c_first_round->Divide(3, 1);
    c_first_round->cd(1);
    h_first_round_X->Draw();
    c_first_round->cd(2);
    h_first_round_Y->Draw();
    c_first_round->cd(3);
    h_first_round_R->Draw();

    TCanvas *c_second_round_map = new TCanvas("c_second_round_map", "Second round QA map", 800, 800);
    h_second_round_xy_map->Draw("COLZ");

    TCanvas *c_second_round_R_Ngamma = new TCanvas("c_second_round_R_Ngamma", "First round fit on ring-like tagged points", 800, 400);
    c_second_round_R_Ngamma->Divide(2, 1);
    c_second_round_R_Ngamma->cd(1);
    h_second_round_R_Ngamma->GetXaxis()->SetRangeUser(found_ring_radius - 3 * found_ring_radius_stddev, found_ring_radius + 3 * found_ring_radius_stddev);
    h_second_round_R_Ngamma->Draw();
    c_second_round_R_Ngamma->cd(2);
    g_resolution->Draw("ALP");

    TCanvas *c_second_round_R_exc_global = new TCanvas("c_second_round_R_exc_global", "First round fit on ring-like tagged points", 800, 400);
    c_second_round_R_exc_global->Divide(2, 1);
    c_second_round_R_exc_global->cd(1);
    h_second_round_R_excluded->Draw();
    c_second_round_R_exc_global->cd(2);
    h_second_round_R_global->Draw();
}