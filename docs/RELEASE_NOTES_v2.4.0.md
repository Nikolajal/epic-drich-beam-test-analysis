# beam-test-analysis v2.4.0

This release lands the **RANSAC ring-finding pipeline**, **ALCOR ToT/SR
operation modes**, **frame-level multithreading**, and **per-campaign /
per-run configuration auto-selection** — the toolchain the 2026 beam test runs
on.

> **Versioning note.** Numbered **v2.4.0** (the next 2.x); **v3.0.0 is reserved
> for the forthcoming publication milestone**.  Despite the minor number, this
> release carries **backward-incompatible config + QA-tree + hit-mask changes**
> (see *Breaking changes*) — treat the upgrade as a hard migration, not a
> drop-in.

---

## Breaking changes

- **Streaming Hough → RANSAC rename (config + QA tree + code).** The streaming
  ring stage is now RANSAC throughout.  The streaming-conf section
  `[streaming_hough]` → `[streaming_ransac]`; the `hough_*` knobs are retired
  in favour of `ransac_*` (`ransac_iterations`, `ransac_min_significance`,
  `ransac_min_inliers`, …).  QA `TDirectory` / histogram names and the
  ring-tag bits rename Hough → RANSAC.  Old streaming-conf files and any
  consumer keying on the old names must update.
- **HitMask scheme reorganised.** Hit-mask bits are regrouped one byte per
  concern (ring/reco, ToT/SR, afterpulse/cross-talk, lane).  HitMask lives only
  in regenerated `lightdata`/`recodata`, so there is no on-disk migration, but
  any external reader keying on specific bit positions must update.
- **Per-campaign config resolution.** `util::campaign_of` now drives
  `conf/sets/<campaign>/` selection from the run id, and for 2026 splits into
  `2026-SPS` / `2026-PS` (see below).  Config no longer comes solely from the
  shared `conf/` defaults for dated runs.

---

## Highlights

- **Far-off-centre RANSAC ring finding** — a grid-free, completeness-corrected
  finder (mist `v1.2.0`) that recovers wide-radius / partially-visible arcs the
  Hough accumulator missed.
- **ALCOR ToT / SR operation modes** — time-over-threshold (and slew-rate)
  reconstruction via one shared edge-pairing stage, with full ToT QA.
- **Frame-level multithreading** — the post-framer score + RANSAC pass now runs
  frames-within-spill in parallel (~1.76× firing-on), bit-identical to serial
  on every deterministic observable.
- **Config that follows the run** — per-campaign config sets + a per-run
  database that drives op_mode (LET/ToT) and the streaming threshold
  automatically through the QA pipeline.

---

## RANSAC ring finding

- **Far-off-centre arc reconstruction (end-to-end).** A RANSAC finder
  (`mist::ring_finding::find_rings_ransac`) replaces the Hough accumulator:
  it samples hit triplets, scores candidates by inlier excess / visible
  on-sensor arc length against an explicit sensor fiducial, and extracts up to
  two rings.  Deterministic (fixed `std::mt19937` seed), grid-free, and free of
  the wide-radius accumulator cost.
- **Rename sweep.** Streaming "Hough" is renamed to "RANSAC" across code, the
  QA tree, and config (see *Breaking changes*).
- **Tuning harness.** `macros/utilities/ransac_tune.cpp` re-runs the finder on
  an existing `lightdata.root` with CLI knobs (and a `--dt-scan` trigger
  census) so the 2026 far-arc regime can be tuned without re-running the
  writer.

---

## ALCOR ToT / SR operation modes

- **One shared edge-pairing stage.** ToT (PCR mode 4), ToT2 (9) and SR (12)
  pair even/odd TDC edges ({0,1}/{2,3}, even = leading / odd = trailing) into a
  single hit at the leading-edge time carrying a generic `duration` (ToT width
  / SR Δt).  Edges are buffered and **time-sorted before pairing** (ALCOR
  stream order ≠ time order) with a max-pair window rejecting cross-rollover
  mis-pairs.  LET (mode 1) is byte-for-byte unchanged.
