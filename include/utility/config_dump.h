/**
 * @file include/utility/config_dump.h
 * @brief Shared helper: every writer drops a self-describing
 *        ``Config/`` ``TDirectory`` into its output ROOT file.
 *
 * The QA dashboard's ``DataInspectPane`` reads this directory back
 * (via uproot) to show the operator exactly which knobs produced a
 * given output.  Before this helper landed, only ``lightdata_writer``
 * had a complete dump; the other writers had partial or no
 * ``Config/`` at all, breaking the "Show params" button on
 * ``recodata.root`` / ``recotrackdata.root``.
 *
 * Conventions the consumer (``qa_quicklook/datainspect.py``) relies on:
 *
 *   - **Numeric values** are stored as ``TParameter<T>`` so uproot
 *     can read ``.member("fVal")`` directly.  Integer-like values
 *     prefer ``TParameter<int>`` / ``TParameter<long>``;
 *     floating-point prefer ``TParameter<double>``.
 *   - **String values** (file paths, mode tags, booleans rendered as
 *     ``"true"`` / ``"false"``) are stored as ``TNamed`` so uproot
 *     can read ``.member("fTitle")``.
 *   - **TOML snapshots** are ``TNamed`` whose key ends in ``_toml``;
 *     the file's verbatim content sits in ``fTitle``.  The dashboard
 *     groups these into a separate "toml payloads" section.
 *
 * Usage::
 *
 *     util::ConfigDump cfg(output_file.get());
 *     cfg.add("max_spill", max_spill)
 *        .add("force_rebuild", force_rebuild)
 *        .add_path("framer_conf_file", framer_conf_file)
 *        .add_toml_snapshot("framer_conf", framer_conf_file);
 *
 * The ``ConfigDump`` ctor creates the ``Config`` subdirectory and
 * cd's into it via an RAII ``TDirectory::TContext``; the dtor
 * restores ``gDirectory``.  Every ``add*`` writes immediately, so
 * subsequent writers' ``Write()`` calls won't accidentally land in
 * ``Config/`` if the helper is used before they cd into their own
 * sub-folders.
 */

#ifndef BTANA_UTILITY_CONFIG_DUMP_H
#define BTANA_UTILITY_CONFIG_DUMP_H

#include <fstream>
#include <sstream>
#include <string>

#include "TDirectory.h"
#include "TFile.h"
#include "TNamed.h"
#include "TParameter.h"

namespace util
{

/**
 * @brief RAII writer for a self-describing ``Config/`` ``TDirectory``.
 *
 * One instance per output file.  Methods return ``*this`` for chaining.
 * Move-only — copying would clone the directory pointer with no clear
 * ownership story.
 */
class ConfigDump
{
public:
    /**
     * @brief Create the ``Config`` subdirectory under @p parent and
     *        make it the current ``gDirectory`` for the lifetime of
     *        ``*this``.
     *
     * Passing ``nullptr`` is a no-op (the helper becomes inert and
     * every ``add*`` call silently returns) — lets callers funnel
     * through this helper without null-checking the parent themselves
     * (e.g. an early-exit path where the output file failed to open).
     */
    explicit ConfigDump(TDirectory *parent)
        : cfg_dir_(parent ? parent->mkdir("Config") : nullptr),
          ctx_(cfg_dir_ ? cfg_dir_ : nullptr)
    {
        if (cfg_dir_)
            cfg_dir_->cd();
    }

    ConfigDump(const ConfigDump &) = delete;
    ConfigDump &operator=(const ConfigDump &) = delete;
    ConfigDump(ConfigDump &&) = delete;
    ConfigDump &operator=(ConfigDump &&) = delete;

    /** Direct pointer to the ``Config/`` directory (null when inert). */
    TDirectory *dir() const { return cfg_dir_; }

    // ---- Numeric scalars → TParameter<T> ----------------------------

    ConfigDump &add(const std::string &name, int value)         { return write_param(name, value); }
    ConfigDump &add(const std::string &name, long value)        { return write_param(name, value); }
    ConfigDump &add(const std::string &name, long long value)   { return write_param(name, value); }
    ConfigDump &add(const std::string &name, unsigned value)    { return write_param(name, value); }
    ConfigDump &add(const std::string &name, float value)       { return write_param(name, static_cast<double>(value)); }
    ConfigDump &add(const std::string &name, double value)      { return write_param(name, value); }

    // ---- Booleans → TNamed("true"/"false") --------------------------
    //
    // Stored as text rather than TParameter<bool> so the QA-side
    // dashboard renders them readably in the "scalar" section
    // without special-casing ROOT's bool serialisation quirks.

    ConfigDump &add(const std::string &name, bool value)
    {
        return add_text(name, value ? "true" : "false");
    }

    // ---- Strings → TNamed -------------------------------------------

    ConfigDump &add(const std::string &name, const char *value)
    {
        return add_text(name, value ? std::string{value} : std::string{});
    }

    ConfigDump &add(const std::string &name, const std::string &value)
    {
        return add_text(name, value);
    }

    /**
     * @brief Alias for ``add(name, value)`` for string values, used
     *        at call sites where the semantic meaning is "this is a
     *        path on disk".  Pure documentation — the on-disk
     *        encoding is identical to ``add(string)``.
     */
    ConfigDump &add_path(const std::string &name, const std::string &path)
    {
        return add_text(name, path);
    }

    /**
     * @brief Snapshot a TOML file under ``<key>_toml`` as a ``TNamed``
     *        whose ``fTitle`` is the file's verbatim content.
     *
     * The trailing ``_toml`` is the contract with
     * ``qa_quicklook/datainspect.py`` — anything matching that suffix
     * is surfaced under "[toml payloads]" rather than the regular
     * scalar grid.  Silently skips when @p file_path can't be opened
     * (an unset or missing conf is a legitimate state on some code
     * paths, e.g. the lightdata writer's optional streaming conf).
     */
    ConfigDump &add_toml_snapshot(const std::string &key,
                                  const std::string &file_path)
    {
        if (!cfg_dir_ || file_path.empty())
            return *this;
        std::ifstream f(file_path);
        if (!f.good())
            return *this;
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string snapshot_key = key + "_toml";
        TNamed named(snapshot_key.c_str(), ss.str().c_str());
        named.Write();
        return *this;
    }

    /**
     * @brief Convenience: dump both the path and the TOML body in one call.
     *
     * Writes ``<key>_file`` (the path) and ``<key>_toml`` (the
     * verbatim content).  Matches the lightdata_writer pattern of
     * "the path tells you what we used, the snapshot tells you what
     * was in it at the time".
     */
    ConfigDump &add_conf_file(const std::string &key,
                              const std::string &file_path)
    {
        add_path(key + "_file", file_path);
        add_toml_snapshot(key, file_path);
        return *this;
    }

private:
    template <typename T>
    ConfigDump &write_param(const std::string &name, T value)
    {
        if (!cfg_dir_)
            return *this;
        TParameter<T> p(name.c_str(), value);
        p.Write();
        return *this;
    }

    ConfigDump &add_text(const std::string &name, const std::string &value)
    {
        if (!cfg_dir_)
            return *this;
        TNamed n(name.c_str(), value.c_str());
        n.Write();
        return *this;
    }

    TDirectory *cfg_dir_;
    TDirectory::TContext ctx_;
};

} // namespace util

#endif // BTANA_UTILITY_CONFIG_DUMP_H
