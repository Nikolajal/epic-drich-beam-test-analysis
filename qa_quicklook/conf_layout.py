"""Setting-set layout helpers for ``conf/``.

The repo's ``conf/`` directory uses a layered profile model:

    conf/
      <name>.toml             ← MASTER symlink — what the C++ writers read
      defaults/<name>.toml    ← pristine baseline, committed, never overwritten
      sets/<set>/<name>.toml  ← named bundles (e.g. "2024", "2025"); only files
                                that differ from default need to live here
      working/<name>.toml     ← per-operator scratch (gitignored); a master
                                points here while there are unsaved edits

This module is the pure-Python brain that walks that layout.  No Qt,
no widgets — the Settings tab calls these helpers; everything UI-side
stays in :mod:`qa_quicklook.settings`.

What the helpers cover
----------------------

  - :func:`scan` — read the symlink targets for every master and
    classify each as ``"default"`` / ``"sets/<name>"`` / ``"working"``
    / ``"missing"``.
  - :func:`active_set_name` — collapse the per-file classification
    into a single label (``"default"`` / ``"<set>"`` / ``"working"``
    / ``"mixed"``).
  - :func:`list_sets` — what's under ``conf/sets/``.
  - :func:`promote_to_working` — copy the master's current target into
    ``conf/working/`` and atomically repoint the master at it; idempotent.
  - :func:`switch_set` — repoint every master at a chosen set (or back
    to defaults).  Working overrides for files the new set covers are
    discarded; for files the set doesn't cover, the master falls back
    to defaults.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Optional


DEFAULTS_DIR = "defaults"
SETS_DIR = "sets"
WORKING_DIR = "working"


class MasterKind(str, Enum):
    """What kind of target a master file currently resolves to."""

    DEFAULT = "default"
    SET = "set"
    WORKING = "working"
    MISSING = "missing"             # symlink target doesn't exist
    NOT_A_SYMLINK = "not_a_symlink"  # master is a real file (legacy)


@dataclass(frozen=True)
class MasterEntry:
    """One master file's current resolution."""

    name: str                       # base name, e.g. "mapping_conf.toml"
    master_path: Path
    target_relative: Optional[str]  # symlink target text; ``None`` if not a symlink
    target_absolute: Optional[Path]
    kind: MasterKind
    set_name: Optional[str]         # populated when kind == SET; else None


def scan(conf_dir: Path) -> list[MasterEntry]:
    """Walk ``conf_dir`` and return one entry per master ``*.toml``.

    A "master" is any ``conf/*.toml`` that is *not* in ``defaults/``,
    ``sets/``, ``working/``, or another sub-directory like ``QA/``.
    Both real files (legacy) and symlinks are reported.
    """
    if not conf_dir.is_dir():
        return []
    # Resolve the conf dir up-front: on macOS /var → /private/var via a
    # symlink, so the resolved target paths inside this dir won't be
    # ``.relative_to(conf_dir)`` unless we resolve the base as well.
    conf_dir_resolved = conf_dir.resolve()
    entries: list[MasterEntry] = []
    for p in sorted(conf_dir.glob("*.toml")):
        entries.append(_classify_master(conf_dir_resolved, p))
    return entries


def _classify_master(conf_dir: Path, master: Path) -> MasterEntry:
    name = master.name
    if not master.is_symlink():
        return MasterEntry(
            name=name,
            master_path=master,
            target_relative=None,
            target_absolute=master.resolve() if master.exists() else None,
            kind=MasterKind.NOT_A_SYMLINK,
            set_name=None,
        )
    raw = os.readlink(master)
    resolved = (master.parent / raw).resolve()
    rel_to_conf = _try_relative(resolved, conf_dir)
    if rel_to_conf is None:
        # Target points outside conf/ entirely — treat as missing-ish.
        kind = MasterKind.MISSING if not resolved.exists() else MasterKind.NOT_A_SYMLINK
        return MasterEntry(name, master, raw, resolved if resolved.exists() else None, kind, None)

    parts = rel_to_conf.parts
    if not resolved.exists():
        return MasterEntry(name, master, raw, None, MasterKind.MISSING, None)

    if len(parts) >= 2 and parts[0] == DEFAULTS_DIR:
        return MasterEntry(name, master, raw, resolved, MasterKind.DEFAULT, None)
    if len(parts) >= 3 and parts[0] == SETS_DIR:
        return MasterEntry(name, master, raw, resolved, MasterKind.SET, parts[1])
    if len(parts) >= 2 and parts[0] == WORKING_DIR:
        return MasterEntry(name, master, raw, resolved, MasterKind.WORKING, None)
    return MasterEntry(name, master, raw, resolved, MasterKind.NOT_A_SYMLINK, None)


def _try_relative(path: Path, base: Path) -> Optional[Path]:
    try:
        return path.relative_to(base)
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# Whole-picture inference
# ---------------------------------------------------------------------------


