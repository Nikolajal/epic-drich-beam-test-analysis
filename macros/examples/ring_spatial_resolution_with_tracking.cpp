#include "../lib_loader.h"

/**
 * @file ring_spatial_resolution.cpp
 * @brief Calculate the spatial resolution of the ring.
 *
 * This exercise estimates the center and radius of a ring of hits and then
 * computes the spatial resolution using multiple methods.
 * 
 * Additonally, this version of the macro exploits the tracking capabilities of the recotrackdata, to check the correlation between tracking angle and ring reconstruction quality.
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

//  --- --- --- !!!
//  This excercise is still a work in progress, stay tuned for updates!
//  --- --- --- !!!

std::array<float, 2> time_cut_boundaries = {-45., 20.};

void ring_spatial_resolution_with_tracking(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
  //  Input files
  std::string input_filename_recotrackdata = data_repository + "/" + run_name + "/recotrackdata.root";

  //  Load recotrackdata, return if not available
  TFile *input_file_recotrackdata = new TFile(input_filename_recotrackdata.c_str());
  if (!input_file_recotrackdata || input_file_recotrackdata->IsZombie())
  {
    std::cerr << "[WARNING] Could not find recotrackdata, making it" << std::endl;
    return;
  }

  //  Link recotrackdata tree locally
  TTree *recotrackdata_tree = (TTree *)input_file_recotrackdata->Get("recotrackdata");
  alcor_recotrackdata *recotrackdata = new alcor_recotrackdata();
  recotrackdata->link_to_tree(recotrackdata_tree);

  //  Get number of frames, limited to maximum requested frames
  auto n_frames = recotrackdata_tree->GetEntries();
  auto all_frames = min((int)n_frames, (int)max_frames);

  //  Time distribution
  TH1F *h_t_distribution = new TH1F("h_t_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
  //  First round X, Y, R
  TH1F *h_first_round_X = new TH1F("h_first_round_X", ";circle center x coordinate (mm)", 120, -30, 30);
  TH1F *h_first_round_Y = new TH1F("h_first_round_Y", ";circle center y coordinate (mm)", 120, -30, 30);
  TH1F *h_first_round_R = new TH1F("h_first_round_R", ";circle radius (mm)", 200, 30, 130);
  TH1F *h_tracking_chi2 = new TH1F("h_tracking_chi2", ";tracking chi2", 1000, 0, 1);
  //  Second round selection
  TH2F *h_second_round_xy_map = new TH2F("h_second_round_xy_map", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_second_round_R_Ngamma = new TH2F("h_second_round_R_Ngamma", ";circle radius (mm);N_{#gamma}", 200, 30, 130, 97, 3, 100);
  TH1F *h_second_round_R_excluded = new TH1F("h_second_round_R_excluded", ";circle radius - point radius (mm)", 120, -30, 30);
  TH1F *h_second_round_R_global = new TH1F("h_second_round_R_global", ";circle radius - point radius (mm)", 120, -30, 30);

  //  Saving the frame number you can speed up secondary loops
  std::vector<int> start_of_spill_frame_ref;
  std::vector<std::pair<int, float>> frame_of_interest_ref;

  //  Loop over frames
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current frame
    recotrackdata_tree->GetEntry(i_frame);

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
      //  Checking
      h_tracking_chi2->Fill(recotrackdata->get_traj_angcoeff(0));
      
      //  Check the tracking of the event is within a certain range
      if (recotrackdata->get_traj_angcoeff(0) > 0.001)
        continue;

      //  Save trigger frames for later, ref to the actual number of used frames in the analysis
      frame_of_interest_ref.push_back({i_frame, default_hardware_trigger->fine_time});

      //  Container for selected hits
      std::vector<std::array<float, 2>> selected_points;
      float avg_radius = 0.; // First estimate for radius

      //  Loop on hits
      for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
      {
        //  Remove afterpulse
        //  Ref: afterpulse_treatment.cpp
        if (recotrackdata->is_afterpulse(current_hit))
          continue;

        //  Fill time distribution to check
        auto time_delta_wrt_ref = recotrackdata->get_hit_t(current_hit) - default_hardware_trigger->fine_time; //  ns
        h_t_distribution->Fill(time_delta_wrt_ref);

        //  Ask for time coincidence
        if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
          continue;

        //  Check the hit has been labeled as ring-belonging
        //  This is done through a simple DBSCAN implementation
        //  Density-Based Spatial Clustering of Applications with Noise > https://it.wikipedia.org/wiki/DBSCAN
        //  Clustering is done in R and t, \phi is ignored (radial simmetry of cricle)
        //  Clustering is done in alcor_recotrackdata::find_rings(...)
        //  TODO: add a flag for sensor type
        if (recotrackdata->is_ring_tagged(current_hit))
          continue;

        //  Store selected points
        selected_points.push_back({recotrackdata->get_hit_x(current_hit), recotrackdata->get_hit_y(current_hit)});
        avg_radius += recotrackdata->get_hit_r(current_hit);
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
    recotrackdata_tree->GetEntry(i_frame.first);

    //  Container for selected hits
    std::vector<std::array<float, 2>> selected_points;

    //  Loop on hits
    for (auto current_hit = 0; current_hit < recotrackdata->get_recodata().size(); current_hit++)
    {
      //  Remove afterpulse
      if (recotrackdata->is_afterpulse(current_hit))
        continue;

      //  Ask for time coincidence
      auto time_delta_wrt_ref = recotrackdata->get_hit_t(current_hit) - i_frame.second; //  ns
      if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
        continue;

      //  Ask the hits are within 3 \sigma of average found radius
      if (std::fabs(recotrackdata->get_hit_r(current_hit, {(float)found_ring_center_x, (float)found_ring_center_y}) - found_ring_radius) > 3 * found_ring_radius_stddev)
        continue;

      //  Store selected points
      selected_points.push_back({(float)recotrackdata->get_hit_x(current_hit), (float)recotrackdata->get_hit_y(current_hit)});

      //  Plot the selection for QA
      //  *_rnd randomise the value within the sensor area, improves data visualisation
      //  Available for x, y, r, phi getters
      h_second_round_xy_map->Fill(recotrackdata->get_hit_x_rnd(current_hit), recotrackdata->get_hit_y_rnd(current_hit));
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
  //  Loop over the y_bin, i.e. Ngamma
  for (auto y_bin = 1; y_bin <= h_second_round_R_Ngamma->GetNbinsY(); y_bin++)
  {
    auto n_gamma = h_second_round_R_Ngamma->GetYaxis()->GetBinCenter(y_bin);

    //  Slice to the resolution
    auto current_r_slice = h_second_round_R_Ngamma->ProjectionX(Form("r_slice_%i", y_bin), y_bin, y_bin);

    //  Select appropriate statistics
    if (current_r_slice->GetEntries() < 100)
      continue;

    //  Fit slice with a gaus function
    auto fit_gaus = new TF1("fit_gaus", "gaus", 0, 150);
    current_r_slice->Fit(fit_gaus, "QNR");

    //  Discard if uncertainty is too high (unreliable fit)
    if (fit_gaus->GetParError(2) / fit_gaus->GetParameter(2) > 0.075)
      continue;

    //  Assign resolution value in the TGraph
    auto current_point = g_resolution->GetN();
    g_resolution->SetPoint(current_point, n_gamma, fit_gaus->GetParameter(2));
    g_resolution->SetPointError(current_point, 0., fit_gaus->GetParError(2));

    //  Memory clean-up
    delete current_r_slice;
    delete fit_gaus;
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

  TCanvas *c_tracking_chi2 = new TCanvas("c_tracking_chi2", "Tracking chi2 distribution", 800, 800);
  h_tracking_chi2->Draw();
}