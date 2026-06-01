/**
 * @file analysis_results.h
 * @brief Persistent storage and query interface for dRICH beam-test analysis results.
 *
 * Provides a three-axis key schema (run, sensor, quantity) backed by a
 * hand-readable TOML file, with upsert semantics and helpers for building
 * TGraphErrors for plotting.  Migrated from a ROOT TTree backend
 * (see top-level DISCUSSION.md "AnalysisResults: TTree → TOML").
 *
 * ### File layout
 * A single TOML file (e.g. `<data_repository>/standard_results.toml`)
 * stores entries nested by (run, sensor, quantity):
 *
 *     [results."20251111-181940"."1350"]
 *     "ex_gap.n_gamma" = { value = 47.3, error = 0.6 }
 *     "ex_gap.sigma"   = { value = 1.42, error = 0.05 }
 *
 * `error` is omitted on disk for dimensionless diagnostics (e.g.
 * `chi2_ndf`, `gs_frac`) where an uncertainty isn't meaningful.
 *
 * ### Quantity naming convention
 * Quantities follow the pattern `<scope>.<name>`, where scope encodes the
 * histogram from which the value was derived:
 *  - `ex_gap`     — outside phi-gap region (main signal)
 *  - `in_gap`     — inside phi-gap region
 *  - `full`       — full phi acceptance
 *
 * Standard names: `n_gamma`, `mu`, `sigma`, `gs_frac`, `chi2_ndf`.
 * Error counterparts are stored as separate entries with the same key but
 * suffixed `_err` (e.g. `"ex_gap.n_gamma_err"`), or via the `error` branch
 * of the primary entry — both conventions are supported by the helpers.
 *
 * @author  Alix (ePIC dRICH beam-test)
 * @date    2025
 */

#pragma once

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "TGraphErrors.h" // for make_graph() return type

// ─────────────────────────────────────────────────────────────────────────────
//  ResultKey
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Three-component key identifying a single analysis result.
 *
 * The key space factorises naturally along three independent axes so that
 * plotting loops can slice on any one of them without string parsing:
 *
 *  - **run**      — data-taking timestamp (directory name under `extData/`)
 *  - **sensor**   — SiPM model (`"all"`, `"1350"`, `"1375"`)
 *  - **quantity** — dot-scoped quantity name (`"ex_gap.n_gamma"`, …)
 *
 * Strict weak ordering is provided so ResultKey can be used as a std::map key.
 */
struct ResultKey
{
    std::string run;      ///< Run identifier, e.g. @c "20251111-181940"
    std::string sensor;   ///< Sensor tag: @c "all" | @c "1350" | @c "1375"
    std::string quantity; ///< Dot-scoped quantity, e.g. @c "ex_gap.n_gamma"

