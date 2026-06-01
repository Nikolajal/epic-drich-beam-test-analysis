"""Unit tests for ``qa_quicklook.thumbs``.

Covers the legibility-critical bits that broke once in dark mode:
  - The custom green→coral colormap is registered with the expected
    end anchors (``#FF6B6B`` low, ``#0BDA51`` high).
  - TH2 rendering masks zero-valued bins (renders the dark facecolor
    through them).
  - TH1 rendering uses the brighter line colour + thicker stroke in
    dark mode.
  - Render is robust against an exception from ``to_numpy`` (broken
    histogram → "(no data)" placeholder, no crash).

Run::

    .venv/bin/python -m unittest tests.test_qa_quicklook_thumbs
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

#  Force Agg before any matplotlib import so the tests run headless.
import matplotlib  # noqa: E402
matplotlib.use("Agg", force=True)
import matplotlib.pyplot as plt  # noqa: E402

from qa_quicklook import thumbs  # noqa: E402


class _FakeTH2:
    """uproot-shaped 3-tuple histogram."""

    def __init__(self, values: np.ndarray, x_edges: np.ndarray, y_edges: np.ndarray):
        self._v = values
        self._x = x_edges
        self._y = y_edges

    def to_numpy(self):
        return self._v, self._x, self._y


class _FakeTH1:
    def __init__(self, values: np.ndarray, edges: np.ndarray):
        self._v = values
        self._e = edges

    def to_numpy(self):
        return self._v, self._e


class _BrokenHist:
    def to_numpy(self):
        raise RuntimeError("uproot blew up")


class TestCustomCmap(unittest.TestCase):
    def test_low_end_is_coral(self) -> None:
        r, g, b, _a = thumbs._DRICH_DARK_CMAP(0.0)
        #  #FF6B6B ≈ (1.0, 0.42, 0.42)
        self.assertAlmostEqual(r, 1.0, places=2)
        self.assertAlmostEqual(g, 0.42, places=2)
        self.assertAlmostEqual(b, 0.42, places=2)

    def test_high_end_is_green(self) -> None:
        r, g, b, _a = thumbs._DRICH_DARK_CMAP(1.0)
        #  #0BDA51 ≈ (0.043, 0.855, 0.318)
        self.assertAlmostEqual(r, 0.04, places=2)
        self.assertAlmostEqual(g, 0.85, places=2)
        self.assertAlmostEqual(b, 0.32, places=2)


class TestTh2Render(unittest.TestCase):
    def test_zero_bins_are_masked(self) -> None:
        values = np.zeros((4, 4))
        values[1, 2] = 5.0
        h = _FakeTH2(values, np.arange(5), np.arange(5))
        fig, ax = plt.subplots()
        try:
            thumbs.render_histogram(ax, h, title="t", dark=True)
            #  Find the AxesImage that imshow added.
            images = [c for c in ax.get_children()
                      if isinstance(c, matplotlib.image.AxesImage)]
            self.assertEqual(len(images), 1, "expected one imshow layer")
            arr = images[0].get_array()
            #  np.ma.masked_equal mapping: zero-valued bins are masked.
            self.assertTrue(np.ma.is_masked(arr))
            #  The transposed entry [2, 1] (was [1, 2]) is NOT masked.
            self.assertFalse(arr.mask[2, 1])
        finally:
            plt.close(fig)


class TestTh1Render(unittest.TestCase):
    def test_dark_mode_line_colour_is_bright_coral(self) -> None:
        values = np.array([0, 1, 4, 9, 4, 1, 0], dtype=float)
        edges = np.arange(len(values) + 1, dtype=float)
        h = _FakeTH1(values, edges)
        fig, ax = plt.subplots()
        try:
            thumbs.render_histogram(ax, h, title="t", dark=True)
            #  The step plot lives as a Line2D on the Axes.
            lines = ax.get_lines()
            self.assertTrue(lines, "expected at least one step Line2D")
            colour = lines[0].get_color()
            #  Dark mode uses #FF8E8E (brighter than the light-mode #FF6B6B).
            self.assertEqual(colour.lower(), "#ff8e8e")
            self.assertGreater(lines[0].get_linewidth(), 1.0)
        finally:
            plt.close(fig)

    def test_light_mode_line_colour_is_coral(self) -> None:
        h = _FakeTH1(np.array([0, 1, 2], dtype=float),
                     np.array([0, 1, 2, 3], dtype=float))
        fig, ax = plt.subplots()
        try:
            thumbs.render_histogram(ax, h, title="t", dark=False)
            lines = ax.get_lines()
            self.assertEqual(lines[0].get_color().lower(), "#ff6b6b")
        finally:
            plt.close(fig)


class TestBrokenHist(unittest.TestCase):
    def test_renders_no_data_placeholder_without_raising(self) -> None:
        fig, ax = plt.subplots()
        try:
            #  Should not raise — the (no data) branch handles
            #  ``to_numpy`` exceptions.
            thumbs.render_histogram(ax, _BrokenHist(), title="t", dark=True)
            texts = [t.get_text() for t in ax.texts]
            self.assertTrue(any("no data" in s for s in texts))
        finally:
            plt.close(fig)


if __name__ == "__main__":
    unittest.main()
