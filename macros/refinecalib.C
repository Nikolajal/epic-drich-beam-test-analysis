#include "../lib/lightio.h"

auto frefine = new TF1("frefine", "[0] + [1] * x - 0.5 * (1 + TMath::Erf( ( x - [2]) / [3] ))", 0., 128.); 

void
refinecalib(std::string refinedata_infilename, std::string finecalib_infilename, std::string finecalib_outfilename, int device = 0, bool show_fit = false)
{
  auto fin = TFile::Open(refinedata_infilename.c_str());
  auto hin = (THnSparse *)fin->Get("hRefine");

  sipm4eic::lightdata::load_fine_calibration(finecalib_infilename);

  /** loop over devices **/
  for (int idevice = 192; idevice < 208; ++idevice) {
    if (device != 0 && device != idevice) continue;
    std::cout << " --- processing device: " << idevice << std::endl;
    auto di = idevice - 192;
    hin->GetAxis(0)->SetRange(di + 1, di + 1);
    hin->GetAxis(1)->SetRange(-1, -1);

    /** loop over calibration indices **/
    auto hcindex = hin->Projection(1);
    for (int ci = 0; ci < 768; ++ci) {
      if (hcindex->GetBinContent(ci + 1) < 100) continue;
      if (ci % 4 == 0) std::cout << " --- processing cindex: " << ci << " " << hcindex->GetBinContent(ci + 1) << std::endl;
      hin->GetAxis(1)->SetRange(ci + 1, ci + 1);
      auto hrefine = hin->Projection(3, 2);
      auto prefine = hrefine->ProfileX("prefine");

      /** initialise refine fit parameters **/
      frefine->SetParameter(0, sipm4eic::lightdata::fine_off[di][ci]);
      frefine->SetParameter(1, sipm4eic::lightdata::fine_iif[di][ci]);
      frefine->SetParLimits(1, sipm4eic::lightdata::fine_iif[di][ci] * 0.8, sipm4eic::lightdata::fine_iif[di][ci] * 1.2);
      frefine->SetParameter(2, sipm4eic::lightdata::fine_cut[di][ci]);
      frefine->SetParLimits(2, sipm4eic::lightdata::fine_cut[di][ci] - 10., sipm4eic::lightdata::fine_cut[di][ci] + 10.);
      frefine->SetParameter(3, 1.);

      /** fit until it converges **/
      int fitres = prefine->Fit(frefine, "0q");
      for (int itry = 0; itry < 10 && fitres != 0; ++itry)
	fitres = prefine->Fit(frefine, "0q");
      //      prefine->Fit(frefine, "IMREq0");

      /** save fit parameters **/
      sipm4eic::lightdata::fine_off[di][ci] = frefine->GetParameter(0); 
      sipm4eic::lightdata::fine_iif[di][ci] = frefine->GetParameter(1);
      sipm4eic::lightdata::fine_cut[di][ci] = frefine->GetParameter(2);
      
      if (show_fit) {
	prefine->SetMarkerStyle(20);
	hrefine->Draw("colz");
	prefine->Draw("same");
	frefine->Draw("same");
	gPad->Update();
      }
    
      delete hrefine;
      delete prefine;
    }
    delete hcindex;
  }

  /** write calibration **/
  sipm4eic::lightdata::write_fine_calibration(finecalib_outfilename);
}
