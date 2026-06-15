"""Writer catalog — what the Run Manager can launch.

One ``WriterSpec`` per writer binary the dashboard knows about; each
spec carries the CLI flag surface so the Run Manager can render the
inputs without hand-coding a separate form per writer.

Flags are split by ``kind``: ``"bool"`` flags become checkboxes on
the right side of the writer card (the dashboard convention); other
kinds (int, float, string) become inputs on the left.  This is the
only knowledge the UI needs to lay out the form.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass(frozen=True)
class FlagSpec:
    """One CLI flag the Run Manager renders as a form control."""

    name: str               # bare flag name (no leading '--')
    label: str              # form label
    kind: str               # 'bool' | 'int' | 'float' | 'string'
    default: object = None  # default value for the control
    help: str = ""          # tooltip


@dataclass(frozen=True)
class WriterSpec:
    """One launchable writer + its flag surface."""

    name: str                                       # short tag, e.g. "lightdata"
    executable: str                                 # relative path under repo
    description: str
    flags: list[FlagSpec] = field(default_factory=list)

    def exists(self, repo_root: Path) -> bool:
        return (repo_root / self.executable).is_file()

    def cmd_prefix(self, repo_root: Path) -> list[str]:
        """argv[:1] — the absolute path to the writer binary."""
        return [str((repo_root / self.executable).resolve())]

    def bool_flags(self) -> list[FlagSpec]:
        return [f for f in self.flags if f.kind == "bool"]

    def input_flags(self) -> list[FlagSpec]:
        return [f for f in self.flags if f.kind != "bool"]


# ---------------------------------------------------------------------------
# Flag surface — varies subtly by writer.
#
# Lightdata is the first stage in the chain, so ``--force-upstream``
# doesn't apply (nothing upstream of it to force).  Recodata accepts
# the full set.
# ---------------------------------------------------------------------------


_QA_FLAG = FlagSpec(
    name="QA",
    label="QA mode",
    kind="bool",
    default=False,
    help="Route configs through conf/QA/ fallbacks.",
)

_FORCE_REBUILD_FLAG = FlagSpec(
    name="force-rebuild",
    label="Force rebuild",
    kind="bool",
    default=False,
    help="Re-run even when an up-to-date output already exists.",
)

_FORCE_UPSTREAM_FLAG = FlagSpec(
    name="force-upstream",
    label="Force upstream",
    kind="bool",
    default=False,
    help="Also rebuild the upstream stage when it's stale or missing.",
)

_MAX_SPILL_FLAG = FlagSpec(
    name="max-spill",
    label="Max spill",
    kind="int",
    default=None,
    help="Cap on spills processed; leave blank for the writer's default.",
)

_THREADS_FLAG = FlagSpec(
    name="threads",
    label="Threads",
    kind="int",
    default=None,
    help="Worker-thread count; leave blank for the writer's auto-detect.",
)

#  ── Calibration anchor overrides ────────────────────────────────────
#
#  Anchor channel (device / chip / eo_channel) is the pulser reference
#  the per-channel Δc is computed against — a key knob for the
#  fine-time calibration.  Persistent default lives in
#  ``conf/calib/calibration_conf.toml`` and the Settings tab covers
#  the durable case; these per-launch overrides on the Run Manager
#  card are the "switch to 193/0/ch0 because 192's hardware just
#  died" escape hatch.  Blank input → no ``--anchor-*`` flag passed
#  → binary uses the TOML value.

_ANCHOR_DEVICE_FLAG = FlagSpec(
    name="anchor-device",
    label="Anchor device",
    kind="int",
    default=None,
    help="Per-launch override of calibration_conf.toml's anchor_device. "
         "Blank → use the TOML value (typically 192).",
)

_ANCHOR_CHIP_FLAG = FlagSpec(
    name="anchor-chip",
    label="Anchor chip",
    kind="int",
    default=None,
    help="Per-launch override of calibration_conf.toml's anchor_chip. "
         "Blank → use the TOML value (typically 0).",
)

_ANCHOR_EO_CHANNEL_FLAG = FlagSpec(
    name="anchor-eo-channel",
    label="Anchor EO channel",
    kind="int",
    default=None,
    help="Per-launch override of calibration_conf.toml's anchor_eo_channel. "
         "Blank → use the TOML value (typically 0).",
)

#  FIFO-salvage anchor.  The pulsed reference (e.g. the KC705 testpulse)
#  is read out on a dedicated FIFO with tdc/fine/pixel/column all -1, so
#  it has no valid channel ordinal — the chip/eo-channel anchor can't
#  address it.  Setting anchor-fifo ≥ 0 salvages it by (device, fifo)
#  instead.  (device, anchor-fifo) is the salvage key; (chip, eo-channel)
#  is the legacy channel key.
_ANCHOR_FIFO_FLAG = FlagSpec(
    name="anchor-fifo",
    label="Anchor FIFO",
    kind="int",
    default=None,
    help="Per-launch override of calibration_conf.toml's anchor_fifo. "
         "≥ 0 salvages the pulsed reference by (anchor_device, anchor_fifo) "
         "— e.g. the KC705 testpulse on device 200 / FIFO 32 — instead of "
         "the legacy chip/eo-channel anchor. Blank → use the TOML value "
         "(dRICH 2026: 32).",
)

#  Pulser frequency override.  The TOML field is ``pulser_period_cc``
#  (a clock-cycle count), but the operator dials Hz on the generator,
#  so the form takes Hz and the CLI driver converts to cc via the
#  320 MHz ALCOR clock (``cc = 320e6 / Hz``).
_PULSER_FREQUENCY_FLAG = FlagSpec(
    name="pulser-frequency-hz",
    label="Pulser freq (Hz)",
    kind="float",
    default=None,
    help="Per-launch override of calibration_conf.toml's pulser_period_cc "
         "expressed as the generator frequency in Hz (e.g. 1000 for 1 kHz). "
         "Converted internally using the 320 MHz clock. Blank → use the TOML "
         "value (typically 1000 Hz = 320000 cc).",
)


WRITERS: list[WriterSpec] = [
    WriterSpec(
        name="calibration",
        executable="build/bin/pulser_calib_writer",
        description=(
            "Pulser-driven fine-time calibration.  Reads raw FIFOs; "
            "writes fine_calib.{txt|toml} + pulser_calib_qa.root."
        ),
        # Calibration is upstream of everything else — no upstream to force.
        # No --QA flag either: pulser_calib_writer's CLI doesn't expose
        # one (conf path comes via --calib-conf instead), so toggling
        # the checkbox would produce an "unexpected argument" error.
        #
        # Anchor + pulser overrides sit on the card so an operator can
        # flip the pulser reference channel / generator frequency
        # per-launch without rewriting the shared calibration_conf.toml.
        # Blank → fall through to TOML.  Order matters for the
        # 2-column form layout — pulser frequency pairs with Max spill
        # on the first row (general knobs); the four anchor fields
        # group on the next rows, paired by addressing mode:
        # (device, FIFO) is the FIFO-salvage key, (chip, EO channel) the
        # legacy channel key.
        flags=[
            _FORCE_REBUILD_FLAG,
            _MAX_SPILL_FLAG, _PULSER_FREQUENCY_FLAG,
            _ANCHOR_DEVICE_FLAG, _ANCHOR_FIFO_FLAG,
            _ANCHOR_CHIP_FLAG, _ANCHOR_EO_CHANNEL_FLAG,
        ],
    ),
    WriterSpec(
        name="lightdata",
        executable="build/bin/lightdata_writer",
        description=(
            "Framer + streaming trigger + RANSAC.  Reads raw FIFOs + "
            "fine_calib.{txt|toml}; writes lightdata.root."
        ),
        # ``lightdata_writer`` does not accept ``--force-upstream`` on
        # the CLI — even though it has an upstream (calibration), the
        # flag isn't wired through.  Keep it off the form to avoid
        # the "unexpected argument" error the user hit.
        flags=[_QA_FLAG, _FORCE_REBUILD_FLAG, _MAX_SPILL_FLAG, _THREADS_FLAG],
    ),
    WriterSpec(
        name="recodata",
        executable="build/bin/recodata_writer",
        description=(
            "Ring refinement, radial fits, σ(N) panels.  Reads "
            "lightdata.root; writes recodata.root."
        ),
        flags=[
            _QA_FLAG, _FORCE_REBUILD_FLAG, _FORCE_UPSTREAM_FLAG,
            _MAX_SPILL_FLAG, _THREADS_FLAG,
        ],
    ),
    WriterSpec(
        name="recotrack",
        executable="build/bin/recotrackdata_writer",
        description=(
            "Track-matched data: telescope tracks merged with reconstructed "
            "hits.  Reads recodata.root + ALTAI tracks; writes "
            "recotrackdata.root."
        ),
        flags=[
            _QA_FLAG, _FORCE_REBUILD_FLAG, _FORCE_UPSTREAM_FLAG,
            _MAX_SPILL_FLAG, _THREADS_FLAG,
        ],
    ),
]


def find_writer(name: str) -> WriterSpec | None:
    for spec in WRITERS:
        if spec.name == name:
            return spec
    return None


__all__ = ["FlagSpec", "WRITERS", "WriterSpec", "find_writer"]
