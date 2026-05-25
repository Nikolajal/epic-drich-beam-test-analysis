/**
 * @file test/tester_global_index.cxx
 * @brief Unit tests for the @ref GlobalIndex value type.
 *
 * Build with:
 *   cmake -B build -DBTANA_BUILD_TESTS=ON && cmake --build build
 * Run with:
 *   ctest --test-dir build --output-on-failure
 *
 * Coverage:
 *   1. Component round-trip — @ref GlobalIndex::from_components followed by
 *      every accessor returns the input.
 *   2. Validity bit — default-constructed and raw-zero instances are
 *      invalid; every named constructor sets @c is_valid to true.
 *   3. `try_from_components` returns `std::nullopt` for every out-of-range
 *      component, and an engaged optional for in-range ones.
 *   4. Detector-aware helpers (`real_chip`, `chip_local_channel`) reproduce
 *      the hardware (chip_raw, channel_raw) when @c kUsesSplitInTwo is true.
 *   5. Reserved bits zero invariant — every constructed instance satisfies
 *      @c reserved_bits_are_zero.
 *   6. Global-channel views (`pixel_raw`/`pixel` → renamed `global_channel_raw`/
 *      `global_channel`) strip the TDC bits while preserving the rest.
 *   7. `channel_ordinal` is dense, counter-style, TDC-independent.
 *
 * The test harness is the minimal `CHECK / CHECK_EQ` macro pattern used by
 * MIST's testers — kept symmetrical so the binaries can be aggregated into
 * one CTest dashboard.
 */

#include "util/global_index.h"

#include <cstdint>
#include <cstdlib>     // std::abort
#include <iostream>
#include <optional>

// ---------------------------------------------------------------------------
//  Minimal test harness
// ---------------------------------------------------------------------------

static int s_tests_run = 0;
static int s_tests_failed = 0;

#define CHECK(expr)                                                \
    do                                                             \
    {                                                              \
        ++s_tests_run;                                             \
        if (!(expr))                                               \
        {                                                          \
            ++s_tests_failed;                                      \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__ \
                      << "  " << #expr << "\n";                    \
        }                                                          \
    } while (false)

#define CHECK_EQ(actual, expected)                                       \
    do                                                                   \
    {                                                                    \
        ++s_tests_run;                                                   \
        const auto _a = (actual);                                        \
        const auto _e = (expected);                                      \
        if (!(_a == _e))                                                 \
        {                                                                \
            ++s_tests_failed;                                            \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__       \
                      << "  " << #actual << " == " << #expected          \
                      << "  (got " << _a << ", expected " << _e << ")\n";\
        }                                                                \
    } while (false)

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------

// 1. Component round-trip
void test_component_round_trip()
{
    // A few hand-picked points spanning the field ranges.
    struct sample
    {
        int device, fifo, chip, channel, tdc;
    };
    constexpr sample samples[] = {
        {  192,  0, 0,  0, 0},
        {  192,  0, 7, 63, 3},
        {  207, 31, 3, 42, 2},
        { 1300, 99, 5,  9, 1},
        { 2047,127, 7, 63, 3},   // every field at its maximum
    };

    for (const auto &s : samples)
    {
        const auto idx = GlobalIndex::from_components(s.device, s.fifo, s.chip, s.channel, s.tdc);
        CHECK(idx.is_valid());
        CHECK(idx.reserved_bits_are_zero());
        CHECK_EQ(idx.device(),  s.device);
        CHECK_EQ(idx.fifo(),    s.fifo);
        CHECK_EQ(idx.chip(),    s.chip);
        CHECK_EQ(idx.channel(), s.channel);
        CHECK_EQ(idx.tdc(),     s.tdc);
    }
}

// 2. Validity bit semantics
void test_validity_bit()
{
    // Default-constructed → invalid
    GlobalIndex def;
    CHECK(!def.is_valid());
    CHECK_EQ(def.raw(), uint32_t{0});

    // Raw-zero constructor → invalid (no constructor blessing)
    GlobalIndex raw_zero{uint32_t{0}};
    CHECK(!raw_zero.is_valid());

    // Random raw (no validity bit set) → invalid
    GlobalIndex bogus{uint32_t{0x12345}};
    CHECK(!bogus.is_valid());

    // from_components → valid
    const auto from_comp = GlobalIndex::from_components(192, 5, 2, 17, 1);
    CHECK(from_comp.is_valid());

    // try_from_components on valid input → engaged optional, value is valid
    auto opt = GlobalIndex::try_from_components(192, 5, 2, 17, 1);
    CHECK(opt.has_value());
    CHECK(opt->is_valid());
}

