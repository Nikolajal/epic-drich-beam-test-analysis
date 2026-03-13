#include "../lib_loader.h"

// ---------------------------------------------------------------------------
//  fit_histogram: adaptive-binning fit along X slices of a TH2
//  Returns a TGraphErrors of fit_function param[param_to_draw] vs X
// ---------------------------------------------------------------------------
TGraphErrors *fit_histogram(TH2 *input_histogram, TF1 *fit_function,
                            int entries_threshold, int param_to_draw)
{
    TGraphErrors *g = new TGraphErrors();
    TH1D *temp_storage = (TH1D *)(input_histogram->ProjectionY("temp_storage", 0, 0)->Clone());
    temp_storage->Reset();
    float bin_center       = 0;
    float bin_width        = 0;
    int   cumulated_entries = 0;

    auto try_fit_and_flush = [&]()
    {
        if (cumulated_entries < entries_threshold) return;
        temp_storage->Fit(fit_function, "Q");
        int n = g->GetN();
        g->SetPoint(n, bin_center + 0.5 * bin_width, fit_function->GetParameter(param_to_draw));
        g->SetPointError(n, 0.5 * bin_width, fit_function->GetParError(param_to_draw));
        temp_storage->Reset();
        bin_width          = 0;
        cumulated_entries  = 0;
    };

    for (int xb = 1; xb <= input_histogram->GetNbinsX(); xb++)
    {
        auto current_projection = input_histogram->ProjectionY("_py", xb, xb);
        if (cumulated_entries == 0)
            bin_center = input_histogram->GetXaxis()->GetBinLowEdge(xb);
        cumulated_entries += current_projection->GetEntries();
        temp_storage->Add(current_projection);
        bin_width += input_histogram->GetXaxis()->GetBinWidth(xb);
        try_fit_and_flush();
    }
    return g;
}

// ---------------------------------------------------------------------------

