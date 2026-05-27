#pragma once

/**
 * @file conf_path.h
 * @brief Path-resolution helper for writer mode-flags (`--QA`, `--calib`).
 *
 * Each writer's CLI exposes mode flags that route config-file lookup
 * through a `conf/<mode>/` sub-folder first (overrides for that mode),
 * falling back to `conf/<basename>` if no override exists.  Live modes:
 *
 *   --QA      → `conf/QA/`     fast-feedback operator tuning
 *   --calib   → `conf/calib/`  pulser-driven fine-time calibration
 *
 * Modes are mutually exclusive at the CLI level (only one mode flag
 * may be set per invocation); the writer mains enforce that.
 *
 * Usage (CLI main, after `CLI11_PARSE`):
 *
 * ```cpp
 * std::string streaming_conf;                            // unset sentinel
 * auto *p_streaming = app.add_option("--streaming-conf", streaming_conf);
 * // ... after parse ...
 * const std::string mode = qa_mode ? "QA" : (calib_mode ? "calib" : "");
 * if (p_streaming->count() == 0)
 *     streaming_conf = util::conf_path("streaming.toml", mode);
 * ```
 *
 * Files that exist only under `conf/<mode>/` will load even when the
 * mode flag is false (if a user explicitly passes the path on the
 * CLI), because this helper only acts when the CLI option was NOT set.
 */

#include <filesystem>
#include <string>

namespace util
{

/**
 * @brief Resolve a config-file basename to its actual path.
 *
 * Subdir form: when @p subdir is non-empty and `conf/<subdir>/<basename>`
 * exists, return that.  Otherwise fall back to `conf/<basename>`.
 *
 * @param basename   Filename inside `conf/` (e.g. `"streaming.toml"`).
 *                   Should NOT carry a directory prefix.
 * @param subdir     Sub-folder under `conf/` to check first.  Empty
 *                   string disables the override (production lookup).
 *                   Conventional values: `"QA"` (for `--QA` flag),
 *                   `"calib"` (for `--calib` flag).
 * @return           The resolved path, ready to pass to a config reader.
 */
inline std::string conf_path(const std::string &basename, const std::string &subdir)
{
    if (!subdir.empty())
    {
        const std::string sub = "conf/" + subdir + "/" + basename;
        if (std::filesystem::exists(sub))
            return sub;
    }
    return "conf/" + basename;
}

/**
 * @brief Legacy bool overload — `qa_mode==true` ↔ `subdir=="QA"`.
 *
 * Marked `[[deprecated]]` 2026-05-28: new code should use the subdir
 * form with an explicit mode string (`"QA"`, `"calib"`, …) so the
 * call site documents which mode it routes to.  This overload is kept
 * only to avoid churning third-party callers that still pass a bool.
 */
[[deprecated("Use util::conf_path(basename, \"QA\") or the empty-string form.")]]
inline std::string conf_path(const std::string &basename, bool qa_mode)
{
    return conf_path(basename, qa_mode ? std::string{"QA"} : std::string{});
}

} // namespace util
