#pragma once

/**
 * @file conf_path.h
 * @brief Path-resolution helper for writer mode-flags (`--QA`, `--calib`)
 *        and per-campaign config sets.
 *
 * Each writer's CLI exposes mode flags that route config-file lookup
 * through a `conf/<mode>/` sub-folder first (overrides for that mode),
 * falling back to `conf/<basename>` if no override exists.  Live modes:
 *
 *   --QA      → `conf/QA/`     fast-feedback operator tuning
 *   --calib   → `conf/calib/`  pulser-driven fine-time calibration
 *
 * On top of that, detector configuration that DRIFTS between test-beam
 * campaigns (e.g. which device/FIFO carries the hardware trigger, which
 * chips form the timing coincidence) lives in the existing per-campaign
 * bundle directory `conf/sets/<campaign>/` (the same bundles the dashboard's
 * manual `switch_set()` repoints to — see qa_quicklook/conf_layout.py), here
 * selected AUTOMATICALLY from the run-id year.  Resolution order, most
 * specific first:
 *
 *   1. conf/sets/<campaign>/<mode>/<basename>
 *   2. conf/sets/<campaign>/<basename>
 *   3. conf/<mode>/<basename>
 *   4. conf/<basename>                         (current-campaign default)
 *
 * Only the files that actually differ per campaign need a copy under
 * `conf/sets/<campaign>/`; everything else falls through to the shared
 * `conf/` default (which tracks the current campaign), so the bundles stay
 * minimal.  A run with no `conf/sets/<year>/` bundle (e.g. the current
 * campaign) uses the default `conf/` — no per-run switching needed.
 *
 * Modes are mutually exclusive at the CLI level (only one mode flag
 * may be set per invocation); the writer mains enforce that.
 *
 * Usage (CLI main, after `CLI11_PARSE`):
 *
 * ```cpp
 * const std::string mode     = qa_mode ? "QA" : (calib_mode ? "calib" : "");
 * const std::string campaign = util::campaign_of(run_name);   // e.g. "2025"
 * if (p_streaming->count() == 0)
 *     streaming_conf = util::conf_path("streaming.toml", mode, campaign);
 * ```
 *
 * Files that exist only under `conf/<mode>/` will load even when the
 * mode flag is false (if a user explicitly passes the path on the
 * CLI), because this helper only acts when the CLI option was NOT set.
 */

#include <cctype>
#include <filesystem>
#include <string>

namespace util
{

/**
 * @brief Campaign tag (4-digit year) of a `YYYYMMDD-HHMMSS` run id.
 *
 * The campaign directory `conf/<campaign>/` is keyed by this tag — the
 * same first-4-chars-are-the-year convention the run-list / database
 * campaign-file picker uses on the Python side.  Returns an empty string
 * (→ no campaign override, shared `conf/` only) when @p run_id does not
 * begin with four digits.
 */
inline std::string campaign_of(const std::string &run_id)
{
    if (run_id.size() < 4)
        return {};
    for (int i = 0; i < 4; ++i)
        if (!std::isdigit(static_cast<unsigned char>(run_id[i])))
            return {};
    return run_id.substr(0, 4);
}

/**
 * @brief Resolve a config-file basename to its actual path.
 *
 * Checks, in order of decreasing specificity, `conf/<campaign>/<subdir>/`,
 * `conf/<campaign>/`, `conf/<subdir>/`, and finally the always-present
 * `conf/<basename>` default.  Empty @p subdir / @p campaign skip their
 * respective levels.
 *
 * @param basename   Filename inside `conf/` (e.g. `"streaming.toml"`).
 *                   Should NOT carry a directory prefix.
 * @param subdir     Mode sub-folder (`"QA"`, `"calib"`, or empty).
 * @param campaign   Campaign tag (e.g. `"2025"`, from @ref campaign_of),
 *                   or empty to use only the shared `conf/`.
 * @return           The resolved path, ready to pass to a config reader.
 */
inline std::string conf_path(const std::string &basename,
                             const std::string &subdir,
                             const std::string &campaign)
{
    const auto try_path = [](const std::string &p) -> bool
    { return std::filesystem::exists(p); };

    if (!campaign.empty())
    {
        const std::string set = "conf/sets/" + campaign + "/";
        if (!subdir.empty())
        {
            const std::string cm = set + subdir + "/" + basename;
            if (try_path(cm))
                return cm;
        }
        const std::string c = set + basename;
        if (try_path(c))
            return c;
    }
    if (!subdir.empty())
    {
        const std::string sub = "conf/" + subdir + "/" + basename;
        if (try_path(sub))
            return sub;
    }
    return "conf/" + basename;
}

/**
 * @brief Two-argument form — no per-campaign override (shared `conf/`).
 */
inline std::string conf_path(const std::string &basename, const std::string &subdir)
{
    return conf_path(basename, subdir, std::string{});
}

/**
 * @brief Legacy bool overload — `qa_mode==true` ↔ `subdir=="QA"`.
 *
 * Marked `[[deprecated]]`: new code should use the subdir
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
