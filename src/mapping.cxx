#include "mapping.h"

//  Constructors
mapping::mapping(std::string conf_file_name) { load_calib(conf_file_name); }

//  Getters
std::optional<int> mapping::get_do_channel(int matrix, int eo_channel) const
{
    auto it = matrix_to_do_channel.find(matrix);
    if (it == matrix_to_do_channel.end())
        return std::nullopt;
    if (eo_channel < 0 || eo_channel >= (int)it->second.size())
        return std::nullopt;
    return it->second[eo_channel];
}
std::optional<std::array<int, 2>> mapping::get_pdu_matrix(int device, int chip) const
{
    auto it = device_chip_to_pdu_matrix.find({device, chip});
    if (it == device_chip_to_pdu_matrix.end())
        return std::nullopt;
    return it->second;
}
std::optional<std::array<float, 2>> mapping::get_position_from_pdu_column_row(int pdu, int column, int row) const
{
    //  TODO: proper checks
    if (pdu < 0 || column < 0 || row < 0)
        return std::nullopt;

    //  Generate the in-matrix placement
    //  TODO: investigate meaning, but for now it works
    float x = 0.05 + 0.1 + 0.2 + 1.5 + 3.2 * column + (column > 7 ? 0.3f : 0.f);
    float y = 0.05 + 0.1 + 0.2 + 1.5 + 3.2 * row + (row > 7 ? 0.3f : 0.f);

    //  Add the full PDU center placement
    auto it = pdu_xy_position.find(pdu);
    if (it != pdu_xy_position.end())
    {
        x += it->second[0];
        y += it->second[1];
    }

    return std::array<float, 2>{x, y};
}
std::optional<std::array<float, 2>> mapping::get_position_from_pdu_matrix_eoch(int pdu, int matrix, int eo_channel) const
{
    auto do_channel = get_do_channel(matrix, eo_channel);
    if (!do_channel)
        return std::nullopt;
    auto column = (*do_channel) / 8 + (matrix > 2 ? 8 : 0);
    auto row = (*do_channel) % 8 + (matrix % 2 == 0 ? 8 : 0);

    //  Rotate the PDU if requested
    if (pdu_rotation[pdu])
        return get_position_from_pdu_column_row(pdu, 15 - column, 15 - row);
    return get_position_from_pdu_column_row(pdu, column, row);
}
std::optional<std::array<float, 2>> mapping::get_position_from_device_chip_eoch(int device, int chip, int eo_channel) const
{
    auto it = device_chip_to_pdu_matrix.find({device, chip});
    if (it == device_chip_to_pdu_matrix.end())
        return std::nullopt;

    auto pdu = it->second[0];
    auto matrix = it->second[1];
    return get_position_from_pdu_matrix_eoch(pdu, matrix, eo_channel);
}
std::optional<std::array<float, 2>> mapping::get_position_from_finedata(alcor_finedata entry) const { return get_position_from_device_chip_eoch(entry.get_device(), entry.get_chip(), entry.get_eo_channel()); }
std::optional<std::array<float, 2>> mapping::get_position_from_global_index(int global_index) const { return get_position_from_device_chip_eoch(get_device_from_global_tdc_index(global_index), get_chip_from_global_tdc_index(global_index), get_eo_channel_index_from_global_tdc_index(global_index)); }
void mapping::assign_position(alcor_finedata_struct &entry)
{
    auto current_position = get_position_from_global_index(entry.global_index);
    if (!current_position)
    {
        entry.hit_x = -99.f;
        entry.hit_y = -99.f;
    }
    entry.hit_x = (*current_position)[0];
    entry.hit_y = (*current_position)[1];
}

