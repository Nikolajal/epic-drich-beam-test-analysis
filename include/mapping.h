#pragma once

#include "utility.h"
#include "alcor_finedata.h"
#include <toml++/toml.h>
#include <unordered_map>
#include <cassert>
#include "utility/toml_utils.h"

/**
 * @file Mapping.h
 * @brief Detector channel-to-position Mapping for the ePIC dRICH beam test.
 *
 * Provides a full chain of lookups from raw ALCOR readout identifiers
 * (device, chip, EO channel) to physical (x, y) Hit coordinates on the
 * SiPM plane, exposes pre-built caches for performance-critical loops,
 * and resolves the HV bias line (channel) that powers any given pixel.
 *
 * ### Coordinate chain
 * ```
 * (device, chip) ──► (PDU, matrix)     [loaded from calibration file]
 *       EO channel ──► DO channel       [static look-up table per matrix]
 *  (DO channel, matrix) ──► (col, row)  inside the PDU 16×16 grid
 *       (PDU, col, row) ──► (x, y) mm   [PDU origin + optional 180° rotation]
 * ```
 *
 * ### HV channel identification
 * Within each matrix quadrant the 64 pixels are arranged in an 8×8 block.
 * HV bias lines run either along columns (VERTICAL) or rows (HORIZONTAL),
 * as configured per-matrix in the calibration TOML under [hv_line_orientation].
 * - VERTICAL   → HV line index = do_channel / 8   (0–7, same x)
 * - HORIZONTAL → HV line index = do_channel % 8   (0–7, same y)
 *
 * @todo Add a method to generate a full-coverage map (Cartesian and R-φ).
 * @todo Profile cache benefit vs. on-the-fly computation.
 */

// ---------------------------------------------------------------------------
// Ancillary types used by both the Mapping class and external analysis code
// ---------------------------------------------------------------------------

/**
 * @brief Orientation of a physical line of 8 adjacent SiPM pixels.
 *
 * VERTICAL   lines share the same x coordinate (constant column index).
 * HORIZONTAL lines share the same y coordinate (constant row index).
 * Used both for geometry analysis and HV bias-line identification.
 */
enum class line_orientation_type
{
    Vertical,
    Horizontal
};

/**
 * @brief Fully-qualified identifier of the HV bias line powering one pixel.
 *
 * The HV line index (0–7) selects one of the eight parallel bias lines
 * within the matrix's 8×8 block, in the direction given by @c orientation.
 */
struct HvChannelAddress
{
    int pdu_index;                     ///< PDU index [1–8].
    int matrix_index;                  ///< Matrix quadrant index [1–4].
    int hv_line_index;                 ///< HV line index within the matrix [0–7].
    line_orientation_type orientation; ///< VERTICAL (column lines) or HORIZONTAL (row lines).
};

// ---------------------------------------------------------------------------

class Mapping
{
public:
    // -------------------------------------------------------------------------
    /** @name Construction & calibration I/O */
    /// @{

    /**
     * @brief Construct and immediately load a calibration file.
     * @param conf_file_name Path to a TOML calibration file containing
     *        @c pdu_xy_position, @c pdu_rotation, @c device_chip_to_pdu_matrix,
     *        and @c hv_line_orientation tables.
     */
    explicit Mapping(std::string conf_file_name);

    /**
     * @brief Load (or reload) calibration data from a TOML file.
     *
     * Populates the four mutable calibration maps:
     * - @c pdu_xy_position        – physical centre of each PDU in mm.
     * - @c pdu_rotation           – whether the PDU is rotated 180°.
     * - @c device_chip_to_pdu_matrix – ALCOR (device, chip) → (PDU, matrix).
     * - @c hv_line_orientation    – HV bias line direction per matrix [1–4].
     *
     * Any previously cached index↔position data is invalidated on reload.
     *
     * @param filename Path to the TOML calibration file.
     * @param verbose  If @c true, emit verbose diagnostic messages (unused atm).
     */
    void load_calib(std::string filename, bool verbose = false);

    /// @}

    // -------------------------------------------------------------------------
    /** @name Single-Hit position look-ups
     *
     *  All getters return @c std::nullopt when the requested identifier is
     *  out of range or not present in the calibration tables.
     */
    /// @{

    /**
     * @brief Translate an EO channel index to a DO channel index for the given matrix.
     *
     * The EO→DO permutation is hard-coded per matrix (1–4) and accounts for
     * the physical wire routing inside each ALCOR ASIC quadrant.
     *
     * @param matrix     Matrix index [1–4].
     * @param eo_channel EO channel index [0–63].
     * @return DO channel index [0–63], or @c std::nullopt if inputs are invalid.
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
     * Pixel pitch is 3.2 mm with guard-ring offsets, plus a 0.3 mm inter-half
     * gap between columns/rows 7 and 8 of the 16×16 grid.
     * The PDU origin from @c pdu_xy_position is added last.
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
     * Resolves EO→DO channel, computes the 16×16 grid address (applying the
     * matrix quadrant offset), then applies the optional 180° rotation before
     * delegating to get_position_from_pdu_column_row().
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
    std::optional<std::array<float, 2>> get_position_from_finedata(AlcorFinedata entry) const
    {
        return get_position_from_device_chip_eoch(entry.get_device(), entry.get_chip(), entry.get_eo_channel());
    }

    /**
     * @brief Compute the physical position from a @ref GlobalIndex.
     *
     * Preferred form — pass a strongly-typed @ref GlobalIndex obtained from
     * @ref GlobalIndex::from_components, @ref GlobalIndex::from_legacy, or
     * @ref GlobalIndex::from_legacy_channel.
     *
     * @param gi Validated @ref GlobalIndex identifying the channel.
     * @return Physical position {x, y} in mm, or @c std::nullopt if unmapped.
     */
    //  Bodies in mapping.cxx — these methods take/use ::GlobalIndex
    //  whose full definition is heavier than we want in this header.
    std::optional<std::array<float, 2>> get_position_from_global_index(::GlobalIndex gi) const;

