#include <TGraphErrors.h>
#include <TCanvas.h>
#include <TAxis.h>
#include <TLegend.h>
#include <iostream>

void radiator_npe_comparison()
{
    // ============================================================================
    // Data: NPE measurements for each radiator-method combination
    // ============================================================================

    // Radiators: 1=Aerogel, 2=Argon, 3=C2F6
    // Methods: 1=MAPMT, 2=SiPMs

    const int kNumRadiators = 3;

    // NPE values (method 1 = MAPMT, method 2 = SiPMs)
    double npe_mapmt[kNumRadiators] = {5.5 / 0.4, 29.3 / 0.9171, 31.2 / 0.9171};
    double npe_sipms[kNumRadiators] = {23.9, 34.0, 32.6};

    // Uncertainties (stat + syst combined)
    double err_mapmt[kNumRadiators] = {0.0, 0.0, 0.0};
    double err_sipms[kNumRadiators] = {0.5, 0.8, 0.8};

    // X-positions: same position for both methods per radiator
    // Radiator 1: x=1, Radiator 2: x=2, Radiator 3: x=3
    const char *radiator_names[kNumRadiators] = {"Aerogel", "Argon", "C2F6"};
    double x_position[kNumRadiators] = {1.0, 2.0, 3.0};

    // ============================================================================
    // Build TGraphErrors for MAPMT method (filled markers)
    // ============================================================================

    double x_mapmt[kNumRadiators];
    double y_mapmt[kNumRadiators];
    double ex_mapmt[kNumRadiators];
    double ey_mapmt[kNumRadiators];

    for (int i = 0; i < kNumRadiators; ++i)
    {
        x_mapmt[i] = x_position[i];
        y_mapmt[i] = npe_mapmt[i];
        ex_mapmt[i] = 0.0;
        ey_mapmt[i] = err_mapmt[i];
    }

    TGraphErrors *graph_mapmt = new TGraphErrors(kNumRadiators, x_mapmt, y_mapmt,
                                                 ex_mapmt, ey_mapmt);
    graph_mapmt->SetName("graph_mapmt");
    graph_mapmt->SetTitle("MAPMT Method");
    graph_mapmt->SetMarkerColor(kBlue);
    graph_mapmt->SetMarkerStyle(20); // Filled circle
    graph_mapmt->SetMarkerSize(1.3);
    graph_mapmt->SetLineColor(kBlue);
    graph_mapmt->SetLineWidth(2);

    // ============================================================================
    // Build TGraphErrors for SiPM method (empty markers, same type as MAPMT)
    // ============================================================================

    double x_sipms[kNumRadiators];
    double y_sipms[kNumRadiators];
    double ex_sipms[kNumRadiators];
    double ey_sipms[kNumRadiators];

    for (int i = 0; i < kNumRadiators; ++i)
    {
        x_sipms[i] = x_position[i];
        y_sipms[i] = npe_sipms[i];
        ex_sipms[i] = 0.0;
        ey_sipms[i] = err_sipms[i];
    }

    TGraphErrors *graph_sipms = new TGraphErrors(kNumRadiators, x_sipms, y_sipms,
                                                 ex_sipms, ey_sipms);
    graph_sipms->SetName("graph_sipms");
    graph_sipms->SetTitle("SiPM Method");
    graph_sipms->SetMarkerColor(kRed);
    graph_sipms->SetMarkerStyle(24); // Open circle (empty)
    graph_sipms->SetMarkerSize(1.3);
    graph_sipms->SetLineColor(kRed);
    graph_sipms->SetLineWidth(2);

    // ============================================================================
    // Create canvas and frame histogram
    // ============================================================================

    TCanvas *canvas = new TCanvas("canvas_radiator_comparison",
                                  "NPE Comparison: MAPMT vs SiPMs", 800, 600);
    canvas->SetLeftMargin(0.12);
    canvas->SetRightMargin(0.05);
    canvas->SetBottomMargin(0.12);
    canvas->SetTopMargin(0.08);

    // Create TH1F frame with 3 bins
    TH1F *frame = new TH1F("frame", "", 3, 0.5, 3.5);
    frame->SetStats(0);
    frame->SetMinimum(10.0);
    frame->SetMaximum(40.0);

    // Configure axes
    TAxis *x_axis = frame->GetXaxis();
    TAxis *y_axis = frame->GetYaxis();

    x_axis->SetTitle("Radiator");
    x_axis->SetLabelSize(0.045);
    x_axis->SetTitleSize(0.05);
    x_axis->SetBinLabel(1, "Aerogel");
    x_axis->SetBinLabel(2, "Argon");
    x_axis->SetBinLabel(3, "C2F6");

    y_axis->SetTitle("NPE (number of photoelectrons)");
    y_axis->SetLabelSize(0.045);
    y_axis->SetTitleSize(0.05);

    // Draw frame only (axes and labels)
    frame->Draw("AXIS");

    // Draw graphs on top without redrawing axes
    graph_mapmt->Draw("P same");
    graph_sipms->Draw("P same");

    // Create legend with transparent fill and outline
    TLegend *legend = new TLegend(0.65, 0.55, 0.90, 0.30);
    legend->SetBorderSize(1);
    legend->SetFillColorAlpha(kWhite, 0.0);
    legend->SetLineColorAlpha(kBlack, 0.0);
    legend->SetTextSize(0.04);
    legend->AddEntry(graph_mapmt, "MAPMT", "p");
    legend->AddEntry(graph_sipms, "SiPMs", "p");
    legend->Draw();

    // ============================================================================
    // Print summary to console
    // ============================================================================

    std::cout << "\n=== Radiator NPE Comparison ===" << std::endl;
    std::cout << std::string(65, '-') << std::endl;
    std::cout << "Radiator\tMAPMT NPE\tSiPM NPE\tDifference" << std::endl;
    std::cout << std::string(65, '-') << std::endl;

    for (int i = 0; i < kNumRadiators; ++i)
    {
        double diff = npe_sipms[i] - npe_mapmt[i];
        std::cout << radiator_names[i] << "\t\t"
                  << npe_mapmt[i] << " ± " << err_mapmt[i] << "\t"
                  << npe_sipms[i] << " ± " << err_sipms[i] << "\t"
                  << diff << " NPE" << std::endl;
    }
    std::cout << std::string(65, '-') << std::endl;

    canvas->SaveAs("radiator_npe_comparison.png");
    canvas->SaveAs("radiator_npe_comparison.root");

    std::cout << "\nPlots saved to radiator_npe_comparison.png and .root" << std::endl;
}