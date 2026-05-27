#pragma once

/**
 * @file types.h
 * @brief Shared types for the lightdata writer's split translation units.
 *
 * Lifted from function-scope structs inside `lightdata_writer()` so the
 * per-frame QA-fill helpers (DCR / afterpulse / cross-talk) can be
 * implemented in their own .cxx and re-use the same compact CT-hit
 * scratch type.  Same purpose as `include/writers/recodata/types.h`.
 */

#include <cstdint>

namespace btana::lightdata
{

/**
 * @brief Compact per-Hit record used by the afterpulse + cross-talk
 *        inner loop.
 *
 * Pre-decoded into this layout once per primary Hit so the O(N²) →
 * O(N log N + N · k_win) sorted-pair scan in
 * `fill_dcr_afterpulse_ct_qa` doesn't re-derive the same fields per
 * comparison.  The buffer holding these is hoisted out of the
 * per-frame loop and `clear()`d / `resize()`d each frame — typical
 * capacity stabilises within a spill, eliminating realloc churn on
 * the hot path (CODE_REVIEW §4.8).
 */
struct CtHit
{
    uint64_t global_t; ///< rollover · 32768 + coarse (continuous timeline)
    uint32_t channel;  ///< GlobalIndex::channel_ordinal() (dense small int)
    int device;        ///< 192 + channel/256
    int fifo;          ///< block of 8 channels within device
    float x;           ///< physical X position [mm] (-999 if unmapped)
    float y;           ///< physical Y position [mm] (-999 if unmapped)
};

} // namespace btana::lightdata
