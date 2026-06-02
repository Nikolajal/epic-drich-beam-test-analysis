/**
 * @file src/writers/anchor_dt_canvas.cxx
 * @brief Implementation of the 3-pad anchor-Δ vs spill canvas.
 *
 * Extracted from ``pulser_calib_writer.cxx`` as part of backlog row
 * P 1.61 so the ``lightdata_writer`` can render the same canvas
 * per trigger without copy-paste drift.
 *
 * Phase: square canvas + consistent Y-axis tick density across pads.
 *
 * Layout math (canvas-height NDC, square canvas 1000 × 1000):
 *   banner :  y = [0.90, 1.00]   10 %   title + total + off-canvas
 *   top    :  y = [0.79, 0.90]   11 %   +rollover ± 50
 *   main   :  y = [0.25, 0.79]   54 %   |Δ| ≤ 250
 *   bot    :  y = [0.00, 0.25]   25 %   −rollover ± 50 + x-axis area
 *
 *   Y data rendered:  100 + 500 + 100 = 700 cc.
 *   Plot-area fraction: ≈ 76 % of canvas height
 *   → 1 cc ≈ 0.00109 of canvas height (uniform across the three
 *     plot pads — equal-px/cc invariant preserved).
 *
 * Y-axis tick spacing: 50 cc / major tick across ALL three pads.
 *   main (500 cc range)   → 10 majors, ~54 px / tick at 1000 px tall
 *   top  (100 cc range)   →  2 majors, ~55 px / tick
 *   bot  (100 cc range)   →  2 majors, ~55 px / tick
 * Operator reading the rollover zoom and the main pad sees tick
 * spacing at the same pixel density on the page.
 *
 * Region split (7 named bands the integral table uses):
 *   Δ > +rollover+50                    (off-canvas, above top zoom)
 *   |Δ − rollover| ≤ 50                 (top zoom in-window)
 *   +250 < Δ < +rollover−50             (off-canvas, between main + top)
 *   |Δ| ≤ 250                           (main pad in-window)
 *   −rollover+50 < Δ < −250             (off-canvas, between bot + main)
 *   |Δ + rollover| ≤ 50                 (bot zoom in-window)
 *   Δ < −rollover−50                    (off-canvas, below bot zoom)
 *
 * The banner's "Off-canvas" count sums the four off-canvas regions
 * so the "did you miss anything?" question has a single-number
 * answer at the top of the page.
 */

#include "writers/anchor_dt_canvas.h"
#include "utility/qa_publish.h"

#include <algorithm>
#include <filesystem>
#include <numeric>

#include <TCanvas.h>
#include <TPad.h>
#include <TLatex.h>
#include <TStyle.h>
#include <TString.h>
#include <TColor.h>
#include <TPaletteAxis.h>
#include <TList.h>

#include <mist/logger/logger.h>

