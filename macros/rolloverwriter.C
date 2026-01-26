#include "../lib/framer.h"
#include "../lib/lightio.h"

std::vector<std::string> devices = {
  "kc705-192",
  "kc705-193",
  "kc705-194",
  "kc705-195",
  "kc705-196",
  "kc705-197",
  "kc705-198",
  "kc705-199",
  "kc705-200",
  "kc705-201",
  "kc705-202",
  "kc705-203"
};

void
rolloverwriter(std::string dirname, std::string outfilename)
{

  auto fout = TFile::Open(outfilename.c_str(), "RECREATE");
  for (auto device : devices) {
    for (int ififo = 0; ififo < 24; ++ififo) {
      std::string filename = dirname + "/" + device + "/decoded/alcdaq.fifo_" + std::to_string(ififo) + ".root";
      auto fin = TFile::Open(filename.c_str());
      if (!fin || !fin->IsOpen()) continue;
      auto gin = (TGraph *)fin->Get("gRollover");
      fout->cd();
      gin->Write(Form("gRollover_%s_%d", device.c_str(), ififo));
      fin->Close();
    }
  }
  fout->Close();

}

