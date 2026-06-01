/**
 * @file src/radiator_efficiency.cxx
 * @brief Implementation of @ref util::radiator_efficiency helpers.
 *
 * See header for the API + physics interpretation.  Ported from the
 * offline `photon_number_new.cpp` start-of-spill
 * coverage-map fill loop and the `radial_efficiency` function, with
 * the φ-gap (kPhiGapRanges) split removed for V1 — that's a
 * finer-analysis concern surfaced in a follow-up.
 *
 * No spill-weighting, no dead-channel masking: the input
 * `channel_xy` map already excludes dead / unmapped channels (both
 * writers build it via `Mapping::get_position_from_global_index` and
 * skip channels that return `nullopt`).  Coverage values are channel
 * counts at each (φ, R) cell — a pure geometric quantity.
 */

#include "utility/radiator_efficiency.h"

#include <TAxis.h>
#include <TH1.h>
#include <TH2.h>
#include <TMath.h>

#include <algorithm>
#include <cmath>

namespace util::radiator_efficiency
{

// ============================================================
//  build_coverage_map
// ============================================================

TH2F *build_coverage_map(
    const std::map<int, std::array<float, 2>> &channel_xy,
    int n_phi_bins,
    float r_min_mm,
    float r_max_mm,
    int n_r_bins,
    float channel_half_width_mm,
    float centre_x,
    float centre_y,
    float min_channel_r_mm,
    const std::map<int, float> *channel_weights)
{
    // Static counter avoids ROOT name clashes when this helper is
    // called more than once in the same process — each map gets a
    // unique name; callers free to Rename afterwards.
    static int counter = 0;
    TH2F *coverage_map = new TH2F(
        (std::string("h_coverage_map_rphi_") + std::to_string(counter++)).c_str(),
        ";#phi (rad);R (mm)",
        n_phi_bins, -static_cast<float>(TMath::Pi()), static_cast<float>(TMath::Pi()),
        n_r_bins, r_min_mm, r_max_mm);

    // For each channel, rasterise its pixel footprint onto the (φ, R)
    // grid by iterating (φ, R) bins in the channel's bounding box
    // and incrementing each bin whose CENTRE, projected back to
    // (x, y), lies inside the pixel.  Counting convention:
    //
    //   coverage_map[φ, R] = number of channels covering that bin.
    //
    // So each channel contributes +1 to each (φ, R) bin it covers
    // (not 1/N_bins — that was a previous bug from in-(x, y)
    // oversampling that *under*-normalised eff(R) to ~16 % because
    // the Jacobian smears each pixel's "weight 1" across the
    // ~6 (φ, R) bins it spans).  This convention matches the
    // offline macro `photon_number_new.cpp` lines 526–550, so
    // eff(R) values are directly comparable to the macro's.
    //
    // Limitation: the single-point bin-centre test produces
    // "speckled" holes inside each tile where the (φ, R) bin
    // centre lies just outside the pixel boundary even though the
    // bin's *area* overlaps the pixel.  Visible as fine empty
    // dots in the coverage TH2F.  Cosmetic — does not affect
    // eff(R) once averaged over φ.  See for the
    // proper-area-integration upgrade path if/when it matters.
    for (const auto &[lut_key, position] : channel_xy)
    {
        //  Per-channel weight (spill-by-spill active-channel
        //  correction;).  If a `channel_weights`
        //  map is supplied:
        //    - channel key NOT in the map  → skip entirely
        //      (effective mask of unmapped / always-dead channels)
        //    - channel key IN the map      → each bin gets the
        //      mapped weight instead of +1
        //  If no `channel_weights` is supplied, all channels
        //  contribute +1 per covered bin (legacy V1 = geometric
        //  upper bound).
        float channel_weight = 1.f;
        if (channel_weights)
        {
            auto it_w = channel_weights->find(lut_key);
            if (it_w == channel_weights->end())
                continue; // unmapped channel — silently skip
            channel_weight = it_w->second;
            if (channel_weight <= 0.f)
                continue; // dead channel — contributes nothing
        }

        const float channel_dx_mm = position[0] - centre_x;
        const float channel_dy_mm = position[1] - centre_y;
        const float channel_r_mm = std::hypot(channel_dx_mm, channel_dy_mm);
        if (channel_r_mm <= 0.f)
            continue;
        //  Optional low-R cut to drop bogus-position channels that
        //  would otherwise produce a "low-R bump" in the coverage
        //  map.  Default min_channel_r_mm = 0 skips no channels.
        if (channel_r_mm < min_channel_r_mm)
            continue;
        const float channel_phi_rad = std::atan2(channel_dy_mm, channel_dx_mm);

        // Bounding box: multiply by √2 to bound the diagonal of
        // the pixel rectangle (its (R, φ) extent worst-case).
        const float delta_r_mm = channel_half_width_mm * std::sqrt(2.f);
        const float delta_phi_rad = channel_half_width_mm * std::sqrt(2.f) /
                                    channel_r_mm;

        const int bin_r_lo = coverage_map->GetYaxis()->FindBin(channel_r_mm - delta_r_mm);
        const int bin_r_hi = coverage_map->GetYaxis()->FindBin(channel_r_mm + delta_r_mm);
        const int bin_phi_lo = coverage_map->GetXaxis()->FindBin(channel_phi_rad - delta_phi_rad);
        const int bin_phi_hi = coverage_map->GetXaxis()->FindBin(channel_phi_rad + delta_phi_rad);

        for (int bin_phi = bin_phi_lo; bin_phi <= bin_phi_hi; ++bin_phi)
        {
            if (bin_phi < 1 || bin_phi > n_phi_bins)
                continue;
            const float bin_phi_centre =
                coverage_map->GetXaxis()->GetBinCenter(bin_phi);
            for (int bin_r = bin_r_lo; bin_r <= bin_r_hi; ++bin_r)
            {
                if (bin_r < 1 || bin_r > n_r_bins)
                    continue;
                const float bin_r_centre =
                    coverage_map->GetYaxis()->GetBinCenter(bin_r);
                // Project the bin centre back to (x, y) and test
                // the pixel-square containment.  Same convention
                // as the offline macro.
                const float bin_dx = bin_r_centre * std::cos(bin_phi_centre) - channel_dx_mm;
                const float bin_dy = bin_r_centre * std::sin(bin_phi_centre) - channel_dy_mm;
                if (std::fabs(bin_dx) > channel_half_width_mm ||
                    std::fabs(bin_dy) > channel_half_width_mm)
                    continue;
                coverage_map->AddBinContent(
                    coverage_map->GetBin(bin_phi, bin_r),
                    static_cast<double>(channel_weight));
            }
        }
    }
    // Manual stats refresh since we used AddBinContent (which
    // doesn't bump GetEntries on its own).
    coverage_map->SetEntries(coverage_map->Integral());
    // Stats are now driven by ROOT's TH2F::Fill bookkeeping — no
    // manual entry-count refresh needed.  Entries == n_channels ×
    // kOversamplesPerSide² (modulo the `sr > 0` skip), which is
    // the natural count given the oversampling.
    return coverage_map;
}

// ============================================================
//  build_coverage_map_xy
// ============================================================

TH2F *build_coverage_map_xy(
    const std::map<int, std::array<float, 2>> &channel_xy,
    int n_x_bins,
    float x_min_mm,
    float x_max_mm,
    int n_y_bins,
    float y_min_mm,
    float y_max_mm,
    float channel_half_width_mm,
    const std::map<int, float> *channel_weights)
{
    static int counter = 0;
    TH2F *coverage_map = new TH2F(
        (std::string("h_coverage_map_xy_") + std::to_string(counter++)).c_str(),
        ";c_{x} (mm);c_{y} (mm)",
        n_x_bins, x_min_mm, x_max_mm,
        n_y_bins, y_min_mm, y_max_mm);

    //  Same weight + footprint convention as build_coverage_map, but
    //  cartesian — each channel's ±channel_half_width pixel square is
    //  rasterised by a plain bounding-box bin-centre containment test
    //  (no polar Jacobian).  Bin value = Σ weight of covering channels.
    for (const auto &[lut_key, position] : channel_xy)
    {
        float channel_weight = 1.f;
        if (channel_weights)
        {
            auto it_w = channel_weights->find(lut_key);
            if (it_w == channel_weights->end())
                continue;  // unmapped → effectively masked
            channel_weight = it_w->second;
            if (channel_weight <= 0.f)
                continue;  // dead channel
        }

        const float cx = position[0];
        const float cy = position[1];
        const int bin_x_lo = coverage_map->GetXaxis()->FindBin(cx - channel_half_width_mm);
        const int bin_x_hi = coverage_map->GetXaxis()->FindBin(cx + channel_half_width_mm);
        const int bin_y_lo = coverage_map->GetYaxis()->FindBin(cy - channel_half_width_mm);
        const int bin_y_hi = coverage_map->GetYaxis()->FindBin(cy + channel_half_width_mm);
        for (int bx = bin_x_lo; bx <= bin_x_hi; ++bx)
        {
            if (bx < 1 || bx > n_x_bins)
                continue;
            const float bx_centre = coverage_map->GetXaxis()->GetBinCenter(bx);
            for (int by = bin_y_lo; by <= bin_y_hi; ++by)
            {
                if (by < 1 || by > n_y_bins)
                    continue;
                const float by_centre = coverage_map->GetYaxis()->GetBinCenter(by);
                if (std::fabs(bx_centre - cx) > channel_half_width_mm ||
                    std::fabs(by_centre - cy) > channel_half_width_mm)
                    continue;
                coverage_map->AddBinContent(
                    coverage_map->GetBin(bx, by),
                    static_cast<double>(channel_weight));
            }
        }
    }
    coverage_map->SetEntries(coverage_map->Integral());
    return coverage_map;
}

// ============================================================
//  radial_efficiency
// ============================================================

TH1F *radial_efficiency(
    const TH2F *coverage_map,
    const TAxis *radial_reference_axis)
{
    if (!coverage_map || !radial_reference_axis)
        return nullptr;

    static int counter = 0;
    TH1F *eff_R = new TH1F(
        (std::string("h_eff_R_") + std::to_string(counter++)).c_str(),
        ";R (mm);#it{eff}(R)",
        radial_reference_axis->GetNbins(),
        radial_reference_axis->GetXmin(),
        radial_reference_axis->GetXmax());

    const int n_phi_bins = coverage_map->GetNbinsX();
    const int n_r_bins = coverage_map->GetNbinsY();
    if (n_phi_bins <= 0)
        return eff_R;

    // Walk the coverage map's R bins, average over φ, route into the
    // output axis (which may have a different / coarser binning).
    // Two-stage accumulation: per output-bin sum + count, finalised
    // at the end with the mean.
    std::vector<double> output_sum(radial_reference_axis->GetNbins() + 2, 0.);
    std::vector<int> output_count(radial_reference_axis->GetNbins() + 2, 0);

    for (int ir = 1; ir <= n_r_bins; ++ir)
    {
        const float bin_r_centre = coverage_map->GetYaxis()->GetBinCenter(ir);
        const int out_bin = eff_R->GetXaxis()->FindBin(bin_r_centre);
        if (out_bin < 1 || out_bin > radial_reference_axis->GetNbins())
            continue;

        double phi_sum = 0.;
        for (int iphi = 1; iphi <= n_phi_bins; ++iphi)
            phi_sum += coverage_map->GetBinContent(iphi, ir);

        output_sum[out_bin] += phi_sum / static_cast<double>(n_phi_bins);
        output_count[out_bin] += 1;
    }

    for (int iout = 1; iout <= radial_reference_axis->GetNbins(); ++iout)
        if (output_count[iout] > 0)
            eff_R->SetBinContent(iout,
                                 output_sum[iout] / static_cast<double>(output_count[iout]));
    return eff_R;
}

// ============================================================
//  azimuthal_coverage_fraction
// ============================================================

float azimuthal_coverage_fraction(
    const std::map<int, std::array<float, 2>> &channel_xy,
    float cx,
    float cy,
    float R,
    float delta_r_mm,
    float channel_half_width_mm)
{
    if (R <= 0.f || delta_r_mm <= 0.f || channel_xy.empty())
        return 0.f;

    // Collect (φ_lo, φ_hi) segments per channel whose pixel lies
    // within ±delta_r_mm of the ring arc.  Each channel's φ extent
    // is ≈ pixel_pitch / r_ch (small for channels far from the
    // centre).  We then sort and merge overlapping segments,
    // accumulate the total covered φ, and divide by 2π.
    //
    // Segments straddling the ±π wrap are split into two: this
    // keeps the merge logic on a single linear axis without
    // modular gymnastics, at the cost of one extra entry per
    // wrap-crossing channel — negligible.
    constexpr float kTwoPi = 2.f * static_cast<float>(TMath::Pi());
    constexpr float kPi = static_cast<float>(TMath::Pi());

    std::vector<std::array<float, 2>> segments;
    segments.reserve(channel_xy.size());

    for (const auto &[lut_key, position] : channel_xy)
    {
        const float dx = position[0] - cx;
        const float dy = position[1] - cy;
        const float r_ch = std::hypot(dx, dy);
        if (r_ch <= 0.f)
            continue;
        if (std::fabs(r_ch - R) > delta_r_mm)
            continue;

        const float phi_ch = std::atan2(dy, dx);
        // Half-extent in φ for this channel: pixel half-width converted to an
        // angular span at the channel's radius.  `channel_half_width_mm` is
        // threaded from RecodataConfigStruct so f_coverage and
        // build_coverage_map describe the same geometry from one config value.
        const float delta_phi = channel_half_width_mm / r_ch;
        float phi_lo = phi_ch - delta_phi;
        float phi_hi = phi_ch + delta_phi;

        if (phi_lo < -kPi)
        {
            segments.push_back({phi_lo + kTwoPi, kPi});
            segments.push_back({-kPi, phi_hi});
        }
        else if (phi_hi > kPi)
        {
            segments.push_back({phi_lo, kPi});
            segments.push_back({-kPi, phi_hi - kTwoPi});
        }
        else
        {
            segments.push_back({phi_lo, phi_hi});
        }
    }
    if (segments.empty())
        return 0.f;

    // Sort by lo edge, merge overlapping / adjacent segments.
    std::sort(segments.begin(), segments.end(),
              [](const std::array<float, 2> &a, const std::array<float, 2> &b)
              { return a[0] < b[0]; });

    float covered_phi = 0.f;
    float cur_lo = segments[0][0];
    float cur_hi = segments[0][1];
    for (std::size_t i = 1; i < segments.size(); ++i)
    {
        if (segments[i][0] <= cur_hi)
        {
            // Overlap (or adjacent) — extend current segment.
            cur_hi = std::max(cur_hi, segments[i][1]);
        }
        else
        {
            covered_phi += (cur_hi - cur_lo);
            cur_lo = segments[i][0];
            cur_hi = segments[i][1];
        }
    }
    covered_phi += (cur_hi - cur_lo);

    return std::min(1.f, covered_phi / kTwoPi);
}

} // namespace util::radiator_efficiency
