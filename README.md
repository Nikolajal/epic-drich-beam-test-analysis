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
├── include/                    # Header files for core data structures
│   └── ring-finding-algorithms/    # Ring-finding algorithm headers (e.g. Hough transform)
├── src/                        # Implementation files (.cxx)
│   └── ring-finding-algorithms/    # Ring-finding algorithm implementations
├── macros/                     # ROOT macros for analysis
│   ├── examples/                   # Ready-to-run example macros
│   └── utilities/                  # Shared macro utilities
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

## Data Formats

The framework defines three levels of data abstraction, each represented by a dedicated C++ class and a corresponding ROOT tree structure.

### ALCOR Raw Data (`alcor_data`, `alcor_finedata`)

Low-level TDC hits decoded from the ALCOR ASIC front-end. These represent the raw time-stamped signals before any frame or event building.

### Lightdata (`alcor_lightdata`)

Frame-based, filtered view of ALCOR data. Hits are grouped into readout frames, with coarse noise rejection applied. This is the typical starting point for Cherenkov photon counting analyses.

### Recodata (`alcor_recodata`, `alcor_recotrackdata`)

Event-level, analysis-ready data structures. Photon rings are reconstructed and optionally associated with particle tracks from the ALTAI tracking telescope. This is the format used for physics-level studies such as ring spatial resolution or Cherenkov angle reconstruction.

---

## Configuration

Runtime configuration is handled through plain-text files in `conf/`. Versioned copies are kept for each data-taking period and symlinked to the active version:

| File                  | Description                                            |
| --------------------- | ------------------------------------------------------ |
| `readout_config.toml` | ALCOR readout channel mapping                          |
| `mapping_conf.toml`   | Pixel-to-SiPM spatial mapping (symlink → current year) |
| `trigger_conf.toml`   | Trigger logic configuration (symlink → current year)   |
| `sensors_config.txt`  | SiPM sensor parameters                                 |

Run lists and a run database for 2025 are available in `run-lists/`.

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

---

## Authors

| Name          | Contact                  |
| ------------- | ------------------------ |
| Nicola Rubini | nicola.rubini@bo.infn.it |

### Main Contributors

| Name                | Contact                        |
| ------------------- | ------------------------------ |
| Roberto Preghenella | roberto.preghenella@bo.infn.it |
