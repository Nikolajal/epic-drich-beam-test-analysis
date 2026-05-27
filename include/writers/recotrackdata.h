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
 * Reads the recodata file for @p run_name (rebuilding upstream stages if the
 * force flags are set) and the ALTAI track file under @p track_data_repository
 * / @p track_run_name.  Matched events are written to a recotrackdata TTree.
 *
 * Force-flag semantics (uniform across all writers):
 *  - @p force_rebuild  — overwrite this writer's output (the recotrackdata
 *                       file).  No effect on upstream stages.
 *  - @p force_upstream — cascade: also rebuild every writer upstream of
 *                       this one.  Internally invokes
 *                       `recodata_writer(force_rebuild = true,
 *                       force_upstream = true)`, which in turn cascades
 *                       to `lightdata_writer`.
 *
 * @param data_repository        Root directory containing the detector run folder.
 * @param run_name               Detector run identifier (sub-directory name).
 * @param track_data_repository  Root directory containing the telescope run folder.
 * @param track_run_name         Telescope run identifier (sub-directory name).
 * @param max_frames             Maximum number of frames to process (default 10 000 000).
 * @param force_rebuild          If @c true, overwrite any existing recotrackdata file.
 * @param force_upstream         If @c true, also rebuild every upstream
 *                               writer (recodata, then lightdata).
 */
void recotrackdata_writer(
    std::string data_repository,
    std::string run_name,
    std::string track_data_repository,
    std::string track_run_name,
    int max_frames = 10000000,
    bool force_rebuild = false,
    bool force_upstream = false);
