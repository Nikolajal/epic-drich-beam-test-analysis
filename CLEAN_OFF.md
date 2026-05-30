# CLEAN_OFF — Roadmap (generated 2026-05-30)
_This file is ephemeral.  Delete when the last cluster ships._

## Progress log

- **C0 — DONE** (commits 2fa3958 + 2c98263 + 6172a62).  All five design
  decisions recorded in the DISCUSSION satellites + writers/README.md:
  C0.1 §D-08 (runtime read_only_ guard + copy/move =delete on base
  class), C0.2 utility/get_phase (lever b, vector by GlobalIndex),
  C0.3 streaming §2.5 (dedup prototype-first gate), C0.4 qa_quicklook
  (show-history popup read-only + per-field audit source tag),
  C0.5 §D-13 (CI = conda-pinned multi-OS).
- **C1 — DONE** (commit 2fa3958).  4 writers build clean, 276 Python
  tests pass *with the dirty tree in place*.  C1.1 `_qa_data_repo()`
  honours `[rsync].local_data_dir`; C1.2 `_qa_python()` prefers
  `sys.executable`; C1.3 install RPATH ships both `$ORIGIN` +
  `@loader_path`; C1.4 conf-symlink README note (guard deferred to C2).
  ⚠ STILL PENDING: runtime GUI verify of C1.1/C1.2 (launch dashboard,
  set a non-default local_data_dir, click QA pipeline) — do first
  thing when the dashboard is next up.
- **HEAD-stabilise — DONE** (commit 137ef2b, 2026-05-30).  Discovered
  during pre-C2 triage that HEAD (84b863f and earlier) committed `.cxx`
  files that referenced declarations living only in the uncommitted
  dirty tree.  Without those pieces HEAD does NOT build — the C1
  "build clean" claim above only held with the dirty tree present.
  Restored the minimum load-bearing set (alcor_finedata low_stats
  cache, alcor_spilldata sorted_frame_ids, parallel_streaming_framer
  frame_map type sync, writers/anchor_dt_canvas.h) so HEAD now builds
  standalone.  After: build clean, 110 Python tests pass (HEAD has
  fewer tests than the 276 in the stash — the rest live in CD-cluster
  test files that ride with retention/sheets/qa_pipeline).
- **C2 — 6/7 DONE** (commit e0f697a, 2026-05-30).  Atomic
  AnalysisResults::update (flock + tmp+rename), audit::log rewrite
  (O_APPEND single-write + TOML escape), recodata_conf_reader cutoff
  routing, ct_scan_dt_min header sync to -5, pdu_rotation malformed
  handling, get_cached_position contract rename + assert.  Build clean,
  110 tests pass.  ⊘ C2.7 DEFERRED: the doc's cited line
  (config_reader.h:345) is comment text, not the TNamed declaration;
  the actual skipped-channel TNamed lives in pulser_calib_writer.cxx
  (C5 territory) and the "legacy text-based sections" sweep is broader
  than C2's scope.  Pick up when C5 opens — see C5 for tracking.
- **C3 — 4/5 DONE** (commit 87e8af2, 2026-05-30).  Hough min_hits floor
  (`std::max(1, …)`), score carry_over_hits cleared after per-spill
  weight rebuild, Hough post-find_rings sanity filter (NaN guard +
  radius-range guard) and near-duplicate centre dedup, legacy
  `fit_circle_init_{x,y,r}` knobs removed from struct + 3 TOMLs with
  one-shot deprecation warning in the reader.  Build clean, 110 tests
  pass.  ⊘ C3.2 DEFERRED: hough_threshold_fraction → min_hits
  re-derivation.  Tuning refinement, not a correctness bug; needs a
  calibration sweep on canonical data to pick the right new constant.
  Sunday or post-Monday item.  Also: the quality-ratio floor part of
  C3.4 was deferred for the same reason (D-04 follow-up).
- **C4 — 3/6 partial** (commit 5db4492, 2026-05-30).  C4.1 no-op
  (BACKLOG row already scrubbed in earlier commits — nothing to
  delete); C4.4 schema-mismatch + zero-entry promoted from warning to
  std::runtime_error in `read_calib_from_file`; C4.5 try/catch around
  the two per-channel `TF1::Fit` call sites with n_fit_exceptions
  counter logged at end of `generate_calibration`.  Build clean, 110
  tests pass.  ⊘ C4.2/C4.3/C4.6 DEFERRED:
    - C4.2 (lever-b: collapse two unordered_maps into vector<CalibEntry>
      by GlobalIndex::raw()) — perf win (~3–4 s/spill on the QA
      cascade), but current impl is correct.  Sunday work.
    - C4.3 (assert(!is_frozen()) in setters + audit
      lightdata_writer.cxx freeze sequence) — debug-only safety net;
      pair with C4.6 framer audit.
    - C4.6 (ParallelStreamingFramer audit + WorkerQA leak fix + clone
      hoist) — medium-effort, broad surface; not same-day-before-live
      material.  Sunday work.
- **C5+C6 trivials — 5 fixes shipped** (commit 2d35230, 2026-05-30).
  C5.6 (anchor-Δt vs spill xhi clamp when spills_seen==0), C6.1
  (postproc progress local counter, not frame_id%100000), C6.2 (stack-
  allocate legline TLine — fixed a per-saved-frame leak), C6.3 (hoist
  StreamingHoughQA construction above the per-frame loop — 24 fewer
  assignments per saved frame), C6.4 (warn once when
  h_trigger_anchor_dt X-bin cap kicks in at max_spill ≥ 256), and
  C6.7 no-op (was conditional on C2.7 which is deferred).  Build
  clean, 110 tests pass.
  Pending C5: ⊘ C5.1/C5.2/C5.3 (mediums — γ-mode aggregation, IRLS
  filter, regime-2 slip detection) all need careful pulser-calib
  domain work — Sunday or post-Monday.  ⊘ C5.4/C5.5 (smalls — ±0.5 cc
  satellite disambiguation in published b; per-TDC residual σ_a
  propagation) — both touch the pulser publish side; pick up next.
  Pending C6: ⊘ C6.5/C6.6 (mediums — dynamic timing cuts; 5 grouped
  @todos at lightdata_writer.cxx:196).
- **C6 addendum — DONE** (commit 99fdf08, 2026-05-30).  New
  function-scope flag `timing_data_seen` gates the Timing/ ROOT
  directory + the timing_alignment PDF canvas in lightdata_writer.
  Empty-card "broken run" appearance on the dashboard avoided when a
  run has no decoded timing data (timing detector absent / RDO off /
  no both-chips-firing frame).  Inline note on ALCOR `tracking` being
  2024-only legacy + why no extra tracking guard is needed (the
  per-trigger QA cards are already lazily created and self-skip).
  Build clean, 110 tests pass.
- **Stash@{0} INTEGRATED — DONE** (commit 76363b9, 2026-05-30).  Popped
  the pre-C2 stash and integrated the long-running dashboard / TOML
  / retention / sheets WIP into HEAD.  60 files: 41 modified, 17 new
  (qa_pipeline / retention / remote_watcher / cross_run_trends / 5
  sheets_* modules / lint.yml / import_runbook.py / 2023+2024+2026
  run-lists), 2 deleted (the obsolete backfill_audit_legacy bootstrap).
  4 conflicts resolved: BACKLOG.md merged both sides (kept today's
  retention rows + stash's new rows); analysis_results.{h,cxx} took
  stash side (TOML migration supersedes C2.1 ROOT-mirrored
  atomic-rename); pulser_calib_writer.cxx took stash 365-line
  refactor + re-applied C5.6 anchor xhi clamp on top (refactor had
  restored the bug pattern).  Build clean, **275 tests pass** (up from
  110 — stash brought in test_retention, test_qa_pipeline,
  test_remote_watcher, test_cross_run_trends, test_qa_quicklook_sheets_sync,
  test_generate_calibration_cache).  Stash@{0} dropped after
  integration.

### Sunday follow-ups (gathered from integration)

  1. Re-apply C2.1 atomic-update concept to the now-TOML
     AnalysisResults backend.  Flock on `<fPath>.lock` + write to
     `<fPath>.tmp.<pid>` + `std::filesystem::rename`.  The
     atomicity rationale (load → merge → write window is racy
     across concurrent writers) is unchanged; only the file format
     shifted from ROOT to TOML.
  2. Complete the retention story.  The sweep filter + the 3-state
     model are in retention.py post-integration, and the audit's
     TOCTOU mtime grace landed today (commit 7828cbb).  Still
     pending: (a) drop `.qa_managed` marker at download time via
     `runmanager._on_download`, (b) dashboard pin/unpin button on
     the run row, (c) CLI helper
     `python -m qa_quicklook.retention pin/unpin <RUN_ID>`.
  3. C2.7 / C3.2 / C4.2 / C4.3 / C4.6 / C5.{1,2,3,4,5} /
     C6.{5,6} / C7 / C8 / C9-C13 — still pending.  CD cluster is
     now mostly *implemented* (lives in HEAD post-integration) but
     wasn't formally walked through the CLEAN_OFF sub-step list;
     audit on Sunday and either close the cluster as DONE-via-
     integration or surface any remaining sub-steps.

- **Post-integration SWEEP — DONE** (commits a8bb62e + 8f9b3d3 +
  320c656 + 7828cbb, 2026-05-30).  4 parallel Explore agents
  cross-checked docs / BACKLOG / new code / dead+stale comments
  against the 14 CLEAN_OFF commits + the integration.  Actioned:
  - BACKLOG: 4 rows moved to DROPPED (shipped via 76363b9), 1
    deleted as duplicate.  Net READY 55 → 50.
  - Docs: DISCUSSION.md hub table updated; streaming/DISCUSSION
    knob table reflects fit_circle_init removal; qa_quicklook/
    DISCUSSION TOML-migration section banner-annotated as shipped.
  - Dead code: dashboard-side fit_circle_init_{x,y,r} entries
    removed from `qa_quicklook/settings.py` (no live consumer).
  - Inconsistency: `min_hits_slack` 0.50 in conf/defaults/streaming.toml
    aligned to 0.75 (matches C++ default; orphan "legacy" value).
  - CRITICAL #1: audit::log PIPE_BUF cap enforced (3.5 KiB) +
    close(fd) guarded.
  - CRITICAL #2: lightdata_writer wraps read_calib_from_file in
    try/catch with fallback (C4.4 throws were uncaught).
  - CRITICAL #3: retention.apply TOCTOU mtime-grace guard (60 s
    default, configurable via kwarg) + new `skipped` field in
    report.  Tests updated to pass `mtime_grace_s=0.0`.
  - Build clean; 275 passed, 1 skipped, 12 subtests.

