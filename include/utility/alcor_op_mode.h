#pragma once

/**
 * @file alcor_op_mode.h
 * @brief Typed ALCOR pixel operation mode (PCR3 bits [12:9], datasheet Table 15).
 *
 * The enumerator values ARE the PCR @c OpMode codes, so they map 1:1 onto the
 * run-database @c op_mode field (@ref RunInfoStruct::op_mode). Legacy runs use
 * @ref AlcorOpMode::LET (=1, leading-edge), which is the behaviour all pre-ToT
 * code assumes.
 *
 * Only the *interpretation* of the decoded coarse/fine timestamps changes with
 * mode — the 32-bit word format and the fine-phase calibration are mode
 * independent. Modes are grouped into an @ref AlcorOpModeFamily that ignores the
 * test-pulse (`_TP_TDC` / `_TP_FE`) and OFF variants; the analysis branches on the
 * family, not the raw code.
 *
 *  - **LET**  : each TDC timestamps an independent leading edge (4 channels/pixel).
 *  - **ToT**  : TDC pairs {0,1},{2,3}; even=primary, odd=secondary; the per-hit
 *               *duration* (= t_secondary − t_primary) is the time-over-threshold.
 *  - **SR**   : slew-rate. **Identical pairing to ToT** ({0,1},{2,3}); the same
 *               *duration* Δt is interpreted as the rise time between two
 *               thresholds (slew-rate), not time-over-threshold.
 *  - **ToT2** : second ToT variant; same pairing, ToT-style interpretation.
 */

#include <cstdint>

/** @brief Pixel operation mode; values equal the PCR @c OpMode codes (Table 15). */
enum class AlcorOpMode : int
{
    Off0        = 0b0000, ///< 0  — control logic OFF, FE disabled
    LET         = 0b0001, ///< 1  — Leading-Edge, standard (legacy default)
    LET_TP_TDC  = 0b0010, ///< 2  — LET, test-pulse to control logic
    LET_TP_FE   = 0b0011, ///< 3  — LET, test-pulse to front-end
    ToT         = 0b0100, ///< 4  — Time-over-Threshold, standard
    ToT_TP_TDC  = 0b0101, ///< 5  — ToT, test-pulse to control logic
    ToT_TP_FE   = 0b0110, ///< 6  — ToT, test-pulse to front-end
    Off7        = 0b0111, ///< 7  — control logic OFF, FE enabled
    Off8        = 0b1000, ///< 8  — control logic OFF, FE enabled
    ToT2        = 0b1001, ///< 9  — ToT2, standard
    ToT2_TP_TDC = 0b1010, ///< 10 — ToT2, test-pulse to control logic
    ToT2_TP_FE  = 0b1011, ///< 11 — ToT2, test-pulse to front-end
    SR          = 0b1100, ///< 12 — Slew-Rate, standard
    SR_TP_TDC   = 0b1101, ///< 13 — SR, test-pulse to control logic
    SR_TP_FE    = 0b1110, ///< 14 — SR, test-pulse to front-end
    Off15       = 0b1111, ///< 15 — control logic OFF, FE disabled
};

/** @brief Coarse mode family — the analysis interpretation axis (TP/OFF folded in). */
enum class AlcorOpModeFamily
{
    Off, ///< Front-end / control logic off.
    LET, ///< Leading-edge timestamping.
    ToT, ///< Time-over-threshold (lead/trail edge pairing).
    ToT2, ///< Second ToT variant (not yet interpreted).
    SR   ///< Slew-rate (duration on the rising edge).
};

/** @brief Map a raw run-database @c op_mode integer to the typed enum. */
constexpr AlcorOpMode to_alcor_op_mode(int code)
{
    return static_cast<AlcorOpMode>(code & 0xF);
}

