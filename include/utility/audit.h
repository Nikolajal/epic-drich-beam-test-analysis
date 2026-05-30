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
 *      ``standard_results.toml``) stays scannable — one ``[run.sensor]``
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
 *  ``standard_results.toml`` → ``standard_results.audit.toml``).  Each
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
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

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
/// ``extData/standard_results.toml`` → ``extData/standard_results.audit.toml``.
inline std::filesystem::path sibling_audit_path(const std::string &primary_path)
{
    namespace fs = std::filesystem;
    fs::path p(primary_path);
    p.replace_extension();              // drop ``.root`` / ``.toml``
    p += ".audit.toml";
    return p;
}

/// Escape a string for TOML basic-string syntax (the ``"..."`` form).
/// We escape only the minimum set required for TOML correctness:
/// backslash, double-quote, and the standard whitespace controls.
/// Writer names / run IDs / quantity names ARE controlled — but
/// ``source = "dashboard"`` edits can carry arbitrary text from the
/// operator (free-form notes), and the cost of always escaping is one
/// allocation per call.
inline std::string toml_basic_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:   out += c;
        }
    }
    return out;
}

/// Append one ``[[entry]]`` block to the audit file at @p audit_path.
///
/// ``value`` and ``error`` are rendered with 6 significant digits to
/// match ``analysis_results.cxx``'s primary-file precision; pass
/// ``error == 0`` for dimensionless quantities (it'll still be
/// emitted, since absence has its own meaning in a log).
///
/// ## Atomicity (C2.2)
///
/// The entry is built into a single ``std::string`` then flushed with
/// one ``::write()`` call on an ``O_APPEND``-opened fd.  POSIX
/// guarantees writes of fewer than ``PIPE_BUF`` bytes (4096 on
/// macOS/Linux) to an ``O_APPEND`` file are atomic across processes,
/// so two writers (e.g. ``lightdata`` and ``recodata``) running
/// concurrently never tear a ``[[entry]]`` block — even without
/// holding the in-process mutex.  The mutex still serialises same-
/// process threads (cheaper than two unrelated entries racing through
/// the fd-open + write window).
///
/// A typical entry is ~180 bytes, comfortably below ``PIPE_BUF``;
/// even with pathological 1 KiB string fields we stay under the
/// guarantee.  If a future caller starts logging multi-KB blobs, the
/// ``PIPE_BUF`` invariant must be re-checked.
inline void log(const std::string &audit_path,
                const std::string &source,
                const std::string &run,
                const std::string &sensor,
                const std::string &quantity,
                double             value,
                double             error = 0.0)
{
    //  In-process mutex — serialises threads of the same process so
    //  two threads don't both call ``::write`` simultaneously (which
    //  the kernel would still atomicise, but the mutex keeps the
    //  fd-open + write window single-threaded which is simpler to
    //  reason about).  Cross-process atomicity comes from the
    //  ``O_APPEND`` + sub-``PIPE_BUF`` invariant documented above.
    static std::mutex s_mu;
    std::lock_guard<std::mutex> lock(s_mu);

    //  Build the entry into a single buffer.  ``std::ostringstream``
    //  is convenient for the numeric formatting (``std::setprecision``)
    //  and the final ``str()`` produces the payload we hand to write(2).
    std::ostringstream os;
    os << std::setprecision(6);
    os << "[[entry]]\n"
       << "at       = \"" << toml_basic_escape(iso8601_now())  << "\"\n"
       << "source   = \"" << toml_basic_escape(source)         << "\"\n"
       << "run      = \"" << toml_basic_escape(run)            << "\"\n"
       << "sensor   = \"" << toml_basic_escape(sensor)         << "\"\n"
       << "quantity = \"" << toml_basic_escape(quantity)       << "\"\n"
       << "value    = " << value << "\n"
       << "error    = " << error << "\n\n";
    const std::string payload = os.str();

    //  Sweep audit (2026-05-30): enforce the sub-PIPE_BUF invariant
    //  documented above.  POSIX guarantees atomic O_APPEND writes only
    //  for payloads < PIPE_BUF (4096 on macOS/Linux).  A typical entry
    //  is ~180 B; under pathological per-field input + TOML escape
    //  expansion (each control char → 2 chars) we could in principle
    //  exceed 4096.  When that happens we'd rather drop the entry
    //  loudly than emit a torn block that breaks toml::parse_file at
    //  the next dashboard load.  3.5 KiB cap leaves headroom against
    //  the kernel boundary.
    constexpr std::size_t kAtomicCap = 3584; // 3.5 KiB < PIPE_BUF (4096)
    if (payload.size() >= kAtomicCap)
    {
        mist::logger::error("[util::audit] entry size " +
                            std::to_string(payload.size()) +
                            " B exceeds atomic-write cap (" +
                            std::to_string(kAtomicCap) +
                            " B) — entry dropped to preserve "
                            "cross-process O_APPEND atomicity in " +
                            audit_path);
        return;
    }

    //  Open with O_APPEND so the kernel atomically positions writes at
    //  end-of-file (multi-process safe); O_CREAT with 0644 so the file
    //  is created on first call.  Close immediately after the single
    //  write — opening per call is cheap, and a long-lived cached fd
    //  would complicate fork() / file-rotation semantics.
    const int fd = ::open(audit_path.c_str(),
                          O_WRONLY | O_APPEND | O_CREAT,
                          0644);
    if (fd < 0)
    {
        mist::logger::error("[util::audit] cannot open " + audit_path +
                            " for append — entry dropped");
        return;
    }

    const ssize_t want = static_cast<ssize_t>(payload.size());
    const ssize_t got  = ::write(fd, payload.data(), payload.size());
    //  Sweep audit (2026-05-30): guard the close.  close(-1) is a no-op
    //  on macOS/Linux but clobbers errno on some BSDs; the open-failure
    //  branch already returns above so fd >= 0 here, but keep the
    //  guard for future-refactor safety.
    if (fd >= 0)
        ::close(fd);

    if (got != want)
    {
        mist::logger::error("[util::audit] short write to " + audit_path +
                            " (" + std::to_string(got) + " / " +
                            std::to_string(want) + " bytes) — entry "
                            "may be truncated");
    }
}

} // namespace util::audit
