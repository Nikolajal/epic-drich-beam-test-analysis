#include "../lib_loader.h"
#include "ringtrack_config.h"
#include "ringtrack_track_selection.h"
#include <fstream>
#include <vector>
#include <algorithm>
#include <random>
#include <cmath>

// =============================================================================
//  ringtrack_ring  —  track-referenced Cherenkov ring analysis
//
//  Core idea: for each event with a reconstructed track, compute the distance
//  of every dRICH hit (in time window) from the track intercept at z_dRICH:
//
//      r_i = sqrt((x_i - x_trk)^2 + (y_i - y_trk)^2)
//
//  where  x_trk = p_x + s_x * z_drich,   y_trk = p_y + s_y * z_drich.
//
//  The Cherenkov signal produces a peak in the *stacked* r distribution at R_C.
//  Background hits have no correlation with the track intercept and produce a
//  smooth (geometrically shaped) baseline.
//
//  Key demonstration — real vs shuffled intercepts:
//    Pass 1: fill h_r_real using the actual track intercept of each event.
//    Pass 2: redo the same computation using a randomly shuffled permutation
//            of stored intercepts (different event's track for each frame).
//    A peak at R_C that disappears with shuffled intercepts proves that tracks
//    are necessary to identify the ring signal.
//
//  Per-event quality metric — hit density ratio:
//    N_on  = hits with |r_i - R_C| < ring_dr
//    N_off = total hits in window - N_on
//    A_on  = 2 * pi * R_C * ring_dr          (area of on-ring annulus)
//    A_tot = pi * r_max^2                     (active area approximation)
//    A_off = A_tot - A_on
//    Q = (N_on / A_on) / (N_off / A_off)     (density ratio; signal: Q >> 1)
//
//  With few hits, Q has large Poisson fluctuations — h2_Q_vs_nhits shows
//  how separation between signal and background grows with multiplicity.
//
//  Conf keys (beyond standard ones):
//    ring_r_min       — min radius for r histogram [mm]       (default   0)
//    ring_r_max       — max radius for r histogram [mm]       (default 120)
//    ring_r_bins      — bins in r histogram                   (default 120)
//    ring_r_expected  — expected Cherenkov radius R_C [mm]    (0 = auto from peak)
//    ring_dr          — half-width of on-ring annulus [mm]    (default   5)
//    ring_seed        — random seed for intercept shuffling   (default  42)
//
//  Outputs:
//    ring.root
//    ring_stats.txt
//    ring_r_stacked.png       — stacked r: real intercepts vs shuffled
//    ring_r_vs_nhits.png      — 2D: r vs N_hits per event (real intercepts)
//    ring_density_ratio.png   — per-event Q distribution
//    ring_density_vs_nhits.png — 2D: Q vs N_hits
//    ring_nhits_onoff.png     — 2D: N_on vs N_off
// =============================================================================
void ringtrack_ring(std::string data_repository, std::string run_name,
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

    const int   first_event         = cfg.get_int  ("first_event",        0);
    const int   max_frames_         = cfg.get_int  ("max_frames",   1000000);
    const float z_drich             = cfg.get_float("z_drich",       -4250.f);
    const bool  apply_afterpulse    = cfg.get_bool ("apply_afterpulse_cut", true);
    const std::array<float,2> tcut  = {
        cfg.get_float("time_cut_min", -50.f),
        cfg.get_float("time_cut_max",  20.f)
    };

    const float r_min        = cfg.get_float("ring_r_min",        0.f);
    const float r_max        = cfg.get_float("ring_r_max",      120.f);
    const int   r_bins       = cfg.get_int  ("ring_r_bins",       120);
    const float r_expected   = cfg.get_float("ring_r_expected",   0.f);  // 0 = auto
    const float ring_dr      = cfg.get_float("ring_dr",           5.f);
    const int   ring_seed    = cfg.get_int  ("ring_seed",          42);

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
    //  Histograms
    // =========================================================================

    // stacked r distributions: real intercept vs shuffled
    TH1F *h_r_real = new TH1F("h_r_real",
        "r = dist(hit, track intercept) — real intercepts;"
        "r (mm);hits / event (normalised)", r_bins, r_min, r_max);
    TH1F *h_r_shuffled = new TH1F("h_r_shuffled",
        "r — shuffled intercepts (background reference);"
        "r (mm);hits / event (normalised)", r_bins, r_min, r_max);

    // 2D: r vs N_hits in event (real intercepts)
    TH2F *h2_r_vs_nhits = new TH2F("h2_r_vs_nhits",
        "r vs N_{hits} in time window (real intercepts);"
        "N_{hits} in window;r (mm)",
        51, -0.5, 50.5, r_bins, r_min, r_max);

    // per-event density ratio Q = (N_on/A_on) / (N_off/A_off)
    TH1F *h_Q = new TH1F("h_Q",
        "Per-event ring density ratio Q = (#rho_{on}/#rho_{off});"
        "Q;events", 100, 0, 20);

    // 2D: Q vs N_hits
    TH2F *h2_Q_vs_nhits = new TH2F("h2_Q_vs_nhits",
        "Ring density ratio Q vs N_{hits};"
        "N_{hits} in window;Q = #rho_{on}/#rho_{off}",
        51, -0.5, 50.5, 100, 0, 20);

    // 2D: N_on vs N_off
    TH2F *h2_non_noff = new TH2F("h2_non_noff",
        "N_{on-ring} vs N_{off-ring} (|r - R_{C}| < #deltaR);"
        "N_{off} (hits outside annulus);N_{on} (hits inside annulus)",
        31, -0.5, 30.5, 31, -0.5, 30.5);

    // track intercept map at z_dRICH (selected events)
    TH2F *h2_intercept = new TH2F("h2_intercept",
        "Track intercept at z_{dRICH} (selected events);"
        "x_{trk} (mm);y_{trk} (mm)",
        200, -200, 200, 200, -200, 200);

    // =========================================================================
    //  Pass 1: fill real-intercept histograms, store intercepts
    // =========================================================================
    struct Intercept { float x, y; };
    std::vector<Intercept> intercepts;  // one per selected event

    int n_selected = 0, n_total_hits = 0;

    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    std::cout << "[ring] Pass 1: real intercepts" << std::endl;

    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 1000 == 0) bar.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;

        auto trig = recotrackdata->get_trigger_by_index(0);
        if (!trig) continue;

        int trk_idx = -1;
        if (!tsel.select_event(recotrackdata, trk_idx)) continue;

        const float px  = recotrackdata->get_det_plane_x(trk_idx);
        const float py  = recotrackdata->get_det_plane_y(trk_idx);
        const float sx  = recotrackdata->get_traj_angcoeff_x(trk_idx);
        const float sy  = recotrackdata->get_traj_angcoeff_y(trk_idx);
        const float xtr = px + sx * z_drich;
        const float ytr = py + sy * z_drich;

        intercepts.push_back({xtr, ytr});
        h2_intercept->Fill(xtr, ytr);
        ++n_selected;

        // collect hits in time window
        int n_hits = 0;
        const auto &hits = recotrackdata->get_recodata();
        for (int ih = 0; ih < (int)hits.size(); ++ih)
        {
            if (apply_afterpulse && recotrackdata->is_afterpulse(ih)) continue;
            const float dt = recotrackdata->get_hit_t(ih) - trig->fine_time;
            if (dt < tcut[0] || dt > tcut[1]) continue;
            ++n_hits;
            const float xi = recotrackdata->get_hit_x_rnd(ih);
            const float yi = recotrackdata->get_hit_y_rnd(ih);
            const float r  = std::hypot(xi - xtr, yi - ytr);
            h_r_real->Fill(r);
            h2_r_vs_nhits->Fill(n_hits, r);  // filled per-hit; n_hits will be corrected below
        }
        n_total_hits += n_hits;

        // density ratio Q (needs R_C — filled after peak detection below)
        // stored in a second loop after pass 1 is complete
    }
    bar.update(all_frames, all_frames);
    bar.finish();

    // =========================================================================
    //  Detect R_C: either from conf or from peak of h_r_real
    // =========================================================================
    float R_C = r_expected;
    if (R_C <= 0.f && h_r_real->GetEntries() > 0)
    {
        int peak_bin = h_r_real->GetMaximumBin();
        R_C = (float)h_r_real->GetBinCenter(peak_bin);
        std::cout << "[ring] R_C auto-detected from peak: " << R_C << " mm" << std::endl;
    }
    else if (R_C > 0.f)
        std::cout << "[ring] R_C from conf: " << R_C << " mm" << std::endl;
    else
    {
        std::cerr << "[ring] WARNING: no data and no ring_r_expected — skipping Q fill" << std::endl;
    }

    // =========================================================================
    //  Pass 1b: fill per-event Q and N_on/N_off using detected R_C
    //  (second pass over the tree — same events)
    // =========================================================================
    const float A_on  = 2.f * M_PI * R_C * ring_dr;
    const float A_tot = M_PI * r_max * r_max;
    const float A_off = std::max(A_tot - A_on, 1.f);

    // reset h2_r_vs_nhits to fill correctly (per-event n_hits on x axis)
    h2_r_vs_nhits->Reset();

    int intercept_idx = 0;
    std::cout << "[ring] Pass 1b: per-event Q and corrected r vs nhits" << std::endl;
    bar = mist::logger::progress_bar(mist::logger::bar_style::BLOCK);

    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 1000 == 0) bar.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;

        auto trig = recotrackdata->get_trigger_by_index(0);
        if (!trig) continue;

        int trk_idx = -1;
        if (!tsel.select_event(recotrackdata, trk_idx)) continue;

        if (intercept_idx >= (int)intercepts.size()) break;
        const float xtr = intercepts[intercept_idx].x;
        const float ytr = intercepts[intercept_idx].y;
        ++intercept_idx;

        const auto &hits = recotrackdata->get_recodata();
        int n_hits = 0, n_on = 0, n_off = 0;
        std::vector<float> rs;

        for (int ih = 0; ih < (int)hits.size(); ++ih)
        {
            if (apply_afterpulse && recotrackdata->is_afterpulse(ih)) continue;
            const float dt = recotrackdata->get_hit_t(ih) - trig->fine_time;
            if (dt < tcut[0] || dt > tcut[1]) continue;
            ++n_hits;
            const float xi = recotrackdata->get_hit_x_rnd(ih);
            const float yi = recotrackdata->get_hit_y_rnd(ih);
            const float r  = std::hypot(xi - xtr, yi - ytr);
            rs.push_back(r);
            if (R_C > 0.f && std::fabs(r - R_C) < ring_dr) ++n_on;
            else ++n_off;
        }

        for (float r : rs) h2_r_vs_nhits->Fill(n_hits, r);

        if (R_C > 0.f && n_hits > 0)
        {
            const float rho_on  = (A_on  > 0.f) ? n_on  / A_on  : 0.f;
            const float rho_off = (A_off > 0.f) ? n_off / A_off : 1e-6f;
            const float Q = (rho_off > 0.f) ? rho_on / rho_off : 0.f;
            h_Q->Fill(Q);
            h2_Q_vs_nhits->Fill(n_hits, Q);
            h2_non_noff->Fill(n_off, n_on);
        }
    }
    bar.update(all_frames, all_frames);
    bar.finish();

    // =========================================================================
    //  Pass 2: shuffled intercepts (background reference)
    // =========================================================================
    std::cout << "[ring] Pass 2: shuffled intercepts" << std::endl;
    std::vector<int> perm(intercepts.size());
    std::iota(perm.begin(), perm.end(), 0);
    std::mt19937 rng(ring_seed);
    std::shuffle(perm.begin(), perm.end(), rng);

    intercept_idx = 0;
    bar = mist::logger::progress_bar(mist::logger::bar_style::BLOCK);

    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 1000 == 0) bar.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;

        auto trig = recotrackdata->get_trigger_by_index(0);
        if (!trig) continue;

        int trk_idx = -1;
        if (!tsel.select_event(recotrackdata, trk_idx)) continue;

        if (intercept_idx >= (int)perm.size()) break;
        // use shuffled intercept from a different event
        const float xtr = intercepts[perm[intercept_idx]].x;
        const float ytr = intercepts[perm[intercept_idx]].y;
        ++intercept_idx;

        const auto &hits = recotrackdata->get_recodata();
        for (int ih = 0; ih < (int)hits.size(); ++ih)
        {
            if (apply_afterpulse && recotrackdata->is_afterpulse(ih)) continue;
            const float dt = recotrackdata->get_hit_t(ih) - trig->fine_time;
            if (dt < tcut[0] || dt > tcut[1]) continue;
            const float xi = recotrackdata->get_hit_x_rnd(ih);
            const float yi = recotrackdata->get_hit_y_rnd(ih);
            h_r_shuffled->Fill(std::hypot(xi - xtr, yi - ytr));
        }
    }
    bar.update(all_frames, all_frames);
    bar.finish();

    // =========================================================================
    //  Normalise r histograms per selected event
    // =========================================================================
    if (n_selected > 0)
    {
        h_r_real    ->Scale(1.0 / n_selected);
        h_r_shuffled->Scale(1.0 / n_selected);
    }

    // =========================================================================
    //  Statistics
    // =========================================================================
    auto pct = [](int a, int b) -> float {
        return b > 0 ? 100.f * (float)a / (float)b : 0.f;
    };

    std::cout << "========================================" << std::endl;
    std::cout << "Ring analysis" << std::endl;
    std::cout << "  Selected events:   " << n_selected    << std::endl;
    std::cout << "  Total hits (win):  " << n_total_hits  << std::endl;
    std::cout << Form("  R_C used:          %.1f mm (half-width %.1f mm)", R_C, ring_dr) << std::endl;
    std::cout << "  Mean hits/event:   "
              << (n_selected > 0 ? (float)n_total_hits / n_selected : 0.f) << std::endl;
    std::cout << "========================================" << std::endl;

    std::ofstream stats(output_dir + "/ring_stats.txt");
    stats << "n_selected\t"    << n_selected   << "\n";
    stats << "n_total_hits\t"  << n_total_hits << "\n";
    stats << "R_C_mm\t"        << R_C          << "\n";
    stats << "ring_dr_mm\t"    << ring_dr      << "\n";
    stats << "mean_hits_per_event\t"
          << (n_selected > 0 ? (float)n_total_hits / n_selected : 0.f) << "\n";
    stats.close();

    // =========================================================================
    //  Plots
    // =========================================================================
    gROOT->SetBatch(true);

    // helper: vertical line at R_C
    auto vline = [&](float x, int col) {
        TLine *l = new TLine(x, 0, x,
            std::max(h_r_real->GetMaximum(), h_r_shuffled->GetMaximum()) * 1.1);
        l->SetLineStyle(2); l->SetLineColor(col); l->SetLineWidth(2); l->Draw();
    };

    // --- r stacked: real vs shuffled ---
    {
        TCanvas *c = new TCanvas("c_r", "r distribution: real vs shuffled", 900, 600);
        h_r_real    ->SetLineColor(kBlue+1);  h_r_real    ->SetLineWidth(2);
        h_r_shuffled->SetLineColor(kGray+1);  h_r_shuffled->SetLineWidth(2);
        h_r_shuffled->SetFillColor(kGray);    h_r_shuffled->SetFillStyle(1001);
        double ymax = std::max(h_r_real->GetMaximum(), h_r_shuffled->GetMaximum());
        h_r_shuffled->SetMaximum(ymax * 1.25);
        h_r_shuffled->SetTitle(
            "r = dist(hit, track intercept) — real vs shuffled;"
            "r (mm);hits per event");
        h_r_shuffled->Draw("HIST");
        h_r_real    ->Draw("HIST SAME");
        if (R_C > 0.f) vline(R_C, kRed+1);
        TLegend *leg = new TLegend(0.55, 0.72, 0.88, 0.88);
        leg->AddEntry(h_r_real,     "real intercepts",     "l");
        leg->AddEntry(h_r_shuffled, "shuffled (bkg ref.)", "f");
        if (R_C > 0.f)
        {
            TLine *lR = new TLine(); lR->SetLineColor(kRed+1); lR->SetLineStyle(2); lR->SetLineWidth(2);
            leg->AddEntry(lR, Form("R_{C} = %.0f mm", R_C), "l");
        }
        leg->Draw();
        c->SaveAs(Form("%s/ring_r_stacked.png", output_dir.c_str()));
        delete c;
    }

    // --- r vs nhits 2D ---
    {
        TCanvas *c = new TCanvas("c_r_nhits", "r vs N_hits", 900, 700);
        h2_r_vs_nhits->Draw("COLZ");
        if (h2_r_vs_nhits->GetEntries() > 0) gPad->SetLogz(1);
        if (R_C > 0.f)
        {
            TLine *l1 = new TLine(h2_r_vs_nhits->GetXaxis()->GetXmin(), R_C - ring_dr,
                                  h2_r_vs_nhits->GetXaxis()->GetXmax(), R_C - ring_dr);
            TLine *l2 = new TLine(h2_r_vs_nhits->GetXaxis()->GetXmin(), R_C + ring_dr,
                                  h2_r_vs_nhits->GetXaxis()->GetXmax(), R_C + ring_dr);
            for (TLine *l : {l1, l2}) {
                l->SetLineStyle(2); l->SetLineColor(kRed+1); l->SetLineWidth(2); l->Draw();
            }
        }
        c->SaveAs(Form("%s/ring_r_vs_nhits.png", output_dir.c_str()));
        delete c;
    }

    // --- Q density ratio distribution ---
    {
        TCanvas *c = new TCanvas("c_Q", "Ring density ratio Q", 900, 600);
        h_Q->SetLineColor(kBlue+1); h_Q->SetLineWidth(2);
        h_Q->Draw("HIST");
        TLine *l1 = new TLine(1, 0, 1, h_Q->GetMaximum() * 1.0);
        l1->SetLineStyle(2); l1->SetLineColor(kGray+1); l1->SetLineWidth(2); l1->Draw();
        TLatex lat; lat.SetTextSize(0.03); lat.SetTextColor(kGray+1);
        lat.DrawLatex(1.05, h_Q->GetMaximum() * 0.95, "Q = 1 (isotropic)");
        c->SaveAs(Form("%s/ring_density_ratio.png", output_dir.c_str()));
        delete c;
    }

    // --- Q vs nhits 2D ---
    {
        TCanvas *c = new TCanvas("c_Q_nhits", "Q vs N_hits", 900, 700);
        h2_Q_vs_nhits->Draw("COLZ");
        if (h2_Q_vs_nhits->GetEntries() > 0) gPad->SetLogz(1);
        TLine *l1 = new TLine(h2_Q_vs_nhits->GetXaxis()->GetXmin(), 1,
                              h2_Q_vs_nhits->GetXaxis()->GetXmax(), 1);
        l1->SetLineStyle(2); l1->SetLineColor(kGray+1); l1->SetLineWidth(2); l1->Draw();
        c->SaveAs(Form("%s/ring_density_vs_nhits.png", output_dir.c_str()));
        delete c;
    }

    // --- N_on vs N_off 2D ---
    {
        TCanvas *c = new TCanvas("c_onoff", "N_on vs N_off", 800, 700);
        h2_non_noff->Draw("COLZ");
        if (h2_non_noff->GetEntries() > 0) gPad->SetLogz(1);
        // diagonal N_on = N_off * (A_on/A_off): isotropic background line
        if (A_off > 0.f)
        {
            float slope = A_on / A_off;
            float xmax  = h2_non_noff->GetXaxis()->GetXmax();
            TLine *liso = new TLine(0, 0, xmax, xmax * slope);
            liso->SetLineStyle(2); liso->SetLineColor(kGray+1); liso->SetLineWidth(2); liso->Draw();
        }
        c->SaveAs(Form("%s/ring_nhits_onoff.png", output_dir.c_str()));
        delete c;
    }

    // --- intercept map ---
    {
        TCanvas *c = new TCanvas("c_int", "Track intercept at z_dRICH", 800, 700);
        h2_intercept->Draw("COLZ");
        if (h2_intercept->GetEntries() > 0) gPad->SetLogz(1);
        c->SaveAs(Form("%s/ring_intercept.png", output_dir.c_str()));
        delete c;
    }

    // =========================================================================
    //  Save ROOT file
    // =========================================================================
    std::string output_root = output_dir + "/ring.root";
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");
    for (auto &kv : cfg._data)
        TNamed(kv.first.c_str(), kv.second.c_str()).Write();
    TNamed("run_name", run_name.c_str()).Write();
    h_r_real->Write(); h_r_shuffled->Write();
    h2_r_vs_nhits->Write();
    h_Q->Write(); h2_Q_vs_nhits->Write();
    h2_non_noff->Write();
    h2_intercept->Write();
    fout->Close();

    std::cout << "Output: " << output_root << std::endl;
}
