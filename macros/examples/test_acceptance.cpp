#include "../lib_loader.h"
#include "utility/root_hist.h"

// ===== Geometry Configuration =====
// A geometry_config struct encapsulates all detector-specific parameters
// Concept: each geometry defines its sensor layout and acceptance calculation bounds

struct geometry_config
{
    std::string name;                                      // Identifier (e.g., "strettoia", "default")
    std::vector<std::pair<double, double>> sensor_centers; // (x, y) in mm
    double sensor_half_width;                              // Half-width of square sensor pixel (mm)
    double ring_nominal_radius;                            // R0: nominal Cherenkov ring radius (mm)
    double radial_sigma;                                   // sigma_r: intrinsic radial resolution (mm)
    double plot_xy_max;                                    // Plot range (mm)
};

// ===== Geometry Definitions =====

geometry_config create_default_geometry(const Mapping &current_mapping)
{
    geometry_config geom;
    geom.name = "default";
    geom.sensor_half_width = 1.5;
    geom.ring_nominal_radius = 47.0;
    geom.radial_sigma = 2.1;

    // Load sensor centers from Mapping (skip center ~5 mm × 5 mm dead zone)
    for (auto i_index = 0; i_index < 2048 * 4; i_index += 4)
    {
        auto position = current_mapping.get_position_from_global_index(i_index);
        if (!position)
            continue;
        if (fabs((*position)[0]) < 5 && fabs((*position)[1]) < 5)
            continue;
        geom.sensor_centers.push_back((*position));
    }

    // Determine plot range from sensor positions
    geom.plot_xy_max = 0.0;
    for (const auto &[sensor_center_x, sensor_center_y] : geom.sensor_centers)
        geom.plot_xy_max = std::max(geom.plot_xy_max, std::max(fabs(sensor_center_x), fabs(sensor_center_y)));
    geom.plot_xy_max += 10.0;

    return geom;
}

geometry_config create_strettoia_geometry()
{
    // MAPMT Strettoia geometry: 4 SQUARE, SYMMETRIC quadrants with equal gaps
    // Measurements from diagram:
    //   - Total X span: 102.25 mm
    //   - Total Y span: 84.5 mm (note: discrepancy — may indicate measurement to different boundary)
    //   - Active area per quadrant: 48.50 mm (confirmed from X-span analysis)
    //   - Offset to y-axis: 2.62 mm (half-gap, from left quad to center)
    //   - Pixel pitch: 3.00 mm
    //
    // Geometry (assuming square quads, derived from X-span):
    //   - Quad size: 48.50 × 48.50 mm (SQUARE)
    //   - Horizontal gap: 5.24 mm (= 2 × 2.62 mm)
    //   - Quadrant centers (symmetric): (±26.87, ±26.87) mm
    //   - Active pixel extents: ±51.12 mm in both X and Y
    //
    // Note: The Y-span discrepancy (84.5 mm measured vs 102.24 mm calculated)
    // may indicate that Y is measured to a different mechanical boundary.
    // The pixel geometry itself is derived from the X-span and square aspect ratio.

    geometry_config geom;
    geom.name = "strettoia";
    geom.sensor_half_width = 1.5; // Half pitch: 3.00 mm / 2
    geom.ring_nominal_radius = 47.0;
    geom.radial_sigma = 2.1;

    const double quad_size = 48.50;                // Side length of square quadrant (mm)
    const double pitch = 3.00;                     // Pixel pitch (mm)
    const double quad_half_size = quad_size / 2.0; // 24.25 mm

    // Number of pixels per side: 48.50 / 3.00 ≈ 16.17 → 16 pixels per side
    const int pixels_per_side = static_cast<int>(quad_size / pitch + 0.5);

    // Quadrant centers (symmetric, derived from gap analysis)
    // From "to y axis: 2.62 mm" and total span 102.25 mm:
    //   left_quad_center_x = -(quad_size/2 + 2.62 + quad_size/2) / 2 = -(48.50 + 2.62) / 2 = -26.87 mm
    //   By symmetry: centers at (±26.87, ±26.87) mm

    const double quad_center_offset = (quad_size + 5.24) / 2.0; // 26.87 mm
                                                                // = 48.50/2 + 2.62 = 24.25 + 2.62

    auto add_quadrant = [&](double quad_center_x, double quad_center_y, const std::string &quad_name)
    {
        for (int ix = 0; ix < pixels_per_side; ix++)
        {
            for (int iy = 0; iy < pixels_per_side; iy++)
            {
                double local_x = -quad_half_size + (ix + 0.5) * pitch;
                double local_y = -quad_half_size + (iy + 0.5) * pitch;
                double global_x = quad_center_x + local_x;
                double global_y = quad_center_y + local_y;
                geom.sensor_centers.push_back({global_x, global_y});
            }
        }
    };

    // Add quadrants with symmetric centers
    add_quadrant(-quad_center_offset, +quad_center_offset, "NORTH");
    add_quadrant(+quad_center_offset, +quad_center_offset, "EAST");
    add_quadrant(-quad_center_offset, -quad_center_offset, "SOUTH");
    add_quadrant(+quad_center_offset, -quad_center_offset, "WEST");

    // Determine plot range
    geom.plot_xy_max = 0.0;
    for (const auto &[sensor_center_x, sensor_center_y] : geom.sensor_centers)
        geom.plot_xy_max = std::max(geom.plot_xy_max, std::max(fabs(sensor_center_x), fabs(sensor_center_y)));
    geom.plot_xy_max += 10.0;

    return geom;
}

