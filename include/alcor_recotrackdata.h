#pragma once

/**
 * @file alcor_recotrackdata.h
 * @brief Track-matched reconstructed hit data for the ePIC dRICH prototype.
 *
 * Extends @ref alcor_recodata with per-track information imported from the
 * ALTAI tracking telescope.  Defines @ref alcor_recotrackdata_struct (track
 * fit parameters for one telescope plane) and @ref alcor_recotrackdata (the
 * container class providing track accessors, ROOT TTree I/O, and import from
 * @ref tracking_altai).
 */

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
    /**
     * @todo Constructor linking to previous recodata object
     */

    /// Default constructor
    alcor_recotrackdata() = default;
    /// @brief Construct by linking to an existing @ref alcor_recodata (shares recodata and trigger pointers).
    alcor_recotrackdata(alcor_recodata &v);

    /**
     * @brief Append a new track entry.
     * @param entry Track data to add.
     */
    void add_recotrackdata(const alcor_recotrackdata_struct &entry) { recotrackdata.push_back(entry); }

    /**
     * @brief Access track data by index (modifiable).
     *
     * Automatically resizes the internal vector if the index is out of bounds.
     *
     * @param idx Index of the track entry
     * @return Reference to the track struct at the given index
     */
    alcor_recotrackdata_struct &recotrackdata_at(std::size_t idx)
    {
        if (recotrackdata.size() <= idx)
            recotrackdata.resize(idx + 1); // default-construct new elements
        return recotrackdata.at(idx);
    }

    /**
     * @brief Combined angular coefficient magnitude from X and Y components.
     * @param idx Index of the track entry
     * @return @c hypot(angcoeff_x, angcoeff_y)
     */
    double get_traj_angcoeff(std::size_t idx) const { return std::hypot(recotrackdata_at(idx).traj_angcoeff_x, recotrackdata_at(idx).traj_angcoeff_y); }

    /// Const accessor — bounds-checked, no auto-resize.
    const alcor_recotrackdata_struct &recotrackdata_at(std::size_t idx) const { return recotrackdata.at(idx); }

    /** @name Setters for individual track fields */
    ///@{
    void set_det_plane_x(std::size_t idx, float val)     { recotrackdata_at(idx).det_plane_x     = val; }
    void set_det_plane_y(std::size_t idx, float val)     { recotrackdata_at(idx).det_plane_y     = val; }
    void set_traj_angcoeff_x(std::size_t idx, float val) { recotrackdata_at(idx).traj_angcoeff_x = val; }
    void set_traj_angcoeff_y(std::size_t idx, float val) { recotrackdata_at(idx).traj_angcoeff_y = val; }
    void set_chi2ndof(std::size_t idx, float val)        { recotrackdata_at(idx).chi2ndof        = val; }
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
    void import_event(std::vector<tracking_altai_struct> vec);
    ///@}

private:
    /// Container holding all reconstructed track entries
    std::vector<alcor_recotrackdata_struct> recotrackdata;

    /// Pointer to the container for I/O convenience
    std::vector<alcor_recotrackdata_struct> *recotrackdata_ptr = &recotrackdata;
};

/**
 * @brief Compute track angle from detector to telescope plane.
 *
 * Returns `atan(pixel_position / detector_to_telescope_plane)`, giving the
 * angle [rad] that the track makes with the beam axis at the detector plane.
 *
 * @param detector_to_telescope_plane Distance from detector to telescope plane [cm]
 * @param pixel_position Pixel displacement [cm]
 * @return Track angle [rad]
 */
inline double calculate_angle(double detector_to_telescope_plane, double pixel_position)
{
    return std::atan(pixel_position / detector_to_telescope_plane);
}

/**
 * @brief Compute track angle resolution via error propagation.
 *
 * Propagates uncertainties on both the plane distance and the pixel position
 * through the `atan` formula.
 *
 * @param detector_to_telescope_plane       Nominal distance [cm]
 * @param detector_to_telescope_plane_error Uncertainty on the distance [cm]
 * @param pixel_position                    Nominal pixel displacement [cm]
 * @param pixel_position_error              Uncertainty on the pixel position [cm]
 * @return Track angle resolution [rad]
 * @todo Implement proper error propagation.
 */
inline double calculate_angle_resolution(double detector_to_telescope_plane, double detector_to_telescope_plane_error,
                                         double pixel_position, double pixel_position_error)
{
    (void)detector_to_telescope_plane;
    (void)detector_to_telescope_plane_error;
    (void)pixel_position;
    (void)pixel_position_error;
    return -1.;
}

// --- Default detector plane distances (cm) ---
/// Distance from the dRICH detector to telescope plane 0 [cm].
inline const double detector_to_telescope_plane_0 = 235.;
/// Distance to telescope plane 1 [cm] (plane 0 + 2.55 cm).
inline const double detector_to_telescope_plane_1 = detector_to_telescope_plane_0 + 2.55;
/// Distance to telescope plane 2 [cm] (plane 1 + 22.5 cm).
inline const double detector_to_telescope_plane_2 = detector_to_telescope_plane_1 + 22.5;
/// Distance to telescope plane 3 [cm] (plane 2 + 2.55 cm).
inline const double detector_to_telescope_plane_3 = detector_to_telescope_plane_2 + 2.55;

// --- Pixel resolutions per plane (cm) ---
inline const double pixel_resolution_plane_0 = 3.7e-3; ///< Pixel resolution of plane 0 [cm]
inline const double pixel_resolution_plane_1 = 3.4e-3; ///< Pixel resolution of plane 1 [cm]
inline const double pixel_resolution_plane_2 = 3.4e-3; ///< Pixel resolution of plane 2 [cm]
inline const double pixel_resolution_plane_3 = 3.7e-3; ///< Pixel resolution of plane 3 [cm]