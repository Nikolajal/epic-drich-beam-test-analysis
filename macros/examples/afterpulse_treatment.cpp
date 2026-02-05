#pragma cling load("libtest_beam_analysis_dict.dylib");
#pragma cling load("libtest_beam_analysis.dylib");

void afterpulse_treatment(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
  //  Input files
  std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";

  //  Load recodata, return if not available
  TFile *input_file_recodata = new TFile(input_filename_recodata.c_str());
  if (!input_file_recodata || input_file_recodata->IsZombie())
  {
    std::cerr << "[WARNING] Could not find recodata, making it" << std::endl;
    return;
  }

  //  Link recodata tree locally
  TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
  alcor_recodata *recodata = new alcor_recodata();
  recodata->link_to_tree(recodata_tree);

  //  Get number of frames, limited to maximum requested frames
  auto n_frames = recodata_tree->GetEntries();
  auto all_frames = min((int)n_frames, (int)max_frames);

  //  Time distribution
  TH1F *h_t_distribution = new TH1F("h_t_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
  TH1F *h_t_AP_distribution = new TH1F("h_t_AP_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);
  TH1F *h_t_noAP_distribution = new TH1F("h_t_noAP_distribution", ";t_{hit} - t_{timing} (ns)", 200, -312.5, 312.5);

  //  Loop over frames
  auto i_spill = -1;
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current frame
    recodata_tree->GetEntry(i_frame);

    //  _HITMASK_dead_lane signals the event is start of spill, tells which channels are available
    if (decode_bits(recodata->get_hit_mask(0))[0] == _HITMASK_dead_lane)
    {
      //  You can internally keep track of spills
      i_spill++;
      //  This event is not of physical interest
      continue;
    }

    //  Select Luca AND trigger (0) or timing trigger (101)
    auto current_trigger = recodata->get_triggers();
    auto it = std::find_if(current_trigger.begin(), current_trigger.end(), [](const trigger_struct &t)
                           { return t.index == 101; });
    if (it != current_trigger.end())
    {
      //  Loop on hits
      for (auto current_hit = 0; current_hit < recodata->get().size(); current_hit++)
      {
        //  Fill time distribution to check
        h_t_distribution->Fill(recodata->get_hit_t(current_hit) - it->fine_time);

        //  Remove afterpulse
        if (recodata->is_afterpulse(current_hit))
        {
          h_t_AP_distribution->Fill(recodata->get_hit_t(current_hit) - it->fine_time);
          continue;
        }
        else
        {
          h_t_noAP_distribution->Fill(recodata->get_hit_t(current_hit) - it->fine_time);
        }
      }
    }
  }

  TCanvas *c_time_delta = new TCanvas();
  gPad->SetLogy();
  h_t_distribution->SetLineColor(kBlack);
  h_t_distribution->SetLineWidth(2);
  h_t_distribution->SetMinimum(1.);
  h_t_distribution->Draw();
  h_t_AP_distribution->SetLineColor(kRed);
  h_t_AP_distribution->Draw("SAME");
  h_t_noAP_distribution->SetLineColor(kBlue);
  h_t_noAP_distribution->Draw("SAME");
}