#include "mapping.h"
#include <mist/logger/logger.h>

// ============================================================================
//  Constructors
// ============================================================================

Mapping::Mapping(std::string conf_file_name) { load_calib(conf_file_name); }

// ============================================================================
//  Getters — non-trivial implementations (one-liners live in the header)
// ============================================================================

std::optional<int> Mapping::get_do_channel(int matrix, int eo_channel) const
{
    auto it = matrix_to_do_channel.find(matrix);
    if (it == matrix_to_do_channel.end())
        return std::nullopt;
    if (eo_channel < 0 || eo_channel >= static_cast<int>(it->second.size()))
        return std::nullopt;
    return it->second[eo_channel];
}

std::optional<std::array<int, 2>> Mapping::get_pdu_matrix(int device, int chip) const
{
    auto it = device_chip_to_pdu_matrix.find({device, chip});
    if (it == device_chip_to_pdu_matrix.end())
        return std::nullopt;
    return it->second;
}

std::optional<std::array<float, 2>> Mapping::get_position_from_pdu_column_row(int pdu, int column, int row) const
{
    if (pdu < 0 || column < 0 || row < 0)
        return std::nullopt;

    //  Reject PDUs that aren't in the geometry table — covers PDU 99
    //  (the sentinel for non-Cherenkov / timing devices defined in
    //  `mapping_conf.toml`) and any other unregistered PDU index.
    //  Previously this branch silently fell through, returning the
    //  bare intra-PDU offset (no PDU origin added) — i.e. positions
    //  in the range (1.85 to 37.45) × (1.85 to 50.45) mm that look
    //  geometrically valid but correspond to no real Cherenkov
    //  channel.  Those phantom positions populated `index_to_hit_xy`
    //  for the timing devices, producing a "low-R bump" in the
    //  recodata-side coverage map and any other
    //  geometry-derived product that iterates the map.
    auto it = pdu_xy_position.find(pdu);
    if (it == pdu_xy_position.end())
        return std::nullopt;

    //  Intra-PDU coordinate geometry [mm].  The first-pixel-centre origin
    //  offset is the guard-ring + edge stack (0.05 + 0.1 + 0.2) plus the
    //  pixel half-width; pixels then step by the SiPM pitch, with an extra
    //  inter-half gap once past the PDU half boundary.  (1.5 mm half-width
    //  duplicates RecodataConfigStruct::channel_half_width_mm by design —
    //  same physical pixel, different consumer.)
    constexpr float kGuardRingOffsetMm = 0.05f + 0.1f + 0.2f;
    constexpr float kPixelHalfWidthMm = 1.5f;
    constexpr float kPduPitchMm = 3.2f;
    constexpr float kInterHalfGapMm = 0.3f;
    constexpr int kPduHalfBoundary = 7; // last column/row before the half gap
    float x = kGuardRingOffsetMm + kPixelHalfWidthMm + kPduPitchMm * column +
              (column > kPduHalfBoundary ? kInterHalfGapMm : 0.f);
    float y = kGuardRingOffsetMm + kPixelHalfWidthMm + kPduPitchMm * row +
              (row > kPduHalfBoundary ? kInterHalfGapMm : 0.f);

    //  Add PDU origin (guaranteed found by the early return above).
    x += it->second[0];
    y += it->second[1];

    return std::array<float, 2>{x, y};
}

std::optional<std::array<float, 2>> Mapping::get_position_from_pdu_matrix_eoch(int pdu, int matrix, int eo_channel) const
{
    auto do_channel_optional = get_do_channel(matrix, eo_channel);
    if (!do_channel_optional)
        return std::nullopt;

    int do_channel = *do_channel_optional;
    int column = do_channel / 8 + (matrix > 2 ? 8 : 0);
    int row = do_channel % 8 + (matrix % 2 == 0 ? 8 : 0);

    //  Apply optional 180° PDU rotation.  Use .find() rather than the
    //  unchecked operator[] — a missing PDU should not silently insert
    //  a default-false entry into the static map, which would mask a
    //  malformed mapping config (and is a write to shared state).
    const auto rotation_it = pdu_rotation.find(pdu);
    const bool rotated = (rotation_it != pdu_rotation.end()) ? rotation_it->second : false;
    if (rotated)
        return get_position_from_pdu_column_row(pdu, 15 - column, 15 - row);
    return get_position_from_pdu_column_row(pdu, column, row);
}

std::optional<std::array<float, 2>> Mapping::get_position_from_device_chip_eoch(int device, int chip, int eo_channel) const
{
    auto it = device_chip_to_pdu_matrix.find({device, chip});
    if (it == device_chip_to_pdu_matrix.end())
        return std::nullopt;

    int pdu_index = it->second[0];
    int matrix_index = it->second[1];
    return get_position_from_pdu_matrix_eoch(pdu_index, matrix_index, eo_channel);
}

