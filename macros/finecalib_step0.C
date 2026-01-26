std::vector<int> devices_indices = {192, 193, 194, 195, 196, 197, 198, 207};

TF1 *f_fit = new TF1("f_fit", "[0]*(1./(exp(([1]-x)/[2])+1))*(1./(exp((x-[3])/[4])+1))", 10., 200.);

void finecalib_step0(std::string input_filename, std::string output_filename = "finecalib_step0.root", int minimum_entries = 1000, bool use_fit = true)
{
  //  Open file with TDC fine distributions
  TFile *input_file = new TFile(input_filename.c_str());
  TFile *outfile = new TFile(output_filename.c_str(), "RECREATE");
  //  Loop over available TDC distributions
  for (auto current_device_index : devices_indices)
  {
    TH2F *current_calib_histo = (TH2F *)(input_file->Get(Form("hFine_%i", current_device_index)));
    if (!current_calib_histo)
      continue;
    cout << "[INFO] Starting device " << current_device_index << endl;
    TH1F *hIIF = new TH1F(Form("hIIF_%i", current_device_index), "hIIF", 768, 0, 768);
    TH1F *hCUT = new TH1F(Form("hCUT_%i", current_device_index), "hCUT", 768, 0, 768);
    //  Take 2D TDC distribution for fit
    for (auto iBin = 1; iBin <= current_calib_histo->GetNbinsX(); iBin++)
    {
      //  Take slice to act on single TDC
      auto current_histo = current_calib_histo->ProjectionY("tmp", iBin, iBin);
      //  Book results variable
      //  If no entries, skip fit
      if (current_histo->GetEntries() < minimum_entries)
      {
        //  Leave 0 as dummy value in the calibration object
        continue;
      }
      // in ToT mode there might be plenty of FINE = 0, remove them
      current_histo->SetBinContent(1, 0.);
      current_histo->SetBinError(1, 0.);
      //  Setup fit function
      auto height_guess = current_histo->GetBinContent(current_histo->GetMaximumBin());
      auto critical_value = 0.25 * height_guess;
      auto min_bin = current_histo->FindFirstBinAbove(critical_value);
      auto min_guess = current_histo->GetBinLowEdge(min_bin);
      auto max_bin = current_histo->FindLastBinAbove(critical_value);
      auto max_guess = current_histo->GetBinLowEdge(max_bin);
      f_fit->SetParameter(0, height_guess);
      f_fit->SetParameter(1, min_guess);
      f_fit->SetParLimits(1, min_guess - 10, min_guess + 10);
      f_fit->SetParameter(2, 0.5);
      f_fit->SetParLimits(2, 0.1, 1.);
      f_fit->SetParameter(3, max_guess);
      f_fit->SetParLimits(3, max_guess - 10, max_guess + 10);
      f_fit->SetParameter(4, 0.5);
      f_fit->SetParLimits(4, 0.1, 1.);
      //  Fit Fine distribution
      if (use_fit)
        current_histo->Fit(f_fit, "0Q", "", 20, 120);
      //  Recover MIN and MAX from fit
      auto minimum = f_fit->GetParameter(1);
      auto maximum = f_fit->GetParameter(3);
      //  Calculate inverse IF and CUT
      auto IIF = 1. / (maximum - minimum);
      auto CUT = 0.5 * (minimum + maximum);
      //  Assign the values in the calibration object
      hIIF->SetBinContent(iBin, IIF);
      hCUT->SetBinContent(iBin, CUT);
    }
    outfile->cd();
    hIIF->Write();
    hCUT->Write();
  }
  outfile->Close();
}
