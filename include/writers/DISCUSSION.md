# Writers — design discussion & open questions

> 🧭 **Hub:** project-wide design log + index of satellites lives at
> [`../../DISCUSSION.md`](../../DISCUSSION.md).

Per-area discussion log for the writers (`pulser_calib_writer`,
`lightdata_writer`, `recodata_writer`, `recotrackdata_writer`).  Use
this file for design hot points, open hypotheses, and follow-up items
that the comments inside the implementation files don't (or
shouldn't) carry.  Keep the implementation comments local and
factual; keep speculation and "we don't know yet" here.

---

## Pulser calibration pipeline (`pulser_calib_writer`)

### Open: ±0.5 cc satellites in the published intercept `b`

After the regime-2 (intermittent-slip) snap and the channel re-fit,
the published `b` distribution still has a heavy-tailed structure
clustering at **half-integer cc** (0.5, 1.5, 2.5, 3.5 cc) — the
fractional part of `|b|` for the surviving outliers is strictly in
[0.40, 0.60].  Integer-cc satellites are *not* present (those would
be permanent whole-TDC slips absorbed naturally by the fit).

We currently do **not** know what these half-integer satellites
represent.  Two working hypotheses (no evidence selecting between
them yet):

1. **Edge-quantisation artefact**.  When a hit's true time straddles
   a coarse-counter boundary, the coarse value can land on either
   side while the fine bin compensates.  For two such hits in the
   same channel-TDC sampled at different sides of the boundary, the
   linear LS fit may land on a half-integer `b` (the LS compromise
   between two integer-cc populations) — *not* a hardware slip.

2. **Real hardware slip at sub-cc granularity**.  If ALCOR has a
   second clock domain or a pipeline stage that operates at finer
   than 1 cc resolution, slips at that resolution would appear at
   integer multiples of the finer unit.  An earlier (2026-05-27)
   speculation tied this to a 640 MHz internal clock; that claim is
   **retracted** — there is no evidence for a second clock domain in
   the codebase or in what's been reported to us.

Diagnostic that would discriminate: dump per-channel `(c, f)`
distribution for a few satellite-affected channels and inspect for
bimodality across a coarse boundary.  Tracked here but not yet
implemented.

### Open: regime-2 slip detection vs. coarse-edge quantisation

The regime-2 pass computes `r_i = (c − (θ + f·a)) mod T_pulser` per
hit and snaps hits whose deviation from the per-(spill, TDC) median
of `r_i` is within `slip_confidence_cc` of an integer cc.  `r_i`
itself is sensitive to coarse-edge quantisation — a hit whose true
time is near a coarse boundary can yield `r_i ≈ ±1 cc` from a
neighbour purely because of which side of the boundary the coarse
counter landed on.

This means regime-2 may be **over-snapping**: counting natural
coarse-edge crossings as slip events.  The pair-difference chi² that
the *fit* minimises is immune to this (the fine correction cancels
the edge effect in the pair difference), but the slip detector
operates on absolute mod-T phase, which is not.

Action items, both deferred:

- Replace the per-hit `r_i` median test with a **pair-difference**
  slip test: a pair whose `Δc − T − model` is close to an integer ≠
  0 cc identifies a slip *between* those two hits.  This inherits
  the same edge-quantisation immunity as the fit.

- Or: keep the current detector but tighten `slip_confidence_cc` to
  well below the expected edge-quantisation noise floor, and verify
  empirically that the snap rate falls to a plausible level.

### Open: fine-band filter is pragmatic; should the fit handle it?

`pulser_calib_writer` currently discards hits with `fine` outside
`[fine_min_valid, fine_max_valid] = [20, 160]` at FIFO ingest.  ALCOR
fine bins typically populate ~[30, 130]; outside that range the
values are pathological (early pickup, end-of-cycle wrap, noise
spikes) and would corrupt the slope fit if let through.

The filter does what we need today, but it's a **band-aid**: the
robust answer is for the fit itself to recognise such hits as
outliers and down-weight them (iteratively reweighted least squares,
M-estimator, etc.).  Until that lands, the filter stays.  When the
fit grows outlier rejection the knob can deprecate to "advisory
only" — still log out-of-band hits, don't refuse them.

### Removed (regime-1 permanent-slip pass)

A "regime-1" pass used to live in `fit_channel`: after the first
solve, any TDC whose `b` landed near a non-zero integer cc had every
hit on that TDC shifted by that integer, then the channel re-fit
with `b ≈ 0` published.  It was removed 2026-05-27.

