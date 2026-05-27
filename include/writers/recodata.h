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
 * @param trigger_conf           Path to the trigger TOML configuration file.
 * @param framer_conf            Path to the framer TOML configuration file.
 * @param recodata_conf          Path to the recodata-side TOML
 *                               configuration (coverage map, per-ring
 *                               photon counting).  Defaults to
 *                               `"conf/recodata.toml"`; `--QA` mode in
 *                               the CLI resolves this to
 *                               `"conf/QA/recodata.toml"` when present.
 */
void recodata_writer(
    std::string data_repository,
    std::string run_name,
    int max_spill = 1000,
    bool force_rebuild = false,
    bool force_upstream = false,
    std::string mapping_conf = "conf/mapping_conf.2025.toml",
    std::string trigger_conf = "conf/trigger_conf.toml",
    std::string framer_conf = "conf/framer_conf.toml",
    std::string recodata_conf = "conf/recodata.toml",
    /**
     * Streaming-pipeline conf for the cascade.  Recodata itself does NOT
     * read this file (the streaming Hough trigger runs upstream in
     * `lightdata_writer`); it's accepted here purely so it can be
     * forwarded into the `force_upstream` cascade.  This is what makes
     * `recodata_writer --force-upstream --QA` propagate the QA Hough
     * thresholds through to the lightdata stage.
     */
    std::string streaming_conf = "conf/streaming.toml");
