#pragma once

/**
 * @file recotrackdata_writer.h
 * @brief Entry point for building track-matched reconstructed-data ROOT files.
 *
 * Provides @ref recotrackdata_writer, which joins an existing recodata TTree
 * with ALTAI telescope tracks and writes a recotrackdata TTree.
 */

#include <string>
#include "tracking_altai.h"

/**
 * @brief Build a recotrackdata ROOT file by merging recodata with ALTAI tracks.
 *
 * Reads the recodata file for @p run_name (rebuilding upstream files if the
 * force flags are set) and the ALTAI track file under @p track_data_repository
 * / @p track_run_name.  Matched events are written to a recotrackdata TTree.
 *
 * @param data_repository        Root directory containing the detector run folder.
 * @param run_name               Detector run identifier (sub-directory name).
 * @param track_data_repository  Root directory containing the telescope run folder.
 * @param track_run_name         Telescope run identifier (sub-directory name).
 * @param max_frames             Maximum number of frames to process (default 10 000 000).
 * @param force_recodata_rebuild  If @c true, rebuild the recodata file first.
 * @param force_lightdata_rebuild If @c true, rebuild the lightdata file first.
 */
void recotrackdata_writer(
    std::string data_repository,
    std::string run_name,
    std::string track_data_repository,
    std::string track_run_name,
    int max_frames = 10000000,
    bool force_recodata_rebuild = false,
    bool force_lightdata_rebuild = false);