/** @brief Reduce a mode to its interpretation family (TP/OFF variants folded in). */
constexpr AlcorOpModeFamily alcor_op_mode_family(AlcorOpMode m)
{
    switch (m) {
        case AlcorOpMode::LET:
        case AlcorOpMode::LET_TP_TDC:
        case AlcorOpMode::LET_TP_FE:
            return AlcorOpModeFamily::LET;
        case AlcorOpMode::ToT:
        case AlcorOpMode::ToT_TP_TDC:
        case AlcorOpMode::ToT_TP_FE:
            return AlcorOpModeFamily::ToT;
        case AlcorOpMode::ToT2:
        case AlcorOpMode::ToT2_TP_TDC:
        case AlcorOpMode::ToT2_TP_FE:
            return AlcorOpModeFamily::ToT2;
        case AlcorOpMode::SR:
        case AlcorOpMode::SR_TP_TDC:
        case AlcorOpMode::SR_TP_FE:
            return AlcorOpModeFamily::SR;
        default:
            return AlcorOpModeFamily::Off;
    }
}

constexpr bool alcor_mode_is_let(AlcorOpMode m) { return alcor_op_mode_family(m) == AlcorOpModeFamily::LET; }
constexpr bool alcor_mode_is_tot(AlcorOpMode m) { return alcor_op_mode_family(m) == AlcorOpModeFamily::ToT; }
constexpr bool alcor_mode_is_sr(AlcorOpMode m) { return alcor_op_mode_family(m) == AlcorOpModeFamily::SR; }

/**
 * @brief Does this mode reconstruct one hit per pulse by pairing the two
 *        threshold crossings ({0,1}/{2,3}; even = primary, odd = secondary)?
 *
 * True for EVERY two-crossing family — ToT, ToT2 and SR — which all use the SAME
 * edge matching and produce the same per-hit Δt (`duration`).  Only the physical
 * *interpretation* of that Δt differs: ToT/ToT2 = time-over-threshold; SR =
 * slew-rate (the rise time between two thresholds).  LET (one independent leading
 * edge per TDC) and OFF do not pair.  The framer uses this to decide whether to
 * run the pairing stage; the interpretation is a downstream/QA concern.
 */
constexpr bool alcor_mode_pairs_edges(AlcorOpMode m)
{
    const auto f = alcor_op_mode_family(m);
    return f == AlcorOpModeFamily::ToT || f == AlcorOpModeFamily::ToT2 || f == AlcorOpModeFamily::SR;
}

// ── Compile-time unit tests (pure logic, no runtime framework needed) ────────
static_assert(static_cast<int>(AlcorOpMode::LET) == 1, "LET must be PCR code 1");
static_assert(static_cast<int>(AlcorOpMode::ToT) == 4, "ToT must be PCR code 4");
static_assert(static_cast<int>(AlcorOpMode::ToT2) == 9, "ToT2 must be PCR code 9");
static_assert(static_cast<int>(AlcorOpMode::SR) == 12, "SR must be PCR code 12");
static_assert(to_alcor_op_mode(4) == AlcorOpMode::ToT, "code 4 -> ToT");
static_assert(alcor_op_mode_family(AlcorOpMode::ToT_TP_FE) == AlcorOpModeFamily::ToT, "TP variant folds to ToT");
static_assert(alcor_op_mode_family(AlcorOpMode::Off7) == AlcorOpModeFamily::Off, "code 7 is OFF");
static_assert(alcor_mode_is_let(AlcorOpMode::LET) && !alcor_mode_is_let(AlcorOpMode::ToT), "LET predicate");
static_assert(alcor_mode_pairs_edges(AlcorOpMode::ToT) && alcor_mode_pairs_edges(AlcorOpMode::SR) &&
                  alcor_mode_pairs_edges(AlcorOpMode::ToT2),
              "ToT, ToT2 and SR all pair edges (same matching; Δt interpretation differs)");
static_assert(!alcor_mode_pairs_edges(AlcorOpMode::LET) && !alcor_mode_pairs_edges(AlcorOpMode::Off0),
              "LET and OFF do not edge-pair");
