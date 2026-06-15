# Streaming-trigger pipeline — design notes

> 🧭 **Hub:** project-wide design log + index of satellites lives at
> [`../../../DISCUSSION.md`](../../../DISCUSSION.md).
> Parent satellite: [`../DISCUSSION.md`](../DISCUSSION.md).

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
    │ RANSAC on the surviving xy points (≤ N rings)   │
    │ Tag ring-member hits with HitmaskRansacRingTag* │
    └────────────────────────────────────────────────┘
        │  TriggerEvent(_TRIGGER_RANSAC_RING_FOUND_)  ×  N
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

> **Status:**  **v1 shipped.**  The score stage is
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

**Three QA histograms** are filled **always**, regardless of whether the
threshold is crossed.  All are normalised by entry count at write time
(`Scale(1/GetEntries())` in `finalize_streaming_qa`), so the y-axis is
**probability per bin**, and all share the same 200 log-spaced bins over
`[0.1, 1000]` n_σ:

| Histogram | When filled | What it tells you |
|---|---|---|
| `h_streaming_score_noise`  | First-frames (noise) window      | Pure-noise n_σ distribution.  $\int_{n_\sigma \geq n_\sigma^\star}$ → **misfire probability** |
| `h_streaming_score_data`   | Data-taking window               | Signal+noise.  Integral above threshold → **acceptance** |
| `h_streaming_score_inbeam` | Fixed window just before each hardware trigger | In-beam, pre-signal background.  Its first empty bin sets the recommendation cut (§1.4.1) |

The published `05_streaming_score.pdf` (lightdata writer) overlays all
three on a **log-x / log-y** canvas over `[0.1, 1000]` — noise **blue**,
data **red**, in-beam **violet** — with the recommendation cut line. (A
legacy `c_streaming_score_overlay` ROOT canvas is also written.)

### 1.4.1  Streaming-score overlay fit  *(modelled, GATED OFF — `kEnableScoreFit = false`)*

**Status (parked).** The `05_streaming_score.pdf` QA plot carries an
optional **component-fit overlay** that models the score distribution as
a sum of physical populations. The model and its fitting machinery are
fully implemented in `src/lightdata_writer.cxx` but **disabled behind a
compile-time flag** (`constexpr bool kEnableScoreFit = false`): the block
neither runs nor draws. The plot ships with the three histograms +
the (independent, histogram-derived) recommendation cut line only. Flip
the flag to `true` to re-enable for offline experimentation. This note
records the model, what was tried, and why it isn't shipped, so the next
person doesn't re-derive it from scratch.

**What the data is.** On a real run (benchmark `20251111-164951`, QA mode,
log-spaced 200 bins over `[0.1, 1000]` n_σ, normalised to probability per
bin) the `data` curve is **three populations**, not a single bump:

| population | n_σ | physics |
|---|---|---|
| DCR / null      | ~1.7 | random dark counts (no event) |
| in-beam bkg     | ~35  | beam-correlated background, pre-signal |
| Cherenkov signal| broad, ~80–200, tail to ~600 | ring photons |

**The model (physically motivated).**
- **DCR = log-Gaussian**, parametrised `A·exp(−½·((log10 x − log10 μ)/s)²)`
  (code uses `log10([1])`, **not** `[1]`) — dark counts are log-normal in n_σ
  (symmetric bump on the log-x axis). Written this way, the mean param **μ is
  the peak position directly in n_σ** (peak at `x = μ`), not a log you must
  exponentiate. `s` stays a log10-space width (decades).
- **in-beam & signal = LINEAR Gaussians** `A·exp(−½·((x − μ)/σ)²)` — each
  is a Poisson **photon-count** population, so Gaussian in *linear* n_σ
  (μ, σ in n_σ directly). On the log-x axis a linear Gaussian renders
  *skewed* (steeper low side). The signal term runs **broad** (σ ≳ μ) and
  acts as the pedestal under the whole high-n_σ tail — this is what lets
  the model track the tail out to ~600, which a log-Gaussian cannot.

**The fit recipe (cascade, validated in batch).**
1. Fit the **DCR** log-Gaussian on the *clean* `h_noise` sample over a
   **focused window** `[0.3, 30]` n_σ (NOT the full support — the noise
   sample's sparse high-n_σ tail otherwise pulls the single Gaussian off
   the bump). Records the DCR **shape** (m, s) + norm.
2. Fit **in-beam** = `h_inbeam` as DCR(shape **fixed**, norm free within
   **±10 %**) + one free linear Gaussian, seeded **data-driven** (local
   maximum of the smoothed tail) with mean floored a **decade (×10)**
   above the DCR.
3. Fit **data** = `h_data` as DCR(fixed, ±10 %) + in-beam(shape **fixed**,
   norm **free** — its fraction differs between the pre-trigger window and
   the full spill) + free broad signal linear Gaussian.
   Each lower component's **mean & width are hard-fixed**; only its
   normalisation breathes (DCR ±10 %, bumps free). Means are enforced
   **incremental** (each new bump ≫ the one below).

**Why it's gated off.** The model is correct and fits *one* run
beautifully (DCR 1.7 / in-beam ~40 / broad signal tracking the tail), but
it is **not robust enough to ship unattended**:
- **Normalisation sensitivity.** The ±10 % DCR-norm leash and the χ²
  landscape assume the three samples are on a comparable
  probability-per-bin scale. The same fit that converges on the *saved*
  (normalised) histograms in a batch macro **diverges in the writer**
  (in-beam railed to a low shoulder, signal mislabelled) — strong sign of
  a normalisation/weighting mismatch at fit time that wasn't fully run to
  ground.
- **χ² is near-meaningless here.** ~10⁸ data entries ⇒ minuscule per-bin
  errors ⇒ χ²/ndf in the millions; the optimiser chases the bulk and
  abandons the small tail bumps, and falls into narrow-spike local minima.
  A Poisson **log-likelihood on raw counts** (option `"L"`, pre-norm) is
  the right tool and was *not* yet wired in.
- **Bound-railing.** Bumps repeatedly parked on their mean/width limits,
  so the reported component n_σ were boundary artefacts, not fits.
- **Cross-run validation** (the model holding on runs other than the
  benchmark) was never done.