// 4. try_from_components rejects out-of-range components
void test_try_from_components_rejects_invalid()
{
    // Each row pushes exactly one component out of range.
    CHECK(!GlobalIndex::try_from_components(2048,   0, 0,  0, 0).has_value()); // device too high
    CHECK(!GlobalIndex::try_from_components(  -1,   0, 0,  0, 0).has_value()); // device negative
    CHECK(!GlobalIndex::try_from_components( 192, 128, 0,  0, 0).has_value()); // fifo too high
    CHECK(!GlobalIndex::try_from_components( 192,   0, 8,  0, 0).has_value()); // chip too high
    CHECK(!GlobalIndex::try_from_components( 192,   0, 0, 64, 0).has_value()); // channel too high
    CHECK(!GlobalIndex::try_from_components( 192,   0, 0,  0, 4).has_value()); // tdc too high

    // A valid call sandwich confirms the factory still works.
    CHECK( GlobalIndex::try_from_components( 192,   0, 0,  0, 0).has_value());
}

// 4. Detector-aware helpers — real_chip and chip_local_channel
void test_detector_aware_helpers()
{
    // For every hardware (chip_raw, channel_raw_5b) tuple, apply the
    // split-in-two trick to derive the logical (chip, channel) and verify
    // that real_chip() / chip_local_channel() recover the original
    // hardware addressing.
    for (int chip_raw = 0; chip_raw <= 7; ++chip_raw)
        for (int channel_raw = 0; channel_raw <= 31; ++channel_raw)
        {
            int chip_logical;
            int channel_logical;
            if constexpr (gidx::kUsesSplitInTwo)
            {
                chip_logical    = chip_raw / 2;
                channel_logical = channel_raw + 32 * (chip_raw % 2);
            }
            else
            {
                chip_logical    = chip_raw;
                channel_logical = channel_raw;
            }
            const auto gi = GlobalIndex::from_components(
                192, /*fifo=*/0, chip_logical, channel_logical, /*tdc=*/0);

            if constexpr (gidx::kUsesSplitInTwo)
            {
                CHECK_EQ(gi.real_chip(), chip_raw);
                CHECK_EQ(gi.chip_local_channel(), channel_raw);
            }
            else
            {
                CHECK_EQ(gi.real_chip(), gi.chip());
                CHECK_EQ(gi.chip_local_channel(), gi.channel());
            }
        }
}

// 6. eo_channel is the alias for channel in the new layout
void test_eo_channel_alias()
{
    const auto gi = GlobalIndex::from_components(197, 12, 3, 42, 1);
    CHECK_EQ(gi.eo_channel(), gi.channel());
    CHECK_EQ(gi.eo_channel(), 42);
}

// 6. Reserved bits stay zero
void test_reserved_bits_zero()
{
    const auto gi1 = GlobalIndex::from_components(197, 12, 3, 42, 1);
    CHECK(gi1.reserved_bits_are_zero());

    // Boundary case — every encoded field at maximum.
    const auto gi2 = GlobalIndex::from_components(2047, 127, 7, 63, 3);
    CHECK(gi2.reserved_bits_are_zero());

    // try_from_components result, when engaged, also has reserved bits zero.
    const auto gi3 = GlobalIndex::try_from_components(2047, 127, 7, 63, 3);
    CHECK(gi3.has_value());
    CHECK(gi3->reserved_bits_are_zero());
}

// 8. Storage size invariant — single uint32_t
void test_size_is_four_bytes()
{
    static_assert(sizeof(GlobalIndex) == 4,
                  "GlobalIndex must be exactly 32 bits");
    CHECK_EQ(sizeof(GlobalIndex), sizeof(uint32_t));
}

// 9. raw() round-trip via the uint32_t constructor preserves the bit pattern
void test_raw_round_trip()
{
    const auto a = GlobalIndex::from_components(197, 12, 3, 42, 1);
    const GlobalIndex b{a.raw()};
    // b was constructed from raw — same bits, same accessor values.
    CHECK_EQ(b.raw(),      a.raw());
    CHECK_EQ(b.device(),   a.device());
    CHECK_EQ(b.fifo(),     a.fifo());
    CHECK_EQ(b.chip(),     a.chip());
    CHECK_EQ(b.channel(),  a.channel());
    CHECK_EQ(b.tdc(),      a.tdc());
    CHECK_EQ(b.is_valid(), a.is_valid());
}

// 10. Global-channel views — TDC stripped, everything else preserved
void test_global_channel_views()
{
    const auto gi = GlobalIndex::from_components(197, 5, 3, 42, 2);

    // global_channel_raw() must zero out the TDC bits but preserve every other field.
    const uint32_t gcr = gi.global_channel_raw();
    CHECK((gcr & 0x3) == 0u);                              // TDC bits cleared
    CHECK_EQ(gcr & ~uint32_t(0x3), gi.raw() & ~uint32_t(0x3)); // everything else identical

    // global_channel() — GlobalIndex sibling with TDC=0, valid bit preserved
    const auto gc = gi.global_channel();
    CHECK_EQ(gc.tdc(),     0);
    CHECK_EQ(gc.channel(), gi.channel());
    CHECK_EQ(gc.chip(),    gi.chip());
    CHECK_EQ(gc.fifo(),    gi.fifo());
    CHECK_EQ(gc.device(),  gi.device());
    CHECK(gc.is_valid());
    CHECK(gc.reserved_bits_are_zero());

    // global_channel() is idempotent — calling it again changes nothing.
    CHECK(gc.global_channel() == gc);

    // Two indices that differ only in TDC share the same global_channel().
    const auto gi_tdc0 = GlobalIndex::from_components(197, 5, 3, 42, 0);
    const auto gi_tdc3 = GlobalIndex::from_components(197, 5, 3, 42, 3);
    CHECK(gi_tdc0.global_channel() == gi_tdc3.global_channel());
    CHECK(gi_tdc0 != gi_tdc3);

    // The hardware-pixel accessor is unaffected — it returns the ALCOR
    // column-row index (0–3) independent of TDC.
    CHECK_EQ(gi.pixel(), gi.global_channel().pixel());
    CHECK_EQ(gi.column(), gi.global_channel().column());
}

