#include "../lib_loader.h"
#include "ringtrack_config.h"

void ringtrack_draw(std::string data_repository, std::string run_name,
                    std::string conf_path = "ringtrack.conf",
                    std::string output_dir = "")
{
    RingtrackConfig cfg;
    cfg.load(conf_path);

    if (output_dir.empty())
        {
        TString _repo = gSystem->DirName(gSystem->DirName(gSystem->DirName(__FILE__)));
        TDatime _now;
        TString _dt = Form("%04d%02d%02d_%02d%02d%02d", _now.GetYear(), _now.GetMonth(), _now.GetDay(), _now.GetHour(), _now.GetMinute(), _now.GetSecond());
        output_dir = std::string(_repo.Data()) + "/plots/" + run_name + "/" + std::string(_dt.Data());
    }

    std::string input_root = output_dir + "/histograms.root";

    const bool  apply_theta_phi_cut = cfg.get_bool("apply_theta_phi_cut", false);
    const bool  apply_angle_xy_cut  = cfg.get_bool("apply_angle_xy_cut",  false);
    const float theta_min    = cfg.get_float("theta_min",    0.f);
    const float theta_max    = cfg.get_float("theta_max",    0.1f);
    const float phi_min      = cfg.get_float("phi_min",     -3.1415f);
    const float phi_max      = cfg.get_float("phi_max",      3.1415f);
    const float angle_x_min  = cfg.get_float("angle_x_min", -0.05f);
    const float angle_x_max  = cfg.get_float("angle_x_max",  0.05f);
    const float angle_y_min  = cfg.get_float("angle_y_min", -0.05f);
    const float angle_y_max  = cfg.get_float("angle_y_max",  0.05f);

    TFile *f = new TFile(input_root.c_str(), "READ");
    if (!f || f->IsZombie())
    {
        std::cerr << "[ERROR] Could not open " << input_root << std::endl;
        return;
    }

    TH1::AddDirectory(false);
    auto get = [&](const char *name) -> TObject * { return f->Get(name); };

    TH1F *h_t_distribution               = (TH1F *)get("h_t_distribution");
    TH1F *h_t_distribution_track         = (TH1F *)get("h_t_distribution_track");
    TH1F *h_t_distribution_selected      = (TH1F *)get("h_t_distribution_selected");
    TH1F *h_tracking_theta               = (TH1F *)get("h_tracking_theta");
    TH1F *h_tracking_phi                 = (TH1F *)get("h_tracking_phi");
    TH1F *h_tracking_angle_x             = (TH1F *)get("h_tracking_angle_x");
    TH1F *h_tracking_angle_y             = (TH1F *)get("h_tracking_angle_y");
    TH2F *h_intercept_drich              = (TH2F *)get("h_intercept_drich");
    TH2F *h_intercept_scint              = (TH2F *)get("h_intercept_scint");
    TH2F *h_hit_xy_map                   = (TH2F *)get("h_hit_xy_map");
    TH1F *h_n_selected_hits              = (TH1F *)get("h_n_selected_hits");
    TH1F *h_all_n_hits                   = (TH1F *)get("h_all_n_hits");
    TH2F *h_n_selected_hits_vs_multiplicity = (TH2F *)get("h_n_selected_hits_vs_multiplicity");
    TH2F *h_n_selected_hits_vs_theta     = (TH2F *)get("h_n_selected_hits_vs_theta");
    TH2F *h_n_selected_hits_vs_ix_drich  = (TH2F *)get("h_n_selected_hits_vs_ix_drich");
    TH2F *h_n_selected_hits_vs_iy_drich  = (TH2F *)get("h_n_selected_hits_vs_iy_drich");
    TH1F *h_track_multiplicity           = (TH1F *)get("h_track_multiplicity");

    f->Close();

    gROOT->SetBatch(true);

    auto draw_with_range = [&](TH1F *h, bool highlight, float xlo, float xhi)
    {
        h->SetLineColor(kBlack); h->SetLineWidth(2); h->Draw();
        if (!highlight) return;
        double ymax = h->GetMaximum() * 1.05;
        TBox *box = new TBox(xlo, 0, xhi, ymax);
        box->SetFillColorAlpha(kRed, 0.25);
        box->SetLineWidth(0);
        box->Draw("SAME");
    };

    // =========================================================================
    //  CANVAS
    // =========================================================================

    // --- hit time: totale vs selezionati ---
    TCanvas *c_time_delta = new TCanvas("c_time_delta", "Hit time wrt trigger", 800, 600);
    gPad->SetLogy();
    {
        TH1F *h1 = (TH1F *)h_t_distribution->Clone("h_t_all");
        TH1F *h2 = (TH1F *)h_t_distribution_selected->Clone("h_t_sel");
        h1->SetLineColor(kBlack); h1->SetLineWidth(2);
        h2->SetLineColor(kRed);   h2->SetLineWidth(2); h2->SetStats(0);
        h1->SetTitle("Hit time wrt trigger;t_{hit} - t_{timing} (ns);counts");
        h1->Draw("HIST"); h2->Draw("HIST SAME");

        std::string active_cuts = "selected";
        if (cfg.get_bool("require_single_track", false))  active_cuts += " | single track";
        if (cfg.get_bool("require_multi_track",  false))  active_cuts += " | multi track";
        if (cfg.get_string("plane1", "NONE") != "NONE")
            active_cuts += " | " + cfg.get_string("plane1") + " " + cfg.get_string("side1");
        if (cfg.get_string("plane2", "NONE") != "NONE")
            active_cuts += " | " + cfg.get_string("plane2") + " " + cfg.get_string("side2");
        if (cfg.get_bool("apply_chi2_cut", false))
            active_cuts += Form(" | chi2/ndf < %.1f", cfg.get_float("chi2_max", 5.f));
        if (cfg.get_bool("apply_theta_phi_cut", false))   active_cuts += " | #theta#phi cut";
        if (cfg.get_bool("apply_angle_xy_cut",  false))   active_cuts += " | angle xy cut";
        if (cfg.get_bool("apply_window_selection", false))
            active_cuts += Form(" | win[%.0f,%.0f]>=%d",
                cfg.get_float("window_sel_min", -65.f),
                cfg.get_float("window_sel_max", -25.f),
                cfg.get_int("window_sel_threshold", 3));
        if (cfg.get_bool("apply_window_veto", false))
            active_cuts += Form(" | veto[%.0f,%.0f]<%d",
                cfg.get_float("window_veto_min", -65.f),
                cfg.get_float("window_veto_max", -25.f),
                cfg.get_int("window_veto_threshold", 3));
        if (cfg.get_bool("apply_window_veto_2", false))
            active_cuts += Form(" | veto2[%.0f,%.0f]<%d",
                cfg.get_float("window_veto_2_min",  80.f),
                cfg.get_float("window_veto_2_max", 120.f),
                cfg.get_int("window_veto_2_threshold", 3));
        if (cfg.get_bool("apply_afterpulse_cut", true))   active_cuts += " | no afterpulse";
        if (cfg.get_bool("apply_radial_cut",    false))   active_cuts += " | radial cut";
        if (cfg.get_bool("require_all_tracks_pass_geometric_cuts", false))
            active_cuts += " | all tracks pass geo";

        TLegend *leg = new TLegend(0.12, 0.75, 0.88, 0.88);
        leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.028);
        leg->AddEntry(h1, "all hits", "l");
        leg->AddEntry(h2, active_cuts.c_str(), "l");
        leg->Draw();
    }

    // --- track multiplicity ---
    TCanvas *c_multiplicity = new TCanvas("c_multiplicity", "Track multiplicity", 1000, 600);
    c_multiplicity->Divide(2, 1);
    c_multiplicity->cd(1);
    h_track_multiplicity->Draw();
    c_multiplicity->cd(2);
    {
        int n_triggered = (int)h_track_multiplicity->GetEntries();
        int n_no_track  = (int)h_track_multiplicity->GetBinContent(1);
        int n_single    = (int)h_track_multiplicity->GetBinContent(2);
        int n_multi_ev  = n_triggered - n_no_track - n_single;
        int n_total_trk = n_single;
        for (int b = 3; b <= h_track_multiplicity->GetNbinsX() + 1; b++)
            n_total_trk += (b-1) * (int)h_track_multiplicity->GetBinContent(b);
        int n_multi_trk = n_total_trk - n_single;

        auto pct_ev  = [&](int n) { return n_triggered > 0 ? 100.*n/n_triggered : 0.; };
        auto pct_trk = [&](int n) { return n_total_trk > 0 ? 100.*n/n_total_trk : 0.; };

        TPaveText *pt = new TPaveText(0.03, 0.03, 0.97, 0.97, "NDC");
        pt->SetTextAlign(12); pt->SetTextFont(42); pt->SetTextSize(0.042);
        pt->SetFillColor(0); pt->SetBorderSize(1);

        pt->AddText("Track multiplicity statistics");
        pt->AddText("-------------------------------------");
        pt->AddText(Form("Triggered events         %7d", n_triggered));
        pt->AddText("-------------------------------------");
        pt->AddText(Form("w/o tracks               %7d   (%5.1f%% of ev.)", n_no_track, pct_ev(n_no_track)));
        pt->AddText("-------------------------------------");
        pt->AddText(Form("Single-track events      %7d   (%5.1f%% of ev.)", n_single,   pct_ev(n_single)));
        pt->AddText(Form("                                    (%5.1f%% of trk.)", pct_trk(n_single)));
        pt->AddText("-------------------------------------");
        pt->AddText(Form("Multi-track events       %7d   (%5.1f%% of ev.)", n_multi_ev, pct_ev(n_multi_ev)));
        pt->AddText(Form("Multi-track tracks       %7d   (%5.1f%% of trk.)", n_multi_trk, pct_trk(n_multi_trk)));
        pt->AddText("-------------------------------------");
        pt->AddText(Form("Total tracks             %7d", n_total_trk));
        pt->Draw();
    }

    // --- tracking angles ---
    TCanvas *c_tracking_angles = new TCanvas("c_tracking_angles", "Tracking angles", 2400, 600);
    c_tracking_angles->Divide(4, 1);
    c_tracking_angles->cd(1); draw_with_range(h_tracking_theta,   apply_theta_phi_cut, theta_min,   theta_max);
    c_tracking_angles->cd(2); draw_with_range(h_tracking_phi,     apply_theta_phi_cut, phi_min,     phi_max);
    c_tracking_angles->cd(3); draw_with_range(h_tracking_angle_x, apply_angle_xy_cut,  angle_x_min, angle_x_max);
    c_tracking_angles->cd(4); draw_with_range(h_tracking_angle_y, apply_angle_xy_cut,  angle_y_min, angle_y_max);

    // --- intercept maps 2x2 (colz + scat for dRICH and scint) ---
    TCanvas *c_intercepts = new TCanvas("c_intercepts", "Track intercept maps", 1600, 1600);
    c_intercepts->Divide(2, 2);
    c_intercepts->cd(1); h_intercept_drich->Draw("COLZ");
    c_intercepts->cd(2); h_intercept_scint->Draw("COLZ");
    c_intercepts->cd(3); ((TH2F *)h_intercept_drich->Clone())->Draw("SCAT");
    c_intercepts->cd(4); ((TH2F *)h_intercept_scint->Clone())->Draw("SCAT");

    // --- hit map ---
    TCanvas *c_hit_map = new TCanvas("c_hit_map", "Hit map", 800, 700);
    h_hit_xy_map->Draw("COLZ");

    // --- hit map 3D ---
    TCanvas *c_hit_map_3d = new TCanvas("c_hit_map_3d", "Hit map 3D", 800, 700);
    gPad->SetLogz();
    {
        TH2F *h3d = (TH2F *)h_hit_xy_map->Clone("h_hit_map_3d");
        h3d->SetTitle("Selected hits on detector plane;x (mm);y (mm);counts");
        h3d->Draw("SURF2Z");
    }

    // --- hit multiplicity: all events vs with track ---
    TCanvas *c_hit_mult = new TCanvas("c_hit_mult", "Hit multiplicity comparison", 900, 600);
    {
        TH1F *h1 = (TH1F *)h_all_n_hits->Clone("h_all_clone");
        TH1F *h2 = (TH1F *)h_n_selected_hits->Clone("h_sel_clone");

        h1->SetLineColor(kBlack); h1->SetLineWidth(2);
        h2->SetLineColor(kRed);   h2->SetLineWidth(2); h2->SetStats(0);

        h1->SetTitle("Hit multiplicity per event;n hits;counts");
        h1->Draw("HIST");
        h2->Draw("HIST SAME");

        TLegend *leg = new TLegend(0.55, 0.75, 0.88, 0.88);
        leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.035);
        leg->AddEntry(h1, "all events", "l");
        leg->AddEntry(h2, "with track", "l");
        leg->Draw();
    }

    // --- photon yield: n_hits + multiplicity ---
    TCanvas *c_photon_yield = new TCanvas("c_photon_yield", "Photon yield", 1200, 600);
    c_photon_yield->Divide(2, 1);
    c_photon_yield->cd(1); h_n_selected_hits->Draw();
    c_photon_yield->cd(2); h_n_selected_hits_vs_multiplicity->Draw("SCAT");

    // --- selected hits vs theta e phi ---
    TCanvas *c_nhits_angles = new TCanvas("c_nhits_angles",
        "Selected hits vs angles", 1200, 600);
    c_nhits_angles->Divide(2, 1);
    c_nhits_angles->cd(1); h_n_selected_hits_vs_theta->Draw("SCAT");
    c_nhits_angles->cd(2);
    {
        TH2F *h_phi = (TH2F *)TFile::Open((output_dir + "/histograms.root").c_str())->Get("h_n_selected_hits_vs_phi");
        if (h_phi) h_phi->Draw("SCAT");
        else h_n_selected_hits_vs_theta->Draw("SCAT");
    }

    // --- selected hits vs chi2/NDF (key diagnostic: fake tracks have high chi2 + 0 hits) ---
    TCanvas *c_nhits_chi2 = new TCanvas("c_nhits_chi2",
        "N dRICH hits vs track #chi^{2}/NDF", 900, 700);
    {
        TH2F *hc = (TH2F *)input_file->Get("h_n_selected_hits_vs_chi2");
        if (hc) { hc->Draw("COLZ"); if (hc->GetEntries() > 0) gPad->SetLogz(1); }
    }

    // --- selected hits vs intercepts ix e iy ---
    TCanvas *c_nhits_intercepts = new TCanvas("c_nhits_intercepts",
        "Selected hits vs intercepts", 1200, 600);
    c_nhits_intercepts->Divide(2, 1);
    c_nhits_intercepts->cd(1); h_n_selected_hits_vs_ix_drich->Draw("SCAT");
    c_nhits_intercepts->cd(2); h_n_selected_hits_vs_iy_drich->Draw("SCAT");

    // --- proiezioni selected hits vs intercepts ---
    TCanvas *c_nhits_intercepts_proj = new TCanvas("c_nhits_intercepts_proj",
        "Selected hits vs intercepts - projections", 1200, 600);
    c_nhits_intercepts_proj->Divide(2, 1);
    c_nhits_intercepts_proj->cd(1);
    ((TH1F *)h_n_selected_hits_vs_ix_drich->ProjectionX())->Draw(); gPad->SetLogy();
    c_nhits_intercepts_proj->cd(2);
    ((TH1F *)h_n_selected_hits_vs_iy_drich->ProjectionX())->Draw(); gPad->SetLogy();

    // --- settings ---
    TCanvas *c_settings = new TCanvas("c_settings", "Analysis settings", 600, 700);
    TPaveText *pt = new TPaveText(0.03, 0.03, 0.97, 0.97, "NDC");
    pt->SetTextAlign(12); pt->SetTextFont(42); pt->SetTextSize(0.028);
    pt->SetFillColor(0); pt->SetBorderSize(1);
    for (auto &kv : cfg._data)
        pt->AddText(Form("%-28s = %s", kv.first.c_str(), kv.second.c_str()));
    pt->AddText(Form("run:                         %s", run_name.c_str()));
    pt->Draw();

    // =========================================================================
    //  SAVE
    // =========================================================================
    std::string output_root = output_dir + "/plots.root";
    gSystem->mkdir(output_dir.c_str(), true);
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");
    for (auto obj : *gROOT->GetListOfCanvases())
    {
        TCanvas *c = (TCanvas *)obj;
        c->SaveAs(Form("%s/%s.png", output_dir.c_str(), c->GetName()));
        c->Write();
    }
    fout->Close();
    cout << "Output: " << output_root << endl;
}