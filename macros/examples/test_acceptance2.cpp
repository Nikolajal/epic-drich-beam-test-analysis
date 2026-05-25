#pragma once
#include "../lib_loader.h"

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


// ===== Electronics Line Map =====
//
// Concept: each ALCOR matrix reads an 8×8 block of the 16×16 PDU pixel grid.
// Within that 8×8 block, the do_channel index encodes a (column, row) pair:
//
//   local_column_index = do_channel / 8   (0–7)  → all 8 sensors share the same x → VERTICAL line
//   local_row_index    = do_channel % 8   (0–7)  → all 8 sensors share the same y → HORIZONTAL line
//
// This module recovers, for every (pdu, matrix) pair in the detector, which
// eo_channels (the raw electronics channel index from ALCOR) belong to each
// physical line of 8 adjacent sensors.

enum class line_orientation_type { VERTICAL, HORIZONTAL };

struct electronics_line
{
    int pdu_index;
    int matrix_index;
    int local_line_index;                    // 0–7: column (VERTICAL) or row (HORIZONTAL) within the 8×8 block
    line_orientation_type orientation;
    std::vector<int> eo_channel_list;        // always 8 entries: the eo_channels forming this line
    float physical_constant_coordinate_mm;   // x (VERTICAL) or y (HORIZONTAL) in mm, from representative eo_channel
};

// Build the full list of electronics lines for all (pdu, matrix) pairs
// found in the Mapping. Requires Mapping::get_device_chip_to_pdu_matrix_map()
// to enumerate active (pdu, matrix) combinations.
std::vector<electronics_line> build_electronics_line_list(const Mapping &current_mapping)
{
    std::vector<electronics_line> result;

    // Collect unique (pdu, matrix) pairs from the full device→chip→pdu→matrix table
    std::set<std::array<int, 2>> unique_pdu_matrix_set;
    for (auto const &[device_chip_key, pdu_matrix_value] : current_mapping.get_device_chip_to_pdu_matrix_map())
        unique_pdu_matrix_set.insert(pdu_matrix_value);

    for (auto const &pdu_matrix_pair : unique_pdu_matrix_set)
    {
        int current_pdu_index    = pdu_matrix_pair[0];
        int current_matrix_index = pdu_matrix_pair[1];

        // For this (pdu, matrix), group all 64 eo_channels by their local column and row
        std::array<std::vector<int>, 8> column_index_to_eo_channels;
        std::array<std::vector<int>, 8> row_index_to_eo_channels;

        for (int eo_channel = 0; eo_channel < 64; ++eo_channel)
        {
            auto do_channel_optional = current_mapping.get_do_channel(current_matrix_index, eo_channel);
            if (!do_channel_optional)
                continue;

            int do_channel         = *do_channel_optional;
            int local_column_index = do_channel / 8;  // which of the 8 vertical lines
            int local_row_index    = do_channel % 8;  // which of the 8 horizontal lines

            column_index_to_eo_channels[local_column_index].push_back(eo_channel);
            row_index_to_eo_channels[local_row_index].push_back(eo_channel);
        }

        // --- VERTICAL lines (same local column → same x coordinate) ---
        for (int local_column_index = 0; local_column_index < 8; ++local_column_index)
        {
            electronics_line current_line;
            current_line.pdu_index        = current_pdu_index;
            current_line.matrix_index     = current_matrix_index;
            current_line.local_line_index = local_column_index;
            current_line.orientation      = line_orientation_type::Vertical;
            current_line.eo_channel_list  = column_index_to_eo_channels[local_column_index];

            // Physical x: query via representative eo_channel (all 8 share the same x)
            current_line.physical_constant_coordinate_mm = -999.f;
            if (!current_line.eo_channel_list.empty())
            {
                int representative_eo_channel = current_line.eo_channel_list[0];
                auto representative_position  = current_mapping.get_position_from_pdu_matrix_eoch(
                    current_pdu_index, current_matrix_index, representative_eo_channel);
                if (representative_position)
                    current_line.physical_constant_coordinate_mm = (*representative_position)[0];
            }

            result.push_back(std::move(current_line));
        }

        // --- HORIZONTAL lines (same local row → same y coordinate) ---
        for (int local_row_index = 0; local_row_index < 8; ++local_row_index)
        {
            electronics_line current_line;
            current_line.pdu_index        = current_pdu_index;
            current_line.matrix_index     = current_matrix_index;
            current_line.local_line_index = local_row_index;
            current_line.orientation      = line_orientation_type::Horizontal;
            current_line.eo_channel_list  = row_index_to_eo_channels[local_row_index];

            // Physical y: query via representative eo_channel (all 8 share the same y)
            current_line.physical_constant_coordinate_mm = -999.f;
            if (!current_line.eo_channel_list.empty())
            {
                int representative_eo_channel = current_line.eo_channel_list[0];
                auto representative_position  = current_mapping.get_position_from_pdu_matrix_eoch(
                    current_pdu_index, current_matrix_index, representative_eo_channel);
                if (representative_position)
                    current_line.physical_constant_coordinate_mm = (*representative_position)[1];
            }

            result.push_back(std::move(current_line));
        }
    }

    return result;
}

void print_electronics_line_list(const std::vector<electronics_line> &electronics_line_list)
{
    std::cout << "\n=== Electronics Line Map ===" << std::endl;
    std::cout << std::left
              << std::setw(6)  << "PDU"
              << std::setw(8)  << "Matrix"
              << std::setw(12) << "Orient."
              << std::setw(10) << "LineIdx"
              << std::setw(14) << "Coord[mm]"
              << "eo_channels (0-63)"
              << std::endl;
    std::cout << std::string(90, '-') << std::endl;

    for (const auto &current_line : electronics_line_list)
    {
        std::string orientation_label =
            (current_line.orientation == line_orientation_type::Vertical) ? "VERTICAL" : "HORIZONTAL";

        std::cout << std::left
                  << std::setw(6)  << current_line.pdu_index
                  << std::setw(8)  << current_line.matrix_index
                  << std::setw(12) << orientation_label
                  << std::setw(10) << current_line.local_line_index
                  << std::setw(14) << std::fixed << std::setprecision(2)
                  << current_line.physical_constant_coordinate_mm;

        for (int eo_channel_value : current_line.eo_channel_list)
            std::cout << eo_channel_value << " ";
        std::cout << std::endl;
    }

    std::cout << "\nTotal lines: " << electronics_line_list.size()
              << "  (expect 16 per active (pdu, matrix) pair)" << std::endl;
}

void test_acceptance2()
{
    Mapping current_mapping("conf/mapping_conf.toml");

    // Inspect electronics line structure before running acceptance
    auto electronics_line_list = build_electronics_line_list(current_mapping);
    print_electronics_line_list(electronics_line_list);

}