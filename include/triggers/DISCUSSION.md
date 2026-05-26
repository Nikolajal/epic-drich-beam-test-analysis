# Trigger subsystem — design notes

This document is the **community-facing** design reference for the trigger
subsystem.  It lives in the source tree (git-tracked) alongside the code it
describes.  The project-root `DISCUSSION.md` is local-only — anything the
team should be able to refer to about trigger design lives **here**.

> **Audience:**  someone joining the project who needs to understand how the
> trigger logic is structured before touching it.  Self-contained; assumes
> only the README's level of familiarity with the ALCOR readout.

---

## 1.  Two kinds of trigger configuration

Every entry in `conf/trigger_conf.<year>.toml` is exactly one of two shapes —
nothing in between.  The two-mode model replaces a previous "scoring + wildcard"
scheme that allowed ambiguous configurations and silent failures.

### 1.1  Device-mode

```toml
[[trigger]]
name   = "luca_and_finger"
index  = 0
device = 196
delay  = 117
```

The hardware itself tags the data word as a trigger
(`AlcorData::is_trigger_tag() == true`).  The configuration exists only to
attach a human-readable name, the logical trigger index, and the delay
correction.  **At most one device-mode entry per `device`** — the device is
the discriminator.

A tagged word from a device with no matching configuration emits a
`_TRIGGER_UNKNOWN_` event (and the framer logs the device once for
deduplication).

### 1.2  Channel-mode

```toml
[[trigger]]
name   = "finger_chip1"
index  = 2
device = 196
fifo   = 5
column = 3        # canonical channel position
pixel  = 0
delay  = 80
```

A normal data channel is **forced** into the trigger path.  A word arrives
tagged as a regular ALCOR hit; the framer checks the
`(device, fifo, column, pixel)` key, and if it matches, emits a
`TriggerEvent` instead of storing the word as a data hit.

**One per `(device, fifo, column, pixel)` tuple.**

The channel position can equivalently be given as a single `eo_channel`
integer in `[0, 31]`:

```toml
eo_channel = 12         # = column * 4 + pixel
```

Both forms together are accepted only if internally consistent.

### 1.3  What used to be valid and is no longer

The previous schema allowed:
- `chip` as a standalone selector (matching all four FIFOs of a chip)
- `use_hit = true` to route a tagged trigger word back into the data-hit path
- Partial channel specs (`column` without `pixel`, etc.)

All of these are now rejected at config-read time with a deprecation warning
or hard error.  The two-mode model is the **only** valid schema.

### 1.4  Why the two-mode split

Specificity scoring (more fields specified → wins the match) sounds harmless
but admits silent ambiguities.  Two configurations could match the same
physical channel with the same score, and the first-by-declaration-order
silently won.  Worse, "intermediate" specificities like `chip`-only had no
physical correspondence — they didn't map to a real hardware boundary.

By collapsing to exactly two semantic modes — `device` (hardware-tagged)
and `device + fifo + channel` (data-tagged, forced) — every entry has a
well-defined meaning and the framer uses O(1) hash lookups in either case.
Duplicates are detected at load time.

### 1.5  Implementation

Two structs, two hash maps, two lookup paths:

| Type | Key | Lookup site in [`parallel_streaming_framer.cxx`](../../src/parallel_streaming_framer.cxx) |
|---|---|---|
| `DeviceTrigger` | `device` | `is_trigger_tag()` branch |
| `ChannelTrigger` | packed `(device, fifo, column, pixel)` | ALCOR-Hit branch (checked **before** normal hit processing) |

Both maps live inside `TriggerConfigSet`, produced by
[`trigger_conf_reader()`](../triggers.h) and stored as a single member of
`ParallelStreamingFramer`.

---

## 2.  The streaming trigger

A separate, **algorithmic** trigger that runs per-frame on the framer's
output: looks for time-clusters of Cherenkov hits in a sliding window and
emits a `_TRIGGER_STREAMING_RING_FOUND_` event when a cluster crosses
threshold.  Frames without a streaming trigger are dropped by the lightdata
writer — the streaming trigger is the **pre-filter** for the Hough
ring-finding stage downstream.

> **Status as of 2026-05-26:**  v1 shipped.  Lives in
> [`triggers/streaming.{h,cxx}`](streaming.h) — extracted out of
> `lightdata_writer.cxx` and rewritten around DCR-weighted scoring and
> $n_\sigma$ thresholding.  The TOML knobs are in `conf/framer_conf.toml`
> under `[streaming_trigger]`.  The v0 unweighted entry point
> (`run_streaming_trigger`) is kept alongside the v1 entry point
> (`run_streaming_trigger_weighted`) for reference and migration paths;
> the lightdata writer now calls v1 exclusively.

