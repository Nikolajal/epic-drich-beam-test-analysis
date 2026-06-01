#include "tracking_altai.h"
#include <mist/logger/logger.h>

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
    mist::logger::info(TString::Format("(TrackingAltai::load_tracking_file) Loading tracking file").Data());
    std::ifstream infile(input_file);
    if (!infile.is_open())
    {
        mist::logger::warning(TString::Format("Cannot open input file : %s", input_file.c_str()).Data());
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
            data_map.try_emplace(event_id); //  event exists, but has no tracks (line is '*')
            continue;
        }

        ss >> event_id;
        // Hoist the per-line map[event_id] + .back() lookup so we do them
        // ONCE instead of 11× (one per stream operator).  Previous version
        // did 22 map operator[]/back() calls per input line.
        auto &tracks_for_event = data_map[event_id];
        auto &track = tracks_for_event.emplace_back();

        ss >> track.zero_plane_x >> track.zero_plane_y >> track.zero_plane_z >> track.angcoeff_dx >> track.angcoeff_dy >> track.angcoeff_dz >> track.chi2 >> track.ndof >> track.chi2ndof >> track.timestamp;
        track.event_id = event_id;
    }

    mist::logger::info(TString::Format("(TrackingAltai::load_tracking_file) Done! Found %zu track events", data_map.size()).Data());
    infile.close();
}
