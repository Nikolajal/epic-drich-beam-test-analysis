# ePIC dRICH Beam Test Analysis Framework

A lightweight **C++ analysis framework** for processing and understanding beam test data from the ePIC **dual‑radiator RICH (dRICH)** prototype using ROOT.  
This repository provides core data structures, I/O utilities, ROOT dictionary support, and example macros to explore and analyze beam‑test ROOT files produced with ALCOR front‑end electronics.

---

## 📌 Purpose

The goal of this repository is to:

- Provide consistent **C++ classes and ROOT bindings** for beam test data formats (`alcor_*`, `lightdata_*`, `recodata_*`).
- Enable fast, reproducible ROOT‑based analysis using example macros.
- Offer utility functions for decoding ALCOR data and interpreting experimental tree branches.
- Serve as a foundation for ongoing and future analysis in the ePIC dRICH data pipeline.

---

## 📁 Repository Overview

```
.
├── include/          # Header files for core data structures
├── src/              # Implementation files
├── macros/           # Example ROOT macros for analysis
├── scripts/          # Utility and build scripts
├── docs/             # Generated documentation (via Doxygen)
├── conf/             # Build and configuration files
├── CMakeLists.txt    # CMake build configuration
└── README.md         # This file
```

---

## 🚀 Getting Started

### Prerequisites

- **C++17 compatible compiler**
- **ROOT (>= 6.x)** installed and configured

### Build

```bash
git clone https://github.com/Nikolajal/epic-drich-beam-test-analysis.git
cd epic-drich-beam-test-analysis
mkdir build && cd build
cmake ..
make -j
```

---

## 🧠 Data Formats

### ALCOR Raw Data
Low‑level decoded TDC hits from ALCOR ASIC readout.

### Lightdata
Frame‑based, filtered view of ALCOR data optimized for Cherenkov analysis.

### Recodata
Event‑level, analysis‑ready data structure for physics studies.

---

## 🔍 Usage

Run example ROOT macros:

```bash
root -l
.x macros/example/(example).C
```

---

## 📘 Documentation

[Online documentation](https://nikolajal.github.io/epic-drich-beam-test-analysis/)

---

## 👤 Authors
| Name | Contact |
|---------|-------------|
| Nicola Rubini | nicola.rubini@bo.infn.it
|---------|-------------|
| Roberto Preghenella | roberto.preghenella@bo.infn.it