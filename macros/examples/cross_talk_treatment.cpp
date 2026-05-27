#include "../lib_loader.h"
#include "util/root_io.h"
#include "util/root_hist.h"

/**
 * @file cross_talk_treatment.cpp
 * @brief Cross-talk probability measurement with DCR baseline subtraction.
 *
 * @details
 * Builds the distribution of Δt = t_j − t_i for all pairs of hits (i primary,
 * j on a neighbouring channel) in triggered frames.  The distribution shows:
 *
 *   - A prompt peak at small Δt  → true CT signal + accidental DCR background
 *   - A flat plateau at large Δt → pure accidental DCR (the baseline)
 *
 * The corrected CT probability is extracted by sideband subtraction:
 *
 *   N_ct_true  = N_signal − N_sideband × (T_signal / T_sideband)
 *   P_ct_true  = N_ct_true / N_primary
 *
 * where signal and sideband windows have the same width (T_signal = T_sideband)
 * so the scale factor is 1 and no extra uncertainty is introduced.
 *
 * Two neighbour definitions are tested independently:
 *   - Physical CT: distance ≤ 3.2 mm (nearest-pixel optical/charge coupling)
 *   - Electrical CT: same device AND same FIFO (shared readout chain)
 *
 * @author Nicola Rubini
 */

