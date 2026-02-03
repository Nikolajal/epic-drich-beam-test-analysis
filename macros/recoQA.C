#include "../lib/lightio.h"
#include "../lib/data.h"
#include "../lib/mapping.h"
#include "../lib/utility.h"

void recoQA(std::string input_file = "recodata_2.root", std::string output_file = "out.root", std::string save_dir = "./images/")
{
  //  Output
  auto hPersistance2D = new TH2F("hPersistance2D", ";X (mm);Y (mm); t (ns)", 396, -99, 99, 396, -99, 99);
  auto hPersistance2D_initial_guess = new TH2F("hPersistance2D_initial_guess", ";X (mm);Y (mm); t (ns)", 66, -99, 99, 66, -99, 99);
  auto hMap_fullsetup_SiPM = new TH2F("hMap_fullsetup_SiPM", ";X (mm);Y (mm)", 4000, -100, 100, 4000, -100, 100);
  auto hMap_availsetup_SiPM = new TH2F("hMap_availsetup_SiPM", ";X (mm);Y (mm)", 4000, -100, 100, 4000, -100, 100);
  std::vector<TH2F *> hMap_found_Rings;

  //  Link TTree to local data instance
  recodata reco_data;
  auto reco_tree = load_data(input_file, reco_data);

  //  Store available SiPMs
  std::vector<std::array<float, 2>> list_of_available_SiPMs;

  //  First loop on events
  std::cout << "[INFO] Start of preliminary loop for X_{0}, Y_{0} and R_{0}" << std::endl;
  std::cout << "[INFO] Input entries: " << reco_tree->GetEntries() << std::endl;
  for (int iEv = 0; iEv < reco_tree->GetEntries(); iEv++)
  {
    //  Recover recodata entry form tree
    reco_tree->GetEntry(iEv);

    if ((iEv + 1) % 10000 == 0)
    {
      std::cout << "[INFO] event: " << iEv + 1 << std::endl;
    }

    //  Persistance plot
    fill_persistance(hPersistance2D, reco_data);
    fill_persistance<false>(hPersistance2D_initial_guess, reco_data);

    //  Loop on hits
    for (int iPnt = 0; iPnt < reco_data.n; iPnt++)
    {
      if (!count(list_of_available_SiPMs.begin(), list_of_available_SiPMs.end(), std::array<float, 2>({reco_data.x[iPnt], reco_data.y[iPnt]})))
        //  Add sensor sensitive area
        list_of_available_SiPMs.push_back({reco_data.x[iPnt], reco_data.y[iPnt]});
    }
  }

  for (auto iPDU = 1; iPDU <= 8; iPDU++)
    for (auto iCol = 0; iCol < 16; iCol++)
      for (auto iRow = 0; iRow < 16; iRow++)
        fill_with_SiPM_coverage(hMap_fullsetup_SiPM, sipm4eic::get_position({iPDU, iCol, iRow}));
  for (auto current_position : list_of_available_SiPMs)
    fill_with_SiPM_coverage(hMap_availsetup_SiPM, current_position);

  auto found_rings = fit_multiple_rings(hPersistance2D_initial_guess);

  //  === Graphics
  gROOT->SetBatch();
  system(Form("mkdir -p %s/", save_dir.c_str()));

  gStyle->SetPalette(kInvertedDarkBodyRadiator);

  //  === === Persistance
  auto current_canvas = get_std_canvas();
  hPersistance2D->Draw();
  current_canvas->SaveAs(Form("%s/hPersistance2D.png", save_dir.c_str()));

  //  === === SiPM and ring coverage
  //  === === === All SiPMs
  current_canvas = get_std_canvas();
  hMap_fullsetup_SiPM->Draw();
  current_canvas->SaveAs(Form("%s/hMap_fullsetup_SiPM.png", save_dir.c_str()));
  //  === === === Available SiPMs
  current_canvas = get_std_canvas();
  hMap_availsetup_SiPM->Draw();
  current_canvas->SaveAs(Form("%s/hMap_availsetup_SiPM.png", save_dir.c_str()));
  //  === === === Rings coverage
  current_canvas = plot_check_coordinates(hPersistance2D, found_rings);
  current_canvas->SaveAs(Form("%s/plot_check_coordinates.png", save_dir.c_str()));

  TFile *out = new TFile(output_file.c_str(), "RECREATE");
  hPersistance2D->Write();
  hMap_fullsetup_SiPM->Write();
  hMap_availsetup_SiPM->Write();
  out->Close();

  gROOT->SetBatch(kFALSE);
}

