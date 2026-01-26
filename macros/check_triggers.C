#include "../lib/framer.h"
#include "../lib/lightio.h"

void check_triggers(std::string lightdata_infilename)
{
  int device;
  int fifo;
  int type;
  int counter;
  int column;
  int pixel;
  int tdc;
  int rollover;
  int coarse;
  int fine;

  TFile *input_file = TFile::Open(lightdata_infilename.c_str(), "READ");
  TTree *target_tree = (TTree*)input_file->Get("alcor");

  target_tree->SetBranchAddress("device", &device);
  target_tree->SetBranchAddress("fifo", &fifo);
  target_tree->SetBranchAddress("type", &type);
  target_tree->SetBranchAddress("counter", &counter);
  target_tree->SetBranchAddress("column", &column);
  target_tree->SetBranchAddress("pixel", &pixel);
  target_tree->SetBranchAddress("tdc", &tdc);
  target_tree->SetBranchAddress("rollover", &rollover);
  target_tree->SetBranchAddress("coarse", &coarse);
  target_tree->SetBranchAddress("fine", &fine);

  TH1F* h_trigger_times = new TH1F("h_trigger_times", "Trigger times;time (s)", 1000, 0., 10.);

  for (auto i_ter = 0; i_ter < target_tree->GetEntries(); ++i_ter)
  {
    target_tree->GetEntry(i_ter);
    if (type != sipm4eic::data::trigger_tag)
      continue;
    h_trigger_times->Fill((coarse + rollover * sipm4eic::data::rollover_to_clock) * sipm4eic::data::coarse_to_ns*1.e-9);
  }

  h_trigger_times->Draw();
}