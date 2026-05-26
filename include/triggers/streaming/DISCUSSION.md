# Streaming-trigger pipeline — design notes

This document is the **community-facing** design reference for the
streaming-trigger pipeline.  It lives in the source tree (git-tracked)
alongside the code it describes.  The project-root `DISCUSSION.md` is
local-only — anything the team should be able to refer to about
streaming-trigger design lives **here**.

> **Audience:**  someone joining the project who needs to understand the
> two-stage streaming pipeline before touching it.  Self-contained;
> assumes only the README's level of familiarity with the ALCOR readout.

For the broader trigger subsystem (hardware-trigger source schema,
`TriggerEvent` semantics, registry), see the parent
[`include/triggers/DISCUSSION.md`](../DISCUSSION.md).

---

## Overview

The streaming trigger is a **two-stage software pre-filter** for ring
events, gating the lightdata writer's output:

```
per-frame Cherenkov hits
        │
        ▼  stage 1 — score & cluster
    ┌────────────────────────────────────────────────┐
    │ DCR-weighted score over a sliding time window  │
    │ S = Σ_hits 1/m_c    n_σ = (S-E[S])/σ_S         │
    │ Fire if n_σ ≥ n_σ_threshold                    │
    └────────────────────────────────────────────────┘
        │  TriggerEvent(_TRIGGER_STREAMING_RING_FOUND_)
        ▼  stage 2 — ring finder
    ┌────────────────────────────────────────────────┐
    │ Time pre-cut around streaming-trigger fine_time│
    │ Hough on the surviving xy points (≤ N rings)   │
    │ Per-ring fit_circle refinement                 │
    └────────────────────────────────────────────────┘
        │  TriggerEvent(_TRIGGER_HOUGH_RING_FOUND_)  ×  N
        ▼
    keep frame in lightdata.root
```

Frames that don't fire stage 1 are dropped.  Frames that fire stage 1
but yield zero rings in stage 2 are *not* dropped (stage 1's trigger
event is already in the frame's trigger collection), but downstream
ring-conditioned analyses won't see them.

Both stages are independent of the hardware-trigger schema (D-05 in the
parent doc).  Both consume the configuration sections of
[`conf/streaming.toml`](../../../conf/streaming.toml).

---

## 1.  Score stage  (`triggers/streaming/score.{h,cxx}`)

> **Status as of 2026-05-26:**  **v1 shipped.**  The score stage is
> operational and being tuned in production runs (current operating
> point: `n_sigma_threshold ≈ 15`, `time_window_ns = 20`).

A per-frame online time-cluster detector.  Scans the frame's
Cherenkov hits with a sliding time window, computes a DCR-weighted score
of the hits in the window, and emits a `_TRIGGER_STREAMING_RING_FOUND_`
event when the score crosses an n_σ threshold.

### 1.1  Why weighted counting

Cherenkov hits arrive in time-coincidence on the ring locus; SiPM dark
counts arrive independently per channel.  A naïve "count hits in 5 ns
window" trigger:

- treats every channel as equally informative;
- is dominated by hot pixels (one channel firing at 10× the median DCR
  drags the false-positive rate up by an order of magnitude);
- needs re-tuning whenever the active-channel count changes (masking
  routine, detector reconfiguration, new device added).

Under the simplifying noise model (per-channel Poisson at rate $\lambda_i$,
plus a uniformly-likely signal across channels), the Neyman–Pearson
optimal pre-filter in the small-signal limit reduces to thresholding

$$S = \sum_{i \in \text{window}} \frac{1}{\lambda_i}$$

— each hit's contribution is its **inverse DCR**.  Quiet channels carry
strong evidence; noisy channels barely contribute.  The "all channels
equal" assumption is replaced by a measurement.

We don't have a per-channel signal model (knowing which channels are on
the ring is the downstream problem the trigger feeds) so signal-aware
weighting $w_i = s_i / \lambda_i$ is deferred — see §1.5.

### 1.2  Reading DCR

The score stage consumes the per-channel DCR profile that the lightdata
writer already maintains for its own QA:

| Histogram | Filled where | Content |
|---|---|---|
| `h_dcr_per_channel` (TProfile) | [`src/lightdata_writer.cxx`](../../../src/lightdata_writer.cxx) DCR-QA fill site, conditional on `TriggerFirstFrames` on the frame | `TProfile::Fill(channel_ordinal, per_frame_hit_count)`: bin content is the **mean** per-frame hit count over all noise frames processed so far |

The trigger consumes the **pre-Scale** TProfile (the writer's final
`Scale(1 / frame_length_ms)` to convert to kHz runs *after* the spill
loop ends).  Internally the trigger works in dimensionless
"expected hits per trigger window" units:

$$k = \frac{T_{\mathrm{win}}}{T_{\mathrm{frame}}}, \quad m_c = \mu_c \cdot k, \quad w_c = 1/m_c$$

— with $T_{\mathrm{win}}$ and $T_{\mathrm{frame}}$ in matching units so
the ratio is dimensionless.  No Hz, no seconds inside the trigger; only
on the user-facing config knobs.

**Rebuild cadence.**  The weight bundle is built **exactly once per
spill**, at the moment the per-frame loop transitions from the
first-frames (noise) window into the data window — i.e. immediately
after this spill's noise frames have finished populating
`h_dcr_per_channel`.  Per-spill rebuilds track channels that come
online late (RDO previously off) and channels that drift in rate over
the fill.

The bundle itself is **run-scope** — it persists across spills, so
spill N's noise frames see spill N-1's already-built weights.  Only the
"have we rebuilt yet for this spill" flag is reset at spill start.

### 1.3  Channel reliability gate

A channel must accumulate at least `min_noise_hits` (default 5) in the
cumulative noise sample before it enters the weight bundle.  Under-
measured channels are excluded outright — admitting them with a clamped
weight (the old `lambda_floor_hz` strategy) rewarded rare-fire pixels
with the maximum possible weight, producing a 20+ σ outlier tail in the
noise QA histogram.

Hits on channels not in the bundle (uncalibrated for any reason — too
few noise hits, late onset between spills, dead pixel inside an
otherwise-active lane) are **skipped entirely** by the trigger hot
loop: no window insert, no `running_score` change, no QA fill.  Any
constant fallback would map every uncalibrated hit to the same
$n_\sigma$ and produce a delta-function spike in the QA hist.

### 1.4  Threshold and QA

The score $S$ is converted to a standardised score before thresholding:

$$n_\sigma = \frac{S - \mathbb{E}[S \mid H_0]}{\sqrt{\mathrm{Var}[S \mid H_0]}}$$

with $\mathbb{E}[S | H_0] = N_{\mathrm{ch}}^{\mathrm{active}}$ (count
of channels in the bundle) and $\mathrm{Var}[S | H_0] = \sum_c 1/m_c$.
The framer fires when $n_\sigma \geq n_\sigma^\star$, where
$n_\sigma^\star$ is set in
[`conf/streaming.toml → [streaming_trigger] n_sigma_threshold`](../../../conf/streaming.toml).

> **`n_σ` is a standardised score, *not* a Gaussian z-score.**  The
> underlying $S$ is **positive-bounded** ($S \geq 0$, by construction —
> weights are positive, hit counts are non-negative).  That alone
> forces the noise distribution to be one-sided: the minimum admissible
> $n_\sigma$ is $-\sqrt{\Lambda}$, where $\Lambda = \sum_c m_c$ is the
> expected total hits per window.  For sparse noise ($\Lambda \lesssim
> 1$, typical) the distribution can barely fluctuate below the mean,
> and the per-hit sampling regime (we fill QA at every hit, dropping
> S = 0 windows) shifts the *observed* distribution strictly positive.
> Bin structure is also discretised by integer hit counts (the comb
> seen on the noise plot).  Operationally this is fine — $n_\sigma$ is
> used as a **discriminator** between noise and signal tails, not as a
> literal "standard deviations above zero".  Don't expect the noise QA
> hist to be centred at zero or Gaussian-shaped.

