#include "../lib/alcor_recodata.h"
#include "recodata_writer.C"

gSystem->Load("alcor_recodata_h.so");

void check_the_tagger(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
  //  Input files
  std::string input_filename = data_repository + "/" + run_name + "/recodata.root";
  TFile *input_file = new TFile(input_filename.c_str());
  if (!input_file || input_file->IsZombie())
  {
    std::cerr << "[WARNING] Could not find recodata, making it" << std::endl;
    recodata_writer(data_repository, run_name, max_frames);
    input_file = new TFile(input_filename.c_str());
    if (!input_file || input_file->IsZombie())
    {
      std::cerr << "[ERROR] Could not open recodata even after making it, exiting" << std::endl;
      return;
    }
  }
  TTree *recodata_tree = (TTree *)input_file->Get("recodata");
  alcor_recodata *recodata = new alcor_recodata();
  recodata->link_to_tree(recodata_tree);
  auto n_frames = recodata_tree->GetEntries();
  auto all_frames = min((int)n_frames, (int)max_frames);

  //  Prepare output file
  TFile *output_file = TFile::Open((data_repository + "/" + run_name + "/check_the_triggers.root").c_str(), "RECREATE");
  // --- General QA
  TH2F *h_check_triggers = new TH2F("h_check_triggers", ";frame_id;triggers", all_frames, 0, all_frames, 5, 0, 5);
  TH2F *h_check_triggers_encode = new TH2F("h_check_triggers_encode", ";frame_id;triggers", all_frames, 0, all_frames, 128, 0, 128);
  std::map<int, TH2F *> h_full_hitmap_per_trg;
  TH2F *h_full_hitmap_tag_no_trg = new TH2F("h_full_hitmap_tag_no_trg", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH2F *h_full_hitmap_tag_no_trg3 = new TH2F("h_full_hitmap_tag_no_trg3", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);

  //  Loop over frames
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current spill
    recodata_tree->GetEntry(i_frame);

    std::map<int, int> encoded_frames_by_trigger;

    if (i_frame % 1000 == 0)
      cout << i_frame << endl;

    //  Triggers
    for (auto trigger : recodata->get_triggers())
    {
      auto current_index = (int)(trigger.index == 100 ? 3 : trigger.index + 1);
      current_index = (int)(trigger.index == 120 ? 4 : trigger.index + 1);
      encoded_frames_by_trigger[i_frame] += encode_bit(current_index);
    }

    for (auto current_hit = 0; current_hit < recodata->get().size(); current_hit++)
    {
      //  Graphic rnd position
      auto hit_x_rnd = recodata->get_hit_x(current_hit) + (_rnd_(_global_gen_) * 3.0 - 1.5);
      auto hit_y_rnd = recodata->get_hit_y(current_hit) + (_rnd_(_global_gen_) * 3.0 - 1.5);

      auto check_trg0_tagger = 0;
      auto check_trg3_tagger = 0;
      for (auto [frame_index, mask] : encoded_frames_by_trigger)
        for (auto trg_index : decode_bits(mask))
        {
          //  Trg_index 1 LucaANDtrg
          //  Trg_index 2 Broad
          //  Trg_index 3 StartOfSpill
          //  Trg_index 4 RingTag

          if (!h_full_hitmap_per_trg.count(trg_index))
            h_full_hitmap_per_trg[trg_index] = new TH2F(Form("h_full_hitmap_per_trg_%i", trg_index), ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
          h_full_hitmap_per_trg[trg_index]->Fill(hit_x_rnd, hit_y_rnd);
          if (trg_index == 1)
            check_trg0_tagger++;
          if (trg_index == 3)
            check_trg3_tagger++;
          if (trg_index == 4)
          {
            check_trg0_tagger++;
            check_trg3_tagger++;
          }
        }
      if (check_trg0_tagger == 2)
        h_full_hitmap_tag_no_trg->Fill(hit_x_rnd, hit_y_rnd);
      if (check_trg3_tagger == 2)
        h_full_hitmap_tag_no_trg3->Fill(hit_x_rnd, hit_y_rnd);
    }
  }

  //  for (auto [frame_index, mask] : encoded_frames_by_trigger)
  //    h_check_triggers_encode->Fill(frame_index, mask);

  new TCanvas();
  h_check_triggers->Draw("COLZ");
  new TCanvas();
  h_check_triggers_encode->Draw("COLZ");
  new TCanvas();
  h_full_hitmap_tag_no_trg->Draw("COLZ");
  new TCanvas();
  h_full_hitmap_tag_no_trg3->Draw("COLZ");

  for (auto [trg_index, histo] : h_full_hitmap_per_trg)
  {
    new TCanvas();
    histo->Draw("COLZ");
    histo->Write();
  }

  //  Close files
  input_file->Close();
  // output_file->Close();
}