### 2.1  Why weighted counting

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
strong evidence; noisy channels barely contribute.  The "all channels equal"
assumption is replaced by a measurement.

We don't have a per-channel signal model (knowing which channels are on the
ring is the downstream problem the trigger feeds) so signal-aware weighting
$w_i = s_i / \lambda_i$ is deferred — see §2.5.

### 2.2  Reading DCR

The streaming trigger consumes the existing per-channel DCR profile that
the lightdata writer already maintains for its own QA:

| Histogram | Filled where | Content |
|---|---|---|
| `h_dcr_per_channel` (TProfile) | [`src/lightdata_writer.cxx`](../../src/lightdata_writer.cxx) — DCR-QA fill site, conditional on `TriggerFirstFrames` being present on the frame | `TProfile::Fill(channel_ordinal, per_frame_hit_count)`: bin content is the **mean** per-frame hit count over all noise frames processed so far |

Because TProfile stores the per-bin mean internally, the per-channel rate
collapses to:

$$\lambda_c \;=\; \frac{\langle N_{\mathrm{hits/frame}}(c)\rangle}{T_{\mathrm{frame}}}$$

with $T_{\mathrm{frame}} = \mathrm{frame\_size} \cdot T_{\mathrm{cc}}$.
No noise-window duration counter, no spill bookkeeping — the writer
already accumulates everything correctly through the existing fill site.

The helper `build_streaming_trigger_weights()` (declared in
[`triggers/streaming.h`](streaming.h)) consumes the pre-Scale TProfile
(passed as `const TH1*` for API flexibility) and emits the
channel-ordinal → weight map plus the precomputed $\mathbb{E}[S | H_0]$
and $\sigma_{S|H_0}$ moments — all in dimensionless **"expected hits per
trigger window"** units (no Hz, no seconds; the
$T_{\mathrm{win}}/T_{\mathrm{frame}}$ ratio is the only normalisation
needed and it's dimensionless).

**Rebuild cadence.**  Exactly **once per spill**, at the moment the
per-frame loop transitions from the first-frames (noise) window into the
data window — i.e. immediately after this spill's noise frames have
finished populating `h_dcr_per_channel`.  Rebuilding per spill (rather
than once per run) is intentional: channels can come online late (an RDO
that was off in spill 0 starts contributing from spill 1) or drift in
rate over the fill, and a per-spill rebuild keeps the bundle current
without paying for redundant computation on every frame.

The bundle itself is **run-scope** — it persists across spills.  Spill
N's noise frames thus see spill N-1's already-built weights, not an
empty bundle.  Only the "have we rebuilt yet for this spill" flag is
reset at spill start.

**Spill 0 caveat.**  Before the very first noise→data crossing the
bundle is empty (default-constructed `σ = 0`, `n_σ_of() ≡ 0`).  Spill 0's
*noise* QA hist therefore fills at `n_σ = 0` until the first build
completes.  Spill 0's *data* QA hist and all subsequent spills (both QA
hists) get real distributions.

