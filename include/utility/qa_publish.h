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

#include <cstdlib>
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

/**
 * @brief Crop a ROOT-emitted PDF in-place to its content bounding box.
 *
 * ROOT's TPDF backend wraps every canvas in an A4-portrait page
 * (MediaBox 595×842 pt, regardless of TCanvas size or
 * @c gStyle->SetPaperSize() — verified on ROOT 6.40.00 / macOS).  The
 * plot itself is drawn at canvas-pixel aspect ratio in the top of the
 * page; the bottom is just whitespace.  Operators view the PDFs in
 * the QA dashboard's tile grid where the surrounding whitespace makes
 * every plot look portrait, defeating the equal-aspect canvas design.
 *
 * Post-process every emitted PDF through @c pdfcrop (from MacTeX /
 * TeXLive) which rewrites the MediaBox to the actual content
 * bounding box plus a small uniform margin.  The dashboard gallery
 * then tiles uniformly because every PDF's MediaBox matches its
 * content aspect.
 *
 * The crop is best-effort:
 *  - If @c pdfcrop is not on PATH, the call is a silent no-op (the
 *    PDF stays portrait-A4 but is still valid).  Returns @c false in
 *    that case so callers can log if they want.
 *  - If the crop fails for any reason (corrupt input, disk full,
 *    permissions), the original PDF is left untouched.  Returns
 *    @c false.
 *
 * @param pdf_path  Absolute path to the PDF to crop in place.  Must
 *                  already exist on disk.
 * @param margin_pt Uniform white-space margin around the content
 *                  bounding box, in PDF points.  Default 5 pt keeps
 *                  axis labels from touching the page edge in the
 *                  dashboard's gallery view.
 * @return @c true if the crop ran and overwrote the file successfully;
 *         @c false if pdfcrop is missing, failed, or the file is
 *         absent.  Never throws.
 */
inline bool crop_pdf_inplace(const std::filesystem::path &pdf_path,
                             int margin_pt = 5)
{
    if (!std::filesystem::exists(pdf_path))
        return false;

    //  pdfcrop overwrites in-place when you pass the same path as
    //  both input and output.  --margins clamps to a uniform
    //  whitespace ring so labels don't touch the edge.  Quiet the
    //  output so the writer log isn't peppered with pdfcrop's
    //  per-file banner.
    //
    //  PATH injection — pdfcrop lives at /Library/TeX/texbin/pdfcrop
    //  on macOS / MacTeX which is NOT in a forked C++ binary's
    //  runtime PATH (only the interactive shell adds it).  Prepend
    //  the common TeX locations so the call resolves even when the
    //  writer is launched from cron / a desktop launcher.
    //  pdfcrop has no --quiet flag (verified on MacTeX 2024 / pdfcrop
    //  1.40); it's already non-verbose by default.  Redirect stdout +
    //  stderr to /dev/null instead.
    //  ``--hires`` uses Ghostscript's high-resolution %%HiResBoundingBox
    //  rather than the coarse integer bbox.  The integer box can round
    //  INWARD and shave a row/column off the plot frame or an edge label
    //  (the "the plot itself got cropped" failure); the hi-res box hugs
    //  the true content, and the uniform ``--margins`` ring then leaves a
    //  few points of clearance on every side so nothing is clipped.
    std::ostringstream cmd;
    cmd << "PATH=\"/Library/TeX/texbin:/usr/local/texlive/2024/bin/universal-darwin:"
        << "/usr/local/bin:/opt/homebrew/bin:${PATH:-}\" "
        << "pdfcrop --hires --margins " << margin_pt << ' '
        << "'" << pdf_path.string() << "' "
        << "'" << pdf_path.string() << "' "
        << "> /dev/null 2>&1";
    const int rc = std::system(cmd.str().c_str());
    return rc == 0;
}

} // namespace util::qa

#endif // BTANA_UTILITY_QA_PUBLISH_H