geometry_config create_standard_geometry()
{
    // MAPMT Standard geometry: 4 quadrants in overlapping CROSS pattern
    // Measurements from diagram:
    //   - Active area per quadrant: 48.50 mm (square)
    //   - Quadrant centers: (0, ±30.90) and (±30.90, 0) mm
    //   - Half-quadrant extent: 24.25 mm
    //   - Outer edges: ±30.90 + 24.25 = ±55.15 mm
    //   - Inner edges: ±30.90 - 24.25 = ±6.65 mm
    //   - Central overlap region: ±6.65 mm (13.30 × 13.30 mm square)
    //     where all 4 quadrants provide simultaneous coverage

    geometry_config geom;
    geom.name = "standard";
    geom.sensor_half_width = 1.5; // Half pitch: 3.00 mm / 2
    geom.ring_nominal_radius = 47.0;
    geom.radial_sigma = 2.1;

    const double quad_size = 48.50;                // Active area (mm)
    const double pitch = 3.00;                     // Pixel pitch (mm)
    const double quad_half_size = quad_size / 2.0; // 24.25 mm

    const int pixels_per_side = static_cast<int>(quad_size / pitch + 0.5); // 16 pixels

    // Quadrant centers (overlapping CROSS pattern)
    // All quads are equidistant from origin, creating a + shape with central overlap
    const double quad_center_offset = 30.90 + quad_half_size; // Distance from origin to quad center (mm)

    auto add_quadrant = [&](double quad_center_x, double quad_center_y, const std::string &quad_name)
    {
        for (int ix = 0; ix < pixels_per_side; ix++)
        {
            for (int iy = 0; iy < pixels_per_side; iy++)
            {
                double local_x = -quad_half_size + (ix + 0.5) * pitch;
                double local_y = -quad_half_size + (iy + 0.5) * pitch;
                double global_x = quad_center_x + local_x;
                double global_y = quad_center_y + local_y;
                geom.sensor_centers.push_back({global_x, global_y});
            }
        }
    };

    // Add quadrants in cross pattern (NORTH/SOUTH on y-axis, EAST/WEST on x-axis)
    add_quadrant(0.0, +quad_center_offset, "NORTH");
    add_quadrant(0.0, -quad_center_offset, "SOUTH");
    add_quadrant(+quad_center_offset, 0.0, "EAST");
    add_quadrant(-quad_center_offset, 0.0, "WEST");

    // Determine plot range
    geom.plot_xy_max = 0.0;
    for (const auto &[sensor_center_x, sensor_center_y] : geom.sensor_centers)
        geom.plot_xy_max = std::max(geom.plot_xy_max, std::max(fabs(sensor_center_x), fabs(sensor_center_y)));
    geom.plot_xy_max += 10.0;

    return geom;
}