### Remaining sweep-audit items for Sunday/post-Monday

  - `qa_pipeline.py:120-133` — JSON parse errors silently dropped;
    surface a warning when >50% of expected stdout events are
    missing (operator sees the writer crash via stderr fallback).
  - `sheets_sync.py:171-178` — `TOMLDecodeError` silently falls
    back to defaults; log + surface in `disabled_reason()`.
  - `lightdata_writer.cxx:923-956` — verify `RootHist<T>` has
    `= delete` on move/copy ctors (else hoisted `hough_qa` raw
    pointers could dangle if the wrapper ever gets moved).
  - **qa_quicklook/DISCUSSION.md rewrite** — annotated with a
    🟢 STATUS UPDATE banner today; a clean rewrite of the
    "Migrating AnalysisResults to TOML" section (frame as
    "shipped feature summary + current limitations + next steps"
    instead of the historical "proposal" framing) is still
    deferred.

### Retention-policy ideas captured (user, 2026-05-30)

Both surfaced during C2 work; landed as adjacent READY rows in
BACKLOG.md by a background integration agent (commit 5f2de6c) so the
main thread could continue on C2:

- P 1.61 — Scope retention sweep to QA-managed runs only via
  `.qa_managed` sentinel.  Today's sweep can delete manually-rsync'd
  runs from under the user.
- P 1.48 — Persistent-baseline pin via `.qa_persistent` (dashboard
  button + CLI helper).  Three-state model (transient / persistent /
  user-managed) captured in the entry.

Both entries point at `qa_quicklook/retention.py` as implementation
home; that file is currently stash-only and lands when CD cluster
opens.  Expect a clean textual merge conflict around the new BACKLOG
rows when the stash pops; resolution is "keep both sides".

### Deferred-decision status (from C0)

- **Sensor-name vocabulary** — ✅ RESOLVED 2026-05-30 (user domain
  explanation, captured in memory `alcor-vs-altai-sensor-distinction.md`).
  `cherenkov` / `timing` / `tracking` are ONE ALCOR pipeline (same chip,
  calibration, data-structure flow) split only by semantics — do NOT
  abstract them apart.  `ALTAI` is a separate external tracker
  (CMOS/LGAD, different DAQ) merged later + cross-referenced in the run
  database.  ALCOR `tracking` is **2024-only legacy → deprecate**.
  Unblocks C12; adds a new step to C6 (see "C6 addendum" below).
- **Δt-cut policy** (gates C3/C6) — STILL deferred; user wants the
  options expanded before deciding.

### C6 addendum — NEW step (user 2026-05-30)

Add to cluster C6 (lightdata_writer sweep): **skip the `timing` and/or
`tracking` per-trigger QA blocks when those detectors have no decoded
data for the run** (same shape as the existing 0-streams guard).  ALCOR
`tracking` is 2024-only legacy and `timing` may be absent; today the
writer emits empty cards that read as "broken" in the dashboard.  When
absent: skip the block + log one INFO line, and tag ALCOR `tracking`
deprecated in the code comment.  Verify: a recent run with no tracking
emits no tracking card; a 2024 run still does.

---


## Reading this file

Top to bottom is the order to ship. Clusters are grouped by **code adjacency** first
(same file / same sub-system / same hot path) and then ordered by max-priority +
ship-blocker count. A few clusters are pulled forward out of priority order because
they unblock downstream work — those are called out in the cluster rationale.

Cross-cluster dependencies worth keeping in mind:
- **C1 (ship-blockers)** unblocks every operator session — must land first regardless of priority.
- **C2 (utility / shared infra)** is touched by every writer; ship before C3/C4/C5 so we don't re-edit headers.
- **C5 (pulser_calib_writer)** is the heaviest single-file cluster — five backlog rows touch the same 2003-LOC file. Doing them serially in one pass saves ~3 file-reread cycles.
- **C7 (streaming_v2 features)** depends on **C3 (streaming-trigger correctness)** landing first — the score/Hough rewrites assume the iterator-UB / mask-writeback / carryover fixes are in.
- **C9 (recodata QA splits)** depends on **C12 (per-run beam-metadata schema)** for the ring→radiator labelling (P 1.00). Ship the schema first so we don't need a second recodata sweep.
- **C11 (D-08 round 2 + 'event' class)** is xlarge and should be the last big code lift; needs the design decisions in C0 closed first.
- **CD (Dashboard)** is one fat cluster on purpose — qa_quicklook iterates without rebuild, so we batch all dashboard work to amortise context-load.

Tradeoff calls baked into the ordering:
- **Adjacency over raw P** in 4 places: (a) all `pulser_calib_writer.cxx` rows fused into C5 even though three are P<1.5; (b) all `hough.cxx` rows fused into C3+C7 instead of by priority; (c) all README/DISCUSSION doc fixes deferred to C13 instead of interleaving with code; (d) the `audit.h` quote-escape (P 0.8) rides with the buffered-write fix (P 1.5) in C2 because both touch lines 113–130.
- **Tests live with the code they cover** — e.g. test_qa_pipeline.py `--threads 4` assertion ships in CD with the dashboard pipeline work, not in a standalone test cluster.
- **C0 (design decisions)** is first because every other big cluster cites one of its decisions as a blocker; spending half a day closing them now saves a re-litigation mid-PR.

## Cluster C0 — Design decisions to close before code lands  [P1 next, half-day]
_Locality: DISCUSSION.md + per-subsystem DISCUSSION satellites_

These are all "park-decided" items in the discussion lane that gate later clusters.
Cheap to resolve (each is a 30-min DISCUSSION pass + a note in the relevant header),
expensive to leave open — every one of them blocks a queued BACKLOG row mid-PR.
Do these BEFORE opening any of the heavy code clusters.

### Step C0.1 — Lock D-08 reader-vs-writer split (runtime flag vs ReadOnly subclass)  [trivial]
- **Files**: include/alcor_recodata.h, include/alcor_recotrackdata.h, DISCUSSION.md
- **Action**: Resolve the parked D-08 question. Write the decision into DISCUSSION.md § D-08 and codify the "non-copyable + non-movable" rule (discussion.17) in include/writers/README.md as the invariant for future ROOT-bound wrappers.
- **Verify**: A "Decision:" entry exists in DISCUSSION.md § D-08 naming the chosen approach. include/writers/README.md states the non-copy/non-move invariant as a numbered rule.
- **Closes**: discussion.4, discussion.17

### Step C0.2 — Lock get_phase lever choice (already decided: lever b, vector<entry> by GlobalIndex)  [trivial]
- **Files**: include/alcor_finedata.h, include/utility/DISCUSSION.md
- **Action**: Inventory decision is already taken (lever b). Write the decision + rejected-alternatives note into include/utility/DISCUSSION.md so C4 can be opened cold.
- **Verify**: include/utility/DISCUSSION.md contains a "get_phase lever decision" paragraph naming lever (b) and noting why (a) and (c) were rejected.
- **Closes**: backlog.10 / memory_user_asks.0 (decision half)

### Step C0.3 — Pick streaming dedup strategy (cheap measurement first vs full P2.09)  [trivial]
- **Files**: include/triggers/streaming/DISCUSSION.md
- **Action**: Decide whether to ship discussion.3's tag_first_per_channel measurement prototype as a gate before the BACKLOG P2.09 implementation. Write the answer into the streaming DISCUSSION.
- **Verify**: streaming/DISCUSSION.md § 2.5 names the sequencing decision; the chosen path is the one C7 opens with.
- **Closes**: discussion.3, backlog.2 (sequencing half)

