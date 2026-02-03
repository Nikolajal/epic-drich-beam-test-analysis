#include "recodata_writer.C"
#include "analysis_example.C"

void process_dcr()
{
  auto _THRESHOLD_DCR_ = 0.25; // kHz
  std::map<std::string, std::array<float, 4>> runs_T_OV_DTh_rad = {
      //  -25C 48.50 Vbd
      //{"20251114-010853", {-25.0, -0.50, 10, 1}},
      {"20251114-015103", {-25.0, +2.00, 10, 1}},
      {"20251114-022450", {-25.0, +2.50, 10, 1}},
      {"20251114-031050", {-25.0, +3.00, 10, 1}},
      {"20251114-035136", {-25.0, +4.00, 10, 1}},
      {"20251114-043649", {-25.0, +4.50, 10, 1}},
      {"20251114-162403", {-25.0, +5.00, 10, 1}},
      /*
      //{"20251114-172149", {-25.0, +3.50, 10, 1}},
      //  -15C 49.00 Vbd
      {"20251114-202429", {-15.0, +3.00, 10, 1}},
      {"20251114-212620", {-15.0, -1.00, 10, 1}},
      {"20251114-215942", {-15.0, +1.50, 10, 1}},
      {"20251114-223529", {-15.0, +2.00, 10, 1}},
      {"20251114-231141", {-15.0, +2.50, 10, 1}},
      {"20251115-004402", {-15.0, +3.50, 10, 1}},
      */
      {"20251115-013423", {-15.0, +4.00, 10, 1}},
      /*
      {"20251115-033117", {-15.0, +4.50, 10, 1}},
      //  -10C 49.25 Vbd
      {"20251115-123427", {-10.0, +2.75, 10, 1}},
      {"20251115-133630", {-10.0, -1.25, 10, 1}},
      {"20251115-160923", {-10.0, +1.25, 10, 1}},
      {"20251115-165919", {-10.0, +1.75, 10, 1}},
      {"20251115-173639", {-10.0, +2.25, 10, 1}},
      {"20251115-181745", {-10.0, +3.25, 10, 1}},
      {"20251115-193955", {-10.0, +4.25, 10, 1}},
      */
      {"20251115-185825", {-10.0, +3.75, 10, 1}},
      /*
      {"20251117-061737", {-10.0, +4.75, 10, 1}},
      {"20251117-070124", {-10.0, +4.25, 10, 1}},
      //  -5C 49.50 Vbd
      {"20251115-235725", {-5.0, +2.50, 10, 1}},
      {"20251116-005925", {-5.0, -1.50, 10, 1}},
      {"20251116-013830", {-5.0, +1.00, 10, 1}},
      {"20251116-024620", {-5.0, +1.50, 10, 1}},
      {"20251116-034911", {-5.0, +2.00, 10, 1}},
      {"20251116-043343", {-5.0, +3.00, 10, 1}},
      {"20251116-051339", {-5.0, +3.50, 10, 1}},
      */
      {"20251116-055302", {-5.0, +4.00, 10, 1}},
      /*
      {"20251116-222527", {-5.0, +4.50, 10, 1}},
      {"20251116-233659", {-5.0, +5.00, 10, 1}},
      //  -0C 49.75 Vbd
      */
      {"20251116-165203", {-0.0, +3.75, 10, 1}}};

  std::vector<std::string> temperature_scan_4OV = {
      "20251116-165203",
      "20251116-055302",
      "20251115-185825",
      "20251115-013423",
      "20251114-035136"};

  auto iter = -1;

  TGraphErrors *g_dcr_vs_temp_50 = new TGraphErrors();
  TGraphErrors *g_dcr_vs_temp_75 = new TGraphErrors();
  TGraphErrors *g_dcr_vs_OV_50 = new TGraphErrors();
  TGraphErrors *g_dcr_vs_OV_75 = new TGraphErrors();
  std::map<std::array<float, 4>, TGraphErrors *> g_dcr_1350;
  std::map<std::array<float, 4>, TGraphErrors *> g_dcr_1375;
  std::map<std::array<float, 4>, TGraphErrors *> g_res_test_0;
  std::map<std::array<float, 4>, TGraphErrors *> g_res_test_1;
  for (auto [current_run, run_info] : runs_T_OV_DTh_rad)
  {
    iter++;
    std::string data_repository = "/Volumes/NRUBINI_DATASTASH_1/BT25/Data/";
    std::cout << " ---- ---- ---- " << current_run << std::endl;
    recodata_writer(data_repository, current_run, 5, true, true);
    TFile *input_file = new TFile((data_repository + "/" + current_run + "/recodata.root").c_str());
    if (!input_file || input_file->IsZombie())
    {
      std::cerr << "[ERROR] Could not open recodata for DCR processing, exiting" << std::endl;
      return;
    }
    auto drc_plot = static_cast<TProfile *>(input_file->Get("h_dcr_rate_start_of_spill"));

    //  Create the TGraph if not present
    g_dcr_1350.try_emplace(run_info, new TGraphErrors());
    g_dcr_1375.try_emplace(run_info, new TGraphErrors());
    g_res_test_0.try_emplace(run_info, new TGraphErrors());
    g_res_test_1.try_emplace(run_info, new TGraphErrors());

    //  Calculate DCR for 1350 sensors
    auto _1350_dcr_val = 0.;
    auto _1350_dcr_err = 0.;
    auto _1350_dcr_RMS = 0.;
    auto _1350_dcr_prt = 0;
    for (int i_bin = 1; i_bin <= 1025; ++i_bin)
    {
      if (drc_plot->GetBinContent(i_bin) > 0)
      {
        _1350_dcr_prt++;
        _1350_dcr_val += drc_plot->GetBinContent(i_bin);
        _1350_dcr_err += drc_plot->GetBinError(i_bin) * drc_plot->GetBinError(i_bin);
      }
    }
    auto _1350_dcr_avg_val = _1350_dcr_val / _1350_dcr_prt;
    _1350_dcr_val = 0.;
    _1350_dcr_err = 0.;
    _1350_dcr_prt = 0;
    for (int i_bin = 1; i_bin <= 1025; ++i_bin)
    {
      if (fabs(drc_plot->GetBinContent(i_bin) - _1350_dcr_avg_val) < _1350_dcr_avg_val * _THRESHOLD_DCR_)
      {
        _1350_dcr_prt++;
        _1350_dcr_val += drc_plot->GetBinContent(i_bin);
        _1350_dcr_err += drc_plot->GetBinError(i_bin) * drc_plot->GetBinError(i_bin);
      }
    }
    _1350_dcr_avg_val = _1350_dcr_val / _1350_dcr_prt;
    _1350_dcr_err = sqrt(_1350_dcr_err) / _1350_dcr_prt;
    for (int i_bin = 1; i_bin <= 1025; ++i_bin)
      if (fabs(drc_plot->GetBinContent(i_bin) - _1350_dcr_avg_val) < _1350_dcr_avg_val * _THRESHOLD_DCR_)
        _1350_dcr_RMS += (drc_plot->GetBinContent(i_bin) - _1350_dcr_avg_val) * (drc_plot->GetBinContent(i_bin) - _1350_dcr_avg_val);
    _1350_dcr_RMS = sqrt(_1350_dcr_RMS / _1350_dcr_prt);

    //  Calculate DCR for 1375 sensors
    auto _1375_dcr_val = 0.;
    auto _1375_dcr_err = 0.;
    auto _1375_dcr_RMS = 0.;
    auto _1375_dcr_prt = 0;
    for (int i_bin = 1026; i_bin <= 2050; ++i_bin)
    {
      if (drc_plot->GetBinContent(i_bin) > 0)
      {
        _1375_dcr_prt++;
        _1375_dcr_val += drc_plot->GetBinContent(i_bin);
        _1375_dcr_err += drc_plot->GetBinError(i_bin) * drc_plot->GetBinError(i_bin);
      }
    }
    auto _1375_dcr_avg_val = _1375_dcr_val / _1375_dcr_prt;
    _1375_dcr_val = 0.;
    _1375_dcr_err = 0.;
    _1375_dcr_prt = 0;
    for (int i_bin = 1026; i_bin <= 2050; ++i_bin)
    {
      if (fabs(drc_plot->GetBinContent(i_bin) - _1375_dcr_avg_val) < _1375_dcr_avg_val * _THRESHOLD_DCR_)
      {
        _1375_dcr_prt++;
        _1375_dcr_val += drc_plot->GetBinContent(i_bin);
        _1375_dcr_err += drc_plot->GetBinError(i_bin) * drc_plot->GetBinError(i_bin);
      }
    }
    _1375_dcr_avg_val = _1375_dcr_val / _1375_dcr_prt;
    _1375_dcr_err = sqrt(_1375_dcr_err) / _1375_dcr_prt;
    for (int i_bin = 1026; i_bin <= 2050; ++i_bin)
      if (fabs(drc_plot->GetBinContent(i_bin) - _1375_dcr_avg_val) < _1375_dcr_avg_val * _THRESHOLD_DCR_)
        _1375_dcr_RMS += (drc_plot->GetBinContent(i_bin) - _1375_dcr_avg_val) * (drc_plot->GetBinContent(i_bin) - _1375_dcr_avg_val);
    _1375_dcr_RMS = sqrt(_1375_dcr_RMS / _1375_dcr_prt);

    std::cout << "[DCR RESULTS] Run " << current_run << " :" << std::endl;
    std::cout << "  - 1350 DCR: " << _1350_dcr_avg_val << " +/- " << _1350_dcr_err << " kHz" << std::endl;
    std::cout << "  - 1375 DCR: " << _1375_dcr_avg_val << " +/- " << _1375_dcr_err << " kHz" << std::endl;
    g_dcr_vs_temp_50->SetPoint(iter, runs_T_OV_DTh_rad[current_run][0], _1350_dcr_avg_val);
    g_dcr_vs_temp_50->SetPointError(iter, 0, _1350_dcr_RMS);
    g_dcr_vs_temp_75->SetPoint(iter, runs_T_OV_DTh_rad[current_run][0], _1375_dcr_avg_val);
    g_dcr_vs_temp_75->SetPointError(iter, 0, _1375_dcr_RMS);
    g_dcr_vs_OV_50->SetPoint(iter, runs_T_OV_DTh_rad[current_run][1], _1350_dcr_avg_val);
    g_dcr_vs_OV_50->SetPointError(iter, 0, _1350_dcr_RMS);
    g_dcr_vs_OV_75->SetPoint(iter, runs_T_OV_DTh_rad[current_run][1], _1375_dcr_avg_val);
    g_dcr_vs_OV_75->SetPointError(iter, 0, _1375_dcr_RMS);

    auto current_point_OV_graph = g_dcr_1350[run_info]->GetN();
    g_dcr_1350[run_info]->SetPoint(current_point_OV_graph, current_point_OV_graph, _1350_dcr_avg_val);
    g_dcr_1350[run_info]->SetPointError(current_point_OV_graph, 0, _1350_dcr_RMS);
    current_point_OV_graph = g_dcr_1375[run_info]->GetN();
    g_dcr_1375[run_info]->SetPoint(current_point_OV_graph, current_point_OV_graph, _1375_dcr_avg_val);
    g_dcr_1375[run_info]->SetPointError(current_point_OV_graph, 0, _1375_dcr_RMS);

    //
    analysis_example(data_repository, current_run);
    TFile *input_file_2 = new TFile((data_repository + "/" + current_run + "/analysis_example.root").c_str());
    if (!input_file_2 || input_file_2->IsZombie())
    {
      std::cerr << "[ERROR] Could not open recodata for DCR processing, exiting" << std::endl;
      return;
    }
    auto c_fit = static_cast<TCanvas *>(input_file_2->Get("c_fit"));
    TF1 *f = nullptr;
    for (auto *obj : *c_fit->GetPad(0)->GetListOfPrimitives())
    {
      f = dynamic_cast<TF1 *>(obj);
      if (f)
        break;
    }
    if (f)
    {
      std::cout << "Found function: " << f->GetName() << std::endl;
    }
    auto point = g_res_test_0[run_info]->GetN();
    g_res_test_0[run_info]->SetPoint(point, point, f->GetParameter(0));
    g_res_test_0[run_info]->SetPointError(point, 0, f->GetParError(0));
    point = g_res_test_1[run_info]->GetN();
    g_res_test_1[run_info]->SetPoint(point, point, f->GetParameter(1));
    g_res_test_1[run_info]->SetPointError(point, 0, f->GetParError(1));
    delete c_fit;
  }

  g_dcr_vs_temp_50->SetMarkerStyle(20);
  g_dcr_vs_temp_50->SetMarkerColor(kBlue);
  g_dcr_vs_temp_75->SetMarkerStyle(20);
  g_dcr_vs_temp_75->SetMarkerColor(kRed);

  TLegend *legend = new TLegend(0.12, 0.65, 0.52, 0.85);
  legend->AddEntry(g_dcr_vs_temp_50, "HPK S13360-3050VS", "p");
  legend->AddEntry(g_dcr_vs_temp_75, "HPK S13360-3075VS", "p");
  legend->SetBorderSize(0);
  legend->SetFillStyle(0);

  TH1F *hframe_OV = new TH1F("", ";OV (V);DCR (kHz)", 30, 1, 6);
  hframe_OV->SetMaximum(10);
  hframe_OV->SetMinimum(0.1);

  TLatex *llatex = new TLatex();
  llatex->SetTextFont(42);

  new TCanvas();
  gStyle->SetOptStat(0);
  hframe_OV->Draw();

  for (auto [run_info, graph] : g_dcr_1350)
  {
    if (run_info[0] > -22.5)
      continue;
    for (auto i_pnt = 0; i_pnt < graph->GetN(); i_pnt++)
    {
      graph->SetPointX(i_pnt, run_info[1]);
      g_dcr_1375[run_info]->SetPointX(i_pnt, run_info[1]);
    }
    graph->Draw("SAME PE");
    graph->SetMarkerStyle(20);
    graph->SetMarkerColor(kBlue);
    g_dcr_1375[run_info]->Draw("SAME PE");
    g_dcr_1375[run_info]->SetMarkerStyle(20);
    g_dcr_1375[run_info]->SetMarkerColor(kRed);
    legend->Draw("SAME");
    llatex->DrawLatexNDC(0.16, 0.85, "@ -25 C");
  }

  new TCanvas();
  gStyle->SetOptStat(0);
  hframe_OV->Draw();
  for (auto [run_info, graph] : g_res_test_0)
  {
    if (run_info[0] > -22.5)
      continue;
    for (auto i_pnt = 0; i_pnt < graph->GetN(); i_pnt++)
    {
      graph->SetPointX(i_pnt, run_info[1]);
      g_res_test_1[run_info]->SetPointX(i_pnt, run_info[1]);
    }
    graph->Draw("SAME PE");
    graph->SetMarkerStyle(20);
    graph->SetMarkerColor(kBlue);
    g_res_test_1[run_info]->Draw("SAME PE");
    g_res_test_1[run_info]->SetMarkerStyle(20);
    g_res_test_1[run_info]->SetMarkerColor(kRed);
    legend->Draw("SAME");
    llatex->DrawLatexNDC(0.16, 0.85, "@ -25 C");
  }

  TH1F *hframe_T = new TH1F("", ";T (C);DCR (kHz)", 70, -30, 5);
  hframe_T->SetMaximum(40);
  hframe_T->SetMinimum(0.1);

  new TCanvas();
  gStyle->SetOptStat(0);
  hframe_T->Draw();

  for (auto [run_info, graph] : g_dcr_1350)
  {
    if (fabs(run_info[1] - 3.825) > 0.2)
      continue;
    for (auto i_pnt = 0; i_pnt < graph->GetN(); i_pnt++)
    {
      graph->SetPointX(i_pnt, run_info[0]);
      g_dcr_1375[run_info]->SetPointX(i_pnt, run_info[0]);
    }
    graph->Draw("SAME PE");
    graph->SetMarkerStyle(20);
    graph->SetMarkerColor(kBlue);
    g_dcr_1375[run_info]->Draw("SAME PE");
    g_dcr_1375[run_info]->SetMarkerStyle(20);
    g_dcr_1375[run_info]->SetMarkerColor(kRed);
    legend->Draw("SAME");
    llatex->DrawLatexNDC(0.16, 0.85, "@ 4 overvoltage");
  }
}