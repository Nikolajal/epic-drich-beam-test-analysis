# ePIC dRICH beam test anlaysis framework  
A lightweight C++ library for analysing beam‑test data in the ROOT framework.  
The repository ships the core data structures, I/O helpers, a dictionary for ROOT
introspection and a set of example macros that can be run from a ROOT prompt.

---

## Table of Contents
| Section | Description |
|---------|-------------|
| [Introduction](#introduction) | What the project is about |
| [Introduction](#full_documentation) | What the project is about |
| [Author](#author) | Author |

---

## Introduction

The **Beam‑Test‑Analysis** repository contains a minimal set of C++ classes that
represent the data format used in the ePIC dRICH ALCOR-based beam‑test. The library
provides:

* **I/O wrappers** for reading ROOT `TTree` branches (`alcor_*`, `lightdata_*`,
  `recodata_*`).
* **Utility functions** (e.g. `decode_bits`, hit‑mask handling).
* **Dictionary** generation so that the objects can be used directly from the
  ROOT interpreter (`root`, `cint`, or `cling`).
* **Example macros** that demonstrate a typical analysis workflow.

---

## Full Documentation

- [Classes](https://nikolajal.github.io/epic-drich-beam-test-analysis/annotated.html)
- [Files](https://nikolajal.github.io/epic-drich-beam-test-analysis/files.html)

---

## Author
| Name | Contact |
|---------|-------------|
| Nicola Rubini | nicola.rubini@bo.infn.it