**Two QA histograms** are filled **always**, regardless of whether the
threshold is crossed.  Both are normalised by entry count at write
time, so the y-axis is **probability per bin**:

| Histogram | When filled | What it tells you |
|---|---|---|
| `h_streaming_score_noise` | First-frames (noise) window | Pure-noise n_σ distribution.  $\int_{n_\sigma \geq n_\sigma^\star}$ → **misfire probability** |
| `h_streaming_score_data`  | Data-taking window           | Signal+noise.  Integral above threshold → **acceptance** |

A pre-made overlay canvas `c_streaming_score_overlay` (red noise + blue
data, log-Y, axis 0–50) is written alongside for visual threshold
tuning.

### 1.5  Workflow

```
  ┌───────────────────────────────────────────────────────┐
  │  framer runs with current n_σ★ (initial guess)        │
  │  • streaming trigger fires above threshold            │
  │  • QA histograms accumulate full distribution always  │
  │  • lightdata writer keeps only fired frames           │
  └───────────────────────┬───────────────────────────────┘
                          │
                          ▼
        ┌───────────────────────────────────────┐
        │  inspect h_streaming_score_{noise,data}│
        │  compute misfire & acceptance at n_σ★  │
        └───────────────────────┬───────────────┘
                                │
                ┌───────────────┴───────────────┐
                │                                │
        operating point fine            operating point off
                │                                │
                ▼                                ▼
             done                  update config, re-run
```

Set `n_sigma_threshold` to a large sentinel (e.g. 1000) to disable
firing while still accumulating QA — useful for first runs on a new
detector configuration.

### 1.6  Deferred to v2

Three improvements are explicitly **out of scope** for v1:

- **Conservative DCR estimator.**  Using the 75th-percentile (rather
  than mean) of the per-channel noise distribution would bias
  $\lambda_i$ estimates *high*, making the trigger more robust to
  under-measured noise.  Adds estimator complexity; not in v1.
- **Crosstalk correction.**  Crosstalk inflates apparent per-channel
  rates non-Poissonianly.  Needs to be either subtracted from
  $\lambda_i$ before weighting, or carried as a per-channel
  multiplicity factor.  Couples the trigger to the crosstalk-treatment
  pipeline; not in v1.
- **Signal-aware weighting** $w_i = s_i / \lambda_i$.  Requires a
  measured per-channel signal model — bootstrap by running v1,
  collecting a ring sample, building the signal histogram,
  re-triggering.  Multi-pass calibration; revisit when v1 statistics
  warrant it.

---

## 2.  Hough stage  (`triggers/streaming/hough.{h,cxx}` — pending)

> **Status:** code currently lives **inline** in
> [`src/lightdata_writer.cxx`](../../../src/lightdata_writer.cxx) (lines
> ~820–900).  Magic constants are scattered.  Phases 2-4 of the
> consolidation plan extract it into the dedicated translation unit and
> move every magic value into [`conf/streaming.toml → [streaming_hough]`](../../../conf/streaming.toml).

### 2.1  Pipeline

On every frame where the score stage fired
(`_TRIGGER_STREAMING_RING_FOUND_` present in `triggers_in_frame`):

```
streaming-trigger event with fine_time = t★
        │
        ▼  time pre-cut
   keep cherenkov hits with |t_hit − t★| < time_cut_ns
        │
        ▼  Hough ring finder
   alcor_find_rings_hough(ring_finder, candidates, ...)
        │  (uses one shared HoughTransform built once at writer init)
        │
        ▼  per ring (up to max_rings)
   mask hits with HitmaskHoughRingTagFirst / Second
   fit_circle on the masked hits → refined centre + radius
        │
        ▼
   emit TriggerEvent(_TRIGGER_HOUGH_RING_FOUND_)
```

### 2.2  Parameter inventory (current values)