void ringtrack_draw(std::string data_repository, std::string run_name)
{
    std::string input_root = data_repository + "/" + run_name + "/plots/histograms.root";
    std::string output_dir = data_repository + "/" + run_name + "/plots";

    TFile *f = new TFile(input_root.c_str(), "READ");
    if (!f || f->IsZombie())
    {
        std::cerr << "[ERROR] Could not open " << input_root << std::endl;
        return;
    }

    // =========================================================================
    //  READ SETTINGS
    // =========================================================================
    auto read_setting = [&](const char *key) -> std::string
    {
        TNamed *n = (TNamed *)f->Get(key);
        return n ? std::string(n->GetTitle()) : "N/A";
    };

    std::string plane1                 = read_setting("plane1");
    std::string side1                  = read_setting("side1");
    std::string cutg1_title            = read_setting("cutg1_title");
    std::string plane2                 = read_setting("plane2");
    std::string side2                  = read_setting("side2");
    std::string cutg2_title            = read_setting("cutg2_title");
    std::string require_single_track   = read_setting("require_single_track");
    std::string require_multi_track    = read_setting("require_multi_track");
    std::string apply_multiplicity_cut = read_setting("apply_multiplicity_cut");
    std::string apply_radial_cut       = read_setting("apply_radial_cut");
    std::string apply_theta_phi_cut    = read_setting("apply_theta_phi_cut");
    std::string apply_angle_xy_cut     = read_setting("apply_angle_xy_cut");
    std::string theta_min              = read_setting("theta_min");
    std::string theta_max              = read_setting("theta_max");
    std::string phi_min                = read_setting("phi_min");
    std::string phi_max                = read_setting("phi_max");
    std::string angle_x_min            = read_setting("angle_x_min");
    std::string angle_x_max            = read_setting("angle_x_max");
    std::string angle_y_min            = read_setting("angle_y_min");
    std::string angle_y_max            = read_setting("angle_y_max");

    // parse numeric values for highlight boxes (before f->Close())
    auto safe_stof = [](const std::string &s, float fallback = 0.f) -> float {
        try { return std::stof(s); } catch (...) { return fallback; }
    };

    bool do_theta_phi_highlight = (apply_theta_phi_cut == "1");
    bool do_angle_xy_highlight  = (apply_angle_xy_cut  == "1");
    float f_theta_min   = safe_stof(theta_min);
    float f_theta_max   = safe_stof(theta_max,    0.1f);
    float f_phi_min     = safe_stof(phi_min,      -3.1415f);
    float f_phi_max     = safe_stof(phi_max,       3.1415f);
    float f_angle_x_min = safe_stof(angle_x_min, -0.05f);
    float f_angle_x_max = safe_stof(angle_x_max,  0.05f);
    float f_angle_y_min = safe_stof(angle_y_min, -0.05f);
    float f_angle_y_max = safe_stof(angle_y_max,  0.05f);

    // =========================================================================
    //  READ HISTOGRAMS
    // =========================================================================
    TH1::AddDirectory(false);
    auto get = [&](const char *name) -> TObject * { return f->Get(name); };

    TH1F *h_t_distribution               = (TH1F *)get("h_t_distribution");
    TH1F *h_first_round_X                = (TH1F *)get("h_first_round_X");
    TH1F *h_first_round_Y                = (TH1F *)get("h_first_round_Y");
    TH1F *h_first_round_R                = (TH1F *)get("h_first_round_R");
    TH1F *h_tracking_theta               = (TH1F *)get("h_tracking_theta");
    TH1F *h_tracking_phi                 = (TH1F *)get("h_tracking_phi");
    TH1F *h_tracking_angle_x             = (TH1F *)get("h_tracking_angle_x");
    TH1F *h_tracking_angle_y             = (TH1F *)get("h_tracking_angle_y");
    TH2F *h_intercept_drich              = (TH2F *)get("h_intercept_drich");
    TH2F *h_intercept_scint              = (TH2F *)get("h_intercept_scint");
    TH2F *h_second_round_xy_map          = (TH2F *)get("h_second_round_xy_map");
    TH2F *h_second_round_xy_map_rejected = (TH2F *)get("h_second_round_xy_map_rejected");
    TH1F *h_second_round_R               = (TH1F *)get("h_second_round_R");
    TH1F *h_n_selected_hits              = (TH1F *)get("h_n_selected_hits");
    TH2F *h_n_selected_hits_vs_multiplicity = (TH2F *)get("h_n_selected_hits_vs_multiplicity");
    TH2F *h_n_selected_hits_vs_theta     = (TH2F *)get("h_n_selected_hits_vs_theta");
    TH2F *h_n_selected_hits_vs_ix_drich  = (TH2F *)get("h_n_selected_hits_vs_ix_drich");
    TH2F *h_n_selected_hits_vs_iy_drich  = (TH2F *)get("h_n_selected_hits_vs_iy_drich");
    TH2F *h_deltaR_vs_theta              = (TH2F *)get("h_deltaR_vs_theta");
    TH2F *h_deltaR_vs_phi                = (TH2F *)get("h_deltaR_vs_phi");
    TH2F *h_deltaR_vs_ix_drich           = (TH2F *)get("h_deltaR_vs_ix_drich");
    TH2F *h_deltaR_vs_iy_drich           = (TH2F *)get("h_deltaR_vs_iy_drich");
    TH1F *h_d_intercept                  = (TH1F *)get("h_d_intercept");
    TH2F *h_deltaR_vs_d_intercept        = (TH2F *)get("h_deltaR_vs_d_intercept");
    TH2F *h_ring_x0_vs_ix_drich          = (TH2F *)get("h_ring_x0_vs_ix_drich");
    TH2F *h_ring_y0_vs_ix_drich          = (TH2F *)get("h_ring_y0_vs_ix_drich");
    TH2F *h_ring_R_vs_ix_drich           = (TH2F *)get("h_ring_R_vs_ix_drich");
    TH2F *h_ring_x0_vs_iy_drich          = (TH2F *)get("h_ring_x0_vs_iy_drich");
    TH2F *h_ring_y0_vs_iy_drich          = (TH2F *)get("h_ring_y0_vs_iy_drich");
    TH2F *h_ring_R_vs_iy_drich           = (TH2F *)get("h_ring_R_vs_iy_drich");
    TH2F *h_ring_x0_vs_theta             = (TH2F *)get("h_ring_x0_vs_theta");
    TH2F *h_ring_y0_vs_theta             = (TH2F *)get("h_ring_y0_vs_theta");
    TH2F *h_ring_R_vs_theta              = (TH2F *)get("h_ring_R_vs_theta");
    TH2F *h_ring_x0_vs_phi               = (TH2F *)get("h_ring_x0_vs_phi");
    TH2F *h_ring_y0_vs_phi               = (TH2F *)get("h_ring_y0_vs_phi");
    TH2F *h_ring_R_vs_phi                = (TH2F *)get("h_ring_R_vs_phi");
    TH2F *h_ring_R_vs_d_intercept        = (TH2F *)get("h_ring_R_vs_d_intercept");

    f->Close();

    // =========================================================================
    //  FIT SETUP
    // =========================================================================
    gROOT->SetBatch(true);

    TF1 *gaus = new TF1("gaus_fit", "gaus", -20, 20);
    const int entries_threshold = 200;

    // param 2 = sigma, param 1 = mean
    auto make_sigma = [&](TH2F *h) { return fit_histogram(h, gaus, entries_threshold, 2); };
    auto make_mean  = [&](TH2F *h) { return fit_histogram(h, gaus, entries_threshold, 1); };

    // =========================================================================
    //  DRAWING HELPERS
    // =========================================================================
    auto draw_graph = [&](TGraphErrors *g, const char *title)
    {
        g->SetTitle(title);
        g->SetMarkerStyle(20);
        g->SetMarkerSize(0.8);
        g->SetLineWidth(2);
        g->Sort();
        g->Draw("AP");
    };

    auto make_trio_canvas = [&](const char *name, const char *title,
                                TH2F *h_deltaR, TGraphErrors *g_fit,
                                const char *g_fit_title, TH2F *h_R_abs)
    {
        TCanvas *c = new TCanvas(name, title, 1800, 600);
        c->Divide(3, 1);
        c->cd(1); h_deltaR->Draw("SCAT");
        c->cd(2); draw_graph(g_fit, g_fit_title);
        c->cd(3); h_R_abs->Draw("SCAT");
        return c;
    };

    // Draw TH1 with optional highlighted range (semitransparent red box)
    auto draw_with_range = [&](TH1F *h, bool highlight, float xlo, float xhi)
    {
        h->SetLineColor(kBlack);
        h->SetLineWidth(2);
        h->Draw();
        if (!highlight) return;
        double ymax = h->GetMaximum() * 1.05;
        TBox *box = new TBox(xlo, 0, xhi, ymax);
        box->SetFillColorAlpha(kRed, 0.25);
        box->SetLineWidth(0);
        box->Draw("SAME");
    };

    // =========================================================================
    //  COMPUTE GRAPHS
    // =========================================================================
    auto g_sigma_theta       = make_sigma(h_deltaR_vs_theta);
    auto g_sigma_phi         = make_sigma(h_deltaR_vs_phi);
    auto g_sigma_ix          = make_sigma(h_deltaR_vs_ix_drich);
    auto g_sigma_iy          = make_sigma(h_deltaR_vs_iy_drich);
    auto g_sigma_d_intercept = make_sigma(h_deltaR_vs_d_intercept);

    auto g_mean_theta        = make_mean(h_deltaR_vs_theta);
    auto g_mean_phi          = make_mean(h_deltaR_vs_phi);
    auto g_mean_ix           = make_mean(h_deltaR_vs_ix_drich);
    auto g_mean_iy           = make_mean(h_deltaR_vs_iy_drich);
    auto g_mean_d_intercept  = make_mean(h_deltaR_vs_d_intercept);

    // =========================================================================
    //  CANVAS — time + basic diagnostics
    // =========================================================================
    TCanvas *c_time_delta = new TCanvas("c_time_delta", "Hit time wrt trigger", 800, 600);
    gPad->SetLogy();
    h_t_distribution->SetLineColor(kBlack);
    h_t_distribution->SetLineWidth(2);
    h_t_distribution->Draw();

    TCanvas *c_first_round = new TCanvas("c_first_round", "Ring fit results - 1st round", 1800, 600);
    c_first_round->Divide(3, 1);
    c_first_round->cd(1); h_first_round_X->Draw();
    c_first_round->cd(2); h_first_round_Y->Draw();
    c_first_round->cd(3); h_first_round_R->Draw();

    TCanvas *c_tracking_angles = new TCanvas("c_tracking_angles", "Tracking angles", 2400, 600);
    c_tracking_angles->Divide(4, 1);
    c_tracking_angles->cd(1); draw_with_range(h_tracking_theta,   do_theta_phi_highlight, f_theta_min,   f_theta_max);
    c_tracking_angles->cd(2); draw_with_range(h_tracking_phi,     do_theta_phi_highlight, f_phi_min,     f_phi_max);
    c_tracking_angles->cd(3); draw_with_range(h_tracking_angle_x, do_angle_xy_highlight,  f_angle_x_min, f_angle_x_max);
    c_tracking_angles->cd(4); draw_with_range(h_tracking_angle_y, do_angle_xy_highlight,  f_angle_y_min, f_angle_y_max);

    // =========================================================================
    //  CANVAS — intercept maps
    // =========================================================================
    TCanvas *c_intercepts_colz = new TCanvas("c_intercepts_colz", "Track intercept maps - COLZ", 1600, 800);
    c_intercepts_colz->Divide(2, 1);
    c_intercepts_colz->cd(1); h_intercept_drich->Draw("COLZ");
    c_intercepts_colz->cd(2); h_intercept_scint->Draw("COLZ");

    TCanvas *c_intercepts_scat = new TCanvas("c_intercepts_scat", "Track intercept maps - SCAT", 1600, 800);
    c_intercepts_scat->Divide(2, 1);
    c_intercepts_scat->cd(1); ((TH2F *)h_intercept_drich->Clone())->Draw("SCAT");
    c_intercepts_scat->cd(2); ((TH2F *)h_intercept_scint->Clone())->Draw("SCAT");

    TCanvas *c_d_intercept = new TCanvas("c_d_intercept", "Distance intercept-ring center", 800, 600);
    h_d_intercept->Draw();

    // =========================================================================
    //  CANVAS — hit maps
    // =========================================================================
    TCanvas *c_hit_maps = new TCanvas("c_hit_maps", "Hit maps", 1600, 800);
    c_hit_maps->Divide(2, 1);
    c_hit_maps->cd(1); h_second_round_xy_map->Draw("COLZ");
    c_hit_maps->cd(2); h_second_round_xy_map_rejected->Draw("COLZ");

    TCanvas *c_hit_maps_scat = new TCanvas("c_hit_maps_scat", "Hit maps - SCAT", 1600, 800);
    c_hit_maps_scat->Divide(2, 1);
    c_hit_maps_scat->cd(1); ((TH2F *)h_second_round_xy_map->Clone())->Draw("SCAT");
    c_hit_maps_scat->cd(2); ((TH2F *)h_second_round_xy_map_rejected->Clone())->Draw("SCAT");

    // =========================================================================
    //  CANVAS — photon yield
    // =========================================================================
    TCanvas *c_photons = new TCanvas("c_photons", "Photon yield", 1800, 600);
    c_photons->Divide(3, 1);
    c_photons->cd(1); h_n_selected_hits->Draw();
    c_photons->cd(2); h_n_selected_hits_vs_multiplicity->Draw("SCAT");
    c_photons->cd(3); h_n_selected_hits_vs_theta->Draw("SCAT");

    TCanvas *c_photons_intercepts = new TCanvas("c_photons_intercepts", "Photon yield vs intercepts", 1600, 800);
    c_photons_intercepts->Divide(2, 1);
    c_photons_intercepts->cd(1); h_n_selected_hits_vs_ix_drich->Draw("SCAT");
    c_photons_intercepts->cd(2); h_n_selected_hits_vs_iy_drich->Draw("SCAT");

    // =========================================================================
    //  CANVAS — ring params vs ix/iy
    // =========================================================================
    TCanvas *c_ring_params_ix = new TCanvas("c_ring_params_ix", "Ring params vs x_{dRICH}", 1800, 600);
    c_ring_params_ix->Divide(3, 1);
    c_ring_params_ix->cd(1); h_ring_x0_vs_ix_drich->Draw("SCAT");
    c_ring_params_ix->cd(2); h_ring_y0_vs_ix_drich->Draw("SCAT");
    c_ring_params_ix->cd(3); h_ring_R_vs_ix_drich->Draw("SCAT");

    TCanvas *c_ring_params_iy = new TCanvas("c_ring_params_iy", "Ring params vs y_{dRICH}", 1800, 600);
    c_ring_params_iy->Divide(3, 1);
    c_ring_params_iy->cd(1); h_ring_x0_vs_iy_drich->Draw("SCAT");
    c_ring_params_iy->cd(2); h_ring_y0_vs_iy_drich->Draw("SCAT");
    c_ring_params_iy->cd(3); h_ring_R_vs_iy_drich->Draw("SCAT");

    // =========================================================================
    //  CANVAS — ring params vs theta/phi
    // =========================================================================
    TCanvas *c_ring_params_theta = new TCanvas("c_ring_params_theta", "Ring params vs #theta", 1800, 600);
    c_ring_params_theta->Divide(3, 1);
    c_ring_params_theta->cd(1); h_ring_x0_vs_theta->Draw("SCAT");
    c_ring_params_theta->cd(2); h_ring_y0_vs_theta->Draw("SCAT");
    c_ring_params_theta->cd(3); h_ring_R_vs_theta->Draw("SCAT");

    TCanvas *c_ring_params_phi = new TCanvas("c_ring_params_phi", "Ring params vs #phi", 1800, 600);
    c_ring_params_phi->Divide(3, 1);
    c_ring_params_phi->cd(1); h_ring_x0_vs_phi->Draw("SCAT");
    c_ring_params_phi->cd(2); h_ring_y0_vs_phi->Draw("SCAT");
    c_ring_params_phi->cd(3); h_ring_R_vs_phi->Draw("SCAT");

    // =========================================================================
    //  CANVAS — deltaR + sigma + R_abs, per variabile
    // =========================================================================
    auto c_res_theta = make_trio_canvas("c_sigma_theta",
        "#sigma(#DeltaR) vs #theta",
        h_deltaR_vs_theta, g_sigma_theta,
        "#sigma(#DeltaR) vs #theta;#theta (rad);#sigma(#DeltaR) (mm)",
        h_ring_R_vs_theta);

    auto c_res_phi = make_trio_canvas("c_sigma_phi",
        "#sigma(#DeltaR) vs #phi",
        h_deltaR_vs_phi, g_sigma_phi,
        "#sigma(#DeltaR) vs #phi;#phi (rad);#sigma(#DeltaR) (mm)",
        h_ring_R_vs_phi);

    auto c_res_ix = make_trio_canvas("c_sigma_ix",
        "#sigma(#DeltaR) vs x_{dRICH}",
        h_deltaR_vs_ix_drich, g_sigma_ix,
        "#sigma(#DeltaR) vs x_{dRICH};x_{dRICH} (mm);#sigma(#DeltaR) (mm)",
        h_ring_R_vs_ix_drich);

    auto c_res_iy = make_trio_canvas("c_sigma_iy",
        "#sigma(#DeltaR) vs y_{dRICH}",
        h_deltaR_vs_iy_drich, g_sigma_iy,
        "#sigma(#DeltaR) vs y_{dRICH};y_{dRICH} (mm);#sigma(#DeltaR) (mm)",
        h_ring_R_vs_iy_drich);

    auto c_res_d = make_trio_canvas("c_sigma_d",
        "#sigma(#DeltaR) vs d_{intercept}",
        h_deltaR_vs_d_intercept, g_sigma_d_intercept,
        "#sigma(#DeltaR) vs d_{intercept};d_{intercept} (mm);#sigma(#DeltaR) (mm)",
        h_ring_R_vs_d_intercept);

    auto c_mean_theta = make_trio_canvas("c_mean_theta",
        "#mu(#DeltaR) vs #theta",
        h_deltaR_vs_theta, g_mean_theta,
        "#mu(#DeltaR) vs #theta;#theta (rad);#mu(#DeltaR) (mm)",
        h_ring_R_vs_theta);

    auto c_mean_phi = make_trio_canvas("c_mean_phi",
        "#mu(#DeltaR) vs #phi",
        h_deltaR_vs_phi, g_mean_phi,
        "#mu(#DeltaR) vs #phi;#phi (rad);#mu(#DeltaR) (mm)",
        h_ring_R_vs_phi);

    auto c_mean_ix = make_trio_canvas("c_mean_ix",
        "#mu(#DeltaR) vs x_{dRICH}",
        h_deltaR_vs_ix_drich, g_mean_ix,
        "#mu(#DeltaR) vs x_{dRICH};x_{dRICH} (mm);#mu(#DeltaR) (mm)",
        h_ring_R_vs_ix_drich);

    auto c_mean_iy = make_trio_canvas("c_mean_iy",
        "#mu(#DeltaR) vs y_{dRICH}",
        h_deltaR_vs_iy_drich, g_mean_iy,
        "#mu(#DeltaR) vs y_{dRICH};y_{dRICH} (mm);#mu(#DeltaR) (mm)",
        h_ring_R_vs_iy_drich);

    auto c_mean_d = make_trio_canvas("c_mean_d",
        "#mu(#DeltaR) vs d_{intercept}",
        h_deltaR_vs_d_intercept, g_mean_d_intercept,
        "#mu(#DeltaR) vs d_{intercept};d_{intercept} (mm);#mu(#DeltaR) (mm)",
        h_ring_R_vs_d_intercept);

    // =========================================================================
    //  CANVAS — settings
    // =========================================================================
    TCanvas *c_settings = new TCanvas("c_settings", "Analysis settings", 600, 600);
    TPaveText *pt = new TPaveText(0.05, 0.05, 0.95, 0.95, "NDC");
    pt->SetTextAlign(12); pt->SetTextFont(42);
    pt->SetFillColor(0);  pt->SetBorderSize(1);
    pt->AddText(Form("plane1:                 %s", plane1.c_str()));
    pt->AddText(Form("side1:                  %s", side1.c_str()));
    pt->AddText(Form("cutg1:                  %s", cutg1_title.c_str()));
    pt->AddText(Form("plane2:                 %s", plane2.c_str()));
    pt->AddText(Form("side2:                  %s", side2.c_str()));
    pt->AddText(Form("cutg2:                  %s", cutg2_title.c_str()));
    pt->AddText(Form("require_single_track:   %s", require_single_track.c_str()));
    pt->AddText(Form("require_multi_track:    %s", require_multi_track.c_str()));
    pt->AddText(Form("apply_multiplicity_cut: %s", apply_multiplicity_cut.c_str()));
    pt->AddText(Form("apply_radial_cut:       %s", apply_radial_cut.c_str()));
    pt->AddText(Form("apply_theta_phi_cut:    %s", apply_theta_phi_cut.c_str()));
    pt->AddText(Form("apply_angle_xy_cut:     %s", apply_angle_xy_cut.c_str()));
    pt->AddText(Form("theta_min:              %s", theta_min.c_str()));
    pt->AddText(Form("theta_max:              %s", theta_max.c_str()));
    pt->AddText(Form("phi_min:                %s", phi_min.c_str()));
    pt->AddText(Form("phi_max:                %s", phi_max.c_str()));
    pt->AddText(Form("angle_x_min:            %s", angle_x_min.c_str()));
    pt->AddText(Form("angle_x_max:            %s", angle_x_max.c_str()));
    pt->AddText(Form("angle_y_min:            %s", angle_y_min.c_str()));
    pt->AddText(Form("angle_y_max:            %s", angle_y_max.c_str()));
    pt->AddText(Form("run:                    %s", run_name.c_str()));
    pt->Draw();

    // =========================================================================
    //  SAVE PNG + canvas in ROOT file
    // =========================================================================
    std::string output_root = output_dir + "/plots.root";
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");
    for (auto obj : *gROOT->GetListOfCanvases())
    {
        TCanvas *c = (TCanvas *)obj;
        c->SaveAs(Form("%s/%s.png", output_dir.c_str(), c->GetName()));
        c->Write();
    }
    g_sigma_theta->Write("g_sigma_theta");
    g_sigma_phi->Write("g_sigma_phi");
    g_sigma_ix->Write("g_sigma_ix");
    g_sigma_iy->Write("g_sigma_iy");
    g_sigma_d_intercept->Write("g_sigma_d_intercept");
    g_mean_theta->Write("g_mean_theta");
    g_mean_phi->Write("g_mean_phi");
    g_mean_ix->Write("g_mean_ix");
    g_mean_iy->Write("g_mean_iy");
    g_mean_d_intercept->Write("g_mean_d_intercept");

    fout->Close();
    cout << "Output: " << output_root << endl;
}