//  I/O
void mapping::load_calib(std::string filename, bool verbose)
{
    //  Invalidate caches whenever calibration is reloaded
    index_to_hit_xy.clear();
    hit_xy_to_index.clear();
    cache_index_to_xy_built = false;
    cache_xy_to_index_built = false;

    //  Parsing file
    auto loaded_tables = toml::parse_file(filename);

    // Load pdu_xy_position
    if (auto placement_table = loaded_tables["pdu_xy_position"].as_table())
    {
        mist::logger::info("(mapping::load_calib) Found pdu_xy_position table, loading contents");
        pdu_xy_position.clear();
        for (auto &[key, val] : *placement_table)
        {
            if (auto arr = val.as_array(); arr && arr->size() == 2)
                pdu_xy_position[std::stoi(std::string(key))] = {
                    static_cast<float>((*arr)[0].value_or(0.0)),
                    static_cast<float>((*arr)[1].value_or(0.0))};
        }
        mist::logger::info(Form("(mapping::load_calib) pdu_xy_position size: %zu", pdu_xy_position.size()));
    }

    // Load pdu_rotation
    if (auto rotation_table = loaded_tables["pdu_rotation"].as_table())
    {
        mist::logger::info("(mapping::load_calib) Found pdu_rotation table, loading contents");
        pdu_rotation.clear();
        for (auto &[key, val] : *rotation_table)
        {
            int idx = std::stoi(std::string(key));
            bool flag = val.value<bool>().value_or(true);
            if (idx >= 1 && idx <= 8)
                pdu_rotation[idx] = flag;
        }
        mist::logger::info(Form("(mapping::load_calib) pdu_rotation size: %zu", pdu_rotation.size()));
    }

    //  Load device_chip_to_pdu_matrix
    if (auto device_chip_to_pdu_matrix_table = loaded_tables["device_chip_to_pdu_matrix"].as_table())
    {
        mist::logger::info("(mapping::load_calib) Found device_chip_to_pdu_matrix table, loading contents");
        device_chip_to_pdu_matrix.clear();
        for (auto &[key, val] : *device_chip_to_pdu_matrix_table)
        {
            // Expect key format "device_chip"
            std::string k = std::string(key);
            auto pos = k.find('_');
            if (pos == std::string::npos)
            {
                mist::logger::info(Form("(mapping::load_calib) Skipping invalid key: %s", k.c_str()));
                continue;
            }

            int device = std::stoi(k.substr(0, pos));
            int chip = std::stoi(k.substr(pos + 1));

            if (auto arr = val.as_array(); arr && arr->size() == 2)
            {
                int pdu = static_cast<int>((*arr)[0].value_or(0));
                int matrix = static_cast<int>((*arr)[1].value_or(0));
                device_chip_to_pdu_matrix[{device, chip}] = {pdu, matrix};
            }
        }
        mist::logger::info(Form("(mapping::load_calib) device_chip_to_pdu_matrix size: %zu", device_chip_to_pdu_matrix.size()));
    }
}

//  Cache construction
void mapping::build_index_to_position_cache(float origin_cut)
{
    index_to_hit_xy.clear();
    cache_index_to_xy_built = false;
    cache_xy_to_index_built = false;   // invalidate reverse cache too

    // Global TDC indices are packed as index = 4 * (chip_offset + eo_group),
    // so we step by 4 over the full range [0, 2048*4).
    constexpr int kMaxIndex = 2048 * 4;
    int n_mapped = 0;

    for (int i_index = 0; i_index < kMaxIndex; i_index += 4)
    {
        auto position = get_position_from_global_index(i_index);
        if (!position)
            continue;

        // Skip hits whose position is suspiciously close to the origin —
        // these typically correspond to unmapped or dead channels that
        // the position formula returns as (0+offsets, 0+offsets) ≈ (0,0).
        if (std::fabs((*position)[0]) < origin_cut &&
            std::fabs((*position)[1]) < origin_cut)
            continue;

        index_to_hit_xy[i_index] = (*position);
        ++n_mapped;
    }

    cache_index_to_xy_built = true;
    mist::logger::info(Form("(mapping::build_index_to_position_cache) "
                            "Built cache with %d entries (origin_cut=%.1f mm).",
                            n_mapped, origin_cut));
}

