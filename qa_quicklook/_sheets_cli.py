"""``python -m qa_quicklook.sheets_sync`` entry point.

This file is loaded by ``qa_quicklook/sheets_sync.py``'s tail when
the module is executed as ``__main__`` — see the ``if __name__ ==
"__main__":`` block there.  Kept as a sibling file so the module
proper stays free of argparse + logging noise.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from . import sheets_sync


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m qa_quicklook.sheets_sync",
        description=(
            "One-shot Google Sheets push for the dashboard's run "
            "database, runlists, joblock state, and audit tail.  Reads "
            "[sheets_sync] from qa_quicklook/qa_quicklook.toml unless "
            "--config-toml is set."
        ),
    )
    p.add_argument(
        "--repo", type=Path,
        default=Path(__file__).resolve().parents[1],
        help="Repository root (defaults to the qa_quicklook parent).",
    )
    p.add_argument(
        "--config-toml", type=Path, default=None,
        help=(
            "Path to qa_quicklook.toml.  Defaults to "
            "<repo>/qa_quicklook/qa_quicklook.toml."
        ),
    )
    p.add_argument(
        "--year", type=str, default=None,
        help="Force a specific campaign year (e.g. 2026).  Default: auto-detect.",
    )
    p.add_argument(
        "--audit-tail", type=int, default=30,
        help=(
            "Number of most-recent audit entries to mirror to the Sheet.  "
            "Default: 30 (Phase B default — the local audit file always "
            "keeps the full history)."
        ),
    )
    p.add_argument(
        "--dry-run", action="store_true",
        help=(
            "Render worksheets but don't touch Google.  Emits the cell "
            "payload as JSON on stdout.  Use to verify the snapshot "
            "shape without exposing credentials."
        ),
    )
    p.add_argument(
        "--print-render", action="store_true",
        help=(
            "Pretty-print the rendered worksheets to stderr in addition "
            "to (or instead of) pushing.  Useful when paired with "
            "--dry-run."
        ),
    )
    p.add_argument(
        "--to-xlsx", type=Path, default=None, metavar="PATH",
        help=(
            "Write a formatted .xlsx preview of the rendered worksheets "
            "to PATH and exit.  Implies --dry-run (no Google call).  "
            "Opens in Excel / Numbers / LibreOffice / Google Sheets via "
            "File → Import.  Requires `pip install openpyxl`."
        ),
    )
    p.add_argument(
        "--no-format", action="store_true",
        help=(
            "Skip the formatting batchUpdate on the live push.  "
            "Conditional formatting rules + bandings survive between "
            "pushes, so a values-only push is correct most of the "
            "time and saves one round trip.  Without this flag, the "
            "look is re-applied on every push (idempotent)."
        ),
    )
    p.add_argument(
        "--max-reverse-edits", type=int,
        default=sheets_sync.MAX_REVERSE_EDITS_PER_TICK, metavar="N",
        help=(
            f"Safety brake: refuse to apply more than N Sheet-side edits "
            f"in one tick.  Default {sheets_sync.MAX_REVERSE_EDITS_PER_TICK}.  "
            "When tripped, the push aborts before touching the local "
            "database — investigate before raising the threshold."
        ),
    )
    p.add_argument(
        "--force-reverse-merge", action="store_true",
        help=(
            "Bypass the reverse-merge safety brake.  Use only when you "
            "are sure that the Sheet has genuinely been edited in bulk "
            "AND you intend those edits to land in the local database."
        ),
    )
    return p


def _resolve_config_path(args) -> Path:
    if args.config_toml is not None:
        return args.config_toml
    return args.repo / "qa_quicklook" / "qa_quicklook.toml"


def _print_render(rendered: dict, stream) -> None:
    """Render an at-a-glance summary on a non-stdout stream."""
    for ws, rows in rendered.items():
        cols = len(rows[0]) if rows else 0
        print(f"  {ws:<10s} rows={len(rows):<5d} cols={cols}", file=stream)


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    cfg_path = _resolve_config_path(args)
    cfg = sheets_sync.load_config(cfg_path)

    #  --to-xlsx implies --dry-run.  The xlsx write doesn't touch
    #  Google at all; it's the local-preview path the README walks
    #  operators through before flipping enabled = true.
    if args.to_xlsx is not None or args.dry_run:
        result = sheets_sync.sync_once(
            cfg, args.repo,
            dry_run=True,
            year=args.year,
            audit_tail_n=args.audit_tail,
        )
        if args.to_xlsx is not None:
            from . import _sheets_format
            try:
                written = _sheets_format.to_xlsx(
                    result.rendered, args.to_xlsx,
                )
            except _sheets_format._OpenpyxlMissing as exc:
                print(f"[sheets_sync] {exc}", file=sys.stderr)
                return 2
            n_cells = sum(len(rows) * (len(rows[0]) if rows else 0)
                          for rows in result.rendered.values())
            print(
                f"[sheets_sync] wrote {written} — "
                f"{n_cells} cells across {len(result.rendered)} worksheets "
                f"(year={result.year})"
            )
            if args.print_render:
                _print_render(result.rendered, sys.stderr)
            return 0
        print(json.dumps(
            {
                "dry_run": True,
                "year": result.year,
                "worksheets": result.rendered,
            },
            default=str,
            ensure_ascii=False,
            indent=2,
        ))
        if args.print_render:
            print("(dry-run summary)", file=sys.stderr)
            _print_render(result.rendered, sys.stderr)
        return 0

    #  Real push — surface unconfigured + missing-dep failures as one-
    #  line stderr + a non-zero exit so a cron / launchd wrapper can
    #  alert without parsing logs.
    if not cfg.is_configured:
        print(
            f"[sheets_sync] disabled: {cfg.disabled_reason()}",
            file=sys.stderr,
        )
        return 2

    try:
        result = sheets_sync.sync_once(
            cfg, args.repo,
            dry_run=False,
            year=args.year,
            audit_tail_n=args.audit_tail,
            apply_formatting=not args.no_format,
            force_reverse_merge=args.force_reverse_merge,
            max_reverse_edits=args.max_reverse_edits,
        )
    except sheets_sync.ReverseMergeBrake as exc:
        #  Distinct exit code so cron / launchd wrappers can pager
        #  the operator without having to grep for the phrase.
        print(f"[sheets_sync] {exc}", file=sys.stderr)
        return 3
    except Exception as exc:  # noqa: BLE001
        print(f"[sheets_sync] push failed: {exc}", file=sys.stderr)
        return 1

    if result.hard_reset:
        print(
            "[sheets_sync] ⚠  HARD RESET — Sheet integrity check failed.",
            file=sys.stderr,
        )
        for issue in result.integrity_issues:
            print(f"  · {issue}", file=sys.stderr)
        print(
            "  Reverse-merge skipped this tick; the canonical local "
            "state was rewritten over whatever was on the Sheet.",
            file=sys.stderr,
        )

    print(
        f"[sheets_sync] pushed year={result.year} at {result.last_push_at} "
        f"— rows changed: {result.rows_changed}, "
        f"worksheets skipped (unchanged): {result.worksheets_skipped}, "
        f"reverse edits applied: {result.edits_applied}, "
        f"skipped-in-sync: {result.edits_skipped_in_sync}"
        + (" [HARD RESET]" if result.hard_reset else ""),
    )
    if args.print_render:
        _print_render(result.rendered, sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