geometry_config create_losanga_geometry()
{
    // MAPMT Losanga (Diamond/Legacy) geometry: 4 quadrants in DIAMOND pattern
    // Measurements from diagram:
    //   - Active area per quadrant: 48.50 mm
    //   - Edge pixel: varies (approximately 3.25 mm)
    //   - Inner pixel: 3.00 mm (pitch)
    //   - Total X span: 157.00 mm (square span for diamond)
    //   - Total Y span: 157.00 mm (square span for diamond)
    //
    // Layout (DIAMOND pattern, rotated square):
    //   - NORTH: top (0, +54.5)
    //   - SOUTH: bottom (0, -54.5)
    //   - EAST: right (+54.5, 0)
    //   - WEST: left (-54.5, 0)
    //   - Quadrants are equidistant from origin along cardinal axes

    geometry_config geom;
    geom.name = "losanga";
    geom.sensor_half_width = 1.5; // Half pitch: 3.00 mm / 2
    geom.ring_nominal_radius = 47.0;
    geom.radial_sigma = 2.1;

    const double quad_size = 48.50;                // Active area (mm)
    const double pitch = 3.00;                     // Pixel pitch (mm)
    const double quad_half_size = quad_size / 2.0; // 24.25 mm

    const int pixels_per_side = static_cast<int>(quad_size / pitch + 0.5); // 16 pixels

    // Quadrant centers (diamond pattern, symmetric about origin)
    // From 157 mm span: quad centers spaced at ~54.5 mm from origin
    const double quad_center_offset = 54.5; // Distance from origin to quad center (mm)

    auto add_quadrant = [&](double quad_center_x, double quad_center_y, const std::string &quad_name)
    {
        for (int ix = 0; ix < pixels_per_side; ix++)
        {
            for (int iy = 0; iy < pixels_per_side; iy++)
            {
                double local_x = -quad_half_size + (ix + 0.5) * pitch;
                double local_y = -quad_half_size + (iy + 0.5) * pitch;
                double global_x = quad_center_x + local_x;
                double global_y = quad_center_y + local_y;
                geom.sensor_centers.push_back({global_x, global_y});
            }
        }
    };

    // Add quadrants in diamond pattern
    add_quadrant(0.0, +quad_center_offset, "NORTH");
    add_quadrant(0.0, -quad_center_offset, "SOUTH");
    add_quadrant(+quad_center_offset, 0.0, "EAST");
    add_quadrant(-quad_center_offset, 0.0, "WEST");

    // Determine plot range
    geom.plot_xy_max = 0.0;
    for (const auto &[sensor_center_x, sensor_center_y] : geom.sensor_centers)
        geom.plot_xy_max = std::max(geom.plot_xy_max, std::max(fabs(sensor_center_x), fabs(sensor_center_y)));
    geom.plot_xy_max += 10.0;

    return geom;
}

// ===== Main Acceptance Calculation =====

