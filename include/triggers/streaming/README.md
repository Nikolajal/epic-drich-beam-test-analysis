# `include/triggers/streaming/` — software trigger pipeline

This folder holds the two-stage software pre-filter that gates the
lightdata writer's output.  Both stages live here so the pipeline is
self-contained, configurable through a single TOML file, and documented
in one place.

> **Design notes:** see [`DISCUSSION.md`](DISCUSSION.md) in this folder
> for the architecture, parameter physics, sub-cell centre refinement,
> `fit_circle` audit, and the open items roadmap.

## Files

| File | Stage | Status |
|---|---|---|
| `score.{h,cxx}` | 1 — DCR-weighted score, time clustering | ✅ Shipped (v1).  Originally `streaming.{h,cxx}` until Phase 3 of the consolidation. |
| `hough.{h,cxx}` | 2 — Hough ring finder + `fit_circle` refinement | ⏳ In progress.  Currently inline in `src/lightdata_writer.cxx` lines ~820–900; extracted in Phase 3. |
| `DISCUSSION.md` | Both | ✅ Current.  Design + open items. |
| `README.md`    | Both | ✅ This file. |

## Configuration

All tuning knobs live in [`conf/streaming.toml`](../../../conf/streaming.toml)
under two sections:

| Section | Stage | Notes |
|---|---|---|
| `[streaming_trigger]` | 1 | Time window, n_σ threshold, min-noise-hits gate.  See `score.h` for parameter semantics. |
| `[streaming_hough]`   | 2 | Hough cell size, radius range, time pre-cut, peak thresholds, max rings, fit_circle initial guess.  Pending Phase 4. |

## Conventions

- **Algorithm headers are not re-exported by `triggers.h`** — include
  them deliberately from the writer (or any other consumer).  Same
  pattern as `utility.h`/`util/` for algorithms vs cross-cutting types.
- **Pipeline stage = one translation unit.**  Each stage has a single
  entry-point free function (`run_streaming_score_trigger`,
  `run_streaming_hough_trigger`) plus any helpers needed to build its
  pre-computed state.
- **No state shared across stages at code level.**  Stage 1's output is
  written into the frame's `trigger_hits` collection as a
  `_TRIGGER_STREAMING_RING_FOUND_` event; stage 2 reads that event back
  to find its trigger times.  No direct call.
- **Magic constants → config, always.**  Anything physically motivated
  (window widths, thresholds, geometry priors) moves into
  `conf/streaming.toml`.  Internal numerical-stability constants
  (e.g. floating-point epsilons) stay in code with a comment.