void Mapping::assign_position(AlcorFinedataStruct &entry)
{
    // entry.GlobalIndex holds the packed raw; pass it through the
    // value-type overload explicitly rather than relying on the int
    // compat shim.
    auto current_position = get_position_from_global_index(::GlobalIndex(entry.GlobalIndex));
    if (!current_position)
    {
        entry.hit_x = -999.f;
        entry.hit_y = -999.f;
        return;
    }
    entry.hit_x = (*current_position)[0];
    entry.hit_y = (*current_position)[1];
}

// ============================================================================
//  HV bias-line identification
// ============================================================================

std::optional<line_orientation_type> Mapping::get_hv_line_orientation(int matrix_index) const
{
    auto it = hv_line_orientation.find(matrix_index);
    if (it == hv_line_orientation.end())
        return std::nullopt;
    return it->second;
}

std::optional<HvChannelAddress> Mapping::get_hv_channel_from_global_index(int stored_raw) const
{
    // Storage is the packed @ref GlobalIndex raw — direct construction.
    const auto gi = ::GlobalIndex(static_cast<uint32_t>(stored_raw));
    const int device = gi.device();
    const int chip = gi.real_chip();
    const int eo_channel = gi.eo_channel();

    auto pdu_matrix_optional = get_pdu_matrix(device, chip);
    if (!pdu_matrix_optional)
        return std::nullopt;
    int pdu_index = (*pdu_matrix_optional)[0];
    int matrix_index = (*pdu_matrix_optional)[1];

    auto do_channel_optional = get_do_channel(matrix_index, eo_channel);
    if (!do_channel_optional)
        return std::nullopt;
    int do_channel = *do_channel_optional;

    auto orientation_optional = get_hv_line_orientation(matrix_index);
    if (!orientation_optional)
        return std::nullopt;
    line_orientation_type current_orientation = *orientation_optional;

    //  HV line index is derived from the pre-rotation do_channel — the physical
    //  bias wire layout is independent of the software coordinate rotation.
    int hv_line_index = (current_orientation == line_orientation_type::Vertical)
                            ? do_channel / 8
                            : do_channel % 8;

    return HvChannelAddress{pdu_index, matrix_index, hv_line_index, current_orientation};
}

// ============================================================================
//  Calibration I/O
// ============================================================================

