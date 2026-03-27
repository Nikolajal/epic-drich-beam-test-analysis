#include "../lib_loader.h"
#include "ringtrack_config.h"
#include <fstream>
#include <sstream>

// ===========================================================================
//  ringtrack_mult_windows
//  Per ogni time window definita nel conf, produce un canvas con il plot
//  triplo della hit multiplicity:
//    - tutti gli eventi (nero)
//    - eventi con traccia (rosso)
//    - eventi senza traccia (blu)
//  Tutte le windows vengono salvate in un unico file ROOT.
//
//  Conf keys:
//    time_window_0 = -7.44  3.66
//    time_window_1 = -65.0 -25.0
//    ...  (finché non se ne trovano altre)
//    apply_afterpulse_cut = true
// ===========================================================================

void ringtrack_mult_windows(std::string data_repository, std::string run_name,
                             std::string conf_path = "ringtrack.conf",
                             std::string output_dir = "")
{
    RingtrackConfig cfg;
    try {
        cfg.load(conf_path);
    } catch (const std::exception &e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return;
    }

    if (output_dir.empty())
        output_dir = data_repository + "/" + run_name + "/plots";

    const bool apply_afterpulse = cfg.get_bool("apply_afterpulse_cut", true);

    // -------------------------------------------------------------------------
    //  Leggi le time windows dal conf
    // -------------------------------------------------------------------------
    std::vector<std::pair<float,float>> windows;
    for (int i = 0; ; i++)
    {
        std::string key = Form("time_window_%d", i);
        std::string val = cfg.get_string(key, "");
        if (val.empty()) break;
        std::istringstream ss(val);
        float tmin, tmax;
        if (!(ss >> tmin >> tmax))
        {
            std::cerr << "[WARNING] Cannot parse " << key << " = " << val << std::endl;
            break;
        }
        windows.push_back({tmin, tmax});
        std::cout << "[INFO] Window " << i << ": [" << tmin << ", " << tmax << "] ns" << std::endl;
    }

    if (windows.empty())
    {
        std::cerr << "[ERROR] No time_window_N keys found in conf." << std::endl;
        return;
    }

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

    const int n_frames = (int)tree->GetEntries();
    const int max_frames = cfg.get_int("max_frames", 1000000);
    const int all_frames = std::min(n_frames, max_frames);

    // -------------------------------------------------------------------------
    //  Crea istogrammi per ogni window (3 per window: all, track, notrack)
    // -------------------------------------------------------------------------
    int nw = (int)windows.size();
    std::vector<TH1F*> h_all(nw), h_track(nw), h_notrack(nw);
    for (int w = 0; w < nw; w++)
    {
        h_all[w]    = new TH1F(Form("h_all_w%d",    w), "", 100, 0, 100);
        h_track[w]  = new TH1F(Form("h_track_w%d",  w), "", 100, 0, 100);
        h_notrack[w]= new TH1F(Form("h_notrack_w%d",w), "", 100, 0, 100);
    }

    // -------------------------------------------------------------------------
    //  Loop unico — riempi tutte le windows in parallelo
    // -------------------------------------------------------------------------
    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 10000 == 0) bar.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;

        auto trigger = recotrackdata->get_trigger_by_index(0);
        if (!trigger) continue;

        const int n_tracks = recotrackdata->n_recotrackdata();

        // conta hit per ogni window
        std::vector<int> n_hits(nw, 0);
        for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); ++i_hit)
        {
            if (apply_afterpulse && recotrackdata->is_afterpulse(i_hit)) continue;
            float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
            for (int w = 0; w < nw; w++)
                if (dt >= windows[w].first && dt <= windows[w].second)
                    ++n_hits[w];
        }

        for (int w = 0; w < nw; w++)
        {
            h_all[w]->Fill(n_hits[w]);
            if (n_tracks == 0) h_notrack[w]->Fill(n_hits[w]);
            else               h_track[w]->Fill(n_hits[w]);
        }
    }
    bar.update(all_frames, all_frames);
    bar.finish();

    // -------------------------------------------------------------------------
    //  Leggi i label per ogni window (sig / bkg / "")
    // -------------------------------------------------------------------------
    std::vector<std::string> labels(nw, "");
    for (int i = 0; i < nw; i++)
        labels[i] = cfg.get_string(Form("time_window_%d_label", i), "");

    // -------------------------------------------------------------------------
    //  Canvas — uno per window
    // -------------------------------------------------------------------------
    gROOT->SetBatch(true);
    std::string output_root = output_dir + "/mult_windows.root";
    gSystem->mkdir(output_dir.c_str(), true);
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");

    // helper: normalizza un clone
    auto norm_clone = [](TH1F *h, const char *name) -> TH1F* {
        TH1F *hc = (TH1F *)h->Clone(name);
        if (hc->Integral() > 0) hc->Scale(1. / hc->Integral());
        hc->SetStats(0);
        return hc;
    };

    // helper: disegna triplo normalizzato
    auto draw_triple_norm = [&](TCanvas *c, int ipad,
                                TH1F *h1, TH1F *h2, TH1F *h3,
                                const char *title,
                                const char *l1, const char *l2, const char *l3)
    {
        c->cd(ipad);
        TH1F *n1 = norm_clone(h1, Form("%s_n1", h1->GetName()));
        TH1F *n2 = norm_clone(h2, Form("%s_n2", h2->GetName()));
        TH1F *n3 = norm_clone(h3, Form("%s_n3", h3->GetName()));
        n1->SetLineColor(kBlack); n1->SetLineWidth(2);
        n2->SetLineColor(kRed);   n2->SetLineWidth(2);
        n3->SetLineColor(kBlue);  n3->SetLineWidth(2);
        double ymax = std::max({n1->GetMaximum(), n2->GetMaximum(), n3->GetMaximum()}) * 1.15;
        n1->SetMaximum(ymax);
        n1->SetTitle(Form("%s;n hits;normalized", title));
        n1->Draw("HIST"); n2->Draw("HIST SAME"); n3->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.50, 0.75, 0.88, 0.88);
        leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.032);
        leg->AddEntry(n1, l1, "l");
        leg->AddEntry(n2, l2, "l");
        leg->AddEntry(n3, l3, "l");
        leg->Draw();
    };

    // helper: disegna coppia sig vs bkg normalizzata
    auto draw_pair_norm = [&](TCanvas *c, int ipad,
                              TH1F *hsig, TH1F *hbkg,
                              const char *title,
                              const char *lsig, const char *lbkg,
                              int col_sig, int col_bkg)
    {
        c->cd(ipad);
        TH1F *ns = norm_clone(hsig, Form("%s_ns", hsig->GetName()));
        TH1F *nb = norm_clone(hbkg, Form("%s_nb", hbkg->GetName()));
        ns->SetLineColor(col_sig); ns->SetLineWidth(2);
        nb->SetLineColor(col_bkg); nb->SetLineWidth(2); nb->SetLineStyle(2);
        double ymax = std::max(ns->GetMaximum(), nb->GetMaximum()) * 1.15;
        ns->SetMaximum(ymax);
        ns->SetTitle(Form("%s;n hits;normalized", title));
        ns->Draw("HIST"); nb->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.50, 0.75, 0.88, 0.88);
        leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.032);
        leg->AddEntry(ns, lsig, "l");
        leg->AddEntry(nb, lbkg, "l");
        leg->Draw();
    };

    for (int w = 0; w < nw; w++)
    {
        float tmin = windows[w].first, tmax = windows[w].second;
        std::string lbl = labels[w];

        TH1F *h1 = h_all[w];
        TH1F *h2 = h_track[w];
        TH1F *h3 = h_notrack[w];

        h1->SetLineColor(kBlack); h1->SetLineWidth(2);
        h2->SetLineColor(kRed);   h2->SetLineWidth(2); h2->SetStats(0);
        h3->SetLineColor(kBlue);  h3->SetLineWidth(2); h3->SetStats(0);

        h1->SetTitle(Form("Hit multiplicity [%.1f, %.1f] ns;n hits;counts", tmin, tmax));

        // --- canvas principale: raw + stats ---
        TCanvas *c = new TCanvas(Form("c_mult_w%d", w),
            Form("Hit mult [%.1f, %.1f] ns", tmin, tmax), 1000, 600);
        c->Divide(2, 1);
        c->cd(1);
        h1->Draw("HIST");
        h2->Draw("HIST SAME");
        h3->Draw("HIST SAME");
        {
            TLegend *leg = new TLegend(0.50, 0.72, 0.88, 0.88);
            leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.035);
            leg->AddEntry(h1, "all events", "l");
            leg->AddEntry(h2, "with track", "l");
            leg->AddEntry(h3, "no track",   "l");
            leg->Draw();
        }
        c->cd(2);
        {
            TPaveText *pt = new TPaveText(0.03, 0.03, 0.97, 0.97, "NDC");
            pt->SetTextAlign(12); pt->SetTextFont(42); pt->SetTextSize(0.042);
            pt->SetFillColor(0); pt->SetBorderSize(1);
            pt->AddText(Form("Window: [%.1f, %.1f] ns%s", tmin, tmax,
                lbl.empty() ? "" : Form("  [%s]", lbl.c_str())));
            pt->AddText("---------------------------------");
            auto add_stats = [&](TH1F *h, const char *label, int color) {
                pt->AddText(Form("%s", label))->SetTextColor(color);
                pt->AddText(Form("  Entries  %7.0f", h->GetEntries()))->SetTextColor(color);
                pt->AddText(Form("  Mean     %7.2f", h->GetMean()))->SetTextColor(color);
                pt->AddText(Form("  Std Dev  %7.2f", h->GetStdDev()))->SetTextColor(color);
                pt->AddText(" ");
            };
            add_stats(h1, "All events", kBlack);
            add_stats(h2, "With track", kRed);
            add_stats(h3, "No track",   kBlue);
            pt->Draw();
        }
        c->Write();
        c->SaveAs(Form("%s/mult_window_%d.png", output_dir.c_str(), w));

        // --- canvas normalizzato per questa window ---
        if (!lbl.empty())
        {
            TCanvas *cn = new TCanvas(Form("c_mult_norm_w%d", w),
                Form("Hit mult norm [%.1f, %.1f] ns [%s]", tmin, tmax, lbl.c_str()), 800, 600);
            draw_triple_norm(cn, 0, h1, h2, h3,
                Form("Normalized [%.1f, %.1f] ns [%s]", tmin, tmax, lbl.c_str()),
                "all events", "with track", "no track");
            cn->Write();
            cn->SaveAs(Form("%s/mult_norm_%s_w%d.png", output_dir.c_str(), lbl.c_str(), w));
        }
    }

    // -------------------------------------------------------------------------
    //  Canvas comparativi sig vs bkg (per ogni coppia sig/bkg trovata)
    // -------------------------------------------------------------------------
    // raccogli indici sig e bkg
    std::vector<int> sig_idx, bkg_idx;
    for (int w = 0; w < nw; w++)
    {
        if (labels[w] == "sig") sig_idx.push_back(w);
        if (labels[w] == "bkg") bkg_idx.push_back(w);
    }

    for (int si : sig_idx)
    {
        for (int bi : bkg_idx)
        {
            float ts_min = windows[si].first, ts_max = windows[si].second;
            float tb_min = windows[bi].first, tb_max = windows[bi].second;

            std::string sig_label = Form("[%.1f,%.1f]", ts_min, ts_max);
            std::string bkg_label = Form("[%.1f,%.1f]", tb_min, tb_max);

            // 3 canvas: all, track, notrack — sig vs bkg
            auto make_pair_canvas = [&](TH1F *hs, TH1F *hb,
                                        const char *cat, int col_s, int col_b)
            {
                TCanvas *cp = new TCanvas(
                    Form("c_sigbkg_%s_s%d_b%d", cat, si, bi),
                    Form("%s: sig%s vs bkg%s", cat, sig_label.c_str(), bkg_label.c_str()),
                    800, 600);
                draw_pair_norm(cp, 0, hs, hb,
                    Form("%s: sig%s vs bkg%s", cat, sig_label.c_str(), bkg_label.c_str()),
                    Form("sig %s", sig_label.c_str()),
                    Form("bkg %s", bkg_label.c_str()),
                    col_s, col_b);
                cp->Write();
                cp->SaveAs(Form("%s/mult_sigbkg_%s_s%d_b%d.png",
                    output_dir.c_str(), cat, si, bi));
            };

            make_pair_canvas(h_all[si],    h_all[bi],    "all",     kBlack,    kGray+2);
            make_pair_canvas(h_track[si],  h_track[bi],  "track",   kRed,      kOrange+1);
            make_pair_canvas(h_notrack[si],h_notrack[bi],"notrack", kBlue,     kCyan+1);
        }
    }

    fout->Close();
    std::cout << "Output: " << output_root << std::endl;
}