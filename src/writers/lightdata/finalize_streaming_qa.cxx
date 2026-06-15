/**
 * @file finalize_streaming_qa.cxx
 * @brief Implementation of the streaming-trigger finalize-block,
 *        lifted from `lightdata_writer()`'s in-function QA writes.
 *
 * Lifted verbatim (with the captured hist pointers + outfile replaced
 * by a single context struct).  Algorithm is unchanged; bit-identical
 * output was verified vs a baseline snapshot at extraction time
 * (since pruned).
 */

#include "writers/lightdata/finalize_streaming_qa.h"

#include "TCanvas.h"
#include "TDirectory.h"
#include "TFile.h"
#include "TH1.h"
#include "TH2.h"
#include "TLegend.h"

namespace btana::lightdata
{

void finalize_streaming_qa(const StreamingTriggerFinalizeContext &ctx)
{
    TDirectory *streaming_trigger_dir = ctx.outfile->mkdir("Streaming Trigger");
    streaming_trigger_dir->cd();

    //  QA score histograms — drive the threshold-tuning workflow
    //  described in include/triggers/DISCUSSION.md § 2.4.  Same n_σ
    //  axis on both so the misfire and acceptance integrals at any
    //  threshold are directly comparable.  Normalised by entry count
    //  so y-axis is **probability per bin** rather than raw counts —
    //  makes noise vs data integrals above a threshold directly
    //  readable as false-positive rate vs signal acceptance.
    if (ctx.h_streaming_score_noise->GetEntries() > 0)
        ctx.h_streaming_score_noise->Scale(1.0 / ctx.h_streaming_score_noise->GetEntries());
    if (ctx.h_streaming_score_data->GetEntries() > 0)
        ctx.h_streaming_score_data->Scale(1.0 / ctx.h_streaming_score_data->GetEntries());
    //  In-beam (pre-trigger) sample — same per-entry normalisation so
    //  all three curves share the probability-per-bin axis and their
    //  above-threshold integrals are directly comparable.  Guarded for
    //  null in case an older caller doesn't populate it.
    if (ctx.h_streaming_score_inbeam &&
        ctx.h_streaming_score_inbeam->GetEntries() > 0)
        ctx.h_streaming_score_inbeam->Scale(
            1.0 / ctx.h_streaming_score_inbeam->GetEntries());
    ctx.h_streaming_score_noise->GetYaxis()->SetTitle("probability per bin");
    ctx.h_streaming_score_data->GetYaxis()->SetTitle("probability per bin");
    ctx.h_streaming_score_noise->Write();
    ctx.h_streaming_score_data->Write();
    if (ctx.h_streaming_score_inbeam)
    {
        ctx.h_streaming_score_inbeam->GetYaxis()->SetTitle("probability per bin");
        ctx.h_streaming_score_inbeam->Write();
    }

    //  Pre-made overlay canvas for visual threshold tuning.
    //  Noise/DCR (first-frames) in blue, signal/data-taking in red, log-Y so the
    //  tails separating signal from noise are visible across many
    //  decades.  The canvas is built self-contained: clones of the
    //  hists (detached from the output TDirectory and marked
    //  kCanDelete) plus a heap-allocated legend.  Without this, the
    //  reopened canvas references freed memory (stack-scoped legend,
    //  hists owned by RootHist) and segfaults inside TCanvas::Build
    //  when the browser tries to redraw it.
    {
        TCanvas *c_streaming_score_overlay = new TCanvas(
            "c_streaming_score_overlay",
            "Streaming-trigger score: noise (blue) vs data (red)",
            1600, 800);
        c_streaming_score_overlay->cd();
        c_streaming_score_overlay->SetLogy();
        c_streaming_score_overlay->SetGridx();
        c_streaming_score_overlay->SetGridy();

        //  Clone hists into the canvas: detached from any directory,
        //  with kCanDelete set so the canvas destructor frees them.
        //  Unique names to avoid collisions with the standalone hists
        //  written elsewhere in the same TDirectory.
        TH1F *h_data_overlay = static_cast<TH1F *>(
            ctx.h_streaming_score_data->Clone("h_streaming_score_data_overlay"));
        h_data_overlay->SetDirectory(nullptr);
        h_data_overlay->SetBit(TObject::kCanDelete);
        h_data_overlay->SetTitle(
            "Streaming-trigger score;n_{#sigma};probability per bin");
        h_data_overlay->Draw("HIST");

        TH1F *h_noise_overlay = static_cast<TH1F *>(
            ctx.h_streaming_score_noise->Clone("h_streaming_score_noise_overlay"));
        h_noise_overlay->SetDirectory(nullptr);
        h_noise_overlay->SetBit(TObject::kCanDelete);
        h_noise_overlay->Draw("HIST SAME");

        TLegend *leg = new TLegend(0.65, 0.75, 0.88, 0.88);
        leg->SetBorderSize(0);
        leg->SetFillStyle(0);
        leg->AddEntry(h_data_overlay, "Data-taking (signal + noise)", "l");
        leg->AddEntry(h_noise_overlay, "First-frames (noise only)", "l");
        leg->SetBit(TObject::kCanDelete);
        leg->Draw();

        c_streaming_score_overlay->Modified();
        c_streaming_score_overlay->Update();
        c_streaming_score_overlay->Write();
        delete c_streaming_score_overlay; // also deletes overlay clones + legend
    }

    //  Top-level streaming-trigger hitmaps + ring-finder summaries.
    ctx.h_streaming_trigger_full_hitmap->Write();
    ctx.h_streaming_trigger_time_cut_hitmap->Write();
    ctx.h_streaming_trigger_ring_finder_nrings->Write();
    ctx.h_streaming_trigger_ring_finder_hitmap->Write();
    ctx.h_streaming_trigger_ring_finder_first_hitmap->Write();
    ctx.h_streaming_trigger_ring_finder_second_hitmap->Write();

    //  Per-ring centre + radius distributions, split into three
    //  subfolders so the upstream → downstream chain (RANSAC seed →
    //  fit_circle refine) is visually grouped in TBrowser.  Order
    //  matters: the RANSAC seed is what the fit climbs from, so any
    //  bad tails in `Fit rings/` should first be checked against the
    //  same hist in `RANSAC rings/` — if the seed is already wrong,
    //  the fit was given a bad starting point (relevant open items
    //  in DISCUSSION.md § 2.5).
    {
        TDirectory *hough_rings_dir = streaming_trigger_dir->mkdir("RANSAC rings");
        hough_rings_dir->cd();
        ctx.h_ring_X_first_ransac->Write();
        ctx.h_ring_Y_first_ransac->Write();
        ctx.h_ring_R_first_ransac->Write();
        ctx.h_ring_X_second_ransac->Write();
        ctx.h_ring_Y_second_ransac->Write();
        ctx.h_ring_R_second_ransac->Write();
        //  Knob-calibration QA (see § 2.5 in the streaming DISCUSSION).
        ctx.h_ring_peak_votes_vs_active_first->Write();
        ctx.h_ring_peak_votes_vs_active_second->Write();
        ctx.h_ring_hit_arc_dist_first->Write();
        ctx.h_ring_hit_arc_dist_second->Write();

        //  ("Fit rings/" subfolder removed — fit_circle work
        //   moved entirely to recodata.  See recodata.root's `Rings/`
        //   subfolder for all fit-derived QA.)

        //  Dual-ring mirror — same hists as `RANSAC rings/` above but
        //  for the first ring, gated on a second ring also being
        //  present in the same frame.  Lets you compare ring-1
        //  properties in the full sample vs the cleaner 2-ring subset.
        TDirectory *hough_rings_dual_dir = streaming_trigger_dir->mkdir("RANSAC rings (dual)");
        hough_rings_dual_dir->cd();
        ctx.h_ring_finder_first_hitmap_dual->Write();
        ctx.h_ring_X_first_ransac_dual->Write();
        ctx.h_ring_Y_first_ransac_dual->Write();
        ctx.h_ring_R_first_ransac_dual->Write();
        ctx.h_ring_peak_votes_vs_active_first_dual->Write();
        ctx.h_ring_hit_arc_dist_first_dual->Write();

        //  ("Fit rings (dual)/" subfolder removed.)

        //  Solo-ring mirror — complement of (dual).  Together they
        //  partition the full first-ring sample, so any systematic
        //  difference between (solo) and (dual) flags single-ring
        //  contamination that the dual requirement filters out.
        TDirectory *hough_rings_solo_dir = streaming_trigger_dir->mkdir("RANSAC rings (solo)");
        hough_rings_solo_dir->cd();
        ctx.h_ring_finder_first_hitmap_solo->Write();
        ctx.h_ring_X_first_ransac_solo->Write();
        ctx.h_ring_Y_first_ransac_solo->Write();
        ctx.h_ring_R_first_ransac_solo->Write();
        ctx.h_ring_peak_votes_vs_active_first_solo->Write();
        ctx.h_ring_hit_arc_dist_first_solo->Write();

        //  ("Fit rings (solo)/" subfolder removed.)

        streaming_trigger_dir->cd(); // restore parent for any later writes
    }

    //  Streaming-ring trigger hists — caller resolved the lookup via
    //  registry.index_of(streaming_ring_index).  Any may be nullptr if
    //  the streaming-ring trigger had no entry in the respective map.
    if (ctx.streaming_ring_frame_population)
        ctx.streaming_ring_frame_population->Write();
    if (ctx.streaming_ring_time_diff_w_cherenkov)
        ctx.streaming_ring_time_diff_w_cherenkov->Write();
    if (ctx.streaming_ring_full_hitmap)
        ctx.streaming_ring_full_hitmap->Write();
}

} // namespace btana::lightdata
