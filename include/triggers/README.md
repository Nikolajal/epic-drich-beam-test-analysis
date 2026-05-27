# `include/triggers/` — trigger subsystem

The trigger subsystem has two largely-orthogonal halves:

1. **Hardware-trigger source schema** — how `[[trigger]]` entries in
   `conf/trigger_conf.<year>.toml` map to physical channels.  Defines
   the schema types, the TOML reader, and the runtime registry used
   for histogram bin labelling.
2. **Streaming software trigger pipeline** — a two-stage pre-filter
   for ring events (DCR-weighted score → Hough ring finder) that
   gates the lightdata writer's output.  Self-contained in
   [`streaming/`](streaming/).

## Files at this level (subsystem-wide)

| File | Contents | Status |
|---|---|---|
| `events.h`    | `TriggerNumber` enum, default tables, `TriggerEvent` struct (the per-event runtime value type) | ✅ Stable |
| `config.h`    | `DeviceTrigger`, `ChannelTrigger`, `TriggerConfigSet`, `pack_channel_key`, `trigger_conf_reader` declaration | ✅ Stable (D-05) |
| `registry.h`  | `TriggerRegistry` — runtime lookup that maps trigger values to dense (value, name) positions for histogram bin labelling | ✅ Stable |
| `DISCUSSION.md` | Design notes for the schema (§ 1) + `TriggerEvent` caveats (§ 2) | ✅ Current |
| `README.md`   | This file | ✅ Current |

The umbrella header at the project root is
[`include/triggers.h`](../triggers.h) — it re-exports `events.h`,
`config.h`, and `registry.h`.  **Algorithm headers (like those under
`streaming/`) are not re-exported** — consumers include them
deliberately.

## Subfolders

| Folder | Purpose |
|---|---|
| [`streaming/`](streaming/) | Two-stage software trigger pipeline (score + Hough).  Has its own README and DISCUSSION; lives self-contained because it's the most software-complex part of the subsystem. |

Future trigger algorithms with comparable complexity should follow the
same pattern: a sibling subfolder with its own README + DISCUSSION,
independent of the schema material at this level.

## Conventions

- **Subsystem types are umbrella-exported.**  `triggers.h` is a pure
  re-exporter for `events.h`, `config.h`, and `registry.h` — same role
  `utility.h` plays for `utility/`.
- **Algorithms are not re-exported.**  Each algorithm header is included
  deliberately by its consumer.
- **TOML files** live in `conf/`, not here.  See the relevant subfolder's
  README for which knob lives where.
- **Documentation:** schema/registry/events caveats in this folder's
  DISCUSSION.md; algorithm-specific design notes in the subfolder's
  DISCUSSION.md.
