/**
 * @file test/tester_config_autocouple.cxx
 * @brief Unit tests for the wide-arc config auto-coupling in the readers.
 *
 * Build with:
 *   cmake -B build -DBTANA_BUILD_TESTS=ON && cmake --build build
 * Run with:
 *   ctest --test-dir build --output-on-failure
 *
 * Coverage:
 *   1. `streaming_ransac_conf_reader` derives r_max = pad + sensor + margin
 *      when r_max_auto=true, and auto-couples centre_xy_half_range_mm to the
 *      padding when that key is unset.
 *   2. r_max_auto is refused (left off, explicit r_max kept) when
 *      centre_padding_mm < 0 — the mutually-exclusive sentinel guard.
 *   3. Legacy mode (no r_max_auto) leaves r_max explicit and does NOT touch
 *      centre_xy_half_range_mm.
 *   4. `recodata_conf_reader` reads arc_span_min_rad and sets
 *      radial_eff_per_ring_centre to mirror the [streaming_ransac] r_max_auto.
 *
 * Harness: the minimal CHECK macro shared with the other testers.
 */

#include "utility/config_reader.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

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

#define CHECK_NEAR(actual, expected, tol)                          \
    do                                                             \
    {                                                              \
        ++s_tests_run;                                             \
        const double _a = (actual);                                \
        const double _e = (expected);                              \
        if (std::fabs(_a - _e) > (tol))                            \
        {                                                          \
            ++s_tests_failed;                                      \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__ \
                      << "  got " << _a << " expected " << _e       \
                      << " ± " << (tol) << "\n";                   \
        }                                                          \
    } while (false)

static std::string write_tmp(const std::string &name, const std::string &body)
{
    const std::string path = std::string("btana_test_") + name;
    std::ofstream os(path);
    os << body;
    os.close();
    return path;
}

// 1. Auto-coupling derives r_max and widens the QA centre axis.
void test_auto_couples_r_max_and_axis()
{
    const std::string path = write_tmp("auto_streaming.toml",
                                       "[streaming_ransac]\n"
                                       "r_min = 35.0\n"
                                       "r_max = 105.0\n"
                                       "r_step = 3.0\n"
                                       "cell_size = 3.0\n"
                                       "centre_padding_mm = 250.0\n"
                                       "r_max_auto = true\n"
                                       "sensor_half_extent_mm = 99.0\n"
                                       "r_margin_mm = 10.0\n");
    const auto cfg = streaming_ransac_conf_reader(path);
    CHECK(cfg.r_max_auto);
    CHECK_NEAR(cfg.r_max, 250.0 + 99.0 + 10.0, 1e-4); // = 359
    CHECK_NEAR(cfg.centre_padding_mm, 250.0, 1e-4);
    // centre_xy_half_range_mm was not set → auto-coupled to the padding.
    CHECK_NEAR(cfg.centre_xy_half_range_mm, 250.0, 1e-4);
    std::remove(path.c_str());
}

// 2. Negative padding refuses auto; explicit r_max is kept.
void test_auto_refused_for_negative_pad()
{
    const std::string path = write_tmp("badpad_streaming.toml",
                                       "[streaming_ransac]\n"
                                       "r_max = 105.0\n"
                                       "centre_padding_mm = -1.0\n"
                                       "r_max_auto = true\n");
    const auto cfg = streaming_ransac_conf_reader(path);
    CHECK(!cfg.r_max_auto);             // guard turned it off
    CHECK_NEAR(cfg.r_max, 105.0, 1e-4); // explicit value preserved
    std::remove(path.c_str());
}

// 3. Legacy mode untouched.
void test_legacy_untouched()
{
    const std::string path = write_tmp("legacy_streaming.toml",
                                       "[streaming_ransac]\n"
                                       "r_max = 105.0\n"
                                       "centre_padding_mm = 10.0\n"
                                       "centre_xy_half_range_mm = 25.0\n");
    const auto cfg = streaming_ransac_conf_reader(path);
    CHECK(!cfg.r_max_auto);
    CHECK_NEAR(cfg.r_max, 105.0, 1e-4);
    CHECK_NEAR(cfg.centre_xy_half_range_mm, 25.0, 1e-4);
    std::remove(path.c_str());
}

// 4. recodata reader: arc_span + per-ring eff flag mirror r_max_auto.
void test_recodata_mirrors_wide_arc()
{
    const std::string s_wide = write_tmp("reco_wide_streaming.toml",
                                         "[streaming_ransac]\n"
                                         "centre_padding_mm = 250.0\n"
                                         "r_max_auto = true\n"
                                         "arc_span_min_rad = 0.7\n"
                                         "min_hits_per_ring = 5\n");
    const std::string s_legacy = write_tmp("reco_legacy_streaming.toml",
                                           "[streaming_ransac]\n"
                                           "r_max = 105.0\n"
                                           "min_hits_per_ring = 5\n");
    const std::string m = write_tmp("reco_mapping.toml",
                                    "[coverage]\n"
                                    "r_min_coverage_mm = 25.0\n"
                                    "r_max_coverage_mm = 360.0\n");

    const auto wide = recodata_conf_reader(s_wide, m);
    CHECK(wide.radial_eff_per_ring_centre);
    CHECK_NEAR(wide.arc_span_min_rad, 0.7, 1e-4);

    const auto legacy = recodata_conf_reader(s_legacy, m);
    CHECK(!legacy.radial_eff_per_ring_centre);

    std::remove(s_wide.c_str());
    std::remove(s_legacy.c_str());
    std::remove(m.c_str());
}

// 5. Wide coarse-scan: explicit r_max (auto off), explicit arc-mode, axis
//    auto-couples to the padding regardless of r_max_auto.
void test_explicit_coarse_scan()
{
    const std::string s = write_tmp("coarse_streaming.toml",
                                    "[streaming_ransac]\n"
                                    "r_min = 10.0\n"
                                    "r_max = 10000.0\n"
                                    "r_max_auto = false\n"
                                    "cell_size = 25.0\n"
                                    "r_step = 25.0\n"
                                    "centre_padding_mm = 1000.0\n"
                                    "radial_eff_per_ring_centre = true\n");
    const std::string m = write_tmp("coarse_mapping.toml",
                                    "[coverage]\n"
                                    "r_min_coverage_mm = 25.0\n"
                                    "r_max_coverage_mm = 600.0\n");
    const auto sh = streaming_ransac_conf_reader(s);
    CHECK(!sh.r_max_auto);
    CHECK_NEAR(sh.r_max, 10000.0, 1e-3);          // explicit, NOT derived
    CHECK_NEAR(sh.centre_padding_mm, 1000.0, 1e-4);
    CHECK_NEAR(sh.centre_xy_half_range_mm, 1000.0, 1e-4); // auto-coupled w/ auto off

    const auto rc = recodata_conf_reader(s, m);
    CHECK(rc.radial_eff_per_ring_centre);          // explicit key honoured
    std::remove(s.c_str());
    std::remove(m.c_str());
}

int main()
{
    std::cout << "Running config auto-couple tests...\n";

    test_auto_couples_r_max_and_axis();
    test_auto_refused_for_negative_pad();
    test_legacy_untouched();
    test_recodata_mirrors_wide_arc();
    test_explicit_coarse_scan();

    std::cout << s_tests_run << " tests run, " << s_tests_failed << " failed.\n";
    if (s_tests_failed == 0)
    {
        std::cout << "All config auto-couple tests passed.\n";
        return 0;
    }
    return 1;
}