**The pass produced silently wrong calibrations**: production hits
still carry the hardware slip, and downstream's
`get_time = c − (f·a − b_pub)` with `b_pub = 0` cannot recover the
true time — off by exactly the slip magnitude.  The fit's natural
`b` correctly absorbs the slip; let it.

The QA histogram `Fits/h_fit_intercept_b` honestly shows satellites
at integer-cc positions for permanently-slipped TDCs; those
satellites are the calibration working as intended, not a defect.

---

## Shared anchor-Δt canvas (`render_anchor_dt_canvas`)

The 3-pad ``Δt_{trg} vs spill`` zoom canvas (banner + +rollover pad +
±250 cc main + −rollover pad, colz palette, equal-px/cc invariant) was
originally inline in `pulser_calib_writer`.  Extracted 2026-05-29 to
`include/writers/anchor_dt_canvas.h` / `src/writers/anchor_dt_canvas.cxx`
so the same renderer covers both:

- `pulser_calib_writer` — one canvas per run.  Two anchor modes:
  *legacy channel* (`cfg.anchor_device/chip/eo_channel`, paired hit-by-hit)
  and *FIFO-salvage* (`cfg.anchor_fifo ≥ 0`).  In FIFO-salvage mode the
  pulsed reference (e.g. the KC705 testpulse, device 200 / FIFO 32) is read
  out on a dedicated FIFO with `tdc/fine/pixel/column` all sentinel (-1) —
  no valid channel ordinal — so it is salvaged by `(device, fifo)` (require
  `trigger_tag`, type 9, to skip the spill markers) and each channel hit is
  referenced to the *nearest* anchor pulse via binary search on the
  per-spill (strictly monotonic) coarse list.  A companion 1D diagnostic
  (`06_anchor_consecutive_dt`, not this 3-pad canvas) fits the consecutive
  anchor-pulse Δt to a Gaussian → pulse period + jitter, and reports the
  average rate (= 1 / (period_cc · CC_TO_NS)).
  **Coincidence analysis (real-laser FIFO mode).** The channel−anchor Δt
  peak sits at the laser/cable delay, often far from 0. Rather than chase it
  with an adaptive window, a configurable **delay** (`cfg.anchor_delay_cc`,
  mimicking the trigger setup; `0` = auto-picker — centre on the measured
  peak when it is picked up correctly, i.e. enough lit pixels, else no shift;
  nonzero pins the delay literally) is subtracted so the peak lands in the
  fixed ±250 cc Δt-vs-spill canvas and
  the ±100 cc integrated plot (`07`, 1 cc bins). `07` is fit with the full
  model **pol0 (DCR) + gaus1 + gaus2** (components drawn individually,
  log-Y); prompt = the larger-area Gaussian, afterpulse = the smaller, and
  the **afterpulse probability = smaller area / larger area**. A physical
  **coincidence hitmap** (`08`, x/y mm via the channel→position `Mapping`,
  same 396² ±99 mm geometry as the lightdata hitmaps) lights up the laser
  spot: per pixel, the raw hit count inside ±2 cc of that pixel's own peak
  (significant pixels only).
- `lightdata_writer` — one canvas per ``TriggerNumber`` that fired
  ≥ 1 time in the run.  Anchor = the trigger's own coarse counter;
  Y = `c_hit − c_trigger` in cc.  Lazy-allocated per trigger; written
  to the per-trigger TDirectory in `lightdata.root` and emitted as
  `qa/lightdata/<NN>_anchor_dt_<trigger_name>.pdf` (NN starts at 05,
  sorted by trigger registry index for deterministic file order).

Layout invariant: **plot areas exactly 1:5:1** matching the
100:500:100 cc Y-data ranges (equal px/cc).  Pad outlines work out to
≈ 1:5:1.47 because the bot pad bundles a 6 % canvas-NDC x-axis area
on top of its plot unit — the geometric trade for keeping the
``spill`` x-axis title visible without clipping.  Tunables live at
the top of the layout block in `anchor_dt_canvas.cxx`
(`kBannerH`/`kXAxisH`/`kTopMargin`); change one, the rest re-solve.

## Lightdata / recodata / recotrackdata writers

Open design points captured 2026-05-29 (BACKLOG Q&A pass).

### pulser_calib — design batch