void mapping::build_position_to_index_cache(std::string collision_policy)
{
    // Ensure the forward cache exists first.
    if (!cache_index_to_xy_built)
    {
        mist::logger::info("(mapping::build_position_to_index_cache) "
                           "Forward cache not built yet — building with default parameters.");
        build_index_to_position_cache();
    }

    hit_xy_to_index.clear();
    cache_xy_to_index_built = false;
    int n_collisions = 0;

    for (auto const &[idx, pos] : index_to_hit_xy)
    {
        auto [it, inserted] = hit_xy_to_index.emplace(pos, idx);

        if (!inserted)   // position key already present → collision
        {
            ++n_collisions;
            if (collision_policy == "last")
            {
                it->second = idx;
            }
            else if (collision_policy == "warn")
            {
                mist::logger::info(Form("(mapping::build_position_to_index_cache) "
                                        "Collision at (%.3f, %.3f): keeping index %d, "
                                        "ignoring index %d.",
                                        pos[0], pos[1], it->second, idx));
            }
            // "first" (default): do nothing — keep the already-stored entry.
        }
    }

    cache_xy_to_index_built = true;
    mist::logger::info(Form("(mapping::build_position_to_index_cache) "
                            "Built reverse cache with %zu entries "
                            "(%d collision(s), policy='%s').",
                            hit_xy_to_index.size(),
                            n_collisions,
                            collision_policy.c_str()));
}

std::optional<std::array<float, 2>> mapping::get_cached_position(int global_index) const
{
    auto it = index_to_hit_xy.find(global_index);
    if (it == index_to_hit_xy.end())
        return std::nullopt;
    return it->second;
}

std::optional<int> mapping::get_cached_index(float x, float y) const
{
    auto it = hit_xy_to_index.find({x, y});
    if (it == hit_xy_to_index.end())
        return std::nullopt;
    return it->second;
}

const std::map<int, std::array<float, 2>> &mapping::get_index_to_position_map() const
{
    return index_to_hit_xy;
}

const std::map<std::array<float, 2>, int> &mapping::get_position_to_index_map() const
{
    return hit_xy_to_index;
}

//  Private static attributes
std::map<int, std::vector<int>> mapping::matrix_to_do_channel = {
    {1, {3, 2, 1, 0, 8, 9, 10, 11, 17, 16, 12, 4, 18, 19, 5, 13, 25, 24, 21, 20, 26, 27, 28, 29, 30, 22, 14, 6, 7, 15, 23, 31, 35, 34, 33, 32, 36, 37, 38, 39, 43, 42, 41, 40, 44, 45, 46, 47, 51, 50, 49, 48, 52, 53, 54, 55, 59, 58, 57, 56, 60, 61, 62, 63}},
    {2, {59, 58, 57, 56, 60, 61, 62, 63, 51, 50, 49, 48, 52, 53, 54, 55, 43, 42, 41, 40, 44, 45, 46, 47, 35, 34, 33, 32, 36, 37, 38, 39, 0, 8, 16, 24, 25, 17, 26, 27, 29, 28, 1, 9, 30, 31, 18, 19, 21, 20, 2, 10, 22, 23, 11, 12, 3, 15, 14, 13, 4, 5, 6, 7}},
    {3, {4, 5, 6, 7, 3, 2, 1, 0, 12, 13, 14, 15, 11, 10, 9, 8, 20, 21, 22, 23, 19, 18, 17, 16, 28, 29, 30, 31, 27, 26, 25, 24, 46, 63, 55, 47, 54, 62, 39, 38, 34, 35, 36, 37, 33, 32, 45, 44, 42, 43, 61, 53, 41, 40, 52, 60, 48, 49, 50, 51, 59, 58, 57, 56}},
    {4, {60, 61, 62, 63, 55, 54, 53, 52, 46, 47, 51, 59, 45, 44, 58, 50, 38, 39, 42, 43, 37, 36, 35, 34, 33, 41, 49, 57, 56, 48, 40, 32, 28, 29, 30, 31, 27, 26, 25, 24, 20, 21, 22, 23, 19, 18, 17, 16, 12, 13, 14, 15, 11, 10, 9, 8, 4, 5, 6, 7, 3, 2, 1, 0}}};
std::map<int, bool> mapping::pdu_rotation = {};
std::map<int, std::array<float, 2>> mapping::pdu_xy_position = {};
std::map<std::array<int, 2>, std::array<int, 2>> mapping::device_chip_to_pdu_matrix = {};