#include "../lib_loader.h"
/**
 * @file dark_count_rate_run_comparison.cpp
 * @brief Compare dark count rate (DCR) between two runs
 * @details
 *
 * @author Mattia Valenti
 */
//spiega in inglese  tutto quello che fa questo codice. spiega che concettualmente non sto calcolando il dcr nelle regioni con il trigger zero ma che quello è già segnale + rumore. il fatto di valutarlo nella finestra out of time serve per sottrarlo nella finestra in time

// 1. Carica i dati di due run (run_name_1 e run_name_2) da file ROOT
// 2. Per ogni run, itera sui frame e classifica i frame in tre regioni in base al tempo dei hit rispetto al trigger 0:
//    - Trigger 100: frame con trigger 100 attivo (riferimento per il DCR con radiatore)
//    - Trigger 0 out-of-time: frame con trigger 0 attivo ma hit fuori dalla finestra temporale in-time (riferimento per il DCR senza radiatore, contiene segnale + rumore)
//    - Trigger 0 in-time: frame con trigger 0 attivo e hit nella finestra temporale in-time (contiene segnale + rumore, ma con possibile contributo di afterpulse)
// 3. Per ogni regione, conta il numero di hit in ciascun frame e calcola il DCR normalizzato al numero di sensori attivi e alla durata della finestra temporale
// 4. Rimuove gli hit marcati come afterpulse tramite recodata->is_afterpulse(...); per trigger 0 scarta anche i frame con selected_hits=0
// 5. Calcola i DCR medi per regione, stampa i risultati e disegna gli istogrammi di tempo/DCR e il riepilogo finale
enum DCRRegion
{
  TRIG100_DCR_ = 0,
  TRIG0_OUT_OF_TIME = 1,
  TRIG0_IN_TIME = 2
};

