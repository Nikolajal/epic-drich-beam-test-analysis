#include "../lib_loader.h"
#include <mist/logger/logger.h>

void dark_count_rate(std::string data_repository = "/Users/nrubini/Analysis/ePIC/test-beam-rec/Data/",
                     std::string run_name = "20251111-164951",
                     int max_frames = 10000000)
{
    //  Input files
    std::string input_filename_recodata = data_repository + "/" + run_name + "/recodata.root";

    TFile *input_file_recodata = new TFile(input_filename_recodata.c_str());
    if (!input_file_recodata || input_file_recodata->IsZombie())
    {
        mist::logger::error("[dark_count_rate] Could not open recodata: " + input_filename_recodata);
        return;
    }

    TTree *recodata_tree = (TTree *)input_file_recodata->Get("recodata");
    alcor_recodata *recodata = new alcor_recodata();
    if (!recodata->link_to_tree(recodata_tree))
        return;

    auto n_frames = recodata_tree->GetEntries();
    auto all_frames = min((int)n_frames, (int)max_frames);
    auto used_frames = 0;

    // ── Histograms ────────────────────────────────────────────────────────────
    TH1F *h_dcr = new TH1F("h_dcr", "Dark Count Rate;DCR [kHz];Entries", 40, 0, 20);
    TProfile *h_dcr_per_channel = new TProfile("h_dcr_per_channel", ";channel;DCR [kHz];", 2048, 0, 2048);
    TH1F *h_average_dcr = new TH1F("h_average_dcr", "1350;DCR [kHz];Entries", 50, 0, 10);
    TH1F *h_average_dcr_2 = new TH1F("h_average_dcr_2", "1375;DCR [kHz];Entries", 50, 0, 10);

    // Log-binned distributions for Gaussian fitting (mirrors extract_DCR)
    // Log binning is used here because DCR spans ~3 decades (0.1–100 kHz);
    // uniform bins would severely under-sample the low-DCR region.
    auto make_log_bins = [](const char *name, const char *title,
                            int nbins, double xmin, double xmax) -> TH1F *
    {
        std::vector<double> bins(nbins + 1);
        const double log_min = std::log10(xmin);
        const double log_max = std::log10(xmax);
        const double step = (log_max - log_min) / nbins;
        for (int i = 0; i <= nbins; ++i)
            bins[i] = std::pow(10., log_min + i * step);
        return new TH1F(name, title, nbins, bins.data());
    };

    TH1F *h_dcr_log_1350 = make_log_bins(("h_dcr_log_1350_" + run_name).c_str(),
                                         (run_name + "; DCR (kHz)").c_str(), 100, 0.1, 100);
    TH1F *h_dcr_log_1375 = make_log_bins(("h_dcr_log_1375_" + run_name).c_str(),
                                         (run_name + "; DCR (kHz)").c_str(), 100, 0.1, 100);

    std::set<uint32_t> active_sensors;
    std::unordered_map<uint32_t, uint16_t> active_sensors_count;

    // ── Main loop ─────────────────────────────────────────────────────────────
    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        recodata_tree->GetEntry(i_frame);

        if (recodata->is_start_of_spill())
        {
            active_sensors.clear();
            active_sensors_count.clear();
            for (auto hit = 0; hit < (int)recodata->get_recodata().size(); hit++)
                active_sensors.insert(recodata->get_global_channel_index(hit));
            continue;
        }

        if (recodata->is_first_frames())
        {
            used_frames++;
            for (const auto &global_channel_index : active_sensors)
                active_sensors_count[global_channel_index] = 0;
            for (auto hit = 0; hit < (int)recodata->get_recodata().size(); hit++)
                active_sensors_count[recodata->get_global_channel_index(hit)]++;
            for (auto &[global_channel_index, count] : active_sensors_count)
                h_dcr_per_channel->Fill(global_channel_index, count);

            h_dcr->Fill(recodata->get_recodata().size() * 1. / (_FRAME_LENGTH_NS_ * 1.e-6 * active_sensors.size()));
        }
    }

    // ── Normalise ─────────────────────────────────────────────────────────────
    h_dcr_per_channel->Scale(1. / (_FRAME_LENGTH_NS_ * 1.e-6));
    h_dcr->Scale(1. / used_frames);

    // ── Fill log-binned distributions from per-channel profile ───────────────
    // Threshold at 100 kHz matches lightdata_writer convention (extract_DCR).
    // Channels <= 1024 are SiPM 1350; channels > 1024 are SiPM 1375.
    const double dcr_threshold = 100.0; // kHz
    for (int x_bin = 1; x_bin <= h_dcr_per_channel->GetNbinsX(); ++x_bin)
    {
        double dcr = h_dcr_per_channel->GetBinContent(x_bin);
        int global_index = (int)h_dcr_per_channel->GetBinCenter(x_bin);
        if (dcr < 0.001 || dcr <= dcr_threshold)
            continue;
        if (global_index <= 1024)
        {
            h_average_dcr->Fill(dcr);
            h_dcr_log_1350->Fill(dcr);
        }
        else
        {
            h_average_dcr_2->Fill(dcr);
            h_dcr_log_1375->Fill(dcr);
        }
    }

    // ── Gaussian fits on log-binned distributions ─────────────────────────────
    // 1350: single Gaussian — one well-defined population expected.
    // 1375: double Gaussian — two SiPM batches with distinct breakdown voltages
    //       produce a bimodal DCR distribution at fixed Vbias.
    TF1 *f_gaus_1350 = new TF1(("f_gaus_1350_" + run_name).c_str(),
                               "[0]*TMath::Gaus(x,[1],[2],true)", 0.1, 100);
    TF1 *f_gaus_1375 = new TF1(("f_gaus_1375_" + run_name).c_str(),
                               "[0]*TMath::Gaus(x,[1],[2],true)"
                               "+[3]*TMath::Gaus(x,[4],[5],true)",
                               0.1, 100);

    f_gaus_1350->SetParameters(h_dcr_log_1350->GetMaximum(),
                               h_dcr_log_1350->GetMean(),
                               h_dcr_log_1350->GetRMS());

    f_gaus_1375->SetParameters(h_dcr_log_1375->GetMaximum(),
                               h_dcr_log_1375->GetMean() - h_dcr_log_1375->GetRMS() / 2.,
                               h_dcr_log_1375->GetRMS() / 4.,
                               h_dcr_log_1375->GetMaximum(),
                               h_dcr_log_1375->GetMean() + h_dcr_log_1375->GetRMS() / 2.,
                               h_dcr_log_1375->GetRMS() / 4.);

    TCanvas *c_log_1350 = new TCanvas(("c_dcr_log_1350_" + run_name).c_str(), "DCR 1350 (log)", 800, 600);
    gPad->SetLogx();
    h_dcr_log_1350->Draw();
    h_dcr_log_1350->Fit(f_gaus_1350, "QR");

    TCanvas *c_log_1375 = new TCanvas(("c_dcr_log_1375_" + run_name).c_str(), "DCR 1375 (log)", 800, 600);
    gPad->SetLogx();
    h_dcr_log_1375->Draw();
    h_dcr_log_1375->Fit(f_gaus_1375, "QR");

    // ── Persist to AnalysisResults ────────────────────────────────────────────
    // 1375 stores both peaks; peak ordering by mean so peak1 < peak2 always.
    // This makes downstream plotting loops unambiguous about which peak is which.
    {
        const double mean1_1375 = f_gaus_1375->GetParameter(1);
        const double mean2_1375 = f_gaus_1375->GetParameter(4);
        const bool peak1_lower = (mean1_1375 < mean2_1375);

        AnalysisResults ar(data_repository + "/standard_results.root");
        ar.update({
            // ── SiPM 1350: single Gaussian ───────────────────────────────────
            {{run_name, "1350", "dcr.mean"}, {f_gaus_1350->GetParameter(1), f_gaus_1350->GetParameter(2)}},
            {{run_name, "1350", "dcr.sigma"}, {f_gaus_1350->GetParameter(2), 0.}},

            // ── SiPM 1375: double Gaussian, low peak ─────────────────────────
            {{run_name, "1375", "dcr.peak_lo.mean"}, {peak1_lower ? mean1_1375 : mean2_1375, peak1_lower ? f_gaus_1375->GetParameter(2) : f_gaus_1375->GetParameter(5)}},
            {{run_name, "1375", "dcr.peak_lo.sigma"}, {peak1_lower ? f_gaus_1375->GetParameter(2) : f_gaus_1375->GetParameter(5), 0.}},

            // ── SiPM 1375: double Gaussian, high peak ────────────────────────
            {{run_name, "1375", "dcr.peak_hi.mean"}, {peak1_lower ? mean2_1375 : mean1_1375, peak1_lower ? f_gaus_1375->GetParameter(5) : f_gaus_1375->GetParameter(2)}},
            {{run_name, "1375", "dcr.peak_hi.sigma"}, {peak1_lower ? f_gaus_1375->GetParameter(5) : f_gaus_1375->GetParameter(2), 0.}},
        });

        mist::logger::info("[dark_count_rate] DCR fit results written to standard_results.root for run " + run_name);
    }

    // ── Drawing ───────────────────────────────────────────────────────────────
    gStyle->SetOptStat(0);
    TCanvas *c_dcr = new TCanvas("c_dcr", "Dark Count Rate", 800, 600);
    h_dcr->Draw();
    TCanvas *c_test2 = new TCanvas("c_test2", "Per-channel DCR", 800, 600);
    h_dcr_per_channel->Draw();
    TCanvas *c_test3 = new TCanvas("c_test3", "Average DCR by sensor", 800, 600);
    h_average_dcr->SetLineWidth(2);
    h_average_dcr->SetLineColor(kRed);
    h_average_dcr_2->SetLineWidth(2);
    h_average_dcr_2->SetLineColor(kBlue);
    h_average_dcr->Draw();
    h_average_dcr_2->Draw("SAME");
    gPad->BuildLegend();
}