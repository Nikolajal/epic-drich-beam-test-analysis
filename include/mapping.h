#pragma once

#include "utility.h"
#include "alcor_finedata.h"
#include <toml++/toml.h>

/**
 * @file mapping.h
 * @brief Detector channel-to-position mapping for the ePIC dRICH beam test.
 *
 * Provides a full chain of lookups from raw ALCOR readout identifiers
 * (device, chip, EO channel) to physical (x, y) hit coordinates on the
 * SiPM plane, and exposes pre-built caches for performance-critical loops.
 *
 * ### Coordinate chain
 * ```
 * (device, chip) ──► (PDU, matrix)   [loaded from calibration file]
 *       EO channel ──► DO channel     [static look-up table per matrix]
 *  (DO channel, matrix) ──► (col, row) inside the PDU 16×16 grid
 *       (PDU, col, row) ──► (x, y) mm  [PDU origin + optional 180° rotation]
 * ```
 *
 * @todo Add a method to generate a full-coverage map (Cartesian and R-φ).
 * @todo Profile cache benefit vs. on-the-fly computation.
 */
class mapping
{
public:
    // -------------------------------------------------------------------------
    /** @name Construction & calibration I/O */
    /// @{

    /**
     * @brief Construct and immediately load a calibration file.
     * @param conf_file_name Path to a TOML calibration file containing
     *        @c pdu_xy_position, @c pdu_rotation, and
     *        @c device_chip_to_pdu_matrix tables.
     */
    explicit mapping(std::string conf_file_name);

    /**
     * @brief Load (or reload) calibration data from a TOML file.
     *
     * Populates the three mutable calibration maps:
     * - @c pdu_xy_position  – physical centre of each PDU in mm.
     * - @c pdu_rotation     – whether the PDU is rotated 180°.
     * - @c device_chip_to_pdu_matrix – ALCOR (device, chip) → (PDU, matrix).
     *
     * Any previously cached index↔position data is invalidated on reload.
     *
     * @param filename Path to the TOML calibration file.
     * @param verbose  If @c true, emit verbose diagnostic messages (unused atm).
     */
    void load_calib(std::string filename, bool verbose = false);

    /// @}

    // -------------------------------------------------------------------------
    /** @name Single-hit position look-ups
     *
     *  All getters return @c std::nullopt when the requested identifier is
     *  out of range or not present in the calibration tables, making it safe
     *  to call without prior validity checks.
     */
    /// @{

    /**
     * @brief Translate an EO (even/odd) channel index to a DO (data-out)
     *        channel index for the given matrix quadrant.
     *
     * The EO→DO permutation is hard-coded per matrix (1–4) and accounts for
     * the physical wire routing inside each ALCOR ASIC quadrant.
     *
     * @param matrix     Matrix index [1–4].
     * @param eo_channel EO channel index [0–63].
     * @return DO channel index, or @c std::nullopt if inputs are invalid.
     */
    std::optional<int> get_do_channel(int matrix, int eo_channel) const;

    /**
     * @brief Retrieve the (PDU, matrix) pair for a given ALCOR (device, chip).
     * @param device ALCOR device index.
     * @param chip   ALCOR chip index within the device.
     * @return Array @c {pdu, matrix}, or @c std::nullopt if not in calibration.
     */
    std::optional<std::array<int, 2>> get_pdu_matrix(int device, int chip) const;

    /**
     * @brief Compute the physical (x, y) position from a PDU grid address.
     *
     * The intra-PDU coordinate is computed from the pixel pitch (3.2 mm),
     * guard-ring offsets, and an extra 0.3 mm gap between the two halves of
     * the 16×16 grid (columns/rows 7→8).  The PDU origin is then added from
     * @c pdu_xy_position.
     *
     * @param pdu    PDU index [1–8].
     * @param column Column in the 16×16 PDU grid [0–15].
     * @param row    Row    in the 16×16 PDU grid [0–15].
     * @return Physical position {x, y} in mm, or @c std::nullopt on bad input.
     */
    std::optional<std::array<float, 2>> get_position_from_pdu_column_row(int pdu, int column, int row) const;

    /**
     * @brief Compute the physical position from a (PDU, matrix, EO channel) triplet.
     *
     * Internally calls get_do_channel() to resolve the column/row within the
     * PDU grid, then applies the optional 180° rotation before delegating to
     * get_position_from_pdu_column_row().
     *
     * @param pdu        PDU index [1–8].
     * @param matrix     Matrix index [1–4].
     * @param eo_channel EO channel index [0–63].
     * @return Physical position {x, y} in mm, or @c std::nullopt on failure.
     */
    std::optional<std::array<float, 2>> get_position_from_pdu_matrix_eoch(int pdu, int matrix, int eo_channel) const;

    /**
     * @brief Compute the physical position from an ALCOR (device, chip, EO channel) triplet.
     *
     * Looks up (PDU, matrix) via the calibration table, then delegates to
     * get_position_from_pdu_matrix_eoch().
     *
     * @param device     ALCOR device index.
     * @param chip       ALCOR chip index.
     * @param eo_channel EO channel index [0–63].
     * @return Physical position {x, y} in mm, or @c std::nullopt if unmapped.
     */
    std::optional<std::array<float, 2>> get_position_from_device_chip_eoch(int device, int chip, int eo_channel) const;

    /**
     * @brief Compute the physical position directly from a decoded ALCOR fine-data word.
     * @param entry Decoded fine-data word providing device, chip, and EO channel.
     * @return Physical position {x, y} in mm, or @c std::nullopt if unmapped.
     */
    std::optional<std::array<float, 2>> get_position_from_finedata(alcor_finedata entry) const;

