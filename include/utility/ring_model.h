#pragma once

/**
 * @file utility/ring_model.h
 * @brief Analytical ring-signal model + histogram-based ring fitter.
 *
 * Models a Cherenkov ring as a Gaussian-in-radius signal on a flat
 * background, with an azimuthally-varying width built from optional
 * logistic features (used to model PDU boundaries).  The fit minimises a
 * chi² over a 2-D histogram via `ROOT::Fit::Fitter`.
 *
 * Result contour plotting is provided by @ref plot_ring_integral —
 * generates `TGraph` polygons at user-specified σ levels.
 */

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <Math/Functor.h>
#include <Fit/Fitter.h>
#include <TError.h>
#include <TGraph.h>
#include <TH2.h>
#include <TMath.h>

/**
 * @brief Difference-of-logistic function used to model azimuthal acceptance gaps.
 * @param variable   Independent variable (e.g. azimuthal angle φ [rad]).
 * @param amplitude  Peak amplitude.
 * @param center_1   Centre of the rising logistic.
 * @param sigma_1    Width of the rising logistic.
 * @param center_2   Centre of the falling logistic.
 * @param sigma_2    Width of the falling logistic.
 * @return           Function value at @p variable.
 */
inline double logistic(double variable, double amplitude, double center_1, double sigma_1, double center_2, double sigma_2)
{
    return amplitude *
           (1. / (1. + exp(-(variable - center_1) / sigma_1)) - 1. / (1. + exp(-(variable - center_2) / sigma_2)));
}

/**
 * @brief Clip an angle to a given range (placeholder — currently throws).
 * @todo Implement the actual clipping.
 */
inline double clip_phi(double phi, double low_bound, double high_bound)
{
    throw std::logic_error("clip_phi: not implemented");
    (void)phi;
    (void)low_bound;
    (void)high_bound;
    return -1.;
}

/**
 * @brief Azimuthally-varying ring-width model.
 *
 * Returns a baseline sigma plus contributions from one or more logistic features
 * at configurable azimuthal positions.  Used to model PDU boundaries.
 *
 * @param phi             Azimuthal angle [rad].
 * @param baseline_sigma  Constant ring-width baseline.
 * @param input_values    Logistic feature descriptors: each element is
 *                        {amplitude, centre, width, logistic-sigma}.
 * @return                Effective ring-width sigma at @p phi.
 */
inline double ring_fit_function_sigma_function(double phi, double baseline_sigma, std::vector<std::array<double, 4>> input_values = {})
{
    double result = baseline_sigma;
    for (auto current_logistic : input_values)
    {
        result += logistic(phi, current_logistic[0], current_logistic[1] - 0.5 * current_logistic[2], current_logistic[3], current_logistic[1] + 0.5 * current_logistic[2], current_logistic[3]);
    }
    return result;
}

/**
 * @brief Evaluate the ring signal + flat background model in (R, φ) coordinates.
 *
 * @param input_values  {R [mm], φ [rad]} of the point to evaluate.
 * @param parameters    {x0, y0, R0, sigma_R, N_gamma, bkg_level}.
 * @param logistic_input_values  Optional azimuthal-gap descriptors.
 * @return              Expected density at the given (R, φ).
 */
inline double ring_fit_function(std::array<double, 2> input_values, std::array<double, 6> parameters, std::vector<std::array<double, 4>> logistic_input_values = {})
{
    auto current_radius = input_values[0];
    auto current_phi = input_values[1];

    auto ring_radius = parameters[2];
    auto ring_sigma = parameters[3];
    auto ring_photons = parameters[4];
    auto bkg_level = parameters[5];

    auto signal = ring_photons * (1. / (2 * TMath::Pi() * ring_radius)) * TMath::Gaus(current_radius, ring_radius, ring_fit_function_sigma_function(current_phi, ring_sigma, logistic_input_values), true);
    return signal + bkg_level;
}

/**
 * @brief Evaluate the ring model in Cartesian (x, y) coordinates.
 */
