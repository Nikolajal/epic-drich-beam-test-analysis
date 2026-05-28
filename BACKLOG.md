# Backlog

Single canonical queue.  Format and taxonomy are defined in
[`DISCUSSION.md` → § Triage taxonomy](DISCUSSION.md#triage-taxonomy).

**Tag line:**

```
[<TYPE> · F<f> L<l> I<i> · <SCOPE> · <STATUS> · <DOMAIN> · P <p>]  <short title>
```

- `TYPE` ∈ {Bug, Liability, Vuln, Patch, Feature, Schema}
- `F` = `6·[F/(F+1) − 0.5]`,  `L` = `min(log₁₀(LOC+1), 3)`, both ∈ [0, 3]
- `I` ∈ {1, 2, 3}
- `SCOPE` ∈ {NOW, CAMPAIGN, LATER}
- `STATUS` ∈ {READY, INVESTIGATE}
- `DOMAIN` ∈ {C++, Dash, Macro, Conf, Doc, CI}
- `P` = `(Sev × Imp) / max(F + L, 1)` for READY; `?` for INVESTIGATE
  (sorted within Purgatory by `Sev × Imp`).

Cost estimates are best-effort; refine when picking up.

---

## READY  (sorted by Priority ↓)

```
[Schema · F1.5 L1.71 I3 · LATER    · READY · C++   · P 2.80]  Detector-evolution: flip kUsesSplitInTwo when 64-ch hardware lands; regress; deprecate split-in-two framer adapter
[Liability · F0.0 L1.71 I2 · CAMPAIGN · READY · C++   · P 2.34]  alcor_recotrackdata.h: calculate_angle_resolution returns -1 (stub) — implement proper error propagation
[Feature · F0.0 L2.00 I2 · CAMPAIGN · READY · C++   · P 2.00]  lightdata_writer: 5 grouped @todos (FIFO config, single/multi-core test, afterpulse plot, QA restructure, external config)
[Feature · F0.0 L2.40 I2 · CAMPAIGN · READY · Dash  · P 1.67]  qa: "Show history" right-click on runcard field → tail audit log for that (run, field)
[Feature · F0.0 L2.48 I2 · CAMPAIGN · READY · C++   · P 1.61]  lightdata: anchor-Δ vs spill PDF per trigger (task #152, deferred earlier)
[Patch · F0.0 L1.32 I2 · CAMPAIGN · READY · CI    · P 1.52]  Wire tools/lint_codebase.py + tools/check_qa.py into a pre-push hook / CI step
[Liability · F0.0 L1.49 I1 · LATER    · READY · C++   · P 1.34]  ring_model.h: clip_phi throws std::logic_error("not implemented") — implement
[Feature · F1.0 L2.00 I2 · LATER    · READY · C++   · P 1.33]  recodata: sensor-model split (k1350 / k1375) in QA histograms
[Feature · F1.0 L2.00 I2 · LATER    · READY · Dash  · P 1.33]  qa_quicklook: rundb.results_update Python wrapper for operator-entered fit overrides
[Feature · F1.0 L1.91 I2 · CAMPAIGN · READY · C++   · P 1.37]  pulser_calib: propagate per-TDC residual σ into the published calibration (sigma_a placeholder)
[Feature · F1.0 L2.18 I2 · LATER    · READY · C++   · P 1.26]  recodata: φ-gap split (in_gap / ex_gap) in Rings/ QA histograms
[Schema · F2.5 L3.00 I2 · LATER    · READY · C++   · P 1.09]  D-08 round 2 — two-layer migration: AlcorRecodata (10 macros), AlcorRecotrackdata (3 files), AlcorData
[Feature · F1.5 L2.40 I2 · LATER    · READY · C++   · P 1.03]  recodata: Stage 2 multithreading (frames-within-spill on top of the per-spill serial loop)
[Feature · F1.5 L2.48 I2 · LATER    · READY · Dash  · P 1.01]  qa: replace _MacrosPlaceholder with a real curated ROOT-macro launcher
[Bug · F0.0 L0.78 I1 · LATER    · READY · C++   · P 1.00]  recodata_writer:353 — N>25 bin-range; widen if a future run sees it
[Bug · F0.0 L1.04 I1 · LATER    · READY · C++   · P 0.96]  recodata_writer:430 — cx/cy half-range hardcoded "25 mm for now"; promote to streaming_hough_cfg
[Bug · F0.0 L1.04 I1 · LATER    · READY · C++   · P 0.96]  pulser_calib_writer:1092 — place anchor-Δ histogram explicitly under Diagnostics/ subfolder
[Bug · F0.0 L1.04 I1 · LATER    · READY · Dash  · P 0.96]  runlist.py:1 — drop stale "read-only scaffold (v1)" header; module is editable now
[Patch · F0.0 L1.20 I1 · LATER    · READY · Dash  · P 0.83]  toml_model.py:47 — DELETE sentinel "Not used in v1 of Settings form" — either wire (checkbox-uncheck) or remove
[Liability · F1.0 L1.49 I1 · CAMPAIGN · READY · C++   · P 0.80]  recotrackdata_writer:92 — "recodata MUST be initialised earlier, may break" (init-order safety)
[Patch · F0.0 L1.32 I1 · LATER    · READY · C++   · P 0.76]  mapping: forward cache built lazily with default params — surface the constructor knobs to callers
[Feature · F1.0 L1.91 I1 · LATER    · READY · Dash  · P 0.69]  Settings: "Save as setting set" stub — package working overlay into conf/sets/<name>/
[Patch · F1.0 L1.91 I2 · LATER    · READY · C++   · P 0.69]  utility/qa_publish.h helper + uniform pdf_path(run_dir, step, order, name) convention across writers
[Schema · F2.71 L1.91 I1 · LATER    · READY · C++   · P 0.65]  Rename include/util/ → include/utility/ (matches umbrella header, mirrors triggers/)
[Feature · F1.0 L2.18 I1 · LATER    · READY · Dash  · P 0.63]  qa: radiators editing in Run-info card (read-only in v1)
[Patch · F1.0 L1.49 I1 · LATER    · READY · C++   · P 0.40]  parallel_streaming_framer: frame-size constants are macros — promote to config
[Patch · F1.5 L1.20 I1 · LATER    · READY · Macro · P 0.37]  3× macros/examples/{photon_number_memory,ring_spatial_resolution{,_with_tracking}}.cpp — add a sensor-type flag
[Patch · F1.0 L1.91 I1 · LATER    · READY · C++   · P 0.34]  triggers/streaming: refresh QA — n_σ_streaming vs n_rings_found; per-ring radius overlays; Δt between stages
[Patch · F1.8 L1.32 I1 · LATER    · READY · C++   · P 0.32]  AnalysisResults: per-campaign file isolation — <repo>/standard_results.root → run-lists/<year>.results.root convention
[Patch · F2.0 L1.71 I1 · LATER    · READY · Conf  · P 0.27]  Re-write legacy text-based sections of config_reader.h consumers for full TOML configuration
[Patch · F2.14 L1.71 I1 · LATER    · READY · C++   · P 0.26]  Phase 2/3/4/5 marker sweep across lightdata + recodata + mapping (most landed — confirm + drop dated breadcrumbs)
[Patch · F1.8 L2.30 I1 · LATER    · READY · Doc   · P 0.24]  Add README.md + DISCUSSION.md to include/util/ and include/writers/ (matches triggers/ pattern)
```

---

## Purgatory (INVESTIGATE — sorted by `Sev × Imp` ↓)

These need a thought / measurement / design before they can be costed.
Once someone graduates one to READY (estimates F + LOC), it joins the
main backlog above at its earned priority.

```
[Schema · F? L? I3 · CAMPAIGN · INVEST · CI    · P? S·I=9]  CI workflow gated off (apt root-system gone; conda Windows churn) — pick a re-enable strategy
[Schema · F? L? I3 · LATER    · INVEST · Dash  · P? S·I=9]  Cross-shifter sync — Google Sheets push as the path of least resistance (4 candidates listed)
[Schema · F? L? I2 · LATER    · INVEST · C++   · P? S·I=6]  D-02 — histogram lifecycle (bare new vs unique_ptr vs helper wrapper); macro audit pending
[Schema · F? L? I2 · LATER    · INVEST · C++   · P? S·I=6]  D-05 — introduce an `event` class consolidating ring + circle + timing + afterpulse + crosstalk (5 sub-questions)
[Schema · F? L? I2 · LATER    · INVEST · C++   · P? S·I=6]  γ-mode (Stage 2) in pulser_calib — per-TDC aggregation across multiple TDCs
[Schema · F? L? I2 · LATER    · INVEST · C++   · P? S·I=6]  Consolidate writer/reader entry points into a single class hierarchy (lightdata.h:10)
[Feature · F? L? I3 · CAMPAIGN · INVEST · C++   · P? S·I=6]  Time-aware ring hit assignment (TODO #33); pure-spatial today; needs Δt cut
[Feature · F? L? I3 · CAMPAIGN · INVEST · C++   · P? S·I=6]  Time-aware hit assignment / dedup in Hough output (3 candidate options listed)
[Liability · F? L? I3 · CAMPAIGN · INVEST · C++   · P? S·I=6]  pulser_calib: ±0.5 cc satellites in published b — edge-quantisation artefact vs real hardware slip?
[Schema · F? L? I2 · LATER    · INVEST · C++   · P? S·I=6]  TriggerEvent schema extension — add GlobalIndex / device-fifo-channel so we can ask "which physical channel emitted this trigger?"
[Schema · F? L? I2 · LATER    · INVEST · C++   · P? S·I=6]  Merge AlcorFinedata + AlcorData (deferred per DISCUSSION:38)
[Schema · F? L? I2 · CAMPAIGN · INVEST · Dash  · P? S·I=6]  Migrate AnalysisResults primary from ROOT TTree → TOML (sibling .toml exists, drop ROOT once readers migrate)
[Liability · F? L? I2 · CAMPAIGN · INVEST · C++   · P? S·I=4]  D-10 — Mapping + RunInfo static-map / no-mutex / float-key-of-array fragility (3 decisions)
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  Streaming v2: conservative DCR estimator (75th-percentile)
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  Streaming v2: crosstalk correction in the score stage
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  Streaming v2: signal-aware weighting w_i = s_i / λ_i (multi-pass bootstrap)
[Liability · F? L? I2 · CAMPAIGN · INVEST · C++   · P? S·I=4]  Hough: hough_threshold formula needs re-derivation post-Stage-1 gating
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  Hough: merge near-duplicate rings before clamping to max_rings=2
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  Hough: is "ring 2" an elliptic deformation of ring 1? (macros/examples/elliptic_investigation.cpp V1)
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  Hough: per-ring (X, Y, R) sanity cuts — 3 independent guards proposed
[Liability · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  RootHist thread-safety polish for the per-thread clone pattern in Stage 2 mt
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  recodata: ring → radiator labelling (needs per-run beam metadata)
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  recodata: multiple time windows in QA (prompt / early / DCR)
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  In-writer N_γ fit (Crystal-Ball + pol3) — kept in macro for V1
[Liability · F? L? I2 · CAMPAIGN · INVEST · C++   · P? S·I=4]  pulser_calib: regime-2 slip detection sensitivity to coarse-edge quantisation (pair-difference test)
[Liability · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  fit_circle design review (initial values, fix_XY granularity, named-struct return, round-trip test)
[Liability · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  Pulser-calib: replace fine-band filter with IRLS / M-estimator inside the fit itself
[Feature · F? L? I2 · LATER    · INVEST · Dash  · P? S·I=4]  qa: Advanced QA tab (currently _PlaceholderTab gated by [ui].show_advanced_qa)
[Feature · F? L? I2 · LATER    · INVEST · Dash  · P? S·I=4]  qa: per-run progress events via lapsed [QA] {json} push protocol
[Feature · F? L? I2 · LATER    · INVEST · Dash  · P? S·I=4]  qa: embedded ROOT canvases (Inspect currently spawns separately; embedding has thread-loop cost)
[Feature · F? L? I2 · LATER    · INVEST · Dash  · P? S·I=4]  qa: cross-run trend plots in the QA tab
[Feature · F? L? I2 · LATER    · INVEST · Dash  · P? S·I=4]  qa: quality-tagging cockpit integrated with Runlist
[Feature · F? L? I2 · LATER    · INVEST · C++   · P? S·I=4]  TODO: dynamically determine timing cuts in lightdata (currently hardcoded)
[Schema · F? L? I1 · LATER    · INVEST · C++   · P? S·I=3]  MIST nn_transform — NN-based ring finder as a Hough alternative
[Patch · F? L? I2 · LATER    · INVEST · C++   · P? S·I=2]  Streaming v2: multiplicity upper-bound cut (flagged 2026-Q2)
[Feature · F? L? I1 · LATER    · INVEST · C++   · P? S·I=2]  Hough: centroid of per-hit predicted-centre locus refinement
[Feature · F? L? I1 · LATER    · INVEST · C++   · P? S·I=2]  Hough: least-squares circle fit refinement (existing fit_circle)
[Feature · F? L? I1 · LATER    · INVEST · C++   · P? S·I=2]  Hough: iterative re-association (refine → re-collect → converge)
[Feature · F? L? I1 · LATER    · INVEST · C++   · P? S·I=2]  recodata: per-event coverage map (if centre-wander discrepancy becomes problem)
[Feature · F? L? I1 · LATER    · INVEST · C++   · P? S·I=2]  Macros: ACLiC-clean state for macros/examples/ → unlocks CI compile-check
[Feature · F? L? I1 · LATER    · INVEST · C++   · P? S·I=2]  recodata: finer-analysis variants (kept under "finer-analysis follow-up")
[Patch · F? L? I1 · LATER    · INVEST · C++   · P? S·I=1]  alcor_spilldata.h:250 — placeholder method "reserved for future use" — wire or delete
[Bug · F? L? I1 · LATER    · INVEST · C++   · P? S·I=1]  recotrackdata_writer:112 — "TODO: understand this" (min(n_frames, max_frames) clamp)
[Patch · F? L? I1 · LATER    · INVEST · C++   · P? S·I=1]  Bit-wise manipulation TODOs in alcor_data.h:20 + alcor_finedata.h:78
[Patch · F? L? I1 · LATER    · INVEST · C++   · P? S·I=1]  mapping.h: full-coverage map (Cartesian + R-φ) + cache-vs-on-the-fly profiling
[Patch · F? L? I1 · LATER    · INVEST · C++   · P? S·I=1]  config_reader.h:327 — skipped-channel list is a TNamed; structured representation would help downstream consumers
[Bug · F? L? I1 · LATER    · INVEST · C++   · P? S·I=1]  recodata:592 — "not yet active on the first pass through"
[Patch · F? L? I1 · LATER    · INVEST · C++   · P? S·I=1]  alcor_finedata.h:152 — AlcorV2FitCalib reserved for a future fit-based correction
[Patch · F? L? I1 · LATER    · INVEST · Conf  · P? S·I=1]  readout_config.toml: per-device sensor-type override (currently per-role only)
[Patch · F? L? I1 · LATER    · INVEST · C++   · P? S·I=1]  global_index.h:21,40 — future v2 GlobalIndex layout escape hatch (kept as design hedge)
```

---

## DROPPED

Tracked here so they don't reappear as discoveries in future sweeps.

```
[DROPPED · C++]  D-12 extension to timing + tracking detectors — Cherenkov-specific motivation (DCR-driven score); other detectors don't share the same noise structure.  Decided 2026-05-28.
```

---

*Generated 2026-05-28 from the comprehensive repo sweep + taxonomy chapter in `DISCUSSION.md`.  Refresh by running another sweep and re-applying the formulas.*