void cross_talk_treatment(std::string data_repository, std::string run_name, int max_frames = 10000000)
{
    //  ── Constants ────────────────────────────────────────────────────────────
    //  Physical CT signal window: [0, 10] cc (causal, optical/charge coupling).
    //  Electrical CT signal window: [-2, 10] cc — small negative Δt allowed for
    //  readout-timing jitter along the shared electrical chain.
    const float kPhysCtLoNs = 0.f;
    const float kPhysCtHiNs = 10 * BTANA_ALCOR_CC_TO_NS; //  31.25 ns
    const float kElecCtLoNs = -2 * BTANA_ALCOR_CC_TO_NS; //  -6.25 ns
    const float kElecCtHiNs = kPhysCtHiNs;
    const float kPhysCtWinNs = kPhysCtHiNs - kPhysCtLoNs;
    const float kElecCtWinNs = kElecCtHiNs - kElecCtLoNs;
    const float kPhysRadiusMm = 3.2f;

    //  Sideband: same width as the respective signal window, well beyond any CT timescale.
    //  At 512 cc (= 1600 ns) we are deep in the flat DCR plateau.
    const float kSidebandLoNs = 512 * BTANA_ALCOR_CC_TO_NS; // 1600 ns
    const float kPhysSidebandHiNs = kSidebandLoNs + kPhysCtWinNs;
    const float kElecSidebandHiNs = kSidebandLoNs + kElecCtWinNs;

    //  ── Input ────────────────────────────────────────────────────────────────
    std::string input_filename = data_repository + "/" + run_name + "/recodata.root";
    TFilePtr input_file(TFile::Open(input_filename.c_str(), "READ"));
    if (!input_file || input_file->IsZombie())
    {
        std::cerr << "[ERROR] Cannot open recodata: " << input_filename << std::endl;
        return;
    }
    TTree *recodata_tree = (TTree *)input_file->Get("recodata");
    AlcorRecodata *recodata = new AlcorRecodata();
    recodata->link_to_tree(recodata_tree);

    const int n_frames = (int)recodata_tree->GetEntries();
    const int all_frames = std::min(n_frames, max_frames);

    //  ── Histograms ───────────────────────────────────────────────────────────
    //  Δt distributions — one bin per cc (3.125 ns), covering a full frame
    const int kDtBins = 1024;
    const float kDtMax = kDtBins * BTANA_ALCOR_CC_TO_NS; //  3200 ns
    const float kElecDtLo = -5 * BTANA_ALCOR_CC_TO_NS;   // -15.625 ns (headroom around -2 cc)

    RootHist<TH1F> h_phys_dt("h_phys_dt",
                             ";#Delta_{t} = t_{j} - t_{i} (ns);physical neighbour pairs / primary Hit",
                             kDtBins, 0, kDtMax);
    RootHist<TH1F> h_elec_dt("h_elec_dt",
                             ";#Delta_{t} = t_{j} - t_{i} (ns);electrical neighbour pairs / primary Hit",
                             kDtBins + 5, kElecDtLo, kDtMax);
    //  Smeared spatial hitmaps for CT-tagged hits (100 fills per Hit, ±1.5 mm uniform smear).
    RootHist<TH2F> h_phys_ct_hitmap("h_phys_ct_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    RootHist<TH2F> h_elec_ct_hitmap("h_elec_ct_hitmap", ";x (mm);y (mm)", 396, -99, 99, 396, -99, 99);
    //  2D diagnostic: (Δchannel, Δt) for every in-frame pair — no neighbour
    //  definition needed; CT clusters near the origin, DCR is flat in Δt.
    RootHist<TH2F> h_dchannel_dt("h_dchannel_dt",
                                 ";#Delta channel (j #minus i);#Delta_{t} (cc)",
                                 65, -32.5, 32.5,
                                 26, -5.5, 20.5);

    //  Per-channel corrected CT probability (filled once per primary Hit)
    RootHist<TProfile> h_phys_ct_corr("h_phys_ct_corrected_per_channel",
                                      ";global channel;Corrected physical CT probability (%)", 2048, 0, 2048);
    RootHist<TProfile> h_elec_ct_corr("h_elec_ct_corrected_per_channel",
                                      ";global channel;Corrected electrical CT probability (%)", 2048, 0, 2048);

    //  ── Loop ─────────────────────────────────────────────────────────────────
    long long n_primary = 0;

    for (int i_frame = 0; i_frame < all_frames; ++i_frame)
    {
        recodata_tree->GetEntry(i_frame);
        if (recodata->is_start_of_spill())
            continue;

        auto trigger = recodata->get_trigger_by_index(0);
        if (!trigger)
            continue;

        const int nhits = (int)recodata->get_recodata().size();

        for (int i = 0; i < nhits; ++i)
        {
            if (recodata->is_afterpulse(i))
                continue;

            const float ti = recodata->get_hit_t(i);
            const float xi = recodata->get_hit_x(i);
            const float yi = recodata->get_hit_y(i);
            const int dev_i = recodata->get_device(i);
            const int fifo_i = recodata->get_fifo(i);
            // Phase 5: replace the lazy `legacy_raw / 4` strip-TDC pattern
            // with the explicit dense channel-ordinal accessor.  Bit-exact
            // with the legacy value for the current detector.
            const int chan_i = recodata->get_global_channel_index(i);
            ++n_primary;

            //  Per-primary-Hit counts in signal and sideband windows
            int n_phys_near = 0, n_phys_far = 0;
            int n_elec_near = 0, n_elec_far = 0;

            for (int j = 0; j < nhits; ++j)
            {
                if (j == i)
                    continue;
                if (recodata->is_afterpulse(j))
                    continue;

                const float dt = recodata->get_hit_t(j) - ti;
                if (dt < kElecDtLo || dt >= kDtMax)
                    continue;

                const float xj = recodata->get_hit_x(j);
                const float yj = recodata->get_hit_y(j);

                //  Physical CT: causal only (dt >= 0)
                const bool is_phys = dt >= 0.f &&
                                     xi > -990.f && xj > -990.f &&
                                     std::hypot(xj - xi, yj - yi) <= kPhysRadiusMm;
                const bool is_elec = recodata->get_device(j) == dev_i &&
                                     recodata->get_fifo(j) == fifo_i;

                if (is_phys)
                    h_phys_dt->Fill(dt);
                if (is_elec)
                    h_elec_dt->Fill(dt);
                const int dchan = recodata->get_global_channel_index(j) -
                                  recodata->get_global_channel_index(i);
                h_dchannel_dt->Fill(dchan, dt / BTANA_ALCOR_CC_TO_NS);

                //  Accumulate per-Hit signal and sideband counts
                if (dt >= kPhysCtLoNs && dt < kPhysCtHiNs)
                    if (is_phys)
                        ++n_phys_near;
                if (dt >= kElecCtLoNs && dt < kElecCtHiNs)
                    if (is_elec)
                        ++n_elec_near;
                if (dt >= kSidebandLoNs && dt < kPhysSidebandHiNs)
                    if (is_phys)
                        ++n_phys_far;
                if (dt >= kSidebandLoNs && dt < kElecSidebandHiNs)
                    if (is_elec)
                        ++n_elec_far;
            }

            //  Smeared hitmaps: weight = n_ct_neighbours × 100 per primary Hit.
            //  Statistical content matches the previous N×100 unit-Fill loop
            //  in expectation but at ~100× lower per-Hit cost (CODE_REVIEW §6.4).
            if (xi > -990.f)
            {
                if (n_phys_near > 0)
                    h_phys_ct_hitmap->Fill(recodata->get_hit_x_rnd(i), recodata->get_hit_y_rnd(i),
                                           100.0 * n_phys_near);
                if (n_elec_near > 0)
                    h_elec_ct_hitmap->Fill(recodata->get_hit_x_rnd(i), recodata->get_hit_y_rnd(i),
                                           100.0 * n_elec_near);
            }

            //  Fill corrected CT probability per channel.
            //  (n_near - n_far) is an unbiased per-event estimator of the true CT
            //  neighbour count; the TProfile averages it over all primary hits on
            //  this channel, giving the corrected probability directly.
            h_phys_ct_corr->Fill(chan_i, (n_phys_near - n_phys_far) > 0 ? 100. : 0.);
            h_elec_ct_corr->Fill(chan_i, (n_elec_near - n_elec_far) > 0 ? 100. : 0.);
        }
    }

    //  ── Global corrected CT probability ──────────────────────────────────────
    auto integral_ns = [](TH1F *h, float lo, float hi)
    {
        return h->Integral(h->GetXaxis()->FindBin(lo), h->GetXaxis()->FindBin(hi) - 1);
    };

    const double phys_n_sig = integral_ns(h_phys_dt, kPhysCtLoNs, kPhysCtHiNs);
    const double phys_n_sb = integral_ns(h_phys_dt, kSidebandLoNs, kPhysSidebandHiNs);
    const double elec_n_sig = integral_ns(h_elec_dt, kElecCtLoNs, kElecCtHiNs);
    const double elec_n_sb = integral_ns(h_elec_dt, kSidebandLoNs, kElecSidebandHiNs);

    //  Windows have equal width per type → scale factor = 1
    const double phys_ct_raw = (n_primary > 0) ? phys_n_sig / n_primary : 0.;
    const double phys_ct_corr = (n_primary > 0) ? (phys_n_sig - phys_n_sb) / n_primary : 0.;
    const double elec_ct_raw = (n_primary > 0) ? elec_n_sig / n_primary : 0.;
    const double elec_ct_corr = (n_primary > 0) ? (elec_n_sig - elec_n_sb) / n_primary : 0.;
    const double phys_dcr_acc = (n_primary > 0) ? phys_n_sb / n_primary : 0.;
    const double elec_dcr_acc = (n_primary > 0) ? elec_n_sb / n_primary : 0.;

    printf("\n=== Cross-talk summary ===\n");
    printf("  Physical CT  signal  [%.0f, %.0f] ns,  sideband [%.0f, %.0f] ns\n",
           kPhysCtLoNs, kPhysCtHiNs, kSidebandLoNs, kPhysSidebandHiNs);
    printf("  Electrical CT signal [%.1f, %.0f] ns,  sideband [%.0f, %.0f] ns\n",
           kElecCtLoNs, kElecCtHiNs, kSidebandLoNs, kElecSidebandHiNs);
    printf("  Primary hits analysed : %lld\n", n_primary);
    printf("  Physical CT  — raw: %.3f%%  DCR acc: %.3f%%  corrected: %.3f%%\n",
           phys_ct_raw * 100., phys_dcr_acc * 100., phys_ct_corr * 100.);
    printf("  Electrical CT — raw: %.3f%%  DCR acc: %.3f%%  corrected: %.3f%%\n\n",
           elec_ct_raw * 100., elec_dcr_acc * 100., elec_ct_corr * 100.);

    //  Normalise Δt distributions to pairs per primary Hit
    if (n_primary > 0)
    {
        h_phys_dt->Scale(1. / n_primary);
        h_elec_dt->Scale(1. / n_primary);
    }

    //  ── Plot ─────────────────────────────────────────────────────────────────
    auto draw_window = [](float lo, float hi, int color, const char *label)
    {
        TLine *l1 = new TLine(lo, gPad->GetUymin(), lo, gPad->GetUymax());
        TLine *l2 = new TLine(hi, gPad->GetUymin(), hi, gPad->GetUymax());
        l1->SetLineColor(color);
        l1->SetLineStyle(2);
        l1->Draw();
        l2->SetLineColor(color);
        l2->SetLineStyle(2);
        l2->Draw();
        TLatex *tex = new TLatex(lo, gPad->GetUymax() * 0.9, label);
        tex->SetTextColor(color);
        tex->SetTextSize(0.03);
        tex->Draw();
    };

    TCanvas *c = new TCanvas("c_ct", "Cross-talk Δt distributions", 1800, 500);
    c->Divide(3, 1);

    c->cd(1);
    gPad->SetLogy();
    h_phys_dt->SetLineColor(kBlue + 1);
    h_phys_dt->SetLineWidth(2);
    h_phys_dt->Draw("HIST");
    draw_window(kPhysCtLoNs, kPhysCtHiNs, kRed, "signal");
    draw_window(kSidebandLoNs, kPhysSidebandHiNs, kGreen + 2, "sideband");

    c->cd(2);
    gPad->SetLogy();
    h_elec_dt->SetLineColor(kOrange + 7);
    h_elec_dt->SetLineWidth(2);
    h_elec_dt->Draw("HIST");
    draw_window(kElecCtLoNs, kElecCtHiNs, kRed, "signal");
    draw_window(kSidebandLoNs, kElecSidebandHiNs, kGreen + 2, "sideband");

    c->cd(3);
    h_dchannel_dt->SetStats(0);
    h_dchannel_dt->Draw("COLZ");
}
