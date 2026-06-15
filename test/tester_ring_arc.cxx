/**
 * @file test/tester_ring_arc.cxx
 * @brief Unit tests for the wide-arc ring-reconstruction primitives.
 *
 * Build with:
 *   cmake -B build -DBTANA_BUILD_TESTS=ON && cmake --build build
 * Run with:
 *   ctest --test-dir build --output-on-failure
 *
 * Coverage (the mist Taubin circle fit recodata now uses):
 *   1. `circle_fit(taubin)` recovers a FULL circle's centre + radius.
 *   2. `circle_fit(taubin)` recovers a far-off-centre ARC (centre (200,0),
 *      R=250, 60° span) — the case the old centroid-seeded iterative fit
 *      failed on (centroid pull → centre collapsed toward the origin).
 *   3. `circle_fit(taubin)` returns ok=false for (near-)collinear points.
 *   4. `circle_fit(taubin)` on a NOISY far arc stays in the right
 *      neighbourhood (no runaway / centroid pull).
 *   5. `azimuthal_coverage_fraction` and the N_γ = n_hits / f_coverage
 *      estimator are invariant under a rigid translation of a full ring +
 *      its channel map (centre-agnostic, as the wide-arc path relies on).
 *
 * Harness: the minimal CHECK macro shared with tester_global_index.cxx.
 */

#include <mist/ring_finding/circle_fit.h>  // mist::ring_finding::circle_fit (Taubin)
#include "utility/radiator_efficiency.h"   // azimuthal_coverage_fraction

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>

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

#define CHECK_NEAR(actual, expected, tol)                                 \
    do                                                                    \
    {                                                                     \
        ++s_tests_run;                                                    \
        const double _a = (actual);                                       \
        const double _e = (expected);                                     \
        if (std::fabs(_a - _e) > (tol))                                   \
        {                                                                 \
            ++s_tests_failed;                                             \
            std::cerr << "  FAIL  " << __FILE__ << ":" << __LINE__        \
                      << "  " << #actual << " ~= " << #expected           \
                      << "  (got " << _a << ", expected " << _e           \
                      << " ± " << (tol) << ")\n";                         \
        }                                                                 \
    } while (false)

using mist::ring_finding::circle_fit;
using mist::ring_finding::circle_method;

static constexpr double kPi = 3.14159265358979323846;

//  A Point2 for circle_fit (needs .x / .y members).
struct TP
{
    double x, y;
};

//  Sample an arc of a circle (cx, cy, R) over [phi0, phi0 + span].
static std::vector<TP>
make_arc(double cx, double cy, double R, double phi0, double span, int n)
{
    std::vector<TP> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        const double phi = phi0 + span * (n > 1 ? double(i) / double(n - 1) : 0.0);
        pts.push_back({cx + R * std::cos(phi), cy + R * std::sin(phi)});
    }
    return pts;
}

// 1. Full circle — Taubin recovers centre + radius.
void test_taubin_full_circle()
{
    const auto fit = circle_fit(make_arc(12, -7, 80, 0, 2 * kPi, 64),
                                circle_method::taubin);
    CHECK(fit.ok);
    CHECK_NEAR(fit.x0, 12.0, 1e-2);
    CHECK_NEAR(fit.y0, -7.0, 1e-2);
    CHECK_NEAR(fit.radius, 80.0, 1e-2);
}

// 2. Far-off-centre arc (the wide-arc case) — exact on noise-free points.
//    The whole point of Taubin: a 60° arc with a 250 mm-distant centre is
//    recovered without the centroid pull that biased the old iterative fit.
void test_taubin_far_arc()
{
    const auto fit = circle_fit(make_arc(200, 0, 250, kPi - 0.5, 1.05 /*~60°*/, 40),
                                circle_method::taubin);
    CHECK(fit.ok);
    CHECK_NEAR(fit.x0, 200.0, 1e-1);
    CHECK_NEAR(fit.y0, 0.0, 1e-1);
    CHECK_NEAR(fit.radius, 250.0, 1e-1);
}

// 3. Collinear points → degenerate → ok == false.
void test_taubin_collinear_rejected()
{
    std::vector<TP> pts;
    for (int i = 0; i < 10; ++i)
        pts.push_back({double(i) * 3.0, 5.0}); // a horizontal line
    CHECK(!circle_fit(pts, circle_method::taubin).ok);
}

// 4. Noisy far arc — lands in the right neighbourhood, no runaway / centroid pull.
void test_taubin_noisy_far_arc()
{
    auto pts = make_arc(200, 0, 250, kPi - 0.45, 0.9, 30);
    // Deterministic pixel-scale jitter (no RNG: ±0.4 mm comb).
    for (std::size_t i = 0; i < pts.size(); ++i)
    {
        pts[i].x += (i % 2 ? 0.4 : -0.4);
        pts[i].y += (i % 3 ? -0.3 : 0.3);
    }
    const auto fit = circle_fit(pts, circle_method::taubin);
    CHECK(fit.ok);
    CHECK_NEAR(fit.x0, 200.0, 25.0);
    CHECK_NEAR(fit.y0, 0.0, 25.0);
    CHECK_NEAR(fit.radius, 250.0, 25.0);
}

// 5. Coverage + N_gamma are translation-invariant (centre-agnostic).
void test_coverage_centre_invariance()
{
    // Build a dense ring of "channels" at radius R about the origin, then the
    // SAME geometry shifted by (+150, -40).  A full ring of hits sits on each;
    // f_coverage and n_hits/f_coverage must match.
    const float R = 70.f;
    const int n_ch = 360;
    std::map<int, std::array<float, 2>> chan_origin, chan_shifted;
    for (int i = 0; i < n_ch; ++i)
    {
        const float phi = 2.f * kPi * float(i) / float(n_ch);
        const float x = R * std::cos(phi), y = R * std::sin(phi);
        chan_origin[i] = {x, y};
        chan_shifted[i] = {x + 150.f, y - 40.f};
    }
    const float f0 = util::radiator_efficiency::azimuthal_coverage_fraction(
        chan_origin, 0.f, 0.f, R, 3.f, 1.5f);
    const float f1 = util::radiator_efficiency::azimuthal_coverage_fraction(
        chan_shifted, 150.f, -40.f, R, 3.f, 1.5f);
    CHECK_NEAR(f0, f1, 1e-4);
    CHECK_NEAR(f0, 1.0, 1e-3); // a full ring → ~complete coverage

    // N_gamma = n_hits / f_coverage with the same n_hits is invariant too.
    const int n_hits = 42;
    CHECK_NEAR(n_hits / f0, n_hits / f1, 1e-3);
}

int main()
{
    std::cout << "Running ring-arc tests...\n";

    test_taubin_full_circle();
    test_taubin_far_arc();
    test_taubin_collinear_rejected();
    test_taubin_noisy_far_arc();
    test_coverage_centre_invariance();

    std::cout << s_tests_run << " tests run, " << s_tests_failed << " failed.\n";
    if (s_tests_failed == 0)
    {
        std::cout << "All ring-arc tests passed.\n";
        return 0;
    }
    return 1;
}