// 8. channel_ordinal — dense counter-style per-channel index
void test_channel_ordinal()
{
    // For every (device, chip_raw, channel_raw, tdc) tuple in the current
    // detector's hardware space, channel_ordinal() must equal the formula
    //   (device - 192) * 256 + chip_raw * 32 + channel_raw    (current detector)
    //   (device - 192) * 512 + chip * 64 + channel             (final detector)
    // — a small dense int suitable for histogram axes.  TDC-independent.
    int n_samples = 0;
    for (int device_off = 0; device_off < 8; ++device_off)
        for (int chip_raw = 0; chip_raw <= 7; ++chip_raw)
            for (int channel_raw = 0; channel_raw <= 31; ++channel_raw)
                for (int tdc = 0; tdc <= 3; ++tdc)
                {
                    int chip_logical, channel_logical;
                    int expected_ordinal;
                    if constexpr (gidx::kUsesSplitInTwo)
                    {
                        chip_logical    = chip_raw / 2;
                        channel_logical = channel_raw + 32 * (chip_raw % 2);
                        expected_ordinal = device_off * 256
                                         + chip_raw * 32
                                         + channel_raw;
                    }
                    else
                    {
                        chip_logical    = chip_raw;
                        channel_logical = channel_raw;
                        expected_ordinal = device_off * 512
                                         + chip_raw  * 64
                                         + channel_raw;
                    }
                    const auto gi = GlobalIndex::from_components(
                        device_off + 192, /*fifo=*/0,
                        chip_logical, channel_logical, tdc);
                    CHECK_EQ(gi.channel_ordinal(), expected_ordinal);
                    ++n_samples;
                }
    // Sanity check: 8 devices × 8 chips × 32 channels × 4 tdcs = 8192.
    CHECK_EQ(n_samples, 8 * 8 * 32 * 4);
}

// 14. channel_ordinal is independent of TDC — same channel, different TDC,
//     same ordinal
void test_channel_ordinal_independent_of_tdc()
{
    const auto gi_tdc0 = GlobalIndex::from_components(197, 5, 3, 42, 0);
    const auto gi_tdc1 = GlobalIndex::from_components(197, 5, 3, 42, 1);
    const auto gi_tdc2 = GlobalIndex::from_components(197, 5, 3, 42, 2);
    const auto gi_tdc3 = GlobalIndex::from_components(197, 5, 3, 42, 3);

    CHECK_EQ(gi_tdc0.channel_ordinal(), gi_tdc1.channel_ordinal());
    CHECK_EQ(gi_tdc0.channel_ordinal(), gi_tdc2.channel_ordinal());
    CHECK_EQ(gi_tdc0.channel_ordinal(), gi_tdc3.channel_ordinal());

    // And different channels give different ordinals.
    const auto gi_other = GlobalIndex::from_components(197, 5, 3, 43, 0);
    CHECK(gi_tdc0.channel_ordinal() != gi_other.channel_ordinal());
}

// 15. Equality / ordering operators
void test_comparison_operators()
{
    const auto a = GlobalIndex::from_components(192, 0, 0, 0, 0);
    const auto b = GlobalIndex::from_components(192, 0, 0, 0, 0);
    const auto c = GlobalIndex::from_components(192, 0, 0, 0, 1);

    CHECK(a == b);
    CHECK(!(a != b));
    CHECK(a != c);
    CHECK(a < c);
}

// ---------------------------------------------------------------------------
//  Entry point
// ---------------------------------------------------------------------------

int main()
{
    std::cout << "Running GlobalIndex tests...\n";

    test_component_round_trip();
    test_validity_bit();
    test_try_from_components_rejects_invalid();
    test_detector_aware_helpers();
    test_eo_channel_alias();
    test_reserved_bits_zero();
    test_size_is_four_bytes();
    test_raw_round_trip();
    test_global_channel_views();
    test_channel_ordinal();
    test_channel_ordinal_independent_of_tdc();
    test_comparison_operators();

    std::cout << s_tests_run << " tests run, " << s_tests_failed << " failed.\n";

    if (s_tests_failed == 0)
    {
        std::cout << "All GlobalIndex tests passed.\n";
        return 0;
    }
    return 1;
}
