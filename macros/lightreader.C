#include "../lib/lightio.h"

void
lightreader(std::string filename = "lightdata.root")
{

  auto hDeltaT = new TH1F("hDeltaT", "hDeltaT", 512, -256, 256);
  auto hDeltaC = new TH1F("hDeltaC", "hDeltaC", 512, -256, 256);
  
  sipm4eic::lightio io;
  io.read_from_tree(filename);

  while (io.next_spill()) {
    while (io.next_frame()) {

      auto trigger0_vector = io.get_trigger0_vector();
      auto ref = trigger0_vector[0].coarse;

      auto timing_vector = io.get_timing_vector();
      for (auto &timing : timing_vector) {
	auto coarse = timing.coarse;
	auto delta = coarse - ref;
	hDeltaT->Fill(delta);
      }

      auto cherenkov_vector = io.get_cherenkov_vector();
      for (auto &cherenkov : cherenkov_vector) {
	auto coarse = cherenkov.coarse;
	auto device = cherenkov.device;
	auto delta = coarse - ref;
	hDeltaC->Fill(delta);
      }
      
    }
  }

  hDeltaT->Draw();
  hDeltaC->Draw("same");

}