inline double ring_fit_function_xy(std::array<double, 2> input_values, std::array<double, 6> parameters, std::vector<std::array<double, 4>> logistic_input_values = {})
{
    auto x_center = input_values[0];
    auto y_center = input_values[1];

    auto ring_x_center = parameters[0];
    auto ring_y_center = parameters[1];

    auto current_radius = hypot(x_center - ring_x_center, y_center - ring_y_center);
    auto current_phi = atan2(y_center - ring_y_center, x_center - ring_x_center);

    return ring_fit_function({current_radius, current_phi}, parameters, logistic_input_values);
}

/**
 * @brief Overload accepting a raw C-style parameter array (ROOT fitter compatible).
 */
inline double ring_fit_function_xy(std::array<double, 2> input_values, const double *parameters, int how_many_logistics = 0)
{
    std::array<double, 6> array_parameters = {
        static_cast<double>(parameters[0]),
        static_cast<double>(parameters[1]),
        static_cast<double>(parameters[2]),
        static_cast<double>(parameters[3]),
        static_cast<double>(parameters[4]),
        static_cast<double>(parameters[5])};

    std::vector<std::array<double, 4>> logistic_input_values;
    for (auto i_logistic = 0; i_logistic < how_many_logistics; i_logistic++)
        logistic_input_values.emplace_back(
            std::array<double, 4>{parameters[5 + i_logistic * 4 + 0],
                                  parameters[5 + i_logistic * 4 + 1],
                                  parameters[5 + i_logistic * 4 + 2],
                                  parameters[5 + i_logistic * 4 + 3]});

    return ring_fit_function_xy(input_values, array_parameters, logistic_input_values);
}

/// Result type for ring fits: { {x0,σ}, {y0,σ}, {R,σ}, {sigma_R,σ}, {N,σ}, {bkg,σ} }.
using RingFitResults = std::array<std::array<double, 2>, 6>;

/**
 * @brief Fit a 2-D ring model to a histogram using chi-squared minimisation.
 *
 * Integrates @ref ring_fit_function_xy over each bin and minimises the sum of
 * squared normalised residuals.
 *
 * @param target_histogram  Input TH2 (any bin size; bins with zero content are skipped).
 * @param initial_values    Initial guess {x0, y0, R, sigma_R, N_gamma, bkg}.
 * @param fix_XY            If @c true, fix (x0, y0) and fit only R and normalisation.
 * @return                  Fit result with central values and uncertainties.
 */
