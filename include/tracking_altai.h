#pragma once

#include <string>
#include <vector>
#include <map>
#include "utility.h"

/**
 * @brief Structure holding single track information for Altai tracker.
 */
struct tracking_altai_struct
{
    uint32_t event_id;  ///< Global event ID
    float zero_plane_x; ///< Reference plane X
    float zero_plane_y; ///< Reference plane Y
    float zero_plane_z; ///< Reference plane Z (always 0)
    float angcoeff_dx;  ///< Track angular coefficient X
    float angcoeff_dy;  ///< Track angular coefficient Y
    float angcoeff_dz;  ///< Track angular coefficient Z (always 1)
    float chi2;         ///< Track chi^2
    float chi2ndof;     ///< Track chi^2 per degree of freedom
    int ndof;           ///< Number of degrees of freedom
    double timestamp;   ///< Timestamp of the track
};

/**
 * @brief Class managing Altai tracking data.
 */
class tracking_altai
{
public:
    /** @name Constructors & Destructor */
    ///@{
    tracking_altai() = default;
    explicit tracking_altai(const std::string &file_name);
    ~tracking_altai() = default;
    ///@}

    /** @name Getters */
    ///@{
    /**
     * @brief Get the full data map
     * @return Map of event_id to vector of tracks
     */
    std::map<uint32_t, std::vector<tracking_altai_struct>> get_data_map() const;
    uint32_t get_number_of_events() const;

    /**
     * @brief Get tracks for a specific event
     * @param event_id ID of the event
     * @return Vector of tracks for that event
     */
    std::vector<tracking_altai_struct> get_event_tracks(uint32_t event_id) const;

    /**
     * @brief Get number of tracks for an event
     * @param event_id ID of the event
     * @return Number of tracks
     */
    int get_event_tracks_size(uint32_t event_id) const;
    ///@}

    /** @name Track field getters */
    ///@{
    float get_zero_plane_x(uint32_t event_id, std::size_t idx) const;
    float get_zero_plane_y(uint32_t event_id, std::size_t idx) const;
    float get_zero_plane_z(uint32_t event_id, std::size_t idx) const;
    float get_angcoeff_dx(uint32_t event_id, std::size_t idx) const;
    float get_angcoeff_dy(uint32_t event_id, std::size_t idx) const;
    float get_angcoeff_dz(uint32_t event_id, std::size_t idx) const;
    float get_chi2(uint32_t event_id, std::size_t idx) const;
    float get_chi2ndof(uint32_t event_id, std::size_t idx) const;
    int get_ndof(uint32_t event_id, std::size_t idx) const;
    double get_timestamp(uint32_t event_id, std::size_t idx) const;
    ///@}

    /** @name Setters */
    ///@{
    /**
     * @brief Add a track to an event
     * @param event_id ID of the event
     * @param track Track to add
     */
    void add_event_track(uint32_t event_id, const tracking_altai_struct &track);

    /**
     * @brief Replace all tracks for an event
     * @param event_id ID of the event
     * @param tracks Vector of tracks
     */
    void set_event_tracks(uint32_t event_id, const std::vector<tracking_altai_struct> &tracks);
    ///@}

    /** @name Checks */
    ///@{
    bool event_has_one_track(uint32_t event_id) const;
    bool event_has_at_least_one_track(uint32_t event_id) const;
    ///@}

    /** @name I/O */
    ///@{
    void load_tracking_file(const std::string &input_file);
    ///@}

private:
    std::map<uint32_t, std::vector<tracking_altai_struct>> data_map; ///< Main storage for tracks
};