void Mapping::load_calib(std::string filename, bool verbose)
{
    //  Invalidate all caches on reload
    index_to_hit_xy.clear();
    hit_xy_to_index.clear();
    cache_index_to_xy_built = false;
    cache_xy_to_index_built = false;

    auto loaded_tables = toml_parse_with_cutoff(filename);

    // --- pdu_xy_position --------------------------------------------------------
    if (auto placement_table = loaded_tables["pdu_xy_position"].as_table())
    {
        mist::logger::info("(Mapping::load_calib) Found pdu_xy_position table, loading contents");
        pdu_xy_position.clear();
        for (auto &[key, val] : *placement_table)
        {
            if (auto arr = val.as_array(); arr && arr->size() == 2)
                pdu_xy_position[std::stoi(std::string(key))] = {
                    static_cast<float>((*arr)[0].value_or(0.0)),
                    static_cast<float>((*arr)[1].value_or(0.0))};
        }
        mist::logger::info(TString::Format("(Mapping::load_calib) pdu_xy_position size: %zu",
                                           pdu_xy_position.size())
                               .Data());
    }

    // --- pdu_rotation -----------------------------------------------------------
    //  Semantics (C2.5): a PDU absent from the map is treated as not rotated
    //  by `get_position_from_pdu_matrix_eoch` (silent — that's the design).
    //  A PDU present but with a non-bool value used to silently default to
    //  `true` via `value_or(true)`, which masks a malformed mapping config.
    //  Now: explicit `value<bool>()` probe — on type mismatch, log a warning
    //  and leave the PDU out of the map (so it falls through to the silent
    //  "missing → not rotated" branch).
    if (auto rotation_table = loaded_tables["pdu_rotation"].as_table())
    {
        mist::logger::info("(Mapping::load_calib) Found pdu_rotation table, loading contents");
        pdu_rotation.clear();
        for (auto &[key, val] : *rotation_table)
        {
            int pdu_index = std::stoi(std::string(key));
            if (pdu_index < 1 || pdu_index > 8)
                continue; //  out-of-range PDU — silently skip (legacy behaviour)

            const auto rotation_flag_opt = val.value<bool>();
            if (!rotation_flag_opt)
            {
                mist::logger::warning(
                    TString::Format("(Mapping::load_calib) pdu_rotation[%d] is not a bool "
                                    "(TOML type mismatch) — treating PDU %d as NOT rotated.",
                                    pdu_index, pdu_index)
                        .Data());
                continue; //  malformed → fall through to "missing → false" at lookup time
            }
            pdu_rotation[pdu_index] = *rotation_flag_opt;
        }
        mist::logger::info(TString::Format("(Mapping::load_calib) pdu_rotation size: %zu",
                                           pdu_rotation.size())
                               .Data());
    }

    // --- device_chip_to_pdu_matrix ----------------------------------------------
    if (auto device_chip_table = loaded_tables["device_chip_to_pdu_matrix"].as_table())
    {
        mist::logger::info("(Mapping::load_calib) Found device_chip_to_pdu_matrix table, loading contents");
        device_chip_to_pdu_matrix.clear();
        for (auto &[key, val] : *device_chip_table)
        {
            std::string key_string = std::string(key);
            auto separator = key_string.find('_');
            if (separator == std::string::npos)
            {
                mist::logger::info(TString::Format("(Mapping::load_calib) Skipping invalid key: %s",
                                                   key_string.c_str())
                                       .Data());
                continue;
            }

            int device_index = std::stoi(key_string.substr(0, separator));
            int chip_index = std::stoi(key_string.substr(separator + 1));

            if (auto arr = val.as_array(); arr && arr->size() == 2)
            {
                int pdu_index = static_cast<int>((*arr)[0].value_or(0));
                int matrix_index = static_cast<int>((*arr)[1].value_or(0));
                device_chip_to_pdu_matrix[{device_index, chip_index}] = {pdu_index, matrix_index};
            }
        }
        mist::logger::info(TString::Format("(Mapping::load_calib) device_chip_to_pdu_matrix size: %zu",
                                           device_chip_to_pdu_matrix.size())
                               .Data());
    }

    // --- hv_line_orientation ----------------------------------------------------
    if (auto hv_orientation_table = loaded_tables["hv_line_orientation"].as_table())
    {
        mist::logger::info("(Mapping::load_calib) Found hv_line_orientation table, loading contents");
        hv_line_orientation.clear();
        for (auto &[key, val] : *hv_orientation_table)
        {
            int matrix_index = std::stoi(std::string(key));
            std::string orientation_string = val.value_or(std::string("V"));
            hv_line_orientation[matrix_index] =
                (orientation_string == "H") ? line_orientation_type::Horizontal
                                            : line_orientation_type::Vertical;
        }
        mist::logger::info(TString::Format("(Mapping::load_calib) hv_line_orientation size: %zu",
                                           hv_line_orientation.size())
                               .Data());
    }
}

// ============================================================================
//  Cache construction
// ============================================================================

void Mapping::build_index_to_position_cache(float origin_cut)
{
    index_to_hit_xy.clear();
    cache_index_to_xy_built = false;
    cache_xy_to_index_built = false;

    int number_of_mapped_channels = 0;

    // Iterate (device, chip_logical, channel_logical) tuples directly
    // and use the @ref GlobalIndex value-type overload of
    // get_position_from_global_index.  The cache key is
    // `4 * channel_ordinal` — a small dense int that doubles as the
    // MIST HoughTransform `lut_key` plumbed through `index_to_hit_xy`.
    const int max_chip = ::gidx::kUsesSplitInTwo ? 4 : 8;
    constexpr int kChannelHi = 64;

    for (int device = ::gidx::kFirstDevice; device < ::gidx::kDeviceUpperBound; ++device)
        for (int chip = 0; chip < max_chip; ++chip)
            for (int channel = 0; channel < kChannelHi; ++channel)
            {
                const auto gi = ::GlobalIndex::from_components(
                    device, /*fifo=*/0, chip, channel, /*tdc=*/0);
                auto position_optional = get_position_from_global_index(gi);
                if (!position_optional)
                    continue;

                //  Skip channels whose resolved position is suspiciously close
                //  to the origin — these correspond to unmapped or dead channels
                //  for which the position formula returns the bare guard-ring
                //  offsets ≈ (0, 0).
                if (std::fabs((*position_optional)[0]) < origin_cut &&
                    std::fabs((*position_optional)[1]) < origin_cut)
                    continue;

                index_to_hit_xy[4 * gi.channel_ordinal()] = *position_optional;
                ++number_of_mapped_channels;
            }

    cache_index_to_xy_built = true;
    mist::logger::info(TString::Format("(Mapping::build_index_to_position_cache) "
                                       "Built cache with %d entries (origin_cut = %.1f mm).",
                                       number_of_mapped_channels, origin_cut)
                           .Data());
}