**Floor:**  channel DCR is clamped at 1 kHz from below.  Channels with no
entries in the noise sample (dead, masked, unmeasured, or simply unfilled
in the current spill's accumulation) get the floor weight via the
bundle's `default_weight` field — no special-casing in the hot loop.

**Statistics:**  with `first_frames_trigger = 5000` frames × 1024 cc ≈
16 ms beam-off time per spill, even a 100 Hz channel collects ~1.6 entries
per spill — accuracy improves linearly with spill count, and the per-spill
rebuild keeps the trigger using the freshest estimate available.

### 2.3  Threshold and QA

The score $S$ is converted to a standardised score before thresholding:

$$n_\sigma = \frac{S - \mathbb{E}[S \mid H_0]}{\sqrt{\mathrm{Var}[S \mid H_0]}}$$

with $\mathbb{E}[S \mid H_0] = N_{\mathrm{ch}}^{\mathrm{active}}$ and
$\mathrm{Var}[S \mid H_0] = \sum_{c \in \mathrm{active}} 1/m_c$, where
"active" denotes channels both reliably measured in the cumulative noise
sample (≥ `min_noise_hits`) and actually participating in the current
spill.  The framer fires when $n_\sigma \geq n_\sigma^\star$, where
$n_\sigma^\star$ is set in the config.

> **`n_σ` is a standardised score, *not* a Gaussian z-score.**  The
> underlying $S$ is **positive-bounded** ($S \geq 0$, by construction —
> weights are positive, hit counts are non-negative).  That alone forces
> the noise distribution to be one-sided: the minimum admissible
> $n_\sigma$ is $-\sqrt{\Lambda}$, where $\Lambda = \sum_c m_c$ is the
> expected total hits per window.  For sparse noise ($\Lambda \lesssim 1$,
> typical) the distribution can barely fluctuate below the mean, and the
> per-hit sampling regime (we fill QA at every hit, dropping S = 0
> windows) shifts the *observed* distribution strictly positive.  Bin
> structure is also discretised by integer hit counts (the comb seen on
> the noise plot).  Operationally this is fine — $n_\sigma$ is used as a
> **discriminator** between noise and signal tails, not as a literal
> "standard deviations above zero".  Don't expect the noise QA hist to be
> centred at zero or Gaussian-shaped.

**Two QA histograms** are filled **always**, regardless of whether the
threshold is crossed.  Both are normalised by entry count at write time
so the y-axis is **probability per bin**:

| Histogram | When filled | What it tells you |
|---|---|---|
| `h_streaming_score_noise` | First-frames window (beam-off; the start-of-spill noise sample tagged by `first_frames_trigger`) | Pure-noise $n_\sigma$ distribution.  $\int_{n_\sigma \geq n_\sigma^\star}$ → **misfire probability** (directly readable thanks to the probability normalisation) |
| `h_streaming_score_data` | Data-taking window (the post-first-frames part of the spill, when hardware triggers are firing) | Signal+noise distribution.  $\int_{n_\sigma \geq n_\sigma^\star}$ → **acceptance**; $1 - \mathrm{acceptance}$ → missed-fire rate |

### 2.4  Workflow

The threshold lives in a TOML config (likely a `[streaming_trigger]` table
inside `framer_conf.toml`, or a separate `conf/streaming_trigger.toml` —
TBD at implementation).  It is **always set**; the QA exists as a
correction loop, not the primary mechanism.

```
              ┌───────────────────────────────────────────────────────┐
              │  framer runs with current n_σ★ (initial guess, e.g. 3)│
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

The current threshold is honoured every run.  QA tells you whether it's in
the right place.

### 2.5  Deferred to v2

Three improvements are explicitly **out of scope** for v1:

- **Conservative DCR estimator.**  Using the 75th-percentile (rather than
  mean) of the per-channel noise distribution would bias $\lambda_i$
  estimates *high*, making the trigger more robust to under-measured
  noise.  Adds estimator complexity; not in v1.
- **Crosstalk correction.**  Crosstalk inflates apparent per-channel rates
  non-Poissonianly.  Needs to be either subtracted from $\lambda_i$ before
  weighting, or carried as a per-channel multiplicity factor.  Couples the
  trigger to the crosstalk-treatment pipeline; not in v1.
- **Signal-aware weighting** $w_i = s_i / \lambda_i$.  Requires a measured
  per-channel signal model — bootstrap by running v1, collecting a ring
  sample, building the signal histogram, re-triggering.  Multi-pass
  calibration; revisit when v1 statistics warrant it.

### 2.6  Extension to other detectors

The weighting framework is detector-agnostic — each channel has its own
$\lambda$ regardless of which detector subsystem it belongs to — but the
streaming trigger is currently Cherenkov-only.  Extending to timing /
tracking detectors needs separate threshold tuning and separate QA
histograms per detector class.  Tracked separately from this design;
revisit once v1 lands for Cherenkov.

---

## 3.  Schema caveats

### 3.1  `TriggerEvent` does not carry physical origin

The stored `TriggerEvent` struct is
`{uint8_t index, uint16_t coarse, float fine_time, bool is_secondary}`.
**There is no detector-side origin field** — no device, no fifo, no
channel.  The framer resolves the trigger source (via the two-mode
configuration) at ingest and emits only the resolved logical `index`.

Consequences:

- Cannot retroactively ask "which physical channel emitted this trigger?"
  from a stored event — framing must be re-run to recover that.
- Trigger-latency-vs-channel-position studies need either an extended
  schema or a parallel framer run with explicit bookkeeping.
- The "unknown trigger" branch in
  [`parallel_streaming_framer.cxx`](../../src/parallel_streaming_framer.cxx)
  stuffs `current_device` into the `coarse` (timestamp) field as the last
  remaining source-ID slot.  Any consumer reading `coarse` as a timestamp
  on an "unknown" event will get a device ID instead — a documented
  workaround until the schema is bumped.

A future schema extension would add either a `GlobalIndex` (TDC- or
channel-level) or an unpacked `{device, fifo, channel}` triple.  ROOT
schema-evolution can keep old trees readable; new trees would carry the
origin natively.

---

*Document version: 2026-05-26.*
*Implements: D-05 (landed), D-12 (v1 landed — DCR-weighted score, $n_\sigma$ threshold, QA score histograms, dedicated translation unit).*
