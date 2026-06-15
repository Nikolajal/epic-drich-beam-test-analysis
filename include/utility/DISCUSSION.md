# include/utility/ — design notes

> 🧭 **Hub:** project-wide design log + index of satellites lives at
> [`../../DISCUSSION.md`](../../DISCUSSION.md).

Per-area discussion log for the helper headers in this directory.
Use this file for design hot points and open questions that don't
belong in the implementation comments themselves.  Keep the
implementation comments local and factual; keep "we don't know yet"
here.

## Active design notes

### `config_dump.h` — knob-dump convention

Every writer carries a `util::ConfigDump` block at the end of its
output ROOT file under a `Config/` `TDirectory` so the file is
self-describing.  The contract:

- One `TNamed` per knob, with the value as the string body.
- One `TNamed` per `conf/<file>.toml` consumed, body = the verbatim
  TOML contents at build time.
- Read-back lives in `qa_quicklook/datainspect.py`.

Adding a new writer?  Mirror the existing four — `util::ConfigDump
dump(out_file); dump.add("max_spill", max_spill).add_path(...)`.  The
RAII guard restores `gDirectory` on dtor.

### `audit.h` — provenance log

Header-only `util::audit::log()` appends `[[entry]]` blocks to a
sibling `<basename>.audit.toml`.  Used by `AnalysisResults::update`
when a `source` parameter is passed.  In-process mutex guards
concurrent writes; cross-process atomicity relies on POSIX `O_APPEND`
for blocks under `PIPE_BUF` (~4096 B), which our ~150 B entries fit
inside comfortably.

### `qa_publish.h` — PDF path convention

Helper for the operator-facing `<run>/qa/<step>/<order>_<name>.pdf`
layout the QA dashboard scans.  See top-level `DISCUSSION.md`
"Writers → QA: what to publish, and how" for the full convention.

### `global_index.h` — packed identifier

32-bit packed `(device, fifo, chip_logical, channel_logical, tdc,
validity_bit)` plus accessors that decode each component.  Stored as
the calibration map key (`AlcorFinedata::calibration_table_`)
and emitted into `fine_calib.toml` as the `key` field.  The legacy
`calib_index()` accessor (without `device`) is kept deprecated for
old-file detection only — see `[[deprecated]]` attribute on the
declaration.

**Raw value vs dense ordinal — do not conflate.**  `raw()` is the *identity*:
the full 32-bit packed value (validity bit 31 set), so for real channels it
lands in the millions and is **sparse**.  Use it only as a map key, the
`fine_calib.toml` `key` field, or an equality token.  Anything that needs a
*dense, contiguous* index — a histogram axis, a flat array, a per-channel or
per-TDC bin — must go through the ordinal helpers (`tdc_ordinal()` at TDC level;
`global_channel_raw()` for the TDC-zeroed channel-level key).  Treating `raw()`
as a dense index silently overflows: the `h2_fine_tune` QA axis was sized for the
*legacy compact* "global tdc index" (0..~10⁴), and under the packed-32-bit
storage it must call `tdc_ordinal()` to recover that bin (see
`parallel_streaming_framer.cxx`, the `h2_fine_tune->Fill` site).  **Standing
rule: raw for identity, ordinal for indexing — never feed the raw value to an
array or histogram axis.**

(ToT edge pairing keys on `global_channel_raw()` — TDC bits zeroed — so the
leading (even TDC) and trailing (odd TDC) edges of one pulse share a key; that
is why pairing groups by channel, not by the TDC-level `raw()`.)

#### Planned: reserved bits `[29,30]` → 2-bit `readout_mode` enum (deferred)

The two reserved bits are currently strict-zero and notionally a
"schema-version" field.  Decision: **repurpose them as a 2-bit ALCOR
`readout_mode` enum** instead.  Rationale:

- A 2-bit version field is a poor version field (only 4 values, no room
  to grow); real layout versioning, if ever needed, belongs at the
  file / TTree-header level where there is room.
- ALCOR operates a channel in one of four readout modes —
  **`LET` (leading-edge time), `ToT` (time-over-threshold), `ToT2`
  (second ToT), `SR` (slew rate)** — which is *exactly* a 2-bit enum and
  determines how the `coarse`/`fine` payload must be interpreted.  Mode is
  per-channel/per-device constant within a run (e.g. timing chip always
  `LET`, data chips `ToT`), so it is categorical address-level metadata,
  not a per-hit measurement — packing it does **not** break key semantics
  (a channel's hits all carry the same mode bits; equality / per-channel
  maps / `global_channel` stay consistent).  It makes each word
  **self-describing** about its payload meaning without a side lookup.

