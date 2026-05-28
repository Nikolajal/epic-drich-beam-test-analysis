"""Operator-side sanity checks on a run's metadata.

Currently one check is implemented — V_bias should fall inside an
expected band for the run's temperature.  The band table lives in
``qa_quicklook.toml`` under ``[detector.vbias_check].bands`` so the
operator can tune it without touching code; outside any defined band
the check returns ``None`` (no opinion) rather than guessing.

Why a band table instead of a single linear law?  Different sensor
generations, different beam-test campaigns, and "cold" vs "warm"
ramp procedures end up with non-contiguous safe operating windows
that don't sit on one straight line.  A table of (T-range,
V-range) pairs is the simplest thing that fits the actual workflow.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

if sys.version_info >= (3, 11):
    import tomllib as _tomllib
else:  # pragma: no cover
    import tomli as _tomllib  # type: ignore


@dataclass
class VbiasBand:
    """One row of the V_bias-vs-T expected-band table."""

    temp_min: float       # °C, inclusive
    temp_max: float       # °C, exclusive (open on the upper side so
                          # adjacent bands tile without ambiguity)
    vbias_min: float      # V, inclusive
    vbias_max: float      # V, inclusive


@dataclass
class VbiasCheck:
    """Result of evaluating a (V_bias, T) pair against the band table.

    ``level`` is one of:
      - ``"ok"``      → in band.
      - ``"warn"``    → out of band.
      - ``"unknown"`` → no band covers this temperature.

    ``message`` is a one-line human summary suitable for a tooltip.
    """

    level: str            # "ok" / "warn" / "unknown"
    message: str
    band: Optional[VbiasBand] = None


def load_bands(dashboard_config: Path) -> list[VbiasBand]:
    """Read ``[detector.vbias_check].bands`` from ``qa_quicklook.toml``.

    Returns an empty list on missing file / section, so the check
    just degrades to ``"unknown"`` — never raises.
    """
    if not dashboard_config.is_file():
        return []
    try:
        with dashboard_config.open("rb") as fh:
            data = _tomllib.load(fh)
    except (OSError, _tomllib.TOMLDecodeError):
        return []
    section = (
        data.get("detector", {})
            .get("vbias_check", {})
            .get("bands", [])
    )
    out: list[VbiasBand] = []
    if not isinstance(section, list):
        return out
    for row in section:
        if not isinstance(row, dict):
            continue
        try:
            out.append(VbiasBand(
                temp_min=float(row.get("temp_min")),
                temp_max=float(row.get("temp_max")),
                vbias_min=float(row.get("vbias_min")),
                vbias_max=float(row.get("vbias_max")),
            ))
        except (TypeError, ValueError):
            # Malformed row — skip, don't crash the whole check.
            continue
    return out


def check_vbias(
    v_bias: object,
    temperature: object,
    bands: list[VbiasBand],
) -> VbiasCheck:
    """Evaluate one (V_bias, T) pair against ``bands``.

    Both inputs accept anything that coerces to float (the database
    sometimes stores numbers as strings); non-coercible values
    short-circuit to ``"unknown"``.
    """
    v = _to_float(v_bias)
    t = _to_float(temperature)
    if v is None and t is None:
        return VbiasCheck("unknown", "V_bias and temperature not set")
    if v is None:
        return VbiasCheck("unknown", "V_bias not set")
    if t is None:
        return VbiasCheck("unknown", "Temperature not set")
    if not bands:
        return VbiasCheck(
            "unknown",
            "No V_bias × T bands configured in qa_quicklook.toml",
        )

    # Pick the band covering this temperature (lower-inclusive,
    # upper-exclusive so adjacent bands tile cleanly).
    band = next(
        (b for b in bands if b.temp_min <= t < b.temp_max),
        None,
    )
    if band is None:
        return VbiasCheck(
            "unknown",
            f"No band covers T = {t:g} °C — extend "
            f"[detector.vbias_check].bands to opine.",
            band=None,
        )
    if band.vbias_min <= v <= band.vbias_max:
        return VbiasCheck(
            "ok",
            f"V_bias = {v:g} V is inside the expected "
            f"[{band.vbias_min:g}, {band.vbias_max:g}] V band for "
            f"T ∈ [{band.temp_min:g}, {band.temp_max:g}) °C.",
            band=band,
        )
    direction = "below" if v < band.vbias_min else "above"
    return VbiasCheck(
        "warn",
        f"V_bias = {v:g} V is {direction} the expected "
        f"[{band.vbias_min:g}, {band.vbias_max:g}] V band for "
        f"T ∈ [{band.temp_min:g}, {band.temp_max:g}) °C.",
        band=band,
    )


def _to_float(x: object) -> Optional[float]:
    if x is None:
        return None
    if isinstance(x, bool):  # bool is a subclass of int — avoid surprises
        return None
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


__all__ = [
    "VbiasBand",
    "VbiasCheck",
    "check_vbias",
    "load_bands",
]
