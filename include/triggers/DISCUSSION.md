# Trigger subsystem — design notes

> 🧭 **Hub:** project-wide design log + index of satellites lives at
> [`../../DISCUSSION.md`](../../DISCUSSION.md).

This document is the **community-facing** design reference for the
trigger subsystem.  It lives in the source tree (git-tracked) alongside
the code it describes.  The project-root `DISCUSSION.md` is local-only —
anything the team should be able to refer to about trigger design lives
**here**.

> **Audience:**  someone joining the project who needs to understand
> how the trigger logic is structured before touching it.  Self-contained;
> assumes only the README's level of familiarity with the ALCOR readout.

The trigger subsystem has two largely-orthogonal concerns:

1. **Hardware-trigger source schema** (this document, § 1).  How TOML
   `[[trigger]]` entries map to physical channels and registry slots.
2. **Software trigger pipeline** for ring events.  Two-stage pre-filter
   (DCR-weighted score + Hough ring finder) that gates the lightdata
   writer's output.  Lives in its own subfolder so it can be documented
   and evolved without entangling the schema material.  See
   [`include/triggers/streaming/DISCUSSION.md`](streaming/DISCUSSION.md)
   and [`include/triggers/streaming/README.md`](streaming/README.md).

§ 2 below covers caveats that span both concerns (`TriggerEvent` schema).

---

## 1.  Two kinds of trigger configuration

Every entry in `conf/trigger_conf.toml` (a symlink; year variants live under `conf/sets/<year>/`) is exactly one of two shapes —
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

## 2.  `TriggerEvent` schema caveats

### 2.1  No physical-origin field

The stored `TriggerEvent` struct is
`{uint8_t index, uint16_t coarse, float fine_time, bool is_secondary}`.
**There is no detector-side origin field** — no device, no fifo, no
channel.  The framer resolves the trigger source (via the two-mode
configuration in § 1) at ingest and emits only the resolved logical
`index`.

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

## 3. Hardcoded trigger index allocation [20, 99]

The trigger-index space is enumerated by-hand in
[`include/triggers/events.h`](events.h) (values < 100) and the
config-merge logic in
[`src/triggers/config.cxx`](../../src/triggers/config.cxx) uses a
`bool used_indices[100]` to track which slots are taken.  Range
[0, 99] is **hardcoded in two places** (the macro values and the
array dimension) and any future expansion needs both sites updated
in lockstep.

Worth tracking as a low-priority refactor: either parameterise the
array bound (`constexpr int kMaxTriggerIndex = 100;` shared between
the two sites) or move to a dynamic container (`std::bitset<256>`,
`std::unordered_set<int>`).  Not urgent — current allocations only
occupy ~10 of the 100 slots.

## 4. `time_window_ns` shared between streaming stages

The Hough stage's hit pre-selection inherits its time window from
the score stage's config (`time_window_ns`).  There is no separate
`time_cut_ns` knob — by design, the two stages share the timing
context of the streaming trigger event they're built around.  See
[`streaming/DISCUSSION.md`](streaming/DISCUSSION.md) for the
rationale.  Flagged here because the **coupling is implicit** at
the call site: a future tuner who edits the score's
`time_window_ns` silently changes the Hough's behaviour.  Worth a
single in-code comment at the score-stage knob site (TODO).

*Implements: D-05 (landed) — two-mode config schema.  Streaming pipeline
design moved to [`streaming/DISCUSSION.md`](streaming/DISCUSSION.md).*