void test_acceptance(const geometry_config &geom, bool draw_plots = true)
{
    std::cout << "\n=== Calculating acceptance for geometry: " << geom.name << " ===" << std::endl;
    std::cout << "Number of sensor centers: " << geom.sensor_centers.size() << std::endl;
    std::cout << "Ring radius R0: " << geom.ring_nominal_radius << " mm, sigma_r: "
              << geom.radial_sigma << " mm" << std::endl;

    // --- Control plot 1: sensor center map ---
    TGraph *sensor_center_graph = new TGraph();
    sensor_center_graph->SetTitle(TString::Format("%s: Sensor centers;x [mm];y [mm]", geom.name.c_str()).Data());
    int sensor_graph_point_index = 0;
    for (const auto &[sensor_center_x, sensor_center_y] : geom.sensor_centers)
        sensor_center_graph->SetPoint(sensor_graph_point_index++, sensor_center_x, sensor_center_y);

    // Overlay the nominal ring for reference
    const int ring_draw_steps = 500;
    TGraph *nominal_ring_graph = new TGraph();
    nominal_ring_graph->SetTitle("Nominal ring");
    for (int ring_step = 0; ring_step <= ring_draw_steps; ring_step++)
    {
        double ring_phi = 2.0 * M_PI * ring_step / ring_draw_steps;
        nominal_ring_graph->SetPoint(ring_step,
                                     geom.ring_nominal_radius * std::cos(ring_phi),
                                     geom.ring_nominal_radius * std::sin(ring_phi));
    }

    // --- Control plot 2: Gaussian weight map ---
    const int weight_map_bins = 300;
    RootHist<TH2D> gaussian_weight_map_histogram(
        TString::Format("gaussian_weight_map_%s", geom.name.c_str()).Data(),
        TString::Format("%s: Gaussian weight map (G(r)#times#DeltaA);x [mm];y [mm]", geom.name.c_str()).Data(),
        weight_map_bins, -geom.plot_xy_max, geom.plot_xy_max,
        weight_map_bins, -geom.plot_xy_max, geom.plot_xy_max);

    // --- Control plot 3: radial profile of accumulated Gaussian weight ---
    const int radial_profile_bins = 200;
    RootHist<TH1D> radial_weight_profile_histogram(
        TString::Format("radial_weight_profile_%s", geom.name.c_str()).Data(),
        TString::Format("%s: Radial weight profile;r [mm];#sum G(r)#times#DeltaA", geom.name.c_str()).Data(),
        radial_profile_bins, 0.0, geom.plot_xy_max);

    // --- Main integration loop ---
    double numerator_sum = 0.0;
    const int grid_steps_per_tile = 100;
    const double tile_area = std::pow(geom.sensor_half_width * 2.0 / grid_steps_per_tile, 2);

    for (const auto &[sensor_center_x, sensor_center_y] : geom.sensor_centers)
    {
        double tile_x_min = sensor_center_x - geom.sensor_half_width;
        double tile_y_min = sensor_center_y - geom.sensor_half_width;
        double tile_step_size = (2.0 * geom.sensor_half_width) / grid_steps_per_tile;

        for (int ix = 0; ix < grid_steps_per_tile; ix++)
        {
            double grid_x = tile_x_min + (ix + 0.5) * tile_step_size;
            for (int iy = 0; iy < grid_steps_per_tile; iy++)
            {
                double grid_y = tile_y_min + (iy + 0.5) * tile_step_size;
                double pixel_radius = std::sqrt(grid_x * grid_x + grid_y * grid_y);
                double radial_delta = pixel_radius - geom.ring_nominal_radius;
                double gaussian_weight = std::exp(-0.5 * (radial_delta * radial_delta) /
                                                  (geom.radial_sigma * geom.radial_sigma));
                double weighted_pixel_area = gaussian_weight * tile_area;

                numerator_sum += weighted_pixel_area;
                gaussian_weight_map_histogram->Fill(grid_x, grid_y, weighted_pixel_area);
                radial_weight_profile_histogram->Fill(pixel_radius, weighted_pixel_area);
            }
        }
    }

    double denominator = 2.0 * M_PI * geom.ring_nominal_radius * geom.radial_sigma * std::sqrt(2.0 * M_PI);
    double acceptance_fraction = numerator_sum / denominator;

    std::cout << "Numerator (weighted area): " << numerator_sum << std::endl;
    std::cout << "Denominator (2π R0 σr √(2π)): " << denominator << std::endl;
    std::cout << "**Acceptance fraction: " << std::setprecision(4) << acceptance_fraction << "**" << std::endl;

    if (!draw_plots)
        return;

    // --- Drawing ---
    TCanvas *sensor_map_canvas = new TCanvas(
        TString::Format("sensor_map_canvas_%s", geom.name.c_str()).Data(),
        TString::Format("Sensor map — %s", geom.name.c_str()).Data(), 700, 700);
    sensor_map_canvas->SetGrid();
    sensor_center_graph->SetMarkerStyle(6);
    sensor_center_graph->SetMarkerColor(kBlue + 1);
    sensor_center_graph->Draw("AP");
    nominal_ring_graph->SetLineColor(kRed);
    nominal_ring_graph->SetLineWidth(2);
    nominal_ring_graph->Draw("L same");
    TLatex *acceptance_label = new TLatex();
    acceptance_label->SetNDC();
    acceptance_label->SetTextSize(0.03);
    acceptance_label->DrawLatex(0.15, 0.92, TString::Format("Acceptance: %.4f", acceptance_fraction).Data());

    TCanvas *weight_map_canvas = new TCanvas(
        TString::Format("weight_map_canvas_%s", geom.name.c_str()).Data(),
        TString::Format("Gaussian weight map — %s", geom.name.c_str()).Data(), 700, 700);
    weight_map_canvas->SetGrid();
    gaussian_weight_map_histogram->SetStats(false);
    gaussian_weight_map_histogram->Draw("COLZ");
    nominal_ring_graph->Draw("L same");
    acceptance_label->DrawLatex(0.15, 0.92, TString::Format("Acceptance: %.4f", acceptance_fraction).Data());

    TCanvas *radial_profile_canvas = new TCanvas(
        TString::Format("radial_profile_canvas_%s", geom.name.c_str()).Data(),
        TString::Format("Radial weight profile — %s", geom.name.c_str()).Data(), 800, 500);
    radial_profile_canvas->SetGrid();
    radial_weight_profile_histogram->SetLineColor(kBlue + 1);
    radial_weight_profile_histogram->SetLineWidth(2);
    radial_weight_profile_histogram->SetStats(false);
    radial_weight_profile_histogram->Draw("HIST");

    // Mark R0 and R0 ± sigma_r with vertical lines
    TLine *ring_center_line = new TLine(geom.ring_nominal_radius, 0,
                                        geom.ring_nominal_radius, radial_weight_profile_histogram->GetMaximum());
    ring_center_line->SetLineColor(kRed);
    ring_center_line->SetLineWidth(2);
    ring_center_line->SetLineStyle(1);
    ring_center_line->Draw();

    TLine *ring_sigma_minus_line = new TLine(geom.ring_nominal_radius - geom.radial_sigma, 0,
                                             geom.ring_nominal_radius - geom.radial_sigma,
                                             radial_weight_profile_histogram->GetMaximum());
    TLine *ring_sigma_plus_line = new TLine(geom.ring_nominal_radius + geom.radial_sigma, 0,
                                            geom.ring_nominal_radius + geom.radial_sigma,
                                            radial_weight_profile_histogram->GetMaximum());
    ring_sigma_minus_line->SetLineColor(kRed);
    ring_sigma_minus_line->SetLineStyle(2);
    ring_sigma_minus_line->Draw();
    ring_sigma_plus_line->SetLineColor(kRed);
    ring_sigma_plus_line->SetLineStyle(2);
    ring_sigma_plus_line->Draw();

    TLegend *radial_profile_legend = new TLegend(0.65, 0.75, 0.88, 0.88);
    radial_profile_legend->AddEntry(ring_center_line,
                                    TString::Format("R_{0} = %.1f mm", geom.ring_nominal_radius).Data(), "l");
    radial_profile_legend->AddEntry(ring_sigma_minus_line,
                                    TString::Format("#sigma_{r} = %.1f mm", geom.radial_sigma).Data(), "l");
    radial_profile_legend->Draw();
}

// ===== Wrapper to test multiple geometries =====

void test_all_geometries(const Mapping &current_mapping)
{
    // Create and test all available geometries
    geometry_config default_geom = create_default_geometry(current_mapping);
    test_acceptance(default_geom, true);

    geometry_config strettoia_geom = create_strettoia_geometry();
    test_acceptance(strettoia_geom, true);

    geometry_config standard_geom = create_standard_geometry();
    test_acceptance(standard_geom, true);

    geometry_config losanga_geom = create_losanga_geometry();
    test_acceptance(losanga_geom, true);
}

// ===== Main entry point for single geometry =====

void test_acceptance()
{
    Mapping current_mapping("conf/mapping_conf.toml");

    // Choose geometry by uncommenting one of the following:

    //geometry_config geom = create_default_geometry(current_mapping);
    geometry_config geom = create_strettoia_geometry();
    // geometry_config geom = create_standard_geometry();
    // geometry_config geom = create_losanga_geometry();

    geom.ring_nominal_radius = 45.0;
    geom.radial_sigma = 2.1;

    test_acceptance(geom, true);
}