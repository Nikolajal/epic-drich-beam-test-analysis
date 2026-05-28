"""Histogram discovery + thumbnail rendering for the QA tab.

Splits the work in two:

  - ``enumerate_histograms(root_path)`` walks every directory in a
    ROOT file (best-effort: skips trees, TGraphs, TF1s, anything
    that isn't a 1D/2D histogram) and returns
    ``[(path, classname), …]``.  ``path`` is the full slash-
    separated key path including subdirs.
  - ``render_histogram(ax, hist)`` draws the histogram onto a
    matplotlib ``Axes``.  TH1F/D/I → step plot; TH2F/D/I → imshow.
    Errors are swallowed and replaced with a "couldn't render"
    label so one bad histogram doesn't tank the whole grid.

The QA tab pulls the list at most once per run + per file, then
constructs a matplotlib FigureCanvas per histogram.  Discovery is
non-recursive into ``Config/`` (that's the parameter-dump
TDirectory the writers emit — not histograms, and it'd just clutter
the grid).
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable, Optional

import matplotlib  # for cm.get_cmap (TH2 zero-masking, dark-mode legibility)
from matplotlib.colors import LinearSegmentedColormap


#  Two-stop colormap used for TH2s in dark mode: coral (``#FF6B6B``)
#  for the low end of the data → bright green (``#0BDA51``) for the
#  high end.  Read order matches the operator's intent (busy bins
#  pop green against the dark surround, near-empty bins fade to a
#  warm coral that still reads on the lightened ``#222`` facecolor).
#  Both anchors visually match the rest of the dashboard's accent
#  palette (Run-info quality chip green + step-plot coral red).
#  Light mode keeps the standard ``magma`` since its low end is
#  light enough not to vanish against a white facecolor.
_DRICH_DARK_CMAP = LinearSegmentedColormap.from_list(
    "drich_dark_coral_green",
    ["#FF6B6B", "#0BDA51"],
    N=256,
)


# Directories we *never* descend into when enumerating histograms.
# ``Config/`` is the writer-side parameter dump (TParameter / TNamed)
# that has its own dedicated viewer in DataInspectPane.  Anything
# else with these names follows the same rationale.
_SKIP_DIRS: set[str] = {"Config"}

# uproot classnames we treat as renderable histograms.  Anything else
# (TTrees, TGraphs, TF1s, etc.) is silently skipped.
_HIST_CLASSES: tuple[str, ...] = (
    "TH1F", "TH1D", "TH1I", "TH1S", "TH1C",
    "TH2F", "TH2D", "TH2I", "TH2S", "TH2C",
    "TProfile", "TProfile2D",
)


def enumerate_histograms(root_path: Path) -> list[tuple[str, str]]:
    """Return ``[(full_path, classname), …]`` for every histogram.

    Walks subdirectories.  Returns an empty list (no exception) on
    missing / corrupt file or when uproot isn't installed.
    """
    if not root_path.is_file():
        return []
    try:
        import uproot
    except ImportError:
        return []
    out: list[tuple[str, str]] = []
    try:
        with uproot.open(root_path) as f:
            _walk(f, "", out)
    except Exception:  # noqa: BLE001 — best-effort
        return []
    return out


def _walk(node, prefix: str, out: list[tuple[str, str]]) -> None:
    """Recursive depth-first walk over a uproot directory.

    ``node.keys()`` returns ``"<name>;<cycle>"`` for each entry; we
    strip the cycle and look up ``classnames`` to decide whether
    to recurse (a TDirectory) or emit (a histogram).
    """
    try:
        names = list(node.keys(recursive=False, cycle=False))
    except TypeError:
        # Older uproot — fall back to .keys() without args.
        names = [k.split(";")[0] for k in node.keys()]
    try:
        cls = node.classnames()
    except Exception:  # noqa: BLE001
        cls = {}
    seen: set[str] = set()
    for name in names:
        if name in seen:
            continue
        seen.add(name)
        classname = cls.get(name) or cls.get(f"{name};1") or ""
        full = f"{prefix}/{name}" if prefix else name
        if classname.startswith("TDirectory") or classname == "ROOT::TDirectoryFile":
            if name in _SKIP_DIRS:
                continue
            try:
                sub = node[name]
            except Exception:  # noqa: BLE001
                continue
            _walk(sub, full, out)
            continue
        if classname in _HIST_CLASSES:
            out.append((full, classname))


def render_histogram(
    ax,
    hist,
    *,
    title: Optional[str] = None,
    dark: bool = False,
) -> None:
    """Draw ``hist`` (an uproot histogram object) onto ``ax``.

    ``dark`` flips the axis / label / tick colours to white so the
    plot stays readable when the surrounding Qt card is dark.  The
    Figure / Axes background patches are kept transparent (the caller
    is responsible for ``fig.patch.set_alpha(0.0)`` /
    ``ax.patch.set_alpha(0.0)``) so the card's own surface shows
    through and we don't get a mismatched white box.

    Quiet on failure — paints a small error message into ``ax`` and
    returns, so a broken histogram doesn't break the whole grid.
    """
    fg = "#E8E8E8" if dark else "#1B1A1A"
    muted = "#9B8E8E"
    #  Colormap choice + axes background.  On dark backgrounds the
    #  matplotlib defaults bottom-out near-black, so a low-occupancy
    #  TH2 bin is invisible.  Two-axis fix:
    #    1. give the axes a slightly-lighter facecolor so the cells
    #       themselves are visible as a faint grid even when empty;
    #    2. mask zero-valued bins (handled in the TH2 branch below)
    #       so the dark facecolor shows through for true "no data".
    #  For the colormap itself: use the dashboard's custom
    #  green→coral gradient in dark mode (operator-picked accents,
    #  ``#0BDA51`` ↔ ``#FF6B6B``) and stick with ``magma`` in light
    #  mode (its low end is light enough on white).
    cmap_for_th2 = _DRICH_DARK_CMAP if dark else matplotlib.cm.get_cmap("magma")
    axes_bg = "#222222" if dark else "#FFFFFF"
    #  TH1 line/trace colour: coral on light theme stays visible;
    #  on dark we want a brighter accent so the step plot pops
    #  against the lighter axes background.
    line_colour = "#FF8E8E" if dark else "#FF6B6B"

    def _apply_axes_style() -> None:
        # Tick / label / spine / facecolor — invoked after every draw
        # so they survive any matplotlib defaults the .step / .imshow
        # path may have re-applied.
        ax.set_facecolor(axes_bg)
        ax.tick_params(axis="both", which="major",
                       labelsize=7, colors=fg)
        for spine in ax.spines.values():
            spine.set_color(fg)
        ax.xaxis.label.set_color(fg)
        ax.yaxis.label.set_color(fg)
        if ax.title:
            ax.title.set_color(fg)

    try:
        # uproot's .to_numpy() returns (values, edges) for 1D and
        # (values, x_edges, y_edges) for 2D.  We branch on the tuple
        # length rather than the classname because that's robust to
        # subclassing (TProfile etc.).
        data = hist.to_numpy()
    except Exception as exc:  # noqa: BLE001
        ax.text(0.5, 0.5, f"(no data)\n{type(exc).__name__}",
                ha="center", va="center", transform=ax.transAxes,
                fontsize=8, color=muted)
        ax.set_xticks([]); ax.set_yticks([])
        if title:
            ax.set_title(title, fontsize=8)
        _apply_axes_style()
        return

    if len(data) == 2:
        values, edges = data
        # Step plot centred on bin edges.  Filled accent on a slightly
        # transparent face so the plot pops on either light or dark
        # background without hiding the grid.  Linewidth bumped to
        # 1.4 in dark mode — at 1.0 the trace got eaten by the
        # background even with a bright colour.
        centres = 0.5 * (edges[:-1] + edges[1:])
        ax.step(centres, values, where="mid",
                linewidth=1.4 if dark else 1.0, color=line_colour)
        ax.fill_between(centres, values, step="mid",
                        color=line_colour, alpha=0.28 if dark else 0.18)
        ax.set_ylim(bottom=0)
    elif len(data) == 3:
        values, x_edges, y_edges = data
        # imshow with origin=lower so the y axis grows the usual way.
        #
        # Mask zero-valued bins so they render transparent (showing
        # the lighter ``axes_bg`` through), instead of mapping to
        # the colormap's near-black floor.  Without this, an
        # almost-empty TH2 on dark mode is one solid black slab — the
        # whole reason the operator flagged this as unreadable.
        import numpy as np
        masked = np.ma.masked_equal(values.T, 0)
        #  Copy so ``set_bad`` doesn't permanently mutate a shared
        #  global colormap (matters because we hand back the same
        #  ``_DRICH_DARK_CMAP`` instance every call in dark mode).
        cmap_obj = cmap_for_th2.copy()
        cmap_obj.set_bad(color="none")  # masked → transparent
        ax.imshow(
            masked,
            origin="lower",
            extent=[x_edges[0], x_edges[-1], y_edges[0], y_edges[-1]],
            aspect="auto",
            cmap=cmap_obj,
        )
    else:
        ax.text(0.5, 0.5, "(unsupported)",
                ha="center", va="center", transform=ax.transAxes,
                fontsize=8, color=muted)
        ax.set_xticks([]); ax.set_yticks([])
        if title:
            ax.set_title(title, fontsize=8)
        _apply_axes_style()
        return

    if title:
        ax.set_title(title, fontsize=9, color=fg)
    _apply_axes_style()


def load_histogram(root_path: Path, hist_path: str):
    """Open ``root_path`` and return the histogram at ``hist_path``.

    Returns ``None`` on any error (file missing, key not found,
    uproot import failure).  Callers should handle ``None`` by
    rendering a placeholder.
    """
    if not root_path.is_file():
        return None
    try:
        import uproot
    except ImportError:
        return None
    try:
        f = uproot.open(root_path)
        return f[hist_path]
    except Exception:  # noqa: BLE001
        return None


def enumerate_histograms_tree(root_path: Path) -> dict:
    """Walk the ROOT file as a nested ``{hists, subdirs}`` tree.

    Recursive mirror of the file's TDirectory layout — sub-sub-dirs
    (``Triggers/TIMING/h_xxx``) become nested tree nodes naturally.
    The dashboard renders one collapsible card per node so the
    structure on disk = the structure on screen, no hardwiring.

    Shape::

        {
          "hists":   [(full_path, classname), …],   # this dir's own
          "subdirs": {"<name>": <same-shaped node>, …},
        }

    Returns an empty tree (``{"hists": [], "subdirs": {}}``) on
    missing file / uproot import failure, so the caller can render
    a "nothing yet" state without special-casing.
    """
    empty = {"hists": [], "subdirs": {}}
    if not root_path.is_file():
        return empty
    try:
        import uproot
    except ImportError:
        return empty
    try:
        with uproot.open(root_path) as f:
            return _walk_tree(f)
    except Exception:  # noqa: BLE001
        return empty


def _walk_tree(node, prefix: str = "") -> dict:
    """Recursive helper: build the nested ``{hists, subdirs}`` node."""
    out = {"hists": [], "subdirs": {}}
    try:
        names = list(node.keys(recursive=False, cycle=False))
    except TypeError:
        names = [k.split(";")[0] for k in node.keys()]
    try:
        cls = node.classnames()
    except Exception:  # noqa: BLE001
        cls = {}
    seen: set[str] = set()
    for name in names:
        if name in seen:
            continue
        seen.add(name)
        classname = cls.get(name) or cls.get(f"{name};1") or ""
        full = f"{prefix}/{name}" if prefix else name
        if classname.startswith("TDirectory") or classname == "ROOT::TDirectoryFile":
            if name in _SKIP_DIRS:
                continue
            try:
                sub = node[name]
            except Exception:  # noqa: BLE001
                continue
            out["subdirs"][name] = _walk_tree(sub, full)
            continue
        if classname in _HIST_CLASSES:
            out["hists"].append((full, classname))
    return out


def enumerate_histograms_grouped(
    root_path: Path,
) -> dict[str, list[tuple[str, str]]]:
    """Group histograms by their parent TDirectory.

    Returns ``{ "<dir-path>": [(full_path, classname), …] }`` where
    ``"<dir-path>"`` is the slash-joined TDirectory path of the
    histogram's container (empty string for the file's root level).
    Histograms inside nested subdirs report their *immediate* parent
    only — a 2-deep walker would handle deeper trees by recursing
    naturally; today's writers stay one level deep so this is fine.

    Order: dicts preserve insertion order; we walk the file with
    ``enumerate_histograms`` (which is already top-down + sorted by
    subdir name within each level), then bin by directory.  The
    caller can iterate ``dict.items()`` in stable order without an
    extra sort.
    """
    out: dict[str, list[tuple[str, str]]] = {}
    for full_path, classname in enumerate_histograms(root_path):
        if "/" in full_path:
            dirname = "/".join(full_path.split("/")[:-1])
        else:
            dirname = ""
        out.setdefault(dirname, []).append((full_path, classname))
    return out


__all__ = [
    "enumerate_histograms",
    "enumerate_histograms_grouped",
    "enumerate_histograms_tree",
    "load_histogram",
    "render_histogram",
]