| Where | Current value | What it controls |
|---|---|---|
| `HoughTransform` ctor | `r_min = 20 mm`, `r_max = 120 mm` | Radius scan range |
| `HoughTransform` ctor | `r_step = 1 mm` | Radius granularity in the accumulator |
| `HoughTransform` ctor | `cell_size = 3 mm` | Accumulator XY cell size — **sets the discrete centre resolution** (see §2.3) |
| Time pre-cut         | `±10 ns` (hardcoded) | Hits selected around `fine_time` |
| `alcor_find_rings_hough` | `threshold_fraction = 0.33` | Min fraction of active hits in peak cell |
| `alcor_find_rings_hough` | `min_hits = hough_threshold × 0.75` | Absolute min vote count (slack on min_active) |
| `alcor_find_rings_hough` | `min_active = hough_threshold` (= `ceil(0.004 · N_active)`) | Minimum surviving hits for Hough to run |
| `alcor_find_rings_hough` | `max_rings = 2` | Hard cap on rings returned per frame |
| `alcor_find_rings_hough` | `collection_radius = 7.5 mm` | Width of ring band for hit association |
| `fit_circle`         | `init = {0, 0, 50}` | Centre-at-origin / R = 50 mm prior |

Every entry in this table moves to a config knob in Phase 4 of the
consolidation plan.

### 2.3  Sub-cell centre refinement  *(was D-03)*

The Hough accumulator's peak cell is **discrete**: the recovered centre
is quantised to `cell_size = 3 mm` regardless of how well the underlying
hits constrain it.  For a single Cherenkov ring this is good enough; for
two close rings, or when the true centre falls near a cell boundary, the
discretisation is the dominant systematic on the recovered centre
position.

The Hough transform was never expected to yield sub-cell centres — that's
not what an accumulator is for.  A two-stage approach is the obvious
direction:

1. **Hough as a candidate finder** (current) — fast, robust, gives an
   approximate centre and a hit-association list.
2. **Sub-cell refinement** on the hits assigned to the candidate ring.
   Options, in increasing order of complexity:

   - **Centroid of the per-hit predicted-centre locus.**  Each hit at
     $(x, y)$ on a ring of radius $R$ has its centre on a circle of
     radius $R$ around $(x, y)$.  The intersection of those circles
     (or, equivalently, the centroid of their intersection points) is
     the true centre.
   - **Least-squares circle fit** on the assigned hits via the existing
     [`fit_circle`](../../util/circle_fit.h) (see §2.4 for the audit).
   - **Iterative re-association**: refine centre → re-collect hits
     within `collection_radius` → refine again → converge.
   - **MIST's planned `nn_transform`** (NN-based ring finder, listed in
     MIST's `DISCUSSION.md` F-01) for end-to-end refinement.

**Decisions needed before this lands:**
- Are sub-cell ring centres a real analysis bottleneck right now?  (If
  the beam-test resolution is limited by other factors — timing, optics
  — this is parking-lot work.)
- If yes, pick a refinement strategy and benchmark against the
  Hough-only centre on synthetic and beam-test data.
- Coordinate with the `fit_circle` audit in §2.4 since the refinement
  step likely calls into the circle fit.

### 2.4  `fit_circle` role and audit  *(was D-04)*

The per-ring centre refinement currently calls
[`fit_circle`](../../util/circle_fit.h) (least-squares χ² minimisation
on radial residuals via `ROOT::Fit::Fitter`).  The function has the
right shape (ROOT fitter, radial-residual objective, optional `fix_XY`
parameter, optional `exclude_points` mask) but several things should be
addressed before it's relied on as the back-end of §2.3 or any new
physics analysis:

