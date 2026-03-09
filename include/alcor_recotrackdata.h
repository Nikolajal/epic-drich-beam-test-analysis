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
#include <cmath>
#include "TTree.h"

// =========================================================================
//  Plain data struct
// =========================================================================

/**
 * @brief Track fit parameters and extrapolated position for one telescope plane.
 */
struct alcor_recotrackdata_struct
{
    float det_plane_x;     ///< Extrapolated X coordinate at the detector plane [mm].
    float det_plane_y;     ///< Extrapolated Y coordinate at the detector plane [mm].
    float traj_angcoeff_x; ///< Track angular coefficient along X (dx/dz).
    float traj_angcoeff_y; ///< Track angular coefficient along Y (dy/dz).
    float chi2ndof;        ///< Fit quality: χ²/NDF.

    alcor_recotrackdata_struct() = default;

    /**
     * @brief Construct from a reconstructed data entry.
     * @param v Source @ref alcor_recodata object.
     */
    alcor_recotrackdata_struct(alcor_recodata &v);
};

// =========================================================================
//  Container class
// =========================================================================

/**
 * @brief Container for track-matched reconstructed data.
 *
 * Inherits the full event/trigger machinery from @ref alcor_recodata and
 * adds a per-event vector of @ref alcor_recotrackdata_struct entries, one
 * per telescope track reconstructed by ALTAI.
 */
class alcor_recotrackdata : public alcor_recodata
{
public:
    // -------------------------------------------------------------------------
    /** @name Construction */
    /// @{

    /// Default constructor — no shared pointers, all containers empty.
    alcor_recotrackdata() = default;

    /**
     * @brief Construct by linking to an existing @ref alcor_recodata.
     *
     * Shares the recodata and trigger pointer arrays of @p v so that a single
     * TTree branch set covers both base and derived data.
     *
     * @param v Source recodata object whose internal pointers are borrowed.
     */
    alcor_recotrackdata(alcor_recodata &v);

    /// @}

    // -------------------------------------------------------------------------
    /** @name Element access */
    /// @{

    /**
     * @brief Mutable access to a track entry; auto-resizes if @p idx is out of range.
     * @param idx Track index (0-based).
     * @return Reference to the @ref alcor_recotrackdata_struct at @p idx.
     */
    alcor_recotrackdata_struct &recotrackdata_at(std::size_t idx);

    /// @brief Const access — bounds-checked, no auto-resize.
    const alcor_recotrackdata_struct &recotrackdata_at(std::size_t idx) const { return recotrackdata.at(idx); }

    /// @brief Append a new track entry to the internal vector.
    void add_recotrackdata(const alcor_recotrackdata_struct &entry) { recotrackdata.push_back(entry); }

    /// @brief Number of track entries in the current event.
    std::size_t n_recotrackdata() const { return recotrackdata.size(); }

    /**
     * @brief Combined angular-coefficient magnitude: @c hypot(angcoeff_x, angcoeff_y).
     * @param idx Track index.
     * @return @c sqrt(angcoeff_x² + angcoeff_y²) for track @p idx.
     */
    double get_traj_angcoeff(std::size_t idx) const;

    /// @brief Extrapolated X coordinate at the detector plane [mm].
    float get_det_plane_x(std::size_t idx) const { return recotrackdata.at(idx).det_plane_x; }
    /// @brief Extrapolated Y coordinate at the detector plane [mm].
    float get_det_plane_y(std::size_t idx) const { return recotrackdata.at(idx).det_plane_y; }
    /// @brief Track angular coefficient along X (dx/dz).
    float get_traj_angcoeff_x(std::size_t idx) const { return recotrackdata.at(idx).traj_angcoeff_x; }
    /// @brief Track angular coefficient along Y (dy/dz).
    float get_traj_angcoeff_y(std::size_t idx) const { return recotrackdata.at(idx).traj_angcoeff_y; }
    /// @brief Fit quality χ²/NDF.
    float get_chi2ndof(std::size_t idx) const { return recotrackdata.at(idx).chi2ndof; }

    /// @}

    // -------------------------------------------------------------------------
    /** @name Polar coordinates */
    /// @{

    /**
     * @brief Radial distance of the extrapolated impact point from the beam axis.
     *
     * Computed as @c hypot(det_plane_x, det_plane_y).
     *
     * @param idx Track index.
     * @return @c r [mm].
     */
    double get_det_plane_r(std::size_t idx) const
    {
        return std::hypot(recotrackdata.at(idx).det_plane_x,
                          recotrackdata.at(idx).det_plane_y);
    }

    /**
     * @brief Azimuthal angle of the extrapolated impact point.
     *
     * Computed as @c atan2(det_plane_y, det_plane_x).
     *
     * @param idx Track index.
     * @return @c φ [rad], in (−π, π].
     */
    double get_det_plane_phi(std::size_t idx) const
    {
        return std::atan2(recotrackdata.at(idx).det_plane_y,
                          recotrackdata.at(idx).det_plane_x);
    }

    /**
     * @brief Magnitude of the transverse angular coefficient vector.
     *
     * Equivalent to @ref get_traj_angcoeff; provided here for naming symmetry
     * with get_traj_angcoeff_phi().
     *
     * @param idx Track index.
     * @return @c hypot(angcoeff_x, angcoeff_y).
     */
    double get_traj_angcoeff_r(std::size_t idx) const
    {
        return std::hypot(recotrackdata.at(idx).traj_angcoeff_x,
                          recotrackdata.at(idx).traj_angcoeff_y);
    }

