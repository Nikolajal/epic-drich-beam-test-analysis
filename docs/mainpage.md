@mainpage ePIC dRICH Beam Test Analysis

A C++/ROOT framework for processing and analysing ePIC dRICH beam-test data,
covering the full chain from raw ALCOR readout to reconstructed Cherenkov rings.

## Data levels

| Level | Class | Description |
|-------|-------|-------------|
| Raw | `AlcorData` | Direct ALCOR decoder output |
| Light | `AlcorLightdata` | Time-clustered, trigger-tagged hits |
| Reco | `AlcorRecodata` | Position-mapped reconstructed hits |
| Track | `AlcorRecotrackdata` | Track-matched hits from ALTAI telescope |

## Key components

- @ref GlobalIndex — Packed 32-bit value type encoding `(device, FIFO, chip, channel, TDC)` with built-in validation, validity sentinel, and explicit TDC-level vs pixel-level views.  All channel addressing in the framework routes through this type.
- @ref Mapping — Channel-to-position lookup chain (device/chip/EO → x,y mm)
- @ref AlcorDataStreamer — Sequential ROOT TTree reader
- @ref RunInfo — TOML-based run metadata and run-list database
- @ref ReadoutConfigList — Readout role assignment and conflict resolution

## Channel addressing

`GlobalIndex` distinguishes between two addressing levels that previously
mixed in the codebase:

| Level | What it identifies | Used by |
|-------|--------------------|---------|
| **TDC-level** (`raw()` / `tdc()`) | one of four TDCs on a specific channel | per-TDC calibration table (`AlcorFinedata::calibration_parameters`) |
| **Global-channel-level** (`global_channel()` / `global_channel_raw()`) | one of the ~512 SiPM channels per device, TDC-agnostic | per-channel maps (position, dead-channel mask, same-channel Δt) |

The legacy `idx / 4` pattern that converted between them is replaced by named
accessors and dedicated factory functions (`from_legacy` for TDC-level inputs,
`from_legacy_channel` for channel-level inputs).

Detector-geometry helpers (`column()`, `pixel()`) remain available on
`GlobalIndex` — they return the ALCOR ASIC's column-row layout (0–7 column,
0–3 row) and are independent of the channel-level / TDC-level split.

## Header layout convention

Helper headers are grouped under two umbrella folders:

| Folder              | Holds                                                                 |
|---------------------|-----------------------------------------------------------------------|
| `include/utility/`  | Standalone utilities (bit ops, `GlobalIndex`, TOML loader, circle/ring fits, ROOT I/O) |
| `include/writers/`  | Sink classes that consume reconstructed levels and emit ROOT trees     |

The `include/utility.h` umbrella re-exports every `utility/` subheader so legacy
call sites keep compiling; new code should `#include "utility/<topic>.h"` directly.

**Inline vs. out-of-line rule** (heavy parsing lives in `.cxx`):
TOML/config readers expose a one-line declaration in their header and put the
heavy parsing logic in the matching `src/*.cxx`.  This keeps `toml++` and
`<mist/logger>` out of public headers — for example, @ref trigger_conf_reader
is declared in `include/triggers.h` and implemented in `src/triggers.cxx`.
The pattern is mirrored by @ref RunInfo / @ref ReadoutConfigList in
`include/utility/config_reader.h` + `src/config_reader.cxx`.

## Repository

Source: https://github.com/Nikolajal/epic-drich-beam-test-analysis