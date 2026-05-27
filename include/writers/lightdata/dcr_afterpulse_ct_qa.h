#pragma once

/**
 * @file dcr_afterpulse_ct_qa.h
 * @brief Per-frame Dark-Count-Rate / afterpulse / cross-talk QA fills.
 *
 * Extracted from the per-frame inner loop of `lightdata_writer()` —
 * the lightdata analog of `compute_ring_fit` in the recodata split.
 * Runs on the Cherenkov hits of a single (first-frames-tagged) frame,
 * fills:
 *
 *   - DCR per-channel TProfile + 2D smeared hitmap
 *   - Per-channel afterpulse near/far/subtracted TProfiles + 2D maps
 *   - Per-channel physical-CT and electrical-CT TProfiles + 2D maps
 *   - Δt distributions (1D + 2D-with-Δchannel) per CT class
 *
 * Caller responsibilities:
 *   - Gate the call on the first-frames trigger being present in the
 *     frame (the function itself does NOT gate — keeps the function
 *     pure compute-on-input).
 *   - Hoist `ct_hits` and `sorted_by_time` outside the per-frame loop
 *     (capacity stabilisation across frames).
 *   - Pass `active_sensors_count` zeroed for every active channel
 *     (the function fills counts; caller does not need to reset).
 *   - Pre-build `active_sensors` once per spill.
 */

#include <cstdint>
#include <set>
#include <unordered_map>
#include <vector>

#include "alcor_finedata.h"          // AlcorFinedataStruct
#include "utility/config_reader.h"      // QaConfigStruct
#include "writers/lightdata/types.h" // CtHit

class TH1F;
class TH2F;
class TProfile;

namespace btana::lightdata
{

/**
 * @brief Pointer-bundle of all 17 histograms touched by the DCR /
 *        afterpulse / cross-talk per-frame QA path.
 *
 * Any pointer may be nullptr to skip the corresponding fill, but in
 * normal production all 17 are wired.  Same convention as
 * `RingFillHists` in the recodata helpers.
 */
struct DcrAfterpulseCtHists
{
    // ── DCR
    TProfile *h_dcr_per_channel = nullptr;
    TH2F *h_dcr_hitmap = nullptr;

    // ── Afterpulse per-channel probability profiles
    TProfile *h_afterpulse_near_per_channel = nullptr;
    TProfile *h_afterpulse_far_per_channel = nullptr;
    TProfile *h_afterpulse_per_channel = nullptr; ///< DCR-subtracted

    // ── Afterpulse 2D smeared hitmaps
    TH2F *h_afterpulse_near_hitmap = nullptr;
    TH2F *h_afterpulse_far_hitmap = nullptr;
    TH2F *h_afterpulse_hitmap = nullptr; ///< DCR-subtracted

    // ── Cross-talk per-channel probability profiles
    TProfile *h_phys_ct_per_channel = nullptr;
    TProfile *h_elec_ct_per_channel = nullptr;

    // ── Cross-talk 2D smeared hitmaps
    TH2F *h_phys_ct_hitmap = nullptr;
    TH2F *h_elec_ct_hitmap = nullptr;

    // ── CT Δt distributions (1D)
    TH1F *h_phys_ct_dt = nullptr;
    TH1F *h_elec_ct_dt = nullptr;

    // ── CT (Δchannel, Δt) 2D diagnostics
    TH2F *h_elec_ct_dchannel_dt = nullptr;
    TH2F *h_phys_ct_dchannel_dt = nullptr;
};

/**
 * @brief Per-frame DCR + afterpulse + cross-talk QA fill.
 *
 * @param cherenkov_hits          Per-frame Cherenkov hit vector (read).
 * @param active_sensors          Set of channel_ordinals active this spill (read).
 * @param active_sensors_count    Per-channel hit-counter (cleared + filled here).
 * @param ct_hits                 Hoisted scratch buffer for per-Hit CT records.
 * @param sorted_by_time          Hoisted scratch buffer for the time-sorted index.
 * @param qa_cfg                  QA timing-window config (read).
 * @param hists                   Pointer-bundle of output histograms.
 */
void fill_dcr_afterpulse_ct_qa(
    const std::vector<AlcorFinedataStruct> &cherenkov_hits,
    const std::set<uint32_t> &active_sensors,
    std::unordered_map<uint32_t, uint16_t> &active_sensors_count,
    std::vector<CtHit> &ct_hits,
    std::vector<std::size_t> &sorted_by_time,
    const QaConfigStruct &qa_cfg,
    const DcrAfterpulseCtHists &hists);

} // namespace btana::lightdata
