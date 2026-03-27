#include "../lib_loader.h"
#include "ringtrack_config.h"

// ---------------------------------------------------------------------------

void ringtrack_draw_display(std::string data_repository, std::string run_name,
                            std::string conf_path = "ringtrack.conf",
                            std::string output_dir = "")
{
    RingtrackConfig cfg;
    cfg.load(conf_path);

    if (output_dir.empty())
        output_dir = data_repository + "/" + run_name + "/plots";

    std::string input_root = output_dir + "/histograms.root";

    const int n_display_events = cfg.get_int("n_display_events", 100000);
    const std::string display_nhits_cut_mode = cfg.get_string("display_nhits_cut_mode", "none");
    const int display_nhits_min = cfg.get_int("display_nhits_min", 0);
    const int display_nhits_max = cfg.get_int("display_nhits_max", 8);

    TFile *f = new TFile(input_root.c_str(), "READ");
    if (!f || f->IsZombie())
    {
        std::cerr << "[ERROR] Could not open " << input_root << std::endl;
        return;
    }

    TH1::AddDirectory(false);
    auto get = [&](const char *name) -> TObject * { return f->Get(name); };

    TH2F   *h_display_hits              = (TH2F   *)get("h_display_hits");
    TGraph *g_display_intercepts_single = (TGraph  *)get("g_display_intercepts_single");
    TGraph *g_display_intercepts_multi  = (TGraph  *)get("g_display_intercepts_multi");
    TCutG  *cutg2_display               = (TCutG   *)get("cutg2_display");
    TH2F   *h_intercept_drich_zero_hits = (TH2F   *)get("h_intercept_drich_zero_hits");

    f->Close();

    gROOT->SetBatch(true);

    // range del display: da conf o dinamico dalle intercette
    const std::string plane2 = cfg.get_string("plane2", "NONE");
    const std::string side2  = cfg.get_string("side2",  "OUTSIDE");
    const std::string plane1 = cfg.get_string("plane1", "NONE");
    const std::string side1  = cfg.get_string("side1",  "OUTSIDE");
    bool drich_inside = (plane2 == "DRICH" && side2 == "INSIDE") ||
                        (plane1 == "DRICH" && side1 == "INSIDE");

    float display_range = 200.f;
    if (drich_inside)
    {
        display_range = 100.f;
    }
    else if (g_display_intercepts_single || g_display_intercepts_multi)
    {
        // calcola range dalle intercette
        double xmax = 0;
        auto check_graph = [&](TGraph *g) {
            if (!g) return;
            for (int i = 0; i < g->GetN(); i++)
            {
                double x, y;
                g->GetPoint(i, x, y);
                xmax = std::max(xmax, std::max(fabs(x), fabs(y)));
            }
        };
        check_graph(g_display_intercepts_single);
        check_graph(g_display_intercepts_multi);
        // se tutte le intercette stanno dentro 100 mm, usa 100
        if (xmax <= 100.) display_range = 100.f;
    }

    // -------------------------------------------------------------------------
    //  Build title suffix from nhits cut
    // -------------------------------------------------------------------------
    std::string nhits_label = "";
    if (display_nhits_cut_mode == "greater")
        nhits_label = Form(" | n_{hits} > %d", display_nhits_min);
    else if (display_nhits_cut_mode == "range")
        nhits_label = Form(" | %d #leq n_{hits} #leq %d", display_nhits_min, display_nhits_max);

    // -------------------------------------------------------------------------
    //  Canvas: hits + intercepts
    // -------------------------------------------------------------------------
    TCanvas *c_display = new TCanvas("c_display",
        Form("Hits + intercepts (%d events)%s", n_display_events, nhits_label.c_str()),
        1100, 900);
    c_display->SetLeftMargin(0.12);
    c_display->SetRightMargin(0.25);
    c_display->SetTopMargin(0.10);
    c_display->SetBottomMargin(0.10);

    h_display_hits->GetXaxis()->SetRangeUser(-display_range, display_range);
    h_display_hits->GetYaxis()->SetRangeUser(-display_range, display_range);
    h_display_hits->SetAxisRange(-display_range, display_range, "X");
    h_display_hits->SetAxisRange(-display_range, display_range, "Y");
    h_display_hits->Draw("SCAT");
    c_display->Update();
    c_display->GetFrame()->SetX1(-display_range);
    c_display->GetFrame()->SetX2( display_range);
    c_display->GetFrame()->SetY1(-display_range);
    c_display->GetFrame()->SetY2( display_range);

    // cutg2 come TBox
    if (cutg2_display)
    {
        double xmin = 1e9, xmax = -1e9, ymin = 1e9, ymax = -1e9;
        for (int i = 0; i < cutg2_display->GetN(); i++)
        {
            double x, y;
            cutg2_display->GetPoint(i, x, y);
            xmin = std::min(xmin, x); xmax = std::max(xmax, x);
            ymin = std::min(ymin, y); ymax = std::max(ymax, y);
        }
        TBox *box = new TBox(xmin, ymin, xmax, ymax);
        box->SetFillColorAlpha(kYellow, 0.35);
        box->SetLineColor(kYellow + 1);
        box->SetLineWidth(2);
        box->Draw("SAME");
    }

    // intercette single (rosso) e multi (blu)
    bool has_single = g_display_intercepts_single && g_display_intercepts_single->GetN() > 0;
    bool has_multi  = g_display_intercepts_multi  && g_display_intercepts_multi->GetN()  > 0;

    if (has_single)
    {
        g_display_intercepts_single->SetMarkerStyle(29);
        g_display_intercepts_single->SetMarkerColor(kRed);
        g_display_intercepts_single->SetMarkerSize(0.9);
        g_display_intercepts_single->Draw("P SAME");
    }
    if (has_multi)
    {
        g_display_intercepts_multi->SetMarkerStyle(29);
        g_display_intercepts_multi->SetMarkerColor(kBlue);
        g_display_intercepts_multi->SetMarkerSize(0.9);
        g_display_intercepts_multi->Draw("P SAME");
    }

    if (has_single || has_multi)
    {
        TLegend *leg = new TLegend(0.77, 0.75, 0.98, 0.88);
        leg->SetBorderSize(1); leg->SetTextFont(42); leg->SetTextSize(0.030);
        leg->SetFillColor(kWhite); leg->SetFillStyle(1001);
        if (has_single) leg->AddEntry(g_display_intercepts_single, "single track", "p");
        if (has_multi)  leg->AddEntry(g_display_intercepts_multi,  "multi track",  "p");
        leg->Draw();
        c_display->RedrawAxis();
    }

    // -------------------------------------------------------------------------
    //  Save
    // -------------------------------------------------------------------------
    std::string output_root = output_dir + "/display.root";
    gSystem->mkdir(output_dir.c_str(), true);
    c_display->SaveAs(Form("%s/c_display.png", output_dir.c_str()));
    TFile *fout = new TFile(output_root.c_str(), "RECREATE");
    c_display->Write();
    fout->Close();

    cout << "Output: " << output_root << endl;
}