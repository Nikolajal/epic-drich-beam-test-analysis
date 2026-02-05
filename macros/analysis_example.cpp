#pragma cling load("libtest_beam_analysis_dict.dylib");
#pragma cling load("libtest_beam_analysis.dylib");

void analysis_example(std::string data_repository, std::string run_name, int max_frames = 10000000)
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

  //  Map of hits
  TH2F *h_xy_coverage_full = new TH2F("h_xy_coverage_full", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
  TH1F *h_t_distribution = new TH1F("h_t_distribution", ";t_{hit} - t_{timing} (ns)", 600, -300, 300);

  //  Loop over frames
  auto i_spill = -1;
  auto n_spils = 0;
  auto used_frames = 0;
  for (int i_frame = 0; i_frame < all_frames; ++i_frame)
  {
    //  Load data for current frame
    recodata_tree->GetEntry(i_frame);

    //  _HITMASK_dead_lane signals the event is start of spill, tells which channels are available
    if (decode_bits(recodata->get_hit_mask(0))[0] == _HITMASK_dead_lane)
    {
      //  You can internally keep track of spills
      i_spill++;
      n_spils++;
    }


    //  Select Luca AND trigger (0) or timing trigger (101)
    auto current_trigger = recodata->get_triggers();
    auto it = std::find_if(current_trigger.begin(), current_trigger.end(), [](const trigger_struct &t)
                           { return t.index == 0; });
    if (it != current_trigger.end())
    {
      //  Keep track of the actual number of frames used in the analysis
      used_frames++;

      //  Loop on hits
      for (auto current_hit = 0; current_hit < recodata->get().size(); current_hit++)
      {
        //  Fill time distribution to check
        h_t_distribution->Fill(recodata->get_hit_t(current_hit) - it->fine_time);

        //  Cut in time to select coincidences
        if (fabs(recodata->get_hit_t(current_hit) - it->fine_time) > 40)
          continue;

        //  Fill a hit wirth a random phi and R within the sensors acceptance (graphical purposes)
        h_xy_coverage_full->Fill(recodata->get_hit_x_rnd(current_hit), recodata->get_hit_y_rnd(current_hit));
      }
    }
  }

  TCanvas *c1 = new TCanvas("hitmap", "", 600, 500);
  h_xy_coverage_full->Draw("COLZ");
  TCanvas *c2 = new TCanvas();
  h_t_distribution->Draw();
}