/*

float t_cut_center = 22.5;
float t_cut_sigma = 22.5;
void recoQA(std::string input_file = "recodata.root", std::string output_file = "out.root", std::string save_dir = "./images/")
{
  //  Output
  auto hPersistance2D = new TH2F("hPersistance2D", ";X (mm);Y (mm); t (ns)", 396, -99, 99, 396, -99, 99);
  auto hPersistance2D_Bkg = new TH2F("hPersistance2D_Bkg", "hPersistance2D_Bkg;x (mm);y (mm)", 60, -102, 102, 60, -102, 102);
  auto hPersistance2D_initial_guess = new TH2F("hPersistance2D_initial_guess", ";X (mm);Y (mm); t (ns)", 66, -99, 99, 66, -99, 99);
  auto hMap_fullsetup_SiPM = new TH2F("hMap_fullsetup_SiPM", ";X (mm);Y (mm)", 4000, -100, 100, 4000, -100, 100);
  auto hMap_availsetup_SiPM = new TH2F("hMap_availsetup_SiPM", ";X (mm);Y (mm)", 4000, -100, 100, 4000, -100, 100);
  auto hRadius_Distribution = new TH2F("hRadius_Distribution", ";N_{#gamma};R (mm)", 50, 0, 50, 250, 0, 100);
  auto hRadius_DistributionTest = new TH2F("hRadius_DistributionTest", ";N_{#gamma};R (mm)", 50, 0, 50, 200, 0, 150);
  std::vector<TH2F *> hMap_found_Rings;

  //  Link TTree to local data instance
  recodata reco_data;
  auto reco_tree = load_data(input_file, reco_data);

  //  Store available SiPMs
  std::vector<std::array<float, 2>> list_of_available_SiPMs;

  //  First loop on events
  std::cout << "[INFO] Start of preliminary loop for X_{0}, Y_{0} and R_{0}" << std::endl;
  for (int iEv = 0; iEv < reco_tree->GetEntries(); iEv++)
  {
    //  Recover recodata entry form tree
    reco_tree->GetEntry(iEv);

    if ((iEv) % 1000 == 0)
    {
      std::cout << "[INFO] event: " << iEv << std::endl;
    }

    //  Persistance plot
    auto fitered_reco_data = reco_data; // select_points<false>(reco_data, {0, 0, 30}, 25);
    fill_persistance(hPersistance2D, fitered_reco_data, t_cut_center, t_cut_sigma);
    fill_persistance<false, false>(hPersistance2D_Bkg, reco_data, t_cut_center, t_cut_sigma);
    fill_persistance<false>(hPersistance2D_initial_guess, fitered_reco_data, t_cut_center, t_cut_sigma);

    //  Loop on hits
    for (int iPnt = 0; iPnt < reco_data.n; iPnt++)
    {
      if (!count(list_of_available_SiPMs.begin(), list_of_available_SiPMs.end(), std::array<float, 2>({reco_data.x[iPnt], reco_data.y[iPnt]})))
        //  Add sensor sensitive area
        list_of_available_SiPMs.push_back({reco_data.x[iPnt], reco_data.y[iPnt]});
    }
  }
  hPersistance2D_Bkg->Scale(1. / (t_cut_sigma * 2 * 1.e-9)); // Convert to Hz
  hPersistance2D_Bkg->Scale(1. / 1000);                      // Convert to kHz
  hPersistance2D_Bkg->SetMinimum(100);                       // Set range from 100kHz
  hPersistance2D_Bkg->SetMaximum(100000);                    // to 100MHz
  auto found_rings = fit_multiple_rings(hPersistance2D_initial_guess);
  auto plot_check_coordinates_canvas = plot_check_coordinates(hPersistance2D, found_rings);

  /-/
  for (auto iPDU = 0; iPDU < 8; iPDU++)
      for (auto iCol = 0; iCol < 16; iCol++)
          for (auto iRow = 0; iRow < 16; iRow++)
              fill_with_SiPM_coverage(hMap_fullsetup_SiPM, sipm4eic::get_position({iPDU, iCol, iRow}));
  for (auto current_position : list_of_available_SiPMs)
      fill_with_SiPM_coverage(hMap_availsetup_SiPM, current_position);
/-/

  //  Second loop on events
  std::cout << "[INFO] Start of analysis loop" << std::endl;
  for (int iEv = 0; iEv < reco_tree->GetEntries(); iEv++)
  {
    //  Recover recodata entry form tree
    reco_tree->GetEntry(iEv);

    if ((iEv) % 1000 == 0)
    {
      std::cout << "[INFO] event: " << iEv << std::endl;
    }

    //  Loop on found loops
    auto iRing = -1;
    for (auto current_ring : found_rings)
    {
      iRing++;
      //  Radius distribution
      auto filtered_reco_data = select_points(reco_data, {current_ring[0][0], current_ring[1][0], current_ring[2][0]}, 4.5);
      auto fit_results = fit_reco_data(filtered_reco_data, {current_ring[0][0], current_ring[1][0], current_ring[2][0]});
      hRadius_Distribution->Fill(filtered_reco_data.n, fit_results[2][0]);
      // Loop on points
      for (int iPnt = 0; iPnt < filtered_reco_data.n; iPnt++)
      {
        auto polar_coordinates = cartesian_to_polar({reco_data.x[iPnt], reco_data.y[iPnt]}, {current_ring[0][0], current_ring[1][0]});
        hRadius_DistributionTest->Fill(filtered_reco_data.n, polar_coordinates[0]);
      }
    }
  }

  //  Loop on found rings
  TLatex *lLatex = new TLatex();
  lLatex->SetTextSize(0.03);
  auto iRing = -1;
  std::vector<TGraphErrors *> gSinglePhotonResolution;
  std::vector<TGraphErrors *> gPhotonRadiusCenter;
  std::vector<TGraphErrors *> gSNR;
  std::system(Form("mkdir -p %s/fitcheck/", save_dir.c_str()));
  for (auto current_ring : found_rings)
  {
    iRing++;
    auto cDump = new TCanvas();
    hRadius_DistributionTest->GetYaxis()->SetRangeUser(current_ring[2][0] - 15, current_ring[2][0] + 15);

    //  Cumulative radius
    auto current_full_radius_distribution = hRadius_DistributionTest->ProjectionY(Form("projY_r_%i", iRing), 4, 10000);
    current_full_radius_distribution->Fit("gaus", "Q");
    auto gaus_fit = current_full_radius_distribution->GetFunction("gaus");
    delete cDump;
    plot_check_coordinates_canvas->cd(2);
    lLatex->DrawLatexNDC(0.78, 0.91 - iRing * 0.25, Form("#sigma_{R} : %.2f#pm%.2f", gaus_fit->GetParameter(2), gaus_fit->GetParError(2)));

    //  Differential radius with number of photons
    cDump = new TCanvas();
    TF1 *fSliceFitFunction = new TF1("fSliceFitFunction", "[0]*(1./([2]*TMath::Sqrt(2*TMath::Pi())))*exp(-0.5*((x-[1])/[2])**2)+[3]*(1./([5]*TMath::Sqrt(2*TMath::Pi())))*exp(-0.5*((x-[4])/[5])**2)+[6]*(1+[7]*(x/150.)+[8]*(2*(x/150.)*(x/150.)-1)+[9]*(4*(x/150.)*(x/150.)*(x/150.)-3*(x/150.)))", -1000, 1000);
    fSliceFitFunction->SetNpx(10000);
    fSliceFitFunction->SetLineColor(kBlue);
    fSliceFitFunction->SetLineWidth(2);
    fSliceFitFunction->SetParameters(1.e6, current_ring[2][0], 2., 1.e6, current_ring[2][0] - 1, 2., 6, -30, 15, -30);
    fSliceFitFunction->SetParLimits(0, 1, 1.e9);
    fSliceFitFunction->SetParLimits(1, current_ring[2][0] - 0.5, current_ring[2][0] + 1);
    fSliceFitFunction->SetParLimits(2, 1, 5);
    fSliceFitFunction->SetParLimits(3, 1, 1.e9);
    fSliceFitFunction->SetParLimits(4, current_ring[2][0] - 1, current_ring[2][0] + 0.5);
    fSliceFitFunction->SetParLimits(5, 1, 5);
    TF1 *fSliceFitFunction_Gaus1 = new TF1("fSliceFitFunction_Gaus1", "[0]*(1./([2]*TMath::Sqrt(2*TMath::Pi())))*exp(-0.5*((x-[1])/[2])**2)", -1000, 1000);
    fSliceFitFunction_Gaus1->SetNpx(10000);
    fSliceFitFunction_Gaus1->SetLineColor(kRed - 2);
    TF1 *fSliceFitFunction_Gaus2 = new TF1("fSliceFitFunction_Gaus2", "[0]*(1./([2]*TMath::Sqrt(2*TMath::Pi())))*exp(-0.5*((x-[1])/[2])**2)", -1000, 1000);
    fSliceFitFunction_Gaus2->SetNpx(10000);
    fSliceFitFunction_Gaus2->SetLineColor(kRed - 4);
    TF1 *fSliceFitFunction_Bkg = new TF1("fSliceFitFunction_Bkg", "[0]*(1+[1]*(x/150.)+[2]*(2*(x/150.)*(x/150.)-1)+[3]*(4*(x/150.)*(x/150.)*(x/150.)-3*(x/150.)))", -1000, 1000);
    fSliceFitFunction_Bkg->SetNpx(10000);
    fSliceFitFunction_Bkg->SetLineColor(kBlue - 2);
    fSliceFitFunction_Bkg->SetLineStyle(kDashed);

    gSinglePhotonResolution.push_back(new TGraphErrors());
    gPhotonRadiusCenter.push_back(new TGraphErrors());
    gSNR.push_back(new TGraphErrors());
    gSinglePhotonResolution[iRing]->SetName(Form("gSinglePhotonResolution_Ring%i_R%.2f", iRing, current_ring[2][0]));
    gPhotonRadiusCenter[iRing]->SetName(Form("gPhotonRadiusCenter_Ring_%i_R%.2f", iRing, current_ring[2][0]));
    gSNR[iRing]->SetName(Form("gSNR_Ring_%i_R%.2f", iRing, current_ring[2][0]));
    auto current_slice = hRadius_DistributionTest->ProjectionY(Form("projY_tmp_%i_%i", 3, iRing), 3, 1000);
    cDump->cd();
    current_slice->Fit(fSliceFitFunction, "QI");
    if (fSliceFitFunction->GetParameter(1) < fSliceFitFunction->GetParameter(4))
    {
      fSliceFitFunction_Gaus1->SetParameters(fSliceFitFunction->GetParameter(0), fSliceFitFunction->GetParameter(1), fSliceFitFunction->GetParameter(2));
      fSliceFitFunction_Gaus2->SetParameters(fSliceFitFunction->GetParameter(3), fSliceFitFunction->GetParameter(4), fSliceFitFunction->GetParameter(5));
    }
    else
    {
      fSliceFitFunction_Gaus1->SetParameters(fSliceFitFunction->GetParameter(3), fSliceFitFunction->GetParameter(4), fSliceFitFunction->GetParameter(5));
      fSliceFitFunction_Gaus2->SetParameters(fSliceFitFunction->GetParameter(0), fSliceFitFunction->GetParameter(1), fSliceFitFunction->GetParameter(2));
    }
    fSliceFitFunction_Gaus1->Draw("SAME");
    fSliceFitFunction_Gaus2->Draw("SAME");
    fSliceFitFunction_Bkg->SetParameters(fSliceFitFunction->GetParameter(6), fSliceFitFunction->GetParameter(7), fSliceFitFunction->GetParameter(8), fSliceFitFunction->GetParameter(9));
    fSliceFitFunction_Bkg->Draw("SAME");
    cDump->SaveAs(Form("%s/fitcheck/%s.png", save_dir.c_str(), Form("projY_tmp_%i_%i", 3, iRing)));

    /-/
    for (auto iBin = 3; iBin < hRadius_DistributionTest->GetNbinsX(); iBin++)
    {
      cDump->cd();
      auto i_current_point = gSinglePhotonResolution[iRing]->GetN();
      auto current_slice = hRadius_DistributionTest->ProjectionY(Form("projY_tmp_%i_%i", iBin, iRing), iBin, iBin);
      if (current_slice->GetEntries() < 100)
        continue;
      fSliceFitFunction->FixParameter(1, current_slice->GetMean());
      current_slice->Fit(fSliceFitFunction, "QI");
      fSliceFitFunction->ReleaseParameter(1);
      current_slice->Fit(fSliceFitFunction, "QI");
      gSinglePhotonResolution[iRing]->SetPoint(i_current_point, iBin, fSliceFitFunction->GetParameter(2));
      gSinglePhotonResolution[iRing]->SetPointError(i_current_point, 0, fSliceFitFunction->GetParError(2));
      gPhotonRadiusCenter[iRing]->SetPoint(i_current_point, iBin, fSliceFitFunction->GetParameter(1));
      gPhotonRadiusCenter[iRing]->SetPointError(i_current_point, 0, fSliceFitFunction->GetParError(1));
      gSNR[iRing]->SetPoint(i_current_point, iBin, fSliceFitFunction->GetParameter(0) / (fSliceFitFunction->GetParameter(0) + fSliceFitFunction->GetParameter(3)));
      // gSNR[iRing]->SetPointError(i_current_point, 0, fSliceFitFunction->GetParameter(0) / (fSliceFitFunction->GetParameter(0) + fSliceFitFunction->GetParameter(3)));
      cDump->SaveAs(Form("%s/fitcheck/%s.png", save_dir.c_str(), Form("projY_tmp_%i_%i", iBin, iRing)));
    }
    /-/
    // TF1 *fSPR_FitFunction = new TF1("fSPR_FitFunction", "[0]/(TMath::Sqrt(x))", -1000, 1000);
    // gSinglePhotonResolution[iRing]->Fit(fSPR_FitFunction, "Q");
    // gPhotonRadiusCenter[iRing]->Fit("pol0");
    // auto pol0 = gPhotonRadiusCenter[iRing]->GetFunction("pol0");
    delete cDump;
    plot_check_coordinates_canvas->cd(2);
    lLatex->DrawLatexNDC(0.02, 0.87 - iRing * 0.25, Form("Fit R (mm):"));
    lLatex->DrawLatexNDC(0.58, 0.87 - iRing * 0.25, Form("R_{0} : %.2f#pm%.2f", fSliceFitFunction->GetParameter(1), fSliceFitFunction->GetParError(1)));
    lLatex->DrawLatexNDC(0.78, 0.87 - iRing * 0.25, Form("#sigma_{R} : %.2f#pm%.2f", fSliceFitFunction->GetParameter(2), fSliceFitFunction->GetParError(2)));
    lLatex->DrawLatexNDC(0.02, 0.83 - iRing * 0.25, Form("Fit R-N_{#gamma} (mm):"));
    // lLatex->DrawLatexNDC(0.39, 0.83 - iRing * 0.25, Form("#sigma_{SPR}: %.2f#pm%.2f", fSPR_FitFunction->GetParameter(0), fSPR_FitFunction->GetParError(0)));
    lLatex->SetTextColor(kRed);
    // lLatex->DrawLatexNDC(0.65, 0.83 - iRing * 0.25, Form("#sigma_{SPR}: %.2f#pm%.2f %%", 100 * fSPR_FitFunction->GetParameter(0) / pol0->GetParameter(0), 100 * fSPR_FitFunction->GetParError(0) / pol0->GetParameter(0)));
    lLatex->SetTextColor(kBlack);

    //  Integrated number of photons
    auto high_bin = hRadius_Distribution->GetYaxis()->FindBin(current_ring[2][0] - 5);
    auto lowr_bin = hRadius_Distribution->GetYaxis()->FindBin(current_ring[2][0] + 5);
    auto hPhotonDistribution = hRadius_Distribution->ProjectionX(Form("projX_tmp_%i", iRing), lowr_bin, high_bin);
    // https://root-forum.cern.ch/t/fitting-a-poisson-distribution-to-a-histogram/12078/4
    //  [0] = Normalizing parameter
    //  [1] / [2] -> mean (mu)
    //  x / [2] -> x
    //  Gamma( x / [2] + 1 ) = factorial (x / [2])
    TF1 *fPhotonFitFunction = new TF1("hPhotonFitFunction", "[0]*TMath::Power(([1]/[2]),(x/[2]))*(TMath::Exp(-([1]/[2])))/TMath::Gamma((x/[2])+1)", -1000, 1000);
    hPhotonDistribution->GetXaxis()->SetRange(4, 1000);
    fPhotonFitFunction->SetParameters(1, 1, 1);
    cDump = new TCanvas();
    hPhotonDistribution->Fit(fPhotonFitFunction, "I");
    delete cDump;
    plot_check_coordinates_canvas->cd(2);
    auto average_photon_value = fPhotonFitFunction->GetParameter(1) / fPhotonFitFunction->GetParameter(2);
    auto average_photon_error = average_photon_value * TMath::Sqrt((fPhotonFitFunction->GetParError(1) / fPhotonFitFunction->GetParameter(1)) * (fPhotonFitFunction->GetParError(1) / fPhotonFitFunction->GetParameter(1)) + (fPhotonFitFunction->GetParError(2) / fPhotonFitFunction->GetParameter(2)) * (fPhotonFitFunction->GetParError(2) / fPhotonFitFunction->GetParameter(2)));
    lLatex->DrawLatexNDC(0.02, 0.79 - iRing * 0.25, Form("Fit N_{#gamma}:"));
    lLatex->SetTextColor(kRed);
    lLatex->DrawLatexNDC(0.65, 0.79 - iRing * 0.25, Form("N_{#gamma}: %.2f#pm%.2f", average_photon_value, average_photon_error));
    lLatex->SetTextColor(kBlack);
  }

  //  === Graphics
  gROOT->SetBatch();
  system(Form("mkdir -p %s/", save_dir.c_str()));
  gStyle->SetPalette(kInvertedDarkBodyRadiator);

  //  === === Persistance
  auto current_canvas = get_std_canvas();
  hPersistance2D->Draw();
  current_canvas->SaveAs(Form("%s/hPersistance2D.png", save_dir.c_str()));
  //  === === === Bkg persistance
  current_canvas = get_std_canvas();
  gStyle->SetOptStat(0);
  hPersistance2D_Bkg->Draw("COLZ");
  current_canvas->SaveAs(Form("%s/hPersistance2D_Bkg.png", save_dir.c_str()));

  //  === === SiPM and ring coverage
  //  === === === All SiPMs
  current_canvas = get_std_canvas();
  hMap_fullsetup_SiPM->Draw();
  current_canvas->SaveAs(Form("%s/hMap_fullsetup_SiPM.png", save_dir.c_str()));
  //  === === === Available SiPMs
  current_canvas = get_std_canvas();
  hMap_availsetup_SiPM->Draw();
  current_canvas->SaveAs(Form("%s/hMap_availsetup_SiPM.png", save_dir.c_str()));
  //  === === === Found rings plots
  gStyle->SetOptStat(0);
  plot_check_coordinates_canvas->SaveAs(Form("%s/plot_check_coordinates.png", save_dir.c_str()));
  delete plot_check_coordinates_canvas;

  TFile *out = new TFile(Form("%s/%s", save_dir.c_str(), output_file.c_str()), "RECREATE");
  hPersistance2D->Write();
  hPersistance2D_Bkg->Write();
  hPersistance2D_initial_guess->Write();
  hMap_fullsetup_SiPM->Write();
  hMap_availsetup_SiPM->Write();
  hRadius_Distribution->Write();
  hRadius_DistributionTest->Write();
  for (auto current_graph : gSinglePhotonResolution)
    current_graph->Write();
  for (auto current_graph : gPhotonRadiusCenter)
    current_graph->Write();
  for (auto current_graph : gSNR)
    current_graph->Write();
  out->Close();

  gROOT->SetBatch(kFALSE);
}
*/