- **Complete orphan accounting.** Every edge becomes exactly one tagged hit:
  paired, secondary-orphan (missing stop), leading-orphan (missing start), or
  ToT-saturated (`fine == 0` sentinel).
- **ToT QA (mode-gated).** Per-sensor ToT spectrum (log-Y, double-Gaussian
  1/2-p.e. fit + valley threshold), ToT-vs-channel (3.125 ns bins, log-Z),
  per-channel pairing health, and a per-hardware-trigger Δt-vs-ToT plot.
  LET runs emit none of it (dashboard tiles auto-drop).
- **Provenance.** The output stamps `alcor_op_mode`; the run reconstructs in the
  mode recorded for it (see *Configuration*).
- **`--leading-only`** reconstructs a ToT run from leading edges only (ToT-as-
  LET cross-check).

---

## Frame-level multithreading

- The post-framer per-frame pass (streaming score + RANSAC ring-finding) is
  split into a **pure compute kernel** (parallel, frames-within-spill) and a
  **serial drain** (histogram fills, trigger emits, tree writes in frame
  order), with a noise → weight-build → data segment driver and per-thread QA
  histogram clones.
- **Bit-identical** across thread counts on every deterministic observable
  (ring geometry, peak votes, arc distance, triggers, masks, counts); only
  pixel-jittered hitmap QA differs (RNG).  ~1.76× firing-on speedup.
- **`--skip-stream-qa`** fast path bypasses the whole post-framer pass and
  writes only the raw-hits tree, for quick cross-checks.

---

## Configuration: per-campaign sets + per-run op_mode

- **Per-campaign config sets.** Trigger / readout / mapping that drift across
  campaigns live under `conf/sets/<campaign>/` and are auto-selected from the
  run id (`util::campaign_of` → `util::conf_path`).
- **2026 SPS → PS split.** 2026 splits on the **2026-06-10** boundary:
  `2026-SPS` (`luca_and_finger` on rdo-192) and `2026-PS` (`luca_and_finger` on
  kc705-200).
- **Per-run op_mode (and streaming threshold).** The QA pipeline now passes
  `--run-database run-lists/<YYYY>.database.toml` to the lightdata stage by
  default, so a run tagged `op_mode = 4` reconstructs in ToT automatically —
  no per-run CLI.  The streaming n_σ threshold is resolved the same way.

---

## Fixes & infrastructure

- **Hardware-trigger recovery across campaign FIFO/device drift** — the framer
  detects the trigger FIFO by range (any FIFO > 31), restoring the trigger
  hitmap / timing / in-beam / seeding when the trigger word drifts FIFO/device
  between campaigns.
- **Trigger-chip lane-mask guard** — the trigger chip (FIFO > 31) is kept out
  of the per-device 32-bit lane masks (fixes a debug-build assert).
- **DCR hitmap as an actual rate** (kHz/mm²) with a visible colour scale.
- **Build** — mist pinned to `v1.2.0` ("grid-free ring finding"), which
  provides the RANSAC finder; the repo no longer builds against the older pin.
- **CI** — the clang-format gate is pinned to 22.1.5 (PyPI wheel) to match the
  local toolchain, so clean source no longer trips the gate on apt's drift.

---

## Known issues / follow-ups

- **Framer tree reproducibility.** The framer's `lightdata` TTree is not
  byte-reproducible run-to-run (serial, both LET and ToT) despite the
  output-stabilising sort — a pre-existing defect, orthogonal to the
  multithreading (which is reproducible on every deterministic observable).
  Tracked in BACKLOG.
- **ToT data coverage.** ToT auto-selection is wired end-to-end; only
  `20260604-232500` is tagged `op_mode = 4` so far.  Remaining beam metadata
  is best filled via the Run Book import.

---
