#include "tracking_altai.h"

std::vector<TrackingAltaiStruct> TrackingAltai::get_event_tracks(uint32_t event_id) const
{
    auto it = data_map.find(event_id);
    if (it != data_map.end())
        return it->second;
    return {};
}

int TrackingAltai::get_event_tracks_size(uint32_t event_id) const
{
    auto it = data_map.find(event_id);
    if (it != data_map.end())
        return static_cast<int>(it->second.size());
    return 0;
}

void TrackingAltai::load_tracking_file(const std::string &input_file)
{
    mist::logger::info(Form("(TrackingAltai::load_tracking_file) Loading tracking file"));
    std::ifstream infile(input_file);
    if (!infile.is_open())
    {
        mist::logger::warning(Form("Cannot open input file : %s", input_file.c_str()));
        return;
    }

    std::string line;
    bool firstLine = true;
    while (std::getline(infile, line))
    {
        if (firstLine)
        {
            firstLine = false;
            continue;
        }

        int event_id;
        std::stringstream ss(line);

        if (line.find('*') != std::string::npos)
        {
            ss >> event_id;
            data_map.try_emplace(event_id);  //  event exists, but has no tracks (line is '*')
            continue;
        }

        ss >> event_id;
        data_map[event_id].emplace_back();

        ss >> data_map[event_id].back().zero_plane_x >> data_map[event_id].back().zero_plane_y >> data_map[event_id].back().zero_plane_z >> data_map[event_id].back().angcoeff_dx >> data_map[event_id].back().angcoeff_dy >> data_map[event_id].back().angcoeff_dz >> data_map[event_id].back().chi2 >> data_map[event_id].back().ndof >> data_map[event_id].back().chi2ndof >> data_map[event_id].back().timestamp;
        data_map[event_id].back().event_id = event_id;
    }

    mist::logger::info(Form("(TrackingAltai::load_tracking_file) Done! Found %zu track events", data_map.size()));
    infile.close();
}
