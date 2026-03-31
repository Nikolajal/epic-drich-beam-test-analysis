#include "../lib_loader.h"
#include "ringtrack_config.h"
#include <fstream>
#include <sstream>
#include <array>
#include <algorithm>

// ===========================================================================
//  ringtrack_timing
//  Per ogni frame: trova il bin di 25 ns dominante nella distribuzione
//  temporale degli hit. Per ogni traccia del frame: scrive la carta
//  d'identità su file txt e riempie istogrammi 2D di controllo.
// ===========================================================================

void ringtrack_timing(std::string data_repository, std::string run_name,
                      std::string conf_path = "ringtrack.conf",
                      std::string output_dir = "")
{
    RingtrackConfig cfg;
    cfg.load(conf_path);
    cfg.print();

    const int   first_event        = cfg.get_int("first_event",   0);
    const int   max_frames_        = cfg.get_int("max_frames",    1000000);
    const float z_drich            = cfg.get_float("z_drich",    -4250.f);
    const float z_scint            = cfg.get_float("z_scint",    -1150.f);
    const bool  apply_afterpulse   = cfg.get_bool("apply_afterpulse_cut", true);

    // finestra temporale totale e bin width
    const float t_min     = -312.5f;
    const float t_max     =  312.5f;
    const float bin_width = cfg.get_float("timing_bin_width", 25.0f);
    const int   n_bins    = (int)((t_max - t_min) / bin_width);

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

    // -------------------------------------------------------------------------
    //  Output txt
    // -------------------------------------------------------------------------
    if (output_dir.empty())
        output_dir = data_repository + "/plots/" + run_name;
    std::ofstream txt(output_dir + "/track_timing.txt");
    txt << "frame\ttrack_idx\tix_drich\tiy_drich\ttheta\tphi\tchi2ndof\t"
        << "dominant_window_center\tn_hits_dominant\tn_hits_total\tdominance_ratio\n";

    // -------------------------------------------------------------------------
    //  Histograms
    // -------------------------------------------------------------------------

    // distribuzione temporale globale
    TH1F *h_t_all = new TH1F("h_t_all", "Hit time wrt trigger (all frames);t (ns);counts", n_bins, t_min, t_max);

    // per ogni bin temporale: mappa 2D delle intercette
    std::vector<TH1F*> h_ix_per_bin(n_bins);
    std::vector<TH1F*> h_iy_per_bin(n_bins);
    for (int b = 0; b < n_bins; b++)
    {
        float bc = t_min + (b + 0.5f) * bin_width;
        h_ix_per_bin[b] = new TH1F(
            Form("h_ix_bin%02d", b),
            Form("Track intercept X | window center %.0f ns;x_{dRICH} (mm);counts", bc),
            120, -200., 200.);
        h_iy_per_bin[b] = new TH1F(
            Form("h_iy_bin%02d", b),
            Form("Track intercept Y | window center %.0f ns;y_{dRICH} (mm);counts", bc),
            120, -200., 200.);
    }

    TH2F *h_ix_vs_window    = new TH2F("h_ix_vs_window",    "Track intercept X vs dominant window;dominant window center (ns);x_{dRICH} (mm)",  n_bins, t_min, t_max, 120, -200., 200.);
    TH2F *h_iy_vs_window    = new TH2F("h_iy_vs_window",    "Track intercept Y vs dominant window;dominant window center (ns);y_{dRICH} (mm)",  n_bins, t_min, t_max, 120, -200., 200.);
    TH2F *h_theta_vs_window = new TH2F("h_theta_vs_window", "Track #theta vs dominant window;dominant window center (ns);#theta (rad)",          n_bins, t_min, t_max, 200, 0., 0.1);
    TH2F *h_chi2_vs_window  = new TH2F("h_chi2_vs_window",  "Track chi2/ndof vs dominant window;dominant window center (ns);chi2/ndof",          n_bins, t_min, t_max, 100, 0., 20.);
    TH1F *h_dominant_window = new TH1F("h_dominant_window", "Dominant window center per frame;dominant window center (ns);n frames",              n_bins, t_min, t_max);
    TH1F *h_n_hits_dominant = new TH1F("h_n_hits_dominant", "N hits in dominant window per frame;n hits;n frames",                               50, 0., 50.);

    TH1F *h_dominance_ratio = new TH1F("h_dominance_ratio", "Dominance ratio per frame;N_{dominant} / N_{total};n frames", 100, 0., 1.);

    TH2F *h_dominance_vs_window = new TH2F("h_dominance_vs_window", "Dominance ratio vs dominant window;dominant window center (ns);dominance ratio", n_bins, t_min, t_max, 100, 0., 1.);

    TH2F *h_mult_vs_window = new TH2F("h_mult_vs_window", "Track multiplicity vs dominant window;dominant window center (ns);n tracks", n_bins, t_min, t_max, 10, -0.5, 9.5);

    // -------------------------------------------------------------------------
    //  Main loop
    // -------------------------------------------------------------------------
    mist::logger::progress_bar bar(mist::logger::bar_style::BLOCK);
    for (int i_frame = first_event; i_frame < all_frames; ++i_frame)
    {
        tree->GetEntry(i_frame);
        if (i_frame % 10000 == 0) bar.update(i_frame, all_frames);
        if (recotrackdata->is_start_of_spill()) continue;

        auto trigger = recotrackdata->get_trigger_by_index(0);
        if (!trigger) continue;

        // -----------------------------------------------------------------
        //  Conta hit in bins di 25 ns
        // -----------------------------------------------------------------
        std::vector<int> bin_counts(n_bins, 0);
        int n_hits_total = 0;

        for (int i_hit = 0; i_hit < (int)recotrackdata->get_recodata().size(); ++i_hit)
        {
            if (apply_afterpulse && recotrackdata->is_afterpulse(i_hit)) continue;
            float dt = recotrackdata->get_hit_t(i_hit) - trigger->fine_time;
            h_t_all->Fill(dt);
            if (dt < t_min || dt >= t_max) continue;
            int b = (int)((dt - t_min) / bin_width);
            if (b >= 0 && b < n_bins) { ++bin_counts[b]; ++n_hits_total; }
        }

        // trova bin dominante
        int dominant_bin = (int)(std::max_element(bin_counts.begin(), bin_counts.end())
                                 - bin_counts.begin());
        int   n_hits_dom    = bin_counts[dominant_bin];
        float window_center = t_min + (dominant_bin + 0.5f) * bin_width;
        float dominance_ratio = (n_hits_total > 0) ? (float)n_hits_dom / n_hits_total : 0.f;

        h_dominant_window->Fill(window_center);
        h_n_hits_dominant->Fill(n_hits_dom);
        h_dominance_ratio->Fill(dominance_ratio);
        h_dominance_vs_window->Fill(window_center, dominance_ratio);

        // -----------------------------------------------------------------
        //  Tracce
        // -----------------------------------------------------------------
        const int n_tracks = recotrackdata->n_recotrackdata();
        if (n_tracks == 0) continue;

        h_mult_vs_window->Fill(window_center, n_tracks);

        for (int i = 0; i < n_tracks; i++)
        {
            float px      = recotrackdata->get_det_plane_x(i);
            float py      = recotrackdata->get_det_plane_y(i);
            float sx      = recotrackdata->get_traj_angcoeff_x(i);
            float sy      = recotrackdata->get_traj_angcoeff_y(i);
            float ix      = px + sx * z_drich;
            float iy      = py + sy * z_drich;
            float theta   = recotrackdata->get_traj_angcoeff_theta(i);
            float phi     = recotrackdata->get_traj_angcoeff_phi(i);
            float chi2    = recotrackdata->get_chi2ndof(i);

            h_ix_per_bin[dominant_bin]->Fill(ix);
            h_iy_per_bin[dominant_bin]->Fill(iy);
            h_ix_vs_window->Fill(window_center, ix);
            h_iy_vs_window->Fill(window_center, iy);
            h_theta_vs_window->Fill(window_center, theta);
            h_chi2_vs_window->Fill(window_center, chi2);

            txt << i_frame << "\t" << i << "\t"
                << ix << "\t" << iy << "\t"
                << theta << "\t" << phi << "\t"
                << chi2 << "\t"
                << window_center << "\t"
                << n_hits_dom << "\t"
                << n_hits_total << "\t"
                << dominance_ratio << "\n";
        }
    }
    bar.update(all_frames, all_frames);
    bar.finish();
    txt.close();

    // -------------------------------------------------------------------------
    //  Save
    // -------------------------------------------------------------------------
    gROOT->SetBatch(true);
    std::string output_root = output_dir + "/track_timing.root";
    gSystem->mkdir(output_dir.c_str(), true);

    TFile *fout = new TFile(output_root.c_str(), "RECREATE");

    h_t_all->Write();
    h_dominant_window->Write();
    h_n_hits_dominant->Write();
    h_ix_vs_window->Write();
    h_iy_vs_window->Write();
    h_theta_vs_window->Write();
    h_chi2_vs_window->Write();
    h_dominance_ratio->Write();
    h_dominance_vs_window->Write();
    h_mult_vs_window->Write();
    for (int b = 0; b < n_bins; b++)
    {
        h_ix_per_bin[b]->Write();
        h_iy_per_bin[b]->Write();
    }

    // canvas di controllo
    TCanvas *c_t = new TCanvas("c_t", "Hit time distribution", 800, 600);
    gPad->SetLogy(); h_t_all->Draw(); c_t->Write(); c_t->SaveAs((output_dir + "/track_timing_t.png").c_str());

    TCanvas *c_dom = new TCanvas("c_dom", "Dominant window", 800, 600);
    gPad->SetLogy(); h_dominant_window->Draw(); c_dom->Write(); c_dom->SaveAs((output_dir + "/track_timing_dom.png").c_str());

    TCanvas *c_ix_win = new TCanvas("c_ix_win", "ix vs window", 1200, 600);
    c_ix_win->Divide(2,1);
    c_ix_win->cd(1); h_ix_vs_window->Draw("COLZ");
    c_ix_win->cd(2); h_iy_vs_window->Draw("COLZ");
    c_ix_win->Write(); c_ix_win->SaveAs((output_dir + "/track_timing_ix_iy.png").c_str());

    TCanvas *c_theta = new TCanvas("c_theta", "theta vs window", 800, 600);
    h_theta_vs_window->Draw("COLZ"); c_theta->Write(); c_theta->SaveAs((output_dir + "/track_timing_theta.png").c_str());

    TCanvas *c_chi2 = new TCanvas("c_chi2", "chi2 vs window", 800, 600);
    h_chi2_vs_window->Draw("COLZ"); c_chi2->Write(); c_chi2->SaveAs((output_dir + "/track_timing_chi2.png").c_str());

    TCanvas *c_dominance = new TCanvas("c_dominance", "Dominance ratio", 1200, 600);
    c_dominance->Divide(2, 1);
    c_dominance->cd(1); h_dominance_ratio->Draw();
    c_dominance->cd(2); h_dominance_vs_window->Draw("COLZ");
    c_dominance->Write(); c_dominance->SaveAs((output_dir + "/track_timing_dominance.png").c_str());

    TCanvas *c_mult = new TCanvas("c_mult", "multiplicity vs window", 800, 600);
    h_mult_vs_window->Draw("COLZ"); c_mult->Write(); c_mult->SaveAs((output_dir + "/track_timing_mult.png").c_str());

    // canvas confronto: sovrappone solo i bin con >= min_entries intercette
    const int min_entries = 50;
    std::vector<int> palette = {kBlack, kRed, kBlue, kGreen+2, kMagenta, kOrange+1, kCyan+1, kViolet};

    auto make_compare = [&](const char *cname, const char *title,
                            std::vector<TH1F*> &hv, const char *xtitle,
                            const char *fname) -> TCanvas*
    {
        TCanvas *c = new TCanvas(cname, title, 900, 600);
        gPad->SetLogy();
        TLegend *leg = new TLegend(0.12, 0.70, 0.88, 0.92);
        leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.025);
        leg->SetFillColor(kWhite); leg->SetFillStyle(1001); leg->SetNColumns(3);

        double ymax = 0;
        for (int b = 0; b < n_bins; b++)
            if (hv[b]->GetEntries() >= min_entries)
                ymax = std::max(ymax, hv[b]->GetMaximum());

        bool first = true; int col_idx = 0;
        for (int b = 0; b < n_bins; b++)
        {
            if (hv[b]->GetEntries() < min_entries) continue;
            float bc = t_min + (b + 0.5f) * bin_width;
            hv[b]->SetLineColor(palette[col_idx % palette.size()]);
            hv[b]->SetLineWidth(2); hv[b]->SetStats(0);
            hv[b]->SetMaximum(ymax * 2.); hv[b]->SetMinimum(0.5);
            hv[b]->SetTitle(Form("%s;%s;counts", title, xtitle));
            hv[b]->Draw(first ? "HIST" : "HIST SAME");
            leg->AddEntry(hv[b], Form("%.0f ns", bc), "l");
            first = false; col_idx++;
        }
        if (!first) { leg->Draw(); }
        c->Write(); c->SaveAs(fname);
        return c;
    };

    make_compare("c_compare_ix", "Track intercept X by dominant window",
        h_ix_per_bin, "x_{dRICH} (mm)",
        (output_dir + "/track_timing_compare_ix.png").c_str());

    make_compare("c_compare_iy", "Track intercept Y by dominant window",
        h_iy_per_bin, "y_{dRICH} (mm)",
        (output_dir + "/track_timing_compare_iy.png").c_str());

    fout->Close();

    std::cout << "Output: " << output_root << std::endl;
    std::cout << "Track timing txt: " << output_dir << "/track_timing.txt" << std::endl;
}