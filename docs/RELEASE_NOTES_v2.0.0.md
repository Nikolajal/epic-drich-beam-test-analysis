# beam-test-analysis v2.0.0

The ePIC dRICH beam-test analysis suite graduates from a batch C++
reconstruction chain into a full **online-QA platform**. v1 reconstructed runs
offline; v2 adds a streaming software trigger, a modular light/reco pipeline
that runs in QA mode, fitted Cherenkov-ring physics, and a PySide6 operator
dashboard for live data-taking during a beam test.

This is a **major** release: the trigger scheme, the writer pipeline, the
configuration schema, and the results backend all changed in
backward-incompatible ways since `v1.0.0`.

---

## Breaking changes

- **Trigger model.** A new streaming software trigger (DCR-weighted score →
  RANSAC ring finder) replaces the v1 trigger handling; trigger events and the
  hit-mask tagging scheme changed.
- **Writer pipeline.** The monolithic lightdata/recodata writers were split
  into composable translation units under `src/writers/`. The
  `lightdata → recodata → recotrack` stages now share a uniform CLI contract
  (`--QA`, `--force-rebuild`, `--force-upstream`, `--threads`, `--max-spill`).
- **Configuration schema.** Config is now read from TOML via a typed reader,
  with a `conf/defaults/` → `conf/working/` setting-set overlay resolved
  through symlinks. New sections/keys: `[readout.timing]`, `[recodata]`
  hardware-ring window, streaming in-beam knobs.
- **Results backend.** `AnalysisResults` moved from a ROOT `TTree` to a keyed
  map (`ResultMap`), changing how downstream multi-run trends read values.
- **Header layout.** `include/util/` was renamed to `include/utility/`.

---

## Highlights

- **QA Quicklook dashboard** — a new ~30k-line PySide6 / matplotlib operator
  app (`qa_quicklook/`) for live data-taking.
- **Streaming software trigger** — a two-stage DCR-weighted + RANSAC trigger
  with per-hit score sampling and data-driven cut recommendation.
- **Fitted Cherenkov physics in QA mode** — recodata reconstructs rings on
  hardware-trigger frames via a time window, producing photon yield (`N_γ`)
  and single-photon resolution (σ) even when the self-trigger is off.
- **Config-driven everything** — every tunable moved out of the source into
  TOML, editable live from the dashboard.

---

## QA Quicklook dashboard (`qa_quicklook/`)

A single-window operator console, launched with `python -m qa_quicklook.app`:

- **Run Info** — *Run Manager* (run lifecycle, calibration anchor overrides,
  pulser-frequency override), *Database* (per-run metadata, multi-year), and
  *Runlists* (campaign-tagged run grouping).
- **QA** — per-stage thumbnail galleries (Lightdata / Recodata / Recotrack /
  Macros) rendered from each stage's canonical `.root`, with an inspect dialog
  and an interactive **streaming-score n_σ picker** that writes the chosen
  threshold back to the run database.
- **Multi-run trends + scatter** — `N_γ` / σ / rate trends across a runlist and
  a configurable QA-quantity-vs-beam-info scatter view.
- **Settings** — live editor for every `conf/*.toml` plus the dashboard's own
  `qa_quicklook.toml`, with an optional hidden *Advanced QA* tab.
- **Acquisition** — a scheme-aware backend (rsync / https) for pulling runs.
- **Cross-shifter sync** — optional Google-Sheets export of run metadata, and
  desktop notifications on pipeline completion.

---

## Streaming software trigger

- **Stage 1 — DCR-weighted score.** A per-hit score over a sliding time window,
  weighted by each channel's dark-count rate, fires the first-level trigger on
  a hit excess.
- **Stage 2 — RANSAC ring finder.** Runs on triggered frames, votes Cherenkov
  hits into a RANSAC accumulator, extracts up to two rings, and tags
  ring-member hits. Centre/radius refinement was moved out of this stage into
  the recodata re-fit.
- **Cut tuning.** Three score samples — first-frames DCR (noise), data-taking,
  and a pre-trigger in-beam background — are histogrammed per hit. A tail-fit
  extrapolation recommends the production n_σ cut; the dashboard picker lets the
  shifter set it per run. In QA mode the trigger is disabled (`n_σ = 1e9`) so
  the score histograms still fill without gating the data.

---

## Reconstruction pipeline

- **Modular writers.** `src/writers/recodata/{ring_compute, frame_pipeline,
  radial_fit, sigma_vs_n_fit}` and `src/writers/lightdata/…` replace the
  in-function lambdas, with a parallel per-frame compute pass and a serial
  histogram drain.
- **Hardware-trigger ring reconstruction.** Because QA mode disables the
  streaming/RANSAC self-trigger (which tagged ring hits), recodata now
  reconstructs rings from non-afterpulse Cherenkov hits within an asymmetric
  ±Δt window around the hardware-trigger reference time
  (`compute_ring_fit_timewindow`, knobs `hardware_ring_dt_min_ns` /
  `hardware_ring_dt_max_ns`). This yields rings — and therefore `N_γ` / σ — in
  QA mode.
- **Photon counting.** A Gaussian + pol3 radial fit with a sideband-subtracted
  `N_γ` cross-check; ring-centre X/Y projections; per-ring σ from a σ-vs-N fit.
  The fitted `N_γ` / σ are published to `AnalysisResults` for the multi-run
  trends.

---

## Timing & calibration

- **Timing trigger as a hardware trigger.** Per-chip alive-channel coincidence
  plus a chip0–chip1 Δt window, now fully configured via `[readout.timing]`
  (`alive_channels`, `delta_center_ns`, `delta_window_ns`, `delta_n_sigma`)
  instead of hardcoded constants. A timing chip-coincidence map annotates the
  highest-occupancy working point so the alive-channel counts can be set from
  the data.
- **Online calibration in QA mode** via per-hit `get_phase`, with a
  per-spill-update toggle for the wall-time/precision trade-off.
- **Pulser calibration pipeline** — `pulser_calib_writer` produces a fine-time
  calibration (`fine_calib.toml`) from a pulser run, with operator anchor and
  pulser-frequency overrides.

---

## Engineering & infrastructure

- C++17 rework and a reworked global channel index.
- Two-pass TTree memory-buffer optimisation; ROOT library-loading and
  TCanvas-segfault fixes.
- All writers now force ROOT batch mode at startup, so rendering QA PDFs no
  longer pops focus-stealing canvas windows.
- `AnalysisResults` migrated from a ROOT `TTree` backend to a keyed map.
- GitHub Actions compliance and PR-level formatting checks.

---