    /**
     * @brief Convenience overload — compute the physical position from the
     *        raw 32-bit value stored on a Hit.
     *
     * Wraps the @p stored_raw into a @ref GlobalIndex and delegates to the
     * value-type overload above.  Use this when calling from code that holds
     * a `uint32_t` (e.g. the stored `internal_data.GlobalIndex` field on
     * `AlcorFinedataStruct`).
     *
     * @param stored_raw  Stored new-layout @ref GlobalIndex raw.
     * @return Physical position {x, y} in mm, or @c std::nullopt if unmapped.
     */
    std::optional<std::array<float, 2>> get_position_from_global_index(int stored_raw) const;

    /**
     * @brief Fill the @c hit_x / @c hit_y fields of a fine-data struct in-place.
     *
     * Sets the fields to @c 9.f as a sentinel when the channel is not mapped.
     *
     * @param entry Fine-data struct to annotate; modified in-place.
     */
    void assign_position(AlcorFinedataStruct &entry);

    /// @}

    // -------------------------------------------------------------------------
    /** @name HV bias-line identification */
    /// @{

    /**
     * @brief Retrieve the HV bias line orientation for a matrix quadrant.
     *
     * The orientation is loaded from the @c [hv_line_orientation] TOML table
     * and controls how the DO channel index is decoded into an HV line index:
     * - @c VERTICAL   → HV line index = do_channel / 8  (column, same x).
     * - @c HORIZONTAL → HV line index = do_channel % 8  (row, same y).
     *
     * @param matrix_index Matrix index [1–4].
     * @return Orientation enum value, or @c std::nullopt if not configured.
     */
    std::optional<line_orientation_type> get_hv_line_orientation(int matrix_index) const;

    /**
     * @brief Identify the HV bias line powering the pixel at a global TDC index.
     *
     * Decodes (device, chip, EO channel) from @p GlobalIndex, resolves
     * (PDU, matrix, DO channel), then applies the per-matrix HV orientation to
     * determine which of the eight bias lines (0–7) supplies that pixel.
     *
     * The returned @c HvChannelAddress contains the PDU, matrix, line index,
     * and orientation — sufficient to uniquely identify the bias line across
     * the full detector.
     *
     * @param GlobalIndex Packed global TDC channel index.
     * @return Fully-qualified HV channel address, or @c std::nullopt if the
     *         channel is unmapped or HV orientation is not configured.
     */
    std::optional<HvChannelAddress> get_hv_channel_from_global_index(int GlobalIndex) const;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Position cache construction
     *
     *  Pre-computed bidirectional look-up tables between global TDC indices and
     *  physical (x, y) positions.  Building the caches once before event loops
     *  is significantly faster than calling the full look-up chain per Hit.
     *  Caches are invalidated automatically when load_calib() is called.
     */
    /// @{

    /**
     * @brief Build the index → position cache.
     *
     * Iterates over all valid global TDC indices (step 4, covering 2048 chips
     * × 4 EO-channel groups) and resolves each to a physical position.
     * Channels that are unmapped or fall within @p origin_cut of the origin
     * (sentinel for absent/dead pixels) are skipped.
     *
     * @param origin_cut Radius threshold in mm; hits with |x| < origin_cut
     *                   AND |y| < origin_cut are excluded (default: 5.0 mm).
     */
    void build_index_to_position_cache(float origin_cut = 5.f);

    /**
     * @brief Build the position → index reverse cache.
     *
     * Inverts the index→position cache; builds it implicitly if not yet done.
     *
     * When two channels share the same nominal position, @p collision_policy
     * controls the outcome:
     * - @c "first"  – keep the first index seen (default).
     * - @c "last"   – overwrite with the last index seen.
     * - @c "warn"   – keep first and log a warning per collision.
     *
     * @param collision_policy One of @c "first", @c "last", @c "warn".
     */
    void build_position_to_index_cache(std::string collision_policy = "first");

