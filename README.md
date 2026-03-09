# ePIC dRICH Beam Test Analysis Framework

A lightweight **C++ analysis framework** for processing and analyzing beam test data from the ePIC **dual-radiator RICH (dRICH)** prototype, built on top of [ROOT](https://root.cern/).

The framework provides core data structures, I/O utilities, ROOT dictionary support, and example macros to explore and analyze beam-test ROOT files produced with ALCOR front-end electronics.

[![Documentation](https://img.shields.io/badge/docs-online-blue)](https://nikolajal.github.io/epic-drich-beam-test-analysis/)

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
├── src/                        # Implementation files (.cxx)
├── macros/                     # ROOT macros for analysis
│   ├── examples/               # Ready-to-run example macros
│   └── utilities/              # Pipeline entry-point macros (lightdata, recodata, …)
├── scripts/                    # Build and install scripts
├── conf/                       # Readout, mapping, and trigger configuration files
├── run-lists/                  # Run database and run list definitions (.toml)
├── dict/                       # ROOT dictionary linkdef header
├── docs/                       # Doxygen configuration and generated documentation
├── CMakeLists.txt              # CMake build configuration
└── README.md
```

---

## Prerequisites

- **C++17**-compatible compiler (GCC ≥ 9 or Clang ≥ 10)
- **ROOT ≥ 6.x** with `root-config` accessible in `PATH`
- **CMake ≥ 3.16**
- Internet access at first build (CMake fetches [CLI11](https://github.com/CLIUtils/CLI11), [toml++](https://github.com/marzer/tomlplusplus), and [MIST](https://github.com/Nikolajal/mist) automatically via `FetchContent`)

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
(`mist::ring_finding::hough_transform`) operating directly on Cherenkov hit clusters
within each 3.2 µs frame. Active areas of development include:

- **Neural-network ring recognition** — a NN-based alternative to the Hough transform
  for improved efficiency at low photon multiplicity.
- **Time-cluster self-triggering** — a time-cluster finding algorithm to enable
  trigger-less operation by identifying Cherenkov bursts without an external hardware trigger.

---

## Data Formats

### ALCOR Raw Data (`alcor_data`, `alcor_finedata`)

Low-level TDC hits decoded from the ALCOR ASIC front-end. `alcor_finedata` adds a per-channel calibration table (3 parameters per TDC) that converts raw fine-time bins into calibrated timestamps in nanoseconds. The detector consists of 8 PDUs of 16×16 SiPM pixels in a square layout (centre PDU absent).

### Lightdata (`alcor_lightdata`, `alcor_spilldata`)

Frame-based, filtered view of ALCOR data. Hits are grouped into readout frames and assigned to one of four categories — **Cherenkov**, **timing**, **tracking**, or **trigger** — based on the readout configuration file. Dead-channel and active-participant bitmasks are tracked per device and per spill.

### Recodata (`alcor_recodata`, `alcor_recotrackdata`)

Event-level, analysis-ready data structures. Photon rings are reconstructed using DBSCAN or the Hough transform and optionally associated with particle tracks from the ALTAI tracking telescope. This is the format used for physics studies such as ring spatial resolution and Cherenkov angle reconstruction.

---

## Configuration

Runtime configuration is handled through plain-text and TOML files in `conf/`:

| File                       | Description                                              |
| -------------------------- | -------------------------------------------------------- |
| `readout_config.txt`       | Maps (device, chip) pairs to hit categories              |
| `mapping_conf.<year>.toml` | Pixel-to-physical-position mapping for the SiPM plane    |
| `trigger_conf.<year>.toml` | Trigger logic and channel assignment                     |
| `sensors_config.txt`       | SiPM sensor parameters (gain, breakdown voltage, …)      |

Run lists and a run metadata database for 2025 are available in `run-lists/` in TOML format, loadable via `run_info::read_database()` and `run_info::read_runslists()`.

---

## Documentation

Full API documentation is generated with Doxygen and hosted online:

[**https://nikolajal.github.io/epic-drich-beam-test-analysis/**](https://nikolajal.github.io/epic-drich-beam-test-analysis/)

To build documentation locally:

```bash
doxygen docs/Doxyfile
```

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