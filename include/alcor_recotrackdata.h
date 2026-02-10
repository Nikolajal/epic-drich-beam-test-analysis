#pragma once

#include "alcor_recodata.h"
#include "tracking_altai.h"
#include <vector>
#include "TTree.h"
#include <cmath>

/**
 * @brief Structure holding reconstructed track data for a single plane.
 */
struct alcor_recotrackdata_struct
{
    /// Plane extrapolated coordinates
    float det_plane_x; ///< Detector plane X coordinate
    float det_plane_y; ///< Detector plane Y coordinate

    /// Track angular coefficients
    float traj_angcoeff_x; ///< Angular coefficient along X
    float traj_angcoeff_y; ///< Angular coefficient along Y

    /// Track fit quality
    float chi2ndof; ///< Chi2 per degree of freedom

    /// Default constructor
    alcor_recotrackdata_struct() = default;
    alcor_recotrackdata_struct(alcor_recodata &v);
};

/**
 * @brief Container class for reconstructed track data.
 *
 * Inherits from @ref alcor_recodata to link general event information with
 * per-track reconstructed data.
 */
class alcor_recotrackdata : public alcor_recodata
{
public:
    /// Default constructor
    alcor_recotrackdata() = default;
    alcor_recotrackdata(alcor_recodata &v);

    /// TODO: Constructor linking to previous recodata object

    /**
     * @brief Add a new track entry.
     * @param entry The track data to add.
     */
    void add_recotrackdata(const alcor_recotrackdata_struct &entry);

    /**
     * @brief Access track data by index (modifiable).
     *
     * Automatically resizes the internal vector if the index is out of bounds.
     *
     * @param idx Index of the track entry
     * @return Reference to the track struct at the given index
     */
    alcor_recotrackdata_struct &recotrackdata_at(std::size_t idx);

    /// Const version of recotrackdata_at
    const alcor_recotrackdata_struct &recotrackdata_at(std::size_t idx) const;

    /** @name Setters for individual track fields */
    ///@{
    void set_det_plane_x(std::size_t idx, float val);
    void set_det_plane_y(std::size_t idx, float val);
    void set_traj_angcoeff_x(std::size_t idx, float val);
    void set_traj_angcoeff_y(std::size_t idx, float val);
    void set_chi2ndof(std::size_t idx, float val);
    ///@}

    /** @name I/O utilities */
    ///@{
    void clear();
    void link_to_tree(TTree *input_tree);
    void write_to_tree(TTree *output_tree);
    ///@}

    /** @name Importing from tracking classes */
    ///@{
    /**
     * @brief Imports data from altai tracking data struct
     *
     * @param v Incoming data
     */
    void import_event( std::vector<tracking_altai_struct> vec);
    ///@}

private:
    /// Container holding all reconstructed track entries
    std::vector<alcor_recotrackdata_struct> recotrackdata;

    /// Pointer to the container for I/O convenience
    std::vector<alcor_recotrackdata_struct> *recotrackdata_ptr = &recotrackdata;
};

/**
 * @brief Compute track angle from detector to telescope plane
 * @param detector_to_telescope_plane Distance from detector to telescope plane [cm]
 * @param pixel_position Pixel displacement [cm]
 * @return Track angle [rad]
 */
inline double calculate_angle(double detector_to_telescope_plane, double pixel_position);

/**
 * @brief Compute track angle resolution (currently TODO)
 */
inline double calculate_angle_resolution(double detector_to_telescope_plane, double detector_to_telescope_plane_error,
                                         double pixel_position, double pixel_position_error);

// --- Default detector plane distances (cm) ---
inline const double detector_to_telescope_plane_0 = 235.;
inline const double detector_to_telescope_plane_1 = detector_to_telescope_plane_0 + 2.55;
inline const double detector_to_telescope_plane_2 = detector_to_telescope_plane_1 + 22.5;
inline const double detector_to_telescope_plane_3 = detector_to_telescope_plane_2 + 2.55;

// --- Pixel resolutions per plane (cm) ---
inline const double pixel_resolution_plane_0 = 3.7e-3;
inline const double pixel_resolution_plane_1 = 3.4e-3;
inline const double pixel_resolution_plane_2 = 3.4e-3;
inline const double pixel_resolution_plane_3 = 3.7e-3;