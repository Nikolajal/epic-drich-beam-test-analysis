#include "../lib/lightio.h"

void
lightfine(std::string lightdata_infilename, std::string finedata_outfilename)
{

  std::map<int, TH2F *> h_fine_device;
  
  sipm4eic::lightio io;
  io.read_from_tree(lightdata_infilename);

  while (io.next_spill()) {
    while (io.next_frame()) {

      auto timing_vector = io.get_timing_vector();
      auto cherenkov_vector = io.get_cherenkov_vector();

      for (auto &vector : {timing_vector, cherenkov_vector}) {
        for (auto &hit : vector) {
          
          auto device = hit.device;
          if (!h_fine_device.count(device))
            h_fine_device[device] = new TH2F(Form("hFine_%d", device), "hFine", 768, 0, 768, 256, 0, 256);
          
          auto index = hit.index;
          auto tdc = hit.tdc;
          auto cindex = hit.cindex();
          auto fine = hit.fine;
          h_fine_device[device]->Fill(cindex, fine);
          
        }
      }
    }
  }

  auto fout = TFile::Open(finedata_outfilename.c_str(), "RECREATE");
  for (auto &h : h_fine_device)
    h.second->Write();
  fout->Close();
  
}
