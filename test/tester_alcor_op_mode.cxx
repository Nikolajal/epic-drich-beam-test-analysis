// Runtime checks for the ALCOR operation-mode enum + helpers.
// Complements the compile-time static_asserts in alcor_op_mode.h by exercising
// the runtime conversions (to_alcor_op_mode masking, family/predicate mapping
// over every code) and registering as a CTest target.

#include "utility/alcor_op_mode.h"
#include <cstdio>

int main()
{
    int fails = 0;
    auto check = [&](bool cond, const char *msg)
    {
        if (!cond)
        {
            std::printf("FAIL: %s\n", msg);
            ++fails;
        }
    };

    // Enum values == PCR OpMode codes (datasheet Table 15).
    check(to_alcor_op_mode(1) == AlcorOpMode::LET, "code 1 -> LET");
    check(to_alcor_op_mode(4) == AlcorOpMode::ToT, "code 4 -> ToT");
    check(to_alcor_op_mode(9) == AlcorOpMode::ToT2, "code 9 -> ToT2");
    check(to_alcor_op_mode(12) == AlcorOpMode::SR, "code 12 -> SR");
    // to_alcor_op_mode masks to the low nibble (defensive against stray high bits).
    check(to_alcor_op_mode(20) == AlcorOpMode::ToT, "20 & 0xF == 4 -> ToT");

    // Family folding (test-pulse + OFF variants).
    check(alcor_op_mode_family(AlcorOpMode::LET_TP_FE) == AlcorOpModeFamily::LET, "LET_TP_FE -> LET family");
    check(alcor_op_mode_family(AlcorOpMode::ToT_TP_TDC) == AlcorOpModeFamily::ToT, "ToT_TP_TDC -> ToT family");
    check(alcor_op_mode_family(AlcorOpMode::SR_TP_FE) == AlcorOpModeFamily::SR, "SR_TP_FE -> SR family");
    check(alcor_op_mode_family(AlcorOpMode::Off0) == AlcorOpModeFamily::Off, "code 0 -> OFF");
    check(alcor_op_mode_family(AlcorOpMode::Off7) == AlcorOpModeFamily::Off, "code 7 -> OFF");
    check(alcor_op_mode_family(AlcorOpMode::Off8) == AlcorOpModeFamily::Off, "code 8 -> OFF");
    check(alcor_op_mode_family(AlcorOpMode::Off15) == AlcorOpModeFamily::Off, "code 15 -> OFF");

    // Predicates.
    check(alcor_mode_is_let(AlcorOpMode::LET) && !alcor_mode_is_let(AlcorOpMode::ToT), "is_let");
    check(alcor_mode_is_tot(AlcorOpMode::ToT) && !alcor_mode_is_tot(AlcorOpMode::ToT2), "is_tot (ToT only, not ToT2)");
    check(alcor_mode_is_sr(AlcorOpMode::SR), "is_sr");

    // All two-crossing families pair edges (same matching; Δt interpretation
    // differs): ToT, ToT2, SR.  LET and OFF do not.
    check(alcor_mode_pairs_edges(AlcorOpMode::ToT), "ToT pairs edges");
    check(alcor_mode_pairs_edges(AlcorOpMode::ToT_TP_FE), "ToT TP variant pairs edges");
    check(alcor_mode_pairs_edges(AlcorOpMode::SR), "SR pairs edges (same matching as ToT)");
    check(alcor_mode_pairs_edges(AlcorOpMode::ToT2), "ToT2 pairs edges");
    check(!alcor_mode_pairs_edges(AlcorOpMode::LET), "LET does not edge-pair");
    check(!alcor_mode_pairs_edges(AlcorOpMode::Off0), "OFF does not edge-pair");

    if (fails)
    {
        std::printf("alcor_op_mode: %d check(s) failed\n", fails);
        return 1;
    }
    std::printf("alcor_op_mode: all checks passed\n");
    return 0;
}
