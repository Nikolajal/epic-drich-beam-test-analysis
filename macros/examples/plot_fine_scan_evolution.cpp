/**
 * @file plot_dt_vs_spill.cpp
 * @brief δt vs spill evolution for selected Cherenkov channels.
 *
 * @details
 * For each requested channel, reads the per-spill TH3F histograms
 * (X=GlobalIndex, Y=δt, Z=fine) and collapses both the global-index
 * and fine axes, filling a single TH2D(spill, δt) that shows how the
 * timing residual distribution evolves across spills.
 *
 * Usage (ROOT prompt):
 *   .x plot_dt_vs_spill.cpp("path/to/timing_fine_calib.root")
 *   .x plot_dt_vs_spill.cpp("path/to/timing_fine_calib.root", {1234, 5678})
 *   .x plot_dt_vs_spill.cpp("path/to/timing_fine_calib.root", {1234}, 0, 9)
 *
 * @param filename   Path to timing_fine_calib.root
 * @param channels   List of GlobalIndex values to inspect (default: {0})
 * @param spill_min  First spill to include (default: 0)
 * @param spill_max  Last spill inclusive (-1 = all spills found in file)
 */

#include "../lib_loader.h"
#include "util/root_io.h"
#include "util/root_hist.h"
#include <TFile.h>
#include <TH1D.h>
#include <TH2D.h>
#include <TH3F.h>
#include <TCanvas.h>
#include <TStyle.h>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>

void plot_fine_scan_evolution(
    const std::string &filename,
    std::vector<int> channels = {0},
    int spill_min             = 0,
    int spill_max             = -1)
{
    // -------------------------------------------------------------------------
    // Open file
    // -------------------------------------------------------------------------
    TFilePtr f(TFile::Open(filename.c_str(), "READ"));
    if (!f || f->IsZombie())
    {
        std::cerr << "[ERROR] Cannot open " << filename << "\n";
        return;
    }

    // -------------------------------------------------------------------------
    // Discover available spills
    // -------------------------------------------------------------------------
    int n_spills_in_file = 0;
    while (f->GetKey(TString::Format("h_fine_scan_cherenkov_spill%03d", n_spills_in_file).Data()))
        ++n_spills_in_file;

    if (n_spills_in_file == 0)
    {
        std::cerr << "[ERROR] No per-spill histograms found in " << filename << "\n";
        return;
    }

    if (spill_min < 0) spill_min = 0;
    if (spill_max < 0 || spill_max >= n_spills_in_file)
        spill_max = n_spills_in_file - 1;

    const int n_spills = spill_max - spill_min + 1;

    std::cout << "[INFO] File contains " << n_spills_in_file << " spill(s). "
              << "Showing spills " << spill_min << " – " << spill_max << ".\n";

    // -------------------------------------------------------------------------
    // Retrieve δt axis binning from the first available histogram so the
    // output TH2D exactly matches what was stored (no hardcoded bin counts).
    // -------------------------------------------------------------------------
    TH3F *h3_proto = (TH3F *)f->Get(
        TString::Format("h_fine_scan_cherenkov_spill%03d", spill_min).Data());
    if (!h3_proto)
    {
        std::cerr << "[ERROR] Cannot read prototype histogram.\n";
        return;
    }
    const int   n_dt_bins = h3_proto->GetNbinsY();
    const double dt_lo    = h3_proto->GetYaxis()->GetXmin();
    const double dt_hi    = h3_proto->GetYaxis()->GetXmax();

    // -------------------------------------------------------------------------
    // Style
    // -------------------------------------------------------------------------
    gStyle->SetOptStat(0);
    gStyle->SetOptTitle(1);
    gStyle->SetPalette(kBird);
    gStyle->SetNumberContours(64);

    // -------------------------------------------------------------------------
    // One canvas per channel, each showing a single TH2D(spill, δt)
    // -------------------------------------------------------------------------
    for (int ch : channels)
    {
        RootHist<TH2D> h_evolution(
            TString::Format("h_dt_vs_spill_ch%d", ch).Data(),
            TString::Format("Channel %d;Spill;#Delta t (ns)", ch).Data(),
            n_spills, spill_min - 0.5, spill_max + 0.5,
            n_dt_bins, dt_lo, dt_hi);
        h_evolution->SetDirectory(nullptr);

        for (int s = spill_min; s <= spill_max; ++s)
        {
            TH3F *h3 = (TH3F *)f->Get(
                TString::Format("h_fine_scan_cherenkov_spill%03d", s).Data());
            if (!h3)
            {
                std::cerr << "[WARN] Spill " << s << " missing, skipping.\n";
                continue;
            }

            // Select the single X bin for this channel.
            // ProjectionY ignores SetRange — bin limits must be passed explicitly.
            // Signature: ProjectionY(name, xfirst, xlast, zfirst, zlast)
            // zfirst=0, zlast=-1 means "all Z (fine) bins".
            const int bx = h3->GetXaxis()->FindBin(static_cast<double>(ch));

            TH1D *h_dt = h3->ProjectionY(
                TString::Format("hpy_ch%d_s%03d", ch, s).Data(),
                bx, bx,   // single channel bin
                0, -1);   // all fine bins
            h_dt->SetDirectory(nullptr);

            // Fill the evolution histogram column by column
            const int spill_bin = s - spill_min + 1;
            for (int by = 1; by <= h_dt->GetNbinsX(); ++by)
                h_evolution->SetBinContent(
                    spill_bin, by,
                    h_evolution->GetBinContent(spill_bin, by) +
                    h_dt->GetBinContent(by));

            delete h_dt;
        }

        TCanvas *c = new TCanvas(
            TString::Format("c_dtspill_ch%d", ch).Data(),
            TString::Format("#Delta t vs spill — channel %d", ch).Data(),
            900, 500);
        c->SetLeftMargin(0.10);
        c->SetBottomMargin(0.12);
        c->SetRightMargin(0.12);

        h_evolution->Draw("COLZ");
        c->Update();
    }
}