Encoding: **`LET = 0`** (so every existing persisted index / `fine_calib.toml`
`key` — which has these bits zero — reads as `LET` by definition); `ToT`,
`ToT2`, `SR` take `1..3` (final 1–3 ordering to match the ALCOR mode
register so the decoder can copy it directly).

Touch-list when implemented: `kReservedBits`→`kModeBits` + `kModeShift`/
`kModeMask` + `mode()` accessor + a `ReadoutMode` enum; constructors gain a
mode arg; drop the `reserved_bits_are_zero()` invariant + its test, add
mode round-trip tests; `global_channel` **keeps** the mode bits (only TDC
zeroed); the decoder sets mode from the readout config.  This is a
**Schema / MAJOR** change (the packed layout is persisted as the
calibration key).  Captured; implementation deferred until the firmware
mode-tagging is wired up.

### `AlcorFinedata::get_phase()` — hot-path representation

**Shipped 2026-06-02 as a code-clarity cleanup (lever a) — NOT a perf win.**
The two parallel maps (`calibration_parameters` + `channel_calibration_method`)
are now **fused into one** `calibration_table_`
(`unordered_map<uint32_t, CalibrationEntry>`, where
`CalibrationEntry = {array<float,3> params, CalibrationMethod method}`), so
`get_phase()` does a single `find()` instead of two.  Behaviour-preserving;
on-disk TOML schema unchanged.

**⚠️ The "~10–19 s" perf premise was FALSE — measured, don't re-chase.**  The
BACKLOG-P-1.71 estimate (two `find()`s × ~95 ns × ~100 M hits ≈ 19 s) was a
**code-inspection hypothesis**, never a direct `get_phase` measurement.  A
controlled before/after A/B (5-spill `--QA` on 20251119-010426, same continuous
machine load, 2026-06-02) showed **no wall-clock difference**:

| | runs (s) | mean |
| --- | --- | --- |
| PRE (two maps) | 115.7, 111.7 | ~113.7 |
| POST (fused)   | 118.8, 115.0, 120.0 | ~117.9 |

Ranges overlap; if anything POST is marginally slower (noise).  So the per-hit
calibration lookup is **not** a wall-clock bottleneck on the `--QA` path — either
the lookup was never the cost, or this path runs `calibration_method=none` so
both `find()`s short-circuit on an empty table.  Either way:

- **Levers (b)** (vector — and note `raw()` is sparse with bit-31 set, so (b)
  would need re-keying to `tdc_ordinal()`, not "indexed by `raw()`" as the old
  plan said) **and (c)** (bake into the struct) are **not worth pursuing** — if
  fusing two lookups into one moved nothing, neither will.
- To find the *real* hot points, **profile/instrument** (CPU/mem/IO) rather than
  reason from code; the calibration lookup is off the suspect list.

Kept (a) because the fused record is simpler than two parallel maps and cannot
be slower — but it carries **no** performance claim.

## Open / deferred items

These live in the top-level `BACKLOG.md` (work-in-progress file at
the repo root, not on the Doxygen site yet — read it locally) under
the C++ domain.  Quick pointers:

- `fit_circle` design review — initial-value validation, `fix_XY`
  granularity, named-struct return with χ²/ndf/status, round-trip
  test against synthetic rings.  Audit also touches the consumer
  side in `recodata_writer`.
- `mapping` lookup-table state — moved to top-level **D-10** since
  it's not strictly a utility-only concern (`RunInfo` shares the
  same static-state pattern).

## Conventions

- **Header-only** unless you're carrying a `static` table.  If you
  cross that line, factor a `.cxx` next to it under `src/utility/`.
- **No project-side `#include`s** except other headers in this
  directory or from `<mist/...>`.  Anything heavier means the helper
  has grown out of "utility".
- **Doxygen `@file` + `@brief`** at the top of every header — the
  utility tree is on the Doxygen site (`docs/Doxyfile` has
  `INPUT = ../include ...`).
- New file?  Add to [`../utility.h`](../utility.h) umbrella so
  consumers can keep the single-include pattern.
