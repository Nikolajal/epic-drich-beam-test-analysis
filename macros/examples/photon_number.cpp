#include "../lib_loader.h"

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

#define _RPHI_GRANULAR_BINS_ 10
std::array<float, 2> time_cut_boundaries = {-20., 30.};
std::vector<std::array<float, 2>> phi_cut_ranges = {{-2.6, -1.9}, {2.4, 2.95}, {-1.65, -1.1}, {-0.1, 0.15}};

TH1F *get_efficency_within_phi(TH2F *h_rphi_coverage_full, std::vector<std::array<float, 2>> phi_ranges, bool inclusive = true, int granularity = _RPHI_GRANULAR_BINS_)
{
  TAxis *yaxis = h_rphi_coverage_full->GetYaxis();
  auto result = new TH1F("h1", h_rphi_coverage_full->GetName(), yaxis->GetNbins() / granularity, yaxis->GetXmin(), yaxis->GetXmax());

  if (!inclusive)
  {
    std::vector<std::array<float, 2>> new_phi_ranges;
    float previous_phi = -TMath::Pi();
    for (auto i = 0; i < phi_ranges.size(); i++)
    {
      new_phi_ranges.push_back({previous_phi, phi_ranges[i][0]});
      previous_phi = phi_ranges[i][1];
    }
    new_phi_ranges.push_back({previous_phi, TMath::Pi()});
    phi_ranges = new_phi_ranges;
  }

  auto final_normalisation = 0.;

  auto n_bins_r = h_rphi_coverage_full->GetNbinsY();
  for (auto i_bin_r = 1; i_bin_r <= n_bins_r; i_bin_r += granularity)
  {
    auto efficiency = 0.;
    for (auto i_bin_phi = 1; i_bin_phi <= h_rphi_coverage_full->GetNbinsX(); i_bin_phi++)
    {
      auto current_phi = h_rphi_coverage_full->GetXaxis()->GetBinCenter(i_bin_phi);
      for (auto range : phi_ranges)
        if (current_phi > range[0] && current_phi < range[1])
        {
          for (auto i_bin_r_second = 0; i_bin_r_second < granularity; i_bin_r_second++)
            efficiency += h_rphi_coverage_full->GetBinContent(i_bin_phi, i_bin_r + i_bin_r_second);
          break;
        }
    }

    result->AddBinContent(i_bin_r / granularity, efficiency / (granularity));
  }

  result->Scale(1. / (h_rphi_coverage_full->GetNbinsX()));
  return result;
}

//  Add assert for within boundaries
void plot_line(TH1 *plot_on_this, TLine *plot_this_line, float value)
{
  double left = gPad->GetLeftMargin();
  double right = gPad->GetRightMargin();
  double top = gPad->GetTopMargin();
  double bottom = gPad->GetBottomMargin();
  auto plot_on_this_axis = plot_on_this->GetXaxis();
  auto maximum = plot_on_this_axis->GetXmax();
  auto minimum = plot_on_this_axis->GetXmin();
  plot_this_line->DrawLineNDC(left + (value - minimum) / (maximum - minimum) * (1 - right - left), bottom, left + (value - minimum) / (maximum - minimum) * (1 - right - left), 1 - top);
}

void plot_range(TH1 *plot_on_this, TLine *plot_this_line, std::vector<std::array<float, 2>> ranges)
{
  for (auto range : ranges)
  {
    plot_line(plot_on_this, plot_this_line, range[0]);
    plot_line(plot_on_this, plot_this_line, range[1]);
  }
}

/*
//  Plot a radial line over the 2D plot to show excluded zones
void plot_radial_line(TH1 *plot_on_this, TLine *plot_this_line, float phi_value, std::array<float, 2> center)
{
  //  Recover information
  double left = gPad->GetLeftMargin();
  double right = gPad->GetRightMargin();
  double top = gPad->GetTopMargin();
  double bottom = gPad->GetBottomMargin();
  auto plot_on_x_axis = plot_on_this->GetXaxis();
  auto plot_on_y_axis = plot_on_this->GetYaxis();
  auto x_maximum = plot_on_x_axis->GetXmax();
  auto x_minimum = plot_on_x_axis->GetXmin();
  auto y_maximum = plot_on_y_axis->GetXmax();
  auto y_minimum = plot_on_y_axis->GetXmin();

  //  Calculate the radial direction
  auto dx = cos(phi_value);
  auto dy = sin(phi_value);

  //  Propagate to histogram edge
  auto ()

  plot_this_line->DrawLineNDC(left + (value - minimum) / (maximum - minimum) * (1 - right - left), bottom, left + (value - minimum) / (maximum - minimum) * (1 - right - left), 1 - top);
}
  */

