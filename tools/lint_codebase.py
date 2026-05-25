#!/usr/bin/env python3
"""
Static lint for beam-test-analysis source — catches the bug classes we
hit during the Phase-5 GlobalIndex migration.

Currently checks:

  R1  Histogram Fill argument mismatch:
      `->Fill(EXPR, ...)` where EXPR pulls a raw packed GlobalIndex value
      (`get_global_index()`, `get_global_tdc_index()`, `global_channel_raw()`)
      WITHOUT wrapping it in a dense-ordinal accessor
      (`channel_ordinal()`, `tdc_ordinal()`).  Catches the entire class of
      Phase-5 fill-target overflows.

  R2  Debug-leftover histogram names:
      `RootHist<...> NAME(NAME_STRING, ...)` or `new TH*(NAME_STRING, ...)`
      where NAME_STRING matches `test\\d*`, `tmp.*`, `foo.*`, `debug.*`.
      Catches debug histograms that survive into output files.

  R3  Commented-out function-call lines:
      `// var.method(...)` or `// func(...)` at file scope.
      Catches lines like the `//framer.resolve_rollover_offsets();` that
      silently disabled the Rollover QA fills.  Some commented calls are
      legitimate (deprecated, see-also examples) — those should be moved
      to a real comment block with explanation rather than left as
      one-line ghosts of code.

  R4  Legacy Phase-4 bit-bashing formulas:
      `GlobalIndex / 4`, `/ 256`, `% 256`, `(chan % NNN) / 8` — these
      reverse-engineer the pre-Phase-5 packed layout and produce wrong
      values under the new layout.  Use `gi.device() / .fifo() /
      .channel_ordinal() / .tdc_ordinal()` instead.

Exit codes:
    0  — clean
    1  — at least one finding
    2  — file read / scanning error

Usage:
    tools/lint_codebase.py                              # scan default tree
    tools/lint_codebase.py src include macros          # scan specific dirs
    tools/lint_codebase.py --only R1                   # one rule only
    tools/lint_codebase.py --skip R3                   # mute one rule

Add new rules by extending the `RULES` list below.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


# ── Helpers ────────────────────────────────────────────────────────────────

@dataclass
class Finding:
    rule: str
    path: str        # repo-relative
    line: int
    snippet: str
    note: str = ""


def _is_in_block_comment(text: str, idx: int) -> bool:
    """Best-effort: returns True if idx is inside a /* ... */ block.
    Counts unbalanced /* and */ tokens before idx."""
    head = text[:idx]
    # Strip line comments first to avoid // /* false positives.
    head = re.sub(r"//[^\n]*", "", head)
    opens = head.count("/*")
    closes = head.count("*/")
    return opens > closes


def _is_suppressed(text: str, idx: int) -> bool:
    """True if the file or the line containing idx (or the immediately
    preceding line) carries a `LINT-OK` marker.

    Convention:
      // LINT-OK: <reason>             — suppresses findings on this line
                                         or the immediately following line
                                         (useful for placing the marker
                                         above a // disabled-code line).
      // LINT-OK-FILE: <reason>        — suppresses findings anywhere in
                                         the file (use for scratchpads).
    """
    # File-level marker: cheap whole-file substring check.
    if "LINT-OK-FILE" in text:
        return True
    line_start = text.rfind("\n", 0, idx) + 1
    line_end = text.find("\n", idx)
    if line_end == -1:
        line_end = len(text)
    if "LINT-OK" in text[line_start:line_end]:
        return True
    # Previous line:
    if line_start >= 2:
        prev_line_start = text.rfind("\n", 0, line_start - 1) + 1
        if "LINT-OK" in text[prev_line_start:line_start - 1]:
            return True
    return False


def _strip_line_comment(line: str) -> tuple[str, bool]:
    """Return (code-part-of-line, in_line_comment-bool).  Crude — string
    literals containing // would confuse this but are rare."""
    idx = line.find("//")
    if idx == -1:
        return line, False
    return line[:idx], True


# ── Rule implementations ──────────────────────────────────────────────────

# Patterns are precompiled at module load.

# R1: ->Fill( opens.  We extract the first argument by paren-tracking.
_FILL_OPEN = re.compile(r"->\s*Fill\s*\(")
# Suspicious raw getters that produce packed/sparse 32-bit values:
_RAW_GETTERS = re.compile(
    r"\b(get_global_index|get_global_tdc_index|global_channel_raw)\s*\("
)
# The wrappers that turn them into dense ordinals:
_ORDINAL_WRAPPERS = re.compile(r"\b(channel_ordinal|tdc_ordinal)\s*\(")

# R2: histogram name being a debug placeholder.
_HIST_DECL = re.compile(
    r"(RootHist\s*<\s*\w+\s*>\s+\w+\s*\(|new\s+(?:TH\w+|TProfile\w*)\s*\()\s*\"([^\"]+)\""
)
_DEBUG_NAME = re.compile(r"^(test\d*|tmp.*|foo.*|debug.*)$", re.IGNORECASE)

# R3: commented-out one-line function call.
# Matches lines like `// ident.method(...);` — must end with `);` (with
# optional whitespace before the line break) to filter out prose comments
# that happen to contain a parenthetical (e.g. `// Background (dashed)` or
# `// fit_circle(points to fit, R, …)`).  Real silently-disabled code
# almost always has the trailing semicolon; English prose does not.
_COMMENTED_CALL = re.compile(
    r"^\s*//\s*([A-Za-z_]\w*)(?:\.\w+|->[A-Za-z_]\w*)?\s*\([^;]*\)\s*;\s*$"
)
# Ignore commented calls inside conventional doxygen "@code ... @endcode" blocks
# and within an @example file.  Crude state machine handles those at scan time.

# R4: legacy bit-bashing formulas.
_LEGACY_PATTERNS = [
    (re.compile(r"\bGlobalIndex\s*/\s*4\b"), "GlobalIndex / 4 — use ::GlobalIndex(...).channel_ordinal() instead"),
    (re.compile(r"\b(?:get_)?global_tdc_index\s*\(\s*\)\s*/\s*4\b"), "tdc_index / 4 — use ::GlobalIndex(...).channel_ordinal() instead"),
    (re.compile(r"\b192\s*\+\s*(?:\w+\s*/\s*256|static_cast<\s*\w+\s*>\s*\(\s*\w+\s*/\s*256)"), "192 + chan/256 — use gi.device() from the GlobalIndex API"),
    (re.compile(r"\b\(\s*\w+\s*%\s*256\s*\)\s*/\s*8\b"), "(chan % 256) / 8 — use gi.fifo() from the GlobalIndex API"),
]

RULE_DESCRIPTIONS = {
    "R1": "histogram Fill x-arg uses a raw packed GlobalIndex getter without an ordinal wrapper",
    "R2": "histogram name is a debug placeholder (test, tmp, foo, debug)",
    "R3": "commented-out one-line function call (silent disable)",
    "R4": "legacy Phase-4 bit-bashing formula on GlobalIndex",
}


def _match_paren(src: str, open_idx: int) -> int:
    """Index of matching ')' for the '(' at @p open_idx, or -1."""
    assert src[open_idx] == "("
    depth = 1
    i = open_idx + 1
    n = len(src)
    in_str = False
    str_quote = ""
    while i < n and depth > 0:
        c = src[i]
        if in_str:
            if c == "\\" and i + 1 < n:
                i += 2
                continue
            if c == str_quote:
                in_str = False
            i += 1
            continue
        if c in ('"', "'"):
            in_str = True
            str_quote = c
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _line_of(src: str, idx: int) -> int:
    return src.count("\n", 0, idx) + 1


# ── Per-file scan ─────────────────────────────────────────────────────────

def scan_file(path: Path, enabled_rules: set[str]) -> list[Finding]:
    findings: list[Finding] = []
    text = path.read_text()
    rel = str(path)

    # ── R1: Fill argument mismatch ─────────────────────────────────────
    if "R1" in enabled_rules:
        for m in _FILL_OPEN.finditer(text):
            open_idx = m.end() - 1
            close_idx = _match_paren(text, open_idx)
            if close_idx == -1:
                continue
            if _is_in_block_comment(text, m.start()):
                continue
            if _is_suppressed(text, m.start()):
                continue
            args_text = text[open_idx + 1: close_idx]
            # Extract the first arg by paren-aware split.
            first_arg = _split_first_arg(args_text)
            if not first_arg:
                continue
            if _RAW_GETTERS.search(first_arg) and not _ORDINAL_WRAPPERS.search(first_arg):
                line = _line_of(text, m.start())
                snip = text[m.start(): close_idx + 1]
                snip = re.sub(r"\s+", " ", snip).strip()
                if len(snip) > 140:
                    snip = snip[:137] + "..."
                findings.append(Finding(
                    "R1", rel, line, snip,
                    note="Fill x-arg uses raw packed GlobalIndex — wrap in .channel_ordinal() or .tdc_ordinal()"))

    # ── R2: debug histogram names ─────────────────────────────────────
    if "R2" in enabled_rules:
        for m in _HIST_DECL.finditer(text):
            if _is_in_block_comment(text, m.start()):
                continue
            if _is_suppressed(text, m.start()):
                continue
            name = m.group(2)
            if _DEBUG_NAME.match(name):
                line = _line_of(text, m.start())
                snip = text[m.start(): m.end()]
                snip = re.sub(r"\s+", " ", snip).strip()
                findings.append(Finding(
                    "R2", rel, line, snip,
                    note=f"histogram name '{name}' looks like a debug leftover"))

    # ── R3: commented-out function-call lines ────────────────────────
    if "R3" in enabled_rules:
        # File-level suppression — short-circuit out for scratchpads etc.
        file_level_suppressed = ("LINT-OK-FILE" in text)
        # Track block-comment state line by line so we don't false-positive
        # on doxygen blocks etc.
        in_block = False
        for line_no, raw in enumerate(text.splitlines(), 1):
            ls = raw
            if file_level_suppressed:
                continue
            # crude block-comment tracking
            if in_block:
                if "*/" in ls:
                    in_block = False
                continue
            if "/*" in ls and "*/" not in ls:
                in_block = True
                continue
            # Match commented call (line must end in ');' — see comment on
            # _COMMENTED_CALL for the prose-filter rationale)
            m = _COMMENTED_CALL.match(ls)
            if m:
                ident = m.group(1)
                # Skip C++ keywords that can syntactically look like calls.
                if ident in {"if", "for", "while", "switch", "return", "static_cast",
                             "dynamic_cast", "reinterpret_cast", "const_cast",
                             "sizeof", "decltype", "typeid", "TODO", "NOTE", "FIXME"}:
                    continue
                # Per-line / previous-line LINT-OK suppression marker.
                if "LINT-OK" in ls:
                    continue
                # Previous line LINT-OK (e.g. marker on the line above the //call).
                if line_no >= 2:
                    prev = text.splitlines()[line_no - 2] if False else None
                    # cheap: re-walk one line back via splitlines is O(N);
                    # we can do better but the file is small.
                lines_seq = text.splitlines()
                if line_no >= 2 and "LINT-OK" in lines_seq[line_no - 2]:
                    continue
                findings.append(Finding(
                    "R3", rel, line_no, ls.strip(),
                    note=f"commented-out call to `{ident}(...)`. If intentional, convert to a real comment block explaining why."))

    # ── R4: legacy bit-bashing formulas ──────────────────────────────
    if "R4" in enabled_rules:
        for pat, hint in _LEGACY_PATTERNS:
            for m in pat.finditer(text):
                if _is_in_block_comment(text, m.start()):
                    continue
                if _is_suppressed(text, m.start()):
                    continue
                # Skip if line starts with //
                line_start = text.rfind("\n", 0, m.start()) + 1
                line_end = text.find("\n", m.end())
                line_end = len(text) if line_end == -1 else line_end
                full_line = text[line_start: line_end]
                code, _ = _strip_line_comment(full_line)
                if m.start() - line_start >= len(code):
                    continue  # match is in line comment
                line = _line_of(text, m.start())
                findings.append(Finding(
                    "R4", rel, line, full_line.strip(),
                    note=hint))

    return findings


def _split_first_arg(args_text: str) -> str:
    """Return the first top-level arg from a comma-separated arg list."""
    depth = 0
    in_str = False
    str_quote = ""
    for i, c in enumerate(args_text):
        if in_str:
            if c == "\\" and i + 1 < len(args_text):
                continue
            if c == str_quote:
                in_str = False
            continue
        if c in ('"', "'"):
            in_str = True
            str_quote = c
        elif c == "(" or c == "<":
            depth += 1
        elif c == ")" or c == ">":
            depth -= 1
        elif c == "," and depth == 0:
            return args_text[:i].strip()
    return args_text.strip()


# ── Driver ─────────────────────────────────────────────────────────────────

DEFAULT_DIRS = ["src", "include", "macros"]
CPP_EXTS = {".cxx", ".cpp", ".cc", ".h", ".hpp"}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Static lint for beam-test-analysis source.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=("Rules:\n" +
                "\n".join(f"  {r}: {d}" for r, d in RULE_DESCRIPTIONS.items())))
    parser.add_argument("paths", nargs="*", default=DEFAULT_DIRS,
                        help=f"Files or dirs to scan (default: {' '.join(DEFAULT_DIRS)})")
    parser.add_argument("--only", action="append", default=[],
                        help="Run only the given rule (e.g. --only R1).  Repeatable.")
    parser.add_argument("--skip", action="append", default=[],
                        help="Mute the given rule (e.g. --skip R3).  Repeatable.")
    parser.add_argument("--quiet", action="store_true",
                        help="Suppress per-rule descriptions in the header.")
    args = parser.parse_args(argv)

    all_rules = set(RULE_DESCRIPTIONS.keys())
    if args.only:
        enabled = set(args.only) & all_rules
    else:
        enabled = all_rules.copy()
    for r in args.skip:
        enabled.discard(r)
    if not enabled:
        print("ERROR: no rules enabled.", file=sys.stderr)
        return 2

    # Collect files
    files: list[Path] = []
    for p in args.paths:
        path = Path(p)
        if path.is_file():
            if path.suffix in CPP_EXTS:
                files.append(path)
        elif path.is_dir():
            for ext in CPP_EXTS:
                files.extend(path.rglob(f"*{ext}"))
        else:
            print(f"WARNING: skipping non-existent path: {p}", file=sys.stderr)

    if not args.quiet:
        print(f"# Scanning {len(files)} files with rules: {sorted(enabled)}")
        for r in sorted(enabled):
            print(f"#   {r}: {RULE_DESCRIPTIONS[r]}")
        print()

    findings: list[Finding] = []
    for f in sorted(files):
        try:
            findings.extend(scan_file(f, enabled))
        except OSError as e:
            print(f"WARNING: failed to read {f}: {e}", file=sys.stderr)

    # Group by rule, then by file
    by_rule: dict[str, list[Finding]] = {}
    for fnd in findings:
        by_rule.setdefault(fnd.rule, []).append(fnd)

    for rule in sorted(by_rule):
        print(f"── {rule} ── {RULE_DESCRIPTIONS[rule]}")
        for fnd in by_rule[rule]:
            print(f"  {fnd.path}:{fnd.line}: {fnd.note}")
            print(f"      | {fnd.snippet}")
        print()

    total = len(findings)
    counts = " ".join(f"{r}={len(v)}" for r, v in sorted(by_rule.items()))
    print(f"Summary: {total} finding(s)   {counts}")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
