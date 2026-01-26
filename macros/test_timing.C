#include "../lib/lightio.h"

void test_timing(std::string filename = "Data/20251111-105543/lightdata.root")
{
  //  Read lightdata
  auto io = new sipm4eic::lightio;
  io->read_from_tree(filename);

  //  Output histograms
  TH1F *h_time_dist = new TH1F("h_time_dist", ";#Delta_{t} (ns)", 1000, -1000, 1000);
  TH1F *h_time_dist_diff = new TH1F("h_time_dist_diff", ";#Delta_{t} (ns)", 1000, -1000, 1000);
  TH1F *h_time_dist_time1 = new TH1F("h_time_dist_time1", ";#Delta_{t} (ns)", 1000, -1000, 1000);
  TH1F *h_time_dist_time2 = new TH1F("h_time_dist_time2", ";#Delta_{t} (ns)", 1000, -1000, 1000);
  TH2F *h_participants_map = new TH2F("h_participants_map", ";n_{hits} time_{0};n_{hits} time_{1}", 33, 0, 33, 33, 0, 33);

  //  Utility variables
  std::map<int, float> timing_participants_map_time0;
  std::map<int, float> timing_participants_map_time1;

  //  Loop on events
  int n_events = 0;
  int n_spill = -1;
  int n_frame = -1;
  while (io->next_spill())
  {
    n_spill++;
    while (io->next_frame())
    {
      n_frame++;

      auto trigger0_vector = io->get_trigger0_vector();
      if (!trigger0_vector.size())
        continue;
      auto ref = trigger0_vector[0].coarse;

      auto timing_vector = io->get_timing_vector();
      for (auto &timing : timing_vector)
      {
        //  Hit variables
        auto index = timing.index;
        auto coarse = timing.coarse;
        auto delta = coarse - ref;
        

        //  Fill histograms
        h_time_dist->Fill(delta * sipm4eic::lightdata::coarse_to_ns);
        if (index < 50)
          h_time_dist_time1->Fill(delta * sipm4eic::lightdata::coarse_to_ns);
        else
          h_time_dist_time2->Fill(delta * sipm4eic::lightdata::coarse_to_ns);

        //  Store participants
        if (index < 50)
          timing_participants_map_time0[index] = delta * sipm4eic::lightdata::coarse_to_ns;
        else
          timing_participants_map_time1[index] = delta * sipm4eic::lightdata::coarse_to_ns;
      }

      if (timing_participants_map_time0.size() == 32 && timing_participants_map_time1.size() == 32)
      {
        auto avg_time0 = 0.f;
        for (auto &[idx0, time0] : timing_participants_map_time0)
          avg_time0 += time0 / 32.f;
        auto avg_time1 = 0.f;
        for (auto &[idx1, time1] : timing_participants_map_time1)
          avg_time1 += time1 / 32.f;
        h_time_dist_diff->Fill(avg_time1 - avg_time0);
      }

      h_participants_map->Fill(timing_participants_map_time0.size(), timing_participants_map_time1.size());
      ++n_events;
    }
    n_frame = -1;
  }

  TCanvas *c1 = new TCanvas("c1", "c1", 800, 600);
  h_time_dist->Draw();
  TCanvas *c2 = new TCanvas("c2", "c2", 800, 600);
  h_time_dist_time1->Draw();
  TCanvas *c3 = new TCanvas("c3", "c3", 800, 600);
  h_time_dist_time2->Draw();
  TCanvas *c4 = new TCanvas("c4", "c4", 800, 600);
  h_participants_map->Draw("COLZ");
  TCanvas *c5 = new TCanvas("c5", "c5", 800, 600);
  h_time_dist_diff->Draw();
}