void Mapping::build_position_to_index_cache(std::string collision_policy)
{
    if (!cache_index_to_xy_built)
    {
        mist::logger::info("(Mapping::build_position_to_index_cache) "
                           "Forward cache not built yet — building with default parameters.");
        build_index_to_position_cache();
    }

    hit_xy_to_index.clear();
    cache_xy_to_index_built = false;
    int number_of_collisions = 0;

    for (auto const &[GlobalIndex, position] : index_to_hit_xy)
    {
        auto [it, inserted] = hit_xy_to_index.emplace(position, GlobalIndex);

        if (!inserted)
        {
            ++number_of_collisions;
            if (collision_policy == "last")
                it->second = GlobalIndex;
            else if (collision_policy == "warn")
                mist::logger::info(TString::Format("(Mapping::build_position_to_index_cache) "
                                                   "Collision at (%.3f, %.3f): keeping index %d, "
                                                   "ignoring index %d.",
                                                   position[0], position[1],
                                                   it->second, GlobalIndex)
                                       .Data());
            // "first" (default): keep already-stored entry, do nothing
        }
    }

    cache_xy_to_index_built = true;
    mist::logger::info(TString::Format("(Mapping::build_position_to_index_cache) "
                                       "Built reverse cache with %zu entries "
                                       "(%d collision(s), policy = '%s').",
                                       hit_xy_to_index.size(),
                                       number_of_collisions,
                                       collision_policy.c_str())
                           .Data());
}

// ============================================================================
//  Static member definitions (only the immutable EO→DO routing table
//  remains static; the four calibration maps are now per-instance and
//  default-initialised to empty by the in-class declarations).
// ============================================================================

const std::map<int, std::vector<int>> Mapping::matrix_to_do_channel = {
    {1, {3, 2, 1, 0, 8, 9, 10, 11, 17, 16, 12, 4, 18, 19, 5, 13, 25, 24, 21, 20, 26, 27, 28, 29, 30, 22, 14, 6, 7, 15, 23, 31, 35, 34, 33, 32, 36, 37, 38, 39, 43, 42, 41, 40, 44, 45, 46, 47, 51, 50, 49, 48, 52, 53, 54, 55, 59, 58, 57, 56, 60, 61, 62, 63}},
    {2, {59, 58, 57, 56, 60, 61, 62, 63, 51, 50, 49, 48, 52, 53, 54, 55, 43, 42, 41, 40, 44, 45, 46, 47, 35, 34, 33, 32, 36, 37, 38, 39, 0, 8, 16, 24, 25, 17, 26, 27, 29, 28, 1, 9, 30, 31, 18, 19, 21, 20, 2, 10, 22, 23, 11, 12, 3, 15, 14, 13, 4, 5, 6, 7}},
    {3, {4, 5, 6, 7, 3, 2, 1, 0, 12, 13, 14, 15, 11, 10, 9, 8, 20, 21, 22, 23, 19, 18, 17, 16, 28, 29, 30, 31, 27, 26, 25, 24, 46, 63, 55, 47, 54, 62, 39, 38, 34, 35, 36, 37, 33, 32, 45, 44, 42, 43, 61, 53, 41, 40, 52, 60, 48, 49, 50, 51, 59, 58, 57, 56}},
    {4, {60, 61, 62, 63, 55, 54, 53, 52, 46, 47, 51, 59, 45, 44, 58, 50, 38, 39, 42, 43, 37, 36, 35, 34, 33, 41, 49, 57, 56, 48, 40, 32, 28, 29, 30, 31, 27, 26, 25, 24, 20, 21, 22, 23, 19, 18, 17, 16, 12, 13, 14, 15, 11, 10, 9, 8, 4, 5, 6, 7, 3, 2, 1, 0}}};
// =============================================================================
// CONVENTION-BREAK NOTICE — see same notice in src/alcor_finedata.cxx
// =============================================================================
//
// `get_position_from_global_index(::GlobalIndex)` and its int-overload
// were inline in `include/mapping.h` until the IWYU sweep.
// They were moved out under a misdiagnosis — the autoparse failures
// were a LinkDef problem, not a header-self-sufficiency problem.  The
// canonical home for these short forwarders is the header.
//
// Not being reverted blindly; do not generalise the pattern.
// =============================================================================

std::optional<std::array<float, 2>>
Mapping::get_position_from_global_index(::GlobalIndex gi) const
{
    return get_position_from_device_chip_eoch(gi.device(),
                                              gi.real_chip(),
                                              gi.eo_channel());
}

std::optional<std::array<float, 2>>
Mapping::get_position_from_global_index(int stored_raw) const
{
    return get_position_from_global_index(::GlobalIndex(static_cast<uint32_t>(stored_raw)));
}
