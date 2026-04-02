#include "../lib_loader.h"
#include "ringtrack_config.h"
#include <fstream>

// =============================================================================
//  ringtrack_notrack  —  study of no-track events and trigger count check
//
//  Two goals:
//
//  1. No-track event characterisation
//     Loop over ALL triggered events (no track selection).
//     Categories: no-track (0), single-track (1), multi-track (>=2).
//     For each: n_hits in time window, timing, hit map.
//     Doubly-empty: events with 0 tracks AND 0 dRICH hits.
//
//  2. Trigger count / alignment check
//     Plot i_frame (TTree entry index) vs beam-trigger coarse timestamp.
//     If every hardware trigger appears exactly once in the TTree, this plot
//     is monotonically increasing (mod 65536).  Any plateau or jump reveals
//     missing or duplicated triggers — i.e. desynchronisation between the
//     two readout systems (tracker + dRICH).
//     Also plots consecutive-frame Δcoarse to quantify gaps.
//
//  Outputs:
//    notrack.root                    — all histograms
//    notrack_stats.txt               — summary statistics
//    notrack_hitmap.png              — hit maps by category (3-panel)
//    notrack_nhits.png               — n_hits distributions overlaid
//    notrack_timing.png              — timing distributions overlaid
//    notrack_iframe_coarse.png       — i_frame vs coarse (trigger count check)
//    notrack_coarse_diff.png         — Δcoarse between consecutive frames
//    notrack_doubly_empty_frames.txt — i_frame list of doubly-empty events
// =============================================================================
void ringtrack_notrack(std::string data_repository, std::string run_name,
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

    const bool apply_afterpulse_cut = cfg.get_bool ("apply_afterpulse_cut", true);
    const std::array<float,2> time_cut = {
        cfg.get_float("time_cut_min", -50.f),
        cfg.get_float("time_cut_max",  20.f)
    };
    const int first_event = cfg.get_int("first_event", 0);
    const int max_frames_ = cfg.get_int("max_frames",  1000000);

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

    std::cout << "[notrack] TTree entries: " << n_frames
              << "  processing: [" << first_event << ", " << all_frames << ")" << std::endl;

    // =========================================================================
    //  Histograms — category study
    // =========================================================================

    TH1F *h_multiplicity = new TH1F("h_multiplicity",
        "Track multiplicity (all triggered events);n tracks;counts", 10, -0.5, 9.5);

    // n_hits in time window by category
    TH1F *h_nhits_notrack = new TH1F("h_nhits_notrack",
        "N hits in time window — no-track;n hits;counts", 80, -0.5, 79.5);
    TH1F *h_nhits_1track  = new TH1F("h_nhits_1track",
        "N hits in time window — 1 track;n hits;counts",  80, -0.5, 79.5);
    TH1F *h_nhits_ntrack  = new TH1F("h_nhits_ntrack",
        "N hits in time window — multi-track;n hits;counts", 80, -0.5, 79.5);

    // timing distributions
    TH1F *h_t_notrack = new TH1F("h_t_notrack",
        "Hit time — no-track;t_{hit} - t_{beam} (ns);counts", 200, -312.5, 312.5);
    TH1F *h_t_1track  = new TH1F("h_t_1track",
        "Hit time — 1 track;t_{hit} - t_{beam} (ns);counts",  200, -312.5, 312.5);
    TH1F *h_t_ntrack  = new TH1F("h_t_ntrack",
        "Hit time — multi-track;t_{hit} - t_{beam} (ns);counts", 200, -312.5, 312.5);

    // hit maps by category
    TH2F *h_hitmap_notrack = new TH2F("h_hitmap_notrack",
        "Hit map (time window) — no-track;x (mm);y (mm)", 198, -99, 99, 198, -99, 99);
    TH2F *h_hitmap_1track  = new TH2F("h_hitmap_1track",
        "Hit map (time window) — 1 track;x (mm);y (mm)",  198, -99, 99, 198, -99, 99);
    TH2F *h_hitmap_ntrack  = new TH2F("h_hitmap_ntrack",
        "Hit map (time window) — multi-track;x (mm);y (mm)", 198, -99, 99, 198, -99, 99);

    // =========================================================================
    //  Histograms — empty trigger characterisation
    // =========================================================================

    // Raw hit count (absolutely no cuts) per category.
    // Distinguishes "truly empty trigger" (raw_hits==0) from
    // "hits exist but outside the time window".
    TH1F *h_raw_nhits_notrack = new TH1F("h_raw_nhits_notrack",
        "Raw hit count (no cuts) — no-track;n raw hits;counts", 80, -0.5, 79.5);
    TH1F *h_raw_nhits_1track  = new TH1F("h_raw_nhits_1track",
        "Raw hit count (no cuts) — 1 track;n raw hits;counts",  80, -0.5, 79.5);

    // Full timing (no time cut, no afterpulse cut) for no-track events only.
    // Shows where the hits are if the time window misses them.
    TH1F *h_t_notrack_full = new TH1F("h_t_notrack_full",
        "Full hit timing — no-track (no time cut, no AP cut);"
        "t_{hit} - t_{beam} (ns);counts", 200, -312.5, 312.5);

    // Same for 1-track events as reference.
    TH1F *h_t_1track_full  = new TH1F("h_t_1track_full",
        "Full hit timing — 1 track (no time cut, no AP cut);"
        "t_{hit} - t_{beam} (ns);counts", 200, -312.5, 312.5);

    // =========================================================================
    //  Histograms — trigger count / alignment check
    // =========================================================================

    // i_frame vs beam-trigger coarse: a straight diagonal line (mod 65536)
    // means every hardware trigger has exactly one TTree entry.
    // Flat segments = missing triggers; vertical jumps = duplicated entries.
    TH2F *h2_iframe_coarse = new TH2F("h2_iframe_coarse",
        "i_frame vs beam-trigger coarse (trigger count check);"
        "i_frame (TTree entry);coarse (ticks)", 500, 0, all_frames, 500, 0, 65535);

    // Δcoarse between consecutive triggered frames: monitors trigger rate and gaps
    TH1F *h_coarse_diff = new TH1F("h_coarse_diff",
        "Coarse step between consecutive triggered frames;"
        "#Deltacoarse (ticks);counts", 500, 0, 5000);

    // number of triggers stored per TTree entry (should always be 1 for beam trigger)
    TH1F *h_n_triggers = new TH1F("h_n_triggers",
        "Number of trigger entries per TTree entry;n triggers;counts", 10, -0.5, 9.5);

    // =========================================================================
    //  Event loop
    // =========================================================================
    int n_no_trigger  = 0;
    int n_spill_start = 0;
    int n_total       = 0;
    int n_notrack     = 0;
    int n_1track      = 0;
    int n_ntrack      = 0;
    int n_zerohits     = 0;  // 0 dRICH hits in time window (any multiplicity)
    int n_doubly_empty = 0;  // 0 tracks AND 0 dRICH hits in time window
    int n_truly_empty  = 0;  // 0 tracks AND 0 raw hits (nothing at all)

    int prev_coarse = -1;

    std::ofstream f_doubly(output_dir + "/notrack_doubly_empty_frames.txt");
    f_doubly << "i_frame\tbeam_coarse\tn_triggers_in_entry\n";

    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 1000 == 0) bar.update(i_frame, all_frames);

        if (recotrackdata->is_start_of_spill()) { ++n_spill_start; continue; }

        // beam trigger (config index 0)
        auto beam_trig = recotrackdata->get_trigger_by_index(0);
        if (!beam_trig) { ++n_no_trigger; continue; }

        ++n_total;

        // number of trigger entries in this TTree event
        const int n_trigs = (int)recotrackdata->get_triggers().size();
        h_n_triggers->Fill(n_trigs);

        // coarse time series
        const int coarse = (int)beam_trig->coarse;
        h2_iframe_coarse->Fill((float)i_frame, (float)coarse);
        if (prev_coarse >= 0)
        {
            int diff = coarse - prev_coarse;
            if (diff < 0) diff += 65536; // uint16 wrap
            h_coarse_diff->Fill((float)diff);
        }
        prev_coarse = coarse;

        // track multiplicity
        const int n_trk = recotrackdata->n_recotrackdata();
        h_multiplicity->Fill(n_trk);

        // count dRICH hits in time window (afterpulse filtered)
        const auto &hits = recotrackdata->get_recodata();
        int n_hits = 0;
        for (int i_hit = 0; i_hit < (int)hits.size(); i_hit++)
        {
            if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
            float dt = recotrackdata->get_hit_t(i_hit) - beam_trig->fine_time;
            if (dt >= time_cut[0] && dt <= time_cut[1]) ++n_hits;
        }
        if (n_hits == 0) ++n_zerohits;

        // raw hit count: no afterpulse cut, no time cut
        const int n_raw = (int)hits.size();

        // doubly-empty (windowed)
        if (n_trk == 0 && n_hits == 0)
        {
            ++n_doubly_empty;
            f_doubly << i_frame << "\t" << coarse << "\t" << n_trigs << "\n";
        }
        // truly empty: 0 tracks AND 0 raw hits
        if (n_trk == 0 && n_raw == 0)
            ++n_truly_empty;

        // fill per-category histograms
        TH1F *h_nhits    = nullptr;
        TH1F *h_t        = nullptr;
        TH2F *h_hitmap   = nullptr;
        TH1F *h_raw_nhits = nullptr;
        TH1F *h_t_full   = nullptr;

        if (n_trk == 0)
        {
            ++n_notrack;
            h_nhits     = h_nhits_notrack;
            h_t         = h_t_notrack;
            h_hitmap    = h_hitmap_notrack;
            h_raw_nhits = h_raw_nhits_notrack;
            h_t_full    = h_t_notrack_full;
        }
        else if (n_trk == 1)
        {
            ++n_1track;
            h_nhits     = h_nhits_1track;
            h_t         = h_t_1track;
            h_hitmap    = h_hitmap_1track;
            h_raw_nhits = h_raw_nhits_1track;
            h_t_full    = h_t_1track_full;
        }
        else
        {
            ++n_ntrack;
            h_nhits  = h_nhits_ntrack;
            h_t      = h_t_ntrack;
            h_hitmap = h_hitmap_ntrack;
        }

        h_nhits->Fill(n_hits);
        if (h_raw_nhits) h_raw_nhits->Fill(n_raw);

        for (int i_hit = 0; i_hit < n_raw; i_hit++)
        {
            float dt_full = recotrackdata->get_hit_t(i_hit) - beam_trig->fine_time;
            if (h_t_full) h_t_full->Fill(dt_full);

            if (apply_afterpulse_cut && recotrackdata->is_afterpulse(i_hit)) continue;
            float dt = dt_full;
            h_t->Fill(dt);
            if (dt < time_cut[0] || dt > time_cut[1]) continue;
            h_hitmap->Fill(recotrackdata->get_hit_x_rnd(i_hit),
                           recotrackdata->get_hit_y_rnd(i_hit));
        }
    }
    bar.update(all_frames, all_frames);
    bar.finish();
    f_doubly.close();

    // =========================================================================
    //  Statistics
    // =========================================================================
    auto pct = [](int num, int den) -> float {
        return den > 0 ? 100.f * (float)num / (float)den : 0.f;
    };

    std::cout << "========================================" << std::endl;
    std::cout << "No-track study" << std::endl;
    std::cout << "  TTree entries:       " << n_frames      << std::endl;
    std::cout << "  Spill-start:         " << n_spill_start << std::endl;
    std::cout << "  No beam-trigger:     " << n_no_trigger  << std::endl;
    std::cout << "  Triggered events:    " << n_total       << std::endl;
    std::cout << std::endl;
    std::cout << "  No track:    " << n_notrack
              << Form("  (%.1f%%)", pct(n_notrack, n_total)) << std::endl;
    std::cout << "  1 track:     " << n_1track
              << Form("  (%.1f%%)", pct(n_1track,  n_total)) << std::endl;
    std::cout << "  Multi-track: " << n_ntrack
              << Form("  (%.1f%%)", pct(n_ntrack,  n_total)) << std::endl;
    std::cout << std::endl;
    std::cout << "  0 dRICH hits (time window): " << n_zerohits
              << Form("  (%.1f%% of triggered)", pct(n_zerohits, n_total)) << std::endl;
    std::cout << "  Doubly-empty (0 trk + 0 hits in window): " << n_doubly_empty
              << Form("  (%.1f%% of triggered)", pct(n_doubly_empty, n_total))
              << Form("  (%.1f%% of no-track)", pct(n_doubly_empty, n_notrack))
              << Form("  (%.1f%% of 0-hit)",    pct(n_doubly_empty, n_zerohits)) << std::endl;
    std::cout << "  Truly empty (0 trk + 0 raw hits): " << n_truly_empty
              << Form("  (%.1f%% of no-track)", pct(n_truly_empty, n_notrack))
              << Form("  (%.1f%% of doubly-empty)", pct(n_truly_empty, n_doubly_empty)) << std::endl;
    std::cout << "========================================" << std::endl;

    std::ofstream stats(output_dir + "/notrack_stats.txt");
    stats << "n_frames_total\t"         << n_frames       << "\n";
    stats << "n_spill_start\t"          << n_spill_start  << "\n";
    stats << "n_no_trigger\t"           << n_no_trigger   << "\n";
    stats << "n_triggered\t"            << n_total        << "\n";
    stats << "n_notrack\t"              << n_notrack      << "\n";
    stats << "n_1track\t"               << n_1track       << "\n";
    stats << "n_ntrack\t"               << n_ntrack       << "\n";
    stats << "pct_notrack\t"            << pct(n_notrack, n_total)           << "\n";
    stats << "pct_1track\t"             << pct(n_1track,  n_total)           << "\n";
    stats << "pct_ntrack\t"             << pct(n_ntrack,  n_total)           << "\n";
    stats << "n_zerohits\t"             << n_zerohits     << "\n";
    stats << "n_doubly_empty\t"          << n_doubly_empty << "\n";
    stats << "pct_doubly_empty\t"        << pct(n_doubly_empty, n_total)      << "\n";
    stats << "pct_doubly_of_notrack\t"   << pct(n_doubly_empty, n_notrack)    << "\n";
    stats << "pct_doubly_of_zerohits\t"  << pct(n_doubly_empty, n_zerohits)   << "\n";
    stats << "n_truly_empty\t"           << n_truly_empty  << "\n";
    stats << "pct_truly_empty_of_notrack\t" << pct(n_truly_empty, n_notrack)  << "\n";
    stats.close();

    // =========================================================================
    //  Plots
    // =========================================================================
    gROOT->SetBatch(true);

    // hit maps 3-panel
    {
        TCanvas *c = new TCanvas("c_hitmaps", "Hit maps by track multiplicity", 2100, 700);
        c->Divide(3, 1);
        c->cd(1); h_hitmap_notrack->Draw("COLZ"); if (h_hitmap_notrack->GetEntries() > 0) gPad->SetLogz(1);
        c->cd(2); h_hitmap_1track ->Draw("COLZ"); if (h_hitmap_1track ->GetEntries() > 0) gPad->SetLogz(1);
        c->cd(3); h_hitmap_ntrack ->Draw("COLZ"); if (h_hitmap_ntrack ->GetEntries() > 0) gPad->SetLogz(1);
        c->SaveAs(Form("%s/notrack_hitmap.png", output_dir.c_str()));
        delete c;
    }

    // n_hits overlaid
    {
        TCanvas *c = new TCanvas("c_nhits", "N hits in time window by category", 900, 600);
        h_nhits_notrack->SetLineColor(kRed+1);   h_nhits_notrack->SetLineWidth(2);
        h_nhits_1track ->SetLineColor(kBlue+1);  h_nhits_1track ->SetLineWidth(2);
        h_nhits_ntrack ->SetLineColor(kGreen+2); h_nhits_ntrack ->SetLineWidth(2);
        double ymax = std::max({h_nhits_notrack->GetMaximum(),
                                h_nhits_1track ->GetMaximum(),
                                h_nhits_ntrack ->GetMaximum()});
        h_nhits_notrack->SetMaximum(ymax * 1.25);
        h_nhits_notrack->Draw("HIST");
        h_nhits_1track ->Draw("HIST SAME");
        h_nhits_ntrack ->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.55, 0.65, 0.88, 0.88);
        leg->AddEntry(h_nhits_notrack, Form("no-track (%d)",    n_notrack), "l");
        leg->AddEntry(h_nhits_1track,  Form("1 track (%d)",     n_1track),  "l");
        leg->AddEntry(h_nhits_ntrack,  Form("multi-track (%d)", n_ntrack),  "l");
        leg->Draw();
        c->SaveAs(Form("%s/notrack_nhits.png", output_dir.c_str()));
        delete c;
    }

    // timing overlaid
    {
        TCanvas *c = new TCanvas("c_timing", "Hit timing by category", 900, 600);
        h_t_notrack->SetLineColor(kRed+1);   h_t_notrack->SetLineWidth(2);
        h_t_1track ->SetLineColor(kBlue+1);  h_t_1track ->SetLineWidth(2);
        h_t_ntrack ->SetLineColor(kGreen+2); h_t_ntrack ->SetLineWidth(2);
        double ymax = std::max({h_t_notrack->GetMaximum(),
                                h_t_1track ->GetMaximum(),
                                h_t_ntrack ->GetMaximum()});
        h_t_notrack->SetMaximum(ymax * 1.25);
        h_t_notrack->Draw("HIST");
        h_t_1track ->Draw("HIST SAME");
        h_t_ntrack ->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.55, 0.65, 0.88, 0.88);
        leg->AddEntry(h_t_notrack, "no-track",    "l");
        leg->AddEntry(h_t_1track,  "1 track",     "l");
        leg->AddEntry(h_t_ntrack,  "multi-track", "l");
        leg->Draw();
        TLine *l1 = new TLine(time_cut[0], 0, time_cut[0], ymax * 1.1);
        TLine *l2 = new TLine(time_cut[1], 0, time_cut[1], ymax * 1.1);
        l1->SetLineStyle(2); l1->SetLineColor(kGray+1); l1->Draw();
        l2->SetLineStyle(2); l2->SetLineColor(kGray+1); l2->Draw();
        c->SaveAs(Form("%s/notrack_timing.png", output_dir.c_str()));
        delete c;
    }

    // raw hit count: no-track vs 1-track
    {
        TCanvas *c = new TCanvas("c_raw_nhits", "Raw hit count (no cuts): no-track vs 1-track", 900, 600);
        h_raw_nhits_notrack->SetLineColor(kRed+1);  h_raw_nhits_notrack->SetLineWidth(2);
        h_raw_nhits_1track ->SetLineColor(kBlue+1); h_raw_nhits_1track ->SetLineWidth(2);
        double ymax = std::max(h_raw_nhits_notrack->GetMaximum(), h_raw_nhits_1track->GetMaximum());
        h_raw_nhits_notrack->SetMaximum(ymax * 1.25);
        h_raw_nhits_notrack->Draw("HIST");
        h_raw_nhits_1track ->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.55, 0.72, 0.88, 0.88);
        leg->AddEntry(h_raw_nhits_notrack, Form("no-track (%d)",  n_notrack), "l");
        leg->AddEntry(h_raw_nhits_1track,  Form("1 track (%d)",   n_1track),  "l");
        leg->Draw();
        // mark bin 0 (truly empty triggers)
        TLine *lz = new TLine(0, 0, 0, ymax * 1.1);
        lz->SetLineStyle(2); lz->SetLineColor(kRed+2); lz->Draw();
        c->SaveAs(Form("%s/notrack_raw_nhits.png", output_dir.c_str()));
        delete c;
    }

    // full timing (no cuts): no-track vs 1-track
    {
        TCanvas *c = new TCanvas("c_t_full", "Full timing (no cuts): no-track vs 1-track", 900, 600);
        h_t_notrack_full->SetLineColor(kRed+1);  h_t_notrack_full->SetLineWidth(2);
        h_t_1track_full ->SetLineColor(kBlue+1); h_t_1track_full ->SetLineWidth(2);
        double ymax = std::max(h_t_notrack_full->GetMaximum(), h_t_1track_full->GetMaximum());
        h_t_notrack_full->SetMaximum(ymax * 1.25);
        h_t_notrack_full->Draw("HIST");
        h_t_1track_full ->Draw("HIST SAME");
        TLegend *leg = new TLegend(0.55, 0.72, 0.88, 0.88);
        leg->AddEntry(h_t_notrack_full, "no-track", "l");
        leg->AddEntry(h_t_1track_full,  "1 track",  "l");
        leg->Draw();
        TLine *l1 = new TLine(time_cut[0], 0, time_cut[0], ymax * 1.1);
        TLine *l2 = new TLine(time_cut[1], 0, time_cut[1], ymax * 1.1);
        l1->SetLineStyle(2); l1->SetLineColor(kGray+1); l1->Draw();
        l2->SetLineStyle(2); l2->SetLineColor(kGray+1); l2->Draw();
        c->SaveAs(Form("%s/notrack_timing_full.png", output_dir.c_str()));
        delete c;
    }

    // i_frame vs coarse (trigger count check)
    {
        TCanvas *c = new TCanvas("c_iframe_coarse",
            "i_frame vs beam-trigger coarse (trigger count check)", 900, 800);
        h2_iframe_coarse->Draw("COLZ");
        if (h2_iframe_coarse->GetEntries() > 0) gPad->SetLogz(1);
        c->SaveAs(Form("%s/notrack_iframe_coarse.png", output_dir.c_str()));
        delete c;
    }

    // Δcoarse between consecutive frames
    {
        TCanvas *c = new TCanvas("c_coarse_diff",
            "Coarse time step between consecutive triggered frames", 900, 600);
        h_coarse_diff->SetLineColor(kBlue+1); h_coarse_diff->SetLineWidth(2);
        h_coarse_diff->Draw("HIST");
        c->SaveAs(Form("%s/notrack_coarse_diff.png", output_dir.c_str()));
        delete c;
    }

    // =========================================================================
    //  Save ROOT file
    // =========================================================================
    std::string output_root = output_dir + "/notrack.root";
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");
    for (auto &kv : cfg._data)
        TNamed(kv.first.c_str(), kv.second.c_str()).Write();
    TNamed("run_name", run_name.c_str()).Write();
    h_multiplicity->Write();
    h_nhits_notrack->Write(); h_nhits_1track->Write(); h_nhits_ntrack->Write();
    h_t_notrack->Write();     h_t_1track->Write();     h_t_ntrack->Write();
    h_hitmap_notrack->Write();h_hitmap_1track->Write();h_hitmap_ntrack->Write();
    h_raw_nhits_notrack->Write(); h_raw_nhits_1track->Write();
    h_t_notrack_full->Write();    h_t_1track_full->Write();
    h2_iframe_coarse->Write();
    h_coarse_diff->Write();
    h_n_triggers->Write();
    fout->Close();

    std::cout << "Output: " << output_root << std::endl;
}
