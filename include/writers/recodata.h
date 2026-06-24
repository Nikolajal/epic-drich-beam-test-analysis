#pragma once

/**
 * @file recodata_writer.h
 * @brief Entry point for building reconstructed-data ROOT files from lightdata.
 *
 * Provides @ref recodata_writer, which reads an existing lightdata TTree,
 * applies channel Mapping, fine-time calibration, and optional ring finding,
 * and writes a recodata TTree suitable for physics analysis.
 */

#include <string>

/// @brief Minimum separation between two valid same-type triggers (clock cycles, ~25 ns).
#define BTANA_TRIGGER_MIN_SEPARATION 16

/// @brief Edge-rejection guard window applied at spill boundaries (ns).
#define BTANA_EDGE_REJECTION_NS 25

/**
 * @brief Build a recodata ROOT file from an existing lightdata file.
 *
 * If an up-to-date recodata file already exists and @p force_rebuild is
 * @c false the function returns immediately.  Pass @p force_upstream =
 * @c true to also rebuild the upstream lightdata file before
 * reconstruction (the flag cascades: lightdata is invoked with
 * `force_rebuild = true`).
 *
 * @param data_repository        Root directory containing the run folder.
 * @param run_name               Run identifier (sub-directory name).
 * @param max_spill              Maximum number of spills to process (default 1000).
 * @param force_rebuild          If @c true, overwrite any existing recodata file.
 * @param force_upstream         If @c true, also rebuild the lightdata
 *                               file first (`lightdata_writer` is invoked
 *                               with `force_rebuild = true`).
 * @param mapping_conf           Path to the Mapping TOML calibration file.
 *                               Also carries the `[coverage]` table with
 *                               the coverage-map geometry (formerly in
 *                               `recodata.toml`).
 * @param trigger_conf           Path to the trigger TOML configuration file.
 * @param framer_conf            Path to the framer TOML configuration file.
 */
void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill = 1000,
    bool force_rebuild = false,
    bool force_upstream = false,
    std::string mapping_conf = "conf/mapping_conf.toml",
    std::string trigger_conf = "conf/trigger_conf.toml",
    std::string framer_conf = "conf/framer_conf.toml",
    /**
     * Streaming-pipeline conf.  The `[streaming_ransac]` table now also
     * carries the recodata ring-reconstruction knobs (hardware ring
     * time window, min_hits_per_ring, delta_r, skip_loo_residuals),
     * which `recodata_writer` reads directly via `recodata_conf_reader`.
     * The streaming *trigger* + RANSAC stages themselves run upstream in
     * `lightdata_writer`; this file is also forwarded into the
     * `force_upstream` cascade so `recodata_writer --force-upstream --QA`
     * propagates the QA RANSAC thresholds through to the lightdata stage.
     */
    std::string streaming_conf = "conf/streaming.toml",
    /**
     * Cherenkov ring-shape policy for the radial coordinate:
     *   "auto"    — classify circle vs ellipse from the fit (default),
     *   "circle"  — force a circular ring (legacy R; --force-ring),
     *   "ellipse" — force the elliptical radius ρ (--force-ellipse).
     * Drives the per-trigger N_γ radial remap + the hitmap overlay.
     */
    std::string ring_shape_mode = "auto");
