# include/triggers/streaming/ — software trigger pipeline

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
| `score.{h,cxx}` | 1 — DCR-weighted score, time clustering | ✅ Shipped (v1).  Was originally `triggers/streaming.{h,cxx}` until the consolidation split the pipeline by stage. |
| `hough.{h,cxx}` | 2 — Hough ring finder | ✅ Shipped.  Extracted out of `src/lightdata_writer.cxx`; called from the writer's per-frame loop.  (The old in-trigger `fit_circle` centre refinement was removed; centre refinement now happens in the recodata re-fit.) |
| `DISCUSSION.md` | Both | ✅ Current.  Design + open items. |
| `README.md`    | Both | ✅ This file. |

## Configuration

All tuning knobs live in [`conf/streaming.toml`](../../../conf/streaming.toml)
under two sections:

| Section | Stage | Notes |
|---|---|---|
| `[streaming_trigger]` | 1 | Time window, n_σ threshold, min-noise-hits gate.  See `score.h` for parameter semantics. |
| `[streaming_hough]`   | 2 | Hough cell size, radius range, time pre-cut, peak thresholds, max rings.  Every knob is live (wired through `run_streaming_hough_trigger`).  (The `fit_circle_init_{x,y,r}` knobs were removed — centre refinement moved to the recodata re-fit.) |

## Conventions

- **Algorithm headers are not re-exported by `triggers.h`** — include
  them deliberately from the writer (or any other consumer).  Same
  pattern as `utility.h`/`utility/` for algorithms vs cross-cutting types.
- **Pipeline stage = one translation unit.**  Each stage exposes its
  entry-point free function(s) plus any helpers needed to build its
  pre-computed state:
    - Stage 1: `run_streaming_trigger` (v0, plain count threshold) and
      `run_streaming_trigger_weighted` (v1, DCR-inverse-weighted score —
      current production path), backed by `build_streaming_trigger_weights`.
    - Stage 2: `run_streaming_hough_trigger`.
- **No state shared across stages at code level.**  Stage 1's output is
  written into the frame's `trigger_hits` collection as a
  `_TRIGGER_STREAMING_RING_FOUND_` event; stage 2 reads that event back
  to find its trigger times.  No direct call.
- **Magic constants → config, always.**  Anything physically motivated
  (window widths, thresholds, geometry priors) moves into
  `conf/streaming.toml`.  Internal numerical-stability constants
  (e.g. floating-point epsilons) stay in code with a comment.
