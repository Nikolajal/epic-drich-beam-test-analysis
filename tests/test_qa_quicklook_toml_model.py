"""Unit tests for ``qa_quicklook.toml_model``.

Pure tomlkit, no Qt.  Covers the walker (every kind we render in the
form), the leaf setter (scalar overwrite + array overwrite + complex
refusal), and the round-trip + replace helpers used by the
write-back path.

We also exercise the helpers against every file in ``conf/`` because
the form widget *only* works as well as the walker does on the real
inputs; a regression in the walker for any real config silently
breaks the Settings tab.

Run::

    .venv/bin/python -m unittest tests.test_qa_quicklook_toml_model
"""

from __future__ import annotations

import sys
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

import tomlkit  # noqa: E402

from qa_quicklook.toml_model import (  # noqa: E402
    Leaf,
    replace_document_text,
    roundtrip_safe,
    set_leaf,
    walk_leaves,
)


CONF_DIR = REPO_ROOT / "conf"


class TestWalkLeaves(unittest.TestCase):
    def test_flat_scalar_table(self):
        doc = tomlkit.parse(textwrap.dedent('''
            [framer]
            frame_size = 1024
            label = "x"
            enabled = true
            scale = 1.5
        '''))
        leaves = list(walk_leaves(doc))
        by_path = {l.path: l for l in leaves}
        self.assertEqual(by_path[("framer", "frame_size")].kind, "int")
        self.assertEqual(by_path[("framer", "frame_size")].value, 1024)
        self.assertEqual(by_path[("framer", "label")].kind, "str")
        self.assertEqual(by_path[("framer", "enabled")].kind, "bool")
        self.assertEqual(by_path[("framer", "scale")].kind, "float")

    def test_array_of_tables_indexed_by_position(self):
        doc = tomlkit.parse(textwrap.dedent('''
            [[trigger]]
            name = "first"
            index = 0
            [[trigger]]
            name = "second"
            index = 1
        '''))
        leaves = list(walk_leaves(doc))
        names = [l for l in leaves if l.path[-1] == "name"]
        self.assertEqual([l.path for l in names], [("trigger", 0, "name"), ("trigger", 1, "name")])
        self.assertEqual([l.value for l in names], ["first", "second"])

    def test_scalar_arrays_classified_by_element_type(self):
        doc = tomlkit.parse(textwrap.dedent('''
            [a]
            ints = [1, 2, 3]
            floats = [1.0, 2.5]
            mixed_numeric = [1, 2.0]
            strings = ["x", "y"]
        '''))
        kinds = {l.path[-1]: l.kind for l in walk_leaves(doc)}
        self.assertEqual(kinds["ints"], "int_array")
        self.assertEqual(kinds["floats"], "float_array")
        # int + float coexist → widen to float array.
        self.assertEqual(kinds["mixed_numeric"], "float_array")
        self.assertEqual(kinds["strings"], "str_array")

    def test_inline_table_array_classified_as_table_array(self):
        """List of inline tables → table_array (rendered as a mini-table)."""
        doc = tomlkit.parse(textwrap.dedent('''
            [readout.cherenkov]
            devices = [{ id = 192, chips = "*" }, { id = 193, chips = "*" }]
        '''))
        leaves = list(walk_leaves(doc))
        leaf = next(l for l in leaves if l.path[-1] == "devices")
        self.assertEqual(leaf.kind, "table_array")

    def test_empty_array_is_str_array(self):
        doc = tomlkit.parse('[a]\nempty = []\n')
        leaf = next(walk_leaves(doc))
        self.assertEqual(leaf.kind, "str_array")
        self.assertEqual(leaf.value, [])


class TestSetLeaf(unittest.TestCase):
    def test_overwrite_int_preserves_other_lines(self):
        original = textwrap.dedent('''\
            # leading comment
            [framer]
            frame_size = 1024    # inline comment
            other = 5
        ''')
        new_text = replace_document_text(original, ("framer", "frame_size"), 2048)
        # The inline comment stays put; only the value changes.
        self.assertIn("frame_size = 2048", new_text)
        self.assertIn("# inline comment", new_text)
        self.assertIn("# leading comment", new_text)
        self.assertIn("other = 5", new_text)

    def test_overwrite_in_array_of_tables(self):
        original = textwrap.dedent('''\
            [[trigger]]
            name = "first"
            delay = 100

            [[trigger]]
            name = "second"
            delay = 200
        ''')
        new_text = replace_document_text(original, ("trigger", 1, "delay"), 250)
        # Second entry's delay updates; first is untouched.
        self.assertIn('name = "second"', new_text)
        self.assertIn("delay = 250", new_text)
        self.assertIn("delay = 100", new_text)

    def test_overwrite_scalar_array(self):
        original = '[a]\nxs = [1, 2, 3]\n'
        new_text = replace_document_text(original, ("a", "xs"), [4, 5, 6])
        self.assertIn("[4, 5, 6]", new_text)

    def test_set_leaf_accepts_table_array_replacement(self):
        """An inline-table array is editable as a whole list."""
        doc = tomlkit.parse(textwrap.dedent('''
            [readout.cherenkov]
            devices = [{ id = 192, chips = "*" }]
        '''))
        set_leaf(
            doc, ("readout", "cherenkov", "devices"),
            [{"id": 192, "chips": "*"}, {"id": 200, "chips": [0, 2]}],
        )
        rendered = tomlkit.dumps(doc)
        self.assertIn("id = 200", rendered)

    def test_missing_path_raises(self):
        doc = tomlkit.parse("[a]\nx = 1\n")
        with self.assertRaises(Exception):  # tomlkit raises NonExistentKey
            set_leaf(doc, ("a", "nope"), 2)


