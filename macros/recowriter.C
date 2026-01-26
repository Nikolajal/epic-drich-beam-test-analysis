#include "../lib/lightio.h"
#include "../lib/mapping.h"

#define TREFCUT
float Tref_min = 32;
float Tref_max = 224;

enum EReferenceTime_t
{
  kTrigger,
  kTiming
};

unsigned short
trigger_mask(sipm4eic::lightio *io, float Tref)
{
  unsigned short trgmask = 0;

  auto trigger0_vector = io->get_trigger0_vector();
  for (auto &hit : trigger0_vector)
  {
    if (std::fabs(hit.coarse - Tref) < 25)
      trgmask |= (1 << 0);
  }

  auto trigger1_vector = io->get_trigger1_vector();
  for (auto &hit : trigger1_vector)
  {
    if (std::fabs(hit.coarse - Tref) < 25)
      trgmask |= (1 << 1);
  }

  auto trigger2_vector = io->get_trigger2_vector();
  for (auto &hit : trigger2_vector)
  {
    if (std::fabs(hit.coarse - Tref) < 25)
      trgmask |= (1 << 2);
  }

  auto trigger3_vector = io->get_trigger3_vector();
  for (auto &hit : trigger3_vector)
  {
    if (std::fabs(hit.coarse - Tref) < 25)
      trgmask |= (1 << 3);
  }

  return trgmask;
}

float reference_time(sipm4eic::lightio *io, EReferenceTime_t method = kTrigger)
{

  /** time from trigger **/
  if (method == kTrigger)
  {
    auto trigger0_vector = io->get_trigger0_vector();
    auto Ttrg = trigger0_vector.size() > 0 ? trigger0_vector[0].coarse : -666.;
    return Ttrg;
  }

  /** reference time from timing system **/
  if (method == kTiming)
  {
    auto timing_vector = io->get_timing_vector();

    /** collect timing hits **/
    std::map<int, sipm4eic::lightdata> timing_hits;
    for (auto &hit : timing_vector)
    {
      auto index = hit.index;
      if (timing_hits.count(index) && timing_hits[index].time() < hit.time())
        continue;
      timing_hits[index] = hit;
    }

    /** compute timing time **/
    int Nref = timing_hits.size();
    float Tref = 0.;
    for (auto &[index, hit] : timing_hits)
      Tref += hit.time();
    if (Nref > 0)
      Tref /= Nref;

    return Tref;
  }

  return 0.;
}

