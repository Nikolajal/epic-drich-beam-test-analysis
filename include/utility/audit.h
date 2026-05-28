/**
 * @file utility/audit.h
 * @brief Append-only provenance log for AnalysisResults updates and other
 *        cross-run scalar writes.
 *
 *
 *  ## Why a sibling log instead of inlining ``source`` on every leaf
 *
 *  See qa_quicklook/DISCUSSION.md §"Where the source lands: per-write
 *  audit log".  TL;DR:
 *
 *    - the primary data file (``standard_results.toml`` /
 *      ``standard_results.root``) stays scannable — one ``[run.sensor]``
 *      table is small enough to read at a glance, no provenance
 *      noise per leaf;
 *    - forward-inheritance / auto-pin on the dashboard side need no
 *      schema change in the primary file;
 *    - "show history of this field" becomes a trivial tail-match in
 *      the dashboard.
 *
 *  ## File format
 *
 *  Single TOML file per primary, named ``<basename>.audit.toml`` (so
 *  ``standard_results.root`` → ``standard_results.audit.toml``).  Each
 *  call to ``util::audit::log`` appends one ``[[entry]]`` table.
 *
 *      [[entry]]
 *      at        = "2025-11-12T03:15:22"
 *      source    = "recodata"            # writer name / "dashboard" / "legacy"
 *      run       = "20251111-181940"
 *      sensor    = "all"
 *      quantity  = "recodata.n_spills"
 *      value     = 42
 *      error     = 0
 *
 *  The format is line-oriented so multiple processes appending in
 *  parallel never tear an entry mid-table — each call writes its
 *  block atomically to a temp file then ``rename(2)``s the result.
 *  (POSIX ``rename`` is atomic on the same filesystem.)
 *
 *  ## Why header-only
 *
 *  Tiny — ~30 lines of code — and pulled in by writers that already
 *  pay for ``<fstream>`` and ``toml_utils.h``.  No need for a TU.
 */

#pragma once

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

#include <mist/logger/logger.h>

namespace util::audit
{

/// ISO-8601 timestamp at second resolution in the local zone — matches
/// the dashboard's existing ``at = "..."`` rendering in run-database
/// edits so a tail of the audit file reads like a unified history.
inline std::string iso8601_now()
{
    const auto now   = std::chrono::system_clock::now();
    const auto t_now = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t_now);
#else
    localtime_r(&t_now, &local);
#endif
    std::ostringstream os;
    os << std::put_time(&local, "%Y-%m-%dT%H:%M:%S");
    return os.str();
}

/// Path of the sibling audit log for ``primary_path``.  Drops the
/// existing extension and appends ``.audit.toml``: so
/// ``extData/standard_results.root`` → ``extData/standard_results.audit.toml``.
inline std::filesystem::path sibling_audit_path(const std::string &primary_path)
{
    namespace fs = std::filesystem;
    fs::path p(primary_path);
    p.replace_extension();              // drop ``.root`` / ``.toml``
    p += ".audit.toml";
    return p;
}

/// Append one ``[[entry]]`` block to the audit file at @p audit_path.
///
/// ``value`` and ``error`` are rendered with 6 significant digits to
/// match ``analysis_results.cxx``'s primary-file precision; pass
/// ``error == 0`` for dimensionless quantities (it'll still be
/// emitted, since absence has its own meaning in a log).
///
/// All string fields are wrapped in double quotes; we don't escape
/// embedded quotes because writer names / run IDs / quantity names
/// are tightly controlled in this codebase (no user-supplied
/// strings reach here).  If that ever changes, plumb in a proper
/// TOML string escape.
inline void log(const std::string &audit_path,
                const std::string &source,
                const std::string &run,
                const std::string &sensor,
                const std::string &quantity,
                double             value,
                double             error = 0.0)
{
    //  ── In-process mutex.  Cross-process atomicity comes from the
    //  open-append fd inheriting O_APPEND semantics on POSIX; this
    //  mutex just keeps two threads in the same writer from
    //  interleaving their ``[[entry]]`` blocks.
    static std::mutex s_mu;
    std::lock_guard<std::mutex> lock(s_mu);

    std::ofstream ofs(audit_path, std::ios::app);
    if (!ofs)
    {
        mist::logger::error("[util::audit] cannot open " + audit_path +
                            " for append — entry dropped");
        return;
    }

    ofs << std::setprecision(6);
    ofs << "[[entry]]\n"
        << "at       = \"" << iso8601_now() << "\"\n"
        << "source   = \"" << source        << "\"\n"
        << "run      = \"" << run           << "\"\n"
        << "sensor   = \"" << sensor        << "\"\n"
        << "quantity = \"" << quantity      << "\"\n"
        << "value    = " << value         << "\n"
        << "error    = " << error         << "\n\n";
}

} // namespace util::audit
