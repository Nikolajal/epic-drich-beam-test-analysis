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
the calibration map key (`AlcorFinedata::calibration_parameters`)
and emitted into `fine_calib.toml` as the `key` field.  The legacy
`calib_index()` accessor (without `device`) is kept deprecated for
old-file detection only — see `[[deprecated]]` attribute on the
declaration.

### `AlcorFinedata::get_phase()` — hot-path representation

**Decision (2026-05-30, CLEAN_OFF C0.2).**  Implementation queued in
cluster C4.

`get_phase()` runs once per hit (~100 M / 5-spill run) and today does
**two** `std::unordered_map::find()` lookups — one on
`calibration_parameters`, one on `channel_calibration_method` —
contributing ~19 s of a 5-spill `--QA`-with-calibration run (two
lookups × ~95 ns × ~100 M hits; measured 2026-05-29 on 20251119-010426).
The frozen-table fast path already removes the per-hit `shared_mutex`,
so the hash lookup itself is the floor.  Three levers were weighed
(BACKLOG P 1.71):

- **(a)** merge the two maps into one
  `unordered_map<uint32_t, struct{array<float,3>, CalibrationMethod}>`
  — one lookup not two.  ~10 s.
- **(b)** replace with `std::vector<CalibEntry>` indexed by
  `GlobalIndex::raw()` — dense key range → single array deref
  (~10 ns vs ~95 ns).  ~15 s.
- **(c)** bake calibration into `AlcorFinedataStruct` at decode time —
  eliminates the lookup.  ~18 s.  Couples decode to calibration
  (+12 B/hit) and complicates the freeze contract.

**Chosen: lever (b).**  Best speedup-to-risk — ~15 s of the ~19 s with
no per-hit memory cost and no decode/calibration coupling.  (a) is the
fallback if a future detector's `GlobalIndex::raw()` range turns out
sparse enough to waste vector memory (not today); (c) stays on the
table only if a later profile shows the array deref's cache misses
dominate.  On-disk TOML schema is unchanged — this is an in-memory
representation swap; `read_calib_from_file` / `write_calib_to_file`
keep their format.

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
