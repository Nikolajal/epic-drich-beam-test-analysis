#pragma once

/**
 * @file util/circle_fit.h
 * @brief Least-squares circle fit minimising radial residuals.
 *
 * Wraps `ROOT::Fit::Fitter` with a chi² that penalises the radial distance
 * of each point from the candidate ring.  Optional `fix_XY` fixes the centre
 * and varies only the radius; `exclude_points` skips specific input indices.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <Math/Functor.h>
#include <Fit/Fitter.h>

/// Result type: { {x0,σx0}, {y0,σy0}, {R,σR} }.
using CircleFitResults = std::array<std::array<float, 2>, 3>;

/**
 * @brief Fit a circle to a set of 2-D points.
 *
 * Minimises the sum of squared radial residuals using ROOT's @c Fitter.
 *
 * @param points           Input points {x, y} [mm].
 * @param initial_values   Initial guess {x0, y0, R}.
 * @param fix_XY           If @c true, fix the centre and fit only R (default: true).
 * @param exclude_points   Indices of points to exclude from the fit (default: empty).
 * @return                 Fit result with central values and uncertainties.
 *
 * @note Error handling, default initial values, and uncertainty propagation
 *       in this routine are pending a focused review — track via the
 *       `fit_circle` GitHub issue (label: `design`) before relying on this
 *       function for new physics analyses.
 */
inline CircleFitResults fit_circle(std::vector<std::array<float, 2>> points,
                                     std::array<float, 3> initial_values,
                                     bool fix_XY = true,
                                     std::vector<int> exclude_points = {{}})
{
    CircleFitResults result;

    //  Chi2 minimisation for points in a circle
    auto chi2_function = [&](const double *parameters)
    {
        float chi2 = 0;
        for (int iPnt = 0; iPnt < points.size(); iPnt++)
        {
            if (std::find(exclude_points.begin(), exclude_points.end(), iPnt) != exclude_points.end())
                continue;
            double delta_x = points[iPnt][0] - parameters[0];
            double delta_y = points[iPnt][1] - parameters[1];
            double delta_r = parameters[2] - std::sqrt(delta_x * delta_x + delta_y * delta_y);
            chi2 += delta_r * delta_r;
        }
        return chi2;
    };

    // wrap chi2 function in a function object for the fit
    ROOT::Math::Functor fit_function(chi2_function, 3);
    ROOT::Fit::Fitter fitter;

    //  Set initial values and variables names
    double internal_initial_values[3] = {initial_values[0], initial_values[1], initial_values[2]};
    fitter.SetFCN(fit_function, internal_initial_values);
    fitter.Config().ParSettings(0).SetName("x0");
    fitter.Config().ParSettings(1).SetName("y0");
    fitter.Config().ParSettings(2).SetName("R");
    fitter.Config().ParSettings(2).SetLowerLimit(0);
    if (fix_XY)
    {
        fitter.Config().ParSettings(0).Fix();
        fitter.Config().ParSettings(1).Fix();
    }

    //  Fitting
    if (!fitter.FitFCN())
    {
        // Error("fit_circle", "Fit failed");
        //  return {{{-2., 0.}, {-2., 0.}, {-2., 0.}}};
    }
    const ROOT::Fit::FitResult &fit_result = fitter.Result();

    auto iTer = -1;
    for (auto current_parameter : fit_result.Parameters())
    {
        iTer++;
        result[iTer][0] = current_parameter;
        result[iTer][1] = fit_result.Errors()[iTer];
    }

    //  Calculate chi2
    double test[3] = {result[0][0], result[1][0], result[2][0]};
    auto myChi2 = chi2_function(test);
    (void)myChi2;

    return result;
}
