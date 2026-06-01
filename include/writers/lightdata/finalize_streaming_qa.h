#pragma once

/**
 * @file finalize_streaming_qa.h
 * @brief Streaming-trigger subdirectory write block, lifted from the
 *        in-function finalize of `lightdata_writer()`.
 *
 * Why this lives in its own translation unit (and the other finalize
 * sub-blocks — Rollover / Triggers / Timing / DCR / Config — don't):
 *
 *  - The other sub-blocks are sequential `hist->Write()` lists.  Lifting
 *    them would add files without removing logic complexity.
 *  - The streaming-trigger block has real ownership complexity around
 *    the score-overlay canvas: heap-allocated TCanvas, cloned hists
 *    detached from gDirectory with `kCanDelete` set, heap-allocated
 *    TLegend.  All three live and die with the canvas — undoing the
 *    `delete c_streaming_score_overlay` at the bottom of the function
 *    silently frees the overlay clones and legend too.  That's worth
 *    isolating so the writer's main flow doesn't carry it inline.
 *  - It also nests three TDirectory subfolders (Hough rings, Hough
 *    rings (dual), Hough rings (solo)).  The pattern is grouped here.
 *
 * All histogram pointers in the context struct are non-owning — they
 * point at RootHist<>-managed instances that live for the lifetime of
 * `lightdata_writer()`.
 */

class TFile;
class TH1F;
class TH2F;

namespace btana::lightdata
{

/**
 * @brief Pointer-bundle of every hist this finalize block touches.
 *
 * Resolved by the caller from `RootHist<...>::get()`.  The three
 * `streaming_ring_*` fields at the bottom are the writer's trigger-
 * indexed map values looked up at `_TRIGGER_STREAMING_RING_FOUND_`;
 * any of those three may be nullptr if the streaming-ring trigger
 * had no entry in the corresponding map (no fires this run).
 */
struct StreamingTriggerFinalizeContext
{
    TFile *outfile;

    // ── Score hists (top-level "Streaming Trigger/")
    TH1F *h_streaming_score_noise;
    TH1F *h_streaming_score_data;
    TH1F *h_streaming_score_inbeam;  // in-beam (pre-trigger) sample

    // ── Top-level streaming-trigger hitmaps + ring-finder summaries
    TH2F *h_streaming_trigger_full_hitmap;
    TH2F *h_streaming_trigger_time_cut_hitmap;
    TH1F *h_streaming_trigger_ring_finder_nrings;
    TH2F *h_streaming_trigger_ring_finder_hitmap;
    TH2F *h_streaming_trigger_ring_finder_first_hitmap;
    TH2F *h_streaming_trigger_ring_finder_second_hitmap;

    // ── "Hough rings/" subfolder — Hough peak (cx, cy, R) + per-ring
    //     knob-calibration QA for the bright and second ring.
    TH1F *h_ring_X_first_hough;
    TH1F *h_ring_Y_first_hough;
    TH1F *h_ring_R_first_hough;
    TH1F *h_ring_X_second_hough;
    TH1F *h_ring_Y_second_hough;
    TH1F *h_ring_R_second_hough;
    TH2F *h_ring_peak_votes_vs_active_first;
    TH2F *h_ring_peak_votes_vs_active_second;
    TH1F *h_ring_hit_arc_dist_first;
    TH1F *h_ring_hit_arc_dist_second;

    // ── "Hough rings (dual)/" subfolder — first-ring observables
    //     gated on a second ring also being present in the frame.
    TH2F *h_ring_finder_first_hitmap_dual;
    TH1F *h_ring_X_first_hough_dual;
    TH1F *h_ring_Y_first_hough_dual;
    TH1F *h_ring_R_first_hough_dual;
    TH2F *h_ring_peak_votes_vs_active_first_dual;
    TH1F *h_ring_hit_arc_dist_first_dual;

    // ── "Hough rings (solo)/" subfolder — complement of (dual).
    TH2F *h_ring_finder_first_hitmap_solo;
    TH1F *h_ring_X_first_hough_solo;
    TH1F *h_ring_Y_first_hough_solo;
    TH1F *h_ring_R_first_hough_solo;
    TH2F *h_ring_peak_votes_vs_active_first_solo;
    TH1F *h_ring_hit_arc_dist_first_solo;

    // ── Streaming-ring trigger lookups (caller resolves
    //     `h_trigger_<kind>[registry.index_of(streaming_ring)]`).
    //     Any may be nullptr if the trigger had no entry.
    TH1F *streaming_ring_frame_population;
    TH1F *streaming_ring_time_diff_w_cherenkov;
    TH2F *streaming_ring_full_hitmap;
};

/**
 * @brief Write the entire "Streaming Trigger/" TDirectory tree.
 *
 * Creates the top-level subfolder, fills it with the score hists +
 * overlay canvas + ring-finder summaries, then creates the three
 * nested `Hough rings*` subfolders.  The output TDirectory cursor
 * is left at the streaming-trigger subfolder on return so any
 * subsequent writes still land in the right place (matches the
 * pre-extraction inline behaviour).
 */
void finalize_streaming_qa(const StreamingTriggerFinalizeContext &ctx);

} // namespace btana::lightdata