    /**
     * @brief Azimuthal angle of the track slope vector in the transverse plane.
     *
     * Computed as @c atan2(angcoeff_y, angcoeff_x); describes the direction
     * in which the track is deflected transversely with respect to the beam axis.
     *
     * @param idx Track index.
     * @return @c φ [rad], in (−π, π].
     */
    double get_traj_angcoeff_phi(std::size_t idx) const
    {
        return std::atan2(recotrackdata.at(idx).traj_angcoeff_y,
                          recotrackdata.at(idx).traj_angcoeff_x);
    }

    /// @}

    // -------------------------------------------------------------------------
    /** @name Setters */
    /// @{

    /// @brief Set extrapolated X [mm].
    void set_det_plane_x(std::size_t idx, float val) { recotrackdata_at(idx).det_plane_x = val; }
    /// @brief Set extrapolated Y [mm].
    void set_det_plane_y(std::size_t idx, float val) { recotrackdata_at(idx).det_plane_y = val; }
    /// @brief Set angular coefficient along X.
    void set_traj_angcoeff_x(std::size_t idx, float val) { recotrackdata_at(idx).traj_angcoeff_x = val; }
    /// @brief Set angular coefficient along Y.
    void set_traj_angcoeff_y(std::size_t idx, float val) { recotrackdata_at(idx).traj_angcoeff_y = val; }
    /// @brief Set χ²/NDF.
    void set_chi2ndof(std::size_t idx, float val) { recotrackdata_at(idx).chi2ndof = val; }

    /// @}

    // -------------------------------------------------------------------------
    /** @name I/O */
    /// @{

    /// @brief Clear all track entries and release their memory; also calls @ref alcor_recodata::clear().
    void clear();

    /**
     * @brief Bind branch addresses to an existing TTree for reading.
     * @param input_tree TTree whose @c recotrackdata branch is to be read.
     */
    void link_to_tree(TTree *input_tree);

    /**
     * @brief Create branches on @p output_tree for writing.
     *
     * Registers @c recotrackdata, @c recodata, and @c triggers branches so a
     * single Fill() call persists the complete event.
     *
     * @param output_tree TTree to write into.
     */
    void write_to_tree(TTree *output_tree);

    /// @}

    // -------------------------------------------------------------------------
    /** @name Import from tracking */
    /// @{

    /**
     * @brief Populate track entries from an ALTAI reconstruction vector.
     *
     * Maps each @ref tracking_altai_struct in @p vec to an internal
     * @ref alcor_recotrackdata_struct, preserving insertion order.
     *
     * @param vec Per-track ALTAI output for the current event.
     */
    void import_event(std::vector<tracking_altai_struct> vec);

    /// @}

private:
    // -------------------------------------------------------------------------
    /** @name Internal storage */
    /// @{

    std::vector<alcor_recotrackdata_struct> recotrackdata;                       ///< Per-event track entries.
    std::vector<alcor_recotrackdata_struct> *recotrackdata_ptr = &recotrackdata; ///< Raw pointer used by ROOT branch addressing.

    /// @}
};

// =========================================================================
//  Free functions & constants
// =========================================================================

/**
 * @brief Compute the track angle at the detector plane.
 *
 * Returns @c atan(pixel_position / detector_to_telescope_plane), giving the
 * angle [rad] that the track makes with the beam axis.
 *
 * @param detector_to_telescope_plane Distance from detector to telescope plane [cm].
 * @param pixel_position              Pixel displacement from the beam axis [cm].
 * @return Track angle [rad].
 */
inline double calculate_angle(double detector_to_telescope_plane, double pixel_position)
{
    return std::atan(pixel_position / detector_to_telescope_plane);
}

/**
 * @brief Compute track-angle resolution via error propagation (stub).
 *
 * Propagates uncertainties on both the plane distance and the pixel position
 * through the @c atan formula.
 *
 * @param detector_to_telescope_plane       Nominal distance [cm].
 * @param detector_to_telescope_plane_error Uncertainty on the distance [cm].
 * @param pixel_position                    Nominal pixel displacement [cm].
 * @param pixel_position_error              Uncertainty on the pixel position [cm].
 * @return Track angle resolution [rad]; returns @c -1 until implemented.
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

// --- Detector-to-telescope plane distances [cm] --------------------------

inline const double detector_to_telescope_plane_0 = 235.;                                 ///< Distance to plane 0 [cm].
inline const double detector_to_telescope_plane_1 = detector_to_telescope_plane_0 + 2.55; ///< Distance to plane 1 [cm].
inline const double detector_to_telescope_plane_2 = detector_to_telescope_plane_1 + 22.5; ///< Distance to plane 2 [cm].
inline const double detector_to_telescope_plane_3 = detector_to_telescope_plane_2 + 2.55; ///< Distance to plane 3 [cm].

// --- Pixel resolutions per plane [cm] ------------------------------------

inline const double pixel_resolution_plane_0 = 3.7e-3; ///< Plane 0 pixel resolution [cm].
inline const double pixel_resolution_plane_1 = 3.4e-3; ///< Plane 1 pixel resolution [cm].
inline const double pixel_resolution_plane_2 = 3.4e-3; ///< Plane 2 pixel resolution [cm].
inline const double pixel_resolution_plane_3 = 3.7e-3; ///< Plane 3 pixel resolution [cm].