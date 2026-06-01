/**
 * @file analysis_results.cxx
 * @brief Implementation of AnalysisResults and associated query helpers.
 *
 * Primary store is a hand-readable TOML file.  Schema:
 *
 *     # standard_results.toml
 *     [results."<run>"."<sensor>"]
 *     "<quantity>" = { value = X, error = Y }   # error optional
 *
 * Same load → patch → rewrite cycle as the previous ROOT-backed
 * implementation; toml++ handles the serialisation both ways.
 *
 * The TTree backend was retired (see DISCUSSION.md
 * "AnalysisResults: TTree → TOML").  Any leftover .root files from
 * the dual-backend phase can be ignored — they're stale on first
 * writer invocation against the new path convention.
 *
 * @author  Alix
 * @date    2025
 */

#include "analysis_results.h"
#include <mist/logger/logger.h>

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/file.h> // flock(2) — POSIX advisory file lock
#include <system_error>
#include <unistd.h> // close(2), getpid(2)

#include "utility/audit.h"      // per-write provenance log
#include "utility/toml_utils.h" // toml++

// ─────────────────────────────────────────────────────────────────────────────
//  AnalysisResults::load
// ─────────────────────────────────────────────────────────────────────────────

ResultMap AnalysisResults::load() const
{
    ResultMap out;

    // Missing file → silently return empty map (first invocation case).
    // toml++ throws on missing file; check explicitly so we can short-
    // circuit without printing an error.
    if (!std::filesystem::exists(fPath))
        return out;

    toml::table doc;
    try
    {
        doc = toml::parse_file(fPath);
    }
    catch (const toml::parse_error &e)
    {
        mist::logger::error("[AnalysisResults] TOML parse error in " + fPath +
                            ": " + std::string(e.what()));
        return out;
    }

    auto *results = doc["results"].as_table();
    if (!results)
        return out;

    //  Walk results."<run>"."<sensor>"."<quantity>" = { value, error }.
    //  Each level guards on the expected node type; malformed entries
    //  are skipped silently rather than aborting the whole load — the
    //  symmetric guard pattern is mirrored on the dashboard side
    //  (qa_quicklook.rundb.results_load).
    for (const auto &[run_key, run_node] : *results)
    {
        const auto *run_tbl = run_node.as_table();
        if (!run_tbl)
            continue;
        for (const auto &[sensor_key, sensor_node] : *run_tbl)
        {
            const auto *sensor_tbl = sensor_node.as_table();
            if (!sensor_tbl)
                continue;
            for (const auto &[qty_key, leaf_node] : *sensor_tbl)
            {
                const auto *leaf = leaf_node.as_table();
                if (!leaf)
                    continue;
                auto value_opt = (*leaf)["value"].value<double>();
                if (!value_opt)
                    continue;
                const double error = (*leaf)["error"].value_or(0.0);
                out[{std::string(run_key.str()),
                     std::string(sensor_key.str()),
                     std::string(qty_key.str())}] = {*value_opt, error};
            }
        }
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnalysisResults::write  (private)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @details
 * Opens (or creates) the file at @p target_path fresh on every call; the
 * caller (update()) has already merged the old and new data into @p data,
 * so a full rewrite is the correct semantic.  toml++ produces a stable
 * string ordering (insertion order, preserved across our std::map → table
 * copy), so the file is git-friendly: writing the same data twice yields
 * byte-identical output.
 *
 * Sweep audit: write() accepts the target path explicitly
 * so that update() can stage the write at a per-process tmp path and
 * atomically rename it over @ref fPath.  Direct callers that don't need
 * atomicity can pass @ref fPath.  The C2.1 atomicity concept that was
 * superseded by the TTree → TOML migration is now re-applied on the
 * TOML backend.
 */
void AnalysisResults::write(const ResultMap &data,
                            const std::string &target_path) const
{
    toml::table doc;
    auto results = toml::table{};

    // Group entries by (run, sensor) so we emit one nested table per
    // (run, sensor) pair — matches the schema and keeps the file compact.
    // Iteration over ``data`` is already in (run, sensor, quantity) order
    // because std::map sorts on the composite key.
    std::string cur_run, cur_sensor;
    toml::table *run_tbl = nullptr;
    toml::table *sensor_tbl = nullptr;
    for (const auto &[key, entry] : data)
    {
        if (key.run != cur_run)
        {
            results.insert(key.run, toml::table{});
            run_tbl = results[key.run].as_table();
            cur_run = key.run;
            cur_sensor.clear();
        }
        if (key.sensor != cur_sensor)
        {
            run_tbl->insert(key.sensor, toml::table{});
            sensor_tbl = (*run_tbl)[key.sensor].as_table();
            cur_sensor = key.sensor;
        }
        // Inline table per leaf: ``{ value = X, error = Y }``.  Omit
        // ``error`` when 0 to keep the file tidy for dimensionless
        // diagnostics that don't carry a meaningful uncertainty.
        toml::table leaf;
        leaf.insert("value", entry.value);
        if (entry.error != 0.0)
            leaf.insert("error", entry.error);
        sensor_tbl->insert(key.quantity, std::move(leaf));
    }
    doc.insert("results", std::move(results));

    std::ofstream ofs(target_path);
    if (!ofs)
    {
        mist::logger::error("[AnalysisResults] ERROR: cannot open '" +
                            target_path + "' for writing");
        return;
    }
    //  Banner comment names the FINAL path (fPath) not the tmp staging
    //  path — readers should still see a self-describing file after the
    //  rename completes.
    ofs << "# " << fPath << "\n"
        << "# Machine-generated by AnalysisResults — safe to regenerate.\n"
        << "# Dashboard consumer: qa_quicklook.rundb.results_load()\n"
        << "# Schema: [results.\"<run>\".\"<sensor>\"]"
           " \"<quantity>\" = { value = X, error = Y }\n\n";
    ofs << doc;

    mist::logger::info("[AnalysisResults] Wrote " +
                       std::to_string(data.size()) +
                       " entries to " + target_path);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnalysisResults::update
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @details
 * The load → patch → rewrite cycle guarantees upsert semantics:
 *  1. Acquire an exclusive ``flock(2)`` on a sidecar ``<fPath>.lock``
 *     file so concurrent writers from different processes (e.g.
 *     ``lightdata_writer`` and ``recodata_writer`` finishing at the
 *     same wall-clock tick) serialise here instead of racing through
 *     load → merge → write and clobbering each other.
 *  2. Load current on-disk state into a local ResultMap.
 *  3. Overwrite or insert every entry from @p entries.
 *  4. Write the merged map to ``<fPath>.tmp.<pid>``.
 *  5. ``std::filesystem::rename`` tmp → final.  POSIX rename is atomic
 *     on the same filesystem (tmp + final are siblings, so this holds):
 *     readers either see the old file or the new file, never a half-
 *     written TOML.
 *  6. Release the flock.
 *
 * Sweep audit: re-applies the CLEAN_OFF C2.1 concept on
 * the post-migration TOML backend.  The race window is identical to
 * the previous ROOT-backed implementation; only the file format
 * changed.
 *
 * Entries present on disk but absent from @p entries are left untouched.
 *
 * Failure modes:
 *  - open / flock on the lock file fails → log error, proceed
 *    WITHOUT the lock (concurrent writes may interleave; single-writer
 *    case still atomic individually via step 5).
 *  - write to tmp fails → ofstream errors are logged inside write();
 *    the subsequent rename guard handles the missing-tmp case.
 *  - rename fails → tmp is best-effort removed; fPath unchanged.
 *
 * Stale tmp files (process killed between write() and rename()) are
 * tolerated rather than swept on every call — they're small and
 * harmless.
 */
void AnalysisResults::update(const ResultMap &entries,
                             const std::string &source) const
{
    namespace fs = std::filesystem;

    // ── Cross-process advisory lock on a sidecar pidfile ─────────────────────
    //  Lifetime tied to this function via a local guard struct.  We
    //  intentionally leave the lock file on disk after release —
    //  creating / unlinking races between processes are a known pitfall,
    //  and an empty lock file is harmless.
    const std::string lock_path = fPath + ".lock";
    int lock_fd = ::open(lock_path.c_str(), O_WRONLY | O_CREAT, 0644);
    if (lock_fd >= 0)
    {
        if (::flock(lock_fd, LOCK_EX) < 0)
        {
            mist::logger::error("[AnalysisResults] flock(LOCK_EX) failed on " +
                                lock_path + " — proceeding without lock "
                                            "(concurrent writes may interleave)");
            ::close(lock_fd);
            lock_fd = -1;
        }
    }
    else
    {
        mist::logger::error("[AnalysisResults] cannot open lock file " +
                            lock_path + " — proceeding without lock");
    }
    struct LockGuard
    {
        int fd;
        ~LockGuard()
        {
            if (fd >= 0)
            {
                ::flock(fd, LOCK_UN);
                ::close(fd);
            }
        }
    } guard{lock_fd};

    // ── Load + merge ─────────────────────────────────────────────────────────
    ResultMap current = load();
    for (const auto &kv : entries)
        current[kv.first] = kv.second;

    // ── Write to tmp + atomic rename ─────────────────────────────────────────
    const std::string tmp_path = fPath + ".tmp." + std::to_string(::getpid());
    write(current, tmp_path);

    std::error_code ec;
    fs::rename(tmp_path, fPath, ec);
    if (ec)
    {
        mist::logger::error("[AnalysisResults] rename('" + tmp_path +
                            "' → '" + fPath + "') failed: " +
                            ec.message() + " — fPath unchanged");
        std::error_code ec_rm;
        fs::remove(tmp_path, ec_rm); // best-effort cleanup
        return;
    }

    //  Provenance log — only when the caller named a source.  Empty
    //  source preserves the pre-audit no-op behaviour, so legacy
    //  call-sites that haven't been ported don't start writing
    //  partial-provenance entries (which would be misleading).
    //  Logged AFTER the rename so the log reflects successfully-
    //  committed updates only.
    if (!source.empty())
    {
        const auto audit_path = util::audit::sibling_audit_path(fPath);
        for (const auto &kv : entries)
        {
            util::audit::log(audit_path.string(),
                             source,
                             kv.first.run,
                             kv.first.sensor,
                             kv.first.quantity,
                             kv.second.value,
                             kv.second.error);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  query_run
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @details
 * Iterates the full ResultMap once; complexity is O(N) in the total number of
 * stored entries.  For the expected database size this is negligible.
 */
std::map<std::string, ResultEntry>
query_run(const ResultMap &m,
          const std::string &run,
          const std::string &sensor)
{
    std::map<std::string, ResultEntry> out;
    for (const auto &kv : m)
        if (kv.first.run == run && kv.first.sensor == sensor)
            out[kv.first.quantity] = kv.second;
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  make_graph
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @details
 * Points are added in the order of @p runs, so the caller controls the x-axis
 * ordering.  Runs absent from @p m produce no point in the graph; the point
 * index therefore matches the index in @p runs only if every run is present.
 *
 * When @p x_err_qty is non-empty the function performs a second map lookup per
 * run and uses the **value** (not the error field) of that entry as the x error
 * bar.  This is the intended pattern for using, e.g., the sigma of a DCR
 * Gaussian fit as the x uncertainty on a N_gamma vs DCR plot.
 */
TGraphErrors *
make_graph(const ResultMap &m,
           const std::vector<std::string> &runs,
           const std::vector<double> &x_vals,
           const std::string &sensor,
           const std::string &quantity,
           const std::string &x_err_qty)
{
    auto *g = new TGraphErrors();

    const bool has_x_err = !x_err_qty.empty();

    for (std::size_t i = 0; i < runs.size(); ++i)
    {
        const std::string &run = runs[i];

        // ── Primary lookup ───────────────────────────────────────────────────
        auto it = m.find({run, sensor, quantity});
        if (it == m.end())
        {
            mist::logger::error("[make_graph] WARNING: no entry for (" +
                                run + ", " +
                                sensor + ", " +
                                quantity + ") — skipped\n");
            continue;
        }

        // ── Optional x-error lookup ──────────────────────────────────────────
        double x_err = 0.;
        if (has_x_err)
        {
            auto it_xe = m.find({run, sensor, x_err_qty});
            if (it_xe != m.end())
                x_err = it_xe->second.value;
            else
                mist::logger::error("[make_graph] WARNING: x_err_qty '" +
                                    x_err_qty + "' not found for run " +
                                    run);
        }

        const int n = g->GetN();
        g->SetPoint(n, x_vals[i], it->second.value);
        g->SetPointError(n, x_err, it->second.error);
    }

    return g;
}