def active_set_name(entries: list[MasterEntry]) -> str:
    """Collapse a list of master entries to a single label.

    Returns one of:

      ``"default"``   — every master resolves into ``defaults/``.
      ``"<set>"``     — every master resolves into either ``defaults/``
                        or into one specific ``sets/<set>/`` directory,
                        with at least one in that set.
      ``"working"``   — at least one master resolves into ``working/``.
                        (Working overrides "win" over set membership in
                        the displayed name because they're the surface
                        the operator is actively editing.)
      ``"mixed"``     — masters resolve into more than one ``sets/<x>``;
                        no single name describes the state.
      ``"empty"``     — no entries.
    """
    if not entries:
        return "empty"
    working = any(e.kind == MasterKind.WORKING for e in entries)
    if working:
        return "working"
    set_names = {e.set_name for e in entries if e.kind == MasterKind.SET}
    if not set_names:
        return "default"
    if len(set_names) == 1:
        return next(iter(set_names))
    return "mixed"


def list_sets(conf_dir: Path) -> list[str]:
    """Names of every directory under ``conf/sets/``, sorted."""
    sets_dir = conf_dir / SETS_DIR
    if not sets_dir.is_dir():
        return []
    return sorted(p.name for p in sets_dir.iterdir() if p.is_dir())


# ---------------------------------------------------------------------------
# Mutations — promote / switch
# ---------------------------------------------------------------------------


def promote_to_working(master: Path) -> Path:
    """Return the path the Settings tab should actually write to.

    Routing per current symlink target:

      - **defaults/** → promote to ``working/<name>``, copy the
        default content there, repoint the master.  Keeps the
        pristine default untouched.
      - **sets/<name>/** → **no promotion**.  The set IS a named
        editable target — write directly to it.  This is the
        "in-set direct editing" workflow: when you say "I'm working
        in set 2026", edits go into 2026 immediately, no overlay
        churn.
      - **working/** → already there, no promotion; write through.

    Used by the Settings tab on each save.  Idempotent.
    """
    conf_dir = master.parent
    working_dir = conf_dir / WORKING_DIR

    # Inspect the current target *before* doing anything.
    if not master.is_symlink():
        # Plain file (e.g. the dashboard's qa_quicklook.toml) — write
        # straight through, no symlink dance.
        return master

    current_target_rel = Path(os.readlink(master))
    current_target_abs = (master.parent / current_target_rel).resolve()

    sets_root = (conf_dir / SETS_DIR).resolve()
    defaults_root = (conf_dir / DEFAULTS_DIR).resolve()
    working_root = working_dir.resolve()

    # In-set direct: master is symlinked into sets/<name>/  → write there.
    if _is_inside(current_target_abs, sets_root):
        # Return the canonical (master.parent/relative) form to keep
        # path identity consistent with the promote branch below
        # (macOS resolves /var → /private/var; we want a stable form).
        return (master.parent / current_target_rel).absolute()

    # Already on working/ → write there directly.
    if _is_inside(current_target_abs, working_root):
        return working_dir / master.name

    # Otherwise (defaults/ or anywhere unrecognised) → promote.
    working_dir.mkdir(parents=True, exist_ok=True)
    working_target = working_dir / master.name
    if not master.exists():
        raise FileNotFoundError(f"master not resolvable: {master}")
    content = master.read_text()
    working_target.write_text(content)

    # Atomic symlink swap: write at a sibling tmp path then replace.
    tmp_link = master.with_suffix(master.suffix + ".tmp")
    if tmp_link.exists() or tmp_link.is_symlink():
        tmp_link.unlink()
    rel_target = Path(WORKING_DIR) / master.name
    os.symlink(rel_target, tmp_link)
    os.replace(tmp_link, master)
    return working_target


def _is_inside(child: Path, parent: Path) -> bool:
    try:
        child.relative_to(parent)
    except ValueError:
        return False
    return True


def switch_set(conf_dir: Path, set_name: str) -> list[MasterEntry]:
    """Repoint every master at ``set_name`` (or back to defaults).

    ``set_name`` is either ``"default"`` or the name of a directory
    under ``conf/sets/``.  For each master:

      - if the set covers it (``conf/sets/<set>/<name>.toml`` exists),
        repoint the master there;
      - otherwise fall back to ``defaults/<name>.toml``.

    Any ``conf/working/<name>.toml`` files are *not* deleted but the
    master no longer points at them — they're left for the operator to
    pick back up (or for the dashboard's "Save as setting set" action
    to consume).

    Returns the post-switch ``scan`` so the caller can refresh the UI.
    """
    if set_name not in ("default", *list_sets(conf_dir)):
        raise ValueError(f"unknown set: {set_name!r}")

    for master in sorted(conf_dir.glob("*.toml")):
        name = master.name
        if set_name == "default":
            target_rel = Path(DEFAULTS_DIR) / name
        else:
            set_file = conf_dir / SETS_DIR / set_name / name
            if set_file.exists():
                target_rel = Path(SETS_DIR) / set_name / name
            else:
                # Set is silent on this file → fall back to default.
                target_rel = Path(DEFAULTS_DIR) / name

        _atomic_relink(master, target_rel)

    return scan(conf_dir)


def _atomic_relink(master: Path, target_rel: Path) -> None:
    """Atomically replace ``master`` with a symlink to ``target_rel``."""
    tmp = master.with_suffix(master.suffix + ".tmp")
    if tmp.exists() or tmp.is_symlink():
        tmp.unlink()
    os.symlink(target_rel, tmp)
    os.replace(tmp, master)


__all__ = [
    "DEFAULTS_DIR",
    "SETS_DIR",
    "WORKING_DIR",
    "MasterEntry",
    "MasterKind",
    "active_set_name",
    "list_sets",
    "promote_to_working",
    "scan",
    "switch_set",
]
