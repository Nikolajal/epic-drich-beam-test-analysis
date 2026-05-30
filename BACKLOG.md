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

**DISCUSSION-entry rule:** any READY row with **Sev ≥ 2 *and* Imp ≥ 2**
needs a DISCUSSION entry in the nearest area satellite
(`include/<area>/DISCUSSION.md`, `qa_quicklook/DISCUSSION.md`, etc.)
before it leaves READY.  Severity mapping: Bug = Patch = 1, Liability =
Feature = 2, Vuln = Schema = 3.  Patches and Sev≥2/I1 rows can ship
without an entry — their scope is small enough that the row text
itself is the spec.

---

## READY  (sorted by Priority ↓)

```
[Liability · F0.0 L1.71 I2 · CAMPAIGN · READY · C++   · P 2.34]  alcor_recotrackdata.h: calculate_angle_resolution returns -1 (stub) — implement proper error propagation
[Schema · F2.0 L2.30 I3 · CAMPAIGN · READY · CI    · P 2.09]  CI workflow re-enable: pick a strategy (Linux-only / conda-pinned / GH self-hosted) and ship it
[Feature · F2.0 L2.30 I3 · CAMPAIGN · READY · C++   · P 2.09]  Time-aware hit handling — Δt cut at writer (TODO #33) + Hough dedup (pick from 3 implementation options)
[Feature · F0.0 L1.91 I2 · CAMPAIGN · READY · C++   · P 2.09]  Streaming-score: three named time-window score samples (prompt / early / DCR) for the QA tail-comparison plot.  Re-scoped 2026-05-30: the row title used to say "recodata" but the intent is the streaming-score path in `src/triggers/streaming/score.cxx`, not recodata's Δt-Cherenkov panel.  Today the writer fills two score samples: `h_streaming_score_noise` from the first-frames window (DCR sample) and `h_streaming_score_data` from the post-first-frames window (mixed early + prompt).  Split into three:  **DCR** = first-frames (existing); **early** = post-first-frames windows where no trigger fires (in-beam-but-not-signal sideband, anchors the in-beam baseline this morning's commits compute via `compute_streaming_inbeam_rates`); **prompt** = windows that DO fire a streaming-ring trigger (the signal sample).  Lets the analyser read prompt-vs-early separation directly off the QA plot when picking the production `n_sigma_threshold` — the existing 2-curve overlay collapses the early sideband into the data curve.  Path: `src/triggers/streaming/score.cxx::run_streaming_trigger_weighted` (the score loop) + `src/lightdata_writer.cxx` (caller picks which TH1F to fill).
[Feature · F0.0 L2.00 I2 · CAMPAIGN · READY · C++   · P 2.00]  lightdata_writer: 5 grouped @todos (FIFO config, single/multi-core test, afterpulse plot, QA restructure, external config)
[Liability · F1.0 L1.00 I2 · CAMPAIGN · READY · C++   · P 2.00]  Hough: hough_threshold formula needs re-derivation post-Stage-1 gating
[Feature · F0.0 L2.40 I2 · CAMPAIGN · READY · Dash  · P 1.67]  qa: "Show history" right-click on runcard field → tail audit log for that (run, field)
[Feature · F0.0 L2.48 I2 · CAMPAIGN · READY · C++   · P 1.61]  lightdata: anchor-Δ vs spill PDF per trigger (task #152, deferred earlier)
[Liability · F1.0 L1.49 I2 · CAMPAIGN · READY · C++   · P 1.60]  pulser_calib: regime-2 slip detection sensitivity — pair-difference test against coarse-edge quantisation
[Schema · F1.5 L2.30 I2 · LATER    · READY · C++   · P 1.58]  γ-mode (Stage 2) in pulser_calib — per-TDC aggregation across multiple TDCs
[Patch · F0.0 L1.32 I2 · CAMPAIGN · READY · CI    · P 1.52]  Wire tools/lint_codebase.py + tools/check_qa.py into a pre-push hook / CI step
[Liability · F1.0 L1.71 I2 · LATER    · READY · C++   · P 1.48]  RootHist thread-safety polish for the per-thread clone pattern in Stage 2 mt
[Liability · F1.0 L1.71 I2 · CAMPAIGN · READY · C++   · P 1.48]  pulser_calib: ±0.5 cc satellites in published b — disambiguate edge-quantisation artefact vs hardware slip
[Feature · F1.0 L1.71 I2 · LATER    · READY · C++   · P 1.48]  Hough: is "ring 2" an elliptic deformation of ring 1? — run macros/examples/elliptic_investigation.cpp V1, measure
[Feature · F1.5 L1.91 I2 · CAMPAIGN · READY · C++   · P 1.18]  recodata writer (src/writers/recodata/radial_fit.cxx): restore Crystal-Ball + pol3 with **explicit analytic CB integral** for N_γ.  Switched away from CB on 2026-05-27 because (α, n) rail-locked on low-stats / broad samples (esp. ring 2); analytic-integral path sidesteps that — α, n can be fixed at physical seeds (or fit loosely) and N_γ extracted from the closed form (erf on Gaussian core + power-law antiderivative on α-tail) instead of relying on a stable fit.  Compare against a freshly regenerated Gauss+pol3 baseline (re-run recodata on the standard run; the old snapshot dir is gone).
[Feature · F1.0 L1.79 I2 · LATER    · READY · C++   · P 1.43]  Streaming v2: conservative DCR estimator (75th-percentile, robust against outliers)
[Feature · F1.0 L1.91 I2 · CAMPAIGN · READY · C++   · P 1.37]  pulser_calib: propagate per-TDC residual σ into the published calibration (sigma_a placeholder)
[Feature · F1.0 L1.91 I2 · LATER    · READY · C++   · P 1.37]  qa: per-run progress events via lapsed [QA] {json} push protocol — **writer side only**.  Dashboard-side reader shipped 2026-05-30 (qa_quicklook/app.py:_QaPipelineProgressBar drives a 3-segment status-bar strip off qa_pipeline.parse_progress_line / consume_progress_buffer; qa_pipeline.py already emits the per-stage {qa_pipeline:{stage,state,exit_code,wall_s,new_pdfs}} envelope under --json).  Remaining work is per-spill push from the C++ writers (lightdata / recodata / recotrack) so the strip can show sub-stage progress instead of only stage-boundary transitions.
[Feature · F1.0 L1.91 I2 · CAMPAIGN · READY · C++   · P 1.37]  Hough: merge near-duplicate rings before clamping to max_rings=2 (handles cell-boundary splits)
[Feature · F1.0 L1.91 I2 · CAMPAIGN · READY · C++   · P 1.37]  Hough: per-ring (X, Y, R) sanity cuts — 3 independent guards (quality ratio floor, geometry box, NaN handling)
[Liability · F1.5 L1.49 I2 · LATER    · READY · C++   · P 1.34]  fit_circle design review — initial-value validation, fix_XY granularity, named-struct return with χ²/ndf/status, round-trip test
[Patch · F0.0 L0.78 I1 · CAMPAIGN · READY · CI    · P 1.00]  Wire tools/check_qa.py into the dashboard QA-tab refresh path (lint_codebase.py shipped as `.github/workflows/lint.yml` 2026-05-29; check_qa.py needs writer output ROOTs which CI doesn't have, so it stays local + maybe dashboard-side).
[Liability · F1.0 L1.71 I2 · CAMPAIGN · READY · C++   · P 1.71]  Per-hit get_phase() overhead audit.  Diagnostic on 20251119-010426 5-spill: --QA-with-calibration-OFF wall = 104.4 s (cmp_b_qa); --QA-with-calibration-ON wall = 127.5 s (cmp_c_calib).  **Total delta = 23 s.**  generate_calibration instrumented to ~4.2 s (cold spill 0 = 3 380 ms / warm spills 2–4 = ~50 ms each), so **~19 s remains unaccounted for** and is the get_phase() residual.  Hypothesis confirmed by code inspection 2026-05-30: `get_phase()` (src/alcor_finedata.cxx:75) does TWO `std::unordered_map::find()` lookups per hit — one on `calibration_parameters`, one on `channel_calibration_method`.  When the maps are empty (--QA calibration_method=none), both `find()` return `end()` immediately and the function short-circuits.  With ~100M hits per 5-spill run, two lookups × ~95 ns each ≈ 19 s.  Concrete levers: (a) merge the two maps into one (`unordered_map<u32, struct{array<float,3>, CalibrationMethod}>`) — saves ~10 s, mechanical refactor; (b) replace `unordered_map` with `std::vector<entry>` indexed by GlobalIndex (key range is dense by construction) — saves ~15 s, drops to ~10 ns/hit; (c) bake calibration into the AlcorFinedataStruct at decode time — saves ~18 s, eliminates the per-hit lookup entirely (trades 12 B/hit of memory).  Path: src/alcor_finedata.cxx::get_phase (line 75); the frozen-table fast-path is already in place (line 91-92) so the lookup IS the floor.
[Feature · F1.5 L1.71 I2 · LATER    · READY · C++   · P 1.25]  ParallelStreamingFramer: partition the rollover into N parts (N = threads), route frames whose c_start lands in the same partition to the same thread.  Idea: keep all "frames at the same modular cc within different rollovers" co-located so any per-channel / per-rollover state stays single-writer, eliminating cross-thread synchronisation on hot per-channel paths (e.g. AlcorFinedata phase tables, rollover-correction lookups).  Audit current framer thread-fanout first — confirm partitioning is by stream/FIFO today, then design the rollover-partition rebalance.
[Feature · F1.0 L1.49 I1 · LATER    · READY · C++   · P 0.35]  AlcorFinedata::generate_calibration — refit retry policy (downgraded 2026-05-29 from low-stats cache after instrumented measurement showed the original target wasn't the bottleneck).  Instrumented findings on 20251119-010426 5-spill run: per-call cost decomposes as (cold spill 0 = 3380 ms total, of which Fit = 3197 ms / ProjectionY = 82 ms / other = 100 ms; warm spills 2-4 = ~50 ms each, of which Fit < 2 ms / ProjectionY ≈ 23 ms / other ≈ 25 ms).  Total calibration cost across 5 spills: ~4.2 s.  ProjectionY is NOT the bottleneck — the original low-stats cache proposal would save ~70 ms over 5 spills, not "10-15 s/spill" as initially estimated.  The real per-call lever is the Fit retry loop on cold spills (5882 fits incl. retries × ~544 µs each).  Two targeted edits: (C) jitter the edge seeds across the 5 retries instead of refitting with the same starting parameters — Minuit is largely deterministic from a fixed start, so the current retry policy rarely converges to a different answer; (E) reduce the retry cap from 5 to 3 after (C) lands, since the seed jitter should make convergence happen within the first 2-3 attempts.  Expected: -1.5 to -2 s on the cold spill, negligible on warm spills.  Total cost is small enough (~4 s / 5 spills) that this is no longer a P > 1 row.  Path: src/alcor_finedata.cxx (lines 411-425, the retry loop).
[Feature · F1.0 L2.00 I2 · LATER    · READY · C++   · P 1.33]  recodata: sensor-model split (k1350 / k1375) in QA histograms
[Feature · F1.0 L2.00 I2 · LATER    · READY · Dash  · P 1.33]  qa_quicklook: rundb.results_update Python wrapper for operator-entered fit overrides
[Feature · F1.0 L2.18 I2 · LATER    · READY · C++   · P 1.26]  recodata: φ-gap split (in_gap / ex_gap) in Rings/ QA histograms
[Feature · F1.5 L1.71 I2 · LATER    · READY · C++   · P 1.25]  Streaming v2: crosstalk correction in the score stage
[Feature · F1.5 L1.71 I2 · LATER    · READY · C++   · P 1.25]  Streaming v2: signal-aware weighting w_i = s_i / λ_i (multi-pass bootstrap)
[Liability · F1.5 L1.71 I2 · LATER    · READY · C++   · P 1.25]  Pulser-calib: replace fine-band filter with IRLS / M-estimator inside the fit itself
[Feature · F1.5 L1.71 I2 · LATER    · READY · C++   · P 1.25]  TODO: dynamically determine timing cuts in lightdata (currently hardcoded)
[Schema · F2.5 L2.70 I2 · LATER    · READY · C++   · P 1.15]  D-05 — introduce an `event` class consolidating ring + circle + timing + afterpulse + crosstalk (5 sub-questions to resolve on pickup)
[Schema · F2.5 L3.00 I2 · LATER    · READY · C++   · P 1.09]  D-08 round 2 — two-layer migration: AlcorRecodata (10 macros), AlcorRecotrackdata (3 files), AlcorData
[Liability · F2.0 L1.71 I2 · CAMPAIGN · READY · C++   · P 1.08]  D-10 — Mapping + RunInfo static-map / no-mutex / float-key-of-array fragility (3 decisions to resolve on pickup)
[Feature · F1.5 L2.40 I2 · LATER    · READY · C++   · P 1.03]  recodata: Stage 2 multithreading (frames-within-spill on top of the per-spill serial loop)
[Feature · F2.0 L2.40 I2 · LATER    · READY · C++   · P 0.98]  lightdata: frame-level multithreading — same compute/drain split as recodata Stage 2, applied to the serial post-framer per-frame loop (lightdata_writer.cxx:1009 `for frame_id : main_sorted_keys`).  Heavier per-thread state than recodata's fit_circle: N HoughTransform instances each with own LUT (~5-50 MB) + accumulator, per-thread hit-mask scratch + hist clones, serial drain at spill end.  Cheap `ROOT::EnableThreadSafety()` already shipped.  Audit (5 spills): ~290 s of the ~360 s wall is serial streaming/Hough on the main thread; with both OFF the same run is 54 s.  PREREQ DECISION: the streaming n_sigma over-firing (firing ~8× physical rate) generates most of that serial Hough work — tune the threshold FIRST and re-measure before committing to the MT refactor; it may render this much less urgent.
[Patch · F1.0 L1.20 I2 · LATER    · READY · C++   · P 1.10]  recodata: redirect the radial_fit / sigma_vs_n_fit per-hist PDFs from the run root into `qa/recodata/`.  Today `src/writers/recodata/radial_fit.cxx` (`<run>/<tag>.pdf`, `<tag>_logy.pdf`) and `sigma_vs_n_fit.cxx` (`<run>/<hist>_sigma_vs_n.pdf`) `SaveAs` straight into the run directory, so the dashboard stage-scanner never sees them AND the qa_pipeline `--clean` has to sweep `h_*.pdf` from the run root.  Route them through `util::qa::pdf_path(run_dir, "recodata", order, name)` like the curated set; drop the `h_*.pdf` glob from `_CLEAN_REMOVE_GLOBS` once done.
[Feature · F1.5 L2.48 I2 · LATER    · READY · Dash  · P 1.01]  qa: replace _MacrosPlaceholder with a real curated ROOT-macro launcher
[Feature · F2.0 L2.00 I2 · LATER    · READY · C++   · P 1.00]  recodata: ring → radiator labelling (requires per-run beam-metadata schema as prereq)
[Feature · F2.0 L2.00 I2 · LATER    · READY · Dash  · P 1.00]  qa: quality-tagging cockpit integrated with Runlist (both pieces exist, glue layer)
[Patch · F1.0 L1.32 I2 · LATER    · READY · C++   · P 0.86]  Streaming v2: multiplicity upper-bound cut (flagged 2026-Q2)
[Feature · F1.0 L1.91 I1 · LATER    · READY · Dash  · P 0.69]  Settings: "Save as setting set" stub — package working overlay into conf/sets/<name>/
[Patch · F1.0 L1.91 I2 · LATER    · READY · C++   · P 0.69]  utility/qa_publish.h helper + uniform pdf_path(run_dir, step, order, name) convention across writers
[Feature · F1.0 L2.18 I1 · LATER    · READY · Dash  · P 0.63]  qa: radiators editing in Run-info card (read-only in v1)
[Patch · F1.0 L1.00 I1 · LATER    · READY · Conf  · P 0.50]  readout_config.toml: per-(device, chip) sensor-type override (currently per-role only).  Per-chip is the realistic granularity ceiling — a single ALCOR chip never carries two sensor models.
[Feature · F3.0 L1.48 I1 · LATER    · READY · C++   · P 0.45]  Macros: ACLiC-clean state for macros/examples/ → unlocks CI compile-check
[Patch · F1.0 L1.49 I1 · LATER    · READY · C++   · P 0.40]  mapping.h: full-coverage map (Cartesian + R-φ) + cache-vs-on-the-fly profiling
[Patch · F1.5 L1.20 I1 · LATER    · READY · Macro · P 0.37]  3× macros/examples/{photon_number_memory,ring_spatial_resolution{,_with_tracking}}.cpp — add a sensor-type flag
[Patch · F1.0 L1.71 I1 · LATER    · READY · C++   · P 0.37]  config_reader.h:327 — skipped-channel list is a TNamed; structured representation would help downstream consumers
[Patch · F1.0 L1.71 I1 · LATER    · READY · C++   · P 0.37]  triggers/streaming: refresh QA REMAINING — per-ring QA landed (X/Y/R + arc-distance) via Hough refactor; still TODO: n_σ_streaming vs n_rings_found correlation hist, radius overlay across rings, Δt between stages.  Cross-checked 2026-05-29.
[Patch · F2.0 L1.71 I1 · LATER    · READY · Conf  · P 0.27]  Re-write legacy text-based sections of config_reader.h consumers for full TOML configuration
```

