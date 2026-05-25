#include "../lib_loader.h"
#include "dark_count_rate.cpp"
#include "photon_number.cpp"

void my_test(
    std::string runlist_name = "vbias_min28",
    std::string data_repository = "extData")
{
    // ── Load run list and metadata ────────────────────────────────────────────
    RunInfo::read_database("run-lists/2025.database.toml");
    RunInfo::read_runslists("run-lists/2025.runlists.toml");

    auto maybe_runs = RunInfo::get_run_list(runlist_name);
    if (!maybe_runs)
    {
        mist::logger::error("[my_test] Run list '" + runlist_name + "' not found");
        return;
    }
    const std::vector<std::string> &run_names = *maybe_runs;

    // ── Collect Vbias per run from database ───────────────────────────────────
    std::vector<double> vbias_vals;
    std::vector<std::string> valid_runs;
    for (const auto &run : run_names)
    {
        auto info = RunInfo::get_run_info(run);
        if (!info)
        {
            mist::logger::warning("[my_test] No metadata for run " + run + " — skipped");
            continue;
        }
        vbias_vals.push_back(info->v_bias);
        valid_runs.push_back(run);
    }

    // ── Load analysis results ─────────────────────────────────────────────────
    AnalysisResults ar(data_repository + "/standard_results.root");
    const ResultMap m_check = ar.load();

    for (const auto &run : run_names)
    {
        //dark_count_rate(data_repository, run);
        // Use ex_gap.n_gamma on sensor "all" as a proxy for "this run is done"
        if (!m_check.count({run, "all", "ex_gap.n_gamma"}))
        {
            //dark_count_rate(data_repository, run);
            //photon_number(data_repository, run);
        }
        else
            mist::logger::info("[my_test] Skipping already-processed run " + run);
    }
    const ResultMap m = ar.load();

    // ── Helper: style a TGraphErrors ─────────────────────────────────────────
    auto style_graph = [](TGraphErrors *g, Color_t color, Style_t marker)
    {
        g->SetMarkerStyle(marker);
        g->SetMarkerSize(1.2);
        g->SetMarkerColor(color);
        g->SetLineColor(color);
        g->SetLineWidth(2);
    };

    gStyle->SetOptStat(0);

    // ── Plot 1: DCR vs Vbias ──────────────────────────────────────────────────
    // DCR mean is the central value; sigma is stored as the error on the mean.
    auto *g_dcr_1350 = make_graph(m, valid_runs, vbias_vals, "1350", "dcr.mean");
    auto *g_dcr_1375_lo = make_graph(m, valid_runs, vbias_vals, "1375", "dcr.peak_lo.mean");
    auto *g_dcr_1375_hi = make_graph(m, valid_runs, vbias_vals, "1375", "dcr.peak_hi.mean");

    style_graph(g_dcr_1350, kBlue + 1, 20);
    style_graph(g_dcr_1375_lo, kRed + 1, 21);
    style_graph(g_dcr_1375_hi, kRed + 1, 25);

    g_dcr_1350->SetTitle("DCR vs V_{bias};V_{bias} (V);DCR (kHz)");

    TH1F *h_frame_dcr = new TH1F("h_frame_dcr", ";V_{bias} (V);DCR (kHz)", 50, 50, 55);
    h_frame_dcr->SetMinimum(0.25);
    h_frame_dcr->SetMaximum(25);

    TCanvas *c1 = new TCanvas("c_dcr_vs_vbias", "DCR vs Vbias", 800, 600);
    gPad->SetMargin(0.15, 0.15, 0.15, 0.15);
    gPad->SetLogy();
    h_frame_dcr->Draw();
    g_dcr_1350->Draw("SAME P");
    g_dcr_1375_lo->Draw("SAME P");
    g_dcr_1375_hi->Draw("SAME P");

    TLegend *leg1 = new TLegend(0.55, 0.18, 0.75, 0.33);
    leg1->SetBorderSize(0);
    leg1->SetFillStyle(0);
    leg1->SetTextSize(0.035);
    leg1->AddEntry(g_dcr_1350, "SiPM 1350", "pe");
    leg1->AddEntry(g_dcr_1375_lo, "SiPM 1375 (low peak)", "pe");
    leg1->AddEntry(g_dcr_1375_hi, "SiPM 1375 (high peak)", "pe");
    leg1->Draw();
    c1->Update();

    // ── DCR x-values for the next three plots ────────────────────────────────
    // Build parallel DCR vectors from the stored means for use as x-axis.
    // x-error = stored sigma (population spread, not uncertainty on the mean).
    std::vector<double> dcr_vals_1350, dcr_err_1350;
    std::vector<double> dcr_vals_1375, dcr_err_1375;
    std::vector<std::string> runs_with_dcr_1350, runs_with_dcr_1375;

    for (const auto &run : valid_runs)
    {
        auto it_mean = m.find({run, "1350", "dcr.mean"});
        auto it_sigma = m.find({run, "1350", "dcr.sigma"});
        if (it_mean != m.end())
        {
            dcr_vals_1350.push_back(it_mean->second.value);
            dcr_err_1350.push_back(it_sigma != m.end() ? it_sigma->second.value : 0.);
            runs_with_dcr_1350.push_back(run);
        }

        auto it_mean_hi = m.find({run, "1375", "dcr.peak_hi.mean"});
        auto it_sigma_hi = m.find({run, "1375", "dcr.peak_hi.sigma"});
        if (it_mean_hi != m.end())
        {
            dcr_vals_1375.push_back(it_mean_hi->second.value);
            dcr_err_1375.push_back(it_sigma_hi != m.end() ? it_sigma_hi->second.value : 0.);
            runs_with_dcr_1375.push_back(run);
        }
    }

    // ── Plot 2: N_gamma vs DCR ────────────────────────────────────────────────
    auto *g_ng_1350 = make_graph(m, runs_with_dcr_1350, dcr_vals_1350, "1350", "ex_gap.n_gamma", "dcr.sigma");
    auto *g_ng_1375 = make_graph(m, runs_with_dcr_1375, dcr_vals_1375, "1375", "ex_gap.n_gamma", "dcr.peak_hi.sigma");
    style_graph(g_ng_1350, kBlue + 1, 20);
    style_graph(g_ng_1375, kRed + 1, 21);
    g_ng_1350->SetTitle("N_{#gamma} vs DCR;DCR (kHz);N_{#gamma}");

    TCanvas *c2 = new TCanvas("c_ngamma_vs_dcr", "N_gamma vs DCR", 800, 600);
    gPad->SetMargin(0.15, 0.15, 0.15, 0.15);
    g_ng_1350->Draw("AP");
    g_ng_1375->Draw("SAME P");

    TLegend *leg2 = new TLegend(0.18, 0.70, 0.50, 0.88);
    leg2->SetBorderSize(0);
    leg2->SetFillStyle(0);
    leg2->SetTextSize(0.035);
    leg2->AddEntry(g_ng_1350, "SiPM 1350", "pe");
    leg2->AddEntry(g_ng_1375, "SiPM 1375 (high peak)", "pe");
    leg2->Draw();
    c2->Update();

    // ── Plot 3: Sigma vs DCR ──────────────────────────────────────────────────
    auto *g_sig_1350 = make_graph(m, runs_with_dcr_1350, dcr_vals_1350, "1350", "ex_gap.sigma", "dcr.sigma");
    auto *g_sig_1375 = make_graph(m, runs_with_dcr_1375, dcr_vals_1375, "1375", "ex_gap.sigma", "dcr.peak_hi.sigma");
    style_graph(g_sig_1350, kBlue + 1, 20);
    style_graph(g_sig_1375, kRed + 1, 21);
    g_sig_1350->SetTitle("#sigma_{R} vs DCR;DCR (kHz);#sigma_{R} (mm)");

    TCanvas *c3 = new TCanvas("c_sigma_vs_dcr", "Sigma vs DCR", 800, 600);
    gPad->SetMargin(0.15, 0.15, 0.15, 0.15);
    g_sig_1350->Draw("AP");
    g_sig_1375->Draw("SAME P");

    TLegend *leg3 = new TLegend(0.18, 0.70, 0.50, 0.88);
    leg3->SetBorderSize(0);
    leg3->SetFillStyle(0);
    leg3->SetTextSize(0.035);
    leg3->AddEntry(g_sig_1350, "SiPM 1350", "pe");
    leg3->AddEntry(g_sig_1375, "SiPM 1375 (high peak)", "pe");
    leg3->Draw();
    c3->Update();

    // ── Plot 4: Sigma vs N_gamma ──────────────────────────────────────────────
    // x = N_gamma (value ± error), y = sigma (value, error=0 in store)
    std::vector<double> ng_vals_1350, ng_vals_1375;
    std::vector<std::string> runs_ng_1350, runs_ng_1375;

    for (const auto &run : valid_runs)
    {
        if (m.count({run, "1350", "ex_gap.n_gamma"}))
        {
            ng_vals_1350.push_back(m.at({run, "1350", "ex_gap.n_gamma"}).value);
            runs_ng_1350.push_back(run);
        }
        if (m.count({run, "1375", "ex_gap.n_gamma"}))
        {
            ng_vals_1375.push_back(m.at({run, "1375", "ex_gap.n_gamma"}).value);
            runs_ng_1375.push_back(run);
        }
    }

    auto *g_sig_ng_1350 = make_graph(m, runs_ng_1350, ng_vals_1350, "1350", "ex_gap.sigma");
    auto *g_sig_ng_1375 = make_graph(m, runs_ng_1375, ng_vals_1375, "1375", "ex_gap.sigma");
    style_graph(g_sig_ng_1350, kBlue + 1, 20);
    style_graph(g_sig_ng_1375, kRed + 1, 21);
    g_sig_ng_1350->SetTitle("#sigma_{R} vs N_{#gamma};N_{#gamma};#sigma_{R} (mm)");

    TCanvas *c4 = new TCanvas("c_sigma_vs_ngamma", "Sigma vs N_gamma", 800, 600);
    gPad->SetMargin(0.15, 0.15, 0.15, 0.15);
    g_sig_ng_1350->Draw("AP");
    g_sig_ng_1375->Draw("SAME P");

    TLegend *leg4 = new TLegend(0.55, 0.70, 0.88, 0.88);
    leg4->SetBorderSize(0);
    leg4->SetFillStyle(0);
    leg4->SetTextSize(0.035);
    leg4->AddEntry(g_sig_ng_1350, "SiPM 1350", "pe");
    leg4->AddEntry(g_sig_ng_1375, "SiPM 1375", "pe");
    leg4->Draw();
    c4->Update();
}