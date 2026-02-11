#include "tracking_altai.h"

tracking_altai::tracking_altai(const std::string &file_name) { load_tracking_file(file_name); }

//  Getters
std::map<uint32_t, std::vector<tracking_altai_struct>> tracking_altai::get_data_map() const { return data_map; }
uint32_t tracking_altai::get_number_of_events() const { return data_map.size(); }
std::vector<tracking_altai_struct> tracking_altai::get_event_tracks(uint32_t event_id) const
{
    auto it = data_map.find(event_id);
    if (it != data_map.end())
        return it->second;
    return {};
}
int tracking_altai::get_event_tracks_size(uint32_t event_id) const
{
    auto it = data_map.find(event_id);
    if (it != data_map.end())
        return static_cast<int>(it->second.size());
    return 0;
}
//  --- Track field getters
float tracking_altai::get_zero_plane_x(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).zero_plane_x; }
float tracking_altai::get_zero_plane_y(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).zero_plane_y; }
float tracking_altai::get_zero_plane_z(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).zero_plane_z; }
float tracking_altai::get_angcoeff_dx(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).angcoeff_dx; }
float tracking_altai::get_angcoeff_dy(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).angcoeff_dy; }
float tracking_altai::get_angcoeff_dz(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).angcoeff_dz; }
float tracking_altai::get_chi2(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).chi2; }
float tracking_altai::get_chi2ndof(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).chi2ndof; }
int tracking_altai::get_ndof(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).ndof; }
double tracking_altai::get_timestamp(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).timestamp; }

//  Setters
void tracking_altai::add_event_track(uint32_t event_id, const tracking_altai_struct &track) { data_map[event_id].push_back(track); }
void tracking_altai::set_event_tracks(uint32_t event_id, const std::vector<tracking_altai_struct> &tracks) { data_map[event_id] = tracks; }

//  Checks
bool tracking_altai::event_has_one_track(uint32_t event_id) const { return get_event_tracks_size(event_id) == 1; }
bool tracking_altai::event_has_at_least_one_track(uint32_t event_id) const { return get_event_tracks_size(event_id) > 0; }

//  I/O
void tracking_altai::load_tracking_file(const std::string &input_file)
{
    logger::log_info(Form("(tracking_altai::load_tracking_file) Loading tracking file"));
    //  Open file
    std::ifstream infile(input_file);
    if (!infile.is_open())
    {
        logger::log_warning(Form("Cannot open input file : %s", input_file.c_str()));
        return;
    }

    std::string line;
    bool firstLine = true;
    int iline = 0;
    while (std::getline(infile, line))
    {
        if (firstLine)
        { // skip header
            firstLine = false;
            continue;
        }

        int event_id;
        std::stringstream ss(line);

        if (line.find('*') != std::string::npos)
        {
            ss >> event_id;
            data_map[event_id];
            continue; // skip '*' rows
        }

        ss >> event_id;
        data_map[event_id].emplace_back();

        ss >> data_map[event_id].back().zero_plane_x >> data_map[event_id].back().zero_plane_y >> data_map[event_id].back().zero_plane_z >> data_map[event_id].back().angcoeff_dx >> data_map[event_id].back().angcoeff_dy >> data_map[event_id].back().angcoeff_dz >> data_map[event_id].back().chi2 >> data_map[event_id].back().ndof >> data_map[event_id].back().chi2ndof >> data_map[event_id].back().timestamp;
        data_map[event_id].back().event_id = event_id;
    }

    logger::log_info(Form("(tracking_altai::load_tracking_file) Done! Found %zu track events", data_map.size()));
    infile.close();
}