### Step C0.4 — Pick "Show history" UX shape + rundb.results_update granularity  [trivial]
- **Files**: qa_quicklook/DISCUSSION.md
- **Action**: Resolve the two parked UX questions (popup-vs-modal + revert-widget for show-history; per-field vs batch+diff for results_update; audit-log differentiation between operator-override and writer-regeneration).
- **Verify**: qa_quicklook/DISCUSSION.md has decisions written down for both questions; CD-cluster steps for backlog.6 and backlog.25 cite them.
- **Closes**: discussion.5, discussion.6, discussion.7

### Step C0.5 — Pick CI strategy (Linux-only / conda-pinned / self-hosted)  [trivial]
- **Files**: DISCUSSION.md
- **Action**: This is the one decision that gates C13 (CI re-enable). Write the chosen strategy into DISCUSSION.md so C13 can be opened cold.
- **Verify**: DISCUSSION.md names the chosen CI strategy and lists the open follow-ups (macros ACLiC-clean state, conf/ symlinks decision).
- **Closes**: backlog.1 (decision half)

---

## Cluster C1 — Ship-blocker correctness + install RPATH  [P0 must-ship, half-day]
_Locality: qa_quicklook/app.py + CMakeLists.txt + conf/ symlinks_

These are the highest-P bugs in the inventory and they block a fresh shifter from
getting any output at all. Three trivial fixes; ship together because all three
manifest on first-launch on a clean machine.

### Step C1.1 — Auto-QA pipeline honour [rsync].local_data_dir  [trivial]
- **Files**: qa_quicklook/app.py
- **Action**: At app.py:846 replace the hardcoded `<repo>/Data` with the resolved path from `download.load_config(self._dashboard_config).local_data_dir`. Reuse the existing `_data_dir_for_retention` helper.
- **Verify**: Configure `[rsync].local_data_dir = /tmp/qa_test_data`, drop a fake run there, click "Inspect" — qa_pipeline launches with `--data-repo /tmp/qa_test_data` and does NOT return EXIT_RUN_MISSING.
- **Closes**: memory_user_asks.3

### Step C1.2 — Drop hardcoded .venv/bin/python interpreter  [trivial]
- **Files**: qa_quicklook/app.py
- **Action**: At app.py:842 prefer `sys.executable`; fall back to `shutil.which("python3")`; only use the .venv path if it exists.
- **Verify**: Rename .venv/, click auto-QA, observe the subprocess spawns with `sys.executable` and runs to EXIT_OK.
- **Closes**: memory_user_asks.4

### Step C1.3 — Linux install RPATH for writers (libmist.so)  [trivial]
- **Files**: CMakeLists.txt
- **Action**: Add `INSTALL_RPATH` block at CMakeLists.txt:220-224 mirroring the macOS `@loader_path/` idiom, so installed writers resolve libmist.so without LD_LIBRARY_PATH gymnastics.
- **Verify**: Fresh Ubuntu docker, `cmake --install`, run `lightdata_writer --help` → exit 0 without `LD_LIBRARY_PATH` set.
- **Closes**: memory_user_asks.29

### Step C1.4 — conf/ symlinks decision (commit real files OR document core.symlinks=true)  [trivial]
- **Files**: conf/streaming.toml, conf/framer_conf.toml, conf/mapping_conf.toml, README.md
- **Action**: Take the decision from C0.5 (CI strategy probably implies Linux). Either replace the 7 symlinks with real files and add a pre-commit hook, or add a documented post-checkout step + startup sanity check in `load_streaming_conf` that errors out if the file content equals a single relative path.
- **Verify**: Fresh checkout on a `core.symlinks=false` config produces either real config files OR a startup error pointing at the documented fix.
- **Closes**: memory_user_asks.31

---

