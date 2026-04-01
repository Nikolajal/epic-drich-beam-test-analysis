#include "../lib_loader.h"
#include "ringtrack_config.h"
#include "ringtrack_track_selection.h"

// =============================================================================
//  ringtrack_noring  —  tag events as ring / no-ring and compare them
//
//  Ring detection method is configurable via conf key:
//    noring_method = nhits | dbscan | hough
//
//  nhits:  event is "no-ring" if n_hits_in_window < noring_nhits_threshold
//  dbscan: calls find_rings(noring_dbscan_dr, noring_dbscan_dt);
//          event is "no-ring" if no hit is ring-tagged after clustering
//  hough:  calls find_rings_hough(noring_hough_threshold, noring_hough_min_hits)
//          after building the LUT once; event is "no-ring" if no hough-tagged hit
//
//  Output:
//    noring.root  — all histograms (ring vs no-ring comparison + display)
//    noring_display_ring.png   — hit display for ring events
//    noring_display_noring.png — hit display for no-ring events
//    noring_stats.txt          — summary statistics
// =============================================================================
void ringtrack_noring(std::string data_repository, std::string run_name,
                      std::string conf_path = "ringtrack.conf",
                      std::string output_dir = "")
{
    // -------------------------------------------------------------------------
    //  Config
    // -------------------------------------------------------------------------
    RingtrackConfig cfg;
    cfg.load(conf_path);
    cfg.print();

    if (output_dir.empty())
    {
        TString _repo = gSystem->DirName(gSystem->DirName(gSystem->DirName(__FILE__)));
        TDatime _now;
        TString _dt = Form("%04d%02d%02d_%02d%02d%02d",
            _now.GetYear(), _now.GetMonth(), _now.GetDay(),
            _now.GetHour(), _now.GetMinute(), _now.GetSecond());
        output_dir = std::string(_repo.Data()) + "/plots/" + run_name + "/" + std::string(_dt.Data());
    }
    gSystem->mkdir(output_dir.c_str(), true);

    // ring-finding method
    const std::string noring_method  = cfg.get_string("noring_method", "nhits");
    // nhits method
    const int   nhits_threshold      = cfg.get_int  ("noring_nhits_threshold", 3);
    // dbscan method
    const float dbscan_dr            = cfg.get_float("noring_dbscan_dr",  5.0f);
    const float dbscan_dt            = cfg.get_float("noring_dbscan_dt",  5.0f);
    // hough method
    const float hough_threshold      = cfg.get_float("noring_hough_threshold",  0.3f);
    const int   hough_min_hits       = cfg.get_int  ("noring_hough_min_hits",   3);
    const float hough_r_min          = cfg.get_float("noring_hough_r_min",      10.0f);
    const float hough_r_max          = cfg.get_float("noring_hough_r_max",      60.0f);
    const float hough_r_step         = cfg.get_float("noring_hough_r_step",      2.0f);

    std::cout << "[noring] method = " << noring_method << std::endl;
    if (noring_method == "nhits")
        std::cout << "[noring] nhits threshold = " << nhits_threshold << std::endl;
    else if (noring_method == "dbscan")
        std::cout << "[noring] DBSCAN dr=" << dbscan_dr << " dt=" << dbscan_dt << std::endl;
    else if (noring_method == "hough")
        std::cout << "[noring] Hough threshold=" << hough_threshold
                  << " min_hits=" << hough_min_hits
                  << " r=[" << hough_r_min << "," << hough_r_max << "] step=" << hough_r_step << std::endl;
    else
    {
        std::cerr << "[ERROR] Unknown noring_method: " << noring_method
                  << " — use nhits, dbscan, or hough" << std::endl;
        return;
    }

    // standard cuts (reuse existing pattern)
    TrackSelectionConfig tsel;
    tsel.load(cfg);

    const bool  apply_afterpulse_cut = cfg.get_bool ("apply_afterpulse_cut", true);
    const std::array<float,2> time_cut = {
        cfg.get_float("time_cut_min", -50.f),
        cfg.get_float("time_cut_max",  20.f)
    };
    const bool  apply_window_selection  = cfg.get_bool ("apply_window_selection",  false);
    const float window_sel_min          = cfg.get_float("window_sel_min",          -10.f);
    const float window_sel_max          = cfg.get_float("window_sel_max",           10.f);
    const int   window_sel_threshold    = cfg.get_int  ("window_sel_threshold",      5);
    const bool  apply_window_veto       = cfg.get_bool ("apply_window_veto",        false);
    const float window_veto_min         = cfg.get_float("window_veto_min",         -65.f);
    const float window_veto_max         = cfg.get_float("window_veto_max",         -25.f);
    const int   window_veto_threshold   = cfg.get_int  ("window_veto_threshold",    3);
    const bool  apply_window_veto_2     = cfg.get_bool ("apply_window_veto_2",      false);
    const float window_veto_2_min       = cfg.get_float("window_veto_2_min",        80.f);
    const float window_veto_2_max       = cfg.get_float("window_veto_2_max",       120.f);
    const int   window_veto_2_threshold = cfg.get_int  ("window_veto_2_threshold",   3);
    const bool  require_all_pass        = cfg.get_bool ("require_all_tracks_pass_geometric_cuts", false);

    const int   n_display_events = cfg.get_int("n_display_events", 10000);

    // -------------------------------------------------------------------------
    //  Open input
    // -------------------------------------------------------------------------
    std::string input_filename = data_repository + "/" + run_name + "/recotrackdata.root";
    TFile *input_file = new TFile(input_filename.c_str());
    if (!input_file || input_file->IsZombie())
    {
        std::cerr << "[ERROR] Could not open " << input_filename << std::endl;
        return;
    }
    TTree *tree = (TTree *)input_file->Get("recotrackdata");
    alcor_recotrackdata *recotrackdata = new alcor_recotrackdata();
    recotrackdata->link_to_tree(tree);

    const long long n_frames   = tree->GetEntries();
    const int       first_event = cfg.get_int("first_event", 0);
    const int       max_frames_ = cfg.get_int("max_frames",  1000000);
    const int       all_frames  = (int)std::min((long long)first_event + max_frames_, n_frames);

    // =========================================================================
    //  Histograms
    // =========================================================================

    // hit maps
    TH2F *h_hit_map_ring   = new TH2F("h_hit_map_ring",
        "Hit map — ring events;x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    TH2F *h_hit_map_noring = new TH2F("h_hit_map_noring",
        "Hit map — no-ring events;x (mm);y (mm)", 396, -99, 99, 396, -99, 99);

    // hit multiplicity
    TH1F *h_nhits_ring   = new TH1F("h_nhits_ring",
        "N hits in window — ring events;n hits;counts", 100, 0, 100);
    TH1F *h_nhits_noring = new TH1F("h_nhits_noring",
        "N hits in window — no-ring events;n hits;counts", 100, 0, 100);

    // timing distributions
    TH1F *h_t_ring   = new TH1F("h_t_ring",
        "Hit time — ring events;t_{hit} - t_{trig} (ns);counts", 200, -312.5, 312.5);
    TH1F *h_t_noring = new TH1F("h_t_noring",
        "Hit time — no-ring events;t_{hit} - t_{trig} (ns);counts", 200, -312.5, 312.5);

    // intercepts
    TH2F *h_intercept_ring   = new TH2F("h_intercept_ring",
        "dRICH intercept — ring events;x (mm);y (mm)", 200, -200, 200, 200, -200, 200);
    TH2F *h_intercept_noring = new TH2F("h_intercept_noring",
        "dRICH intercept — no-ring events;x (mm);y (mm)", 200, -200, 200, 200, -200, 200);

    // n_hits vs intercept (ring vs no-ring)
    TH2F *h_nhits_vs_ix_ring   = new TH2F("h_nhits_vs_ix_ring",
        "N hits vs ix dRICH — ring;x_{dRICH} (mm);n hits", 120, -200, 200, 100, -0.5, 99.5);
    TH2F *h_nhits_vs_ix_noring = new TH2F("h_nhits_vs_ix_noring",
        "N hits vs ix dRICH — no-ring;x_{dRICH} (mm);n hits", 120, -200, 200, 100, -0.5, 99.5);

    // display: accumulated hit scatter for first N events of each type
    TH2F *h_display_ring   = new TH2F("h_display_ring",
        Form("Hits + intercepts — ring (first %d events);x (mm);y (mm)", n_display_events),
        396, -200, 200, 396, -200, 200);
    TH2F *h_display_noring = new TH2F("h_display_noring",
        Form("Hits + intercepts — no-ring (first %d events);x (mm);y (mm)", n_display_events),
        396, -200, 200, 396, -200, 200);

    TGraph *g_intercepts_ring   = new TGraph();
    g_intercepts_ring  ->SetName("g_intercepts_ring");
    g_intercepts_ring  ->SetTitle("Intercepts — ring events;x (mm);y (mm)");
    TGraph *g_intercepts_noring = new TGraph();
    g_intercepts_noring->SetName("g_intercepts_noring");
    g_intercepts_noring->SetTitle("Intercepts — no-ring events;x (mm);y (mm)");

    int n_ring = 0, n_noring = 0, n_rejected = 0, n_no_trigger = 0;
    int n_display_ring = 0, n_display_noring = 0;

    // =========================================================================
    //  Hough LUT (built once before the loop)
    // =========================================================================
    if (noring_method == "hough")
    {
        std::cout << "[noring] Building Hough LUT from first frame..." << std::endl;
        tree->GetEntry(0);
        // Build geometry map: global_index -> (x, y) from the first frame
        std::map<int, std::array<float, 2>> idx_to_xy;
        for (int i = 0; i < (int)recotrackdata->get_recodata().size(); i++)
        {
            uint32_t gidx = recotrackdata->get_global_index(i);
            idx_to_xy[(int)gidx] = { recotrackdata->get_hit_x(i),
                                     recotrackdata->get_hit_y(i) };
        }
        recotrackdata->build_hough_lut(idx_to_xy,
            hough_r_min, hough_r_max, hough_r_step, 3.2f);
        std::cout << "[noring] Hough LUT built." << std::endl;
    }

    // =========================================================================
    //  Event loop
    // =========================================================================
    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 1000 == 0) bar.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;

        auto trigger = recotrackdata->get_trigger_by_index(0);
        if (!trigger) { ++n_no_trigger; continue; }

        const int n_trk = recotrackdata->n_recotrackdata();

        // window selection / veto
        if (apply_window_selection || apply_window_veto || apply_window_veto_2)
        {
            int n_sel = 0, n_veto = 0, n_veto2 = 0;
            for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
            {
                if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
                float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
                if (apply_window_selection && dt >= window_sel_min   && dt <= window_sel_max)   ++n_sel;
                if (apply_window_veto      && dt >= window_veto_min  && dt <= window_veto_max)  ++n_veto;
                if (apply_window_veto_2    && dt >= window_veto_2_min && dt <= window_veto_2_max) ++n_veto2;
            }
            if (apply_window_selection && n_sel   <  window_sel_threshold)   continue;
            if (apply_window_veto      && n_veto  >= window_veto_threshold)  continue;
            if (apply_window_veto_2    && n_veto2 >= window_veto_2_threshold) continue;
        }

        // multiplicity
        if (!tsel.passes_multiplicity(n_trk)) continue;

        // require all tracks to pass geometric cuts
        if (require_all_pass && n_trk > 1)
        {
            bool all_pass = true;
            for (int i = 0; i < n_trk; i++)
            {
                if (!is_track_selected(
                        recotrackdata->get_det_plane_x(i), recotrackdata->get_det_plane_y(i),
                        recotrackdata->get_traj_angcoeff_x(i), recotrackdata->get_traj_angcoeff_y(i),
                        tsel.cutg1, tsel.plane1, tsel.side1,
                        tsel.cutg2, tsel.plane2, tsel.side2,
                        tsel.z_drich, tsel.z_scint))
                { all_pass = false; break; }
            }
            if (!all_pass) { ++n_rejected; continue; }
        }

        // geometric + chi2 cut on at least one track
        bool any_track_selected = false;
        int  best_track = -1;
        float best_chi2 = 1e9f;
        for (int i = 0; i < n_trk; i++)
        {
            if (tsel.apply_chi2_cut && recotrackdata->get_chi2ndof(i) > tsel.chi2_max) continue;
            if (!is_track_selected(
                    recotrackdata->get_det_plane_x(i), recotrackdata->get_det_plane_y(i),
                    recotrackdata->get_traj_angcoeff_x(i), recotrackdata->get_traj_angcoeff_y(i),
                    tsel.cutg1, tsel.plane1, tsel.side1,
                    tsel.cutg2, tsel.plane2, tsel.side2,
                    tsel.z_drich, tsel.z_scint)) continue;
            any_track_selected = true;
            float c = recotrackdata->get_chi2ndof(i);
            if (c < best_chi2) { best_chi2 = c; best_track = i; }
        }
        if (!any_track_selected) { ++n_rejected; continue; }

        // intercept of best selected track at dRICH
        const float px = recotrackdata->get_det_plane_x(best_track);
        const float py = recotrackdata->get_det_plane_y(best_track);
        const float sx = recotrackdata->get_traj_angcoeff_x(best_track);
        const float sy = recotrackdata->get_traj_angcoeff_y(best_track);
        const float ix = px + sx * tsel.z_drich;
        const float iy = py + sy * tsel.z_drich;

        // count hits in time window
        int n_hits = 0;
        for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
        {
            if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
            float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
            if (dt >= time_cut[0] && dt <= time_cut[1]) ++n_hits;
        }

        // ---- ring detection ----
        bool has_ring = false;

        if (noring_method == "nhits")
        {
            has_ring = (n_hits >= nhits_threshold);
        }
        else if (noring_method == "dbscan")
        {
            recotrackdata->find_rings(dbscan_dr, dbscan_dt);
            for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
            {
                if (recotrackdata->is_ring_tagged(i_hit)) { has_ring = true; break; }
            }
        }
        else if (noring_method == "hough")
        {
            recotrackdata->find_rings_hough(hough_threshold, hough_min_hits);
            for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
            {
                if (recotrackdata->is_hough_ring_tag(i_hit)) { has_ring = true; break; }
            }
        }

        // ---- fill histograms ----
        TH2F  *h_map     = has_ring ? h_hit_map_ring     : h_hit_map_noring;
        TH1F  *h_nhits   = has_ring ? h_nhits_ring       : h_nhits_noring;
        TH1F  *h_t       = has_ring ? h_t_ring           : h_t_noring;
        TH2F  *h_icept   = has_ring ? h_intercept_ring   : h_intercept_noring;
        TH2F  *h_nhvix   = has_ring ? h_nhits_vs_ix_ring : h_nhits_vs_ix_noring;
        TH2F  *h_disp    = has_ring ? h_display_ring     : h_display_noring;
        TGraph*g_disp    = has_ring ? g_intercepts_ring  : g_intercepts_noring;
        int   &n_disp    = has_ring ? n_display_ring      : n_display_noring;

        h_nhits->Fill(n_hits);
        h_icept->Fill(ix, iy);
        h_nhvix->Fill(ix, n_hits);

        for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); i_hit++)
        {
            if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
            float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
            h_t->Fill(dt);
            if (dt < time_cut[0] || dt > time_cut[1]) continue;
            h_map->Fill(recotrackdata->get_hit_x_rnd(i_hit),
                        recotrackdata->get_hit_y_rnd(i_hit));
            if (n_disp < n_display_events)
                h_disp->Fill(recotrackdata->get_hit_x_rnd(i_hit),
                             recotrackdata->get_hit_y_rnd(i_hit));
        }
        if (n_disp < n_display_events) { g_disp->AddPoint(ix, iy); ++n_disp; }

        if (has_ring) ++n_ring; else ++n_noring;
    }
    bar.update(all_frames, all_frames);
    bar.finish();

    // =========================================================================
    //  Statistics
    // =========================================================================
    int n_total = n_ring + n_noring;
    std::cout << "========================================"     << std::endl;
    std::cout << "No-ring study — method: " << noring_method   << std::endl;
    std::cout << "  Total selected events: " << n_total        << std::endl;
    std::cout << "  Ring events:    " << n_ring
              << Form("  (%.1f%%)", n_total > 0 ? 100.f * n_ring   / n_total : 0.f) << std::endl;
    std::cout << "  No-ring events: " << n_noring
              << Form("  (%.1f%%)", n_total > 0 ? 100.f * n_noring / n_total : 0.f) << std::endl;
    std::cout << "  No trigger:  " << n_no_trigger              << std::endl;
    std::cout << "  Rejected:    " << n_rejected                << std::endl;
    std::cout << "========================================"      << std::endl;

    // stats txt
    std::ofstream stats_file(output_dir + "/noring_stats.txt");
    stats_file << "method\t"        << noring_method << "\n";
    stats_file << "total\t"         << n_total       << "\n";
    stats_file << "ring\t"          << n_ring        << "\n";
    stats_file << "noring\t"        << n_noring      << "\n";
    stats_file << "ring_pct\t"      << (n_total > 0 ? 100.f * n_ring   / n_total : 0.f) << "\n";
    stats_file << "noring_pct\t"    << (n_total > 0 ? 100.f * n_noring / n_total : 0.f) << "\n";
    stats_file.close();

    // =========================================================================
    //  Draw and save
    // =========================================================================
    gROOT->SetBatch(true);

    // helper: draw hit display canvas
    auto draw_display = [&](TH2F *h_hits, TGraph *g_ic, const char *label, const char *png_name) {
        if (!h_hits || h_hits->GetEntries() == 0) return;
        TCanvas *c = new TCanvas(Form("c_display_%s", label),
            Form("Display — %s events", label), 1000, 900);
        c->SetLeftMargin(0.12); c->SetRightMargin(0.15);
        h_hits->Draw("SCAT");
        if (g_ic && g_ic->GetN() > 0)
        {
            g_ic->SetMarkerStyle(29);
            g_ic->SetMarkerColor(kRed);
            g_ic->SetMarkerSize(0.9);
            g_ic->Draw("P SAME");
        }
        c->SaveAs(Form("%s/%s", output_dir.c_str(), png_name));
        delete c;
    };

    draw_display(h_display_ring,   g_intercepts_ring,   "ring",   "noring_display_ring.png");
    draw_display(h_display_noring, g_intercepts_noring, "noring", "noring_display_noring.png");

    // hit map comparison
    {
        TCanvas *c = new TCanvas("c_hit_map_compare",
            "Hit map comparison: ring vs no-ring", 1600, 700);
        c->Divide(2, 1);
        c->cd(1); h_hit_map_ring  ->Draw("COLZ"); gPad->SetLogz(1);
        c->cd(2); h_hit_map_noring->Draw("COLZ"); gPad->SetLogz(1);
        c->SaveAs(Form("%s/noring_hit_map_compare.png", output_dir.c_str()));
        delete c;
    }

    // nhits comparison
    {
        TCanvas *c = new TCanvas("c_nhits_compare",
            "N hits comparison: ring vs no-ring", 900, 600);
        h_nhits_ring  ->SetLineColor(kBlue+1); h_nhits_ring  ->SetLineWidth(2);
        h_nhits_noring->SetLineColor(kRed+1);  h_nhits_noring->SetLineWidth(2);
        double ymax = std::max(h_nhits_ring->GetMaximum(), h_nhits_noring->GetMaximum());
        h_nhits_ring  ->SetMaximum(ymax * 1.2);
        h_nhits_ring  ->Draw("HIST"); h_nhits_noring->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.6, 0.7, 0.88, 0.88);
        leg->AddEntry(h_nhits_ring,   "ring",    "l");
        leg->AddEntry(h_nhits_noring, "no-ring", "l");
        leg->Draw();
        c->SaveAs(Form("%s/noring_nhits_compare.png", output_dir.c_str()));
        delete c;
    }

    // timing comparison
    {
        TCanvas *c = new TCanvas("c_t_compare",
            "Timing comparison: ring vs no-ring", 900, 600);
        h_t_ring  ->SetLineColor(kBlue+1); h_t_ring  ->SetLineWidth(2);
        h_t_noring->SetLineColor(kRed+1);  h_t_noring->SetLineWidth(2);
        double ymax = std::max(h_t_ring->GetMaximum(), h_t_noring->GetMaximum());
        h_t_ring  ->SetMaximum(ymax * 1.2);
        h_t_ring  ->Draw("HIST"); h_t_noring->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.6, 0.7, 0.88, 0.88);
        leg->AddEntry(h_t_ring,   "ring",    "l");
        leg->AddEntry(h_t_noring, "no-ring", "l");
        leg->Draw();
        c->SaveAs(Form("%s/noring_timing_compare.png", output_dir.c_str()));
        delete c;
    }

    // intercept comparison
    {
        TCanvas *c = new TCanvas("c_intercept_compare",
            "dRICH intercept: ring vs no-ring", 1600, 700);
        c->Divide(2, 1);
        c->cd(1); h_intercept_ring  ->Draw("COLZ");
        c->cd(2); h_intercept_noring->Draw("COLZ");
        c->SaveAs(Form("%s/noring_intercept_compare.png", output_dir.c_str()));
        delete c;
    }

    // =========================================================================
    //  Save ROOT file
    // =========================================================================
    std::string output_root = output_dir + "/noring.root";
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");
    for (auto &kv : cfg._data)
        TNamed(kv.first.c_str(), kv.second.c_str()).Write();
    TNamed("run_name",       run_name.c_str()).Write();
    TNamed("noring_method",  noring_method.c_str()).Write();
    h_hit_map_ring->Write();    h_hit_map_noring->Write();
    h_nhits_ring->Write();      h_nhits_noring->Write();
    h_t_ring->Write();          h_t_noring->Write();
    h_intercept_ring->Write();  h_intercept_noring->Write();
    h_nhits_vs_ix_ring->Write(); h_nhits_vs_ix_noring->Write();
    h_display_ring->Write();    h_display_noring->Write();
    g_intercepts_ring->Write(); g_intercepts_noring->Write();
    fout->Close();

    std::cout << "Output: " << output_root << std::endl;
}
