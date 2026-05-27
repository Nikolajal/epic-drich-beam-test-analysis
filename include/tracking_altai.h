#pragma once

/**
 * @file TrackingAltai.h
 * @brief ALTAI silicon telescope track I/O — plain-data struct
 *        @ref TrackingAltaiStruct plus the @c TrackingAltaiReader that
 *        loads track files produced upstream by the ALTAI reconstruction.
 *
 * The framework consumes ALTAI tracks at the @c recotrackdata_writer stage
 * to associate Cherenkov hits with the impact point on the radiator plane.
 */

#include <string>
#include <vector>
#include <map>
#include "utility.h"

/**
 * @brief Plain-data struct holding a single reconstructed track from the ALTAI telescope.
 */
struct TrackingAltaiStruct
{
    uint32_t event_id;  ///< Global event ID.
    float zero_plane_x; ///< Track extrapolated X at the reference plane [mm].
    float zero_plane_y; ///< Track extrapolated Y at the reference plane [mm].
    float zero_plane_z; ///< Track extrapolated Z at the reference plane [mm] (always 0).
    float angcoeff_dx;  ///< Track angular coefficient along X (dx/dz).
    float angcoeff_dy;  ///< Track angular coefficient along Y (dy/dz).
    float angcoeff_dz;  ///< Track angular coefficient along Z (always 1).
    float chi2;         ///< Track χ².
    float chi2ndof;     ///< Track χ²/NDF.
    int ndof;           ///< Number of degrees of freedom.
    double timestamp;   ///< Timestamp of the track.
};

/**
 * @brief Container and loader for ALTAI telescope tracking data.
 *
 * Stores one map from event ID to the list of reconstructed tracks for that
 * event.  Data are loaded from a plain-text ALTAI output file via
 * @ref load_tracking_file.
 */
class TrackingAltai
{
public:
    /** @name Construction */
    ///@{

    /// @brief Default constructor — empty container.
    TrackingAltai() = default;

    /**
     * @brief Construct and immediately load @p file_name.
     * @param file_name Path to the ALTAI tracking output file.
     */
    explicit TrackingAltai(const std::string &file_name) { load_tracking_file(file_name); }

    /// @brief Default destructor.
    ~TrackingAltai() = default;

    ///@}

    /** @name Getters */
    ///@{

    /**
     * @brief Return a copy of the full event → track-list map.
     * @return Map from event ID to vector of @ref TrackingAltaiStruct.
     */
    std::map<uint32_t, std::vector<TrackingAltaiStruct>> get_data_map() const { return data_map; }

    /// @brief Return the total number of events (keys) in the map.
    uint32_t get_number_of_events() const { return static_cast<uint32_t>(data_map.size()); }

    /**
     * @brief Return all tracks for a given event.
     * @param event_id  Event to query.
     * @return Vector of @ref TrackingAltaiStruct; empty if the event is absent.
     */
    std::vector<TrackingAltaiStruct> get_event_tracks(uint32_t event_id) const;

    /**
     * @brief Return the number of tracks reconstructed for @p event_id.
     * @param event_id  Event to query.
     * @return Track count, or 0 if the event is absent.
     */
    int get_event_tracks_size(uint32_t event_id) const;

    ///@}

    /** @name Track field getters */
    ///@{

    /// @brief Extrapolated X at the reference plane [mm] for track @p idx of event @p event_id.
    float get_zero_plane_x(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).zero_plane_x; }

    /// @brief Extrapolated Y at the reference plane [mm] for track @p idx of event @p event_id.
    float get_zero_plane_y(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).zero_plane_y; }

    /// @brief Extrapolated Z at the reference plane [mm] for track @p idx of event @p event_id (always 0).
    float get_zero_plane_z(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).zero_plane_z; }

    /// @brief Angular coefficient dx/dz for track @p idx of event @p event_id.
    float get_angcoeff_dx(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).angcoeff_dx; }

    /// @brief Angular coefficient dy/dz for track @p idx of event @p event_id.
    float get_angcoeff_dy(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).angcoeff_dy; }

    /// @brief Angular coefficient dz/dz for track @p idx of event @p event_id (always 1).
    float get_angcoeff_dz(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).angcoeff_dz; }

    /// @brief χ² for track @p idx of event @p event_id.
    float get_chi2(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).chi2; }

    /// @brief χ²/NDF for track @p idx of event @p event_id.
    float get_chi2ndof(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).chi2ndof; }

    /// @brief Degrees of freedom for track @p idx of event @p event_id.
    int get_ndof(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).ndof; }

    /// @brief Timestamp for track @p idx of event @p event_id.
    double get_timestamp(uint32_t event_id, std::size_t idx) const { return data_map.at(event_id).at(idx).timestamp; }

    ///@}

    /** @name Setters */
    ///@{

    /**
     * @brief Append @p track to the track list for @p event_id.
     * @param event_id  Target event.
     * @param track     Track to append.
     */
    void add_event_track(uint32_t event_id, const TrackingAltaiStruct &track) { data_map[event_id].push_back(track); }

    /**
     * @brief Replace all tracks for @p event_id with @p tracks.
     * @param event_id  Target event.
     * @param tracks    New track list.
     */
    void set_event_tracks(uint32_t event_id, const std::vector<TrackingAltaiStruct> &tracks) { data_map[event_id] = tracks; }

    ///@}

    /** @name Checks */
    ///@{

    /// @brief @c true if exactly one track was reconstructed for @p event_id.
    bool event_has_one_track(uint32_t event_id) const { return get_event_tracks_size(event_id) == 1; }

    /// @brief @c true if at least one track was reconstructed for @p event_id.
    bool event_has_at_least_one_track(uint32_t event_id) const { return get_event_tracks_size(event_id) > 0; }

    ///@}

    /** @name I/O */
    ///@{

    /**
     * @brief Parse an ALTAI plain-text output file and populate the internal map.
     * @param input_file  Path to the ALTAI tracking output file.
     */
    void load_tracking_file(const std::string &input_file);

    ///@}

private:
    std::map<uint32_t, std::vector<TrackingAltaiStruct>> data_map; ///< Event ID → list of reconstructed tracks.
};
