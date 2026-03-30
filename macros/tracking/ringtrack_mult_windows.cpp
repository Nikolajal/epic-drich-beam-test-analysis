#include "../lib_loader.h"
#include "ringtrack_config.h"
#include <fstream>
#include <sstream>
#include <map>

// ===========================================================================
//  ringtrack_mult_windows
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
    //  Leggi le time windows e label dal conf
    // -------------------------------------------------------------------------
    std::vector<std::pair<float,float>> windows;
    std::vector<std::string> labels;
    for (int i = 0; ; i++)
    {
        std::string val = cfg.get_string(Form("time_window_%d", i), "");
        if (val.empty()) break;
        std::istringstream ss(val);
        float tmin, tmax;
        if (!(ss >> tmin >> tmax)) { std::cerr << "[WARNING] Cannot parse time_window_" << i << std::endl; break; }
        windows.push_back({tmin, tmax});
        labels.push_back(cfg.get_string(Form("time_window_%d_label", i), ""));
        std::cout << "[INFO] Window " << i << ": [" << tmin << ", " << tmax << "] ns  label=" << labels.back() << std::endl;
    }

    if (windows.empty()) { std::cerr << "[ERROR] No time_window_N keys found in conf." << std::endl; return; }

    const int nw = (int)windows.size();

    // -------------------------------------------------------------------------
    //  Open input
    // -------------------------------------------------------------------------
    std::string input_filename = data_repository + "/" + run_name + "/recotrackdata.root";
    TFile *input_file = new TFile(input_filename.c_str());
    if (!input_file || input_file->IsZombie()) { std::cerr << "[ERROR] Could not open " << input_filename << std::endl; return; }
    TTree *tree = (TTree *)input_file->Get("recotrackdata");
    alcor_recotrackdata *recotrackdata = new alcor_recotrackdata();
    recotrackdata->link_to_tree(tree);

    const int all_frames = std::min((int)tree->GetEntries(), cfg.get_int("max_frames", 1000000));

    // -------------------------------------------------------------------------
    //  Istogrammi
    // -------------------------------------------------------------------------
    std::vector<TH1F*> h_all(nw), h_track(nw), h_notrack(nw);
    for (int w = 0; w < nw; w++)
    {
        h_all[w]     = new TH1F(Form("h_all_w%d",     w), "", 100, 0, 100);
        h_track[w]   = new TH1F(Form("h_track_w%d",   w), "", 100, 0, 100);
        h_notrack[w] = new TH1F(Form("h_notrack_w%d", w), "", 100, 0, 100);
    }

    // -------------------------------------------------------------------------
    //  Loop
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
    //  Output
    // -------------------------------------------------------------------------
    gROOT->SetBatch(true);
    gSystem->mkdir(output_dir.c_str(), true);
    std::string output_root = output_dir + "/mult_windows.root";
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");

    // --- helpers ---
    auto norm_clone = [](TH1F *h, const char *name) -> TH1F* {
        TH1F *hc = (TH1F *)h->Clone(name);
        if (hc->Integral() > 0) hc->Scale(1. / hc->Integral());
        hc->SetStats(0);
        return hc;
    };

    auto draw_triple = [&](TH1F *h1, TH1F *h2, TH1F *h3, const char *title, bool normalize)
    {
        TH1F *n1 = normalize ? norm_clone(h1, Form("%s_n", h1->GetName())) : (TH1F*)h1->Clone(Form("%s_c", h1->GetName()));
        TH1F *n2 = normalize ? norm_clone(h2, Form("%s_n", h2->GetName())) : (TH1F*)h2->Clone(Form("%s_c", h2->GetName()));
        TH1F *n3 = normalize ? norm_clone(h3, Form("%s_n", h3->GetName())) : (TH1F*)h3->Clone(Form("%s_c", h3->GetName()));
        n1->SetLineColor(kBlack); n1->SetLineWidth(2); n1->SetStats(0);
        n2->SetLineColor(kRed);   n2->SetLineWidth(2); n2->SetStats(0);
        n3->SetLineColor(kBlue);  n3->SetLineWidth(2); n3->SetStats(0);
        double ymax = std::max({n1->GetMaximum(), n2->GetMaximum(), n3->GetMaximum()}) * 1.15;
        n1->SetMaximum(ymax);
        n1->SetTitle(Form("%s;n hits;%s", title, normalize ? "normalized" : "counts"));
        n1->Draw("HIST"); n2->Draw("HIST SAME"); n3->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.50, 0.75, 0.88, 0.88);
        leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.032);
        leg->AddEntry(n1, "all events", "l");
        leg->AddEntry(n2, "with track", "l");
        leg->AddEntry(n3, "no track",   "l");
        leg->Draw();
    };

    auto draw_pair = [&](TH1F *hs, TH1F *hb, const char *title,
                         const char *lsig, const char *lbkg,
                         int col_sig, int col_bkg, bool normalize)
    {
        TH1F *ns = normalize ? norm_clone(hs, Form("%s_n", hs->GetName())) : (TH1F*)hs->Clone(Form("%s_c", hs->GetName()));
        TH1F *nb = normalize ? norm_clone(hb, Form("%s_n", hb->GetName())) : (TH1F*)hb->Clone(Form("%s_c", hb->GetName()));
        ns->SetLineColor(col_sig); ns->SetLineWidth(2); ns->SetStats(0);
        nb->SetLineColor(col_bkg); nb->SetLineWidth(2); nb->SetLineStyle(2); nb->SetStats(0);
        double ymax = std::max(ns->GetMaximum(), nb->GetMaximum()) * 1.15;
        ns->SetMaximum(ymax);
        ns->SetTitle(Form("%s;n hits;%s", title, normalize ? "normalized" : "counts"));
        ns->Draw("HIST"); nb->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.50, 0.75, 0.88, 0.88);
        leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.032);
        leg->AddEntry(ns, lsig, "l");
        leg->AddEntry(nb, lbkg, "l");
        leg->Draw();
    };

    // ---- cartella con tutti gli istogrammi singoli ----
    {
        TDirectory *hdir = fout->mkdir("histograms");
        hdir->cd();
        for (int w = 0; w < nw; w++)
        {
            std::string lbl = labels[w].empty() ? Form("w%d", w) : labels[w];
            // raw
            TH1F *r1 = (TH1F*)h_all[w]->Clone(Form("h_all_%s",     lbl.c_str())); r1->SetStats(0); r1->Write();
            TH1F *r2 = (TH1F*)h_track[w]->Clone(Form("h_track_%s",   lbl.c_str())); r2->SetStats(0); r2->Write();
            TH1F *r3 = (TH1F*)h_notrack[w]->Clone(Form("h_notrack_%s",lbl.c_str())); r3->SetStats(0); r3->Write();
            // normalized
            TH1F *n1 = norm_clone(h_all[w],     Form("h_all_%s_norm",     lbl.c_str())); n1->Write();
            TH1F *n2 = norm_clone(h_track[w],   Form("h_track_%s_norm",   lbl.c_str())); n2->Write();
            TH1F *n3 = norm_clone(h_notrack[w], Form("h_notrack_%s_norm", lbl.c_str())); n3->Write();
        }
    }

    // ---- canvas per ogni window ----
    for (int w = 0; w < nw; w++)
    {
        float tmin = windows[w].first, tmax = windows[w].second;
        std::string lbl = labels[w];
        TH1F *h1 = h_all[w], *h2 = h_track[w], *h3 = h_notrack[w];

        std::string folder_name = Form("window_%d_%s", w, lbl.empty() ? "unlabeled" : lbl.c_str());
        TDirectory *wdir = fout->mkdir(folder_name.c_str());
        wdir->cd();

        std::string wlbl = lbl.empty() ? "" : Form(" [%s]", lbl.c_str());

        // raw + stats
        TCanvas *c_raw = new TCanvas(Form("c_raw_w%d", w),
            Form("Hit mult [%.1f,%.1f] ns%s", tmin, tmax, wlbl.c_str()), 1000, 600);
        c_raw->Divide(2, 1);
        c_raw->cd(1);
        h1->SetTitle(Form("Hit multiplicity [%.1f,%.1f] ns;n hits;counts", tmin, tmax));
        h1->SetLineColor(kBlack); h1->SetLineWidth(2); h1->SetStats(0);
        h2->SetLineColor(kRed);   h2->SetLineWidth(2); h2->SetStats(0);
        h3->SetLineColor(kBlue);  h3->SetLineWidth(2); h3->SetStats(0);
        h1->Draw("HIST"); h2->Draw("HIST SAME"); h3->Draw("HIST SAME");
        {
            TLegend *leg = new TLegend(0.50, 0.72, 0.88, 0.88);
            leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.035);
            leg->AddEntry(h1, "all events", "l");
            leg->AddEntry(h2, "with track", "l");
            leg->AddEntry(h3, "no track",   "l");
            leg->Draw();
        }
        c_raw->cd(2);
        {
            TPaveText *pt = new TPaveText(0.03, 0.03, 0.97, 0.97, "NDC");
            pt->SetTextAlign(12); pt->SetTextFont(42); pt->SetTextSize(0.042);
            pt->SetFillColor(0); pt->SetBorderSize(1);
            pt->AddText(Form("Window: [%.1f, %.1f] ns%s", tmin, tmax, wlbl.c_str()));
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
        c_raw->Write();
        c_raw->SaveAs(Form("%s/mult_w%d_raw.png", output_dir.c_str(), w));

        // normalizzato
        TCanvas *c_norm = new TCanvas(Form("c_norm_w%d", w),
            Form("Hit mult norm [%.1f,%.1f] ns%s", tmin, tmax, wlbl.c_str()), 800, 600);
        draw_triple(h1, h2, h3,
            Form("Normalized [%.1f,%.1f] ns%s", tmin, tmax, wlbl.c_str()), true);
        c_norm->Write();
        c_norm->SaveAs(Form("%s/mult_w%d_norm.png", output_dir.c_str(), w));
    }

    // ---- canvas comparativi sig vs bkg per gruppo ----
    // raggruppa per suffisso (es. sig_12ns -> gruppo "12ns")
    std::map<std::string, int> sig_by_group, bkg_by_group;
    for (int w = 0; w < nw; w++)
    {
        std::string lbl = labels[w];
        if (lbl.substr(0, 4) == "sig_")
            sig_by_group[lbl.substr(4)] = w;
        else if (lbl.substr(0, 4) == "bkg_")
            bkg_by_group[lbl.substr(4)] = w;
    }

    bool any_pair = false;
    for (auto &kv : sig_by_group)
    {
        std::string group = kv.first;
        int si = kv.second;
        if (bkg_by_group.find(group) == bkg_by_group.end()) continue;
        int bi = bkg_by_group[group];
        any_pair = true;

        float ts0 = windows[si].first, ts1 = windows[si].second;
        float tb0 = windows[bi].first, tb1 = windows[bi].second;
        std::string sl = Form("[%.1f,%.1f]", ts0, ts1);
        std::string bl = Form("[%.1f,%.1f]", tb0, tb1);

        TDirectory *cdir = fout->mkdir(Form("comparison_%s", group.c_str()));
        cdir->cd();

        struct Cat { TH1F *hs; TH1F *hb; const char *name; int cs; int cb; };
        std::vector<Cat> cats = {
            {h_all[si],     h_all[bi],     "all",     kBlack, kGray+2},
            {h_track[si],   h_track[bi],   "track",   kRed,   kOrange+1},
            {h_notrack[si], h_notrack[bi], "notrack", kBlue,  kCyan+1}
        };

        for (auto &cat : cats)
        {
            std::string lsig = Form("sig %s", sl.c_str());
            std::string lbkg = Form("bkg %s", bl.c_str());

            TCanvas *cr = new TCanvas(
                Form("c_raw_%s_%s", cat.name, group.c_str()),
                Form("%s raw [%s]: sig%s vs bkg%s", cat.name, group.c_str(), sl.c_str(), bl.c_str()), 800, 600);
            draw_pair(cat.hs, cat.hb,
                Form("%s [%s]: sig%s vs bkg%s", cat.name, group.c_str(), sl.c_str(), bl.c_str()),
                lsig.c_str(), lbkg.c_str(), cat.cs, cat.cb, false);
            cr->Write();
            cr->SaveAs(Form("%s/mult_%s_%s_raw.png", output_dir.c_str(), group.c_str(), cat.name));

            TCanvas *cn = new TCanvas(
                Form("c_norm_%s_%s", cat.name, group.c_str()),
                Form("%s norm [%s]: sig%s vs bkg%s", cat.name, group.c_str(), sl.c_str(), bl.c_str()), 800, 600);
            draw_pair(cat.hs, cat.hb,
                Form("%s norm [%s]: sig%s vs bkg%s", cat.name, group.c_str(), sl.c_str(), bl.c_str()),
                lsig.c_str(), lbkg.c_str(), cat.cs, cat.cb, true);
            cn->Write();
            cn->SaveAs(Form("%s/mult_%s_%s_norm.png", output_dir.c_str(), group.c_str(), cat.name));
        }
    }

    if (!any_pair)
        std::cout << "[INFO] No matching sig_*/bkg_* pairs found — skipping comparison canvases." << std::endl;

    fout->Close();
    std::cout << "Output: " << output_root << std::endl;
}