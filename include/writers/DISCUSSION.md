# Writers — design discussion & open questions

> 🧭 **Hub:** project-wide design log + index of satellites lives at
> [`../../DISCUSSION.md`](../../DISCUSSION.md).  Open items here
> also show up in the top-level [`BACKLOG.md`](../../BACKLOG.md).

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

## Lightdata / recodata / recotrackdata writers

(No open items at sweep time.  Add as they surface.)
