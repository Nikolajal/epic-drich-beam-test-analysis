"""Cross-section integrity checks for ``conf/readout_config.toml``.

The readout config splits ALCOR devices across three physical roles:

  - ``[readout.cherenkov]`` — devices reading the Cherenkov radiator PDUs.
  - ``[readout.timing]``    — timing reference (MCP-PMT / fast scintillator).
  - ``[readout.tracking]``  — charged-particle tracking devices.

Each role lists ``devices = [{id = N, chips = …}, …]`` where ``chips``
is either ``"*"`` (all 8 chips on the device) or an explicit list of
chip indices ``[0, 2, …]``.  A given physical chip can only sit in one
role at a time — the framer routes hits by (device, chip) lookup, so
a chip assigned to two roles produces ambiguous output.

This module's :func:`find_overlaps` walks the three lists and surfaces
every (device, chip) pair that's assigned to more than one role, as
short human-readable strings the Settings tab renders as a warning
banner.  Pure stdlib + tomllib (read-only) — no Qt, no side effects.
"""

from __future__ import annotations

from typing import Iterable

#  ALCOR devices ship with 8 channels of 8 chips each.  The wildcard
#  ``chips = "*"`` is sugar for "all 8 chips on this device".  Kept
#  here as a module constant so a hardware refresh only touches one
#  line — the writers use the same expansion via :func:`_expand_chips`.
ALCOR_CHIPS_PER_DEVICE = 8

#  The three known roles.  Order matches the file layout so overlap
#  messages read naturally (Cherenkov first, then Timing, then Tracking).
_ROLES: tuple[str, ...] = ("cherenkov", "timing", "tracking")


def _expand_chips(chips) -> set[int]:
    """``"*"`` → all 8; list → set of ints; anything else → ``set()``.

    Defensive against hand-edited TOML — unparseable values yield an
    empty set rather than raising, so a malformed entry doesn't kill
    validation of the rest of the file.
    """
    if isinstance(chips, str):
        if chips.strip() == "*":
            return set(range(ALCOR_CHIPS_PER_DEVICE))
        #  Stray quoted list like ``"[0, 2]"`` — treat as malformed.
        return set()
    if isinstance(chips, (list, tuple)):
        out: set[int] = set()
        for c in chips:
            try:
                out.add(int(c))
            except (TypeError, ValueError):
                pass
        return out
    return set()


def _walk_devices(role_block) -> Iterable[tuple[int, set[int]]]:
    """Yield ``(device_id, chip_set)`` tuples from a role's ``devices`` list.

    Skips entries that don't carry an integer ``id`` — same defensive
    posture as :func:`_expand_chips`.
    """
    if not isinstance(role_block, dict):
        return
    devices = role_block.get("devices", [])
    if not isinstance(devices, (list, tuple)):
        return
    for entry in devices:
        if not isinstance(entry, dict):
            continue
        try:
            dev_id = int(entry.get("id"))
        except (TypeError, ValueError):
            continue
        yield dev_id, _expand_chips(entry.get("chips", []))


def find_overlaps(doc) -> list[str]:
    """Return human-readable overlap descriptions; empty list ⇒ clean.

    Each line names one (device, chip) pair and the roles claiming it.
    Adjacent chips on the same (device, roles) pair are folded into a
    range so a wildcard collision reads ``device 200 chips 0-7`` rather
    than eight separate lines.

    Format: ``device <D> chip(s) <SPEC>: <Role A> ↔ <Role B> …``
    """
    if not isinstance(doc, dict):
        return []
    readout = doc.get("readout")
    if not isinstance(readout, dict):
        return []

    #  Build (device, chip) → set of role names.
    by_chip: dict[tuple[int, int], set[str]] = {}
    for role in _ROLES:
        for dev_id, chip_set in _walk_devices(readout.get(role)):
            for chip in chip_set:
                by_chip.setdefault((dev_id, chip), set()).add(role)

    #  Filter to actual overlaps + group by (device, roles-tuple) so
    #  consecutive chip indices collapse into a range.
    grouped: dict[tuple[int, tuple[str, ...]], list[int]] = {}
    for (dev_id, chip), roles in by_chip.items():
        if len(roles) < 2:
            continue
        key = (dev_id, tuple(sorted(roles)))
        grouped.setdefault(key, []).append(chip)

    out: list[str] = []
    for (dev_id, roles), chips in sorted(grouped.items()):
        chips_sorted = sorted(chips)
        spec = _format_chip_spec(chips_sorted)
        roles_pretty = " ↔ ".join(r.capitalize() for r in roles)
        suffix = "chip" if len(chips_sorted) == 1 else "chips"
        out.append(f"device {dev_id} {suffix} {spec}: {roles_pretty}")
    return out


def _format_chip_spec(chips: list[int]) -> str:
    """Fold runs of consecutive ints into ``a-b`` ranges.

    ``[0,1,2,3,4,5,6,7]`` → ``"0-7"``;
    ``[0, 2, 4]``         → ``"0, 2, 4"``;
    ``[0, 1, 4, 5]``      → ``"0-1, 4-5"``.
    """
    if not chips:
        return "(none)"
    runs: list[tuple[int, int]] = []
    start = prev = chips[0]
    for c in chips[1:]:
        if c == prev + 1:
            prev = c
            continue
        runs.append((start, prev))
        start = prev = c
    runs.append((start, prev))
    parts = [
        f"{a}-{b}" if a != b else f"{a}"
        for a, b in runs
    ]
    return ", ".join(parts)


__all__ = ["find_overlaps", "ALCOR_CHIPS_PER_DEVICE"]