namespace util::qa
{
std::vector<double> make_anchor_dt_y_edges(int rollover_cc)
{
    //  Edges chosen so bin centres land on integer cc values:
    //  the bin centred at cc = k has edges [k − 0.5, k + 0.5).
    //  Two gap bins capture everything between the main pad and
    //  each rollover zoom in a SINGLE wide bin — collapses
    //  ~32 467 useless 1-cc bins per side into one summable
    //  counter, which is exactly the "off-canvas (in no pad)"
    //  number the banner wants to surface.
    std::vector<double> e;
    e.reserve(706);
    // −rollover zoom — 101 1-cc bins
    for (int k = -rollover_cc - 50; k <= -rollover_cc + 51; ++k)
        e.push_back(static_cast<double>(k) - 0.5);
    // −gap (single bin) — its lower edge IS the last edge above
    // (so we just push the upper edge here): −250.5
    e.push_back(-250.5);
    // main pad — 501 1-cc bins, edges −250.5 → +250.5.
    // first edge (−250.5) is already pushed above as the upper
    // edge of the −gap; continue from −249.5.
    for (int k = -249; k <= 251; ++k)
        e.push_back(static_cast<double>(k) - 0.5);
    // +gap (single bin) — its upper edge is +rollover − 50.5
    e.push_back(+rollover_cc - 50.5);
    // +rollover zoom — 101 1-cc bins, continue from +rollover−49.5
    for (int k = +rollover_cc - 49; k <= +rollover_cc + 51; ++k)
        e.push_back(static_cast<double>(k) - 0.5);
    return e;
}

long long render_anchor_dt_canvas(TH2F &h_anchor_dt_vs_spill,
                                  const AnchorDtCanvasOpts &opts)
{
    const int kRollover = opts.rollover_cc;

    //  Per-pad under/overflow counts — *paramount* for reading
    //  this plot, since the rollover-zone populations are the
    //  whole point.  We split the Y axis into 7 named regions
    //  and integrate each against the source histogram.  Each
    //  pad's overlay then reports its in-window count + the
    //  total of everything above and everything below its
    //  window so leakage is impossible to miss.
    struct Region
    {
        const char *latex;
        const char *plain;
        double y_lo;
        double y_hi;
        long long count;
    };
    const double E_top_hi = kRollover + 50.5;
    const double E_top_lo = kRollover - 50.5;
    const double E_main_hi = 250.5;
    const double E_main_lo = -250.5;
    const double E_bot_hi = -kRollover + 50.5;
    const double E_bot_lo = -kRollover - 50.5;
    Region regions[] = {
        {"#Delta > +rollover+50", "Delta > +rollover+50",
         E_top_hi, 1e9, 0},
        {"|#Delta #minus rollover| #leq 50", "|Delta - rollover| <= 50",
         E_top_lo, E_top_hi, 0},
        {"+250 < #Delta < +rollover#minus50", "+250 < Delta < +rollover-50",
         E_main_hi, E_top_lo, 0},
        {"|#Delta| #leq 250 (main)", "|Delta| <= 250 (main)",
         E_main_lo, E_main_hi, 0},
        {"#minusrollover+50 < #Delta < #minus250", "-rollover+50 < Delta < -250",
         E_bot_hi, E_main_lo, 0},
        {"|#Delta + rollover| #leq 50", "|Delta + rollover| <= 50",
         E_bot_lo, E_bot_hi, 0},
        {"#Delta < #minusrollover#minus50", "Delta < -rollover-50",
         -1e9, E_bot_lo, 0},
    };
    const int n_x = h_anchor_dt_vs_spill.GetNbinsX();
    const int n_y = h_anchor_dt_vs_spill.GetNbinsY();
    const auto *yax = h_anchor_dt_vs_spill.GetYaxis();
    for (auto &r : regions)
    {
        //  Map the region's Y bounds to bin indices, including
        //  underflow (bin 0) and overflow (bin n_y+1).  Variable
        //  bins for the anchor-Δt axis make this honest: the two
        //  "gap" regions and the under/over-flows are each
        //  exactly ONE bin, so reading the off-canvas count is a
        //  single GetBinContent on each.  The previous
        //  ``FindFixBin(r.y_hi) - 1`` convention silently lost
        //  the overflow bin because ``FindFixBin`` returned the
        //  same overflow index for both region bounds, leaving
        //  an empty range.
        int b_lo = (r.y_lo <= yax->GetXmin())
                       ? 0
                       : yax->FindFixBin(r.y_lo);
        int b_hi = (r.y_hi >= yax->GetXmax())
                       ? n_y + 1
                       : yax->FindFixBin(r.y_hi) - 1;
        r.count = static_cast<long long>(
            h_anchor_dt_vs_spill.Integral(1, n_x, b_lo, b_hi));
    }
    const long long n_total = std::accumulate(
        std::begin(regions), std::end(regions), 0LL,
        [](long long a, const Region &r)
        { return a + r.count; });

    //  Log the breakdown — shows up in the writer's stdout so
    //  operators running on the CLI see it too.
    mist::logger::info(TString::Format(
                           "%s anchor-Δ region split (total %lld):",
                           opts.logger_prefix.c_str(), n_total)
                           .Data());
    for (const auto &r : regions)
    {
        const double pct = n_total > 0
                               ? 100.0 * static_cast<double>(r.count) / static_cast<double>(n_total)
                               : 0.0;
        mist::logger::info(TString::Format(
                               "%s   %-30s : %lld (%.3f%%)",
                               opts.logger_prefix.c_str(), r.plain, r.count, pct)
                               .Data());
    }

    //  Stats box off — we have our own custom overlay.  Saved
    //  + restored so we don't poison other writers that might
    //  run later in the same process.
    const int prev_optstat = gStyle->GetOptStat();
    gStyle->SetOptStat(0);

    //  Canvas 1000 × 1000 (square) — the dashboard's PDF preview
    //  tile assumes a square aspect for the 1/2/3/4-per-row
    //  reflow grid; a portrait canvas made the tile box itself
    //  portrait, breaking the layout.  The Z-palette readability
    //  concern from earlier is handled by the trimmed palette
    //  (NDC y 0.15→0.85, see ``with_palette`` block below) and
    //  the widened right margin (0.13) — both keep the palette
    //  legible at this aspect.
    TCanvas c_anchor("c_anchor_dt_vs_spill", "", 1000, 1000);
    //  Plot frame shifted slightly left — left margin tightened
    //  (Y labels still fit at rollover even at 0.11) so the data
    //  display + colz palette get a bit more room on the right.
    constexpr double kLeftMargin = 0.11;  // 5-digit Y labels at rollover
    constexpr double kRightMargin = 0.13; // colz palette + spacing

    TPad p_banner("p_banner", "", 0.0, 0.90, 1.0, 1.00);
    p_banner.SetTopMargin(0.0);
    p_banner.SetBottomMargin(0.0);
    p_banner.SetLeftMargin(0.0);
    p_banner.SetRightMargin(0.0);
    p_banner.SetBorderMode(0);
    p_banner.Draw();

    auto configure_pad = [&](TPad &p, double top, double bot)
    {
        p.SetTopMargin(top);
        p.SetBottomMargin(bot);
        p.SetLeftMargin(kLeftMargin);
        p.SetRightMargin(kRightMargin);
        p.SetBorderMode(0);
        p.SetBorderSize(0);
        p.SetFrameBorderMode(0);
        p.SetFrameBorderSize(0);
        //  Log-z by default — populations of "clean" Δt land in
        //  the ±1 cc band and dominate by 3–4 orders of magnitude
        //  over rollover tails, so linear-z hides the tails
        //  entirely.  Per-pad property in ROOT (not pad-style),
        //  hence set here in the shared configurator.
        p.SetLogz(1);
        p.Draw();
    };
    //  Pad NDC heights set so the PLOT AREAS (the data display
    //  inside the frame) come out in the exact 1:5:1 ratio that
    //  matches the 50 cc : 250 cc : 50 cc half-ranges of the data
    //  — equal px/cc across all three pads.  The bot pad bundles
    //  a dedicated x-axis area (``kXAxisH``) on top of its plot
    //  unit so labels + title clear the canvas edge.  Net pad-
    //  outline ratio is 1 : 5 : 1 + kXAxisH-fraction (≈ 1:5:1.47
    //  with the current numbers) — close to 1:5:1 with bot a
    //  touch taller because it carries the axis.
    //
    //  Constants here so a tweak to ``kBannerH`` / ``kXAxisH`` /
    //  ``kTopMargin`` flows through the layout automatically.
    constexpr double kBannerH = 0.10;
    constexpr double kTopMargin = 0.02; // breathing room from banner
    constexpr double kXAxisH = 0.06;    // x-axis labels + title area (canvas NDC)
    //  plot_unit solves: banner + top_pad + main_pad + bot_pad = 1
    //  with top_pad = plot/(1−tm), main_pad = 5·plot, bot_pad = plot + xa
    //  ⇒ plot · (1/(1−tm) + 6) = 1 − banner − xa
    constexpr double kPlotUnitH =
        (1.0 - kBannerH - kXAxisH) / (1.0 / (1.0 - kTopMargin) + 6.0);
    constexpr double kTopPadH = kPlotUnitH / (1.0 - kTopMargin);
    constexpr double kMainPadH = 5.0 * kPlotUnitH;
    constexpr double kBotPadH = kPlotUnitH + kXAxisH;
    constexpr double kBotMargin = kXAxisH / kBotPadH;
    constexpr double kBannerY0 = 1.0 - kBannerH; // 0.90
    constexpr double kTopY0 = kBannerY0 - kTopPadH;
    constexpr double kMainY0 = kTopY0 - kMainPadH;

    TPad p_top("p_top", "+rollover zoom",
               0.0, kTopY0, 1.0, kBannerY0);
    configure_pad(p_top, kTopMargin, 0.0);
    TPad p_main("p_main", "main",
                0.0, kMainY0, 1.0, kTopY0);
    configure_pad(p_main, 0.0, 0.0);
    TPad p_bot("p_bot", "-rollover zoom",
               0.0, 0.0, 1.0, kMainY0);
    configure_pad(p_bot, 0.0, kBotMargin);

    const double z_min = 1.0;
    const double z_max = std::max(
        1.0, static_cast<double>(h_anchor_dt_vs_spill.GetMaximum()));

    //  Font sizes — bumped for legibility, especially on the
    //  in-pad count overlays + the axis titles + the banner.
    //  Each font is in absolute pixels (kFont 43) so the value
    //  is the rendered text height regardless of which pad it
    //  lands in.
    constexpr int kFont = 43;
    constexpr int kLabelPx = 20;     // tick labels
    constexpr int kTitlePx = 26;     // axis titles
    constexpr int kOverlayPx = 22;   // in-pad count overlay
    constexpr int kBannerPx = 30;    // banner title (centered)
    constexpr int kBannerSubPx = 20; // banner subtitle (total + off-canvas)

    //  Tick spacing — uniform 50 cc / major tick everywhere so the
    //  rollover zoom pads + the main pad show the same px/tick
    //  density to the eye.  See the module-level comment for the
    //  arithmetic.
    constexpr int kTickIntervalCc = 50;

    auto draw_in_pad = [&](TPad &pad, double y_lo_r, double y_hi_r,
                           bool show_xaxis, bool with_palette,
                           bool show_ytitle,
                           const char *in_label)
    {
        pad.cd();
        auto *h = static_cast<TH2F *>(
            h_anchor_dt_vs_spill.Clone(
                TString::Format("h_anchor_dt_pad_%s", pad.GetName())));
        h->SetDirectory(nullptr);
        h->SetStats(0);
        h->SetTitle("");
        h->GetYaxis()->SetRangeUser(y_lo_r, y_hi_r);
        h->SetMinimum(z_min);
        h->SetMaximum(z_max);
        h->SetLineWidth(1);

        auto *yax_pad = h->GetYaxis();
        yax_pad->SetLabelFont(kFont);
        yax_pad->SetLabelSize(kLabelPx);
        yax_pad->SetTitleFont(kFont);
        yax_pad->SetTitleSize(kTitlePx);
        yax_pad->SetTitleOffset(1.5);
        if (!show_ytitle)
            yax_pad->SetTitle("");
        //  Equal-density ticks: ``kTickIntervalCc`` cc per major
        //  tick everywhere.  The ratio range/interval gives the
        //  primary-division count; 5 secondary subdivisions per
        //  major to mark 10-cc fines without crowding.
        const double pad_range = y_hi_r - y_lo_r;
        const int n_primary = std::max(
            1, static_cast<int>(std::lround(pad_range / kTickIntervalCc)));
        yax_pad->SetNdivisions(n_primary, 5, 0);
        if (&pad != &p_main)
        {
            //  Slim-pad boundary labels fall on bin-edge .5-offset
            //  positions (the histogram's bins are centered on
            //  integer cc; edges sit at half-integers).  Reading
            //  ``32818.5`` / ``32717.5`` at the pad limits is
            //  noisy; hide them and let the central tick (the
            //  rollover value itself, a clean integer like
            //  ``32768``) carry the only label.  The pad's frame
            //  visually communicates the ±50 cc band.
            yax_pad->ChangeLabel(1, -1, -1, -1, -1, -1, " ");
            yax_pad->ChangeLabel(-1, -1, -1, -1, -1, -1, " ");
        }

        auto *xax = h->GetXaxis();
        if (show_xaxis)
        {
            xax->SetLabelFont(kFont);
            xax->SetLabelSize(kLabelPx);
            xax->SetTitleFont(kFont);
            xax->SetTitleSize(kTitlePx);
            //  Title offset 0.9 — keeps ``spill`` close to the
            //  tick labels (was 1.2; originally 2.4).  At
            //  kTitlePx = 26, 0.9× ≈ 23 px below the labels,
            //  comfortably inside the bot pad's x-axis area.
            xax->SetTitleOffset(0.9);
        }
        else
        {
            xax->SetLabelSize(0);
            xax->SetTitle("");
            xax->SetTickLength(0.0);
        }

        h->GetZaxis()->SetMaxDigits(7);
        h->Draw(with_palette ? "colz" : "col");

        //  Shrink the colz palette vertically while keeping it
        //  centered on the main pad's frame.  Default ROOT spans
        //  the full frame height [0, 1] in pad NDC (since top +
        //  bot margins are 0 on the main pad); 0.15 → 0.85
        //  trims 30 % off the height with equal 15 % padding
        //  top and bottom.  Need to force a pad update first so
        //  the TPaletteAxis object exists.
        if (with_palette)
        {
            pad.Modified();
            pad.Update();
            //  ``static_cast`` not ``dynamic_cast`` — ROOT's
            //  HistPainter library doesn't export the
            //  TPaletteAxis typeinfo we'd need for an RTTI cast.
            //  The "palette" name lookup is the documented way
            //  to fetch it, so the static cast is safe.
            auto *palette = static_cast<TPaletteAxis *>(
                h->GetListOfFunctions()->FindObject("palette"));
            if (palette)
            {
                palette->SetY1NDC(0.15);
                palette->SetY2NDC(0.85);
            }
        }

        //  In-pad count overlay — JUST the in-window count.
        //  Closed interval [y_lo_r, y_hi_r]: FindFixBin returns
        //  the bin CONTAINING the value, and we want both ends
        //  included (the visual "|Δ| ≤ 250" means inclusive),
        //  so no ``- 1`` on b_hi.  The previous convention
        //  silently dropped the upper boundary bin.
        const int b_lo = yax_pad->FindFixBin(y_lo_r);
        const int b_hi = yax_pad->FindFixBin(y_hi_r);
        const long long in_n = static_cast<long long>(
            h_anchor_dt_vs_spill.Integral(1, n_x, b_lo, b_hi));
        TLatex tx;
        tx.SetNDC();
        tx.SetTextFont(43);
        tx.SetTextSize(&pad == &p_main ? kOverlayPx
                                       : std::max(14, kOverlayPx - 4));
        tx.SetTextAlign(13);
        //  Shifted right a touch so the overlay clears the Y-axis
        //  labels even on the rollover pads (5-digit labels are
        //  wider than the main pad's ±200).  Y nudged up so the
        //  text sits well clear of the data band on each pad.
        tx.DrawLatex(0.18,
                     &pad == &p_main ? 0.96 : 0.92,
                     TString::Format("%s: %lld", in_label, in_n));
    };
    draw_in_pad(p_top, +kRollover - 50, +kRollover + 50,
                false, false, false, "+rollover #pm 50");
    draw_in_pad(p_main, -250.0, +250.0,
                false, true, true, "|#Delta| #leq 250");
    draw_in_pad(p_bot, -kRollover - 50, -kRollover + 50,
                true, false, false, "#minusrollover #pm 50");

    //  Banner — title + total + off-canvas count, both centered.
    p_banner.cd();
    TLatex bn;
    bn.SetNDC();
    bn.SetTextFont(kFont);
    bn.SetTextSize(kBannerPx);
    bn.SetTextAlign(23); // top, horizontal center
    bn.SetTextColor(kBlack);
    //  Y dropped from 0.95 → 0.85 so the title sits a touch
    //  below the top canvas edge — visually centred within the
    //  banner band rather than hugging its top.
    bn.DrawLatex(0.50, 0.85, opts.title.c_str());

    const long long n_off_canvas =
        regions[0].count + regions[2].count + regions[4].count + regions[6].count;
    const double off_pct = n_total > 0
                               ? 100.0 * static_cast<double>(n_off_canvas) / static_cast<double>(n_total)
                               : 0.0;
    bn.SetTextSize(kBannerSubPx);
    bn.SetTextColor(n_off_canvas > 0 ? kRed + 1 : kGray + 3);
    bn.DrawLatex(0.50, 0.30,
                 TString::Format(
                     "Total: %lld   |   Off-canvas (in no pad): %lld (%.3f%%)",
                     n_total, n_off_canvas, off_pct));

    c_anchor.SaveAs(opts.pdf_path.c_str());
    //  ROOT TPDF wraps the canvas in A4-portrait regardless of
    //  TCanvas size or gStyle->SetPaperSize (verified 6.40/macOS).
    //  Crop the MediaBox back to the content bounding box so the
    //  dashboard's QA gallery tiles uniformly.  No-op if pdfcrop
    //  isn't installed.
    util::qa::crop_pdf_inplace(std::filesystem::path(opts.pdf_path));

    gStyle->SetOptStat(prev_optstat);
    return n_total;
}
} // namespace util::qa
