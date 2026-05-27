#pragma once

/**
 * @file conf_path.h
 * @brief Path-resolution helper for the `--QA` fast-feedback mode.
 *
 * Each writer's CLI exposes a `--QA` boolean flag.  When set, config
 * files are looked up under `conf/QA/<basename>` first (a fast / biased
 * tuning intended for live operator feedback), falling back to
 * `conf/<basename>` if the override doesn't exist.
 *
 * Usage (CLI main, after `CLI11_PARSE`):
 *
 * ```cpp
 * std::string streaming_conf;                            // unset sentinel
 * auto *p_streaming = app.add_option("--streaming-conf", streaming_conf);
 * // ... after parse ...
 * if (p_streaming->count() == 0)                         // user didn't override
 *     streaming_conf = util::conf_path("streaming.toml", qa_mode);
 * ```
 *
 * Files that exist only under `conf/QA/` will load even when `qa_mode`
 * is false (if a user explicitly passes `--streaming-conf conf/QA/foo`),
 * because this helper only acts when the CLI option was NOT set.
 */

#include <filesystem>
#include <string>

namespace util
{

/**
 * @brief Resolve a config-file basename to its actual path.
 *
 * @param basename   Filename inside `conf/` (e.g. `"streaming.toml"`).
 *                   Should NOT carry a directory prefix.
 * @param qa_mode    If true and `conf/QA/<basename>` exists, returns
 *                   that path.  Else returns `conf/<basename>`.
 * @return           The resolved path, ready to pass to a config reader.
 */
inline std::string conf_path(const std::string &basename, bool qa_mode)
{
    if (qa_mode)
    {
        const std::string qa = "conf/QA/" + basename;
        if (std::filesystem::exists(qa))
            return qa;
    }
    return "conf/" + basename;
}

} // namespace util
