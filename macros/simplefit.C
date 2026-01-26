std::map<std::string, std::array<double, 2>>
simplefit(std::string recodata_infilename)
{

  unsigned short n;
  float x[65534];
  float y[65534];
  float t[65534];
  auto fin = TFile::Open(recodata_infilename.c_str());
  auto tin = (TTree *)fin->Get("recodata");
  auto nev = tin->GetEntries();
  tin->SetBranchAddress("n", &n);
  tin->SetBranchAddress("x", &x);
  tin->SetBranchAddress("y", &y);
  tin->SetBranchAddress("t", &t);

  auto hXY = new TH2F("hMap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  hXY->SetStats(false);
  auto hT = new TH1F("hT", ";t (ns);", 50, -78.125, 78.125);

  auto hN = new TH1F("hN", "", 396 / 2, -99, 99);
  auto hS = new TH1F("hS", "", 396 / 2, -99, 99);
  auto hE = new TH1F("hE", "", 396 / 2, -99, 99);
  auto hO = new TH1F("hO", "", 396 / 2, -99, 99);
  auto hR = new TH1F("hR", "", 396, -99, 99);
  
  for (int iev = 0; iev < nev; ++iev) {
    tin->GetEntry(iev);
    for (int i = 0 ; i < n; ++i) {
      hT->Fill(t[i]);
      if (t[i] > 25 || t[i] < 5) continue;
      auto xx = gRandom->Uniform(x[i] - 1.5, x[i] + 1.5);
      auto yy = gRandom->Uniform(y[i] - 1.5, y[i] + 1.5);
      hXY->Fill(xx, yy);
      if (fabs(x[i]) < 5. && y[i] > 0.)
	  hN->Fill(y[i]);
      if (fabs(x[i]) < 5. && y[i] < 0.)
	  hS->Fill(y[i]);
      if (fabs(y[i]) < 5. && x[i] > 0.)
	  hE->Fill(x[i]);
      if (fabs(y[i]) < 5. && x[i] < 0.)
	  hO->Fill(x[i]);
    } 
  }

  auto cXY = new TCanvas("cXY", "cXY", 800, 800);
  cXY->SetMargin(0.15, 0.15, 0.15, 0.15);
  cXY->SetLogz();
  hXY->Draw("colz");
  cXY->SaveAs("hitmap.png");

  auto f = new TF1("f", "[0] * TMath::Gaus(x, [1], [2]) + [3]");
  
  auto cE = new TCanvas("cE", "cE", 800, 800);
  f->SetParameter(0, hE->GetEntries());
  f->SetParameter(1, hE->GetMean());
  f->SetParLimits(1, hE->GetMean() - 10., hE->GetMean() + 10.);
  f->SetParameter(2, 3.);
  //  f->SetParLimits(2, 1., 4.);
  f->SetParameter(3, 0.01 * hE->GetEntries());
  hE->Draw();
  hE->Fit(f, "", "", hE->GetMean() - 10., hE->GetMean() + 10.);
  auto xE = f->GetParameter(1);
  auto cO = new TCanvas("cO", "cO", 800, 800);
  f->SetParameter(0, hO->GetEntries());
  f->SetParameter(1, hO->GetMean());
  f->SetParLimits(1, hO->GetMean() - 10., hO->GetMean() + 10.);
  f->SetParameter(2, 3.);
  //  f->SetParLimits(2, 1., 4.);
  f->SetParameter(3, 0.01 * hO->GetEntries());
  hO->Draw();
  hO->Fit(f, "", "", hO->GetMean() - 10., hO->GetMean() + 10.);
  auto xO = f->GetParameter(1);
  auto x0 = 0.5 * (xE + xO);

  auto cN = new TCanvas("cN", "cN", 800, 800);
  f->SetParameter(0, hN->GetEntries());
  f->SetParameter(1, hN->GetMean());
  f->SetParLimits(1, hN->GetMean() - 10., hN->GetMean() + 10.);
  f->SetParameter(2, 3.);
  //  f->SetParLimits(2, 1., 4.);
  f->SetParameter(3, 0.01 * hN->GetEntries());
  hN->Draw();
  hN->Fit(f, "", "", hN->GetMean() - 10., hN->GetMean() + 10.);
  auto yN = f->GetParameter(1);
  auto cS = new TCanvas("cS", "cS", 800, 800);
  f->SetParameter(0, hS->GetEntries());
  f->SetParameter(1, hS->GetMean());
  f->SetParLimits(1, hS->GetMean() - 10., hS->GetMean() + 10.);
  f->SetParameter(2, 3.);
  //f->SetParLimits(2, 1., 4.);
  f->SetParameter(3, 0.01 * hS->GetEntries());
  hS->Draw();
  hS->Fit(f, "", "", hS->GetMean() - 10., hS->GetMean() + 10.);
  auto yS = f->GetParameter(1);

  auto y0 = 0.5 * (yN + yS);

  std::cout << " x0 = " << x0 << " | y0 = " << y0 << std::endl;

  x0 = 0.;
  y0 = 0.;
  
  for (int iev = 0; iev < nev; ++iev) {
    tin->GetEntry(iev);
    for (int i = 0 ; i < n; ++i) {
      if (t[i] > 25 || t[i] < 5) continue;
      //      auto xx = gRandom->Uniform(x[i] - 1.5, x[i] + 1.5) - x0;
      //      auto yy = gRandom->Uniform(y[i] - 1.5, y[i] + 1.5) - y0;
      auto xx = x[i] - x0;
      auto yy = y[i] - y0;
      auto R = std::hypot(xx, yy);
      hR->Fill(R);
    } 
  }

  auto cR = new TCanvas("cR", "cR", 800, 800);
  f->SetParameter(0, hR->GetEntries());
  f->SetParameter(1, hR->GetMean());
  f->SetParLimits(1, hR->GetMean() - 10., hN->GetMean() + 10.);
  f->SetParameter(2, 3.);
  //  f->SetParLimits(2, 1., 4.);
  f->SetParameter(3, 0.01 * hR->GetEntries());
  hR->Draw();
  hR->Fit(f, "", "", hR->GetMean() - 10., hN->GetMean() + 10.);

  auto radius = std::array<double, 2>( { f->GetParameter(1), f->GetParError(1) } );
  auto sigma = std::array<double, 2>( { f->GetParameter(2), f->GetParError(2) } );

  return { { "radius", radius } , { "sigma", sigma } };
}

std::map<std::string, float> runlist = {
  {"20240527-111636",	0.},
  {"20240527-111009",	10.},
  {"20240527-000410",	20.},
  {"20240526-234138",	30.},
  {"20240526-231457",	40.},
  {"20240527-001117",	45.},
  {"20240526-230701",	50.},
  {"20240526-225403",	55.},
  {"20240526-221557",	60.},
  {"20240526-220929",	65.},
  {"20240526-214648",	70.},
  {"20240526-212506",	75.},
  {"20240526-235529",	75.},
  {"20240526-222205",	80.},
  {"20240526-222751",	85.},
  {"20240526-223322",	90.},
  {"20240526-223957",	95.},
  {"20240526-224653",	100.},
  {"20240527-144641",   130.}
};  

void
mirror_scan()
{
  auto gR = new TGraphErrors;
  auto gS = new TGraphErrors;
  auto n = 0;
  for (auto &[run, position] : runlist) {
    std::string filename = run + "/process-online/recodata.root";
      auto result = simplefit(filename);
      gR->SetPoint(n, position, result["radius"][0]);
      gR->SetPointError(n, 0, result["radius"][1]);
      gS->SetPoint(n, position, result["sigma"][0]);
      gS->SetPointError(n, 0, result["sigma"][1]);
      ++n;
  }
  gS->Draw("ap*");
}
