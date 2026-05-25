/**
 * @file analysis_results.cpp
 * @brief Implementation of AnalysisResults and associated query helpers.
 *
 * All ROOT I/O is confined to this translation unit.  The load → patch →
 * rewrite cycle used by update() is safe for the expected database size
 * (~hundreds of entries per beam-test campaign) and avoids the complexity
 * of in-place TTree row editing, which ROOT does not support natively.
 *
 * @author  Alix
 * @date    2025
 */

#include "analysis_results.h"
#include <mist/logger/logger.h>

#include <cstring>
#include <iostream>

#include "TFile.h"
#include "TTree.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Internal constants
// ─────────────────────────────────────────────────────────────────────────────

namespace
{

/// Maximum length of the `run` branch string (including null terminator).
constexpr std::size_t kRunBufLen = 64;

/// Maximum length of the `sensor` branch string (including null terminator).
constexpr std::size_t kSensorBufLen = 32;

/// Maximum length of the `quantity` branch string (including null terminator).
constexpr std::size_t kQtyBufLen = 128;

/// Name of the TTree inside the ROOT file.
constexpr const char *kTreeName = "results";

/// Title of the TTree (informational only).
constexpr const char *kTreeTitle = "Standard dRICH analysis results";

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  AnalysisResults — construction
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
//  AnalysisResults::load
// ─────────────────────────────────────────────────────────────────────────────

ResultMap AnalysisResults::load() const
{
    ResultMap out;

    TFile *f = TFile::Open(fPath.c_str(), "READ");
    if (!f || f->IsZombie())
    {
        // Silently return empty map — file may not exist yet on the first run.
        return out;
    }

    TTree *t = dynamic_cast<TTree *>(f->Get(kTreeName));
    if (!t)
    {
        mist::logger::error("[AnalysisResults] WARNING: no '" +
                            std::string(kTreeName) +
                            "' tree in " +
                            fPath);
        f->Close();
        return out;
    }

    // ── Branch wiring ────────────────────────────────────────────────────────
    char run_buf[kRunBufLen] = {};
    char sensor_buf[kSensorBufLen] = {};
    char qty_buf[kQtyBufLen] = {};
    double val = 0., err = 0.;

    t->SetBranchAddress("run", run_buf);
    t->SetBranchAddress("sensor", sensor_buf);
    t->SetBranchAddress("quantity", qty_buf);
    t->SetBranchAddress("value", &val);
    t->SetBranchAddress("error", &err);

    const Long64_t n_entries = t->GetEntries();
    out.clear();
    for (Long64_t i = 0; i < n_entries; ++i)
    {
        t->GetEntry(i);
        out[{std::string(run_buf),
             std::string(sensor_buf),
             std::string(qty_buf)}] = {val, err};
    }

    f->Close();
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnalysisResults::write  (private)
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @details
 * Opens (or creates) the file in RECREATE mode, which truncates any existing
 * content.  This is intentional: the caller (update()) has already merged the
 * old and new data into @p data before calling write().
 */
void AnalysisResults::write(const ResultMap &data) const
{
    TFile *f = new TFile(fPath.c_str(), "RECREATE");
    if (!f || f->IsZombie())
    {
        mist::logger::error("[AnalysisResults] ERROR: cannot open '" +
                            fPath +
                            "' for writing\n");
        return;
    }

    TTree *t = new TTree(kTreeName, kTreeTitle);

    // ── Branch declaration ───────────────────────────────────────────────────
    char run_buf[kRunBufLen] = {};
    char sensor_buf[kSensorBufLen] = {};
    char qty_buf[kQtyBufLen] = {};
    double val = 0., err = 0.;

    t->Branch("run", run_buf, "run/C");
    t->Branch("sensor", sensor_buf, "sensor/C");
    t->Branch("quantity", qty_buf, "quantity/C");
    t->Branch("value", &val, "value/D");
    t->Branch("error", &err, "error/D");

    // ── Fill ─────────────────────────────────────────────────────────────────
    for (const auto &kv : data)
    {
        // Use strncpy + explicit null-termination to avoid buffer overruns.
        std::strncpy(run_buf, kv.first.run.c_str(), kRunBufLen - 1);
        std::strncpy(sensor_buf, kv.first.sensor.c_str(), kSensorBufLen - 1);
        std::strncpy(qty_buf, kv.first.quantity.c_str(), kQtyBufLen - 1);
        run_buf[kRunBufLen - 1] = '\0';
        sensor_buf[kSensorBufLen - 1] = '\0';
        qty_buf[kQtyBufLen - 1] = '\0';

        val = kv.second.value;
        err = kv.second.error;
        t->Fill();
    }

    f->Write();
    f->Close();

    mist::logger::info("[AnalysisResults] Wrote " +
                       std::to_string(data.size()) +
                       " entries to " +
                       fPath);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AnalysisResults::update
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @details
 * The load → patch → rewrite cycle guarantees upsert semantics:
 *  1. Load current on-disk state into a local ResultMap.
 *  2. Overwrite or insert every entry from @p entries.
 *  3. Rewrite the entire file from the merged map.
 *
 * Entries present on disk but absent from @p entries are left untouched.
 */
void AnalysisResults::update(const ResultMap &entries) const
{
    ResultMap current = load();
    for (const auto &kv : entries)
        current[kv.first] = kv.second;
    write(current);
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