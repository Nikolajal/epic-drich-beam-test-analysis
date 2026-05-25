#!/usr/bin/env python3
"""
QA content check for beam-test-analysis output ROOT files.

Walks every TDirectory in the file, examines every TH1/TH2/TH3/TProfile,
and flags:
  - EMPTY      — histogram has 0 entries (often a bug; sometimes legitimate)
  - OVERFLOW   — more than --overflow-frac of entries fell in over/underflow
                 (typical sign of wrong axis range or wrong fill value)
  - OK         — entries > 0 and overflow fraction < threshold

Run after lightdata_writer / recodata_writer to catch the class of bugs we
hit during the Phase-5 migration (per-channel TProfiles fed with sparse
packed indices, etc.).  See DISCUSSION.md §6.2 for the original symptom.

Usage:
    tools/check_qa.py path/to/lightdata.root
    tools/check_qa.py path/to/lightdata.root --strict        # exit nonzero on any EMPTY too
    tools/check_qa.py path/to/lightdata.root --overflow-frac 0.10
    tools/check_qa.py path/to/lightdata.root --known-empty 'Streaming Trigger/.*'

Exit codes:
    0  — all histograms healthy
    1  — at least one OVERFLOW finding
    2  — at least one EMPTY finding AND --strict was passed
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


# ── lazy import so --help works without ROOT in PATH ────────────────────────
def _import_root():
    try:
        import ROOT  # type: ignore
    except ImportError:
        print("ERROR: PyROOT not available.  Source thisroot.sh or activate "
              "a ROOT-enabled environment before running this script.",
              file=sys.stderr)
        sys.exit(3)
    ROOT.gROOT.SetBatch(True)
    return ROOT


@dataclass
class Finding:
    """One row of the report."""
    status: str       # EMPTY / OVERFLOW / OK
    path: str         # /Directory/.../HistName
    entries: float
    in_range: float
    overflow_count: float
    note: str = ""    # extra context, e.g. "known-empty" or "TProfile (Integral=0 != entries)"


def _total_overflow(h) -> float:
    """Sum of all over/underflow bins (works for TH1, TH2, TH3)."""
    nx = h.GetNbinsX()
    if h.InheritsFrom("TH3"):
        ny = h.GetNbinsY()
        nz = h.GetNbinsZ()
        with_of = h.Integral(0, nx + 1, 0, ny + 1, 0, nz + 1)
    elif h.InheritsFrom("TH2"):
        ny = h.GetNbinsY()
        with_of = h.Integral(0, nx + 1, 0, ny + 1)
    else:
        with_of = h.Integral(0, nx + 1)
    in_range = h.Integral()
    return with_of - in_range


def _classify(h, overflow_frac_threshold: float) -> Finding:
    """Return a Finding for a single histogram."""
    entries = h.GetEntries()
    in_range = h.Integral()
    overflow_count = _total_overflow(h)
    path = h.GetDirectory().GetPath().split(":", 1)[-1].lstrip("/")
    full = f"/{path}/{h.GetName()}" if path else f"/{h.GetName()}"

    if entries == 0:
        return Finding("EMPTY", full, entries, in_range, overflow_count)

    # For TProfiles, Integral() can be 0 even with non-zero entries (it's
    # the sum of bin "means", not the sum of weights).  Use the underlying
    # weight sum (GetSumOfWeights) as a sanity proxy.
    is_profile = h.InheritsFrom("TProfile") or h.InheritsFrom("TProfile2D")
    if is_profile:
        # For profiles, "overflow" means: the entries that landed outside
        # the binned range.  GetEntries counts ALL fills (including over);
        # the in-range count is GetEntries - overflow_count where the
        # overflow_count is computed from the per-bin entry counts.
        # Simpler proxy: the overflow weight from Integral(...) of
        # ProjectionX() — but for now compare entries to per-bin-entries sum.
        # If GetEntries() > 0 but every per-bin entries count is 0, all are
        # in overflow.
        nx = h.GetNbinsX()
        per_bin_entries_sum = sum(
            h.GetBinEntries(i) for i in range(1, nx + 1)
        )
        # Use per-bin entries instead of Integral for the threshold check.
        of_entries = entries - per_bin_entries_sum
        if entries > 0 and of_entries / entries > overflow_frac_threshold:
            return Finding(
                "OVERFLOW", full, entries, per_bin_entries_sum, of_entries,
                note="TProfile: most fills landed outside the binned range")
        return Finding(
            "OK", full, entries, per_bin_entries_sum, of_entries,
            note="TProfile")

    if entries > 0 and overflow_count / entries > overflow_frac_threshold:
        return Finding("OVERFLOW", full, entries, in_range, overflow_count)

    return Finding("OK", full, entries, in_range, overflow_count)


def _walk(root_dir, hists: list):
    """Depth-first walk over a TDirectory, collecting all TH/TProfile objects."""
    keys = root_dir.GetListOfKeys()
    for key in keys:
        obj = key.ReadObj()
        cls = obj.IsA().GetName()
        if obj.InheritsFrom("TDirectory"):
            _walk(obj, hists)
        elif obj.InheritsFrom("TH1") or obj.InheritsFrom("TProfile"):
            hists.append(obj)


def check(
    path: Path,
    overflow_frac: float,
    known_empty_patterns: list[re.Pattern],
    known_overflow_patterns: list[re.Pattern],
    strict: bool,
) -> int:
    """Run the QA check on @p path.  Returns process exit code."""
    ROOT = _import_root()

    if not path.is_file():
        print(f"ERROR: file not found: {path}", file=sys.stderr)
        return 3

    f = ROOT.TFile.Open(str(path), "READ")
    if not f or f.IsZombie():
        print(f"ERROR: cannot open ROOT file: {path}", file=sys.stderr)
        return 3

    hists = []
    _walk(f, hists)

    findings: list[Finding] = []
    for h in hists:
        finding = _classify(h, overflow_frac)
        # Annotate "known-empty" matches but keep the status as EMPTY so the
        # tabular report still shows them — strict mode is the gate.
        if finding.status == "EMPTY":
            for pat in known_empty_patterns:
                if pat.search(finding.path.lstrip("/")):
                    finding.note = f"matched --known-empty pattern '{pat.pattern}'"
                    finding.status = "EMPTY*"
                    break
        # Same treatment for known-overflow.
        if finding.status == "OVERFLOW":
            for pat in known_overflow_patterns:
                if pat.search(finding.path.lstrip("/")):
                    finding.note = f"matched --known-overflow pattern '{pat.pattern}'"
                    finding.status = "OVERFLOW*"
                    break
        findings.append(finding)

    # ── Report ─────────────────────────────────────────────────────────────
    headers = ("status", "path", "entries", "in-range", "overflow")
    rows = [(f.status, f.path, f"{f.entries:.4g}", f"{f.in_range:.4g}",
             f"{f.overflow_count:.4g}", f.note) for f in findings]
    headers = ("status", "path", "entries", "in-range", "overflow", "note")
    widths = [max(len(headers[i]), max((len(r[i]) for r in rows), default=0))
              for i in range(len(headers))]
    line = " | ".join(h.ljust(w) for h, w in zip(headers, widths))
    print(line)
    print("-+-".join("-" * w for w in widths))
    for r in rows:
        print(" | ".join(c.ljust(w) for c, w in zip(r, widths)))

    n_overflow_unknown = sum(1 for f in findings if f.status == "OVERFLOW")
    n_overflow_known   = sum(1 for f in findings if f.status == "OVERFLOW*")
    n_empty_unknown    = sum(1 for f in findings if f.status == "EMPTY")
    n_empty_known      = sum(1 for f in findings if f.status == "EMPTY*")
    n_ok               = sum(1 for f in findings if f.status == "OK")

    print()
    print(f"Summary: OK={n_ok}  "
          f"EMPTY={n_empty_unknown}  EMPTY*(expected)={n_empty_known}  "
          f"OVERFLOW={n_overflow_unknown}  OVERFLOW*(expected)={n_overflow_known}  "
          f"total={len(findings)}")

    if n_overflow_unknown > 0:
        return 1
    if strict and n_empty_unknown > 0:
        return 2
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="QA content check for beam-test-analysis ROOT output files."
    )
    parser.add_argument("path", type=Path,
                        help="ROOT file to inspect (e.g. lightdata.root).")
    parser.add_argument(
        "--overflow-frac", type=float, default=0.05,
        help="Flag a histogram OVERFLOW when (under+overflow)/entries exceeds "
             "this fraction.  Default: 0.05 (5%%).")
    parser.add_argument(
        "--known-empty", action="append", default=[],
        help="Regex on histogram path (e.g. 'Streaming Trigger/.*') that is "
             "allowed to be EMPTY without escalating to a failure.  Repeatable.")
    parser.add_argument(
        "--known-overflow", action="append", default=[],
        help="Regex on histogram path that is allowed to be OVERFLOW without "
             "failing — use for naturally long-tailed distributions (e.g. "
             "same-channel Δt with a DCR tail).  Repeatable.")
    parser.add_argument(
        "--strict", action="store_true",
        help="Exit nonzero if any histogram is EMPTY (not matching a known-empty "
             "pattern).  Default: warn-only on EMPTY, fail only on OVERFLOW.")
    args = parser.parse_args(argv)

    empty_pats    = [re.compile(p) for p in args.known_empty]
    overflow_pats = [re.compile(p) for p in args.known_overflow]
    return check(args.path, args.overflow_frac, empty_pats, overflow_pats, args.strict)


if __name__ == "__main__":
    sys.exit(main())