    /**
     * @brief Query the index → position cache.
     *
     * The cache is keyed by ``4 * GlobalIndex::channel_ordinal()`` — see
     * @ref build_index_to_position_cache.  That formula collapses the four
     * tdc values of a given physical channel onto a single slot, so callers
     * must pass a key with the bottom two bits zero (i.e. the tdc=0
     * representative).  Passing a key with @c key % 4 != 0 would silently
     * miss the cache and return @c std::nullopt; the assertion below makes
     * that bug loud in debug builds (release: silent miss preserved).
     *
     * @param channel_ordinal_times_four  Cache key, equal to
     *                                    @c 4 * GlobalIndex::channel_ordinal()
     *                                    for the queried physical channel.
     * @return Cached {x, y} in mm, or @c std::nullopt if not in cache.
     */
    std::optional<std::array<float, 2>> get_cached_position(int channel_ordinal_times_four) const
    {
        assert(channel_ordinal_times_four % 4 == 0 &&
               "get_cached_position: key must be 4 * channel_ordinal "
               "(the cache stores tdc=0 representatives only)");
        auto it = index_to_hit_xy.find(channel_ordinal_times_four);
        return (it != index_to_hit_xy.end()) ? std::optional{it->second} : std::nullopt;
    }

    /**
     * @brief Query the position → index reverse cache.
     * @param x X coordinate in mm.
     * @param y Y coordinate in mm.
     * @return Cached global TDC index, or @c std::nullopt if not in cache.
     */
    std::optional<int> get_cached_index(float x, float y) const
    {
        auto it = hit_xy_to_index.find({x, y});
        return (it != hit_xy_to_index.end()) ? std::optional{it->second} : std::nullopt;
    }

    /**
     * @brief Read-only access to the full index → position cache.
     *
     * Useful for iterating over all active channels, e.g. when filling
     * occupancy histograms or building detector response matrices.
     */
    const std::unordered_map<int, std::array<float, 2>> &get_index_to_position_map() const { return index_to_hit_xy; }

    /**
     * @brief Read-only access to the full position → index reverse cache.
     *        Key is {x, y} in mm as a pair of floats.
     */
    const std::map<std::array<float, 2>, int> &get_position_to_index_map() const { return hit_xy_to_index; }

    /// @}

    // -------------------------------------------------------------------------
    /** @name Raw calibration table accessors */
    /// @{

    /**
     * @brief Read-only access to the (device, chip) → (PDU, matrix) calibration map.
     *
     * Useful for enumerating all active (PDU, matrix) pairs, e.g. when
     * building per-matrix electronics line structures.
     */
    const std::map<std::array<int, 2>, std::array<int, 2>> &get_device_chip_to_pdu_matrix_map() const { return device_chip_to_pdu_matrix; }

    /// @}

private:
    // -------------------------------------------------------------------------
    /** @name Channel-routing tables (compile-time constants) */
    /// @{

    /// EO→DO channel permutation for each of the four matrix quadrants.
    /// Indexed by matrix [1–4]; inner vector has 64 entries (one per
    /// EO channel).  Kept @c static because the EO→DO permutation is
    /// a fixed ALCOR hardware property — not mutated by load_calib —
    /// and sharing the table across instances has no concurrency
    /// downside (read-only after initialisation).
    static const std::map<int, std::vector<int>> matrix_to_do_channel;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Calibration data (loaded at runtime, per-instance) */
    /// @{

    //  These four maps are populated by load_calib at construction
    //  time from the mapping TOML.  They are PER-INSTANCE rather than
    //  static because (a) closes D-10's "two Mapping instances
    //  silently share state" trap and (b) supports any future
    //  reload-mid-run feature without racing against framer workers.
    //  No API change for callers — Mapping is already constructed
    //  per-instance everywhere (`Mapping current_mapping(conf)`).

    /// Physical centre position {x, y} in mm for each PDU, keyed by PDU index.
    std::map<int, std::array<float, 2>> pdu_xy_position;

    /// 180° rotation flag per PDU. @c true means column/row are mirrored as
    /// (15 − col, 15 − row) before the position lookup.
    std::map<int, bool> pdu_rotation;

    /// Maps (device, chip) pairs to their (PDU, matrix) assignment.
    std::map<std::array<int, 2>, std::array<int, 2>> device_chip_to_pdu_matrix;

    /// HV bias line orientation per matrix quadrant [1–4].
    /// VERTICAL  → bias lines run along columns (do_channel / 8).
    /// HORIZONTAL → bias lines run along rows    (do_channel % 8).
    std::map<int, line_orientation_type> hv_line_orientation;

    /// @}

    // -------------------------------------------------------------------------
    /** @name Position caches */
    /// @{

    //  index_to_hit_xy is `std::unordered_map` for O(1) hot-path
    //  lookups — the framer workers hit `get_position_from_*` on
    //  every Hit, and the ~2k-entry table was log-N tree walks
    //  under `std::map`.  The reverse `hit_xy_to_index` keeps
    //  `std::map` because the float-pair key would need a custom
    //  hasher; see D-10 option D (rounded-integer-mm key) for the
    //  proper fix.
    std::unordered_map<int, std::array<float, 2>> index_to_hit_xy; ///< global index → {x, y} mm.
    std::map<std::array<float, 2>, int> hit_xy_to_index;           ///< {x, y} mm → global index.
    bool cache_index_to_xy_built{false};
    bool cache_xy_to_index_built{false};

    /// @}
};