    /**
     * @brief Compute the physical position from a global TDC channel index.
     *
     * The global index encodes (device, chip, EO channel) into a single integer
     * via the utility functions get_device_from_global_tdc_index() etc.
     *
     * @param global_index Packed global TDC channel index.
     * @return Physical position {x, y} in mm, or @c std::nullopt if unmapped.
     */
    std::optional<std::array<float, 2>> get_position_from_global_index(int global_index) const;

    /**
     * @brief Fill the @c hit_x / @c hit_y fields of a fine-data struct in-place.
     *
     * Sets the fields to @c -99.f as a sentinel when the channel is not mapped.
     *
     * @param entry Fine-data struct to annotate; modified in-place.
     */
    void assign_position(alcor_finedata_struct &entry);

    /// @}

    // -------------------------------------------------------------------------
    /** @name Position cache construction
     *
     *  These methods pre-compute bidirectional look-up tables between global
     *  TDC channel indices and physical (x, y) positions.  Building the caches
     *  once and reusing them is significantly faster than calling the full
     *  look-up chain inside tight event loops.
     *
     *  The caches are invalidated automatically when load_calib() is called.
     */
    /// @{

    /**
     * @brief Build the @c index → position cache.
     *
     * Iterates over all valid global TDC indices (step 4, covering 2048 chips
     * × 4 EO-channel groups) and resolves each to a physical position.
     * Channels that are unmapped or whose position falls within a 5 mm radius
     * of the origin (used as a sentinel for absent/dead pixels) are skipped.
     *
     * After this call, the cache can be queried with get_cached_position().
     *
     * @note The 5 mm origin cut mirrors the filter applied in the standard
     *       event-loop snippet; adjust @p origin_cut if the geometry changes.
     *
     * @param origin_cut Radius threshold in mm below which a hit is considered
     *                   invalid (default: 5.0 mm, applied independently on x
     *                   and y via @c fabs).
     */
    void build_index_to_position_cache(float origin_cut = 5.f);

    /**
     * @brief Build the @c position → index reverse cache.
     *
     * Inverts the index→position cache.  Must be called *after*
     * build_index_to_position_cache() (or will build it implicitly if not yet
     * done).
     *
     * Because two distinct channels can in principle share the same nominal
     * position (e.g. dead-pixel replacements), a @p collision_policy argument
     * controls the behaviour:
     * - @c "first"  – keep the first index seen (default, safe for typical use).
     * - @c "last"   – overwrite with the last index seen.
     * - @c "warn"   – keep first and emit a warning for every collision.
     *
     * After this call, the cache can be queried with get_cached_index().
     *
     * @param collision_policy One of @c "first", @c "last", @c "warn".
     */
    void build_position_to_index_cache(std::string collision_policy = "first");

    /**
     * @brief Query the index→position cache built by build_index_to_position_cache().
     * @param global_index Global TDC channel index to look up.
     * @return Cached {x, y} position, or @c std::nullopt if not in cache.
     */
    std::optional<std::array<float, 2>> get_cached_position(int global_index) const;

    /**
     * @brief Query the position→index cache built by build_position_to_index_cache().
     * @param x X coordinate in mm.
     * @param y Y coordinate in mm.
     * @return Cached global TDC index, or @c std::nullopt if not in cache.
     */
    std::optional<int> get_cached_index(float x, float y) const;

    /**
     * @brief Read-only access to the full index→position cache map.
     *
     * Useful for iterating over all active channels, e.g. when filling
     * occupancy histograms or building detector response matrices.
     *
     * @return Const reference to the internal @c index_to_hit_xy map.
     */
    const std::map<int, std::array<float, 2>> &get_index_to_position_map() const;

    /**
     * @brief Read-only access to the full position→index cache map.
     * @return Const reference to the internal @c hit_xy_to_index map.
     *         The key is encoded as a pair of floats {x, y}.
     */
    const std::map<std::array<float, 2>, int> &get_position_to_index_map() const;

    /// @}

private:
    // -------------------------------------------------------------------------
    /** @name Static channel-routing tables (compile-time constants) */
    /// @{

    /// EO→DO channel permutation for each of the four matrix quadrants.
    /// Indexed by matrix [1–4]; inner vector has 64 entries (one per EO channel).
    static std::map<int, std::vector<int>> matrix_to_do_channel;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Calibration data (loaded at runtime) */
    /// @{

    /// Physical centre position {x, y} in mm for each PDU, keyed by PDU index.
    static std::map<int, std::array<float, 2>> pdu_xy_position;

    /// 180° rotation flag per PDU.  @c true means column/row are mirrored as
    /// (15 − col, 15 − row) before the position lookup.
    static std::map<int, bool> pdu_rotation;

    /// Maps (device, chip) pairs to their (PDU, matrix) assignment.
    static std::map<std::array<int, 2>, std::array<int, 2>> device_chip_to_pdu_matrix;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Position caches */
    /// @{

    /// Cached map from global TDC index to physical position.
    /// Populated by build_index_to_position_cache().
    std::map<int, std::array<float, 2>> index_to_hit_xy;

    /// Cached reverse map from physical position to global TDC index.
    /// Populated by build_position_to_index_cache().
    std::map<std::array<float, 2>, int> hit_xy_to_index;

    /// Tracks whether index_to_hit_xy has been built.
    bool cache_index_to_xy_built{false};

    /// Tracks whether hit_xy_to_index has been built.
    bool cache_xy_to_index_built{false};

    /// @}
};