    /**
     * @brief Lexicographic comparison enabling use as std::map key.
     * @param o  Other key to compare against.
     * @return   @c true if @c *this is lexicographically less than @p o.
     */
    bool operator<(const ResultKey &o) const
    {
        return std::tie(run, sensor, quantity) < std::tie(o.run, o.sensor, o.quantity);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  ResultEntry
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Value–error pair stored for each ResultKey.
 *
 * @note  Set @c error to 0 for dimensionless diagnostics (e.g. @c chi2_ndf,
 *        @c gs_frac) where an uncertainty is not meaningful.
 */
struct ResultEntry
{
    double value; ///< Central value of the quantity
    double error; ///< 1-sigma uncertainty; 0 if not applicable
};

// ─────────────────────────────────────────────────────────────────────────────
//  Type alias
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief In-memory representation of the full result database.
 *
 * Used both as the return type of AnalysisResults::load() and as the argument
 * to AnalysisResults::update().  Keys are sorted, so iteration is deterministic.
 */
using ResultMap = std::map<ResultKey, ResultEntry>;

// ─────────────────────────────────────────────────────────────────────────────
//  AnalysisResults
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Handle for a persistent TOML-backed analysis result store.
 *
 * Wraps a single hand-readable TOML file whose top-level table is
 * `results."<run>"."<sensor>"."<quantity>" = { value, error }`.  The
 * class provides upsert semantics through a load → patch → rewrite
 * cycle — cheap at the expected campaign size (~hundreds of entries),
 * keeps the file self-describing and git-mergeable, and matches the
 * dashboard reader (`qa_quicklook.rundb.results_load`) byte for byte.
 *
 * ### Typical workflow
 * @code
 * // After photon_number() fills fit_results …
 * AnalysisResults ar(data_repository + "/standard_results.toml");
 * ar.update({
 *     {{"20251111-181940", "all",  "ex_gap.n_gamma"}, {N,   N_err }},
 *     {{"20251111-181940", "all",  "ex_gap.sigma"},   {sig, sig_err}},
 *     {{"20251111-181940", "1350", "ex_gap.n_gamma"}, {N_1350, ...}},
 * }, "recodata");  // source tag → audit log
 *
 * // Later, in a plotting macro …
 * auto m  = ar.load();
 * auto *g = make_graph(m, RunList, vbias_vals, "1350", "ex_gap.n_gamma");
 * @endcode
 */
class AnalysisResults
{
public:
    // ── Construction ─────────────────────────────────────────────────────────

    /**
     * @brief Construct a handle pointing at @p path.
     *
     * The file does not need to exist yet; it will be created on the first
     * call to update().
     *
     * @param path  Filesystem path to the TOML result file.  The
     *              writers feed it as @c <data_repository>/standard_results.toml
     *              (typically @c Data/standard_results.toml), so the
     *              cross-run aggregate sits next to the per-run
     *              directories it summarises.  An audit log
     *              @c *.audit.toml is emitted alongside on every
     *              @c update() that carries a non-empty @c source tag.
     */
    explicit AnalysisResults(const std::string &path) : fPath(path) {}

    // ── I/O ──────────────────────────────────────────────────────────────────

    /**
     * @brief Load the full result database into memory.
     *
     * Parses the TOML file and walks the
     * `results."<run>"."<sensor>"."<quantity>"` table tree into a
     * ResultMap.  Returns an empty map if the file does not exist,
     * fails to parse, or carries no top-level `[results]` table.
     *
     * @return  ResultMap containing every stored (key, entry) pair.
     */
    ResultMap load() const;

    /**
     * @brief Upsert a batch of entries into the persistent store.
     *
     * Loads the current file contents, merges @p entries (overwriting any
     * existing entries with the same key), then rewrites the entire file.
     * Entries not present in @p entries are preserved unchanged.
     *
     * @param entries  Map of keys and values to insert or update.
     * @param source   Optional provenance tag.  When non-empty, one
     *                 ``[[entry]]`` block per key in @p entries is appended
     *                 to the sibling ``*.audit.toml`` file via
     *                 ``util::audit::log`` (see ``utility/audit.h``).
     *                 Pass the writer name — ``"lightdata"``,
     *                 ``"recodata"``, ``"recotrack"``, ``"calibration"`` —
     *                 or ``"dashboard"`` from Python edits.  Empty (the
     *                 default) preserves the pre-audit behaviour: no log.
     */
    void update(const ResultMap &entries,
                const std::string &source = "") const;

    /**
     * @brief Upsert a single entry into the persistent store.
     *
     * Convenience overload for updating one quantity at a time.
     *
     * @param key    Three-component key (run, sensor, quantity).
     * @param value  Central value to store.
     * @param error  Associated uncertainty (default 0).
     * @param source Optional provenance tag (see batch overload).
     */
    void update(const ResultKey &key, double value, double error = 0.,
                const std::string &source = "") const
    {
        update(ResultMap{{key, {value, error}}}, source);
    }

    /**
     * @brief Return the filesystem path this handle points to.
     * @return  Path string passed to the constructor.
     */
    const std::string &path() const { return fPath; }

private:
    std::string fPath; ///< Path to the backing TOML file

    /**
     * @brief Overwrite the TOML file at @p target_path with the contents
     *        of @p data.
     *
     * Truncates the file at @p target_path and writes a fresh `[results]`
     * table tree containing every entry from @p data.  Called internally
     * by update() after patching the in-memory map.
     *
     * For the atomic-update path (see @ref update), @p target_path is a
     * per-process ``"<fPath>.tmp.<pid>"`` staging path which @ref update
     * then renames over @ref fPath.  Direct callers that don't need
     * atomicity can pass @ref fPath.
     *
     * @param data         Complete result database to persist.
     * @param target_path  Destination file path (see above).
     */
    void write(const ResultMap &data, const std::string &target_path) const;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Query helpers (free functions)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Slice a ResultMap by run and sensor, returning a quantity → entry map.
 *
 * Useful when you need all quantities for a specific (run, sensor) combination,
 * e.g. to print a summary table or build a multi-quantity legend entry.
 *
 * @param m       Full result database (from AnalysisResults::load()).
 * @param run     Run identifier to filter on.
 * @param sensor  Sensor tag to filter on (@c "all", @c "1350", @c "1375").
 * @return        Map from quantity string to ResultEntry for matching keys.
 *                Empty if no entries match.
 */
std::map<std::string, ResultEntry>
query_run(const ResultMap &m,
          const std::string &run,
          const std::string &sensor);

/**
 * @brief Build a TGraphErrors from a ResultMap for a scan over multiple runs.
 *
 * Iterates over @p runs in order, looks up (@p sensor, @p quantity) for each,
 * and populates a TGraphErrors with the corresponding x / y / ey values.
 * Runs for which no entry exists in @p m are silently skipped, so the returned
 * graph may have fewer points than @p runs.
 *
 * The optional @p x_err_qty parameter specifies a quantity whose **value**
 * (not error) is used as the x-error bar — useful when the x-axis itself
 * carries an uncertainty (e.g. DCR spread from a Gaussian fit).
 *
 * @param m          Full result database.
 * @param runs       Ordered list of run identifiers (defines x ordering).
 * @param x_vals     Parallel vector of numeric x-axis values (same length as @p runs).
 * @param sensor     Sensor tag to select.
 * @param quantity   Quantity to plot on the y-axis.
 * @param x_err_qty  Optional quantity whose stored **value** becomes the x error bar.
 *                   Pass an empty string (default) to leave x errors at zero.
 * @return           Heap-allocated TGraphErrors (caller takes ownership).
 *                   Never null; may have zero points if nothing matched.
 */
TGraphErrors *
make_graph(const ResultMap &m,
           const std::vector<std::string> &runs,
           const std::vector<double> &x_vals,
           const std::string &sensor,
           const std::string &quantity,
           const std::string &x_err_qty = "");