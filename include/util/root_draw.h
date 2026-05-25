#pragma once

/**
 * @file util/root_draw.h
 * @brief ROOT canvas-drawing helpers (currently just @ref draw_circle).
 *
 * Thin wrappers around the ROOT graphics primitives so macros and the
 * analysis layer don't have to repeat the `new TEllipse(...) + style + DrawEllipse`
 * sequence.  Add new shape helpers here as they become reusable.
 */

#include <array>
#include <TAttLine.h>
#include <TEllipse.h>

/**
 * @brief Draw a circle on the current ROOT canvas pad.
 * @param parameters    {x0, y0, R} [mm].
 * @param line_colour   ROOT line colour (default: @c kBlack).
 * @param line_style    ROOT line style (default: @c kSolid).
 * @param line_width    ROOT line width in pixels (default: 1).
 */
inline void draw_circle(std::array<float, 3> parameters, int line_colour = kBlack, int line_style = kSolid, int line_width = 1)
{
    // Previous version `new TEllipse(...); result->DrawEllipse(...)` was
    // doubly wrong (CODE_REVIEW §5.6):
    //   - `TEllipse::DrawEllipse` constructs a SECOND TEllipse internally
    //     and draws it; the named `result` was never drawn and never freed.
    //   - The heap-allocated `result` leaked on every call.
    // DrawClone makes a copy of the local stack TEllipse and registers it
    // with the current pad (which owns the clone) — no leak, no double-draw.
    TEllipse ellipse(parameters[0], parameters[1], parameters[2]);
    ellipse.SetFillStyle(0);
    ellipse.SetLineColor(line_colour);
    ellipse.SetLineStyle(line_style);
    ellipse.SetLineWidth(line_width);
    ellipse.DrawClone("same");
}
