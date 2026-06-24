# v2.5.2 — adopt mist-hep (RAII ownership + sideband subtraction)

Maintenance / dependency-adoption release.  **No behaviour change** — identical
QA artefacts and fit results (verified: per-trigger fit numbers unchanged,
pytest 340).

## Dependency
- **mist-hep v1.0.0** is now a dependency — ROOT-backed analysis helpers built
  on mist (`mist::hep::owned`, …).

## Changes
- **v2.5.1 — ownership** — `RootHist<T>` is reimplemented as a thin shim over
  `mist::hep::owned::root_ptr<T>`.  The detach-on-create + ownership-correct
  deletion logic now lives in mist-hep (`owned::make/adopt/root_deleter`)
  instead of being duplicated in-tree.  Interface and semantics unchanged
  (~300 call sites untouched); explicit `Write()` preserved.
- **v2.5.2 — sideband** — `fit_radial_distribution`'s N_γ sideband cross-check
  now uses the ROOT-free `mist::stats::sideband_subtract` (identical equal-width
  flanking geometry: peak μ±3σ, wings to μ±6σ) instead of hand-rolled bin
  arithmetic.
