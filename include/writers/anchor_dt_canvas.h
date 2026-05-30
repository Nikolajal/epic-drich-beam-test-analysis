#pragma once

/**
 * @file writers/anchor_dt_canvas.h
 * @brief Shared 3-pad "Δt vs spill" diagnostic canvas rendering.
 *
 * Builds the canvas seen on the QA Calibration sub-tab (one per run
 * from ``pulser_calib_writer``) and the QA Lightdata sub-tab (one
 * per ``TriggerNumber`` that fired, from ``lightdata_writer``):
 *   - Banner pad with title + total-entries + off-canvas count
 *   - Top zoom pad covering +rollover ± 50 cc
 *   - Main pad covering ±250 cc (the in-spec band)
 *   - Bottom zoom pad covering −rollover ± 50 cc
 *
 * Equal pixels-per-cc across the three plot pads so a 5-px vertical
 * span means the same Δt everywhere; shared z-range across pads;
 * colz palette (log-z by default) only on the main pad — the slim
 * zooms reuse the same scale.
 *
 * Originally lived inside ``pulser_calib_writer.cxx``; extracted as
 * part of backlog row P 1.61 ("anchor-Δt vs spill PDF per trigger",
 * since closed) so both writers render the same canvas without
 * copy-paste drift.
 *
 * The caller fills the ``TH2F`` (X = spill index, Y = c_channel −
 * c_reference in cc — where the reference is the pulser anchor for
 * ``pulser_calib_writer`` and the firing trigger's coarse counter
 * for ``lightdata_writer``) and supplies its own title + PDF path +
 * log prefix.  Everything else (region thresholds, layout, font
 * sizes, log-z, palette geometry) is internal to this file so
 * changes happen in one place.
 */

#include <string>
#include <vector>
#include <TH2.h>

namespace util::qa
{
    /// Build the variable-bin Y-edge array for the anchor-Δt histogram.
    ///
    /// Layout (705 in-range bins, vs 65 737 with uniform 1-cc binning —
    /// a 93× memory cut, plus the under/overflow + two gap bins make
    /// the "off-canvas" count trivially readable):
    ///
    ///   - bins 1 ↔ 101   : −rollover zoom, 101 bins of 1 cc
    ///                      (centers at −rollover−50 … −rollover+50)
    ///   - bin  102       : −gap (single huge bin spanning
    ///                      −rollover+50.5 ↔ −250.5)
    ///   - bins 103 ↔ 603 : main pad, 501 bins of 1 cc
    ///                      (centers at −250 … +250)
    ///   - bin  604       : +gap (single huge bin spanning
    ///                      +250.5 ↔ +rollover−50.5)
    ///   - bins 605 ↔ 705 : +rollover zoom, 101 bins of 1 cc
    ///   - bin  0         : underflow (Δ < −rollover−50)
    ///   - bin  706       : overflow  (Δ > +rollover+50)
    ///
    /// Used by both ``pulser_calib_writer`` and ``lightdata_writer`` so
    /// the canvas helper assumes a fixed bin layout — no per-writer
    /// special-casing in ``render_anchor_dt_canvas``.
    std::vector<double> make_anchor_dt_y_edges(int rollover_cc);

    /// Tunables for ``render_anchor_dt_canvas``.  Kept as a struct so
    /// future additions (log-z toggle, alternate layouts, etc.) don't
    /// change the call signature.
    struct AnchorDtCanvasOpts
    {
        /// Coarse-counter rollover boundary in cc.  Typically
        /// ``BTANA_ALCOR_ROLLOVER_TO_CC = 32768``.
        int rollover_cc = 32768;

        /// Full canvas title — TLatex codes (``#Delta`` etc.) allowed.
        /// E.g. ``"#Deltat vs spill   (channel #minus anchor: device 200, chip 0, channel 0)"``
        /// for the pulser writer; ``"#Deltat vs spill   (channel #minus trigger #00)"``
        /// for a lightdata per-trigger plot.
        std::string title;

        /// Where to save the rendered canvas as PDF.  Directory must exist.
        std::string pdf_path;

        /// Prefix for ``mist::logger`` lines emitted from inside this
        /// renderer, so the calling writer's tag shows up in the run
        /// log (e.g. ``"(pulser_calib_writer)"``).
        std::string logger_prefix;
    };

    /// Render the 3-pad anchor-Δ vs spill diagnostic canvas and save
    /// as PDF at ``opts.pdf_path``.  The histogram is consumed
    /// non-destructively (clones are used inside).
    ///
    /// Returns the total number of entries in the histogram across
    /// all 7 named regions (= the visible canvas's banner "Total").
    long long render_anchor_dt_canvas(TH2F &h,
                                      const AnchorDtCanvasOpts &opts);
} // namespace util::qa