| Concern | Current state |
|---|---|
| ~~Error path~~ | ✅ Resolved.  `CircleFitResults result{};` value-init + restored early-return on `!FitFCN()`; failure path returns a NaN-tagged sentinel.  Callers test `std::isnan(result[2][0])`. |
| ~~`exclude_points` default arg~~ | ✅ Resolved.  `{{}}` → `{}` — default no longer silently drops point 0. |
| Initial values | Caller supplies `{x0, y0, R}` with no validation.  A degenerate initial guess (e.g. `R = 0`) silently produces nonsense.  Worth at least an assert or a documented contract. |
| `fix_XY` semantics | When `true` the fit varies only `R`.  Useful for known-centre geometries but masks fit-quality information about the centre.  No way for the caller to ask for "fit X, fix Y" or vice versa — only the joint flag. |
| `exclude_points` scan cost | Linear scan inside the chi² loop.  Fine for small `exclude_points`, but worth a `std::unordered_set` if it ever grows. |
| Uncertainty propagation | Returns the `ROOT::Fit::FitResult::Errors()` values verbatim — these assume a properly-normalised chi².  No documentation of what "normalisation" the caller should expect when the points have heterogeneous uncertainties (currently the chi² treats every point with equal weight). |
| Output structure | `CircleFitResults = std::array<std::array<float, 2>, 3>` — packed `{x0, y0, R}` with errors but no fit-quality fields (chi², ndf, status code).  A small struct with named fields would be clearer; the NaN sentinel for failure is a stopgap. |
| Test coverage | Zero tests today.  A round-trip test (inject N points on a known circle with Gaussian noise → fit → recover within stat error) is cheap and would catch any future regression. |

**Decision before next use:**
- Patch `include/util/circle_fit.h` in place — add the remaining
  initial-value validation, `fix_XY` granularity, fit-quality
  named-struct return, and the round-trip test.
- Or migrate to a library implementation (ROOT has `TGraph::Fit("pol2")`
  workarounds; MIST could absorb a `mist::circle_fit` if it grows into a
  cross-project need).

Either path should also replace the NaN-sentinel return with a proper
`std::optional<CircleFitResults>` or a named-struct fit-quality field.

### 2.5  Open items (gated on extraction + config)

Items that become small focused changes once the Hough stage is its
own translation unit with config-driven knobs (Phase 5 of the
consolidation plan):

- **Time-cut width alignment.**  The Hough's time pre-cut (±10 ns
  hardcoded) is half the streaming-score window (currently 20 ns).
  These should default to matching values — otherwise the Hough may
  drop hits that the score stage included, or vice versa.  Default
  the Hough's `time_cut_ns` to the score's `time_window_ns`.
- **`fit_circle` init from Hough peak.**  The current `{0, 0, 50}` fixed
  prior pulls every ring towards origin / R = 50 mm regardless of where
  the Hough actually peaked.  Pass the Hough's discrete `(x_c, y_c, R)`
  estimate as the initial guess; the fit then refines locally instead
  of climbing back from a generic starting point.
- **Hough threshold formula review.**  `hough_threshold = ceil(0.004 ×
  N_active_cherenkov)` was tuned for the era when the streaming trigger
  didn't gate Hough entry.  Now that stage 1 is selective, stage 2's
  threshold can be re-derived (or made a flat tunable knob).
- **Multi-ring beyond 2.**  The `max_rings = 2` cap may be limiting for
  multi-track events (cosmic + beam) or events with reflection rings.
  Audit how many frames hit the cap; consider raising or making
  configurable.
- **QA refresh.**  Plot `n_σ_streaming` vs `n_rings_found` to expose the
  correlation between the two stages; per-ring radius histograms with
  overlay for the two ring slots; Δt between Hough-trigger time and
  streaming-trigger time.

---

## 3.  Cross-cutting

### 3.1  Extension to other detectors

The weighting framework (§1.1, §1.2) is detector-agnostic — each channel
has its own $\lambda$ regardless of subsystem.  The streaming trigger is
currently Cherenkov-only.  Extending to timing / tracking detectors
needs:

- a separate DCR profile per detector class (or a multi-detector
  TProfile);
- separate threshold tuning and separate QA histograms per detector;
- a coincidence-trigger mode if multi-detector events are interesting.

Tracked separately from this design; revisit once the Hough stage lands
(Phases 2-4) and the Cherenkov pipeline is stable.

---

*Document version: 2026-05-26.*
*Implements: D-12 score stage (v1 landed).  In progress: streaming-Hough
stage extraction (Phases 1-4), absorbing former D-03 (sub-cell centre
refinement) and D-04 (`fit_circle` audit).*