void photon_number(std::string data_repository, std::string run_name, int max_frames = 10000000)
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

  //  Analysis results and QA
  // --- Expected Coverage Map
  TH2F *h_xy_coverage_full = new TH2F("h_xy_coverage_full", ";x (mm);y (mm)", 396 * _RPHI_GRANULAR_BINS_, -99, 99, 396 * _RPHI_GRANULAR_BINS_, -99, 99);
  TH2F *h_rphi_coverage_full = new TH2F("h_rphi_coverage_full", ";#phi (rad); R (mm)", 400 * _RPHI_GRANULAR_BINS_, -TMath::Pi(), +TMath::Pi(), 50 * _RPHI_GRANULAR_BINS_, 25, 125);
  // --- Dataset Map
  TH2F *h_xy_hits_full = new TH2F("h_xy_hits_full", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_rphi_hits_full = new TH2F("h_rphi_hits_full", ";#phi (rad); R (mm)", 400, -TMath::Pi(), +TMath::Pi(), 75, 25, 125);
  TH1F *h_r_distribution_full = new TH1F("h_r_distribution_full", ";R (mm)", 50, 25, 125);
  TH1F *h_r_distribution_excluded = new TH1F("h_r_distribution_excluded", ";R (mm)", 50, 25, 125);
  TH1F *h_r_distribution_included = new TH1F("h_r_distribution_included", ";R (mm)", 50, 25, 125);
  //  Time distribution
  TH1F *h_t_distribution = new TH1F("h_t_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
  //  First round X, Y, R
  TH1F *h_first_round_X = new TH1F("h_first_round_X", ";circle center x coordinate (mm)", 240, -30, 30);
  TH1F *h_first_round_Y = new TH1F("h_first_round_Y", ";circle center y coordinate (mm)", 240, -30, 30);
  TH1F *h_first_round_R = new TH1F("h_first_round_R", ";circle radius (mm)", 400, 30, 130);

  //  Store useful cache
  //  --- Saving the frame number you can speed up secondary loops
  std::vector<int> start_of_spill_frame_ref;
  //  --- Saving the coverage bins for X-Y and R-\Phi
  std::map<int, std::array<std::vector<std::array<int, 2>>, 2>> index_to_bins_covered_xy_rphi;

  //  --- X-Y-R global fit results
  std::array<float, 3> global_XYR_result;

  //  Loop over frames
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current frame
    recodata_tree->GetEntry(i_frame);

    //  Takes note of spill evolution
    if (recodata->is_start_of_spill())
    {
      //  You can internally keep track of
      start_of_spill_frame_ref.push_back(i_frame);

      //  This event is not of physical interest, skip it
      continue;
    }

    //  Select Luca AND trigger (0) or timing trigger (101)
    auto default_hardware_trigger = recodata->get_trigger_by_index(0);
    if (default_hardware_trigger)
    {
      //  Save trigger frames for later, ref to the actual number of used frames in the analysis
      // frame_of_interest_ref.push_back({i_frame, default_hardware_trigger->fine_time});

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

        //  Check the hit has been labeled as ring-belonging
        //  This is done through a simple DBSCAN implementation
        //  Density-Based Spatial Clustering of Applications with Noise > https://it.wikipedia.org/wiki/DBSCAN
        //  Clustering is done in R and t, \phi is ignored (radial simmetry of cricle)
        //  Clustering is done in alcor_recodata::find_rings(...)
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

  TF1 *f_fit_first_round = new TF1("f_fit_first_round", "[0]*TMath::Gaus(x,[1],[2],true) + [3]*TMath::Gaus(x,[1],[4],true) ", -30, 130);
  f_fit_first_round->SetParameters(100, h_first_round_X->GetMean(), 1., 100, 5.);
  h_first_round_X->Fit(f_fit_first_round, "QNR");
  global_XYR_result[0] = f_fit_first_round->GetParameter(1);
  f_fit_first_round->SetParameters(100, h_first_round_Y->GetMean(), 1., 100, 5.);
  h_first_round_Y->Fit(f_fit_first_round, "QNR");
  global_XYR_result[1] = f_fit_first_round->GetParameter(1);
  f_fit_first_round->SetParameters(100, h_first_round_R->GetMean(), 1., 100, 5.);
  h_first_round_R->Fit(f_fit_first_round, "QNR");
  global_XYR_result[2] = f_fit_first_round->GetParameter(1);

  //  Second loop over frames
  auto used_frames = 0;
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current frame
    recodata_tree->GetEntry(i_frame);

    //  Takes note of spill evolution
    if (recodata->is_start_of_spill())
    {
      //  Loop on hits available for the spill
      for (auto current_hit = 0; current_hit < recodata->get_recodata().size(); current_hit++)
      {
        //  Get hit position
        std::array<float, 2> current_position = {recodata->get_hit_x(current_hit), recodata->get_hit_y(current_hit)};

        //  --- X-Y Coverage map ---

        auto index_coverage = index_to_bins_covered_xy_rphi.find(recodata->get_global_index(current_hit));
        if (index_coverage != index_to_bins_covered_xy_rphi.end())
        {
          for (auto bin : index_coverage->second[0])
            h_xy_coverage_full->AddBinContent(bin[0], bin[1], 1. / start_of_spill_frame_ref.size());
          for (auto bin : index_coverage->second[1])
            h_rphi_coverage_full->AddBinContent(bin[0], bin[1], 1. / start_of_spill_frame_ref.size());
          continue;
        }

        //  Locate position center bin
        auto bin_center_x_top = h_xy_coverage_full->GetXaxis()->FindBin(current_position[0] + 1.5);
        auto bin_center_x_low = h_xy_coverage_full->GetXaxis()->FindBin(current_position[0] - 1.5);
        auto bin_center_y_right = h_xy_coverage_full->GetYaxis()->FindBin(current_position[1] + 1.5);
        auto bin_center_y_left = h_xy_coverage_full->GetYaxis()->FindBin(current_position[1] - 1.5);
        for (auto x_bin = bin_center_x_low; x_bin <= bin_center_x_top; ++x_bin)
          for (auto y_bin = bin_center_y_left; y_bin <= bin_center_y_right; ++y_bin)
          {
            h_xy_coverage_full->SetBinContent(x_bin, y_bin, 1. / start_of_spill_frame_ref.size());
            index_to_bins_covered_xy_rphi[recodata->get_global_index(current_hit)][0].push_back({x_bin, y_bin});
          }

        //  --- R-\Phi Coverage map ---

        //  Locate position center bin
        auto current_center_r = std::hypot(current_position[0], current_position[1]);
        auto current_center_phi = std::atan2(current_position[1], current_position[0]);
        auto bin_center_r_top = h_rphi_coverage_full->GetYaxis()->FindBin(current_center_r + 1.5 * sqrt(2));
        auto bin_center_r_low = h_rphi_coverage_full->GetYaxis()->FindBin(current_center_r - 1.5 * sqrt(2));
        auto bin_center_phi_right = h_rphi_coverage_full->GetXaxis()->FindBin(current_center_phi + 1.5 * sqrt(2) / current_center_r);
        auto bin_center_phi_left = h_rphi_coverage_full->GetXaxis()->FindBin(current_center_phi - 1.5 * sqrt(2) / current_center_r);
        for (auto phi_bin = bin_center_phi_left; phi_bin <= bin_center_phi_right; ++phi_bin)
        {
          auto current_phi = h_rphi_coverage_full->GetXaxis()->GetBinCenter(phi_bin);
          for (auto r_bin = bin_center_r_low; r_bin <= bin_center_r_top; ++r_bin)
          {
            auto current_r = h_rphi_coverage_full->GetYaxis()->GetBinCenter(r_bin);
            auto current_x = current_r * cos(current_phi);
            auto current_y = current_r * sin(current_phi);

            if (std::fabs(current_x - current_position[0]) > 1.5 || std::fabs(current_y - current_position[1]) > 1.5)
              continue;

            auto &current_vector = index_to_bins_covered_xy_rphi[recodata->get_global_index(current_hit)][1];
            if (std::find(current_vector.begin(), current_vector.end(), std::array<int, 2>{phi_bin, r_bin}) != current_vector.end())
              continue;

            h_rphi_coverage_full->SetBinContent(phi_bin, r_bin, 1. / start_of_spill_frame_ref.size());
            current_vector.push_back({phi_bin, r_bin});
          }
        }
      }
      continue;
    }

    //  Select Luca AND trigger (0) or timing trigger (101)
    auto default_hardware_trigger = recodata->get_trigger_by_index(0);
    if (default_hardware_trigger)
    {
      //  Used frames counter
      used_frames++;

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

        bool is_within_range = false;
        auto current_phi = recodata->get_hit_phi(current_hit, {global_XYR_result[0], global_XYR_result[1]});
        for (auto range : phi_cut_ranges)
          if (current_phi > range[0] && current_phi < range[1])
          {
            is_within_range = true;
            break;
          }

        if (is_within_range)
        {
          //  Included
          h_r_distribution_included->Fill(recodata->get_hit_r(current_hit, {global_XYR_result[0], global_XYR_result[1]}));

          //  Full
          h_xy_hits_full->Fill(recodata->get_hit_x_rnd(current_hit), recodata->get_hit_y_rnd(current_hit));
          h_rphi_hits_full->Fill(recodata->get_hit_phi_rnd(current_hit, {global_XYR_result[0], global_XYR_result[1]}), recodata->get_hit_r_rnd(current_hit, {global_XYR_result[0], global_XYR_result[1]}));
          h_r_distribution_full->Fill(recodata->get_hit_r(current_hit, {global_XYR_result[0], global_XYR_result[1]}));
        }
        else
        {
          //  Excluded
          h_r_distribution_excluded->Fill(recodata->get_hit_r(current_hit, {global_XYR_result[0], global_XYR_result[1]}));

          //  Full
          h_xy_hits_full->Fill(recodata->get_hit_x_rnd(current_hit), recodata->get_hit_y_rnd(current_hit));
          h_rphi_hits_full->Fill(recodata->get_hit_phi_rnd(current_hit, {global_XYR_result[0], global_XYR_result[1]}), recodata->get_hit_r_rnd(current_hit, {global_XYR_result[0], global_XYR_result[1]}));
          h_r_distribution_full->Fill(recodata->get_hit_r(current_hit, {global_XYR_result[0], global_XYR_result[1]}));
        }
      }
    }
  }

  gStyle->SetOptStat(0);
  gStyle->SetOptFit(0);

  TCanvas *c_XYRt_finding = new TCanvas("c_XYRt_finding", "Preliminary Y, Y, R and t distributions", 1200, 1200);
  //  Define the line to signal the center chosen
  TLine *line_center = new TLine();
  line_center->SetLineWidth(3);
  line_center->SetLineColor(kRed);
  line_center->SetLineStyle(kDashed);
  c_XYRt_finding->Divide(2, 2);
  c_XYRt_finding->cd(1);
  h_first_round_X->Draw();
  line_center->DrawLineNDC(0.1 + (+30 + global_XYR_result[0]) / (60.) * 0.8, 0.1, 0.1 + (+30 + global_XYR_result[0]) / (60.) * 0.8, 0.9);
  c_XYRt_finding->cd(2);
  h_first_round_Y->Draw();
  line_center->DrawLineNDC(0.1 + (+30 + global_XYR_result[1]) / (60.) * 0.8, 0.1, 0.1 + (+30 + global_XYR_result[1]) / (60.) * 0.8, 0.9);
  c_XYRt_finding->cd(3);
  h_first_round_R->Draw();
  line_center->DrawLineNDC(0.1 + (-30 + global_XYR_result[2]) / (100.) * 0.8, 0.1, 0.1 + (-30 + global_XYR_result[2]) / (100.) * 0.8, 0.9);
  c_XYRt_finding->cd(4);
  h_t_distribution->Scale(1. / used_frames);
  h_t_distribution->Draw();

  TCanvas *c_coverage = new TCanvas("c_coverage", "Expected coverage map", 1200, 600);
  c_coverage->Divide(2, 1);
  c_coverage->cd(1);
  h_xy_coverage_full->Draw("COLZ");
  c_coverage->cd(2);
  h_rphi_coverage_full->Draw("COLZ");

  TCanvas *c_data_show = new TCanvas("c_data", "Expected coverage map", 1200, 600);
  c_data_show->Divide(2, 1);
  c_data_show->cd(1);
  h_xy_hits_full->Scale(1. / used_frames);
  h_xy_hits_full->Draw("COLZ");
  auto fit_results = fit_ring_integral(h_xy_hits_full,
                                       {global_XYR_result[0],
                                        global_XYR_result[1],
                                        global_XYR_result[2],
                                        2.15,
                                        25.0,
                                        0.0003},
                                       true);
  auto sigma_plots = plot_ring_integral(fit_results, {0, -3, 3}, {{2, 2, 0.5, 0.01}});
  std::vector<std::array<int, 3>> plot_nice = {{kBlack, 2, kSolid},
                                               {kRed, 2, kDashed},
                                               {kRed, 2, kDashed}};
  auto iTer = -1;
  for (auto current_plot : sigma_plots)
  {
    iTer++;
    current_plot->SetLineColor(plot_nice[iTer][0]);
    current_plot->SetLineWidth(plot_nice[iTer][1]);
    current_plot->SetLineStyle(plot_nice[iTer][2]);
    current_plot->Draw("SAME PL");
  }
  c_data_show->cd(2);
  h_rphi_hits_full->Draw("COLZ");
  plot_range(h_rphi_hits_full, line_center, phi_cut_ranges);

  TCanvas *c_radial_distribution = new TCanvas("c_radial_distribution", "Radial distribution of hits", 1800, 600);
  c_radial_distribution->Divide(3, 1);
  c_radial_distribution->cd(1);
  h_r_distribution_full->Scale(1. / used_frames);
  h_r_distribution_full->Divide(get_efficency_within_phi(h_rphi_coverage_full, {{-TMath::Pi(), TMath::Pi()}}));
  h_r_distribution_full->Draw("COLZ");
  c_radial_distribution->cd(2);
  h_r_distribution_included->Scale(1. / used_frames);
  h_r_distribution_included->Divide(get_efficency_within_phi(h_rphi_coverage_full, phi_cut_ranges));
  h_r_distribution_included->Draw("COLZ");
  c_radial_distribution->cd(3);
  h_r_distribution_excluded->Scale(1. / used_frames);
  h_r_distribution_excluded->Divide(get_efficency_within_phi(h_rphi_coverage_full, phi_cut_ranges, false));
  h_r_distribution_excluded->Draw("COLZ");

  TCanvas *c_radial_efficiencies = new TCanvas("c_radial_efficiencies", "Radial efficiencies", 1800, 600);
  c_radial_efficiencies->Divide(3, 1);
  c_radial_efficiencies->cd(1);
  get_efficency_within_phi(h_rphi_coverage_full, {{-TMath::Pi(), TMath::Pi()}})->Draw("COLZ");
  c_radial_efficiencies->cd(2);
  get_efficency_within_phi(h_rphi_coverage_full, phi_cut_ranges)->Draw("COLZ");
  c_radial_efficiencies->cd(3);
  get_efficency_within_phi(h_rphi_coverage_full, phi_cut_ranges, false)->Draw("COLZ");

  //  Test Sigma
  new TCanvas();
  TGraph *test = new TGraph();
  for (auto i = 0; i < 1000; i++)
  {
    auto current_phi = 2 * TMath::Pi() * (i / 1000.);
    test->SetPoint(i, current_phi, ring_fit_function_sigma_function(current_phi, 2.15, {{0.5, 2, 0.5, 0.01}}));
  }
  test->Draw("ALP");
}