void recowriter(std::string lightdata_infilename,
                std::string recodata_outfilename,
                std::string finecalib_infilename = "",
                EReferenceTime_t reference_method = kTrigger)
{
  //  Read from decoded light data
  auto io = new sipm4eic::lightio;
  io->read_from_tree(lightdata_infilename);

  //  Load fine calibration
  sipm4eic::lightdata::load_fine_calibration(finecalib_infilename);

  //  Hit map for visualization
  TH1F *h_time_dist_trigger = new TH1F("h_time_dist_trigger", ";t_{trigger0} - t_{trigger1} (ns)", 1000, -40, 40);
  TH1F *h_time_dist = new TH1F("h_time_dist", ";t_{cherenkov} - t_{trigger} (ns)", 1000, -40, 40);
  TH1F *h_time_dist_timing = new TH1F("h_time_dist_timing", ";#Delta_{t} (ns)", 1000, -1000, 1000);
  TH1F *h_time_dist_timing_to_cherenkov = new TH1F("h_time_dist_timing_to_cherenkov", ";#Delta_{t} (ns)", 1000, -1000, 1000);
  TH1F *h_time_dist_timing_to_ref = new TH1F("h_time_dist_timing_to_ref", ";#Delta_{t} (ns)", 500, -1000, 1000);
  TH2F *h_time_dist_per_spill = new TH2F("h_time_dist_per_spill", ";spill;#Delta_{t} (ns)", 10, 0, 10, 60, -100, 100);
  TH1F *h_radius_dist = new TH1F("h_radius_dist", ";Radius (mm)", 80, 20, 120);
  TH1F *h_radius_dist_fpeak = new TH1F("h_radius_dist_fpeak", ";Radius (mm)", 80, 20, 120);
  TH1F *h_radius_dist_speak = new TH1F("h_radius_dist_speak", ";Radius (mm)", 80, 20, 120);
  TH2F *h_hit_map = new TH2F("h_hit_map", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_hit_map_timing_test = new TH2F("h_hit_map_timing_test", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_hit_map_fpeak = new TH2F("h_hit_map_fpeak", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_hit_map_speak = new TH2F("h_hit_map_speak", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_hit_map_timing = new TH2F("h_hit_map_timing", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_hit_map_notiming = new TH2F("h_hit_map_notiming", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_participants_map = new TH2F("h_participants_map", ";n_{hits} time_{0};n_{hits} time_{1}", 33, 0, 33, 33, 0, 33);

  // prepare output data
  unsigned short spill;
  unsigned int frame;
  float tref;
  unsigned short trgmask;
  unsigned short n;
  unsigned short ch[65543];
  float x[65534];
  float y[65534];
  float t[65534];
  auto fout = TFile::Open(recodata_outfilename.c_str(), "RECREATE");
  auto tout = new TTree("recodata", "recodata");
  tout->Branch("spill", &spill, "spill/s");
  tout->Branch("frame", &frame, "frame/i");
  tout->Branch("tref", &tref, "tref/F");
  tout->Branch("trgmask", &trgmask, "trgmask/s");
  tout->Branch("n", &n, "n/s");
  tout->Branch("ch", &ch, "ch[n]/s");
  tout->Branch("x", &x, "x[n]/F");
  tout->Branch("y", &y, "y[n]/F");
  tout->Branch("t", &t, "t[n]/F");

  //  Utility variables
  std::map<int, float> timing_participants_map_time0;
  std::map<int, float> timing_participants_map_time1;

  int n_spills = 0;
  std::cout << "[INFO] Starting processing spill: " << n_spills << std::endl;
  while (io->next_spill())
  {
    std::cout << "[INFO] Processing spill: " << n_spills << std::endl;
    spill = n_spills;

    while (io->next_frame())
    {
      frame = io->get_current_frame_id();

      /** reset event **/
      n = 0;

      /** reference time information **/
      auto Tref = reference_time(io, reference_method);
      if (Tref == -666.)
        continue;
#ifdef TREFCUT
      if (Tref < Tref_min || Tref > Tref_max)
        continue;
#endif
      tref = Tref * sipm4eic::lightdata::coarse_to_ns;

      auto trigger0_vector = io->get_trigger0_vector();
      auto Ttrg0 = trigger0_vector.size() > 0 ? trigger0_vector[0].coarse : -666.;
      auto trigger1_vector = io->get_trigger1_vector();
      auto Ttrg1 = trigger1_vector.size() > 0 ? trigger1_vector[0].coarse : -666.;
      if (Ttrg0 != -666. && Ttrg1 != -666.)
        h_time_dist_trigger->Fill((Ttrg0 - Ttrg1) );

      /** trigger information **/
      trgmask = trigger_mask(io, Tref);

      /** loop over timing hits **/
      auto timing_map = io->get_timing_map();
      for (auto &[idx, hits] : timing_map)
      {

        //  Hit variables
        auto index = hits[0].index;
        auto coarse = hits[0].coarse * sipm4eic::lightdata::coarse_to_ns;
        auto delta = coarse - tref;
        //  Store participants
        if (index < 50)
          timing_participants_map_time0[index] = coarse;
        else
          timing_participants_map_time1[index] = coarse;
      }
      //if (timing_participants_map_time0.size() + timing_participants_map_time1.size() < 63)
     //   continue;

      auto timing_time = 0.;
      auto timing_time_0 = 0.;
      auto timing_time_1 = 0.;
      for (auto &[index, delta] : timing_participants_map_time0)
      {
        timing_time += delta;
        timing_time_0 += delta;
      }
      for (auto &[index, delta] : timing_participants_map_time1)
      {
        timing_time += delta;
        timing_time_1 += delta;
      }
      if (timing_participants_map_time0.size() > 0)
        timing_time_0 /= timing_participants_map_time0.size();
      if (timing_participants_map_time1.size() > 0)
        timing_time_1 /= timing_participants_map_time1.size();
      if (timing_participants_map_time0.size() + timing_participants_map_time1.size() > 0)
        timing_time /= (timing_participants_map_time0.size() + timing_participants_map_time1.size());
      h_time_dist_timing->Fill(timing_time_0 - timing_time_1);
      bool time_coincidence = (std::fabs(timing_time_0 - timing_time_1) < 25.);
      if (time_coincidence)
        h_time_dist_timing_to_ref->Fill(timing_time - tref);
      bool time_ref_coincidence = (std::fabs(timing_time - tref) < 200);

      /** loop over cherenkov hits **/
      auto cherenkov_map = io->get_cherenkov_map();
      for (auto &[idx, hits] : cherenkov_map)
      {
        std::sort(hits.begin(), hits.end());
        auto hit = hits[0];
        auto device = hit.device;
        auto index = hit.index;
        auto chip = index / 32;
        auto time = hit.time();
        auto delta = time - Tref;

        if (fabs(delta+5) > 10.)
          continue;

        auto geo = sipm4eic::get_geo(hit);
        auto pos = sipm4eic::get_position(geo);

        ch[n] = (device - 192) * 256 + index;
        x[n] = pos[0];
        y[n] = pos[1];
        t[n] = delta;
        h_time_dist->Fill(delta);
        h_time_dist_per_spill->Fill(spill, delta * sipm4eic::lightdata::coarse_to_ns);
        h_hit_map->Fill(gRandom->Uniform(pos[0] - 1.5, pos[0] + 1.5), gRandom->Uniform(pos[1] - 1.5, pos[1] + 1.5));
        h_time_dist_timing_to_cherenkov->Fill(time * sipm4eic::lightdata::coarse_to_ns - timing_time);
        if (time_coincidence)
          h_hit_map_timing->Fill(gRandom->Uniform(pos[0] - 1.5, pos[0] + 1.5), gRandom->Uniform(pos[1] - 1.5, pos[1] + 1.5));
        else
          h_hit_map_notiming->Fill(gRandom->Uniform(pos[0] - 1.5, pos[0] + 1.5), gRandom->Uniform(pos[1] - 1.5, pos[1] + 1.5));

        if (fabs(time * sipm4eic::lightdata::coarse_to_ns - timing_time + 4.5) < 7)
          h_hit_map_timing_test->Fill(gRandom->Uniform(pos[0] - 1.5, pos[0] + 1.5), gRandom->Uniform(pos[1] - 1.5, pos[1] + 1.5));

        h_radius_dist->Fill(sqrt(pos[0] * pos[0] + pos[1] * pos[1]));
        if (fabs(delta * sipm4eic::lightdata::coarse_to_ns + 30) < 10)
        {
          h_hit_map_fpeak->Fill(gRandom->Uniform(pos[0] - 1.5, pos[0] + 1.5), gRandom->Uniform(pos[1] - 1.5, pos[1] + 1.5));
          h_radius_dist_fpeak->Fill(sqrt(pos[0] * pos[0] + pos[1] * pos[1]));
        }
        if (fabs(delta * sipm4eic::lightdata::coarse_to_ns - 5) < 15)
        {
          h_hit_map_speak->Fill(gRandom->Uniform(pos[0] - 1.5, pos[0] + 1.5), gRandom->Uniform(pos[1] - 1.5, pos[1] + 1.5));
          h_radius_dist_speak->Fill(sqrt(pos[0] * pos[0] + pos[1] * pos[1]));
        }
        ++n;
      }

      h_participants_map->Fill(timing_participants_map_time0.size(), timing_participants_map_time1.size());

      tout->Fill();
    }
    ++n_spills;
  }

  std::cout << " --- collected " << tout->GetEntries() << " events, " << n_spills << " spills " << std::endl;
  std::cout << " --- output written: " << recodata_outfilename << std::endl;

  fout->cd();
  tout->Write();
  fout->Close();

  TCanvas *c1 = new TCanvas("c1", "c1", 800, 800);
  h_time_dist_trigger->Draw();
  TCanvas *c2 = new TCanvas("c2", "c2", 800, 800);
  h_hit_map->Draw("colz");

  /*
  TCanvas *c3 = new TCanvas("c3", "c3", 800, 800);
  h_hit_map_fpeak->Draw("colz");
  TCanvas *c4 = new TCanvas("c4", "c4", 800, 800);
  h_hit_map_speak->Draw("colz");
  TCanvas *c5 = new TCanvas("c5", "c5", 800, 800);
  h_radius_dist->Draw();
  TCanvas *c6 = new TCanvas("c6", "c6", 800, 800);
  h_radius_dist_fpeak->Draw();
  TCanvas *c7 = new TCanvas("c7", "c7", 800, 800);
  h_radius_dist_speak->Draw();
  TCanvas *c8 = new TCanvas("c8", "c8", 800, 800);
  h_time_dist_per_spill->Draw("colz");
  TCanvas *c9 = new TCanvas("c9", "c9", 800, 800);
  h_participants_map->Draw("colz");

  TCanvas *c10 = new TCanvas("c10", "c10", 800, 800);
  h_hit_map_timing->Draw("colz");
  TCanvas *c11 = new TCanvas("c11", "c11", 800, 800);
  h_hit_map_notiming->Draw("colz");

  TCanvas *c12 = new TCanvas("c12", "c12", 800, 800);
  h_time_dist_timing->Draw();
  TCanvas *c13 = new TCanvas("c13", "c13", 800, 800);
  h_time_dist_timing_to_ref->Draw();

  TCanvas *c14 = new TCanvas("c14", "c14", 800, 800);
  h_time_dist_timing_to_cherenkov->Draw();
  TCanvas *c15 = new TCanvas("c15", "c15", 800, 800);
  h_hit_map_timing_test->Draw("colz");
  */
}

void recowriter(std::string run_name)
{
  recowriter("Data/" + run_name + "/lightdata.root", "Data/" + run_name + "/recodata.root");
}