class TestRoundTripSafe(unittest.TestCase):
    def test_simple_doc_is_safe(self):
        self.assertTrue(roundtrip_safe('[a]\nx = 1\n'))

    def test_garbage_is_not_safe(self):
        self.assertFalse(roundtrip_safe('this = not = toml'))


class TestRealConfFiles(unittest.TestCase):
    """Make sure every conf/*.toml is round-trip safe and walkable.

    A walker regression on a real config would silently break the
    Settings tab — catch it here at test time, not at GUI time.
    """

    @classmethod
    def setUpClass(cls):
        if not CONF_DIR.is_dir():
            raise unittest.SkipTest(f"no {CONF_DIR}")
        cls.files = sorted(CONF_DIR.glob("*.toml"))
        if not cls.files:
            raise unittest.SkipTest("no conf/*.toml files")

    def test_all_files_roundtrip(self):
        for path in self.files:
            with self.subTest(file=path.name):
                self.assertTrue(roundtrip_safe(path.read_text()),
                                f"{path.name} does not round-trip through tomlkit")

    def test_all_files_walkable(self):
        for path in self.files:
            with self.subTest(file=path.name):
                doc = tomlkit.parse(path.read_text())
                leaves = list(walk_leaves(doc))
                self.assertGreater(len(leaves), 0, f"{path.name} produced no leaves")
                # Every leaf has a non-empty path and a recognised kind.
                kinds = {"bool", "int", "float", "str",
                         "int_array", "float_array", "str_array",
                         "table_array", "complex"}
                for leaf in leaves:
                    self.assertGreater(len(leaf.path), 0)
                    self.assertIn(leaf.kind, kinds)


if __name__ == "__main__":
    unittest.main()


class ResultsLoadTests(unittest.TestCase):
    """``rundb.results_load`` round-trip from the AnalysisResults TOML schema."""

    def setUp(self) -> None:
        import tempfile
        self._tmp = Path(tempfile.mkdtemp(prefix="qaq-results-"))

    def test_missing_file_returns_empty(self) -> None:
        from qa_quicklook import rundb
        out = rundb.results_load(self._tmp / "no-such.toml")
        self.assertEqual(out, {})

    def test_parses_canonical_schema(self) -> None:
        from qa_quicklook import rundb
        p = self._tmp / "r.toml"
        p.write_text(
            '[results."20251111-181940"."1350"]\n'
            '"lightdata.n_events"        = { value = 12345 }\n'
            '"recodata.ex_gap.n_gamma"   = { value = 12.3, error = 0.5 }\n'
            '\n'
            '[results."20251111-181940"."1375"]\n'
            '"recodata.ex_gap.n_gamma"   = { value = 11.8, error = 0.5 }\n'
        )
        out = rundb.results_load(p)
        self.assertIn("20251111-181940", out)
        runs = out["20251111-181940"]
        self.assertEqual(set(runs.keys()), {"1350", "1375"})
        # Error optional → defaults to 0.0.
        self.assertEqual(runs["1350"]["lightdata.n_events"].value, 12345.0)
        self.assertEqual(runs["1350"]["lightdata.n_events"].error, 0.0)
        # Error present is preserved.
        self.assertAlmostEqual(
            runs["1350"]["recodata.ex_gap.n_gamma"].value, 12.3,
        )
        self.assertAlmostEqual(
            runs["1350"]["recodata.ex_gap.n_gamma"].error, 0.5,
        )

    def test_unparseable_file_returns_empty(self) -> None:
        from qa_quicklook import rundb
        p = self._tmp / "bad.toml"
        p.write_text("this is { not valid toml")
        self.assertEqual(rundb.results_load(p), {})