## Cluster C2 — Shared utility headers (audit, AnalysisResults, mapping, config_reader)  [P0 must-ship, one-day]
_Locality: include/utility/*.h + src/analysis_results.cxx + src/config_reader.cxx + src/mapping.cxx_

Every writer + the dashboard read from these. Ship them BEFORE touching any
writer so we don't re-edit downstream headers when the utility APIs shift.
Two ship-blockers here (AnalysisResults race + audit-log file format).

### Step C2.1 — AnalysisResults::update — atomic tmp+rename + flock  [small]
- **Files**: src/analysis_results.cxx, include/analysis_results.h
- **Action**: At analysis_results.cxx:181 wrap the load→merge→write window in a flock/O_EXCL pidfile guard; write to `<path>.tmp.<pid>` then `std::filesystem::rename`. Three writer call sites (lightdata:1818, recodata:1589, recotrack:219) require no change.
- **Verify**: New unit test runs two writer subprocesses both calling `update()` with disjoint entries; resulting standard_results.toml contains BOTH entries; audit log shows two write events without overlap.
- **Closes**: memory_user_asks.6

### Step C2.2 — util::audit::log — single ::write() on O_APPEND fd + escape quotes/backslashes  [trivial]
- **Files**: include/utility/audit.h
- **Action**: At audit.h:113-130 replace the chained `<<` ofstream with a `std::string` build + single `::write(fd, buf, n)` on an `O_APPEND` fd opened once. Add a minimal basic-string escaper for `\\ " \n \t \r` (ride with the same line edit).
- **Verify**: Stress test (16 threads × 1000 writes each) produces a file that `toml::parse_file` accepts; values containing `"` and `\` survive the round trip; PIPE_BUF atomicity holds (no interleaved `[[entry]]` headers).
- **Closes**: memory_user_asks.7, memory_user_asks.8

### Step C2.3 — recodata_conf_reader: route through toml_parse_with_cutoff  [trivial]
- **Files**: src/config_reader.cxx
- **Action**: At config_reader.cxx:913 (recodata_conf_reader) replace `toml::parse_file` with `toml_parse_with_cutoff` to match every other reader.
- **Verify**: Add `## example block` below the live config in conf/recodata.toml; recodata writer parses without surfacing the example as config.
- **Closes**: memory_user_asks.9

### Step C2.4 — QaConfigStruct.ct_scan_dt_min header default sync (-10 → -5)  [trivial]
- **Files**: include/utility/config_reader.h, conf/defaults/framer_conf.toml, conf/QA/framer_conf.toml
- **Action**: Pick a single source of truth (recommended: -5, matching both toml files). Update header default to -5.
- **Verify**: Construct `QaConfigStruct{}` in a test, assert ct_scan_dt_min == -5; production toml load still parses to -5.
- **Closes**: memory_user_asks.10

### Step C2.5 — Mapping::pdu_rotation unchecked map access + parser fallback  [trivial]
- **Files**: src/mapping.cxx, include/mapping.h
- **Action**: At mapping.cxx:74 (and adjacent sites) guard with `.find()` + logged fallback. At mapping.cxx:195 replace `val.value<bool>().value_or(true)` with explicit `.value()` + warning on type mismatch; document the semantic ("missing → false; malformed → warn+false").
- **Verify**: Synthetic mapping config with malformed pdu_rotation entry → writer logs a warning and treats that PDU as not-rotated (does NOT segfault).
- **Closes**: discussion.0, memory_user_asks.12

### Step C2.6 — Mapping::get_cached_position — align docstring with tdc=0-only cache  [trivial]
- **Files**: include/mapping.h, src/mapping.cxx
- **Action**: At mapping.h:300 either rename the parameter to `channel_ordinal_times_four` + assert `key % 4 == 0`, OR extend the cache build at mapping.cxx:281-295 to enumerate all 4 tdc values. Recommended: assert + rename (zero-call-site refactor, future-trap eliminated).
- **Verify**: Test passes `gi.raw()` with tdc != 0 and expects either the documented behaviour or the assertion fire.
- **Closes**: memory_user_asks.11

### Step C2.7 — config_reader.h skipped-channel list as TNamed → structured + TOML rewrite of legacy text  [small]
- **Files**: include/utility/config_reader.h
- **Action**: Promote the TNamed skipped-channel list at config_reader.h:345 to a structured representation (vector<GlobalIndex> or similar). Sweep remaining legacy text-based sections (backlog.49) in the same pass while the file is open.
- **Verify**: Round-trip test reads, mutates, and re-emits a config without losing the skipped-channel set.
- **Closes**: backlog.46, backlog.49

---

## Cluster C3 — Streaming trigger correctness (hough + score hot path)  [P1 next, one-day]
_Locality: src/triggers/streaming/{hough,score}.cxx + include/triggers/streaming/_

Five correctness items on the streaming hot path. All five touch hough.cxx /
score.cxx — group aggressively because each C++ rebuild is ~minutes. Must land
before C7 (streaming v2 features) which assumes correctness fixes are in.

### Step C3.1 — Hough min_hits truncated-to-0 floor  [trivial]
- **Files**: src/triggers/streaming/hough.cxx
- **Action**: At hough.cxx:109 wrap `static_cast<int>(min_active * cfg.min_hits_slack)` with `std::max(1, ...)` to prevent min_active==1+slack<1 → min_hits=0.
- **Verify**: Synthetic low-occupancy frame with min_active=1, slack=0.5 → Hough min_hits == 1, not 0.
- **Closes**: memory_user_asks.13

### Step C3.2 — Hough threshold formula re-derivation (post-Stage-1 gating)  [small]
- **Files**: include/triggers/streaming/hough.h, src/triggers/streaming/hough.cxx
- **Action**: Re-derive `hough_threshold_fraction → min_hits` for the post-Stage-1 gating semantics; update the comment at hough.h:103 with the derivation; bump the default if the analysis changes.
- **Verify**: A canonical run produces unchanged accepted-rings count if the re-derivation collapses to the old formula, or the documented new value if it shifts.
- **Closes**: backlog.5

### Step C3.3 — Score: clear carry_over_hits when bundle is rebuilt  [trivial]
- **Files**: src/triggers/streaming/score.cxx, src/lightdata_writer.cxx
- **Action**: At lightdata_writer.cxx:961-980 (the build_streaming_trigger_weights site) clear `carry_over_hits` immediately after the rebuild so the first windows of the next spill don't mix old-bundle running_score with new-bundle expected/sigma.
- **Verify**: First-of-spill window n_σ matches the steady-state distribution (no >5σ outliers tied to spill boundaries) on a canonical run.
- **Closes**: memory_user_asks.14

### Step C3.4 — Hough: per-ring (X,Y,R) sanity cuts + near-duplicate merge before max_rings clamp  [small]
- **Files**: src/triggers/streaming/hough.cxx
- **Action**: Add the three independent sanity guards (quality-ratio floor, geometry box, NaN) and the near-duplicate dedup pass that merges cell-boundary splits BEFORE the max_rings=2 clamp.
- **Verify**: QA hist of accepted-rings count is non-increasing vs baseline; no rings with NaN center / R<R_min appear; cell-boundary "split ring" anti-pattern absent from a canonical run.
- **Closes**: backlog.21, backlog.22

### Step C3.5 — Drop legacy fit_circle_init_{x,y,r} TOML knobs + sync stale conf/QA  [trivial]
- **Files**: conf/streaming.toml, conf/QA/streaming.toml, conf/defaults/streaming.toml, include/utility/config_reader.h, include/triggers/streaming/hough.h
- **Action**: Knobs are dead per hough.cxx:188-190. Decision (default): delete from all three TOMLs + drop the StreamingHoughConfigStruct fields + bump the schema version note. Alternative: sync conf/QA to working/. Pick deletion.
- **Verify**: Config-dump shows no fit_circle_init keys; loading a run without those keys succeeds; loading a run that still has them logs a "deprecated/ignored" warning.
- **Closes**: discussion.1, memory_user_asks.15

---

## Cluster C4 — AlcorFinedata hot path (get_phase lever b + freeze contract + calib hardening)  [P1 next, one-day]
_Locality: include/alcor_finedata.h + src/alcor_finedata.cxx + src/parallel_streaming_framer.cxx_

The C0.2 decision is "lever b: vector<entry> indexed by GlobalIndex". This cluster
also closes BACKLOG.9 (generate_calibration retry — already shipped, just bookkeeping)
and hardens the four surrounding liabilities on the same file.

### Step C4.1 — Mark BACKLOG.9 closed (generate_calibration retry already shipped in ea4390c)  [trivial]
- **Files**: BACKLOG.md
- **Action**: Strip the closed row (P 0.35 generate_calibration retry seed jitter + cap 5→3) from BACKLOG.md.
- **Verify**: `grep "seed jitter" BACKLOG.md` returns no hits.
- **Closes**: backlog.9

### Step C4.2 — get_phase: collapse two unordered_maps into vector<entry> by GlobalIndex  [small]
- **Files**: include/alcor_finedata.h, src/alcor_finedata.cxx
- **Action**: Lever (b) per C0.2. Replace `calibration_parameters` (uint32_t → array<float,3>) + `channel_calibration_method` (uint32_t → CalibrationMethod) with a single `std::vector<CalibEntry>` indexed by `GlobalIndex::raw()`. CalibEntry packs the array + method. get_phase() at alcor_finedata.cxx:85 becomes one dense-vector lookup. Keep frozen-table fast path semantics; preserve the freeze() / is_calibration_frozen() contract.
- **Verify**: Re-run the canonical 5-spill QA — per-spill wall time shows ~14-15 s in the get_phase block vs 18-19 s pre-change. Calibration outputs (fine_calib.toml content) byte-identical to baseline.
- **Closes**: backlog.10, memory_user_asks.0

### Step C4.3 — Calibration-freeze contract — assert(!is_frozen()) in setters  [small]
- **Files**: include/alcor_finedata.h, src/alcor_finedata.cxx, src/lightdata_writer.cxx
- **Action**: Add `assert(!is_calibration_frozen())` to the setters per the header warning at alcor_finedata.h:944-947. Audit lightdata_writer.cxx:306 (freeze) vs :694, :841, :843 (set_param2) — either (a) delay freeze until after per-spill updates, or (b) drop the freeze around the per-spill block and refreeze after. Pick (b) and document.
- **Verify**: Debug build runs to completion on a canonical run; any future refactor that overlaps workers with the per-spill updates trips the assert in debug.
- **Closes**: memory_user_asks.18

### Step C4.4 — Promote read_calib_from_file schema mismatch from warning to error  [trivial]
- **Files**: src/alcor_finedata.cxx
- **Action**: At alcor_finedata.cxx:242-249 throw `std::runtime_error` on schema mismatch (or zero-entry guard) instead of warning + continue. Backwards-compat: we are the only emitter of v3 so this is safe.
- **Verify**: Synthetic v4 file → writer fails fast with a clear error; v3 file → succeeds unchanged.
- **Closes**: memory_user_asks.19

### Step C4.5 — generate_calibration: try/catch around per-channel TF1::Fit + fit_exceptions counter  [trivial]
- **Files**: src/alcor_finedata.cxx
- **Action**: Wrap the per-channel `projection->Fit` at alcor_finedata.cxx:458/471 in try/catch; increment a `fit_exceptions` counter; continue. Log the count at the success-log site (line 486-495).
- **Verify**: Synthetic OOM-injection test (or just normal run) shows the counter at 0 in audit log; a forced bad_alloc on one channel does not abort the run.
- **Closes**: memory_user_asks.20

### Step C4.6 — ParallelStreamingFramer audit + WorkerQA leak fix + clone hoist  [medium]
- **Files**: include/parallel_streaming_framer.h, src/parallel_streaming_framer.cxx
- **Action**: First audit (writes a note in include/utility/DISCUSSION.md) of the current framer thread-fanout per BACKLOG row P 1.25. Then: (a) wrap WorkerQA TH*F clones in unique_ptr/RootHist to fix the exception-path leak at parallel_streaming_framer.cxx:608-622/719-733; (b) hoist clone allocation out of the per-spill loop (allocate once sized to kMaxWorkers, Reset() per spill).
- **Verify**: Inject a throw in the merge loop → no TH*F leak (asan). Canonical 1000-spill run shows reduced ROOT TNamed-table churn (rough timing: less alloc time per spill).
- **Closes**: backlog.8 (audit half), memory_user_asks.16, memory_user_asks.17

---

## Cluster C5 — pulser_calib_writer.cxx (5 backlog rows + plot bug, same 2003-LOC file)  [P1 next, two-day]
_Locality: src/writers/pulser_calib_writer.cxx + include/writers/pulser_calib.h_

Five BACKLOG rows (P 1.60, 1.58, 1.48, 1.37, 1.25) plus the anchor_dt_vs_spill xhi
plot bug all touch the same writer. Doing them serially in one focused pass means
ONE deep file read instead of five. Order: header-touching items first, then in-file
algorithm rewrites, then the small plot fix.

### Step C5.1 — γ-mode (Stage 2) per-TDC aggregation across multiple TDCs  [medium]
- **Files**: src/writers/pulser_calib_writer.cxx, include/writers/pulser_calib.h, include/utility/global_index.h
- **Action**: Add the per-TDC aggregation pass that keys off GlobalIndex layout; emit per-TDC summary tables.
- **Verify**: Canonical pulser-calib run produces per-TDC γ tables in the QA root; values match a hand-rolled aggregation script.
- **Closes**: backlog.12

### Step C5.2 — Replace fine-band filter with IRLS / M-estimator inside the fit  [medium]
- **Files**: src/writers/pulser_calib_writer.cxx, include/writers/pulser_calib.h
- **Action**: Swap the fine-band filter for an IRLS / M-estimator pass that's robust to outliers without the hard cutoff.
- **Verify**: Synthetic outlier injection: IRLS fit converges to the truth within tolerance; pre-change filter mode left some outlier influence in the published b.
- **Closes**: backlog.29

### Step C5.3 — Regime-2 slip detection: pair-difference test against coarse-edge quantisation  [medium]
- **Files**: src/writers/pulser_calib_writer.cxx, include/writers/pulser_calib.h
- **Action**: Add the pair-difference test that distinguishes hardware slip from edge quantisation.
- **Verify**: Run a known-slip pulser run; the test flags the slip. Run a known-clean run; the test does not flag.
- **Closes**: backlog.11

### Step C5.4 — ±0.5 cc satellite disambiguation in published b  [small]
- **Files**: src/writers/pulser_calib_writer.cxx, include/writers/pulser_calib.h
- **Action**: Distinguish edge-quantisation satellites from hardware slip in the published b values; emit a per-channel flag.
- **Verify**: QA panel shows satellites correctly labelled on a run that has both quantisation and slip features.
- **Closes**: backlog.15

### Step C5.5 — Propagate per-TDC residual σ into published calibration (sigma_a)  [small]
- **Files**: src/writers/pulser_calib_writer.cxx, include/writers/pulser_calib.h, include/alcor_finedata.h
- **Action**: Replace the sigma_a placeholder with the actual per-TDC residual σ. Plumb through to AlcorFinedata if downstream consumes it.
- **Verify**: Published calibration carries a non-placeholder sigma_a; downstream consumer (if wired) reads it without warning.
- **Closes**: backlog.19

### Step C5.6 — h_anchor_dt_vs_spill xhi clamp when spills_seen==0  [trivial]
- **Files**: src/writers/pulser_calib_writer.cxx
- **Action**: At pulser_calib_writer.cxx:1098 wrap xhi with `std::max(0.5, spills_seen - 0.5)` to avoid the ROOT "TAxis low and high equal" path.
- **Verify**: Run a synthetic case with spills_seen==0 — no ROOT warning; plot renders empty but valid.
- **Closes**: memory_user_asks.22

---

## Cluster C6 — lightdata_writer.cxx (5 grouped todos + 4 small bugs)  [P1 next, two-day]
_Locality: src/lightdata_writer.cxx + src/writers/lightdata/*_

Same adjacency argument as C5: one fat file plus its sub-files. The 5 grouped @todos
at lightdata_writer.cxx:196 are FIFO config / single-multi-core test / afterpulse plot /
QA restructure / external config. Ride the smaller bugs in the same pass.

### Step C6.1 — Per-frame postprocessing progress: local counter, not frame_id%100000  [trivial]
- **Files**: src/lightdata_writer.cxx
- **Action**: At lightdata_writer.cxx:881/884-889 replace the `frame_id % 100000` test with a separate `done` counter mod 1000, matching recodata_writer.cxx:841.
- **Verify**: Progress bar moves on a normal run (no longer stuck near 0% until the per-spill snap).
- **Closes**: memory_user_asks.21

### Step C6.2 — Streaming-score canvas: stack-allocate legline TLine  [trivial]
- **Files**: src/lightdata_writer.cxx
- **Action**: At lightdata_writer.cxx:1693 replace `new TLine()` with stack `TLine legline; leg.AddEntry(&legline, ...)`.
- **Verify**: ASan canonical run shows one fewer leak per saved canvas.
- **Closes**: memory_user_asks.25

### Step C6.3 — Hoist StreamingHoughQA struct construction above the per-frame loop  [trivial]
- **Files**: src/lightdata_writer.cxx
- **Action**: At lightdata_writer.cxx:1012-1044 build the StreamingHoughQA once per spill (not per frame). Pointers don't change across frames.
- **Verify**: Diff of QA hist contents byte-identical to baseline; CPU profile shows the ~25 assigns/frame collapsed.
- **Closes**: memory_user_asks.27

### Step C6.4 — Anchor-Δt vs spill TH2F: warn or relabel when max_spill > 256  [trivial]
- **Files**: src/lightdata_writer.cxx
- **Action**: At lightdata_writer.cxx:1095-1107 emit a runtime warning when `max_spill > 256`, OR relabel axis title to "spill bin (K spills/bin)". Recommended: both (warning at the writer, axis-aware label).
- **Verify**: 300-spill synthetic run → log shows the warning, PDF carries the K-spill-per-bin label.
- **Closes**: memory_user_asks.28

### Step C6.5 — Dynamically determine timing cuts (TODO at lightdata_writer.cxx:854)  [medium]
- **Files**: src/lightdata_writer.cxx, src/writers/lightdata/dcr_afterpulse_ct_qa.cxx, src/writers/lightdata/finalize_streaming_qa.cxx
- **Action**: Replace the hardcoded timing window with the data-driven choice: pick the most-populated non-zero bin of the per-TDC delta-time distribution and centre the acceptance window on it. Design in include/writers/DISCUSSION.md.
- **Verify**: Compare per-TDC offsets pre/post — TDCs whose true peak sat near the static-window edge show a corrected mean.
- **Closes**: backlog.30, inline_todos.0

### Step C6.6 — Tackle the 5 grouped @todos at lightdata_writer.cxx:196  [medium]
- **Files**: src/lightdata_writer.cxx, src/writers/lightdata/*, include/writers/lightdata.h
- **Action**: Work through (a) FIFO config, (b) single/multi-core test, (c) afterpulse plot, (d) QA restructure, (e) external config. Each is small in isolation; the meta-comment groups them because they share the writer's structure. Recommended order: (a) → (e) → (d) → (c) → (b).
- **Verify**: Canonical run produces the documented external-config + restructured QA layout; the afterpulse plot ships.
- **Closes**: backlog.4

### Step C6.7 — Mark BACKLOG row P 0.27 (legacy config text → TOML) closed if covered by C2.7  [trivial]
- **Files**: BACKLOG.md
- **Action**: Bookkeeping. If C2.7 covered the legacy text rewrite, strip the row here.
- **Verify**: BACKLOG.md no longer carries the duplicate row.
- **Closes**: backlog.49 (bookkeeping)

---

## Cluster C7 — Streaming v2 features (DCR estimator + signal-aware weighting + crosstalk + multiplicity)  [P1 next, one-day]
_Locality: src/triggers/streaming/{score,hough}.cxx + macros/examples/cross_talk_treatment.cpp_

All four are streaming v2 features on score.cxx + hough.cxx. Sequenced AFTER C3
(correctness) so we're not rewriting buggy logic. C0.3 decided whether to ship the
cheap tag-first-only measurement prototype as a gate; this step honours that decision.

### Step C7.1 — Tag-first-only dedup measurement prototype (gate for P2.09)  [small]
- **Files**: src/triggers/streaming/hough.cxx, include/triggers/streaming/hough.h, conf/streaming.toml
- **Action**: Per C0.3 decision. Add a `tag_first_per_channel` knob + side-by-side QA hist of N_hits per ring with-vs-without dedup. Run on a canonical noisy run.
- **Verify**: Measured inflation either <5% (close P2.09 as wontfix) or significant (promote to config knob and continue with the writer-side path).
- **Closes**: discussion.3, partial backlog.2

### Step C7.2 — Time-aware hit handling (Δt cut at writer + Hough dedup)  [medium]
- **Files**: src/triggers/streaming/hough.cxx, src/writers/lightdata/dcr_afterpulse_ct_qa.cxx, src/lightdata_writer.cxx, src/recodata_writer.cxx, include/triggers/streaming/hough.h
- **Action**: Implement the chosen option from C7.1's measurement; ship writer-side Δt gating + Hough hit dedup.
- **Verify**: Per-ring N_hits hist matches the C7.1 prediction; no double-count regressions.
- **Closes**: backlog.2

### Step C7.3 — Conservative DCR estimator (75th percentile)  [small]
- **Files**: src/triggers/streaming/score.cxx, include/triggers/streaming/score.h
- **Action**: Swap the current estimator for a 75th-percentile robust estimator.
- **Verify**: Run with outliers injected — estimator does not move; mean estimator did.
- **Closes**: backlog.18

### Step C7.4 — Signal-aware weighting w_i = s_i / λ_i  [medium]
- **Files**: src/triggers/streaming/score.cxx, include/triggers/streaming/score.h, src/triggers/streaming/hough.cxx
- **Action**: Multi-pass bootstrap; document convergence criterion.
- **Verify**: Score distribution on canonical run shifts as predicted; n_σ★ output remains sensible.
- **Closes**: backlog.28

### Step C7.5 — Crosstalk correction in the score stage  [medium]
- **Files**: src/triggers/streaming/score.cxx, include/triggers/streaming/score.h
- **Action**: Wire the crosstalk correction (reference: macros/examples/cross_talk_treatment.cpp).
- **Verify**: Crosstalk-heavy run shows the documented reduction; clean run unchanged.
- **Closes**: backlog.27

### Step C7.6 — Multiplicity upper-bound cut  [small]
- **Files**: src/triggers/streaming/score.cxx, src/triggers/streaming/hough.cxx, include/triggers/streaming/score.h
- **Action**: Add the multiplicity upper-bound cut flagged 2026-Q2.
- **Verify**: Distribution tail beyond the cut is absent post-change; canonical run efficiency unchanged.
- **Closes**: backlog.38

### Step C7.7 — Streaming QA: n_σ vs n_rings correlation hist + radius overlay + Δt-between-stages  [small]
- **Files**: src/triggers/streaming/hough.cxx, src/triggers/streaming/score.cxx, src/writers/lightdata/dcr_afterpulse_ct_qa.cxx, include/triggers/streaming/DISCUSSION.md
- **Action**: Add the three QA histograms; reuse the existing canvas-save pattern.
- **Verify**: Output PDFs render; values match a hand-rolled cross-check on one canonical run.
- **Closes**: backlog.47

### Step C7.8 — Carry-over hit index=-1 sentinel → typed wrapper  [trivial]
- **Files**: src/triggers/streaming/score.cxx, include/triggers/streaming/score.h
- **Action**: Replace the `-1` sentinel with `std::optional<size_t>` or a tagged variant. No live consumer reads it today; cheap insurance before C7.4 adds one.
- **Verify**: Build clean; ASan/UBSan canonical run clean.
- **Closes**: discussion.16

---

## Cluster C8 — Hough investigation + ring-fit hardening (math layer)  [P1 next, half-day]
_Locality: macros/examples/elliptic_investigation.cpp + include/utility/circle_fit.h + src/writers/recodata/ring_compute.cxx_

Two related but file-separate items: the elliptic-deformation measurement (drives
whether C7+C9 need an elliptic branch) and the fit_circle design review. Bundling
them because both are math-layer touches that produce decisions, not big PRs.

### Step C8.1 — Run elliptic_investigation V1 + record the measurement  [small]
- **Files**: macros/examples/elliptic_investigation.cpp, include/utility/ring_model.h, DISCUSSION.md
- **Action**: Build the macro ACLiC-clean, run on a canonical multi-ring run, record the measurement in DISCUSSION.md with the verdict (Hough needs elliptic branch yes/no).
- **Verify**: DISCUSSION.md carries the numeric result + the decision; if yes, a follow-up row gets filed in BACKLOG.
- **Closes**: backlog.16

### Step C8.2 — fit_circle design review — named-struct return + χ²/ndf/status + round-trip test  [medium]
- **Files**: include/utility/circle_fit.h, src/writers/recodata/ring_compute.cxx, tests/
- **Action**: At circle_fit.h:39 redesign the API: init-value validation, fix_XY granularity, return a named struct carrying χ²/ndf/status. Adjust the two callers at ring_compute.cxx:82, :126. Add a round-trip test.
- **Verify**: New test passes; recodata canonical run produces identical ring fits within tolerance.
- **Closes**: backlog.23

---

## Cluster C9 — recodata writer QA (sensor-model + φ-gap + multi-window + Stage 2 mt + CB fit + thread-safety)  [P1 next, two-day]
_Locality: src/recodata_writer.cxx + src/writers/recodata/* + include/utility/root_hist.h_

Six BACKLOG rows + 2 small bugs all on the recodata side. Order: thread-safety
polish first (gates Stage 2 mt), then QA axis splits (sensor + φ-gap + multi-window),
then radial fit, then ring→radiator (blocked by C12 schema).

### Step C9.1 — recodata_writer raw AlcorSpilldata → unique_ptr + dead constexpr cleanup  [trivial]
- **Files**: src/recodata_writer.cxx, include/writers/recodata.h
- **Action**: At recodata_writer.cxx:137 convert raw `new AlcorSpilldata()` to `std::unique_ptr`. At recodata_writer.cxx:239 delete the dead `edge_rejection_ns` constant (live value is `BTANA_EDGE_REJECTION_NS`); delete the `#if 0` block at 890-1108.
- **Verify**: ASan clean; binary diff of writer output identical to baseline.
- **Closes**: memory_user_asks.23, memory_user_asks.24

### Step C9.2 — RootHist thread-safety polish for per-thread clone pattern  [small]
- **Files**: include/utility/root_hist.h, src/recodata_writer.cxx, src/writers/recodata/frame_pipeline.cxx
- **Action**: Audit and harden the per-thread clone pattern; document the invariant for Stage 2 mt callers.
- **Verify**: tsan canonical Stage 2 run clean.
- **Closes**: backlog.14

### Step C9.3 — Stage 2 multithreading (frames-within-spill)  [medium]
- **Files**: src/recodata_writer.cxx, src/writers/recodata/frame_pipeline.cxx, include/utility/root_hist.h
- **Action**: Populate the Stage 2 scaffold at recodata_writer.cxx:804 with frames-within-spill parallelism on top of the per-spill serial loop.
- **Verify**: Canonical run wall time decreases on multi-core; output hist content byte-identical to single-thread Stage 2.
- **Closes**: backlog.34

### Step C9.4 — Sensor-model split (k1350 / k1375) + φ-gap split (in_gap / ex_gap) in QA  [small]
- **Files**: src/recodata_writer.cxx, src/writers/recodata/frame_pipeline.cxx, src/writers/recodata/ring_compute.cxx, conf/readout_config.toml, src/mapping.cxx, include/utility/config_reader.h
- **Action**: Two parallel QA axis splits. Includes the readout_config per-(device,chip) sensor-type override (P 0.50) needed by the k1350/k1375 split.
- **Verify**: QA panel shows both splits; entries sum to the un-split baseline.
- **Closes**: backlog.24, backlog.26, backlog.42

### Step C9.5 — Multiple time windows in QA (prompt / early / DCR)  [medium]
- **Files**: src/recodata_writer.cxx, src/writers/recodata/frame_pipeline.cxx, src/writers/recodata/ring_compute.cxx, src/writers/recodata/radial_fit.cxx, src/writers/recodata/sigma_vs_n_fit.cxx
- **Action**: Restructure the histogram book to carry prompt/early/DCR windows side-by-side.
- **Verify**: New QA panels render; window definitions documented in include/writers/DISCUSSION.md.
- **Closes**: backlog.3

### Step C9.6 — Restore Crystal-Ball + pol3 with explicit analytic CB integral for N_γ  [medium]
- **Files**: src/writers/recodata/radial_fit.cxx, src/writers/recodata/sigma_vs_n_fit.cxx, src/recodata_writer.cxx, src/writers/recodata/ring_compute.cxx
- **Action**: Restore the CB + pol3 fit; replace numerical integration with the analytic CB integral for the photon-count.
- **Verify**: N_γ on canonical run matches the pre-revert value within tolerance.
- **Closes**: backlog.17

### Step C9.7 — Ring → radiator labelling  [medium]
- **Files**: src/recodata_writer.cxx, include/utility/radiator_efficiency.h, qa_quicklook/rundb.py, include/analysis_results.h
- **Action**: Requires C12 (per-run beam-metadata schema). Once the schema lands, wire the ring→radiator mapping via the rundb radiators field.
- **Verify**: Canonical run shows ring labels matching the run's radiators field.
- **Closes**: backlog.36

---

## Cluster C10 — recotrackdata + writer small cleanups  [P2 follow-up, half-day]
_Locality: src/recotrackdata_writer.cxx + include/alcor_recotrackdata.h_

Smaller writer-side items grouped because they all sit on the recotrack file path
and benefit from a single build cycle.

### Step C10.1 — calculate_angle_resolution — proper error propagation  [small]
- **Files**: include/alcor_recotrackdata.h, src/recotrackdata_writer.cxx, include/utility/circle_fit.h
- **Action**: Replace the stub (`return -1`) at alcor_recotrackdata.h:294 with proper telescope-plane geometric error propagation.
- **Verify**: Canonical run reports a believable σ_θ; sanity check vs an independent calculation.
- **Closes**: backlog.0

### Step C10.2 — recotrackdata: consume or strip track_data_repository / track_run_name  [trivial]
- **Files**: src/recotrackdata_writer.cxx, include/writers/recotrackdata.h, macros/utilities/recotrackdata_writer.cpp
- **Action**: Either honour the parameters at recotrackdata_writer.cxx:85 (fallback to data_repository + '/' + run_name when unset) or strip them from the signature.
- **Verify**: Operator pointing at a shared tracks dir produces an output that actually reads from that dir.
- **Closes**: memory_user_asks.26

---

## Cluster CD — Dashboard (qa_quicklook) — fat cluster: tab work + bugs + tests  [P1 next, two-day]
_Locality: qa_quicklook/*.py + tests/test_*.py_

Per the operating principle: dashboard iterates without a rebuild, so we batch
aggressively. 17 items including ship-blocker race conditions, the "...coming"
banner, history/results-update UX, retention test, threads-4 test, and the small
qa.py cleanups.

### Step CD.1 — rundb.update_run_field — cross-thread lock (DbWorker + sheets_worker race)  [small]
- **Files**: qa_quicklook/rundb.py, qa_quicklook/dbworker.py, qa_quicklook/sheets_sync.py, qa_quicklook/sheets_worker.py
- **Action**: At rundb.py:275 add a module-level threading.Lock around update_run_field/append_runs/delete_runs (or route sheets_worker writes through DbWorker FIFO).
- **Verify**: Stress: 16 concurrent threads writing disjoint fields → all 16 land. No silent drops.
- **Closes**: memory_user_asks.5

### Step CD.2 — Dashboard QA tab: "...coming" banner when run has writer/qa_pipeline running  [small]
- **Files**: qa_quicklook/qa.py, qa_quicklook/app.py, qa_quicklook/joblock.py, qa_quicklook/qa_pipeline.py, qa_quicklook/remote_watcher.py
- **Action**: On QA tab refresh check joblock cache for the inspected run_id; if locked, render a banner ("...coming — writer/qa_pipeline running"). Wire the banner to joblock-change events.
- **Verify**: Start a fake qa_pipeline run; switch to QA tab on that run — banner appears; on completion banner disappears and plots render.
- **Closes**: memory_user_asks.2, backlog.50

### Step CD.3 — qa_pipeline subprocess: stream stdout/stderr, no full-capture  [small]
- **Files**: qa_quicklook/qa_pipeline.py
- **Action**: At qa_pipeline.py:357 replace `subprocess.run(..., capture_output=True)` with `Popen` + iter stdout, line-by-line to qa.log.
- **Verify**: 50-spill run no longer balloons dashboard process memory; qa.log gets streamed output.
- **Closes**: memory_user_asks.33

### Step CD.4 — Show history right-click + rundb.results_update wrapper  [small]
- **Files**: qa_quicklook/qa.py, qa_quicklook/rundb.py, include/utility/audit.h, qa_quicklook/app.py
- **Action**: Implement using the UX shape locked in C0.4. Show-history tails the audit log filtered to (run, field); results_update is the per-field set/get with the audit-log distinguisher decided in C0.4.
- **Verify**: Right-click on a runcard field opens the chosen UX surface; entries show as expected; an operator override registers distinctly from a writer regeneration in the audit log.
- **Closes**: backlog.6, backlog.25

### Step CD.5 — Cross-run trend plots in QA tab General page  [medium]
- **Files**: qa_quicklook/cross_run_trends.py, qa_quicklook/qa.py, qa_quicklook/rundb.py, src/analysis_results.cxx
- **Action**: Build on the existing cross_run_trends.py scaffold; surface tiles on the QA tab General page.
- **Verify**: Trend tiles render for a multi-run window; values match a manual extraction from standard_results.toml.
- **Closes**: backlog.7

### Step CD.6 — Quality-tagging cockpit integrated with Runlist  [small]
- **Files**: qa_quicklook/qa.py, qa_quicklook/runlist.py, qa_quicklook/runlists.py, qa_quicklook/rundb.py
- **Action**: Per C0.4 shape decision (side-panel vs modal). Wire the quality-tag UI on the QA tab to the runlist data.
- **Verify**: Tagging a run from the cockpit reflects in the runlist within one refresh cycle.
- **Closes**: backlog.37

### Step CD.7 — Replace _MacrosPlaceholder with real macro launcher  [small]
- **Files**: qa_quicklook/qa.py
- **Action**: At qa.py:1773 replace _MacrosPlaceholder with a curated launcher targeting the macros/examples/ .cpp set.
- **Verify**: Each curated macro launches; failures (ACLiC issues per C13.x) surface a clean error to the operator.
- **Closes**: backlog.35

### Step CD.8 — Settings: "Save as setting set" stub  [small]
- **Files**: qa_quicklook/settings.py, qa_quicklook/conf_layout.py
- **Action**: Package the working overlay into `conf/sets/<name>/`. Both dirs already exist.
- **Verify**: Save → conf/sets/<name>/ contains the overlay; restart and load → working state restored.
- **Closes**: backlog.39

### Step CD.9 — Radiators editing in Run-info card (read-only v1)  [small]
- **Files**: qa_quicklook/qa.py, qa_quicklook/rundb.py, include/utility/radiator_efficiency.h
- **Action**: Add the read-only display. Acts as the C9.7 (ring→radiator) prereq.
- **Verify**: Run-info card shows the radiators field from rundb; no edit path in v1.
- **Closes**: backlog.41 (= C12 in the dependency graph)

### Step CD.10 — Wire tools/check_qa.py into dashboard QA-tab refresh path  [small]
- **Files**: tools/check_qa.py, qa_quicklook/qa.py, qa_quicklook/qa_pipeline.py
- **Action**: Call check_qa.py during QA-tab refresh; surface failures inline.
- **Verify**: Stale QA output triggers a visible warning in the tab.
- **Closes**: backlog.13

### Step CD.11 — Per-run streaming n_sigma_threshold field — wire via per-run TOML  [small]
- **Files**: run-lists/2025.database.toml, macros/utilities/lightdata_writer.cpp, include/utility/config_reader.h, qa_quicklook/rundb.py
- **Action**: Pick option 1 (per-run TOML field; writer CLI driver overrides streaming_trigger_cfg.n_sigma_threshold post-parse). Add the field to the rundb schema; surface in the Run-info card.
- **Verify**: Edit the per-run field, re-run writer, observe the override applied (config dump reflects new threshold).
- **Closes**: discussion.2

### Step CD.12 — Per-run progress events via `[QA] {json}` (writer side)  [small]
- **Files**: src/lightdata_writer.cxx, src/recodata_writer.cxx, src/recotrackdata_writer.cxx, qa_quicklook/qa_pipeline.py, qa_quicklook/app.py
- **Action**: Add the per-spill JSON progress emit from the three writers (reader is already shipped).
- **Verify**: Progress bar in dashboard advances per spill not per writer.
- **Closes**: backlog.20

### Step CD.13 — Small bugs/hardening sweep across qa_quicklook  [small]
- **Files**: qa_quicklook/app.py, qa_quicklook/qa_pipeline.py
- **Action**: Batch the small items: app.py:498 narrow exception catching (memory_user_asks.34); app.py:231 matplotlib prewarm on main thread (memory_user_asks.35); app.py:817 QProcess parent (memory_user_asks.36); app.py:189 QFileSystemWatcher parent-dir addPath (memory_user_asks.37); app.py:608 QAQ_DISABLE_STARTUP_SWEEP truthy parse + README doc (memory_user_asks.38); app.py:865 stale shell-comment fix + shlex.split migration (memory_user_asks.32); qa_pipeline.py:46 drop dead imports / hoist `re` (memory_user_asks.39); qa_pipeline.py:506-513 don't print "QA done" when nothing ran (memory_user_asks.41).
- **Verify**: ASan/lint canonical session clean; dashboard launches without warnings on a broken dashboard_config.toml.
- **Closes**: memory_user_asks.32, 34, 35, 36, 37, 38, 39, 41

### Step CD.14 — Tests: retention joblock + qa_pipeline threads-4 + lightdata-failure dead-OR  [trivial]
- **Files**: tests/test_retention.py, tests/test_qa_pipeline.py
- **Action**: (a) Add an integration test for retention.sweep+apply that verifies a joblock'd run survives. (b) Add parametric test at test_qa_pipeline.py:38-50 asserting default `--threads 4` is in the writer argv. (c) Drop the dead OR clause at test_qa_pipeline.py:181 that masks real failure.
- **Verify**: pytest tests/ green; flipping --threads default to auto in qa_pipeline.py fails the new test; reverting C1/C2 fixes breaks the relevant integration tests.
- **Closes**: memory_user_asks.30, memory_user_asks.42, memory_user_asks.40

---

## Cluster C11 — Mapping + RunInfo + macros ACLiC clean-up  [P2 follow-up, one-day]
_Locality: include/mapping.h + src/mapping.cxx + macros/examples/_

Lower-priority code-shape cluster. Resolves D-10 mapping decisions, the
mapping cache extension, and gets macros/examples/ ACLiC-clean (which unblocks C13's
CI macro compile-check).

### Step C11.1 — D-10 — Mapping + RunInfo static-map / no-mutex / float-key-of-array decisions  [medium]
- **Files**: include/mapping.h, src/mapping.cxx, include/utility/config_reader.h, include/utility/global_index.h
- **Action**: Resolve the 3 parked decisions. Implement the chosen approach.
- **Verify**: Canonical run produces identical mapping content; tsan clean on the no-mutex path.
- **Closes**: backlog.33

### Step C11.2 — mapping.h: full-coverage map (Cartesian + R-φ) + cache profiling  [medium]
- **Files**: include/mapping.h, src/mapping.cxx, include/utility/global_index.h
- **Action**: Extend the cache; profile cache-vs-on-the-fly to pick a default.
- **Verify**: Lookup unit test passes for full (channel, tdc) coverage; profile recorded in DISCUSSION.md.
- **Closes**: backlog.44

### Step C11.3 — ring-fit example macros: sensor-type flag (3 macros, same TODO) + invert-bug fix + commented-out-gate decision  [small]
- **Files**: macros/examples/ring_spatial_resolution.cpp, macros/examples/ring_spatial_resolution_with_tracking.cpp, macros/examples/photon_number_memory.cpp
- **Action**: (a) Plumb a `sensor_type` flag through the three macros and gate `is_ring_tagged()` on it. (b) Flip the inverted gate at ring_spatial_resolution_with_tracking.cpp:153 (currently `if (is_ring_tagged) continue;` — drops ring hits; sibling does the opposite). Verify against recotrackdata convention before flipping. (c) Restore or delete the commented-out gate at photon_number_memory.cpp:234-235.
- **Verify**: All three macros produce ring-only fits for a canonical run; sibling-to-sibling consistency check passes.
- **Closes**: backlog.45, inline_todos.1, inline_todos.2, inline_todos.3

### Step C11.4 — macros/examples/ ACLiC-clean state  [medium]
- **Files**: macros/examples/*
- **Action**: Get every macro to compile under ACLiC. Required by C13's CI compile-check step.
- **Verify**: `root -l -b -q '.L macros/examples/<each>.cpp+'` returns 0 for every macro.
- **Closes**: backlog.43

---

## Cluster C12 — AnalysisResults + per-campaign isolation + sensor-name vocab  [P2 follow-up, half-day]
_Locality: src/analysis_results.cxx + include/analysis_results.h + qa_quicklook/rundb.py_

Schema housekeeping on the AnalysisResults TOML backend. C2.1 already shipped
the atomic-write fix; this is the per-campaign isolation + vocab pinning.

### Step C12.1 — Per-campaign file isolation (standard_results.toml → run-lists/<year>.results.toml)  [small]
- **Files**: src/analysis_results.cxx, include/analysis_results.h, qa_quicklook/rundb.py, qa_quicklook/runlist.py, include/utility/audit.h
- **Action**: Split standard_results.toml per campaign year; update reader/writer + audit log.
- **Verify**: Multi-year run-list reads the right file per run; old standard_results.toml is migrated cleanly.
- **Closes**: backlog.48

### Step C12.2 — Pin sensor-name vocabulary for results.toml  [trivial]
- **Files**: include/analysis_results.h, qa_quicklook/rundb.py, qa_quicklook/DISCUSSION.md
- **Action**: Define a constexpr / enum set of permitted sensor names ("all" / "1350" / "1375" / ...). Assert in the writer; lint check in tools/.
- **Verify**: Writing an off-vocab sensor name trips the assert; trend plot scan is clean of typos.
- **Closes**: discussion.8

---

## Cluster C13 — CI + lint + docs (README sweep + utility/ + writers/ + qa_quicklook/)  [P2 follow-up, one-day]
_Locality: README.md + .github/workflows/ + include/utility/README.md + include/writers/README.md + qa_quicklook/README.md + DISCUSSION.md_

Pure doc + CI sweep. Lump together because they all involve reading docs cold and
fixing stale references. Ship AFTER C11.4 (ACLiC-clean) so the CI compile-check
actually passes.

### Step C13.1 — CI workflow re-enable (build + test + macro compile-check)  [medium]
- **Files**: .github/workflows/, tools/lint_codebase.py, tools/check_qa.py
- **Action**: Implement the strategy chosen in C0.5. Land the build + test workflow; wire tools/check_qa.py + macro compile-check.
- **Verify**: PR check is green on the canonical branch.
- **Closes**: backlog.1

### Step C13.2 — utility/qa_publish.h adoption across writers + manifest emit (optional)  [small]
- **Files**: include/utility/qa_publish.h, src/lightdata_writer.cxx, src/recodata_writer.cxx, src/recotrackdata_writer.cxx, src/writers/pulser_calib_writer.cxx, qa_quicklook/qa.py
- **Action**: Adopt the qa_publish helper across the four writers with a uniform `pdf_path(run_dir, step, order, name)` convention. Optionally emit a `qa/<step>/index.json` manifest (captions/units/expected ranges) for the dashboard.
- **Verify**: All four writers produce paths under the same convention; if manifest shipped, dashboard renders the captions.
- **Closes**: backlog.40, discussion.9

### Step C13.3 — Top README sweep (paths + umbrella + pipeline diagram + lint badge + design-questions section)  [small]
- **Files**: README.md
- **Action**: Fix all stale paths and umbrella references: include/util/ → include/utility/; drop include/utility.h umbrella claim; fix GlobalIndex (3 links) + toml_utils.h links; add include/utilities/ (-ies) as a separate row; add Lint badge + CI section update; rewrite §Open design questions to point at DISCUSSION.md hub + satellites; add pulser_calib + qa_pipeline stages to the Data Pipeline diagram; trim example macros table (or pointer); fix btana-dump build path (`build/bin/` not `bin/`); doxygen.yml mention; add `pytest tests/` to Testing section.
- **Verify**: Every README link resolves (`find . -name "*.md" -exec markdown-link-check {} \;` clean).
- **Closes**: readme.0, readme.1, readme.2, readme.3, readme.4, readme.5, readme.6, readme.7, readme.8, readme.9, readme.19

### Step C13.4 — qa_quicklook/README.md sweep (tabs / layout / qa_pipeline CLI / workflow)  [small]
- **Files**: qa_quicklook/README.md
- **Action**: Resolve the tabs-intro-vs-table contradiction; add Layout rows for qa_pipeline / retention / remote_watcher / cross_run_trends / _sheets_format; add a §QA pipeline CLI section mirroring the docstring; add a §Typical shift workflow walkthrough; note the "...coming" banner from CD.2 as current behaviour.
- **Verify**: A fresh shifter can follow the README from launch → QA pipeline → quality-tag without external help.
- **Closes**: readme.10, readme.11, readme.12, readme.13, readme.14

### Step C13.5 — include/utility/README.md + include/writers/README.md + include/triggers/README.md  [small]
- **Files**: include/utility/README.md, include/writers/README.md, include/triggers/README.md
- **Action**: Drop the umbrella claim in utility/README.md; add audit.h / qa_publish.h / config_dump.h to the file-by-file table; add lightdata/ sub-table to writers/README.md; verify triggers.h still exists and is referenced — if dead, delete and update; codify the non-copyable + non-movable convention.
- **Verify**: `test -f` every claimed path; grep for `#include "triggers.h"` to decide its fate.
- **Closes**: readme.15, readme.16, readme.17, readme.18, discussion.17

### Step C13.6 — Streaming DISCUSSION.md sweep: retire stale fit_circle audit § entries  [trivial]
- **Files**: include/triggers/streaming/DISCUSSION.md
- **Action**: Remove the now-moot fit_circle audit entries from § 2.4 / § 2.5 / § 2.3.2; consolidate surviving open items into a single consumer-side note.
- **Verify**: Doc lint clean; no internal cross-refs point at deleted sections.
- **Closes**: discussion.14

### Step C13.7 — D-02 root_hist<T> wrapper — write the wrapper OR correct the BACKLOG claim  [small]
- **Files**: include/utility/root_hist.h, src/lightdata_writer.cxx, src/recodata_writer.cxx, BACKLOG.md, DISCUSSION.md
- **Action**: Choose: (i) write the RootHist wrapper that BACKLOG claims exists, OR (ii) re-open D-02 as a real item, OR (iii) correct the BACKLOG note. Recommended: (iii) for now + (i) when the macro layer is refactored.
- **Verify**: DISCUSSION.md + BACKLOG.md tell the same story about D-02.
- **Closes**: discussion.15

---

## Cluster C14 — Schema / "event" class (xlarge, last)  [P3 nice-to-have, three-plus-day]
_Locality: include/alcor_recodata.h + include/alcor_recotrackdata.h + include/alcor_data.h + macros/examples/_

The two xlarge schema rewrites. Sequenced last because both depend on C0.1 (D-08
decision) and benefit from every cluster above having stabilised the surrounding
APIs first.

### Step C14.1 — D-05: introduce 'event' class consolidating ring + circle + timing + afterpulse + crosstalk  [large]
- **Files**: include/, include/alcor_recodata.h, include/alcor_recotrackdata.h, include/utility/ring_model.h, include/utility/circle_fit.h
- **Action**: Resolve the 5 parked D-05 sub-questions; design and ship the event class; migrate primary consumers.
- **Verify**: Macros and writers compile against the new class; output is byte-identical on a canonical run.
- **Closes**: backlog.31

### Step C14.2 — D-08 round 2 — AlcorRecodata (10 macros) + AlcorRecotrackdata (3 files) + AlcorData migration  [xlarge]
- **Files**: include/alcor_recodata.h, include/alcor_recotrackdata.h, include/alcor_data.h, macros/examples/
- **Action**: Two-layer migration per the C0.1 decision. Honour the non-copyable + non-movable invariant from discussion.17.
- **Verify**: 10 macros + 3 files migrated; pytest tests/ + canonical writer run still green.
- **Closes**: backlog.32

---

## Deferred (not in any cluster)

- **Lightdata writer magic constants → config** (discussion.12) — Defer per the "avoid early generalisation" principle until the next campaign demands re-tuning.
- **alcor_recotrackdata.h plane-distances + pixel-resolutions → config** (discussion.13) — Same as above; promote when geometry changes.
- **Sheet-side edit attribution via drive.metadata.readonly + revisions.list** (discussion.10) — Held until operators ask for per-edit attribution; trade is wider IAM scope.
- **Sheets reverse-merge for structural fields (radiators, schema_extras)** (discussion.11) — Defer until workflow demands editing structural fields in the Sheet.
