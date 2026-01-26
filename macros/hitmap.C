#include "../lib/lightio.h"
#include "../lib/mapping.h"

void hitmap(std::string lightdata_infilename = "lightdata.root")
{

  gStyle->SetOptStat(false);
  gStyle->SetTitleXOffset(1.3);
  gStyle->SetTitleYOffset(1.3);

  TH2F *hMap[8] = {nullptr};
  TH1F *hDelta[8] = {nullptr};
  for (int ipdu = 0; ipdu < 8; ++ipdu)
  {
    hMap[ipdu] = new TH2F(Form("hMap_%d", ipdu), "", 16, 0, 16, 16, 0, 16);
    hDelta[ipdu] = new TH1F(Form("hDelta_%d", ipdu), "", 200, -100, 100);
  }

  TH2F *hPos = new TH2F("hPos", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);

  auto io = new sipm4eic::lightio();
  io->read_from_tree(lightdata_infilename);

  int n_events = 0;
  while (io->next_spill())
  {
    while (io->next_frame())
    {

      auto trigger0_vector = io->get_trigger0_vector();
      auto ref = trigger0_vector[0].coarse;

      auto cherenkov_vector = io->get_cherenkov_vector();
      for (auto &cherenkov : cherenkov_vector)
      {
        auto coarse = cherenkov.coarse;
        auto delta = coarse - ref;
        if (fabs(delta) > 10)
          continue;

        auto geo = sipm4eic::get_geo(cherenkov);
        auto pdu = geo[0];
        auto col = geo[1];
        auto row = geo[2];
        hMap[pdu]->Fill(col, row);

        auto pos = sipm4eic::get_position(geo);
        hPos->Fill(gRandom->Uniform(pos[0] - 1.5, pos[0] + 1.5), gRandom->Uniform(pos[1] - 1.5, pos[1] + 1.5));
      }

      ++n_events;
    }
  }

  auto cMap = new TCanvas("cMap", "cMap", 800, 800);
  cMap->Divide(3, 3, 0, 0);
  for (int ipdu = 0; ipdu < 8; ++ipdu)
  {
    cMap->cd(sipm4eic::placement[ipdu + 1]);
    cMap->cd(sipm4eic::placement[ipdu + 1])->SetLogz();
    hMap[ipdu]->Scale(1. / n_events);
    hMap[ipdu]->GetZaxis()->SetRangeUser(1. / n_events, 1.);
    hMap[ipdu]->Draw("col");
  }

  auto cPos = new TCanvas("cPos", "cPos", 800, 800);
  cPos->SetLogz();
  hPos->Draw("col");
}
