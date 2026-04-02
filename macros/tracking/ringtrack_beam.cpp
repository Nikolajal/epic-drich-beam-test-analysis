#include "../lib_loader.h"
#include "ringtrack_config.h"
#include "ringtrack_track_selection.h"
#include <fstream>

// =============================================================================
//  ringtrack_beam  —  beam composition analysis + multi-track vertex finding
//
//  Part 1 — Event statistics (no track selection applied):
//    Counts and percentages of no-track / 1-track / 2-track / 3-track / 4+
//    track events.  Per-track chi2, theta, angle distributions.
//
//  Part 2 — Multi-track back-projection and vertex finding:
//    For each event with >= 2 tracks (optional chi2 cut), and for every pair
//    of tracks, the z-position of minimum transverse distance (closest
//    approach) is computed analytically:
//
//      Track i: x_i(z) = px_i + sx_i*z,   y_i(z) = py_i + sy_i*z
//      Track j: x_j(z) = px_j + sx_j*z,   y_j(z) = py_j + sy_j*z
//
//      D²(z) = (Δpx + Δsx·z)² + (Δpy + Δsy·z)²
//      dD²/dz = 0  →  z_v = -(Δpx·Δsx + Δpy·Δsy) / (Δsx² + Δsy²)
//
//    The "best" vertex per event is the pair with the smallest DCA at z_v.
//    The vertex position (x_v, y_v) is the midpoint between the two tracks
//    at z_v.
//
//    Back-projection display: accumulates (z, x(z)) and (z, y(z)) for all
//    tracks in multi-track events over a configurable z range, giving a
//    visual "where do the tracks cross?" picture.
//
//  Conf keys (beyond standard ones):
//    vertex_z_min        — lower z bound for vertex search [mm]  (default -5500)
//    vertex_z_max        — upper z bound for vertex search [mm]  (default  +500)
//    vertex_dca_max      — max DCA to accept a vertex [mm]       (default 1e9)
//    backproj_n_z_steps  — z steps for back-projection display   (default 200)
//
//  Outputs:
//    beam.root
//    beam_stats.txt
//    beam_multiplicity.png       — event type bar chart with percentages
//    beam_chi2.png               — chi2/NDF distributions by multiplicity
//    beam_theta.png              — theta angle distributions
//    beam_vertex_z.png           — vertex z distribution (main result)
//    beam_vertex_dca.png         — DCA at vertex (quality metric)
//    beam_vertex_xy.png          — vertex (x,y) at reconstructed z_v
//    beam_backproj_xz.png        — 2D back-projection x–z (all multi-track)
//    beam_backproj_yz.png        — 2D back-projection y–z
//    beam_backproj_from_vertex.png — back-projection x–z and y–z with tracks
//                                    drawn only from the reconstructed vertex
//                                    (DCA cut applied; geometric selection from conf)
// =============================================================================
void ringtrack_beam(std::string data_repository, std::string run_name,
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

    TrackSelectionConfig tsel;
    tsel.load(cfg);

    const int   first_event = cfg.get_int  ("first_event", 0);
    const int   max_frames_ = cfg.get_int  ("max_frames",  1000000);
    const bool  apply_chi2  = cfg.get_bool ("apply_chi2_cut", false);
    const float chi2_max    = cfg.get_float("chi2_max", 5.f);

    // vertex-finding range
    const float vz_min     = cfg.get_float("vertex_z_min",    -5500.f);
    const float vz_max     = cfg.get_float("vertex_z_max",     +500.f);
    const float dca_max    = cfg.get_float("vertex_dca_max",   1.e9f);
    const int   n_z_steps  = cfg.get_int  ("backproj_n_z_steps", 200);
    const float z_drich    = cfg.get_float("z_drich", -4250.f);
    const float z_scint    = cfg.get_float("z_scint", -1150.f);

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
    const int       all_frames = (int)std::min((long long)first_event + max_frames_, n_frames);

    // =========================================================================
    //  Histograms — Part 1: statistics
    // =========================================================================

    TH1F *h_mult_all = new TH1F("h_mult_all",
        "Track multiplicity — all triggered events;n tracks;counts", 8, -0.5, 7.5);

    // chi2/NDF per track, split by event multiplicity
    TH1F *h_chi2_1trk  = new TH1F("h_chi2_1trk",
        "#chi^{2}/NDF — 1-track events;#chi^{2}/NDF;counts", 100, 0, 20);
    TH1F *h_chi2_multi = new TH1F("h_chi2_multi",
        "#chi^{2}/NDF — multi-track events;#chi^{2}/NDF;counts", 100, 0, 20);

    // theta angle per track
    TH1F *h_theta_1trk  = new TH1F("h_theta_1trk",
        "#theta — 1-track events;#theta (rad);counts", 500, 0, 0.1);
    TH1F *h_theta_multi = new TH1F("h_theta_multi",
        "#theta — multi-track events;#theta (rad);counts", 500, 0, 0.1);

    // angle_x, angle_y per track
    TH1F *h_ax_1trk  = new TH1F("h_ax_1trk",
        "#alpha_{x} — 1-track;#alpha_{x} (rad);counts", 500, -0.1, 0.1);
    TH1F *h_ay_1trk  = new TH1F("h_ay_1trk",
        "#alpha_{y} — 1-track;#alpha_{y} (rad);counts", 500, -0.1, 0.1);
    TH1F *h_ax_multi = new TH1F("h_ax_multi",
        "#alpha_{x} — multi-track;#alpha_{x} (rad);counts", 500, -0.1, 0.1);
    TH1F *h_ay_multi = new TH1F("h_ay_multi",
        "#alpha_{y} — multi-track;#alpha_{y} (rad);counts", 500, -0.1, 0.1);

    // =========================================================================
    //  Histograms — Part 2: vertex finding
    // =========================================================================

    TH1F *h_vertex_z = new TH1F("h_vertex_z",
        "Back-projected vertex z (best pair per event);z_{vertex} (mm);events",
        200, vz_min, vz_max);

    TH1F *h_vertex_dca = new TH1F("h_vertex_dca",
        "DCA at vertex (best pair per event);DCA (mm);events",
        200, 0, 50);

    TH2F *h2_vertex_xy = new TH2F("h2_vertex_xy",
        "Vertex (x,y) at z_{vertex} (best pair, DCA cut applied);"
        "x_{vertex} (mm);y_{vertex} (mm)",
        200, -200, 200, 200, -200, 200);

    TH2F *h2_vertex_z_dca = new TH2F("h2_vertex_z_dca",
        "Vertex z vs DCA (best pair per event);"
        "z_{vertex} (mm);DCA (mm)",
        200, vz_min, vz_max, 100, 0, 50);

    // all pairs (not just best) — gives full vertex z distribution
    TH1F *h_vertex_z_allpairs = new TH1F("h_vertex_z_allpairs",
        "Vertex z — all track pairs;z_{vertex} (mm);pairs", 200, vz_min, vz_max);

    // back-projection display: (z, x(z)) and (z, y(z)) for all tracks
    // in multi-track events, over [vz_min, vz_max]
    const int bp_bins_z = 300;
    const int bp_bins_xy = 300;
    TH2F *h2_bp_xz = new TH2F("h2_bp_xz",
        "Back-projection x–z (multi-track events);"
        "z (mm);x(z) (mm)",
        bp_bins_z, vz_min, vz_max, bp_bins_xy, -200, 200);
    TH2F *h2_bp_yz = new TH2F("h2_bp_yz",
        "Back-projection y–z (multi-track events);"
        "z (mm);y(z) (mm)",
        bp_bins_z, vz_min, vz_max, bp_bins_xy, -200, 200);

    // back-projection from the reconstructed vertex to the tracker
    // (secondary tracks drawn only from z_vertex onward)
    TH2F *h2_bp_xz_fv = new TH2F("h2_bp_xz_fv",
        "Back-projection x–z from vertex (DCA cut applied);"
        "z (mm);x(z) (mm)",
        bp_bins_z, vz_min, vz_max, bp_bins_xy, -200, 200);
    TH2F *h2_bp_yz_fv = new TH2F("h2_bp_yz_fv",
        "Back-projection y–z from vertex (DCA cut applied);"
        "z (mm);y(z) (mm)",
        bp_bins_z, vz_min, vz_max, bp_bins_xy, -200, 200);

    // =========================================================================
    //  Counters
    // =========================================================================
    int n_no_trigger  = 0, n_spill_start = 0, n_total = 0;
    int n_0trk = 0, n_1trk = 0, n_2trk = 0, n_3trk = 0, n_4plus = 0;
    int n_vertex_found = 0;  // events where a valid vertex was found

    // =========================================================================
    //  Event loop
    // =========================================================================
    const float z_step = (vz_max - vz_min) / n_z_steps;

    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 1000 == 0) bar.update(i_frame, all_frames);

        if (recotrackdata->is_start_of_spill()) { ++n_spill_start; continue; }
        auto beam_trig = recotrackdata->get_trigger_by_index(0);
        if (!beam_trig) { ++n_no_trigger; continue; }

        ++n_total;
        const int n_trk = recotrackdata->n_recotrackdata();

        // --- Part 1: statistics ---
        h_mult_all->Fill(n_trk);
        if      (n_trk == 0) ++n_0trk;
        else if (n_trk == 1) ++n_1trk;
        else if (n_trk == 2) ++n_2trk;
        else if (n_trk == 3) ++n_3trk;
        else                 ++n_4plus;

        for (int i = 0; i < n_trk; i++)
        {
            const float chi2  = recotrackdata->get_chi2ndof(i);
            const float theta = recotrackdata->get_traj_angcoeff_theta(i);
            const float ax    = std::atan(recotrackdata->get_traj_angcoeff_x(i));
            const float ay    = std::atan(recotrackdata->get_traj_angcoeff_y(i));

            if (n_trk == 1)
            {
                h_chi2_1trk ->Fill(chi2);
                h_theta_1trk->Fill(theta);
                h_ax_1trk   ->Fill(ax);
                h_ay_1trk   ->Fill(ay);
            }
            else
            {
                h_chi2_multi ->Fill(chi2);
                h_theta_multi->Fill(theta);
                h_ax_multi   ->Fill(ax);
                h_ay_multi   ->Fill(ay);
            }
        }

        // --- Part 2: vertex finding (multi-track only) ---
        if (n_trk < 2) continue;

        // collect valid tracks (optional chi2 cut + geometric selection from conf)
        std::vector<int> valid;
        for (int i = 0; i < n_trk; i++)
        {
            if (apply_chi2 && recotrackdata->get_chi2ndof(i) > chi2_max) continue;
            const float px = recotrackdata->get_det_plane_x(i);
            const float py = recotrackdata->get_det_plane_y(i);
            const float sx = recotrackdata->get_traj_angcoeff_x(i);
            const float sy = recotrackdata->get_traj_angcoeff_y(i);
            if (!is_track_selected(px, py, sx, sy,
                                   tsel.cutg1, tsel.plane1, tsel.side1,
                                   tsel.cutg2, tsel.plane2, tsel.side2,
                                   tsel.z_drich, tsel.z_scint))
                continue;
            valid.push_back(i);
        }
        if ((int)valid.size() < 2) continue;

        // back-projection display: fill (z, x(z)), (z, y(z)) for each valid track
        for (int idx : valid)
        {
            const float px = recotrackdata->get_det_plane_x(idx);
            const float py = recotrackdata->get_det_plane_y(idx);
            const float sx = recotrackdata->get_traj_angcoeff_x(idx);
            const float sy = recotrackdata->get_traj_angcoeff_y(idx);
            for (int iz = 0; iz <= n_z_steps; ++iz)
            {
                float z = vz_min + iz * z_step;
                h2_bp_xz->Fill(z, px + sx * z);
                h2_bp_yz->Fill(z, py + sy * z);
            }
        }

        // vertex finding: all pairs
        float best_dca = 1.e9f;
        float best_zv  = 0.f, best_xv = 0.f, best_yv = 0.f;

        for (int ii = 0; ii < (int)valid.size(); ++ii)
        for (int jj = ii + 1; jj < (int)valid.size(); ++jj)
        {
            const int i = valid[ii], j = valid[jj];
            const float pxi = recotrackdata->get_det_plane_x(i);
            const float pyi = recotrackdata->get_det_plane_y(i);
            const float sxi = recotrackdata->get_traj_angcoeff_x(i);
            const float syi = recotrackdata->get_traj_angcoeff_y(i);
            const float pxj = recotrackdata->get_det_plane_x(j);
            const float pyj = recotrackdata->get_det_plane_y(j);
            const float sxj = recotrackdata->get_traj_angcoeff_x(j);
            const float syj = recotrackdata->get_traj_angcoeff_y(j);

            const float dpx = pxi - pxj;
            const float dpy = pyi - pyj;
            const float dsx = sxi - sxj;
            const float dsy = syi - syj;
            const float denom = dsx * dsx + dsy * dsy;

            if (denom < 1.e-12f) continue; // parallel tracks

            const float zv = -(dpx * dsx + dpy * dsy) / denom;

            // only consider vertices within the configured z range
            if (zv < vz_min || zv > vz_max) continue;

            const float xi  = pxi + sxi * zv;
            const float yi  = pyi + syi * zv;
            const float xj  = pxj + sxj * zv;
            const float yj  = pyj + syj * zv;
            const float dca = std::hypot(xi - xj, yi - yj);

            h_vertex_z_allpairs->Fill(zv);

            if (dca < best_dca)
            {
                best_dca = dca;
                best_zv  = zv;
                best_xv  = 0.5f * (xi + xj);
                best_yv  = 0.5f * (yi + yj);
            }
        }

        if (best_dca < 1.e8f)  // at least one valid pair found
        {
            ++n_vertex_found;
            h_vertex_z  ->Fill(best_zv);
            h_vertex_dca->Fill(best_dca);
            h2_vertex_z_dca->Fill(best_zv, best_dca);
            if (best_dca < dca_max)
            {
                h2_vertex_xy->Fill(best_xv, best_yv);
                // back-projection from vertex: each track drawn only for z >= best_zv
                const float zfv_step = (vz_max - best_zv) / n_z_steps;
                for (int idx : valid)
                {
                    const float px = recotrackdata->get_det_plane_x(idx);
                    const float py = recotrackdata->get_det_plane_y(idx);
                    const float sx = recotrackdata->get_traj_angcoeff_x(idx);
                    const float sy = recotrackdata->get_traj_angcoeff_y(idx);
                    for (int iz = 0; iz <= n_z_steps; ++iz)
                    {
                        const float z = best_zv + iz * zfv_step;
                        h2_bp_xz_fv->Fill(z, px + sx * z);
                        h2_bp_yz_fv->Fill(z, py + sy * z);
                    }
                }
            }
        }
    }
    bar.update(all_frames, all_frames);
    bar.finish();

    // =========================================================================
    //  Statistics
    // =========================================================================
    auto pct = [](int num, int den) -> float {
        return den > 0 ? 100.f * (float)num / (float)den : 0.f;
    };

    std::cout << "========================================" << std::endl;
    std::cout << "Beam analysis" << std::endl;
    std::cout << "  Total triggered:  " << n_total << std::endl;
    std::cout << "  No track:         " << n_0trk
              << Form("  (%.1f%%)", pct(n_0trk,  n_total)) << std::endl;
    std::cout << "  1 track:          " << n_1trk
              << Form("  (%.1f%%)", pct(n_1trk,  n_total)) << std::endl;
    std::cout << "  2 tracks:         " << n_2trk
              << Form("  (%.1f%%)", pct(n_2trk,  n_total)) << std::endl;
    std::cout << "  3 tracks:         " << n_3trk
              << Form("  (%.1f%%)", pct(n_3trk,  n_total)) << std::endl;
    std::cout << "  4+ tracks:        " << n_4plus
              << Form("  (%.1f%%)", pct(n_4plus, n_total)) << std::endl;
    const int n_multi = n_2trk + n_3trk + n_4plus;
    std::cout << "  Multi-track tot:  " << n_multi
              << Form("  (%.1f%%)", pct(n_multi, n_total)) << std::endl;
    std::cout << "  Vertices found:   " << n_vertex_found
              << Form("  (%.1f%% of multi)", pct(n_vertex_found, n_multi)) << std::endl;
    std::cout << "========================================" << std::endl;

    std::ofstream stats(output_dir + "/beam_stats.txt");
    stats << "n_triggered\t"   << n_total       << "\n";
    stats << "n_0trk\t"        << n_0trk        << "\n";
    stats << "n_1trk\t"        << n_1trk        << "\n";
    stats << "n_2trk\t"        << n_2trk        << "\n";
    stats << "n_3trk\t"        << n_3trk        << "\n";
    stats << "n_4plus\t"       << n_4plus       << "\n";
    stats << "n_multi\t"       << n_multi       << "\n";
    stats << "pct_0trk\t"      << pct(n_0trk,  n_total) << "\n";
    stats << "pct_1trk\t"      << pct(n_1trk,  n_total) << "\n";
    stats << "pct_2trk\t"      << pct(n_2trk,  n_total) << "\n";
    stats << "pct_3trk\t"      << pct(n_3trk,  n_total) << "\n";
    stats << "pct_4plus\t"     << pct(n_4plus, n_total) << "\n";
    stats << "pct_multi\t"     << pct(n_multi, n_total) << "\n";
    stats << "n_vertex_found\t"<< n_vertex_found        << "\n";
    stats.close();

    // =========================================================================
    //  Plots
    // =========================================================================
    gROOT->SetBatch(true);

    // --- multiplicity bar chart with percentages ---
    {
        TCanvas *c = new TCanvas("c_mult", "Event composition", 900, 600);
        c->SetLeftMargin(0.12);
        h_mult_all->SetFillColor(kAzure+7);
        h_mult_all->SetLineColor(kBlue+2);
        h_mult_all->SetLineWidth(2);
        h_mult_all->Draw("BAR2");
        // label each bar with percentage
        TLatex lat;
        lat.SetTextSize(0.04);
        lat.SetTextAlign(21);
        int counts[5] = {n_0trk, n_1trk, n_2trk, n_3trk, n_4plus};
        for (int b = 0; b < 5; b++)
        {
            float y = counts[b] + h_mult_all->GetMaximum() * 0.03f;
            lat.DrawLatex(b, y, Form("%.1f%%", pct(counts[b], n_total)));
        }
        c->SaveAs(Form("%s/beam_multiplicity.png", output_dir.c_str()));
        delete c;
    }

    // --- chi2 comparison ---
    {
        TCanvas *c = new TCanvas("c_chi2", "#chi^{2}/NDF: 1-track vs multi-track", 900, 600);
        h_chi2_1trk ->SetLineColor(kBlue+1);  h_chi2_1trk ->SetLineWidth(2);
        h_chi2_multi->SetLineColor(kRed+1);   h_chi2_multi->SetLineWidth(2);
        double ymax = std::max(h_chi2_1trk->GetMaximum(), h_chi2_multi->GetMaximum());
        h_chi2_1trk->SetMaximum(ymax * 1.25);
        h_chi2_1trk ->Draw("HIST");
        h_chi2_multi->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.55, 0.72, 0.88, 0.88);
        leg->AddEntry(h_chi2_1trk,  "1-track",    "l");
        leg->AddEntry(h_chi2_multi, "multi-track","l");
        leg->Draw();
        c->SaveAs(Form("%s/beam_chi2.png", output_dir.c_str()));
        delete c;
    }

    // --- theta comparison ---
    {
        TCanvas *c = new TCanvas("c_theta", "#theta: 1-track vs multi-track", 900, 600);
        h_theta_1trk ->SetLineColor(kBlue+1); h_theta_1trk ->SetLineWidth(2);
        h_theta_multi->SetLineColor(kRed+1);  h_theta_multi->SetLineWidth(2);
        double ymax = std::max(h_theta_1trk->GetMaximum(), h_theta_multi->GetMaximum());
        h_theta_1trk->SetMaximum(ymax * 1.25);
        h_theta_1trk ->Draw("HIST");
        h_theta_multi->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.55, 0.72, 0.88, 0.88);
        leg->AddEntry(h_theta_1trk,  "1-track",    "l");
        leg->AddEntry(h_theta_multi, "multi-track","l");
        leg->Draw();
        c->SaveAs(Form("%s/beam_theta.png", output_dir.c_str()));
        delete c;
    }

    // --- vertex z ---
    {
        TCanvas *c = new TCanvas("c_vz", "Vertex z (back-projection, best pair)", 900, 600);
        h_vertex_z->SetLineColor(kBlue+1); h_vertex_z->SetLineWidth(2);
        h_vertex_z->Draw("HIST");
        // mark detector positions
        auto vline = [&](float z, const char *label, int color) {
            TLine *l = new TLine(z, 0, z, h_vertex_z->GetMaximum() * 1.0);
            l->SetLineStyle(2); l->SetLineColor(color); l->SetLineWidth(2); l->Draw();
            TLatex lat; lat.SetTextSize(0.028); lat.SetTextColor(color);
            lat.DrawLatex(z, h_vertex_z->GetMaximum() * 1.05, label);
        };
        vline(z_drich, "dRICH",  kRed+1);
        vline(z_scint, "Scint",  kGreen+2);
        vline(0,       "Tracker",kGray+1);
        c->SaveAs(Form("%s/beam_vertex_z.png", output_dir.c_str()));
        delete c;
    }

    // --- vertex z all pairs overlaid ---
    {
        TCanvas *c = new TCanvas("c_vz_all", "Vertex z: best pair vs all pairs", 900, 600);
        h_vertex_z_allpairs->SetLineColor(kGray+1); h_vertex_z_allpairs->SetLineWidth(1);
        h_vertex_z         ->SetLineColor(kBlue+1); h_vertex_z         ->SetLineWidth(2);
        double ymax = std::max(h_vertex_z->GetMaximum(), h_vertex_z_allpairs->GetMaximum());
        h_vertex_z_allpairs->SetMaximum(ymax * 1.25);
        h_vertex_z_allpairs->Draw("HIST");
        h_vertex_z->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.12, 0.72, 0.50, 0.88);
        leg->AddEntry(h_vertex_z,          "best pair / event", "l");
        leg->AddEntry(h_vertex_z_allpairs, "all pairs",         "l");
        leg->Draw();
        c->SaveAs(Form("%s/beam_vertex_z_compare.png", output_dir.c_str()));
        delete c;
    }

    // --- vertex DCA ---
    {
        TCanvas *c = new TCanvas("c_dca", "DCA at vertex", 900, 600);
        h_vertex_dca->SetLineColor(kBlue+1); h_vertex_dca->SetLineWidth(2);
        h_vertex_dca->Draw("HIST");
        c->SaveAs(Form("%s/beam_vertex_dca.png", output_dir.c_str()));
        delete c;
    }

    // --- vertex xy ---
    {
        TCanvas *c = new TCanvas("c_vxy", "Vertex (x,y) at z_{vertex}", 900, 800);
        h2_vertex_xy->Draw("COLZ");
        if (h2_vertex_xy->GetEntries() > 0) gPad->SetLogz(1);
        c->SaveAs(Form("%s/beam_vertex_xy.png", output_dir.c_str()));
        delete c;
    }

    // --- vertex z vs DCA ---
    {
        TCanvas *c = new TCanvas("c_vz_dca", "Vertex z vs DCA", 900, 700);
        h2_vertex_z_dca->Draw("COLZ");
        if (h2_vertex_z_dca->GetEntries() > 0) gPad->SetLogz(1);
        // mark detector z
        TLine *ld = new TLine(z_drich, 0, z_drich, 50);
        ld->SetLineStyle(2); ld->SetLineColor(kRed+1); ld->SetLineWidth(2); ld->Draw();
        TLine *ls = new TLine(z_scint, 0, z_scint, 50);
        ls->SetLineStyle(2); ls->SetLineColor(kGreen+2); ls->SetLineWidth(2); ls->Draw();
        c->SaveAs(Form("%s/beam_vertex_z_dca.png", output_dir.c_str()));
        delete c;
    }

    // --- back-projection xz and yz ---
    {
        TCanvas *c = new TCanvas("c_bp", "Back-projection (multi-track events)", 1600, 700);
        c->Divide(2, 1);
        c->cd(1); h2_bp_xz->Draw("COLZ"); if (h2_bp_xz->GetEntries() > 0) gPad->SetLogz(1);
        // mark detector positions
        auto hline_z = [&](TH2F *h, float z, int color) {
            TLine *l = new TLine(z, h->GetYaxis()->GetXmin(), z, h->GetYaxis()->GetXmax());
            l->SetLineStyle(2); l->SetLineColor(color); l->SetLineWidth(2); l->Draw();
        };
        hline_z(h2_bp_xz, z_drich, kRed+1);
        hline_z(h2_bp_xz, z_scint, kGreen+2);
        hline_z(h2_bp_xz, 0,       kGray+1);
        c->cd(2); h2_bp_yz->Draw("COLZ"); if (h2_bp_yz->GetEntries() > 0) gPad->SetLogz(1);
        hline_z(h2_bp_yz, z_drich, kRed+1);
        hline_z(h2_bp_yz, z_scint, kGreen+2);
        hline_z(h2_bp_yz, 0,       kGray+1);
        c->SaveAs(Form("%s/beam_backproj.png", output_dir.c_str()));
        delete c;
    }

    // --- back-projection from vertex (DCA cut applied) ---
    {
        TCanvas *c = new TCanvas("c_bp_fv", "Back-projection from vertex (DCA cut)", 1600, 700);
        c->Divide(2, 1);
        c->cd(1); h2_bp_xz_fv->Draw("COLZ"); if (h2_bp_xz_fv->GetEntries() > 0) gPad->SetLogz(1);
        auto hline_z_fv = [&](TH2F *h, float z, int color) {
            TLine *l = new TLine(z, h->GetYaxis()->GetXmin(), z, h->GetYaxis()->GetXmax());
            l->SetLineStyle(2); l->SetLineColor(color); l->SetLineWidth(2); l->Draw();
        };
        hline_z_fv(h2_bp_xz_fv, z_drich, kRed+1);
        hline_z_fv(h2_bp_xz_fv, z_scint, kGreen+2);
        hline_z_fv(h2_bp_xz_fv, 0,       kGray+1);
        c->cd(2); h2_bp_yz_fv->Draw("COLZ"); if (h2_bp_yz_fv->GetEntries() > 0) gPad->SetLogz(1);
        hline_z_fv(h2_bp_yz_fv, z_drich, kRed+1);
        hline_z_fv(h2_bp_yz_fv, z_scint, kGreen+2);
        hline_z_fv(h2_bp_yz_fv, 0,       kGray+1);
        c->SaveAs(Form("%s/beam_backproj_from_vertex.png", output_dir.c_str()));
        delete c;
    }

    // =========================================================================
    //  Save ROOT file
    // =========================================================================
    std::string output_root = output_dir + "/beam.root";
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");
    for (auto &kv : cfg._data)
        TNamed(kv.first.c_str(), kv.second.c_str()).Write();
    TNamed("run_name", run_name.c_str()).Write();
    h_mult_all->Write();
    h_chi2_1trk->Write();  h_chi2_multi->Write();
    h_theta_1trk->Write(); h_theta_multi->Write();
    h_ax_1trk->Write();    h_ay_1trk->Write();
    h_ax_multi->Write();   h_ay_multi->Write();
    h_vertex_z->Write();   h_vertex_z_allpairs->Write();
    h_vertex_dca->Write();
    h2_vertex_xy->Write();
    h2_vertex_z_dca->Write();
    h2_bp_xz->Write();     h2_bp_yz->Write();
    h2_bp_xz_fv->Write(); h2_bp_yz_fv->Write();
    fout->Close();

    std::cout << "Output: " << output_root << std::endl;
}
