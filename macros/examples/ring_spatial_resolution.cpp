#pragma cling load("libtest_beam_analysis_dict.dylib");
#pragma cling load("libtest_beam_analysis.dylib");

std::array<float, 2> time_cut_boundaries = {-20., 20.};

void ring_spatial_resolution(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
  //  Input files
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

  //  Get number of frames, limited to maximum requested frames
  auto n_frames = recodata_tree->GetEntries();
  auto all_frames = min((int)n_frames, (int)max_frames);

  //  Time distribution
  TH1F *h_t_distribution = new TH1F("h_t_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
  //  First round X, Y, R
  TH1F *h_first_round_X = new TH1F("h_first_round_X", ";circle center x coordinate (mm)", 60, -15, 15);
  TH1F *h_first_round_Y = new TH1F("h_first_round_Y", ";circle center y coordinate (mm)", 60, -15, 15);
  TH1F *h_first_round_R = new TH1F("h_first_round_R", ";circle radius (mm)", 200, 30, 130);

  //  Saving the frame numebr you can speed up secondary loops
  std::vector<int> start_of_spill_frame_ref;
  std::vector<int> frame_of_interest_ref;

  //  Loop over frames
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current frame
    recodata_tree->GetEntry(i_frame);

    //  _HITMASK_dead_lane signals the event is start of spill, tells which channels are available
    if (decode_bits(recodata->get_hit_mask(0))[0] == _HITMASK_dead_lane)
    {
      //  You can save the frame number of spill start if you want
      start_of_spill_frame_ref.push_back(i_frame);
      //  This event is not of physical interest
      continue;
    }

    //  Select Luca AND trigger (0) or timing trigger (101)
    //  TODO: Make this a class method, w/ possibility to ask for multiple triggers at a time
    auto current_trigger = recodata->get_triggers();
    auto it = std::find_if(current_trigger.begin(), current_trigger.end(), [](const trigger_struct &t)
                           { return t.index == 101; });
    if (it != current_trigger.end())
    {
      //  Save trigger frames for later, ref to the actual number of used frames in the analysis
      frame_of_interest_ref.push_back(i_frame);

      //  Container for selected hits
      std::vector<std::array<float, 2>> selected_points;
      float avg_radius = 0.; // First estimate for radius

      //  Loop on hits
      for (auto current_hit = 0; current_hit < recodata->get().size(); current_hit++)
      {
        //  Fill time distribution to check
        auto time_delta_wrt_ref = recodata->get_hit_t(current_hit) - it->fine_time; //  ns
        h_t_distribution->Fill(time_delta_wrt_ref);

        //  Remove afterpulse
        //  Ref: afterpulse_treatment.cpp
        // if (recodata->is_afterpulse(current_hit))
        //  continue;

        //  Ask for time coincidence
        if ((time_delta_wrt_ref < time_cut_boundaries[0]) || (time_delta_wrt_ref > time_cut_boundaries[1]))
          continue;

        //  Check the hit has been labeled as ring-belonging
        //  This is done through a simple DBSCAN implementation
        //  Density-Based Spatial Clustering of Applications with Noise > https://it.wikipedia.org/wiki/DBSCAN
        //  Clustering is done in R and t, \phi is ignored (radial simmetry of cricle)
        if (recodata->is_ring_tagged(current_hit))
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
        auto fit_result = fit_circle(selected_points, {0., 0., avg_radius / selected_points.size()}, false);

        //  Save results for later QA
        h_first_round_X->Fill(fit_result[0][0]);
        h_first_round_Y->Fill(fit_result[1][0]);
        h_first_round_R->Fill(fit_result[2][0]);
      }
    }
  }

  TCanvas *c_time_delta = new TCanvas();
  gPad->SetLogy();
  h_t_distribution->SetLineColor(kBlack);
  h_t_distribution->SetLineWidth(2);
  h_t_distribution->Draw();

  TCanvas *c_first_round = new TCanvas("c_first_round", "", 1200, 400);
  c_first_round->Divide(3, 1);
  c_first_round->cd(1);
  h_first_round_X->Draw();
  c_first_round->cd(2);
  h_first_round_Y->Draw();
  c_first_round->cd(3);
  h_first_round_R->Draw();
}