#pragma once

/**
 * @file utilities/btana_dump.h
 * @brief Human-readable decoder for the framework's three on-disk formats.
 *
 * The `btana-dump` CLI is a thin wrapper around the functions declared
 * here.  They live in the framework library so a macro or test can also
 * call them — same printed format as the CLI, no copy-paste.
 *
 * Auto-detection is by tree name:
 *
 *  | Tree name        | Format          | Per-entry meaning |
 *  |------------------|-----------------|-------------------|
 *  | `lightdata`      | lightdata       | one spill (N frames inside) |
 *  | `recodata`       | recodata        | one frame |
 *  | `recotrackdata`  | recotrackdata   | one frame (with ALTAI tracks) |
 *
 * If multiple matching trees are present the lookup prefers
 * `recotrackdata` → `recodata` → `lightdata` (richest format first).
 *
 * Print volume is bounded by `n_entries`; pass `-1` for "all".  Output
 * goes to `std::cout`; nothing is written to disk and the framework
 * library state (calibration table, registries) is left untouched.
 */

#include <cstddef>
#include <string>

namespace btana::utilities
{

/// Detected on-disk format.
enum class DumpFormat
{
    Unknown,
    Lightdata,
    Recodata,
    Recotrackdata,
    /// Pulser-calibration QA file (no tree; Config/ + Spreads/ + RunSummary/
    /// + Fits/ TDirectories of scalars and histograms).
    PulserCalibQa,
};

/// @return human-readable name of @p f (e.g. "lightdata").
const char *format_name(DumpFormat f);

/// Open @p file_path read-only and identify the on-disk format by
/// looking for known tree names.  Returns `Unknown` if no match.
DumpFormat detect_format(const std::string &file_path);

/// Pretty-print the first @p n_entries spills from @p file_path.  Each
/// spill prints a one-line summary plus, for the first few spills, a
/// per-frame breakdown (frame ID, trigger names + times, hit counts).
/// @return `0` on success, non-zero on failure.
int dump_lightdata(const std::string &file_path, long n_entries = 5);

/// Pretty-print the first @p n_entries frames from @p file_path.  Each
/// frame prints its hit count + per-trigger entries (index, name from
/// the built-in @c default_names table, coarse cc, fine_time ns).
int dump_recodata(const std::string &file_path, long n_entries = 5);

/// Pretty-print the first @p n_entries frames + track entries from
/// @p file_path.  Per-frame output is recodata-style plus a
/// `tracks=...` line summarising N_tracks with `det_plane_(x,y)`,
/// `r`, `chi2/ndf`.
int dump_recotrackdata(const std::string &file_path, long n_entries = 5);

/// Pretty-print embedded metadata from a pulser-calibration QA file
/// (no tree, just TDirectories of TParameter / TNamed / histograms).
int dump_pulser_calib_qa(const std::string &file_path);

/// Auto-detect dispatcher.  Wraps the three format-specific functions
/// above and prints a one-line header identifying which it picked.
/// @return `0` on success; `2` if format is unknown; format dispatcher's
///         return code otherwise.
int dump_file(const std::string &file_path, long n_entries = 5);

} // namespace btana::utilities
