#include <TFile.h>
#include <TH2F.h>
#include "../lib/alcor_finedata.h"

gSystem->Load("alcor_recodata_h.so");

void test_fine()
{
  // Open the ROOT file
  TFile *file = TFile::Open("./Data/20251111-164951/lightdata.root");

  // Get the TH2F histogram
  TH2F *hist = (TH2F *)file->Get("TH2F_fine_calib_global_index");

  // Create finedata instance
  alcor_finedata fd;

  // Load calibration
  fd.generate_calibration(hist);

 // file->Close();
}