void dark_count_rate_run_comparison(
  std::string data_repository = "/Users/mattiavalenti/Desktop/analisi_tesi_magistrale/data",
  std::string run_name_1 = "20251111-164951",
  std::string run_name_2 = "20251118-230049",
  int max_frames = 1000000,
  float trig0_out_of_time_min_run_1 = -100.f,
  float trig0_out_of_time_max_run_1 = -40.f,
  float trig0_out_of_time_min_run_2 = -100.f,
  float trig0_out_of_time_max_run_2 = -40.f,
  float trig0_in_time_min_run_1 = -35.f,
  float trig0_in_time_max_run_1 = 50.f,
  float trig0_in_time_min_run_2 = -40.f,
  float trig0_in_time_max_run_2 = 20.f)
{
  //  Input files
  std::string input_filename_recodata_1 = data_repository + "/" + run_name_1 + "/recodata.root";
  std::string input_filename_recodata_2 = data_repository + "/" + run_name_2 + "/recodata.root";        

    //monte carlo che genra due variabili da 0 a 3200, e ogi volta fa la differenza tra le due variabili e la riempie in un istogramma, fai questo per il numero di entries di h_time_run_w_radiator e disegna l'istogramma alla fine, dovrebbe essere una distribuzione uniforme tra -3200 e 3200, con media intorno a zero
    
    TH1F *h_time_random = new TH1F("h_time_random", "Random time differences;dt [ns];Entries", 2048, -3200, 3200);
    TRandom3 rand_gen(0);
    for (int i = 0; i < 2000000; ++i)
    {
    auto t1 = rand_gen.Uniform(0.f, 3200.f);
    auto t2 = rand_gen.Uniform(0.f, 3200.f);
    h_time_random->Fill(t1 - t2);
    }

    TCanvas *c_random = new TCanvas("c_random", "Random time differences", 800, 600);
        
    
    h_time_random->Scale(1./h_time_random->GetMaximum());
    //  Load recodata, return if not available
    TFile *input_file_recodata_1 = new TFile(input_filename_recodata_1.c_str());
    if (!input_file_recodata_1 || input_file_recodata_1->IsZombie())
    {
        std::cerr << "[WARNING] Could not find recodata for run " << run_name_1 << ", making it" << std::endl;
        return;
    }
    TFile *input_file_recodata_2 = new TFile(input_filename_recodata_2.c_str());
    if (!input_file_recodata_2 || input_file_recodata_2->IsZombie())
    {
        std::cerr << "[WARNING] Could not find recodata for run " << run_name_2 << ", making it" << std::endl;
        return;
    }

    //  Link recodata tree locally
    TTree *recodata_tree_1 = (TTree *)input_file_recodata_1->Get("recodata");
    alcor_recodata *recodata_1 = new alcor_recodata();
    recodata_1->link_to_tree(recodata_tree_1);
    TTree *recodata_tree_2 = (TTree *)input_file_recodata_2->Get("recodata");
    alcor_recodata *recodata_2 = new alcor_recodata();
    recodata_2->link_to_tree(recodata_tree_2);

    //  Get number of frames, limited to maximum requested frames
    auto n_frames_1 = recodata_tree_1->GetEntries();
    auto n_frames_2 = recodata_tree_2->GetEntries();
    auto all_frames_1 = min((int)n_frames_1, (int)max_frames);
    auto all_frames_2 = min((int)n_frames_2, (int)max_frames);

    //  Analysis regions: 0 = trigger 100, 1 = trigger 0 out-of-time, 2 = trigger 0 in-time
    const int n_regions = 3;
    const char *region_labels[n_regions] = {"Trigger 100", "Trigger 0 out-of-time", "Trigger 0 in-time"};

    //  Time distributions (trigger 0 reference)
    TH1F *h_time_run_w_radiator = new TH1F("h_time_run_w_radiator", ("Hit time wrt trigger 0 - " + run_name_1 + ";t_{hit} - t_{trig0} [ns];Entries").c_str(), 2048, -3200, 3200);
    TH1F *h_time_run_wo_radiator = new TH1F("h_time_run_wo_radiator", ("Hit time wrt trigger 0 - " + run_name_2 + ";t_{hit} - t_{trig0} [ns];Entries").c_str(), 2048, -3200, 3200);

    //  DCR histograms (2 per region => 6 total). The x region is the dcr range
    TH1F *h_dcr_run_w_radiator[n_regions];
    TH1F *h_dcr_run_wo_radiator[n_regions];
    for (int i_region = 0; i_region < n_regions; ++i_region)
    {
      int n_bins = 20;
      float x_min = 0.f;
      float x_max = 10.f;
      if (i_region == 1)
      {
        n_bins = 15;
        x_min = 0.f;
        x_max = 30.f;
      }
      if (i_region == 2)
      {
        n_bins = 50;
        x_max = 500.f;
      }

      h_dcr_run_w_radiator[i_region] = new TH1F(Form("h_dcr_run_w_radiator_%d", i_region), Form("%s - %s;Rate [kHz];Entries", region_labels[i_region], run_name_1.c_str()), n_bins, x_min, x_max);
      h_dcr_run_wo_radiator[i_region] = new TH1F(Form("h_dcr_run_wo_radiator_%d", i_region), Form("%s - %s;Rate [kHz];Entries", region_labels[i_region], run_name_2.c_str()), n_bins, x_min, x_max);
    }

    int used_frames_1[n_regions] = {0, 0, 0};
    int used_frames_2[n_regions] = {0, 0, 0};

    int used_frames_1_total = 0;
    int used_frames_2_total = 0;

    //  Window widths for trigger-0 regions (ns)
    auto trig0_out_window_width_run_1 = std::max(1e-6f, trig0_out_of_time_max_run_1 - trig0_out_of_time_min_run_1);
    auto trig0_in_window_width_run_1 = std::max(1e-6f, trig0_in_time_max_run_1 - trig0_in_time_min_run_1);
    auto trig0_out_window_width_run_2 = std::max(1e-6f, trig0_out_of_time_max_run_2 - trig0_out_of_time_min_run_2);
    auto trig0_in_window_width_run_2 = std::max(1e-6f, trig0_in_time_max_run_2 - trig0_in_time_min_run_2);

    std::cout << "Run " << run_name_1 << ": trigger 0 out-of-time window width = " << trig0_out_window_width_run_1 << " ns, trigger 0 in-time window width = " << trig0_in_window_width_run_1 << " ns" << std::endl;
    std::cout << "Run " << run_name_2 << ": trigger 0 out-of-time window width = " << trig0_out_window_width_run_2 << " ns, trigger 0 in-time window width = " << trig0_in_window_width_run_2 << " ns" << std::endl;


    long long removed_afterpulse_hits_1 = 0;
    long long removed_afterpulse_hits_2 = 0;

    //  Loop over frames for run 1
    std::set<uint32_t> active_sensors_1;
    for (int i_frame_1 = 0; i_frame_1 < all_frames_1; ++i_frame_1)
    {
      recodata_tree_1->GetEntry(i_frame_1);

      if (recodata_1->is_start_of_spill())
      {
        active_sensors_1.clear();
        for (const auto &hit : recodata_1->get_recodata())
          active_sensors_1.insert(hit.global_index);
        continue;
      }

      if (active_sensors_1.empty())
        continue;

      bool use_region[n_regions] = {false, false, false};
      float selected_hits[n_regions] = {0.0, 0.0, 0.0};

      if (recodata_1->is_first_frames())
      {
        use_region[0] = true;
        selected_hits[0] = 0;
        for (int current_hit = 0; current_hit < recodata_1->get_recodata().size(); ++current_hit)
          if (!recodata_1->is_afterpulse(current_hit))
            selected_hits[0]++;
          else
            removed_afterpulse_hits_1++;
      }

      auto trigger0_1 = recodata_1->get_trigger_by_index(0);
      if (trigger0_1)
      {
        use_region[1] = true;
        use_region[2] = true;

        for (int current_hit = 0; current_hit < recodata_1->get_recodata().size(); ++current_hit)
        {
          if (recodata_1->is_afterpulse(current_hit))
          {
            removed_afterpulse_hits_1++;
            continue;
          }

          auto dt = recodata_1->get_hit_t(current_hit) - trigger0_1->fine_time;
          h_time_run_w_radiator->Fill(dt);

          if (dt >= trig0_out_of_time_min_run_1 && dt <= trig0_out_of_time_max_run_1)
            selected_hits[1]+=1./h_time_random->GetBinContent(h_time_random->FindBin(dt));
          if (dt >= trig0_in_time_min_run_1 && dt <= trig0_in_time_max_run_1)
            selected_hits[2]+=1./h_time_random->GetBinContent(h_time_random->FindBin(dt));
        }
      }

      used_frames_1_total++;

      for (int i_region = 0; i_region < n_regions; ++i_region)
      {
        if (!use_region[i_region])
          continue;

        //if ((i_region != TRIG100_DCR_) && (selected_hits[i_region] == 0))
        //  continue;

        used_frames_1[i_region]++;
        double frame_dcr = 0.;
        if (i_region == 0)
        {
          frame_dcr = selected_hits[i_region] * 1. / (_FRAME_LENGTH_NS_ * 1.e-6 * active_sensors_1.size());
          //cout << "Run " << run_name_1 << " - " << region_labels[i_region] << ": frame " << i_frame_1 << ", selected hits = " << selected_hits[i_region] << ", active sensors = " << active_sensors_1.size() << ", frame DCR = " << frame_dcr << " kHz" << std::endl;
        }  
        if (i_region == 1)
        {
          frame_dcr = selected_hits[i_region] * 1. / (trig0_out_window_width_run_1 * 1.e-6 * active_sensors_1.size());
          //cout << "Run " << run_name_1 << " - " << region_labels[i_region] << ": frame " << i_frame_1 << ", selected hits = " << selected_hits[i_region] << ", active sensors = " << active_sensors_1.size() << ", frame DCR = " << frame_dcr << " kHz" << std::endl;
        } 
        if (i_region == 2)
        {
          frame_dcr = selected_hits[i_region] * 1. / (trig0_in_window_width_run_1 * 1.e-6 * active_sensors_1.size());
          //cout << "Run " << run_name_1 << " - " << region_labels[i_region] << ": frame " << i_frame_1 << ", selected hits = " << selected_hits[i_region] << ", active sensors = " << active_sensors_1.size() << ", frame DCR = " << frame_dcr << " kHz" << std::endl;   
        }
        h_dcr_run_w_radiator[i_region]->Fill(frame_dcr);
      }
    }

    //  Loop over frames for run 2
    std::set<uint32_t> active_sensors_2;
    for (int i_frame_2 = 0; i_frame_2 < all_frames_2; ++i_frame_2)
    {
      recodata_tree_2->GetEntry(i_frame_2);

      if (recodata_2->is_start_of_spill())
      {
        active_sensors_2.clear();
        for (const auto &hit : recodata_2->get_recodata())
          active_sensors_2.insert(hit.global_index);
        continue;
      }

      if (active_sensors_2.empty())
        continue;

      bool use_region[n_regions] = {false, false, false};
      int selected_hits[n_regions] = {0, 0, 0};

      if (recodata_2->is_first_frames())
      {
        use_region[0] = true;
        selected_hits[0] = 0;
        for (int current_hit = 0; current_hit < recodata_2->get_recodata().size(); ++current_hit)
          if (!recodata_2->is_afterpulse(current_hit))
            selected_hits[0]++;
          else
            removed_afterpulse_hits_2++;
      }

      auto trigger0_2 = recodata_2->get_trigger_by_index(0);
      if (trigger0_2)
      {
        use_region[1] = true;
        use_region[2] = true;

        for (int current_hit = 0; current_hit < recodata_2->get_recodata().size(); ++current_hit)
        {
          if (recodata_2->is_afterpulse(current_hit))
          {
            removed_afterpulse_hits_2++;
            continue;
          }

          auto dt = recodata_2->get_hit_t(current_hit) - trigger0_2->fine_time;
          h_time_run_wo_radiator->Fill(dt);

          if (dt >= trig0_out_of_time_min_run_2 && dt <= trig0_out_of_time_max_run_2)
            selected_hits[1]+=1./h_time_random->GetBinContent(h_time_random->FindBin(dt));
          if (dt >= trig0_in_time_min_run_2 && dt <= trig0_in_time_max_run_2)
            selected_hits[2]+=1./h_time_random->GetBinContent(h_time_random->FindBin(dt));
        }
      }

      used_frames_2_total++;

      for (int i_region = 0; i_region < n_regions; ++i_region)
      {
        if (!use_region[i_region])
          continue;

        //if ((i_region != TRIG100_DCR_) && (selected_hits[i_region] == 0))
        //  continue;

        used_frames_2[i_region]++;
        double frame_dcr = 0.;
        if (i_region == 0)
          frame_dcr = selected_hits[i_region] * 1. / (_FRAME_LENGTH_NS_ * 1.e-6 * active_sensors_2.size());
        if (i_region == 1)
          frame_dcr = selected_hits[i_region] * 1. / (trig0_out_window_width_run_2 * 1.e-6 * active_sensors_2.size());
        if (i_region == 2)
          frame_dcr = selected_hits[i_region] * 1. / (trig0_in_window_width_run_2 * 1.e-6 * active_sensors_2.size());
        h_dcr_run_wo_radiator[i_region]->Fill(frame_dcr);
      }
    }


    h_time_random->Draw();
    //prendi ora un intervallo da -200 a -600 e fai un l'integrale sia per h_time_run_w_radiator e per h_time_random
    //  Normalisation (same logic as current script)
    for (int i_region = 0; i_region < n_regions; ++i_region)
    {
      if (used_frames_1[i_region] > 0)
        h_dcr_run_w_radiator[i_region]->Scale(1. / used_frames_1[i_region]);
      if (used_frames_2[i_region] > 0)
        h_dcr_run_wo_radiator[i_region]->Scale(1. / used_frames_2[i_region]);
    }

    //  Print summary
    std::cout << "Run " << run_name_1 << " - afterpulse hits removed = " << removed_afterpulse_hits_1 << std::endl;
    std::cout << "Run " << run_name_2 << " - afterpulse hits removed = " << removed_afterpulse_hits_2 << std::endl;
    for (int i_region = 0; i_region < n_regions; ++i_region)
    {
      std::cout << "Run " << run_name_1 << " - " << region_labels[i_region] << ": " << used_frames_1[i_region] << " frames used, DCR mean = " << h_dcr_run_w_radiator[i_region]->GetMean() << " kHz" << " +/- " << h_dcr_run_w_radiator[i_region]->GetRMS() / std::sqrt(h_dcr_run_w_radiator[i_region]->GetEntries()) << std::endl;
      std::cout << "Run " << run_name_2 << " - " << region_labels[i_region] << ": " << used_frames_2[i_region] << " frames used, DCR mean = " << h_dcr_run_wo_radiator[i_region]->GetMean() << " kHz" << " +/- " << h_dcr_run_wo_radiator[i_region]->GetRMS() / std::sqrt(h_dcr_run_wo_radiator[i_region]->GetEntries()) << std::endl;
    }

    //  Canvas 1: time distributions for the two runs
    TCanvas *c_time = new TCanvas("c_time", "Hit time distributions", 900, 600);
    h_time_run_w_radiator->SetLineColor(kBlue);
    h_time_run_wo_radiator->SetLineColor(kRed);
    h_time_run_w_radiator->SetLineWidth(2);
    h_time_run_wo_radiator->SetLineWidth(2);

    h_time_run_w_radiator->Scale(1./used_frames_1_total);
    h_time_run_wo_radiator->Scale(1./used_frames_2_total);

    h_time_run_w_radiator->Divide(h_time_random);
    h_time_run_wo_radiator->Divide(h_time_random);

    h_time_run_w_radiator->Draw("HIST");
    h_time_run_wo_radiator->Draw("HIST SAME");

    TLegend *leg_time = new TLegend(0.62, 0.72, 0.88, 0.88);
    leg_time->SetBorderSize(0);
    leg_time->AddEntry(h_time_run_w_radiator, run_name_1.c_str(), "l");
    leg_time->AddEntry(h_time_run_wo_radiator, run_name_2.c_str(), "l");
    leg_time->Draw();

    //  Canvas 2-4: DCR histograms, one canvas per region (2 hist each)
    for (int i_region = 0; i_region < n_regions; ++i_region)
    {
      TCanvas *c_region = new TCanvas(Form("c_dcr_%d", i_region), Form("DCR comparison - %s", region_labels[i_region]), 900, 600);
      h_dcr_run_w_radiator[i_region]->SetLineColor(kBlue);
      h_dcr_run_wo_radiator[i_region]->SetLineColor(kRed);
      h_dcr_run_w_radiator[i_region]->SetLineWidth(2);
      h_dcr_run_wo_radiator[i_region]->SetLineWidth(2);
      h_dcr_run_w_radiator[i_region]->Draw("HIST");
      h_dcr_run_wo_radiator[i_region]->Draw("HIST SAME");

      TLegend *leg_region = new TLegend(0.58, 0.72, 0.88, 0.88);
      leg_region->SetBorderSize(0);
      leg_region->AddEntry(h_dcr_run_w_radiator[i_region], run_name_1.c_str(), "l");
      leg_region->AddEntry(h_dcr_run_wo_radiator[i_region], run_name_2.c_str(), "l");
      leg_region->Draw();
      c_region->Update();
    }

    //  Final summary graph: y = DCR, x = region
    TH1F *h_summary_1 = new TH1F("h_summary_1", "Summary DCR;Region;Rate[kHz]", n_regions, 0.5, n_regions + 0.5);
    TH1F *h_summary_2 = new TH1F("h_summary_2", "Summary DCR;Region;Rate[kHz]", n_regions, 0.5, n_regions + 0.5);
    for (int i_region = 0; i_region < n_regions; ++i_region)
    {
      h_summary_1->GetXaxis()->SetBinLabel(i_region + 1, region_labels[i_region]);
      h_summary_2->GetXaxis()->SetBinLabel(i_region + 1, region_labels[i_region]);

      h_summary_1->SetBinContent(i_region + 1, h_dcr_run_w_radiator[i_region]->GetMean());
      h_summary_2->SetBinContent(i_region + 1, h_dcr_run_wo_radiator[i_region]->GetMean());

      auto err1 = (h_dcr_run_w_radiator[i_region]->GetEntries() > 0) ? h_dcr_run_w_radiator[i_region]->GetRMS() / std::sqrt(h_dcr_run_w_radiator[i_region]->GetEntries()) : 0.;
      auto err2 = (h_dcr_run_wo_radiator[i_region]->GetEntries() > 0) ? h_dcr_run_wo_radiator[i_region]->GetRMS() / std::sqrt(h_dcr_run_wo_radiator[i_region]->GetEntries()) : 0.;
      h_summary_1->SetBinError(i_region + 1, err1);
      h_summary_2->SetBinError(i_region + 1, err2);
    }

    TCanvas *c_summary = new TCanvas("c_summary", "DCR summary by region", 900, 600);
    h_summary_1->SetLineColor(kBlue);
    h_summary_1->SetMarkerColor(kBlue);
    h_summary_1->SetMarkerStyle(20);
    h_summary_2->SetLineColor(kRed);
    h_summary_2->SetMarkerColor(kRed);
    h_summary_2->SetMarkerStyle(21);
    h_summary_1->SetStats(0);
    h_summary_2->SetStats(0);
    h_summary_1->Draw("E1");
    h_summary_2->Draw("E1 SAME");
    c_summary->SetGridy();
    c_summary->SetLogy();

    TLegend *leg_summary = new TLegend(0.62, 0.74, 0.88, 0.88);
    leg_summary->SetBorderSize(0);
    leg_summary->AddEntry(h_summary_1, run_name_1.c_str(), "lp");
    leg_summary->AddEntry(h_summary_2, run_name_2.c_str(), "lp");
    leg_summary->Draw();
}   