---

## Purgatory (INVESTIGATE — sorted by `Sev × Imp` ↓)

Purgatory exhausted 2026-05-29.  Every item that had a defensible
F/L/I estimate (even a rough one) graduated to READY at its earned
priority — design uncertainty isn't a reason to keep something
uncosted forever; it's a refinement note for the implementer.  Items
that were truly speculative (placeholder UI, upstream dependencies,
observation-conditional re-adds, explicit "kept-as-decided" markers)
moved to DROPPED with the reason recorded.

Re-add items here only when a *new* design question surfaces that
genuinely blocks costing — not as a parking lot for low-conviction
ideas.

---

## DROPPED

Tracked here so they don't reappear as discoveries in future sweeps.

```
[DROPPED · C++]    D-12 extension to timing + tracking detectors — Cherenkov-specific motivation (DCR-driven score); other detectors don't share the same noise structure.  Decided 2026-05-28.
[DROPPED · C++]    Hough: centroid of per-hit predicted-centre locus refinement — alternative to sliding-window; sliding-window already shipped and covers the problem.
[DROPPED · C++]    Hough: least-squares circle fit refinement — alternative to sliding-window; same rationale.  fit_circle already exists in recodata for downstream use.
[DROPPED · C++]    Hough: iterative re-association — alternative to sliding-window; premature without evidence sliding-window is the bottleneck.
[DROPPED · C++]    recodata: finer-analysis variants — too vague ("more variants"); no concrete plan.  Re-add when a specific variant is on the table.
[DROPPED · C++]    alcor_spilldata.h:250 — "placeholder method reserved for future use" — empty stub; delete the placeholder.  Reinstate when there's a real use case.
[DROPPED · C++]    recodata_writer.cxx:592 — "not yet active on the first pass through" stale comment — verify-and-remove next time the function is touched.
[NOT DROPPED · C++]  alcor_finedata.h:152 — "AlcorV2FitCalib reserved for a future fit-based correction" — the enum is the production calibration method (used in finedata reader, pulser_calib writer, timing macro).  The comment is stale; fixing it is now a READY Bug.
[DROPPED · C++]    pulser_calib_writer:1092 anchor-Δ Diagnostics/ placement — verified: histogram already lives in Diagnostics/ via diag_dir->WriteObject (line 1865).  Comment-pointer was correct.
[DROPPED · C++]    recodata_writer:353 N>25 bin-range — code comment already encodes the watch-condition ("widen if a future run sees N > 25").  Not actionable without an observation; the code itself is the tracker.
[DROPPED · C++]    Wave 1 lands: recotrackdata_writer:112 (max_frames cap re-enabled + rationale), recodata_writer:430 (cx/cy half-range → streaming_hough_cfg.centre_xy_half_range_mm), Phase 2/3/4/5 marker sweep (8 stale breadcrumbs stripped, kept pulser_calib local Phase 3a/3b labels).

# Detector evolution 2026-05-29
[DROPPED · C++]    Detector-evolution: flip kUsesSplitInTwo when 64-ch hardware lands.  Re-opens the chapter when new electronics arrive — source-side comments are sufficient until then.
[DROPPED · C++]    ring_model.h::clip_phi — placeholder function that threw "not implemented" with zero callers.  Deleted from ring_model.h + the utility DISCUSSION pointer; reinstate only if a real consumer surfaces.
[DROPPED · C++]    TriggerEvent schema extension (GlobalIndex / device-fifo-channel fields).  Cross-checked 2026-05-29: the "which physical channel emitted this trigger?" question is answerable by walking the associated hits' GlobalIndex (time-windowed in src/triggers/streaming/hough.cxx).  Architecture took the implicit-mapping route; explicit field denormalisation was deliberately not taken.  Re-add only if a TriggerEvent-only consumer (e.g. TBrowser interactive inspection) needs the convenience.

# Comment-debt sweep 2026-05-29
[DROPPED · C++]    Bit-wise manipulation TODOs (alcor_data.h:20 + alcor_finedata.h:78).  Resolved: GlobalIndex (utility/global_index.h) owns identity packing; timing components (rollover/coarse/fine) intentionally stay as separate uint32_t (memory bloat acceptable, hardware emits this layout).  Stale @todos replaced with forward-pointers to GlobalIndex.
[DROPPED · Doc]    config_reader.h:14 file-level @todo "Re-write legacy text-based sections" — removed as too vague to action.  The remaining legacy-text consumer cleanup is tracked by its own Patch row (P 0.27 in READY).

# Mapping ctor knobs 2026-05-29
[DROPPED · C++]    Mapping forward-cache constructor knobs surfaced to callers (origin_cut + collision_policy).  Dropped: caller-side override is rare in practice (only the streaming-Hough LUT builder uses non-default cache params), and post-construct `build_*_cache(...)` calls already serve that path.  Constructor stays one-arg; the standalone builders carry the optional knobs.

# Cross-shifter sync shipped 2026-05-29 — full Sheets push + reverse-merge
[DROPPED · Dash]   Cross-shifter sync via Google Sheets push (P 2.37 from READY) — shipped 2026-05-29.  Implementation: `qa_quicklook/sheets_sync.py` (config + snapshot + render + cell-diff reverse-merge), `sheets_worker.py` (QThread driver), `_sheets_adapter.py` (lazy-imported google-api push/pull), `_sheets_cli.py` (`python -m qa_quicklook.sheets_sync` for dry-run / headless).  Status-bar wiring in `app.py`.  45 unit tests in `tests/test_qa_quicklook_sheets_sync.py`.  Setup runbook in `qa_quicklook/README.md`.  See qa_quicklook/DISCUSSION.md → "Cross-shifter sync — Google Sheets push (shipped 2026-05-29)" for the deferred follow-ups (editor identity, structural-field reverse-merge, DeveloperMetadata watermarks).

# Live-QA system shipped 2026-05-29 (4 backlog rows closed in one batch)
[DROPPED · Dash]   qa_pipeline wrapper — shipped 2026-05-29.  Implementation: `qa_quicklook/qa_pipeline.py` (~480 LOC, Python CLI + in-process `run_pipeline(opts, repo_root, callbacks)` for the dashboard's button).  Stop-on-first-failure with exit codes 10/20/30 per stage; skip-stage-when-existing-output; manifest JSON at `Data/<RUN>/qa/qa_pipeline_manifest.json`; --notify macos|stdout|file + libnotify fallback on Linux; joblock guard rejects same-run double-launches.  9 tests in `tests/test_qa_pipeline.py` covering happy path + every failure branch.
[DROPPED · Dash]   qa_pipeline two-tier auto-cleanup — shipped 2026-05-29.  Implementation: `qa_quicklook/retention.py` (pure-function sweep+apply, ~210 LOC).  Settings exposes `[retention] full_keep_n / qa_keep_n / keep_recotrackdata / sweep_on_startup`.  Triggers: `MainWindow._retention_sweep_on_startup` (gated by toml + `QAQ_DISABLE_STARTUP_SWEEP` env + `QT_QPA_PLATFORM=offscreen` → dry-run); `runmanager._on_download` (pre-download).  Joblock filter skips runs with active locks.  19 tests in `tests/test_retention.py` covering tier transitions + edge cases.
[DROPPED · Dash]   Remote-run picker live-run guard — shipped 2026-05-29.  Implementation: `download.newest_run_id()` shared helper; `_RemoteRunsPickerDialog` badges the lex-newest row 🔴 + orange foreground + tooltip; modal QCheckBox blocks OK until shifter confirms data-taking has stopped.  Older runs skip the prompt (sealed by construction).
[DROPPED · Dash]   Live data-taking auto-QA monitor — shipped 2026-05-29.  Implementation: `qa_quicklook/remote_watcher.py` (QObject-on-QThread following sheets_worker.py template, ~200 LOC, state machine with stability quorum).  Orchestration in `app.py`: `_build_remote_watcher` + 8 handlers wire watcher → rsync QProcess → qa_pipeline subprocess → distinct Hero.aiff notification (vs Glass for manual).  Live-reload of watcher on Settings toggle via `_on_dashboard_config_changed_livemon`.  Error handler shows modal + auto-stops worker.  11 tests in `tests/test_remote_watcher.py` covering cold-start, transitions, flap rejection, error propagation.

# Available-runs picker in the rsync Download flow — shipped 2026-05-29
[DROPPED · Dash]   Run Manager Download button: SSH-listing picker dialog instead of typed-id input — shipped 2026-05-29 (ad-hoc, not from a backlog row).  Implementation: `download.list_remote_runs()` + `_RemoteRunsPickerDialog` in `runmanager.py`.  Sorted newest-first, "✓ local" badge on already-mirrored runs, "Type id manually…" fallback escape hatch.  14 new tests in `tests/test_qa_quicklook_download_extras.py`.  Single-select for v1; multi-select batch-download is a follow-up if shifters ask for it.

# Framer constants — already in TOML, audit 2026-05-29
[DROPPED · C++]    parallel_streaming_framer frame-size constants → config.  Audit found the promotion already shipped: conf/defaults/framer_conf.toml carries all four knobs (frame_size, first_frames_trigger, afterpulse_deadtime, trigger_secondary_window); config_reader.cxx:257–264 reads them into FramerConfigStruct; the canonical writer constructor at parallel_streaming_framer.cxx:30 consumes the struct.  Header #defines persist only as compile-time defaults for the legacy single-arg constructor (line 17–27) — defensible design hedge.

# Inline correctness fix 2026-05-29
[DROPPED · C++]    BTANA_FRAME_LENGTH_NS hardcoded "3.125" multiplier — replaced with BTANA_ALCOR_CC_TO_NS (alcor_data.h) so the ALCOR clock-to-ns constant has one source of truth.  Same fix applied to FramerConfigStruct::frame_length_ns() in include/utility/config_reader.h:201.  Added alcor_data.h include to config_reader.h so the macro is reachable.  Build green; behaviour identical (same numeric value).

# Init-order guard 2026-05-29
[DROPPED · C++]    recotrackdata_writer:92 init-order safety — explicit `if (!recodata)` guard added before the `*recodata` deref at `AlcorRecotrackdata` construction; stale "TODO safer implementation" comment removed and replaced with a forward-looking comment explaining why a future refactor that moved the bind without re-ordering the construct would now fail loud.  Build green.

# AnalysisResults TTree → TOML atomic switch 2026-05-29
[DROPPED · C++]    AnalysisResults primary backend flip — TTree → TOML, atomic single-PR cutover.  src/analysis_results.cxx rewritten: load() reads via toml::parse_file, write() emits the TOML directly (no ROOT TTree code path remaining).  All 4 writer call sites (lightdata, recodata, recotrack, pulser_calib) + 4 example macros + audit.h + DISCUSSION updated `standard_results.root` → `standard_results.toml`.  Dashboard reader (qa_quicklook.rundb.results_load) was already TOML-only.  Full build green; ROOT-side TFile/TTree includes dropped from analysis_results.h.

# Purgatory exhaustion 2026-05-29 — 3 + 6 dropped, rest graduated to READY
[DROPPED · C++]    Consolidate writer/reader entry points into class hierarchy (lightdata.h:10) — actively rejected by writers/README.md: "no umbrella header — the writers don't share an interface and a writers.h re-export would carry zero value".  Re-add only if a shared interface materialises.
[DROPPED · Dash]   qa: embedded ROOT canvases — current Inspect-spawn pattern already works.  Embedding has measurable thread-loop cost and dashboard-side ROOT-API coupling; the cost/benefit doesn't justify the work absent a specific UX complaint.
[DROPPED · C++]    global_index.h:21,40 — future v2 GlobalIndex layout escape hatch.  Explicit "design hedge" note in the source; not a task, just a comment.  Convert to a real entry only when v2 needs designing.
[DROPPED · C++]    D-02 histogram lifecycle — convention adopted in practice (RootHist wrapper + smart pointers in writers).  Outstanding macro-side sweep folds into D-08 round 2 (already on READY).
[DROPPED · C++]    Merge AlcorFinedata + AlcorData — explicitly deferred per DISCUSSION:38 ("not worth it"; the per-hit value-type vs per-run aggregate distinction earns the duplication).  Re-add if downstream cost surfaces.
[DROPPED · Dash]   qa: Advanced QA tab — placeholder with no scoped content; gated off by [ui].show_advanced_qa.  Reinstate when a concrete advanced-QA panel is on the table.
[DROPPED · C++]    MIST nn_transform — NN-based ring finder as Hough alternative.  Blocked on MIST upstream exposing the transform; not actionable in this repo.  Re-add when MIST ships it.
[DROPPED · C++]    recodata: per-event coverage map — explicit conditional ("if centre-wander discrepancy becomes problem").  No measurement currently shows the problem; re-add on observation.

# Post-CLEAN_OFF sweep 2026-05-30 — 4 rows closed via stash integration (76363b9) + duplicate purge
[DROPPED · Dash]   qa retention: scope sweep to QA-managed runs only (`.qa_managed`) — shipped 76363b9.  Implementation is in `qa_quicklook/retention.py` (sweep filter at `sweep()`), with the marker-drop hook outstanding as a small wiring task that lives in CLEAN_OFF.md's Sunday follow-up #2 (not its own backlog row).
[DROPPED · Dash]   qa retention: persistent-baseline pin (`.qa_persistent` + dashboard button + CLI helper) — shipped partial in 76363b9 (sweep filter logic in retention.py recognises both sentinels).  The dashboard button and CLI helper land alongside follow-up #2 above.  Re-add only if the marker mechanism is reconsidered.
[DROPPED · Dash]   qa: cross-run trend plots in the QA tab (matplotlib + AnalysisResults reader) — shipped 76363b9.  `qa_quicklook/cross_run_trends.py` + `tests/test_cross_run_trends.py` are in HEAD; the AnalysisResults TOML reader the row depended on landed in the same integration.
[DROPPED · C++]    AnalysisResults per-campaign file isolation (per-year `*.results.root` convention) — rendered moot by 76363b9 (TTree → TOML migration).  Per-campaign sharding was a ROOT TTree-merge workaround; TOML's text-mergeable format eliminates the problem.  Single `standard_results.toml` + Python-side rundb indexes suffice.
[DROPPED · C++]    ring_model.h::clip_phi — duplicate of the 2026-05-29 detector-evolution drop above.  The function was already deleted; the READY row was a leftover.  Removed during the sweep.
```