inline RingFitResults fit_ring_integral(TH2 *target_histogram, std::array<double, 6> initial_values, bool fix_XY = true)
{
    RingFitResults result{}; // value-init so the failure path returns zeros, not garbage

    // Explicit captures: target_histogram by value (it's a pointer, safe to
    // copy), nothing by reference.  Avoids the [&] footgun once the lambda
    // ever escapes the function
    auto chi2_function = [target_histogram](const double *parameters)
    {
        double chi2 = 0;
        for (auto x_bin = 1; x_bin <= target_histogram->GetNbinsX(); x_bin++)
            for (auto y_bin = 1; y_bin <= target_histogram->GetNbinsY(); y_bin++)
            {
                double x_center = target_histogram->GetXaxis()->GetBinCenter(x_bin);
                double y_center = target_histogram->GetYaxis()->GetBinCenter(y_bin);
                double x_width = target_histogram->GetXaxis()->GetBinWidth(x_bin);
                double y_width = target_histogram->GetYaxis()->GetBinWidth(y_bin);
                double z_value = target_histogram->GetBinContent(x_bin, y_bin);
                double z_error = target_histogram->GetBinError(x_bin, y_bin);

                if (z_value <= 0)
                    continue;

                double current_function_value = x_width * y_width * ring_fit_function_xy({x_center, y_center}, parameters);
                chi2 += (z_value - current_function_value) * (z_value - current_function_value) / (z_error * z_error);
            }
        return chi2;
    };

    ROOT::Math::Functor fit_function(chi2_function, 6);
    ROOT::Fit::Fitter fitter;

    double internal_initial_values[6] = {initial_values[0], initial_values[1], initial_values[2], initial_values[3], initial_values[4], initial_values[5]};
    fitter.SetFCN(fit_function, internal_initial_values);
    fitter.Config().ParSettings(0).SetName("centerX");
    fitter.Config().ParSettings(0).SetLimits(-5.0, 5.0);
    fitter.Config().ParSettings(1).SetName("centerY");
    fitter.Config().ParSettings(1).SetLimits(-5.0, 5.0);
    fitter.Config().ParSettings(2).SetName("ring__R");
    fitter.Config().ParSettings(2).SetLowerLimit(0);
    fitter.Config().ParSettings(3).SetName("sigma_R");
    fitter.Config().ParSettings(3).SetLimits(0., 7.5);
    fitter.Config().ParSettings(4).SetName("N_gamma");
    fitter.Config().ParSettings(4).SetLimits(0., 50.);
    fitter.Config().ParSettings(5).SetName("bkg");
    fitter.Config().ParSettings(5).SetLowerLimit(0.);
    if (fix_XY)
    {
        fitter.Config().ParSettings(0).Fix();
        fitter.Config().ParSettings(1).Fix();
    }

    if (!fitter.FitFCN())
    {
        const ROOT::Fit::FitResult &fit_result = fitter.Result();
        Error("fit_ring_integral", "Fit failed");
        fit_result.Print(std::cout);
        return {{{-2., 0.}, {-2., 0.}, {-2., 0.}, {0., 0.}, {0., 0.}, {0., 0.}}};
    }
    const ROOT::Fit::FitResult &fit_result = fitter.Result();
    fit_result.Print(std::cout);

    auto iTer = -1;
    for (auto current_parameter : fit_result.Parameters())
    {
        iTer++;
        //  Debug print removed (it pulled in `<mist/logger/logger.h>` which
        //  broke ROOT's dict autoparse on this header-only file); re-add as
        //  std::cout if needed for local debugging.
        // result is std::array<…, 6> — valid indices 0..5.  The previous
        // guard `if (iTer > 6) continue` would let iTer == 6 fall through and
        // write past the end of result; with `iTer >= (int)result.size()`
        // (== 6) we stop the moment we'd OOB
        if (iTer >= static_cast<int>(result.size()))
            break;
        result[iTer][0] = current_parameter;
        result[iTer][1] = fit_result.Errors()[iTer];
    }

    return result;
}

/**
 * @brief Generate ring-contour TGraphs at specified sigma levels from a fit result.
 * @param fit_results           Result of @ref fit_ring_integral.
 * @param sigma_values          List of sigma multipliers.
 * @param logistic_input_values Optional azimuthal-gap descriptors.
 * @param n_points              Number of points per contour (default: 500).
 * @return                      One owning `unique_ptr<TGraph>` per sigma value.
 *
 * Returning `unique_ptr` makes ownership explicit
 * `TGraph` is not adopted by `gDirectory` like `TH1`, so the previous raw
 * `TGraph *` return leaked per call unless every caller remembered to
 * `delete` afterwards.  Callers that need to hand a `TGraph` over to ROOT
 * (e.g. for `DrawClone`-style ownership transfer) call `.release()`.
 */
inline std::vector<std::unique_ptr<TGraph>> plot_ring_integral(RingFitResults fit_results, std::vector<float> sigma_values, std::vector<std::array<double, 4>> logistic_input_values = {}, int n_points = 500)
{
    std::vector<std::unique_ptr<TGraph>> result;
    result.reserve(sigma_values.size());
    for (auto current_sigma_value : sigma_values)
    {
        auto graph = std::make_unique<TGraph>();
        for (auto i_point = 0; i_point <= n_points; i_point++)
        {
            auto current_phi = -TMath::Pi() + 2 * TMath::Pi() * (1. * i_point) / n_points;
            auto current_x = fit_results[0][0] + (fit_results[2][0] + current_sigma_value * ring_fit_function_sigma_function(current_phi, fit_results[3][0], logistic_input_values)) * cos(current_phi);
            auto current_y = fit_results[1][0] + (fit_results[2][0] + current_sigma_value * ring_fit_function_sigma_function(current_phi, fit_results[3][0], logistic_input_values)) * sin(current_phi);
            graph->SetPoint(i_point, current_x, current_y);
        }
        result.push_back(std::move(graph));
    }
    return result;
}
