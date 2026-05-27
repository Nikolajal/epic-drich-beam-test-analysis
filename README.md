# ePIC dRICH Beam Test Analysis Framework

A lightweight **C++ analysis framework** for processing and analyzing beam test data from the ePIC **dual-radiator RICH (dRICH)** prototype, built on top of [ROOT](https://root.cern/).

The framework provides core data structures, I/O utilities, ROOT dictionary support, and example macros to explore and analyze beam-test ROOT files produced with ALCOR front-end electronics.

[![Documentation](https://img.shields.io/badge/docs-online-blue)](https://nikolajal.github.io/epic-drich-beam-test-analysis/)
[![Build](https://github.com/Nikolajal/epic-drich-beam-test-analysis/actions/workflows/build.yml/badge.svg)](https://github.com/Nikolajal/epic-drich-beam-test-analysis/actions/workflows/build.yml)
[![Formatting](https://github.com/Nikolajal/epic-drich-beam-test-analysis/actions/workflows/clang-format.yml/badge.svg)](https://github.com/Nikolajal/epic-drich-beam-test-analysis/actions/workflows/clang-format.yml)

---

## Table of Contents

- [Purpose](#purpose)
- [Repository Structure](#repository-structure)
- [Prerequisites](#prerequisites)
- [Build & Install](#build--install)
- [Usage](#usage)
- [Data Pipeline](#data-pipeline)
- [Data Formats](#data-formats)
- [Configuration](#configuration)
- [Documentation](#documentation)
- [Testing & CI](#testing--ci)
- [Open design questions](#open-design-questions)
- [Contributing](#contributing)
- [Authors](#authors)

---

## Purpose

This repository aims to:

- Provide consistent **C++ classes and ROOT bindings** for beam test data formats (`alcor_*`, `lightdata_*`, `recodata_*`, `recotrackdata_*`).
- Enable fast, reproducible ROOT-based analysis through ready-to-use example macros.
- Offer utility functions for decoding ALCOR TDC data and interpreting experimental tree branches.
- Serve as a shared foundation for ongoing and future analysis in the ePIC dRICH data pipeline.

---

## Repository Structure

```
.
├── include/                    # Public header files for all classes and utilities
│   ├── utility.h               # Umbrella header — re-exports everything in util/
│   ├── util/                   # Small header-only helpers (GlobalIndex, RootHist, …)
│   ├── triggers.h              # Umbrella header — re-exports events / config / registry
│   ├── triggers/               # Trigger subsystem
│   │   ├── events.h            #   - runtime types: TriggerNumber, TriggerEvent
│   │   ├── config.h            #   - schema + reader: DeviceTrigger, ChannelTrigger, TriggerConfigSet
│   │   ├── registry.h          #   - bin-label lookup: TriggerRegistry
│   │   ├── streaming/          #   - software trigger pipeline (split by stage):
│   │   │   ├── score.h         #       stage 1: DCR-weighted score + time clustering
│   │   │   └── hough.h         #       stage 2: Hough ring finder + fit_circle refinement
│   │   └── DISCUSSION.md       #   - community-facing design notes
│   └── writers/                # Independent pipeline-stage entry points (no umbrella by design — see DISCUSSION attention-point)
├── src/                        # Implementation files (.cxx)
├── macros/                     # ROOT macros for analysis
│   ├── examples/               # Ready-to-run example macros
│   └── utilities/              # Pipeline entry-point macros (lightdata, recodata, …)
├── scripts/                    # Build and install scripts
├── conf/                       # Readout, Mapping, trigger, and streaming-trigger
│                                #   configuration files
├── run-lists/                  # Run database and run list definitions (.toml)
├── dict/                       # ROOT dictionary linkdef header
├── docs/                       # Doxygen configuration and generated documentation
├── CMakeLists.txt              # CMake build configuration
└── README.md
```

### Repository conventions for `include/` subdirectories

Three coexisting organisational patterns, each fitting its role:

| Pattern | Example | When to use |
|---|---|---|
| **Umbrella + helpers** | [`utility.h`](include/utility.h) ↔ [`util/`](include/util) | Subsystem of small, low-coupling, header-only helpers.  The umbrella is a pure re-exporter; consumers `#include "utility.h"` to get everything or cherry-pick from `util/`. |
| **Subsystem types + algorithms** | [`triggers.h`](include/triggers.h) ↔ [`triggers/`](include/triggers) | Subsystem with cross-cutting types **and** algorithms.  Types/config/registry live in sub-headers re-exported by the umbrella; algorithm headers (e.g. [`triggers/streaming/score.h`](include/triggers/streaming/score.h), [`triggers/streaming/hough.h`](include/triggers/streaming/hough.h)) are **not** re-exported — include them deliberately. |
| **Category grouping** | [`writers/`](include/writers) | Folder of independent entry points that share no types or interface.  No umbrella; adding one would re-export nothing.  Stays flat on purpose. |

> **Trigger subsystem.**  The two-mode config schema (device / channel) is
> documented in [`include/triggers/DISCUSSION.md`](include/triggers/DISCUSSION.md);
> the two-stage software trigger pipeline (DCR-weighted score → Hough ring
> finder, D-12) is in [`include/triggers/streaming/DISCUSSION.md`](include/triggers/streaming/DISCUSSION.md).
> TOML knobs live in [`conf/streaming.toml`](conf/streaming.toml) under the
> `[streaming_trigger]` (stage 1) and `[streaming_hough]` (stage 2) sections;
> tune `n_sigma_threshold` from the QA score histograms
> (`Streaming Trigger/h_streaming_score_{noise,data}` in the lightdata output)
> after a first run.

---

## Prerequisites

- **C++17**-compatible compiler (GCC ≥ 9, Clang ≥ 10, or MSVC ≥ 19.20)
- **ROOT ≥ 6.x** with `root-config` accessible in `PATH`
- **CMake ≥ 3.16**
- Internet access at first build — CMake fetches the following via `FetchContent`:
  - [CLI11](https://github.com/CLIUtils/CLI11) (pinned to v2.4.2)
  - [toml++](https://github.com/marzer/tomlplusplus) (pinned to v3.3.0)
  - [MIST](https://github.com/Nikolajal/mist) (pinned to a specific commit — update the `GIT_TAG` in [CMakeLists.txt](CMakeLists.txt) when the MIST API moves)

---

## Build & Install

The fastest way is to use the provided script, which cleans any previous build, configures, compiles, and installs to `~/.local`:

```bash
bash scripts/install.sh
```

Alternatively, build manually:

```bash
git clone https://github.com/Nikolajal/epic-drich-beam-test-analysis.git
cd epic-drich-beam-test-analysis
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build . --parallel
cmake --install .
```

The shared library and headers will be installed under `$HOME/.local/lib` and `$HOME/.local/include` respectively.

---

## Usage

Example macros live in `macros/examples/`. Load the library loader header first so ROOT can find the installed library, then run any macro interactively:

```bash
root -l
.x macros/examples/dark_count_rate.cpp
```

Or non-interactively:

```bash
root -l -q 'macros/examples/ring_spatial_resolution.cpp'
```

Available examples:

| Macro                                       | Description                                       |
| ------------------------------------------- | ------------------------------------------------- |
| `dark_count_rate.cpp`                       | Estimate dark count rate from ALCOR data          |
| `photon_number.cpp`                         | Photon yield analysis                             |
| `afterpulse_treatment.cpp`                  | Afterpulse identification and removal             |
| `cross_talk_treatment.cpp`                  | Cross-talk correction                             |
| `ring_spatial_resolution.cpp`               | Ring spatial resolution using reconstructed rings |
| `ring_spatial_resolution_with_tracking.cpp` | Ring spatial resolution with ALTAI tracking       |

---

## Data Pipeline

Raw ALCOR data is processed through three sequential steps, each producing a ROOT file:

```
ALCOR raw (.root, one file per FIFO)
        │
        ▼  lightdata_writer
lightdata.root       ← framed (1024 cc / frame), trigger-selected,
                       hit-categorised (Cherenkov / timing / tracking / trigger),
                       afterpulse-suppressed, position-assigned
        │
        ▼  recodata_writer
recodata.root        ← cross-talk cleaned, ring-found (DBSCAN or Hough),
                       analysis-ready hit + trigger collections
        │
        ▼  recotrackdata_writer  (optional — requires ALTAI tracking file)
recotrackdata.root   ← track-matched recodata with ALTAI telescope information
```

Each step is invoked via the corresponding macro in `macros/utilities/` or the matching free function declared in `include/`.

### In development

The `lightdata_writer` step implements a streaming Hough-transform ring finder
(`mist::ring_finding::HoughTransform`) operating directly on Cherenkov hit clusters
within each 3.2 µs frame. Active areas of development include:

- **Neural-network ring recognition** — a NN-based alternative to the Hough transform
  for improved efficiency at low photon multiplicity.
- **Time-cluster self-triggering** — a time-cluster finding algorithm to enable
  trigger-less operation by identifying Cherenkov bursts without an external hardware trigger.

---

## Data Formats

### ALCOR Raw Data (`AlcorData`, `AlcorFinedata`)

Low-level TDC hits decoded from the ALCOR ASIC front-end. `AlcorFinedata` adds a per-channel calibration table (3 parameters per TDC) that converts raw fine-time bins into calibrated timestamps in nanoseconds. The detector consists of 8 PDUs of 16×16 SiPM pixels in a square layout (centre PDU absent).

### Lightdata (`AlcorLightdata`, `AlcorSpilldata`)

Frame-based, filtered view of ALCOR data. Hits are grouped into readout frames and assigned to one of four categories — **Cherenkov**, **timing**, **tracking**, or **trigger** — based on the readout configuration file. Dead-channel and active-participant bitmasks are tracked per device and per spill.

### Recodata (`AlcorRecodata`, `AlcorRecotrackdata`)

Event-level, analysis-ready data structures. Photon rings are reconstructed using DBSCAN or the Hough transform and optionally associated with particle tracks from the ALTAI tracking telescope. This is the format used for physics studies such as ring spatial resolution and Cherenkov angle reconstruction.

### Channel addressing — `GlobalIndex`

Every hit carries a packed 32-bit address identifying its origin TDC channel:
`(device, FIFO, chip, channel, TDC)`.  The address is wrapped in a value type
[`GlobalIndex`](include/GlobalIndex.h) with built-in validation, a validity
sentinel bit, and explicit accessors for both the **TDC-level** view (full
address, used by per-TDC calibration tables) and the **global-channel-level**
view (TDC bits zeroed, used as a key for per-channel Mapping):

```cpp
auto gi = GlobalIndex::from_components(197, /*fifo=*/5, /*chip=*/3,
                                         /*channel=*/42, /*tdc=*/1);
gi.device();              // 197
gi.real_chip();           // 6  (split-in-two: 2*chip + channel>>5)
gi.chip_local_channel();  // 10 (channel within the physical 32-ch chip)
gi.column();              // 2  (ALCOR column-row layout — hardware geometry)
gi.pixel();               // 2  (row within the column — hardware geometry)
gi.global_channel();      // GlobalIndex sibling with tdc=0 (TDC-agnostic key)
gi.is_valid();            // true

// Decode legacy raw values:
auto gi_from_file = GlobalIndex::from_legacy(tdc_level_raw);          // AlcorData::get_global_tdc_index()
auto gc_from_file = GlobalIndex::from_legacy_channel(channel_raw);    // AlcorData::get_global_index()
```

The layout is final-detector native (64-ch chips, up to 2048 devices); the
current 32-ch split-in-two detector is handled by an adapter at the framer's
input boundary.  See [`include/GlobalIndex.h`](include/GlobalIndex.h) for
the full bit layout and the `gidx::kUsesSplitInTwo` compile-time flag.

---

## Configuration

Runtime configuration is handled through TOML files in `conf/`:

| File                       | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `readout_config.toml`      | Maps (device, chip) pairs to hit categories              |
| `framer_conf.toml`         | Streaming-framer + QA-window parameters (frame size, afterpulse / cross-talk sidebands) |
| `mapping_conf.<year>.toml` | Pixel-to-physical-position Mapping for the SiPM plane    |
| `trigger_conf.<year>.toml` | Trigger logic and channel assignment                     |
| `streaming.toml`           | Software-trigger pipeline: `[streaming_trigger]` (stage 1 score), `[streaming_hough]` (stage 2 ring finder) |
| `recodata.toml`            | Recodata live-QA pipeline: coverage-map geometry, per-ring photon counting |

All config files honour the `##` cutoff sentinel (see [include/toml_utils.h](include/toml_utils.h))
so you can append `## --- disabled ---` and keep scratch entries below without them being parsed.

Run lists and a run metadata database for 2025 are available in `run-lists/` in TOML format, loadable via `RunInfo::read_database()` and `RunInfo::read_runslists()`.

---

## Documentation

Full API documentation is generated with Doxygen and hosted online:

[**https://nikolajal.github.io/epic-drich-beam-test-analysis/**](https://nikolajal.github.io/epic-drich-beam-test-analysis/)

To build documentation locally:

```bash
doxygen docs/Doxyfile
```

**For contributors:** the project's naming and style rules are documented in
[`docs/coding_conventions.md`](docs/coding_conventions.md).

### Repository-side checks (`tools/`)

Two standalone scripts live under [`tools/`](tools/).  Run them after any
change that touches histograms, GlobalIndex usage, or build infrastructure —
they catch the bug classes the recent Phase-5 migration surfaced.

| Script | Purpose | Typical use |
|---|---|---|
| [`tools/lint_codebase.py`](tools/lint_codebase.py) | **Static lint.**  Flags histogram-Fill arguments that bypass the GlobalIndex ordinal accessors (R1), debug-leftover histogram names (R2), commented-out function-call lines (R3), and legacy Phase-4 bit-bashing formulas (R4).  Suppress per-line with `// LINT-OK: <reason>` or whole-file with `// LINT-OK-FILE: <reason>`. | `tools/lint_codebase.py` — exits 0 if clean, 1 on findings. |
| [`tools/check_qa.py`](tools/check_qa.py) | **Runtime QA content check.**  Opens an output ROOT file, walks every `TH1`/`TH2`/`TH3`/`TProfile`, and flags histograms that are empty or have most content in over/underflow.  Catches Phase-5 fill-target mismatches that the lint missed. | `tools/check_qa.py path/to/lightdata.root --known-empty 'Streaming Trigger/.*' --known-overflow 'Single-Pixel Noise/h_afterpulse_dt'` — exits 0 if all histograms are OK, expected-empty, or expected-overflow. |

Both are pure-Python (PyROOT for `check_qa`) and require no build.  See the
header docstring of each script for the full set of options.

---

## Testing & CI

The repository runs build + test verification on every push and pull request
via [`.github/workflows/build.yml`](.github/workflows/build.yml).
The matrix exercises:

|                | Release                | Debug                  |
|----------------|------------------------|------------------------|
| Linux (Ubuntu) | build + ctest, required| build + ctest, required|
| macOS          | build + ctest, required| build + ctest, required|
| Windows        | build + ctest, best-effort | —                  |

The Windows leg uses ROOT from `conda-forge` and is currently marked
`continue-on-error: true` while the configuration stabilises — failures there
do not block PRs.  Promote it to a required check once it is consistently
green (remove the `continue-on-error` line).

### Building & running tests locally

Enable the test suite with `-DBTANA_BUILD_TESTS=ON`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBTANA_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

| Binary | Source | Coverage |
|--------|--------|----------|
| `test_global_index` | [test/tester_global_index.cxx](test/tester_global_index.cxx) | Component round-trip, legacy round-trip (every legacy raw on devices 192–199), validity-bit semantics, `try_from_components` nullopt cases, detector-aware helpers (`real_chip`, `chip_local_channel`), TDC vs pixel-level views, `from_legacy_pixel` factory, lazy `idx/4` replacement |

Further unit tests (lightdata pipeline round-trip, calibration table read/write,
ring-finder regression) are on the roadmap.

### Formatting

A second workflow ([`.github/workflows/clang-format.yml`](.github/workflows/clang-format.yml))
runs `clang-format-22` over every `.h`/`.cxx`/`.cpp` under `include/`, `src/`, and `macros/` on each PR.
It auto-commits any reformatting back to the PR branch and fails the check so the author
knows to `git pull` before merging.  Format locally to skip the round-trip:

```bash
find include src macros -type f \( -name '*.h' -o -name '*.cxx' -o -name '*.cpp' \) \
  | xargs clang-format -i --style=file
```

---

## Open design questions

Open design questions and refactor proposals live as GitHub issues with the
`design` label.  Read the relevant issue before landing any change that
touches one of the open items so the rationale follows the decision.

---

## Contributing

Contributions are welcome. The repository follows a **trunk-based branching model** with two protected branches:

```
main
 └── dev
      ├── dev_feature_A
      ├── dev_feature_B
      └── ...
```

- **`main`** — stable, tagged releases only. Updated exclusively via reviewed pull requests from `dev`.
- **`dev`** — integration branch for completed features. Updated exclusively via reviewed pull requests from feature branches.
- **`dev_<feature>`** — short-lived branches for individual features or fixes, forked from `dev`. No strict review policy; push freely here.

### Workflow

1. **Fork `dev`** into a new branch named after your contribution:

   ```bash
   git checkout dev
   git pull origin dev
   git checkout -b dev_my_feature
   ```

2. **Develop and commit** on your feature branch. Keep commits focused; a descriptive commit message helps reviewers.

3. **Open a pull request** targeting `dev`. At least one review approval is required before merging.

4. **Do not push directly to `dev` or `main`.** Both branches have branch protection enabled.

5. Merges from `dev` into `main` are performed by the maintainers and correspond to versioned releases (following [Semantic Versioning](https://semver.org/)).

### Code Style

The project uses the **C/C++ extension for VS Code** (`ms-vscode.cpptools`) as the formatting reference. The style is fully defined in `.clang-format` at the repo root, so any `clang-format`-aware editor or tool will apply it automatically.

Formatting is enforced on every PR targeting `dev` or `main` via `.github/workflows/clang-format.yml`. The workflow:

1. Runs `clang-format --style=file` over all `.h`, `.cxx`, and `.cpp` files under `include/` and `src/`.
2. If any file changed, **commits the fixes back to your PR branch** automatically.
3. **Fails the check** regardless, so you know to `git pull` the fixup commit before the PR can be merged.

To avoid the round-trip, format locally before pushing:

```bash
# Requires clang-format — install with: brew install clang-format
find include src -type f \( -name '*.h' -o -name '*.cxx' -o -name '*.cpp' \) \
  | xargs clang-format -i --style=file
```

Other style expectations:

- New data structures should provide a corresponding ROOT dictionary entry in `dict/alcor_linkdef.h`.
- New example macros go in `macros/examples/`; shared helpers go in `macros/utilities/`.
- All public headers must carry Doxygen `@file`, `@brief`, and `@param` / `///` comments on every class, struct, field, and non-trivial method. Use `@name` / `///@{` groups to organise getters, setters, and I/O utilities within a class.
- Inline constants that are part of the public API (e.g. detector geometry, conversion factors) must be documented with `///` trailing comments.
- Use `@warning` to flag methods with known caveats or non-obvious ownership semantics.

---

## Authors

| Name          | Contact                  |
| ------------- | ------------------------ |
| Nicola Rubini | nicola.rubini@bo.infn.it |

### Main Contributors

| Name                | Contact                        |
| ------------------- | ------------------------------ |
| Roberto Preghenella | roberto.preghenella@bo.infn.it |