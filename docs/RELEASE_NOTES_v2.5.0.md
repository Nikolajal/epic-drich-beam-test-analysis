# v2.5.0 — elliptical ring reconstruction + per-trigger N_γ

Backward-compatible (MINOR): new QA plots; new CLI flags default to the prior
behaviour; a backward-compatible timing-decode fix. No change to the
`AnalysisResults` / `standard_results.toml` key scheme or the config schema.

## Cherenkov reconstruction (recodata)
- **Per-trigger N_γ** — photon yield per hardware trigger: each trigger's
  in-window radial is normalised to that trigger's frame population
  (photons / triggered frame), shown raw + eff(R)-corrected, grouped under the
  trigger chapter in the dashboard.
- **Elliptical ring fit** — a geometric aggregate ellipse fit (r(φ) profile
  about the pinned centre) with an automatic circle fallback. The per-trigger
  radial is then measured in the elliptical radius ρ (R → ρ; the fit model is
  unchanged), which removes optical a−b smearing — σ 2.55 → 2.22 mm on a
  ~10 %-elliptical ring, with N_γ preserved. New `--force-ring` /
  `--force-ellipse` flags override the auto classification (default: auto).
- **Ring centre** — 0.25 mm centre-map binning + Gaussian-core centroid.
- **QA-plot rework** — shared 2D-map-with-projections panel; readable 10/50/90 %
  projection ticks + grid with rotated Y-projection labels; flat (shadowless)
  stat boxes in the free corner; hitmap ellipse/circle overlay with a/b/θ axes.
  Removed the per-ring ellipse plots (unmeasurable at ~15 hits/ring) and the
  redundant N_gamma_per_ring plot.

## Timing decode (framer)
- **Timing hits always decode leading-edge**, even in ToT runs: timing edges
  bypass the ToT edge-pairing buffer and the ToT-as-LET odd-edge drop, so the
  timing sub-detector is never paired regardless of the run's op_mode.

## ToT QA (lightdata)
- **dt_vs_tot 1 p.e. vs 2 p.e. Δt overlay** — a per-sensor companion plot
  (`dt_vs_tot_<trigger>_<sensor>_pe`) overlaying the two area-normalised Δt
  projections within their ToT bands, annotated with the Δ⟨Δt⟩ time-walk shift.
