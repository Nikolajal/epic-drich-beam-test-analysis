"""Unit tests for ``qa_quicklook.readout_validate``.

Covers:
  - chip-set expansion (``"*"`` → all 8, list → set, malformed → empty)
  - overlap detection across cherenkov / timing / tracking roles
  - chip-range folding in the human-readable output
  - defensive parsing of malformed TOML structures

Run::

    .venv/bin/python -m unittest tests.test_qa_quicklook_readout_validate
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from qa_quicklook.readout_validate import (  # noqa: E402
    ALCOR_CHIPS_PER_DEVICE,
    _expand_chips,
    _format_chip_spec,
    find_overlaps,
)


class TestExpandChips(unittest.TestCase):
    def test_wildcard_returns_all_eight(self) -> None:
        self.assertEqual(_expand_chips("*"), set(range(ALCOR_CHIPS_PER_DEVICE)))

    def test_list_returns_set_of_ints(self) -> None:
        self.assertEqual(_expand_chips([0, 2, 4]), {0, 2, 4})

    def test_tuple_works_too(self) -> None:
        self.assertEqual(_expand_chips((1, 3, 5)), {1, 3, 5})

    def test_string_list_quoted_is_malformed(self) -> None:
        # ``chips = "[0, 2]"`` (operator typo: list wrapped in quotes)
        # falls into the unrecognised-string branch.
        self.assertEqual(_expand_chips("[0, 2]"), set())

    def test_none_is_empty(self) -> None:
        self.assertEqual(_expand_chips(None), set())

    def test_mixed_list_drops_non_ints(self) -> None:
        self.assertEqual(_expand_chips([0, "two", 4, None]), {0, 4})


class TestFormatChipSpec(unittest.TestCase):
    def test_empty(self) -> None:
        self.assertEqual(_format_chip_spec([]), "(none)")

    def test_single(self) -> None:
        self.assertEqual(_format_chip_spec([3]), "3")

    def test_contiguous_run_folds(self) -> None:
        self.assertEqual(_format_chip_spec([0, 1, 2, 3, 4, 5, 6, 7]), "0-7")

    def test_isolated_indices_stay_listed(self) -> None:
        self.assertEqual(_format_chip_spec([0, 2, 4]), "0, 2, 4")

    def test_mixed_runs_and_isolated(self) -> None:
        self.assertEqual(_format_chip_spec([0, 1, 4, 5, 7]), "0-1, 4-5, 7")


class TestFindOverlaps(unittest.TestCase):
    def test_clean_doc_returns_empty(self) -> None:
        doc = {
            "readout": {
                "cherenkov": {"devices": [{"id": 192, "chips": "*"}]},
                "timing":    {"devices": [{"id": 201, "chips": "*"}]},
                "tracking":  {"devices": []},
            }
        }
        self.assertEqual(find_overlaps(doc), [])

    def test_chip_in_two_roles_flagged(self) -> None:
        doc = {
            "readout": {
                "cherenkov": {"devices": [{"id": 200, "chips": "*"}]},
                "timing":    {"devices": [{"id": 200, "chips": [0, 2]}]},
            }
        }
        out = find_overlaps(doc)
        self.assertEqual(len(out), 1)
        msg = out[0]
        self.assertIn("device 200", msg)
        self.assertIn("Cherenkov", msg)
        self.assertIn("Timing", msg)
        #  Non-contiguous chips render listed, not range-folded.
        self.assertIn("0, 2", msg)

    def test_full_wildcard_overlap_renders_as_range(self) -> None:
        doc = {
            "readout": {
                "cherenkov": {"devices": [{"id": 200, "chips": "*"}]},
                "timing":    {"devices": [{"id": 200, "chips": "*"}]},
            }
        }
        out = find_overlaps(doc)
        self.assertEqual(len(out), 1)
        self.assertIn("0-7", out[0])

    def test_three_role_overlap_lists_all_three(self) -> None:
        doc = {
            "readout": {
                "cherenkov": {"devices": [{"id": 200, "chips": [3]}]},
                "timing":    {"devices": [{"id": 200, "chips": [3]}]},
                "tracking":  {"devices": [{"id": 200, "chips": [3]}]},
            }
        }
        out = find_overlaps(doc)
        self.assertEqual(len(out), 1)
        for role in ("Cherenkov", "Timing", "Tracking"):
            self.assertIn(role, out[0])

    def test_singular_chip_label_when_one_chip(self) -> None:
        doc = {
            "readout": {
                "cherenkov": {"devices": [{"id": 200, "chips": [3]}]},
                "timing":    {"devices": [{"id": 200, "chips": [3]}]},
            }
        }
        out = find_overlaps(doc)
        self.assertIn("chip 3", out[0])
        self.assertNotIn("chips 3", out[0])

    def test_missing_readout_section_returns_empty(self) -> None:
        self.assertEqual(find_overlaps({}), [])
        self.assertEqual(find_overlaps({"other": "thing"}), [])

    def test_malformed_devices_dont_crash(self) -> None:
        doc = {
            "readout": {
                "cherenkov": {"devices": [
                    "bare-string-entry",
                    {"id": "not-an-int", "chips": "*"},
                    {"id": 192, "chips": "*"},
                ]},
                "timing": {"devices": [{"id": 192, "chips": "*"}]},
            }
        }
        out = find_overlaps(doc)
        #  Only the well-formed 192 overlap survives.
        self.assertEqual(len(out), 1)
        self.assertIn("192", out[0])

    def test_separate_devices_no_overlap(self) -> None:
        doc = {
            "readout": {
                "cherenkov": {"devices": [{"id": 192, "chips": "*"}]},
                "timing":    {"devices": [{"id": 193, "chips": "*"}]},
            }
        }
        self.assertEqual(find_overlaps(doc), [])


if __name__ == "__main__":
    unittest.main()
