/**
 * @file include/utility/qa_publish.h
 * @brief Tiny helper: every writer drops curated PDFs into a
 *        per-run, per-step directory the QA dashboard auto-picks up.
 *
 * Convention (also in qa_quicklook/DISCUSSION.md):
 *
 *   Data/<run-id>/qa/<step>/NN_<name>.pdf
 *
 * where ``step`` is one of ``"lightdata"`` / ``"recodata"`` /
 * ``"recotrack"`` / ``"calibration"`` (or anything else; the
 * dashboard's per-step QA sub-tabs render whatever's there).
 * ``NN`` is a 2-digit prefix that governs display order — sort by
 * filename and you get the writer's intended order.
 *
 * Why a helper instead of every writer rolling its own path?
 * The QA dashboard hard-codes the ``qa/<step>/`` convention; if a
 * writer drifts (puts files under ``QA/`` or in the run root), the
 * dashboard silently misses them.  Funnel through ``pdf_path()`` so
 * "rename a step" / "tighten the convention" stays a one-place edit.
 *
 * Usage::
 *
 *     TCanvas c("c_trigger_qa", "", 1200, 800);
 *     // … draw …
 *     c.SaveAs(util::qa::pdf_path(run_dir, "lightdata", 1, "trigger_qa")
 *              .c_str());
 *
 * The parent directory is created on-demand.  The helper is
 * header-only — no source unit needed.
 */

#ifndef BTANA_UTILITY_QA_PUBLISH_H
#define BTANA_UTILITY_QA_PUBLISH_H

#include <filesystem>
#include <sstream>
#include <string>

namespace util::qa
{

/**
 * @brief Build the canonical PDF path for a writer-emitted QA plot.
 *
 * Side effect: creates ``<run_dir>/qa/<step>/`` if it doesn't exist
 * (``std::filesystem::create_directories`` — idempotent and quiet
 * about already-existing parents).
 *
 * @param run_dir  Absolute or relative path to the run directory
 *                 (typically ``<data_repository>/<run_name>``).
 * @param step     Pipeline step tag — should match what the QA tab
 *                 expects: ``"lightdata"`` / ``"recodata"`` /
 *                 ``"recotrack"`` / ``"calibration"``.
 * @param order    2-digit display-order prefix.  Sort-by-name
 *                 governs rendering order in the dashboard; pick
 *                 1, 2, 3 … per file in publication order.
 * @param name     Short identifier for the plot (no spaces / no
 *                 extension — the helper appends ``.pdf``).
 *
 * @return ``std::filesystem::path`` ready to pass to
 *         ``TCanvas::SaveAs(p.string().c_str())``.
 */
inline std::filesystem::path pdf_path(const std::string &run_dir,
                                      const std::string &step,
                                      int order,
                                      const std::string &name)
{
    std::filesystem::path dir = std::filesystem::path(run_dir) / "qa" / step;
    // Idempotent — no-op if the dir already exists; throws on
    // permission errors which is the right failure mode (writer
    // can't possibly recover from "QA dir not writable").
    std::filesystem::create_directories(dir);

    // 2-digit zero-padded prefix so filename-sort = publication
    // order across 1..99.  Anything past 99 still sorts correctly
    // because the underscore between order and name keeps fields
    // separated.
    std::ostringstream basename;
    if (order < 10 && order >= 0)
        basename << "0" << order;
    else
        basename << order;
    basename << "_" << name << ".pdf";

    return dir / basename.str();
}

} // namespace util::qa

#endif // BTANA_UTILITY_QA_PUBLISH_H