**To resume** (checklist for whoever picks this up): fit on **raw counts
with `"L"`** then normalise only for display; confirm the writer fits the
*same* histogram state the batch macro does; keep the DCR window focused
(`[0.3,30]`); keep the cascade (DCR log-G fixed → in-beam linear-G →
broad signal linear-G) with data-driven seeds + the ×10 DCR→in-beam mean
floor; leave the signal **broad** (don't bound its width); validate on
≥3 runs before flipping `kEnableScoreFit` back on.

> ⚠️ **Parked code is mid-migration.** The DCR term + the readout (logger
> echo, cut-loss) are coherent: the DCR log-Gaussian uses `log10([1])` so
> `[1]` is the DCR mean directly in n_σ, the DCR-derived tail floor is
> linear, and all echoed means/cut-loss are linear. **Still to reconcile**
> on resume: the in-beam/signal **linear**-Gaussian terms in stages 2–3
> still carry log10-era seeds, mean floors, and width bands (e.g.
> `find_bump_*` returns log10, `m2_lo`/`m3_lo` and `kWidth*` are log10
> values, and the in-beam/signal component-overlay TF1s use a log-Gaussian
> formula). Convert those to linear n_σ (mirror the validated batch values:
> in-beam μ≈40 σ≈10, signal μ broad σ≳100) before re-enabling.

The histogram-derived
**recommendation cut** (primary: the **first empty bin of the in-beam
background histogram** above its peak — where beam-correlated background
runs out; fallback for runs with no in-beam sample: an FP = 1e-6 walk on
the combined background) is a separate, shipped computation and does
**not** depend on the gated fit.

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

### 1.5.1  QA-mode disable + analyser threshold-setting (2026-05-29 audit)

The audit landed two structural changes that operationalise the
threshold-setting loop and decouple it from writer wall-time:

1. **`conf/QA/streaming.toml` now disables firing** (`n_sigma_threshold
   = 1e9`).  The QA-mode writer (`--QA`) accumulates the noise+data
   score histograms in full but pays **0 s of RANSAC-cascade CPU**:
   audit showed streaming+RANSAC was 290 s of a 358 s baseline; with
   `--QA` the writer drops to ~55 s on 5 spills.  Operator gets the
   full score distribution to read off without waiting for RANSAC work
   that's going to be thrown away anyway.

2. **`qa/lightdata/05_streaming_score.pdf`** — emitted by the
   lightdata writer on every run (QA *or* production).  Red curve =
   `h_streaming_score_noise` (first-frames noise sample), blue curve =
   `h_streaming_score_data` (post-first-frames data sample).  Log-Y so
   the bkg-vs-sig crossover is readable.

**Analyser workflow** (replaces the old "1.5 Workflow" diagram for
fresh runs):

```
  ┌──────────────────────────────────────────────────────────────┐
  │  --QA writer run                                              │
  │    streaming threshold = 1e9 → 0 fires, 0 RANSAC               │
  │    score hists fill normally on every window                  │
  │    writer ~55 s on 5 spills (vs ~358 s w/ default threshold) │
  └────────────────────────────┬─────────────────────────────────┘
                               │
                               ▼
  ┌──────────────────────────────────────────────────────────────┐
  │  Operator inspects qa/lightdata/05_streaming_score.pdf       │
  │    Where does data tail cross noise tail?                    │
  │    What's the misfire rate at the proposed cut?              │
  │    Is there a clean signal-tail-only region?                 │
  └────────────────────────────┬─────────────────────────────────┘
                               │
              ┌────────────────┴───────────────┐
              │                                │
       no/marginal sig                      clear sig
       run lives with                       set production
       threshold = ∞                        n_sigma_threshold
       (skip RANSAC cascade)                 (database hook — see § 1.5.2)
              │                                │
              ▼                                ▼
       QA-only outputs              production writer w/ tuned n_σ
```

### 1.5.2  Per-run threshold field

The analyser-chosen production threshold has a home: a per-run field in
the run database.  All three options (TOML field, CLI flag, dashboard
widget) are shipped.

- **Option A — Per-run TOML field** (SHIPPED).
  `run-lists/<year>.database.toml` accepts
  `streaming_n_sigma_threshold = X` per run id, inherits from the
  previous run when absent (same pattern as the other run-card
  fields).  `lightdata_writer`'s CLI driver takes `--run-database
  PATH`, looks the field up post-CLI-parse via
  `RunInfo::get_run_info(run_name)`, and overrides
  `streaming_trigger_cfg.n_sigma_threshold` before the writer
  launches.  Clean separation: per-run physics, not per-campaign
  config.  `0` (the default) leaves the streaming-conf value
  untouched.
- **Option B — CLI flag** `--n-sigma-threshold X` (SHIPPED).
  Direct override that bypasses the rundb lookup.  Takes priority
  over `--run-database` when both are supplied — convenient for
  ad-hoc operator tuning without editing the rundb file.
- **Option C — Annotation on the score PDF in the dashboard**
  (SHIPPED).  Clicking the `h_streaming_score_noise` /
  `h_streaming_score_data` thumbnail in QA → Full plots → Lightdata
  opens the n_σ picker dialog (`_StreamingScorePickerDialog` in
  `qa_quicklook/qa.py`): both samples overlay on a log-Y matplotlib
  canvas, a draggable vertical line marks the candidate cut, a side
  panel shows `P(misfire)` / `Acceptance` / `S/N above cut` /
  `N(data) above cut` live.  Seed priority chain:  rundb-saved value
  → noise/data crossover heuristic when the conf default is at the
  QA-disable sentinel → conf default → 5.0 fallback (see
  `qa_quicklook/streaming_picker.py :: seed_threshold`).
  "Save to rundb" commits via `rundb.update_run_field` with
  `auto_pin=True` so only the active run changes; downstream
  inherit-from-prev runs are pinned to the OLD value so their
  merged view is preserved.  Source-tagged
  `"dashboard:streaming_picker"` in the audit log for provenance.

### 1.6  Deferred to v2

Four improvements are explicitly **out of scope** for v1:

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
- **Upper-bound multiplicity cut** *(flagged 2026-Q2 from RANSAC
  calibration QA)*.  The score stage today gates only the lower bound
  (`n_σ ≥ n_sigma_threshold`).  Events with very high hit counts in
  the time window — observed at `|active| ≈ 80–100` in
  `peak_votes_vs_active_first` while typical real-ring events sit at
  `|active| ≈ 15–25` — also pass the lower-bound gate but are
  **structurally not Cherenkov rings**: they're multi-particle
  pile-up, electronics bursts, common-mode noise, shower events, or
  cascading afterpulses.  The RANSAC then dutifully finds *some* peak
  in the noise, producing a "ring" that's downstream of a non-ring
  event.  Proposed knob: `n_sigma_max_threshold` (or a count-based
  `max_hits_per_window`) that rejects clusters above a multiplicity
  ceiling.  Equivalent to making the trigger a band rather than a
  single-sided cut.  Needs a stage-1 QA hist of `|active|` /
  hits-per-cluster so the ceiling can be tuned from data, in the same
  spirit as the noise-vs-data overlay used for the lower bound.

### 1.7  Per-frame scratch-buffer lifecycle  *(clarity cleanup, no perf win)*

`run_streaming_trigger_weighted` runs once per frame and, in the original
form, allocated a fresh set of working vectors each call:
`cherenkov_finedata_hits`, `orig_idx`, the sort-reorder `tmp`, the
sliding-`window` deque, and `peak_times`.  They are now **`static
thread_local` + `clear()` at entry** — one allocation lifecycle reused
across frames (`clear()` keeps capacity, so after the first few frames
these allocate ~nothing).  The score path is serial on the main thread
(see the streaming/RANSAC serial note in the parent DISCUSSION); the
`thread_local` keeps it correct if that ever changes.  The `median_of`
lambdas also take `const std::vector<float>&` instead of by-value.

**This is a clarity/lifecycle change, NOT a measured speedup.**  The
hypothesis was that per-frame vector churn was a dominant malloc/free
cost (it showed up heavily in `sample`).  A controlled before/after A/B
(5-spill `--QA` on 20251119-010426, same continuous machine load,
3× POST bracketing 2× PRE) found **no win**:

| | wall (s) | mean | max RSS (GB) | mean |
| --- | --- | --- | --- | --- |
| PRE (per-frame alloc) | 114.5, 116.4 | ~115.5 | 11.70, 10.58 | ~11.1 |
| POST (reused buffers) | 118.1, 111.8, 119.7 | ~116.5 | 9.64, 11.29, 11.95 | ~11.0 |

Wall flat (POST marginally slower, noise); RSS flat (POST produced both
the lowest *and* the highest single sample).  So the per-frame
allocations are **not** the wall-clock or RSS driver — the allocator
absorbs the churn cheaply, or the real cost lives elsewhere (framer
per-hit reconstruction, all-thread allocation, dense-map scans).  Kept
because reused buffers are simpler than per-frame ones and cannot be
slower, but it carries **no** performance claim.  To find the real hot
points, **heap-profile** rather than reason from `sample` stacks — same
lesson as the `get_phase` fusion (see `include/utility/DISCUSSION.md`).

---

## 2.  RANSAC stage  (`triggers/streaming/hough.{h,cxx}`)

> **Status:** ✅ **shipped.**  Phases 2-4 of the consolidation plan
> completed: the algorithm was extracted out of `src/lightdata_writer.cxx`
> into [`src/triggers/streaming/ransac.cxx`](../../../src/triggers/streaming/ransac.cxx)
> (declared in [`include/triggers/streaming/ransac.h`](hough.h)), and
> every magic value moved into
> [`conf/streaming.toml → [streaming_ransac]`](../../../conf/streaming.toml).
> Entry point: `run_streaming_ransac_trigger(...)`, called per-frame from
> the writer's main loop.

### 2.1  Pipeline

On every frame where the score stage fired
(`_TRIGGER_STREAMING_RING_FOUND_` present in `triggers_in_frame`):

```
streaming-trigger event with fine_time = t★
        │
        ▼  time pre-cut
   keep cherenkov hits with |t_hit − t★| < time_cut_ns
        │
        ▼  RANSAC ring finder
   ring_finder.find_rings(generic_hits, ...)
        │  (uses one shared HoughTransform built once at writer init)
        │
        ▼  per ring (up to max_rings)
   mask hits with HitmaskRansacRingTagFirst / Second
        │
        ▼
   emit TriggerEvent(_TRIGGER_RANSAC_RING_FOUND_)
        │
        ▼  (recodata_writer, downstream)
   re-fit mask-tagged hits → refined centre + radius
```

### 2.2  Parameter inventory (current values)

| Where | Current value | What it controls | Tunable? |
|---|---|---|---|
| `HoughTransform` ctor | `r_min = 35 mm`, `r_max = 105 mm` | Radius scan range | ✅ `[streaming_ransac].r_min/r_max` |
| `HoughTransform` ctor | `r_step = 1.5 mm` | Radius granularity in the accumulator | ✅ `[streaming_ransac].r_step` |
| `HoughTransform` ctor | `cell_size = 1.5 mm` | Accumulator XY cell size — **sets the discrete centre resolution** (see §2.3) | ✅ `[streaming_ransac].cell_size` |
| Time pre-cut          | inherited from streaming-score window | Hits selected around `fine_time` (`|t_hit − t_streaming| < time_window_ns`) | ❌ inherited from `[streaming_trigger].time_window_ns` |
| `find_rings` | `threshold_fraction = 0.33` | Min fraction of active hits in peak cell | ✅ `[streaming_ransac].threshold_fraction` |
| `find_rings` | `min_hits = min_active × 0.75` | Absolute min vote count (slack on min_active) | ✅ `[streaming_ransac].min_hits_slack` |
| `find_rings` | `min_active = ceil(0.0035 · N_active)` | Minimum surviving hits for RANSAC to run | ✅ `[streaming_ransac].hough_threshold_fraction` |
| `find_rings` | `max_rings = 2` | Hard cap on rings returned per frame | ❌ hardcoded (physical: two radiators ⇒ max two concentric rings) |
| `find_rings` | `collection_radius = 2 mm` | Width of ring band for hit association | ✅ `[streaming_ransac].collection_radius` |
| ~~`fit_circle_init_{x,y,r}`~~ | — | **REMOVED 2026-05-30** (commit 87e8af2, CLEAN_OFF C3.5).  No live consumer — recodata seeds the per-ring refit from the RANSAC peak `(cx, cy, radius)` directly. | Reader emits one-shot deprecation warning per key still in config; tolerance to be removed in a later release. |

**Non-tunable rationale:**

- `time_cut_ns` is **inherited from `[streaming_trigger].time_window_ns`**.
  There is no physical reason for the RANSAC hit-selection window to
  differ from the score-stage clustering window — allowing them to drift
  only creates a configuration with two ways to misalign.
- `max_rings = 2` is a **detector-geometry constraint**: the dRICH
  prototype has two Cherenkov radiators (aerogel + gas), so at most two
  concentric rings can fire on a single charged-particle event.  A
  third "ring" from the algorithm would necessarily be noise or
  combinatorial — better to cap it at the physical limit.

### 2.3  Sub-cell centre refinement  *(was D-03)*

The RANSAC accumulator's peak cell is **discrete**: the recovered centre
is quantised to `cell_size = 3 mm` regardless of how well the underlying
hits constrain it.  For a single Cherenkov ring this is good enough; for
two close rings, or when the true centre falls near a cell boundary, the
discretisation is the dominant systematic on the recovered centre
position.

#### 2.3.1  Sliding-window aggregation in the peak finder  *(MIST patch — **shipped**)*

> **Status:** both halves of this section are
> implemented, tested, and active in the current writer binary.
> The active TOML recipe:
> `r_step = cell_size = 1.5 mm`, `aggregation_window_cells = 2`.

**2026-Q2 measurement (writer-side QA `RANSAC rings/`):**  with
`cell_size = 3 mm` the per-event ring-1 `peak_votes` averages 8.9
(see `peak_votes_vs_active_first`).  After halving `cell_size` to
1.5 mm *without* changing the peak finder, ring-1 mean `peak_votes`
dropped 22 % (8.9 → 6.9), confirming **moderate fragmentation**: a
real ring's votes were spreading across 2–3 adjacent cells in the
3 mm grid and the peak finder was reporting only the largest
fragment.  Ring-2 arc-distance mean also tightened 40 %
(0.93 → 0.56 mm) at the finer scale, evidence that the finer grid
*does* localise centres better — once the lost acceptance is
recovered, the localisation gain is kept.

The fix lives in MIST and was shipped in two coupled commits:

**Part A — halve the grid (TOML)**:  `cell_size` and `r_step` move
from 3 mm to 1.5 mm.  8× more accumulator cells (~26 k → ~210 k),
still trivial in memory.

**Part B — sliding-window aggregation (MIST `find_rings`)**:
When scanning the accumulator for the peak, evaluate the sum over a
2×2×2 sub-cell window at every position on the finer grid and
report the maximum-sum position.  Concretely:
`sum_{(i, i+1) × (j, j+1) × (k, k+1)} cell[i,j,k]` scans a
physical 3 × 3 × 3 mm³ volume — identical to the original coarse
cell volume — so thresholds keep their meaning.

Why alignment-free matters: on the coarse 3 mm grid, any real-ring
centre that falls near a cell boundary splits its votes across two
adjacent cells and the single-cell peak reports only half.  The
2×2×2 window on the 1.5 mm grid catches the full cluster at *some*
window position regardless of where the centre falls — guaranteed
because the window slides continuously across the grid.

**Part C — SAT peak finder (MIST `find_peak`)** *(shipped same
session)*: the naive sliding-window scan is O(n_cells × W³).  For
W=2 on the 8× finer grid this is ~64× the legacy single-cell scan,
which produced a measurable run-time increase.  Replaced with a
3-D **Summed-Area-Table (integral image)**:

1. Build a 3-D prefix-sum array `sat` from `accum_` in O(n_cells)
   via three sequential 1-D cumulative-sum passes (along x, y, R).
2. Evaluate each window sum in O(1) via inclusion-exclusion on the
   8 corners of the box:
   ```
   window_sum(iR, iy, ix) =
       sat[iR+1][iy+1][ix+1] − sat[iR ][iy+1][ix+1]
     − sat[iR+1][iy ][ix+1] − sat[iR+1][iy+1][ix ]
     + sat[iR ][iy ][ix+1] + sat[iR ][iy+1][ix ]
     + sat[iR+1][iy ][ix ] − sat[iR ][iy ][ix ]
   ```
   (indices are 1-based exclusive upper bounds; 0 → boundary clamp
   to 0.)

Total peak-finding cost: O(n_cells × 8) — back to ~8× legacy
(matching the voting step) instead of ~64×.  Results are
bit-for-bit identical to the naive scan: all three MIST unit tests
(`test_ransac`, `test_rnd`, `test_logger`) pass unchanged.

`sat_` is a `mutable std::vector<int>` member pre-allocated in
`build_lut`; no heap allocation occurs during `find_peak`.

Thresholds keep their meaning: `peak_votes` is "vote count over a
3³ mm³ physical volume" regardless of W.  No re-tuning of
`[streaming_ransac]` knobs beyond checking that `peak_votes_vs_active_*`
shows the expected lift over the pre-aggregation baseline.

Knob plumbing: `aggregation_window_cells` in MIST (default 1 = legacy
single-cell; 2 = active recipe).  Surfaced to the writer via
`[streaming_ransac].aggregation_window_cells` in `conf/streaming.toml`
and `StreamingRansacConfigStruct::aggregation_window_cells` in
`include/utility/config_reader.h`.

**Part D — tight LUT padding (`centre_padding_mm`)** *(shipped 2026-05-26)*:
After SAT, the dominant per-event cost shifted to memory bandwidth on
the accumulator itself.  On a 1.5 mm grid, `build_lut` was padding
the X/Y bounds by `r_max = 105 mm` on every side — necessary if a
ring centre could land anywhere within `r_max` of a hit, but for a
fixed-radiator Cherenkov the centre is constrained to lie close to
the detector plane.  Most of the accumulator was dead space.

Added an optional `centre_padding_mm` parameter to MIST's `build_lut`
(default `-1.f` → legacy `r_max` for backwards compatibility).  When
set positive, replaces `r_max` as the X/Y pad.  At the standard
`r_max = 105 mm` → `pad = 25 mm` recipe (well bracketing the
observed `ring_X/Y_first_ransac` half-spread of ~10–15 mm), the
accumulator extent shrinks by:

```
nx_legacy ≈ (det_x + 2*105) / cell_size
nx_tight  ≈ (det_x + 2* 25) / cell_size
```

— roughly a ~2× reduction per X/Y axis, ~4× fewer cells total.
Voting cost scales ~linearly in `n_cells × n_R` and the SAT build
scales ~linearly in `n_cells × n_R`, so wall-clock drops in step.

Tuning recipe lives in `conf/streaming.toml` next to the knob:
inspect `ring_X/Y_first_ransac` half-spread, pick ~1.5–2× that, then
verify no QA-hist edge clustering.  Picking too small clips real
rings; the QA hists make this immediately visible.

Knob plumbing: `centre_padding_mm` in MIST → `StreamingRansacConfigStruct`
→ `[streaming_ransac].centre_padding_mm` in `conf/streaming.toml`.
The writer's `streaming_ransac_conf_reader` echoes the loaded value
with a `lut padding:` log line so the active setting is visible at
run start.

#### 2.3.2  Other refinement strategies *(deferred)*

The RANSAC transform was never expected to yield sub-cell centres — that's
not what an accumulator is for.  Beyond 2.3.1, a two-stage approach is the
natural direction:

1. **RANSAC as a candidate finder** (current) — fast, robust, gives an
   approximate centre and a hit-association list.
2. **Sub-cell refinement** on the hits assigned to the candidate ring.
   Options, in increasing order of complexity:

   - **Centroid of the per-hit predicted-centre locus.**  Each hit at
     $(x, y)$ on a ring of radius $R$ has its centre on a circle of
     radius $R$ around $(x, y)$.  The intersection of those circles
     (or, equivalently, the centroid of their intersection points) is
     the true centre.
   - **Least-squares circle fit** on the assigned hits via the existing
     [`fit_circle`](../../utility/circle_fit.h) (see §2.4 for the audit).
   - **Iterative re-association**: refine centre → re-collect hits
     within `collection_radius` → refine again → converge.
   - **MIST's planned `nn_transform`** (NN-based ring finder, listed in
     MIST's `DISCUSSION.md` F-01) for end-to-end refinement.

**Decisions needed before this lands:**
- Are sub-cell ring centres a real analysis bottleneck right now?  (If
  the beam-test resolution is limited by other factors — timing, optics
  — this is parking-lot work.)
- If yes, pick a refinement strategy and benchmark against the
  RANSAC-only centre on synthetic and beam-test data.
- Coordinate with the `fit_circle` audit in §2.4 since the refinement
  step likely calls into the circle fit.

### 2.4  `fit_circle` role and audit  *(retired here — moved to consumer)*

The RANSAC trigger stage no longer calls `fit_circle`.  Centre/radius
refinement happens in `recodata_writer` on the mask-tagged hits — see
[`include/writers/DISCUSSION.md`](../../writers/DISCUSSION.md) for the
remaining audit items (initial-value validation, `fix_XY` granularity,
named-struct return with chi²/ndf/status, round-trip test) which
follow the consumer.  The util header itself
([`include/utility/circle_fit.h`](../../utility/circle_fit.h)) is unchanged.

### 2.5  Open items (gated on extraction + config)

Items that become small focused changes once the RANSAC stage is its
own translation unit with config-driven knobs (Phase 5 of the
consolidation plan):

- ✅ **Time-cut width alignment** — closed by design.  The RANSAC's time
  pre-cut is **inherited** from `[streaming_trigger].time_window_ns`
  (no separate knob).  See § 2.2.
- ✅ **`max_rings` policy** — closed by design.  Hardcoded to 2 because
  the detector has two Cherenkov radiators and no physically realisable
  single-event configuration can produce more than two concentric rings.
  See § 2.2.
- **`fit_circle` init from RANSAC peak.**  *Migrated.* The relevant
  fit now lives in `recodata_writer`; the init-from-RANSAC-peak idea
  applies there.  See
  [`include/writers/DISCUSSION.md`](../../writers/DISCUSSION.md).
  The legacy `[streaming_ransac].fit_circle_init_{x,y,r}` knobs were
  REMOVED 2026-05-30 (commit 87e8af2, CLEAN_OFF C3.5); existing
  configs that still carry them log a one-shot deprecation warning
  per key and are otherwise ignored.
- **RANSAC threshold formula review.**  `hough_threshold = ceil(0.004 ×
  N_active_cherenkov)` (now `hough_threshold_fraction`) was tuned for
  the era when the streaming trigger didn't gate RANSAC entry.  Now that
  stage 1 is selective, stage 2's threshold can be re-derived (or made
  a flat tunable knob).  **Operational observation (2026-Q2):** with
  the current settings the RANSAC is picking up random coincidences as
  rings — the `RANSAC rings/` subfolder in the writer output shows
  centres and radii that already wander far outside the geometry, so
  the long tails in `Fit rings/` are downstream of a bad seed rather
  than a misbehaving fit.  Three knobs interact here and should be
  revisited together: `threshold_fraction` (peak-cell minimum), the
  derived `min_hits = min_active × min_hits_slack` (absolute vote
  floor), and the `min_active` gate itself.  Likely action: raise the
  absolute floor (the relative `threshold_fraction = 0.33` is fine for
  a single isolated ring but collapses to "a third of nothing" when
  the surviving hit count is already low after the time pre-cut).
- **QA refresh.**  Plot `n_σ_streaming` vs `n_rings_found` to expose the
  correlation between the two stages; per-ring radius histograms with
  overlay for the two ring slots; Δt between RANSAC-trigger time and
  streaming-trigger time.
- **Merge near-duplicate rings, then clamp.**  The RANSAC extraction is
  not duplicate-aware: a single physical ring with hits on both sides
  of the `collection_radius` can occasionally surface as two
  near-coincident accumulator peaks (same `(x_c, y_c)` to within a
  cell, same `R` to within `r_step`).  Currently we cap `max_rings = 2`
  *before* any merge, so when this happens the second slot is consumed
  by a duplicate and the *actual* second ring (the other radiator) is
  lost.  Proposal: bump the internal `max_rings` to 3 or 4, then run a
  post-pass that merges rings whose centres are within `~2·cell_size`
  AND whose radii are within `~2·r_step`, then clamp the result back
  to 2.  The merge thresholds become two more `[streaming_ransac]` knobs
  (`merge_dxy_mm`, `merge_dr_mm`).
- **Is "ring 2" an elliptic deformation of ring 1?**  MIST's spatial-
  only ring finder will happily place two circles on a single elliptic
  ring if its eccentricity is moderate — the algorithm has no model
  of ellipses.  Beam misalignment, radiator non-uniformity, or
  off-axis incidence can deform a single Cherenkov ring into an
  ellipse, which MIST would then decompose into "two rings" sharing
  approximately the same centre.

  This is a non-trivial alternative to the conventional
  "dual-radiator → two physical rings" interpretation.  The
  observables that distinguish the two:
   - Per-event `(R_first, R_second)` correlation — co-moving radii
     → shared physics → likely elliptic.  Independent radii →
     genuine two-ring.
   - Per-event `(centre_first, centre_second)` proximity — same
     centre → same ring distorted.  Different centres → two rings.
   - Ellipse fit on the combined hit set vs two-circle fit χ² —
     lower ellipse χ² wins.

  **Refined hypothesis (user, 2026-05-26):** there ARE two physical
  rings, but RING 2 specifically is genuinely an ELLIPSE — MIST
  fits it as a circle because that's the only shape it knows.

  **CONFIRMED REQUIREMENT (user, next campaign):** the *next dataset will
  contain elliptical rings.*  This is no longer a hypothesis to test but an
  upcoming requirement — both the RANSAC finder (circles only) and the
  recodata ring fit (currently a circle + Gaussian-radial fit) will need a
  genuine **ellipse model** `(cx, cy, a, b, θ)`.  The investigation macro
  below is the starting point for that work.

  Macro: `macros/examples/elliptic_investigation.cpp` — **two-layer test:**

  *Layer 1 — correlation cross-check.*  For dual-ring frames,
  re-fit each ring as a circle and fill:
   - `h_R_first_vs_second`, `h_cx_first_vs_second`,
     `h_cy_first_vs_second` (2D correlations)
   - `h_dx`, `h_dy`, `h_dR`, `h_d_centre` (1D residuals)

  *Layer 2 — ellipse fit on ring 2 hits.*  For frames with
  enough ring-2 hits (≥8), fit a 5-parameter ellipse
  `(cx, cy, a, b, θ)` to the ring-2 hit set via inline
  `fit_ellipse` (algebraic distance min, Migrad).  Compare χ² to
  the circle fit's χ² on the SAME hits.  Fill:
   - `h_ellipse_semi_major`, `h_ellipse_semi_minor`,
     `h_ellipse_eccentricity`, `h_ellipse_position_angle`
   - `h_ellipse_a_vs_b` (TH2F, diagonal = circle)
   - `h_chi2_circle_vs_ellipse` (TH2F)
   - `h_chi2_relative_improvement` (TH1F):
     `(χ²_circle − χ²_ellipse) / χ²_circle`

  Run: `root -l 'macros/examples/elliptic_investigation.cpp("<data_repo>", "<run_name>")'`
  after recodata is built.

  *Verdict criteria (printed in the log):*
   - `<eccentricity> ~0` AND `<χ² improvement> ~0` → ring 2 is a
     circle within fit noise, hypothesis rejected.
   - `<eccentricity> > 0.2` AND `<χ² improvement> > 0.3` → ring 2
     is genuinely elliptic, hypothesis supported.
   - Intermediate values → inconclusive; needs higher stats or a
     v2 with per-event significance test (not in V1).

- **Time-aware hit assignment (purely-spatial ring finder).**  MIST's
  `collect_ring_hits` assigns a hit to a ring purely on the spatial
  criterion `|sqrt((x − c_x)² + (y − c_y)²) − R| < collection_radius`.
  The hit's `time` is used only to compute `RingResult::mean_time` as
  an output statistic — it is **not** used in vote-LUT lookup, peak
  finding, or hit-to-ring assignment.  The only time gate in the
  pipeline is the writer-side `±time_window_ns` pre-cut on
  `ring_candidates` (currently 20 ns), applied **before** MIST is
  invoked.  Within that window, hits are equivalent regardless of
  arrival time.

  Consequence: if a single channel fires multiply within the trigger
  window (real photon + correlated afterpulse, prompt + delayed
  Cherenkov, or two genuine photons through the same pixel), all
  duplicates enter `generic_hits` with identical `(x, y, lut_key)`
  and different `time`.  They vote in lockstep, both pass the spatial
  cut for ring 1, and **both get assigned to ring 1** — they cannot
  split across rings.  Two hitmaps will never share these specific
  pixels; but `N_hits per ring` and `peak_votes` are inflated by the
  hit-multiplicity rate, and `mean_time` is pulled toward whichever
  subpopulation dominates.  `arc_dist` is unbiased (duplicates have
  identical `dist`).  Single-photon spatial resolution (σ_r) is
  unaffected because the duplicates have identical positions — they
  pile up at the same arc-distance bin.

  Three places this could be addressed, in increasing order of
  invasiveness:

  1. **Writer-side dedup** (cleanest).  Collapse `(channel, frame)`
     duplicates to a single hit (earliest, or longest-pulse) before
     `generic_hits` is built.  Eliminates the inflation upstream of
     MIST.  Side benefit: tightens DCR rate estimates too.  Knob:
     `[streaming_ransac].dedup_same_channel = true/false`.
  2. **MIST-side time window in `collect_ring_hits`**.  Extend the
     spatial cut to `(|dist − R| < collection_radius) AND
     (|hit.time − ring.mean_time| < δt)` with `δt` configurable.
     Adds a coupled centre / radius / mean-time iteration — more
     complex, requires re-voting after a time refinement pass.  Lets
     downstream uses (DCR rate, time resolution) keep multi-hit info.
  3. **Per-(channel, ring) flag in the writer's tagging loop**.
     Quick hack — track which `(channel, ring_idx)` pairs have
     already been tagged and skip subsequent duplicates.  Cheapest
     mitigation; doesn't touch MIST or the framer.  Knob:
     `[streaming_ransac].tag_first_per_channel = true/false`.

  Suggested order: prototype option 3 first to measure the
  inflation magnitude (entries with-vs-without dedup); if the effect
  is small (<5 %) close as wontfix; if significant promote option 1
  to a config knob and ship.

  **Decision (2026-05-30, CLEAN_OFF C0.3).**  Confirmed: prototype
  option 3 (`tag_first_per_channel`) FIRST as a measurement gate,
  before any full dedup.  Measure-before-build is this codebase's
  established discipline — the same correction the
  `generate_calibration` (P 1.20 → P 0.35) and `get_phase` (P 1.71)
  estimates needed, both off by an order of magnitude until
  instrumented.  Cluster C7 opens with the prototype: add the
  one-bit-per-channel guard, run on a ring-rich run (20251119-040419
  has all three trigger kinds firing), read the inflation off
  `full_hitmap` before/after.  Only if the fraction is significant
  (>5 %) does option 1 (writer-side dedup config knob) get built; if
  small, close as wontfix with the prototype shipped as-is.

- **Per-ring (X, Y, R) sanity cuts.**  Both `RANSAC rings/` and
  `Fit rings/` subfolders show long tails to unphysical values on
  background events.  The 2026-Q2 split into seed/refined hists
  (writer output) confirmed that the bad tails enter at the *RANSAC*
  stage — the fit then dutifully refines around a nonsense seed.
  Three independent guards apply:  (i) reject rings whose RANSAC peak
  votes / `min_hits` ratio is below a configurable floor (stage 2
  quality — this is the **primary** lever given the diagnosis above);
  (ii) reject fits whose `(x_c, y_c)` falls outside an acceptance box
  around the active geometry, and whose `R` falls outside `[r_min,
  r_max]`; (iii) reject fits returning a NaN sentinel (currently
  silently filling the QA hists with NaN — `fit_circle` already
  returns one on failure).  All three want `[streaming_ransac]` knobs;
  the QA hists should also gain pre-cut / post-cut variants so the
  rejection power is auditable.

### 2.6  Live-QA pipeline (recodata-side, downstream of RANSAC)

**Status:**  V1 **shipped**.  Foundation helper module
(`utility/radiator_efficiency`), `[recodata]` config block (struct +
parser + TOML + values-dump log), `recodata_writer` wiring (coverage
map at init, per-frame ring fit + fills, `eff(R)` division
at finalize) all in place.  Output ROOT file gains a `Rings/`
subfolder with the 10 hists listed under "V1 scope" below.

**Ring reconstruction is no longer RANSAC-gated.**  The streaming/RANSAC
self-trigger is disabled in QA mode, so the frame never carries
`_TRIGGER_RANSAC_RING_FOUND_` and there are no RANSAC-tagged hits to refit.
Instead, recodata reconstructs the ring from every **non-afterpulse
Cherenkov hit within an asymmetric ±Δt window around the *hardware*
trigger's reference time** — see `compute_ring_fit_timewindow` (in
`writers/recodata/ring_compute.{h,cxx}`), driven by the
`hardware_ring_dt_min_ns` / `hardware_ring_dt_max_ns` knobs in
`conf/working/recodata.toml`.  The reference time is the first genuine
hardware trigger in the frame (synthetic + derived ring markers skipped;
see `frame_pipeline.cxx`).  The histogram fill gate is
`res.first.fit_ok || res.second.fit_ok` (fill only on a successful fit).
With no RANSAC there is no first/second ring separation, so the
time-window fit is the single reconstructed ring (second stays empty).
The old RANSAC-mask helpers `compute_ring_fit` + `refit_and_fill_ring`
have been removed; only `compute_ring_fit_timewindow` + `fill_ring_hists`
remain in `ring_compute.{h,cxx}`.  The historical RANSAC-tagged-hits fit
(`fit_circle` + `is_ring_tagged`) was retired with the QA-mode time-window
reconstruction and is no longer kept as a standalone macro.

Deferred items remain in the "post-V1" list at the bottom of this
section.  This section is the recovery-handoff document for those —
anyone picking up the next slice should read § 2.6 + the helper's
docstrings + `recodata_writer.cxx` § "Live-QA radiator pipeline" and
be able to continue.

**What it is.**  Per-event photon-counting and radial-distribution QA
that runs inline in `recodata_writer` instead of as a separate offline
macro pass.  Goal: live-visible Cherenkov physics observables for
beam-test operators (the macro `photon_number_new.cpp`
becomes a thin plotter, or goes away).

**Why recodata, not lightdata.**  Architectural split:

| Writer | Job |
|---|---|
| `lightdata_writer` | Pre-scan: framing, trigger finding (streaming + RANSAC), per-hit mask-tagging.  Already done. |
| `recodata_writer` | Physics analysis: time-window cuts, radial distributions, efficiency, photon counting.  Currently a thin passthrough; this work makes it the analysis stage. |

The macro's API usage (`recodata->get_hit_phi(ihit, centre)`,
`recodata->is_afterpulse(ihit)`, `recodata->get_trigger_by_index(...)`)
maps 1:1 to recodata's existing interface — porting the macro's body
into recodata is a more direct fit than into lightdata.

**Architectural decision: no `TriggerEvent` schema bump.**  The
attractive-looking option of extending the RANSAC trigger payload to
carry `(cx, cy, R, peak_votes)` per ring would have touched every
trigger consumer in the codebase and bumped the ROOT TBranch schema
on `lightdata`.  Avoided.  (Historically, lightdata mask-tagged hits
with `HitmaskRansacRingTagFirst / Second` and recodata re-ran `fit_circle`
per RANSAC-tagged ring at consumption time.  With the RANSAC self-trigger
disabled in QA mode that path is dormant — recodata now fits the ring
from non-afterpulse Cherenkov hits in a ±Δt window around the hardware
trigger instead; see the status block above.)  Either way the per-event
`(cx, cy, R)` is recovered at consumption time, so no schema bump.
Cost: ~50 µs per ring × ~21 k events = **< 1 s wall-clock per run** —
well below the budget.

**Centre conventions** (`config_reader.h`'s `RecodataConfigStruct`):

- **`eff(R)`** is computed at FINALIZE from a coverage map built
  around a *fixed nominal centre* (typically `(0, 0)` or the
  beam-axis projection — TOML knob).  Matches the offline macro's
  convention exactly, so existing analyses remain comparable.
- **Per-hit `(R, φ)`** uses the *per-event fit-refined centre* from
  the `fit_circle` re-run above.  Per-event radial coordinate is
  correct even when the RANSAC centre wanders by ~10–25 mm RMS;
  `eff(R)` discrepancy is <1 % at typical ring radii (35–105 mm)
  and judged acceptable for V1.

**Spill-by-spill active-channel weighting (shipped — matches offline macro):**

The coverage map is **NOT** the geometric upper bound.  Per-spill
bookkeeping mirrors the offline `photon_number_new.cpp` macro's
recipe exactly:

- **Active-channel mask**: populated from `spilldata->get_not_dead_participants()`
  during the StartOfSpill-marker construction at the top of each
  spill loop.  Same source the macro reads via `is_start_of_spill()`
  frames (which carry one synthetic hit per participating channel).
- **Physics counter**: counts frames carrying any accepted trigger
  except the bookkeeping/sampling sentinels (TriggerFirstFrames,
  _TRIGGER_STREAMING_RING_FOUND_, TriggerStartOfSpill,
  _TRIGGER_UNKNOWN_).  In practice this counts beam-defining
  triggers (luca_and_finger, broad_scintillator, TIMING, TRACKING)
  + downstream-physics triggers (RING_FOUND, RANSAC_RING_FOUND).
  Replaces the earlier macro-aligned `STREAMING_RING_FOUND` choice
  because the streaming trigger is itself an internal /
  pre-selection sampler and shouldn't drive the physics-event
  normalisation (the macro's choice was historical; this
  predicate is closer to the beam-test operator's mental model).

At finalize:

    spill_weight[s] = n_streaming_per_spill[s] / Σ n_streaming
    channel_weight[k] = Σ over spills active in of spill_weight[s]

Channels in the geometry but never active in any spill get weight 0
— silent dead-channel mask.  Channels in all spills get weight 1.
Intermediate channels get a fractional weight reflecting their
streaming-rate-weighted duty cycle.

Result: `eff(R)` reflects the actually-delivered acceptance, not
the geometric upper bound.  `N_photons = N_hits / f_coverage`
becomes physics-honest even with dead channels.  Directly
comparable to the offline macro's eff(R) and N_photons.

Fallback: if total_streaming = 0 (background-only run, or all
streaming triggers rejected at edge/duplicate), the writer falls
back to the geometric-upper-bound build and logs a warning — same
observable as the V1-pre-spill-weighting behaviour.

**Helper module: `utility/radiator_efficiency`.**  Pure-geometry helpers,
no dependency on the recodata API:

| Function | Returns | Purpose |
|---|---|---|
| `build_coverage_map(channel_xy, n_phi, r_min, r_max, n_r, half_width, cx, cy, min_r, channel_weights)` | `TH2F*(φ, R)` | Iterates (φ, R) bins in each channel's bounding box and increments each bin whose centre (projected to x, y) lies inside the pixel.  Per-channel increment is `channel_weights[key]` (if supplied) or +1 (if `nullptr`).  Channels NOT in `channel_weights` are silently skipped when the map is non-null — functions as a dead-channel mask.  Convention: `coverage_map[φ, R] = Σ over active channels of weight × (channel covers this bin)`.  Matches the offline `photon_number_new.cpp` macro exactly so `eff(R)` values are directly comparable.  *Speckled-hole artifact (bin centres just outside the pixel boundary) is cosmetic; does not affect eff(R) after φ-averaging.* |
| `radial_efficiency(coverage_map, axis)` | `TH1F* eff(R)` | Collapse to 1D by averaging over φ.  No φ-gap split in V1 (see "V1 scope" below). |
| `azimuthal_coverage_fraction(channel_xy, cx, cy, R, delta_r)` | `float ∈ [0,1]` | Per-ring scalar.  Iterates channels, accumulates φ-segments within `±delta_r` of the arc, merges overlaps, returns fraction of `2π`. |

The macro's `phi_extrapolation_scale` is **not** ported in V1 (it's a
gap-correction factor only needed when the radial hist excludes
φ-gap regions).

**V1 scope (live-ready set — 18 hists total in `Rings/`):**

*Engineering / sanity (geometry-only, static):*
- ✅ Coverage map `h_coverage_map_rphi` (1 TH2F) + cartesian sibling
  `h_coverage_map_xy` (the operator-facing view + detector-readiness banner)
- ✅ `eff(R)` `h_eff_R` (1 TH1F)

*Per-ring physics observables (the headline numbers operators read):*
- ✅ Fitted ring radius `h_R_first/_second` (2 TH1F)
- ✅ Single-photon σ per ring `h_sigma_first/_second` (2 TH1F)
- ✅ `N_hits` per ring `h_nhits_first/_second` (2 TH1F)
- ✅ `N_photons` (eff-corrected) `h_nphotons_first/_second` (2 TH1F)
- ✅ Ring centre map `h_centre_xy_first/_second` (2 TH2F)
- ✅ Radius vs hits `h_R_vs_nhits_first/_second` (2 TH2F)
- ✅ Per-hit LOO residual vs N_hits `h_residual_vs_n_first/_second`
  (2 TH2F).  For each hit, refit with that hit *excluded* and
  measure its residual against the leave-one-out fit:
  `Δr_i = r_hit_i − R_-i`.  Hit i didn't participate in the fit it's
  measured against → **unbiased** σ_photon.  Supersedes the earlier
  `h_fit_sigma_R_vs_n_*` attempt (wrong observable: that was the LM
  Hessian σ_R, not the per-hit width).
- ✅ Dual/solo split for first-ring vs_n hists
  `h_R_vs_nhits_first_{dual,solo}` and
  `h_residual_vs_n_first_{dual,solo}` (4 TH2F).  Same predicate as
  the lightdata-side splits (frame has a second ring tagged?).
  Lets you A/B σ_photon between clean two-radiator events (dual)
  and potentially fake-contaminated single-ring events (solo).
- ✅ Slice-fit recipe at finalize.  For every vs_n TH2F:
  1. `TH2::FitSlicesY` → per-X-slice Gaussian → σ-per-slice as a
     side-product hist (`<name>_2`).
  2. Fit the σ(N) hist with `TF1("...", "sqrt([0]/x + [1])", 5, 40)`:
     σ²(N) = A/N + B.  Output TF1 stored next to the parent TH2F.
     - For `h_residual_vs_n_*`: B = σ_photon² (intrinsic single-hit
       resolution²), A = leftover statistical fluctuation.
     - For `h_R_vs_nhits_*`: B = irreducible ring-radius spread²
       (e.g. multi-ring physics), A = per-event statistical R
       uncertainty contribution.
  3. **Extract σ_photon = √B as a labeled `TNamed` scalar**
     `<name>_sigma_photon_mm` written alongside the fit, with
     value `"σ ± δσ"` (propagating the fit's uncertainty on B).
     Also echoed to the run log so the operator sees the
     single-photon resolution number in realtime.

  This is the physics recipe: per-N Gaussian slice fits → σ(N)
  curve → σ_photon as the asymptotic floor.  Matches the user-stated
  procedure ("slices Gaussian, take the resolution against
  n_participants, get the single photon resolution") exactly.

- ✅ Crystal-Ball + pol3 fit on the eff-corrected radial
  distribution (`h_radial_first/_second/_first_dual/_first_solo`),
  ported from `photon_number_new.cpp`'s
  `fit_radial_distribution` lambda.  Two-stage procedure:
  1. **Sideband prefit**: clone the hist, mask signal region
     (peak ± few σ), fit pol3 background on the remaining bins.
  2. **Combined fit**: 9-parameter `CB(amp, μ, σ, α, n) + pol3(c0..c3)`,
     pol3 parameters initialised from step 1, frozen for first
     iteration (lets CB find the peak), released for second.

  Outputs per radial hist (4 total: full + second + dual + solo):
  - `<name>_ring_fit`          — TF1, full Gaussian+pol3 form
  - `<name>_bg_only`           — TF1, pol3 background only (for overlay)
  - `<name>_N_gamma_per_ring`  — TNamed, photons per Cherenkov ring
  - `<name>_N_gamma_total`     — TNamed, total photons "X.X (over N rings)"
  - `<name>_peak_mu_mm`        — TNamed, peak position ± error
  - `<name>_peak_sigma_mm`     — TNamed, peak (Gaussian) σ ± error

  N_γ extraction:
    `N_gamma_total = (∫ ring_fit − ∫ bg_only) over [fit_lo, fit_hi]`
                       `÷ bin_width_of_hist`
    `N_gamma_per_ring = N_gamma_total / N_rings`
  where `N_rings = entries of the matching h_R_* hist` (one entry
  per accepted ring fit).  The canonical "Cherenkov photons per
  ring" observable is the per-ring value — what beam-test operators
  read off the live plots.

  *Bug fix 2026-05-26*: earlier version used
  `bin_width = (fit_hi - fit_lo) / hist.GetNbinsX()` which mixed
  the fit range with the hist's full-range bin count → 1/(90/80) ÷
  1/(100/80) = 1.11× over-count.  Now uses `hist.GetXaxis()->
  GetBinWidth(1)` directly.

  Run-log echo: each fit logs
  `N_gamma=...  peak_mu=... ± ... mm  peak_sigma=... ± ... mm`
  so operators see the headline numbers live.

  Caveats: skips silently with a warning if `< 100` entries (too
  few to fit).  Peak σ is bounded to `[0.3, 5.0]` mm to keep the
  LM from collapsing to delta-function fits in noisy samples.

**Persistent summary QA plots (the "QA that lasts"):**

Per-ring TNamed scalars + run-log lines are good for live read-out
but not visually comparable.  Five bin-labeled TH1Fs collect the
headline numbers across all ring slots in a single TBrowser plot:

| Hist | Source | Y axis |
|---|---|---|
| `h_sigma_photon_summary` | `h_residual_vs_n_*` LOO + √(A/N + B) | σ_photon ± err (mm) |
| `h_sigma_R_intrinsic_summary` | `h_R_vs_nhits_*` LOO + √(A/N + B) | σ_R_intrinsic ± err (mm) |
| `h_N_gamma_summary` | CB+pol3 on radial hists | N_γ |
| `h_peak_mu_summary` | CB+pol3 on radial hists | μ ± err (mm) |
| `h_peak_sigma_summary` | CB+pol3 on radial hists | σ_peak ± err (mm) |

Each up to 4 bins (first / second / first_dual / first_solo).
Operators eyeball these for at-a-glance summaries; downstream
scripts read bin contents programmatically (no TNamed string
parsing).

*Diagnostics (when something looks wrong, look here first):*
- ✅ `f_coverage` per ring `h_f_coverage_first/_second` (2 TH1F)
- ✅ Radial hit distribution (eff-corrected) `h_radial_first/_second` (2 TH1F)

*Trigger gate and edge rejection (recodata_writer):*

- Ring fills are gated on `res.first.fit_ok || res.second.fit_ok` — a
  successful ring fit, not a trigger count.  (The original V1 design
  gated on `accepted_triggers.count(_TRIGGER_RANSAC_RING_FOUND_)`, but the
  RANSAC self-trigger is disabled in QA mode; the ring is now fit from
  non-afterpulse Cherenkov hits in a ±Δt window around the hardware
  trigger reference time — see § 2.6.)
- Edge rejection in recodata_writer was previously a hidden units
  mismatch: `current_trigger.fine_time` (ns) compared against
  `edge_rejection_cc` (clock cycles, ~4 cc) and
  `framer_cfg.frame_size - edge_rejection_cc` (mixed units).  Almost
  every trigger ended up edge-rejected.  Fixed by switching both
  sides of the inequality to ns:  `edge_rejection_ns = 25.f`,
  `frame_size_ns = framer_cfg.frame_size * BTANA_ALCOR_CC_TO_NS`.

*Reading guide for live operators (TBrowser glance order):*
1. `h_R_first` — Cherenkov ring radius peak position is the run quality marker.
2. `h_nphotons_first` — photoelectron yield distribution.
3. `h_residual_vs_n_first` — σ_photon via Gaussian-fit of each N
   slice (unbiased, LOO).  Cross-check against `h_sigma_first`,
   which is biased low by fit-self-consistency (the fit minimises
   the very residuals that `h_sigma_*` measures, on the same hits).
4. `h_centre_xy_first` — beam centre + drift over the run.
5. Drop to diagnostics if any of the above look wrong.

*Deferred (post-V1 finer analysis):*
- ❌ φ-gap (in_gap / ex_gap) split — finer-analysis follow-up
- ❌ Sensor-model split (k1350 / k1375) — finer-analysis follow-up
- ❌ Multiple time windows (prompt / early / DCR) — finer-analysis
- ❌ N_γ fit (Gaussian-on-CB) — stays in the macro for V1

**TOML knobs — shipped in `conf/recodata.toml`:**

| Knob | Default | Role |
|---|---|---|
| `delta_r_for_coverage_mm` | 3.0 | Channel-on-arc bandwidth for `azimuthal_coverage_fraction`. |
| `n_phi_bins_coverage` | 360 | Coverage map azimuthal binning (1°/bin). |
| `n_r_bins_coverage` | 80 | Coverage map radial binning. |
| `r_min_coverage_mm` | 25.0 | Coverage map R lower edge. |
| `r_max_coverage_mm` | 125.0 | Coverage map R upper edge. |
| `channel_half_width_mm` | 1.5 | Channel pixel half-side (3 mm pitch → 1.5). |
| `nominal_centre_x_mm` | 0.0 | Fixed centre for coverage / `eff(R)`. |
| `nominal_centre_y_mm` | 0.0 | Same, Y. |
| `min_hits_per_ring` | 5 | Floor below which a ring is skipped (re-fit + fills). |
| `min_channel_r_for_coverage_mm` | 0.0 | Skip channels with `r_channel <` this when building the coverage map.  Default 0 = no filter.  Set above the "low-R bump" R if the coverage map shows artifacts from bogus `Mapping` positions; diagnostic via the 10-smallest-r channel log at `recodata_writer` startup. |

Parser: `recodata_conf_reader` in `src/config_reader.cxx` echoes
loaded values at startup (grep-able fixed-format `mist::logger::info`
lines) — same "did my edit take effect?" diagnostic as
`streaming_ransac_conf_reader`.

**Deferred items (post-V1):**

- Ring → radiator labelling.  V1 names hists by ring **slot**
  (`_first` / `_second`) — the MIST sort-by-`peak_votes` order, not
  the physical radiator.  Operators map mentally from beam-test
  knowledge.  The correct abstraction is to predict a ring radius
  per radiator from `R = depth × √(n² − 1/β²)` using
  `RadiatorInfoStruct` + per-run beam metadata (energy, particle),
  then match found rings to predicted radii.  Skipped for V1
  because:
  - per-run beam metadata isn't wired into recodata yet, and
  - the intermediate "per-run TOML mapping `_first` → radiator name"
    is a *lie* whenever the MIST sort flips slots on noisy events
    (contaminates the hists silently) — A → C directly when beam
    metadata is wired, skipping B.

- φ-gap split radial hists (`*_in_gap` / `*_ex_gap`), reusing the
  macro's `kPhiGapRanges` once the detector geometry is finalised.
- Sensor-model splits (k1350 / k1375 in the macro).  Needs a
  device → sensor mapping in `Mapping`.
- Multiple time windows (prompt / early / DCR).  Needs recodata's
  existing `is_afterpulse` + per-trigger Δt access.
- Per-event coverage map (currently fixed-centre) — only if the
  ~1 % `eff(R)` discrepancy from centre wander becomes a problem.
- N_γ fit in the writer's finalize step — only if the offline fit
  script is judged too cumbersome for live use.
- Multithreading: per-spill serial, frames-within-spill parallel
  (see open item in § 2.7).  Targets ~3–4× speedup; sequenced
  *after* V1 lands so we can measure the actual hot spot.

### 2.7  Multithreading (post-framer, frames-within-spill)  *(shipped 2026-05-26)*

**Status:** SHIPPED.  Three-stage refactor in `recodata_writer.cxx`:
1A — split `refit_and_fill_ring` into `compute_ring_fit_pure` +
`fill_ring_hists` (compute / drain separation for the heaviest
per-frame work).
1B — extracted the whole per-frame body into `process_frame_pure`
returning a `FrameResult`, plus `drain_frame_result` consuming it.
2 — `std::async`-based parallel dispatch with per-frame slots in
`std::vector<FrameResult>` (no contention) + serial drain in
frame order to preserve recodata.root write ordering + histogram
fill ordering.  Pattern mirrors
`parallel_streaming_framer.cxx::next_spill`.

**Model.**  Spills processed serially; within each spill, frame
processing dispatched to a thread pool.  Framer prepares all frames
(already serial), workers consume frames in parallel, `wait_all()`
before writing the spill output sequentially.  No cross-spill
reordering buffer needed — the output is always written after all
of its spill's frames are done.

**Per-thread state isolation:**

- `HoughTransform` instance — LUT is read-only after `build_lut`, but
  the accumulator is mutable.  Per-thread instance (sharing the LUT
  via `const` ref) is the cleanest solution.  Memory cost:
  `N_threads × accumulator_size` ≈ `N × 5 MB` at the tight padding.
- QA histograms — per-thread `Clone()` at thread spawn, `Add()` into
  the canonical hist at `wait_all()`.  ROOT idiom.
- `spilldata.add_trigger_to_frame(frame_id, ...)` — keyed by
  `frame_id`, each thread writes to its own slot, no contention.
- DCR rates / channel-position maps — read-only after init, share
  via `const` ref.

**Expected gain.**  Current ~450 s lightdata wall-clock with the SAT
+ centre_padding stack is voting-dominated.  Voting is
embarrassingly parallel per active hit.  On an 8-core box: target
~60–80 s wall-clock.

**The fiddly bits, honestly.**

- ROOT histograms aren't thread-safe; the per-thread clone / merge
  pattern works but every new QA hist added going forward needs to
  follow the same pattern, easy to forget.
- `Mapping`'s position cache uses a `shared_mutex`; check it's
  read-only on the workers.
- `RootHist` wrapper — needs to support thread-local clones.  May
  need a small extension.

**Why deferred.**  V1 live QA gives operators a useful observable
even at current wall-clock; multithreading is a comfort
improvement, not a blocker.  Once V1 is exercised by real
beam-test data the actual hot spot is observable (it may not be
where the cost model suggests), so we'd parallelise the right
code, not the predicted-right code.

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

Tracked separately from this design; revisit once the RANSAC stage lands
(Phases 2-4) and the Cherenkov pipeline is stable.


*Implemented*: § 1 score stage (D-12, v1 landed); § 2.1–§ 2.4 RANSAC
stage extraction (Phases 1–4); § 2.3.1 sub-cell aggregation + SAT
peak finder + tight LUT padding (MIST patches shipped); § 2.6 V1
live-QA pipeline — `utility/radiator_efficiency` helper, `[recodata]`
config block, `recodata_writer` wiring (coverage map / N_photons /
`eff(R)` / radial fills), output ROOT `Rings/` subfolder.

*Queued*: § 2.7 frames-within-spill multithreading (post-V1
optimisation); § 2.6 finer-analysis follow-ups (φ-gap / sensor /
time-window splits, per-event eff(R), in-writer N_γ fit); § 2.5
open items (time-aware ring assignment task #33, fit_circle init
from RANSAC peak, threshold formula review, merge pass for
duplicate rings).*

---

## Design decisions captured 2026-05-29

A Q&A pass against the BACKLOG resolved the following open points.
Treat them as committed unless a follow-up Q reverses them.

### RANSAC — merge near-duplicate rings (before clamp to `max_rings=2`)

**Criterion:** two found rings collapse to one iff `|Δc_x|`, `|Δc_y|`,
and `|ΔR|` are all within one accumulator cell on the respective axis
(i.e. they would have voted into the same RANSAC bin had the LUT
discretisation been one cell coarser).  Cheaper than per-hit overlap
fraction and aligns naturally with the RANSAC cell size already in
config — no new tolerance knob.

### RANSAC — per-ring (X, Y, R) sanity cuts

Three independent guards, applied **after** find_rings returns and
**before** the clamp / mask write-back:

1. **NaN guard** — already in place (`hough.cxx:222` defensive check on
   mean-time / centre-fit divisions).  Keep.
2. **Geometry box** — ring centre `(c_x, c_y)` must fall inside the
   *physical detector envelope* (sensor-plane bounding box from
   `mapping`) plus a configurable margin.  Rings centred off-detector
   are spurious — either RANSAC peak collisions or noise locks.
3. **Quality-ratio floor** — `peak_votes / N_active` must clear a
   minimum; rings that "win" their cell with almost no support are
   noise.  Threshold lives in `streaming_ransac_cfg`.

### RANSAC — is "ring 2" an elliptic deformation of ring 1?

`macros/examples/elliptic_investigation.cpp` V1 is ready as-is — the
task is to **run + measure**, not extend code first.  Expected outcome
is informative either way: if eccentricity tracks beam impact angle,
the "two-rings" model is wrong and we should fit an ellipse instead;
if not, ring-2 is a genuine second Cherenkov ring (different radiator
/ different velocity).  Decision rule: run on three different beam
angles, check eccentricity correlation.

### DCR estimator (Streaming v2)

The conservative 75th-percentile estimator is **not** pre-committed
against median+MAD or other robust alternatives.  Pick on evidence:
run both on a representative noise-dominated dataset, compare
stability across runs, pick whichever has lower run-to-run variance
on the gated trigger rate.

### RANSAC threshold — make it DCR-dependent (replace fixed `hough_threshold_fraction`)

**Decided (design, deferred impl):** the stage-2 RANSAC acceptance floor
should scale with the **dark count rate**, not with channel occupancy.

Today `hough_min_active = ceil(hough_threshold_fraction × N_active_cherenkov)`
([lightdata_writer.cxx]) — a fixed fraction of the *channel count*, blind to
DCR.  This is the lone fixed-fraction holdout: stage 1 already gates on
`n_σ = (S − E[S])/σ_S` over the first-frames noise model, so stage 2 is
inconsistent.  In a noisy run (e.g. 20260614-132826, σ_S ≈ 3161) the fixed
fraction sits *below* the random-coincidence floor → a flood of solo "rings"
from DCR coincidences.  Empirically a constant bump can't win: raising the
floor enough to kill the DCR junk also halves the real (dual) ring yield,
while the solo population stays noise-dominated — the classic symptom of a
threshold that must be data-adaptive.

The machinery already exists (`score.h`): per-channel `m_c` (expected DCR
hits/window from the first-frames sample), `E[S] = Σ m_c·w_c`,
`σ_S = √Σ 1/m_c`.  The expected dark hits per window is `Σ_c m_c` — one extra
sum over the same bundle.  Sketch:

```
E_dark      = Σ_c m_c                         # expected DCR hits in the window
min_active  = max(abs_floor, E_dark + k·√E_dark)   # Poisson n_σ over the dark floor
```

i.e. a real ring must exceed the DCR expectation by `k` sigma (mirrors stage 1's
`n_σ`).  Validate across regimes (streaming-noisy σ_S≳5000 vs dense) and confirm
the dual-ring yield is preserved while the solo-junk collapses.

**Status update (v2.2.0 — Hough→RANSAC migration).**  Stage 2 is now the
grid-free RANSAC finder, and it ALREADY carries a significance gate
(`ransac_min_significance`): a candidate's weighted inlier EXCESS over the
expected background must exceed `min_significance·√(expected)`.  That is the
"n_σ over a background floor" idea this task asked for — but the background is
currently estimated as a UNIFORM areal density over the sensor fiducial
(`ρ·L·2·band`, see `ransac_ring_finder.h`), NOT the per-channel DCR model
`E_dark = Σ_c m_c`.  So the data-adaptive floor is *half-built*: the gate shape
is right, the background estimate is uniform rather than DCR-weighted.

To finish it: `score.{h,cxx}` already computes and publishes
`expected_dark_hits_per_window = Σ_c m_c` (added during the migration, **currently
unused**) — plumb it (and/or the per-channel `1/m_c` weights, already passed to
the finder) into the RANSAC background term so the significance gate is taken
over the true per-window dark expectation instead of a flat density.  No new
`hough_*` knob: the live gate is `ransac_min_significance`.

The old Hough-era stopgap knobs (`hough_threshold_fraction`,
`min_ring_votes_floor`) are now IGNORED (vestigial, flagged in
`conf/*/streaming.toml`).  Current acceptance is `ransac_min_significance` +
`min_inliers` + the recodata `arc_span_min_rad = 0.3` (relaxed from 0.8 so the
~36° far arcs survive — 0.8 was rejecting exactly the arcs we want).

### Time-aware hit handling (task #33)

**Decided:** per-trigger Δt cut applied **at the writer** (lightdata)
plus deduplication inside the RANSAC stage.  Rationale: the writer
cut gives the RANSAC a cleaner sample (fewer DCR hits voting into
cells); the RANSAC dedup catches the residual within-window doubles
that the cut still admits.  Single-cut variants (cut only, dedup
only) explicitly rejected — neither alone covers both failure modes.

### Dynamic timing cuts in lightdata

**Decided:** operator-set per campaign via `streaming.toml` knobs.
*Not* per-spill fit-and-cut (avoids feedback loops where a spill with
unusual physics drifts the cut and biases the next spill); *not* a
per-run autofit (intra-campaign stability assumed).  Keeps the
control surface deterministic and reviewable.

### Recodata multiple time windows (QA)

**Decided:** trigger-aligned windows — `±Δt around trigger`,
`side-bands` (random-coincidence baseline), and `DCR` (out-of-spill or
far-from-trigger reference).  *Not* prompt/early/DCR as the BACKLOG
row originally suggested; the trigger-relative split is more useful
because it lets us separate "in-time signal" from "in-spill random"
from "noise floor" cleanly.
