#include "../lib/alcor_recodata.h"
#include "../lib/utility.h"
#include "recodata_writer.C"

gSystem->Load("./alcor_recodata_h.so");

bool is_phi_excluded(float phi, const std::vector<std::array<float, 2>> &phi_exclusion)
{
  for (const auto &r : phi_exclusion)
    if (phi >= r[0] && phi <= r[1])
      return true;
  return false;
}

void analysis_example(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
  //  Quick fix
  std::array<float, 2> circle_center = {1.4, 1.4};
  std::vector<std::array<float, 2>> phi_exclusion = {{}}; //{{{-2.6, -2.0}, {2.5, 2.85}}}; // AEROGEL FIX

  //  Input files
  std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";

  //  Load recodata, build if not existing
  TFile *input_file_recodata = new TFile(input_filename_recodata.c_str());
  if (!input_file_recodata || input_file_recodata->IsZombie())
  {
    std::cerr << "[WARNING] Could not find recodata, making it" << std::endl;
    recodata_writer(data_repository, run_name, max_frames);
    input_file_recodata = new TFile(input_filename_recodata.c_str());
    if (!input_file_recodata || input_file_recodata->IsZombie())
    {
      std::cerr << "[ERROR] Could not open recodata even after making it, exiting" << std::endl;
      return;
    }
  }

  //  Link recodata tree locally
  TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
  alcor_recodata *recodata = new alcor_recodata();
  recodata->link_to_tree(recodata_tree);

  //  Get number of frames, limited to maximum requested frames
  auto n_frames = recodata_tree->GetEntries();
  auto all_frames = min((int)n_frames, (int)max_frames);

  //  Prepare output file
  TFile *output_file = TFile::Open((data_repository + "/" + run_name + "/analysis_example.root").c_str(), "RECREATE");

  //  Analysis results and QA
  // --- Expected Coverage Map
  TH2F *h_xy_coverage_full = new TH2F("h_xy_coverage_full", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_rphi_coverage_full = new TH2F("h_rphi_coverage_full", ";#phi (rad); R (mm)", 400, -TMath::Pi(), +TMath::Pi(), 75, 25, 125);
  TH2F *h_xy_coverage_actual = new TH2F("h_xy_coverage_actual", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_rphi_coverage_actual = new TH2F("h_rphi_coverage_actual", ";#phi (rad); R (mm)", 400, -TMath::Pi(), +TMath::Pi(), 75, 25, 125);
  TH1F *h_r_coverage_actual_efficiency = new TH1F("h_r_coverage_actual_efficiency", "; R (mm)", 75, 25, 125);
  //  --- Photons measurement
  TH2F *h_xy_distribution = new TH2F("h_xy_distribution", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_rphi_distribution = new TH2F("h_rphi_distribution", ";#phi (rad); R (mm)", 400, -TMath::Pi(), +TMath::Pi(), 75, 25, 125);
  TH1F *h_r_distribution = new TH1F("h_r_distribution", ";R (mm)", 75, 25, 125);
  TH1F *h_t_distribution = new TH1F("h_t_distribution", ";t_{hit} - t_{timing} (ns)", 200, -100, 100);
  //  --- --- Legacy
  TH2F *h_full_xy_coverage = new TH2F("h_full_xy_coverage", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_rphi_coverage = new TH2F("h_full_rphi_coverage", ";y (mm)", 200, -TMath::Pi(), +TMath::Pi(), 200, 30, 130);
  //  --- Other
  TH2F *h_full_hitmap_tag = new TH2F("h_full_hitmap_tag", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_reselect = new TH2F("h_full_hitmap_reselect", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_trg0 = new TH2F("h_full_hitmap_trg0", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_trg0_2nd = new TH2F("h_full_hitmap_trg0_2nd", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_trg0_3rd = new TH2F("h_full_hitmap_trg0_3rd", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_trg0_4th = new TH2F("h_full_hitmap_trg0_4th", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_tag_2nd = new TH2F("h_full_hitmap_tag_2nd", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH1F *h_fit_x = new TH1F("h_fit_x", ";x (mm)", 200, -10, 10);
  TH1F *h_fit_y = new TH1F("h_fit_y", ";y (mm)", 200, -10, 10);
  TH1F *h_fit_r = new TH1F("h_fit_r", ";R (mm)", 3000, 0, 150);
  TH2F *h_fit_r_n_gamma = new TH2F("h_fit_r_n_gamma", ";R (mm);N_{#gamma}", 1200, 0, 150, 50, -0.5, 99.5);
  TH1F *h_avg_r = new TH1F("h_avg_r", ";y (mm)", 200, -10, 10);
  TH1F *h_avg_t = new TH1F("h_avg_t", ";y (mm)", 200, -10, 10);
  TH2F *h_sel_rphi = new TH2F("h_sel_rphi", ";y (mm)", 200, -TMath::Pi(), +TMath::Pi(), 200, 30, 130);
  TH2F *h_full_hitmap_test_filter = new TH2F("h_full_hitmap_test_filter", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH1F *h_test_nphotons = new TH1F("h_test_nphotons", ";N_{#gamma}", 200, 0, 200);

  //  Coverage estimation
  std::map<int, std::array<float, 2>> index_to_hit_xy;
  std::map<std::array<float, 2>, int> hit_to_index_xy;
  for (auto i_index = 0; i_index < 2048 * 4; i_index += 4)
  {
    auto position = sipm4eic::get_position(i_index);
    if (fabs(position[0]) < 5 && fabs(position[1]) < 5)
      continue;
    index_to_hit_xy[i_index] = position;
    hit_to_index_xy[position] = i_index;
  }
  std::map<int, std::vector<std::array<int, 2>>> index_to_bins_covered_xy;
  std::map<int, std::vector<std::array<int, 2>>> index_to_bins_covered_rphi;
  std::map<std::array<int, 2>, bool> bin_assigned_xy;
  std::map<std::array<int, 2>, bool> bin_assigned_rphi;
  for (auto [position, index] : hit_to_index_xy)
  {
    //  --- X-Y Coverage map ---

    //  Locate position center bin
    auto x_bin_starting_position = h_xy_coverage_full->GetXaxis()->FindBin(position[0] - 1.5);
    auto y_bin_starting_position = h_xy_coverage_full->GetYaxis()->FindBin(position[1] - 1.5);

    //  Cycle to set all covered bins to 1
    // auto break_cycle = false;
    auto x_bin_iterator = x_bin_starting_position - 1;
    auto y_bin_iterator = y_bin_starting_position - 1;
    while (true)
    {
      x_bin_iterator++;
      y_bin_iterator = y_bin_starting_position - 1;
      if (h_xy_coverage_full->GetXaxis()->GetBinCenter(x_bin_iterator) >= position[0] + 1.5)
        break;
      while (true)
      {
        y_bin_iterator++;
        if (bin_assigned_xy[{{x_bin_iterator, y_bin_iterator}}])
          continue;
        bin_assigned_xy[{{x_bin_iterator, y_bin_iterator}}] = true;
        h_xy_coverage_full->SetBinContent(x_bin_iterator, y_bin_iterator, 1);
        index_to_bins_covered_xy[index].push_back({x_bin_iterator, y_bin_iterator});
        if (h_xy_coverage_full->GetYaxis()->GetBinCenter(y_bin_iterator) >= position[1] + 1.5)
          break;
      }
    }

    //  --- R-Phi Coverage map ---
    for (auto iter = 0; iter < 1000; iter++)
    {
      auto hit_x_rnd = position[0] - circle_center[0] + (_rnd_(_global_gen_) * 3.0 - 1.5);
      auto hit_y_rnd = position[1] - circle_center[1] + (_rnd_(_global_gen_) * 3.0 - 1.5);

      auto current_radius = std::hypot(hit_x_rnd, hit_y_rnd);
      auto current_phi = std::atan2(hit_y_rnd, hit_x_rnd);
      if (is_phi_excluded(current_phi, phi_exclusion))
        continue;
      auto x_bin_iterator = h_rphi_coverage_full->GetXaxis()->FindBin(current_phi);
      auto y_bin_iterator = h_rphi_coverage_full->GetYaxis()->FindBin(current_radius);
      if (bin_assigned_rphi[{{x_bin_iterator, y_bin_iterator}}])
        continue;
      bin_assigned_rphi[{{x_bin_iterator, y_bin_iterator}}] = true;
      h_rphi_coverage_full->SetBinContent(x_bin_iterator, y_bin_iterator, 1);
      index_to_bins_covered_rphi[index].push_back({x_bin_iterator, y_bin_iterator});
    }
  }

  //  Loop over frames
  auto i_spill = -1;
  auto n_spils = 0;
  auto used_frames = 0;
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current frame
    recodata_tree->GetEntry(i_frame);

    //  Loop over hit for efficiency x acceptance
    if (decode_bits(recodata->get_hit_mask(0))[0] == _HITMASK_dead_lane)
    {
      i_spill++;
      n_spils++;
      for (auto current_hit = 0; current_hit < recodata->get().size(); current_hit++)
      {
        //  Run through available channels
        auto current_hit_channel = 4 * recodata->get_channel(current_hit);

        //  Build actual coverage maps
        for (auto bin : index_to_bins_covered_xy[current_hit_channel])
          h_xy_coverage_actual->AddBinContent(bin[0], bin[1]);
        for (auto bin : index_to_bins_covered_rphi[current_hit_channel])
          h_rphi_coverage_actual->AddBinContent(bin[0], bin[1]);
      }
    }

    //  Select timing trigger
    auto current_trigger = recodata->get_triggers();
    auto it = std::find_if(current_trigger.begin(), current_trigger.end(), [](const trigger_struct &t)
                           { return t.index == 0; });
    if (it != current_trigger.end())
    {
      used_frames++;
      //  Loop on hits
      for (auto current_hit = 0; current_hit < recodata->get().size(); current_hit++)
      {
        h_t_distribution->Fill(recodata->get_hit_t(current_hit) - it->coarse * _ALCOR_CC_TO_NS_);
        if (fabs(recodata->get_hit_t(current_hit) - it->coarse * _ALCOR_CC_TO_NS_) > 40)
          continue;
        if (is_phi_excluded(recodata->get_hit_phi(current_hit, circle_center), phi_exclusion))
          continue;
        h_xy_distribution->Fill(recodata->get_hit_x_rnd(current_hit), recodata->get_hit_y_rnd(current_hit));
        h_rphi_distribution->Fill(recodata->get_hit_phi_rnd(current_hit, circle_center), recodata->get_hit_r_rnd(current_hit, circle_center));
        h_r_distribution->Fill(recodata->get_hit_r(current_hit, circle_center));
      }
    }
  }

  //  Calcuate the R efficiency
  h_xy_coverage_actual->Scale(1. / n_spils);
  h_rphi_coverage_actual->Scale(1. / n_spils);
  h_r_distribution->Scale(1. / used_frames);
  for (auto y_bin = 1; y_bin <= h_rphi_coverage_actual->GetYaxis()->GetNbins(); y_bin++)
  {
    auto current_radius = 0.;
    auto current_radius_error = 0.;
    for (auto x_bin = 1; x_bin <= h_rphi_coverage_actual->GetXaxis()->GetNbins(); x_bin++)
    {
      current_radius += h_rphi_coverage_actual->GetBinContent(x_bin, y_bin);
      current_radius_error += h_rphi_coverage_actual->GetBinError(x_bin, y_bin) * h_rphi_coverage_actual->GetBinError(x_bin, y_bin);
    }
    h_r_coverage_actual_efficiency->SetBinContent(y_bin, current_radius / h_rphi_coverage_actual->GetXaxis()->GetNbins());
    h_r_coverage_actual_efficiency->SetBinError(y_bin, sqrt(current_radius_error) / h_rphi_coverage_actual->GetXaxis()->GetNbins());
  }
  h_r_distribution->Divide(h_r_coverage_actual_efficiency);

  TF1 *f_fit_Rpeak = new TF1("f_fit_Rpeak", "[0]*TMath::Gaus(x,[1],[2],true)+[3]+x*[4]+x*x*[5]");
  f_fit_Rpeak->SetParameters(20, 45, 2, 0.5, 0.1, 0., 5, 75, 5);
  //f_fit_Rpeak->SetParLimits(0, 10, 35);
  //f_fit_Rpeak->SetParLimits(1, 60, 90);
  //f_fit_Rpeak->SetParLimits(2, 1, 4);
  h_r_distribution->Fit(f_fit_Rpeak);

  //  Bin counting
  auto center = f_fit_Rpeak->GetParameter(1);
  auto std_dev = f_fit_Rpeak->GetParameter(2);
  auto signal = 0.;
  auto signal_error = 0.;
  auto background = 0.;
  auto background_error = 0.;
  for (auto x_bin = 0; x_bin <= h_r_distribution->GetXaxis()->GetNbins(); x_bin++)
  {
    auto bin_center = h_r_distribution->GetBinCenter(x_bin);
    if (fabs(bin_center - center) < 5 * std_dev)
    {
      signal += h_r_distribution->GetBinContent(x_bin);
      signal_error += h_r_distribution->GetBinError(x_bin) * h_r_distribution->GetBinError(x_bin);
    }
    else if (fabs(bin_center - center) < 10 * std_dev)
    {
      background += h_r_distribution->GetBinContent(x_bin);
      background_error += h_r_distribution->GetBinError(x_bin) * h_r_distribution->GetBinError(x_bin);
    }
    else
      continue;
  }
  std::cout << "signal: " << signal << std::endl;
  std::cout << "background: " << background << std::endl;
  std::cout << "signal - background: " << signal - background << std::endl;

  //  Plotting results
  new TCanvas();
  h_xy_coverage_full->Draw("colz");
  new TCanvas();
  h_rphi_coverage_full->Draw("colz");
  new TCanvas();
  h_xy_coverage_actual->Draw("colz");
  new TCanvas();
  h_rphi_coverage_actual->Draw("colz");
  new TCanvas();
  h_r_coverage_actual_efficiency->Draw("");
  new TCanvas();
  h_r_distribution->DrawCopy("HIST");
  new TCanvas();
  h_r_distribution->Draw("HIST");
  new TCanvas();
  h_rphi_distribution->Draw("colz");
  new TCanvas();
  h_xy_distribution->Draw("colz");
  new TCanvas();
  h_t_distribution->Draw("HIST");

  /*

  // std::map<int, std::vector<std::array<float, 2>>> hit_xy_available;

    //  Load data for current spill
    recodata_tree->GetEntry(i_frame);

    //  Loop over hit
    std::map<int, std::vector<float>> avg_t;
    std::vector<float> avg_r;

    //  Trg0 hits
    bool trigger_0 = false;
    auto trigger_0_time = 0.;
    for (auto trigger : recodata->get_triggers())
      if ((int)trigger.index == 101)
      {
        trigger_0 = true;
        trigger_0_time = trigger.fine_time;
      }

    h_test_nphotons->Fill(recodata->get().size());

    for (auto current_hit = 0; current_hit < recodata->get().size(); current_hit++)
    {
      //  Graphic rnd position
      auto hit_x_rnd = recodata->get_hit_x(current_hit) + (_rnd_(_global_gen_) * 3.0 - 1.5);
      auto hit_y_rnd = recodata->get_hit_y(current_hit) + (_rnd_(_global_gen_) * 3.0 - 1.5);

      if (trigger_0 && (fabs(recodata->get_hit_t(current_hit) - trigger_0_time * _ALCOR_CC_TO_NS_) < 20))
      {
        //  QA plots
        h_full_hitmap_trg0->Fill(hit_x_rnd, hit_y_rnd);
      }
      else if (trigger_0 && (fabs(recodata->get_hit_t(current_hit) - trigger_0_time * _ALCOR_CC_TO_NS_ - 45) < 25))
      {
        //  QA plots
        //  Con I dati dell’anno scorso possiamo confrontarci con Simone
        h_full_hitmap_trg0_2nd->Fill(hit_x_rnd, hit_y_rnd);
      }
      else if (trigger_0 && (fabs(recodata->get_hit_t(current_hit) - trigger_0_time * _ALCOR_CC_TO_NS_ - 95) < 25))
      {
        h_full_hitmap_trg0_3rd->Fill(hit_x_rnd, hit_y_rnd);
      }
      else if (trigger_0 && (fabs(recodata->get_hit_t(current_hit) - trigger_0_time * _ALCOR_CC_TO_NS_ - 145) < 25))
      {
        h_full_hitmap_trg0_4th->Fill(hit_x_rnd, hit_y_rnd);
      }

      //  Only hit signalled to be ring-like
      if (decode_bits(recodata->get_hit_mask(current_hit))[0] == 1)
      {
        //  Store for avg
        avg_t[recodata->get_device(current_hit)].push_back(recodata->get_hit_t(current_hit));
        avg_r.push_back(recodata->get_hit_r(current_hit));

        //  QA plots
        h_full_hitmap_tag->Fill(hit_x_rnd, hit_y_rnd);
        h_avg_r->Fill(recodata->get_hit_r(current_hit));
        h_avg_t->Fill(recodata->get_hit_t(current_hit));
      }
    }

    if (avg_r.size() < 1)
      continue;
    auto avg_r_value = std::accumulate(avg_r.begin(), avg_r.end(), 0.0) / avg_r.size();
    std::map<int, float> avg_t_value;
    for (auto [device, times] : avg_t)
      if (times.size() > 0)
        avg_t_value[device] = std::accumulate(times.begin(), times.end(), 0.0) / times.size();

    //  Re-loop
    std::vector<std::array<float, 2>> points;
    for (auto current_hit = 0; current_hit < recodata->get().size(); current_hit++)
    {
      //  Time complying
      if (fabs(recodata->get_hit_t(current_hit) - avg_t_value[recodata->get_device(current_hit)]) > 2)
        continue;
      //  Radius complying
      if (fabs(recodata->get_hit_r(current_hit) - avg_r_value) > 4.5)
        continue;

      //  Store points to fit
      points.push_back({recodata->get_hit_x(current_hit), recodata->get_hit_y(current_hit)});

      //  Graphic rnd position
      auto hit_x_rnd = recodata->get_hit_x(current_hit) + (_rnd_(_global_gen_) * 3.0 - 1.5);
      auto hit_y_rnd = recodata->get_hit_y(current_hit) + (_rnd_(_global_gen_) * 3.0 - 1.5);
      auto hit_r_rand = sqrt((hit_x_rnd - circle_center[0]) * (hit_x_rnd - circle_center[0]) + (hit_y_rnd - circle_center[1]) * (hit_y_rnd - circle_center[1]));
      auto hit_phi_rand = atan2((hit_y_rnd - circle_center[1]), (hit_x_rnd - circle_center[0]));

      //  QA plots
      h_full_hitmap_tag_2nd->Fill(hit_x_rnd, hit_y_rnd);
      h_sel_rphi->Fill(hit_phi_rand, hit_r_rand);
    }

    //  Fit ring-like points
    if (points.size() < 5)
      continue;
    auto fit_results = fit_circle(points, {circle_center[0], circle_center[1], 74.40}, true); // sigma 1.86154
    h_fit_x->Fill(fit_results[0][0]);
    h_fit_y->Fill(fit_results[1][0]);
    h_fit_r->Fill(fit_results[2][0]);
    h_fit_r_n_gamma->Fill(fit_results[2][0], points.size());
  }

  //  Let's see if this works
  h_sel_rphi->Scale(1. / all_frames);
  // h_sel_rphi->Scale(1., "width");
  TF2 *f_rphi_fit_test = new TF2("f_rphi_fit_test", "[0]*TMath::Gaus(y,[1]+x*[2],[3],true)", -4, 360, 0, 100);
  f_rphi_fit_test->SetParameter(0, 1.5);
  f_rphi_fit_test->SetParameter(1, 75.);
  f_rphi_fit_test->FixParameter(2, 0.);
  f_rphi_fit_test->SetParameter(3, 2.0);
  h_sel_rphi->Fit(f_rphi_fit_test, "");

  new TCanvas();
  h_sel_rphi->GetYaxis()->SetRangeUser(60, 90);
  h_sel_rphi->Draw("LEGO");
  f_rphi_fit_test->DrawCopy("SAME");

  new TCanvas();
  h_full_rphi_coverage->Scale(1. / h_full_rphi_coverage->GetEntries());
  h_full_rphi_coverage->Scale(1., "width");
  h_full_rphi_coverage->Draw("COLZ");

  TGraphErrors *g_r_resolution = new TGraphErrors();
  for (auto y_bin = 1; y_bin <= h_fit_r_n_gamma->GetNbinsY(); y_bin++)
  {
    auto n_gamma = h_fit_r_n_gamma->GetYaxis()->GetBinCenter(y_bin);
    auto current_r_slice = h_fit_r_n_gamma->ProjectionX(Form("r_slice_%i", y_bin), y_bin, y_bin);
    if (current_r_slice->GetEntries() < 100)
      continue;
    auto fit_gaus = new TF1("fit_gaus", "gaus", 0, 150);
    current_r_slice->Fit(fit_gaus, "QNR");
    g_r_resolution->SetPoint(g_r_resolution->GetN(), n_gamma, fit_gaus->GetParameter(2));
    g_r_resolution->SetPointError(g_r_resolution->GetN() - 1, 0, fit_gaus->GetParError(2));
    delete current_r_slice;
    delete fit_gaus;
  }
  g_r_resolution->SetName("g_r_resolution");
  TF1 *h_res_function = new TF1("h_res_function", "sqrt(([0]*[0]/(x-[2]))+[1]*[1])", 0, 100);
  h_res_function->SetParLimits(0, 0.25, 2.5);
  h_res_function->SetParLimits(1, 0.25, 2.5);
  h_res_function->SetParLimits(2, 0., 8.);
  h_res_function->SetParameters(2.0, 0.50, 0.50);
  auto c_fit = new TCanvas("c_fit", "c_fit", 600, 600);
  g_r_resolution->Draw("ALP");
  g_r_resolution->Fit(h_res_function, "S", "S", 0, 100);
  h_res_function->DrawCopy("SAME");

  new TCanvas();
  h_full_hitmap_tag->Draw("COLZ");
  draw_circle({1.4, 1.4, 74.4});

  new TCanvas();
  h_full_hitmap_trg0->Draw("COLZ");
  draw_circle({1.4, 1.4, 74.4});

  new TCanvas();
  h_full_hitmap_trg0_2nd->Draw("COLZ");
  draw_circle({1.4, 1.4, 74.4});

  new TCanvas();
  h_full_hitmap_trg0_3rd->Draw("COLZ");
  draw_circle({1.4, 1.4, 74.4});

  new TCanvas();
  h_full_hitmap_trg0_4th->Draw("COLZ");
  draw_circle({1.4, 1.4, 74.4});

  new TCanvas();
  h_full_hitmap_tag_2nd->Draw("COLZ");
  draw_circle({1.4, 1.4, 74.4});

  new TCanvas();
  h_test_nphotons->Draw();

  new TCanvas();
  h_fit_x->Draw();
  new TCanvas();
  h_fit_y->Draw();
  new TCanvas();
  h_fit_r->Draw();
  new TCanvas();
  h_fit_r_n_gamma->Draw();

  new TCanvas();
  h_full_xy_coverage->Draw("colz");

  new TCanvas();
  h_full_hitmap_test_filter->Draw("colz");

  new TCanvas();
  h_sel_rphi->Draw("COLZ");

  h_fit_r_n_gamma->Write();
  c_fit->Write();
  input_file_recodata->Close();
  // output_file->Close();
  */
}