- **`±0.5 cc satellites in published `b`** — disambiguate
  edge-quantisation artefact vs hardware slip by **per-spill
  stability**: if the satellite population correlates with spill
  number (clusters at specific spills), it's a real hardware slip
  event; if uniform in time, it's coarse-edge quantisation noise.
  The existing anchor-Δ vs spill diagnostic plot already carries the
  signal — read it off, no new code needed for the measurement
  protocol.
- **Regime-2 slip pair-difference test** — design is clear: take
  consecutive `(c_h − c_p)` pairs (same hardware, same anchor); under
  pure quantisation noise their difference distributes as
  `0 ± √2 · σ_quant`.  A real slip shows up as a step in the
  pair-difference time series.  Quantitative thresholds get pinned
  on first data run.
- **γ-mode (Stage 2) per-TDC aggregation** — firm design: pool
  cdiff histograms across TDCs that share the same `(chip,
  channel_logical)` (i.e. TDCs that see the same physical pixel),
  fit *once* per channel, distribute the result back.  Reduces per-
  TDC statistical noise on the channels that have multiple TDCs
  feeding them.
- **Per-TDC residual σ propagation (`sigma_a` placeholder)** —
  currently emitted as `0.0` (hard-coded at `pulser_calib_writer.cxx:820`).
  Three sub-tasks: (i) compute per-TDC residual σ from the fit, (ii)
  emit into the published TOML calibration file, (iii) define the
  downstream meaning (does AlcorFinedata propagate this into the
  per-hit time σ used by lightdata/recodata?).  (iii) is the design
  question; the other two are wiring.

### recodata writer — split / variant strategy

- **Sensor-model split (k1350 / k1375)** — source the per-device
  sensor type from `readout_conf.toml` (the existing per-role
  `sensor_type` field).  Add the per-device override later
  (separate Patch row, see `readout_config.toml`).
- **Time-window QA split** — trigger-aligned windows, not
  prompt/early/DCR.  See `triggers/streaming/DISCUSSION.md` →
  "Recodata multiple time windows" for the rationale.
- **φ-gap split** — **DROPPED 2026-05-29**.  Was on READY; user
  decided the QA value didn't justify the complexity.
- **Stage 2 multithreading (frames-within-spill)** — motivation is
  pure **wall-clock reduction on large datasets** (idle cores during
  per-spill serial loop).  *Not* latency for live operator feedback
  (the per-spill loop is already fast enough for that).  This rules
  out the schedulers that prioritise first-frame latency over
  throughput.
- **Ring → radiator labelling** — blocked on a **per-run beam-metadata
  schema** that doesn't exist yet.  The current database has a
  `radiators` field per run; what's missing is the ring-index ↔
  radiator-index association rule.  Design the schema first, then
  the writer wiring is straightforward.
- **CB+pol3 N_γ restoration (with analytic integral)** — switched
  away from CB on 2026-05-27 (Gauss+pol3 in `radial_fit.cxx`) because
  CB tail parameters `(α, n)` rail-locked on low-stats / broad
  samples, especially ring 2.  The **analytic-integral path
  sidesteps this**: fix `α, n` at physical seeds (or fit loosely),
  extract `N_γ` from the closed form (erf on Gaussian core +
  power-law antiderivative on α-tail) instead of relying on a
  well-converged fit.  Integration domain: right side → `+∞` since
  the tail past `μ` is pure Gaussian; left side → `fit_lo` or
  further as the α-tail prescribes.  Compare against a freshly
  regenerated Gauss+pol3 baseline (re-run recodata on the standard
  run before/after the change — the old snapshot dir is gone).

### lightdata writer — 5 grouped @todos sub-roadmap

The five grouped lightdata-writer sub-roadmap items (FIFO-in-config,
etc.) are still valid; the inline `@todo` markers were lifted out of
`lightdata_writer.cxx` into a prose pointer at the CLI-driver scope.
Sub-roadmap on pickup:

1. **FIFO in config file** — surface the FIFO id from
   `readout_conf.toml` rather than `framer_conf` constants.
2. **Single/multi-core consistency test** — golden-output diff
   between `--jobs 1` and `--jobs N` on a small dataset; must be
   bit-identical for non-mt-sensitive observables.
3. **Afterpulse-fraction plot** — per-channel histogram of
   "consecutive hits within afterpulse-Δt window" / "total hits";
   currently the afterpulse logic flags hits but doesn't publish
   the rate.
4. **QA restructure / re-evaluate needs** — the Hough-refactor QA
   landed per-ring histograms (`triggers/streaming/DISCUSSION.md` §
   2.6 changes).  Audit which lightdata-side QA hists are now
   redundant with the streaming-side ones; consolidate.
5. **Config files from outside** — generalised "load this knob
   from this `.toml`" wiring that replaces the per-config-struct
   boilerplate (`framer_conf_reader`, `qa_conf_reader`,
   `streaming_*_conf_reader`, ...).
