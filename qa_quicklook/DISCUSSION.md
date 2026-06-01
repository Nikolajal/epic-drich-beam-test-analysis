# qa_quicklook — longer-term roadmap

> 🧭 **Hub:** project-wide design log + index of satellites lives at
> [`../DISCUSSION.md`](../DISCUSSION.md).  Open items here also show
> up in the top-level [`BACKLOG.md`](../BACKLOG.md).

Items the dashboard *should* do eventually but that aren't pressing.
This file is intentionally short — only entries that have made it
through "would this actually be used during a shift?".

## Writers → QA: what to publish, and how

The QA tab now has per-step sub-tabs (*Lightdata / Recodata /
Recotrack / Macros*) and each one looks for two things on disk for
the selected run:

  1. ``<run>/qa/<step>/*.pdf`` — writer-emitted PDFs, **preferred**
     (writer fully owns the look-and-feel; dashboard just renders).
  2. ``<run>/<step>.root`` — TH1/TH2 thumbnails via uproot + matplotlib
     (fallback; useful while writers don't yet emit PDFs, and as the
     "browse everything" path even after they do).

The PDF path is the lighter-weight contract: writers already use
``TCanvas``; one ``c->SaveAs(qa_dir / "<plot>.pdf")`` per canvas
gives us shareable, publication-quality artefacts that the dashboard
shows inline + the operator can attach to slides one click later.

### Convention to settle

Pinning the directory layout up-front so all writers + dashboard
agree:

```
Data/<run-id>/
  qa/
    lightdata/
      01_trigger_qa.pdf
      02_timing_qa.pdf
      03_dcr_per_pixel.pdf
      …
    recodata/
      01_ring_overview.pdf
      02_radial_fit.pdf
      …
    recotrack/
      …
    calibration/
      01_pulser_calib_qa.pdf
      …
```

The numeric prefix governs render order on the QA sub-tab (sorted
alphabetically).  Writers should keep the count small and curated —
the goal is "shift readout in 30 seconds", not "every diagnostic".
The full set still lives in the ``.root`` files for deep dives.

### What each writer should publish (first cut — to iterate)

| Step | First-cut publishable plots |
|---|---|
| **lightdata** | trigger-rate QA · timing alignment (Δt vs Cherenkov peak) · single-pixel noise (DCR per channel) · afterpulse + crosstalk sideband fits · per-spill hit count |
| **recodata** | ring overview (azimuth-vs-radius) · radial fit per ring · σ_R vs N · coverage map · efficiency(R) |
| **recotrack** | track-matched hit map · track multiplicity · per-ring acceptance · matched-hits-per-track distribution |
| **calibration** | pulser slope distribution · fine-cal residuals · skipped-TDC list as a one-page summary |

### Helper API to land in C++

A thin wrapper around ``TCanvas::SaveAs`` so writers don't each
hand-roll the path logic:

```cpp
// include/utility/qa_publish.h
namespace util::qa {
  // Resolve <repo>/Data/<run>/qa/<step>/<numbered-name>.pdf and
  // make the parent dirs.  Caller owns the canvas.
  std::filesystem::path pdf_path(const std::string& run_dir,
                                 const std::string& step,
                                 int order, const std::string& name);
}
```

Usage::

    TCanvas c("c_trigger_qa", "", 1200, 800);
    // … draw …
    c.SaveAs(util::qa::pdf_path(run_dir, "lightdata", 1, "trigger_qa")
             .c_str());

### Open questions

  - Should the dashboard ever **trigger a re-publish** (run the writer
    in a "PDFs-only" mode)?  Probably no — writers always emit; if
    the operator wants a refresh they re-launch the writer.
  - Should we bake a **manifest** (``qa/<step>/index.json``) the
    writer also writes, so the dashboard knows captions / units /
    expected-ranges per PDF?  Nice-to-have, not blocking.

## Cross-run trends: revamp ``AnalysisResults``

The codebase already has the **right abstraction** for cross-run
trends — ``include/analysis_results.h``::

  ResultKey = (run, sensor, quantity)     // composite key
  ResultEntry = (value, error)            // value + 1-σ
  AnalysisResults  = ROOT-backed map  with load() / update() upsert
  query_run(map, run, sensor)             // slice helper
  // + a graph-builder for "scan a quantity across a runlist"

What's missing today:
  - **Writers don't emit into it.**  Today only post-processing
    macros write entries; the per-run writers (lightdata / recodata /
    recotrack / pulser_calib) don't.  Every interesting scalar they
    compute (N_γ, σ_R, fit-χ², per-radiator efficiency, fine-cal
    slope, …) should land here as part of the same Config/-style
    "self-describing output" pass.
  - **No dashboard consumer.**  The QA tab's *cross-run trends*
    sub-tab (queued as a fourth shape originally; folded out when we
    went per-step) wants exactly this — pick (sensor, quantity), pick
    a runlist filter, plot vs run-id or vs a scan variable.
  - **Per-campaign file isolation.**  Today the path is hardcoded
    (``<data_repository>/standard_results.toml`` — typically
    ``Data/standard_results.toml``).  With one database per
    year / beam test now landing in ``run-lists/<year>.database.toml``,
    the natural symmetry is ``run-lists/<year>.results.root`` (or
    similar).  The ``AnalysisResults`` ctor already takes a path; we
    just need the writers + dashboard to pick the right one.

### Writer responsibilities (proposed)

Each writer's existing ``Config/`` pass already enumerates its
scalars for **reproducibility**.  Add a parallel ``Results/`` pass
that streams the same numeric outputs into ``AnalysisResults``
keyed by ``(this_run, "all", "lightdata.<name>")`` /
``"recodata.<name>"`` etc.  Concretely:

| Step | Candidate scalars to publish |
|---|---|
| **lightdata** | ``n_events``, ``n_spills``, ``afterpulse_frac``, ``ct_phys_frac``, ``mean_dcr_hz``, per-rdo trigger count |
| **recodata** | ``n_rings``, per-radiator ``n_gamma`` / ``sigma_R`` / ``fit_chi2_ndf`` / ``ring_centre_x/y`` |
| **recotrack** | ``n_matched_tracks``, per-radiator ``acceptance``, ``residual_sigma`` |
| **calibration** | ``mean_slope``, ``slope_spread``, ``n_skipped_tdcs``, ``n_pairs_used`` |

These are exactly the numbers that need cross-run trending.

### Dashboard side (proposed)

The QA tab gets back a **Trends** sub-tab (or a new top-level "Trends"
under Run Info → reflects the same data layer):

  - Picker for **sensor** + **quantity** (both populated from the
    results file's actual contents — no hardcoded list).
  - Picker for **scope**: a named runlist, an ad-hoc selection from
    the Database tab, or "all runs".
  - **Trend plot** via matplotlib (re-uses the same renderer the QA
    thumbnails already pull in) — vs run-id by default, with an
    optional dropdown to plot vs a scan variable from the database
    (V_bias, temperature, beam energy, …).
  - The plot is also exportable as a PDF into ``Data/<run>/qa/trends/``
    so a "selected scan" can be archived alongside the source runs.

### Open questions for cross-run

  - One results file per year/campaign (``<year>.results.root``) or
    one per scan/runlist?  Lean *one per campaign* — matches the
    database picker, and a "scan view" is a query filter, not a
    storage boundary.
  - When does the writer write?  At the end of its own run, just
    after Config/ — same TFilePtr scope.  Append-only via
    ``AnalysisResults::update``; no race when two writers touch the
    same file because each operates on a different ``run`` key.
    Concurrent writes by the same writer for the same run won't
    happen (we have the joblock mutex already).
  - **Schema evolution.**  ``ResultKey`` is currently three strings.
    If we ever want extra dimensions (e.g. ``radiator``, ``ring_id``)
    they can stay packed into ``quantity`` (``"recodata.ring1.n_gamma"``)
    or become first-class — pin via use cases, not speculation.

### Migrating ``AnalysisResults`` to TOML

> **🟢 STATUS UPDATE 2026-05-30 — SHIPPED.**  The migration described
> below LANDED in commit 76363b9 (via the 2026-05-29 dashboard / TOML
> stash integration).  ``src/analysis_results.cxx`` now reads/writes
> a single hand-readable ``standard_results.toml`` via toml++; the
> ROOT TTree code path is gone.  Dashboard reader
> ``qa_quicklook.rundb.results_load`` was already TOML-only.
>
> The text below is the **original migration proposal**, kept for
> historical context (why the choice was made + what trade-offs were
> evaluated).  A clean rewrite into a "shipped-feature summary +
> current limitations + next steps" section is tracked as a Sunday
> follow-up (the "qa_quicklook/DISCUSSION.md rewrite" Doc row in
> ``BACKLOG.md``); for now this banner makes the shipped status
> unambiguous on a casual read.
>
> Current open items (now-shipped section is below for reference):
> * Atomic flock + tmp+rename for ``update()`` — concept from
>   CLEAN_OFF C2.1, superseded by the TOML migration; re-apply on
>   the TOML backend Sunday.
> * ``rundb.results_update`` Python wrapper for operator overrides
>   (BACKLOG row P 1.33, still READY).
> * Right-click "Show history" popup → tail audit log for a
>   ``(run, field)`` (BACKLOG row P 1.67, still READY).

The ROOT-TTree backing was a fine choice when only ROOT macros
touched the file, but with the dashboard becoming the primary
consumer the calculus has flipped:

  - The dashboard already speaks TOML fluently (``run-lists/*.toml``,
    ``conf/*.toml``).  Reading results from TOML reuses the same
    ``tomllib``-fast path that gave the Database tab its 500×
    speedup, no ROOT runtime needed in Python.
  - The file becomes **human-readable and git-mergeable** — exactly
    like the run database next to it.  Operators can audit / hand-
    edit / cherry-pick entries; PR diffs show what changed.
  - It pairs naturally with the per-campaign file convention:
    ``run-lists/2025.database.toml``  ↔  ``run-lists/2025.results.toml``.

Proposed schema (nested-tables flavour, the most compact):

```toml
# run-lists/2025.results.toml — produced by the writers,
# consumed by the QA Trends sub-tab.

# Per-run, per-sensor, per-quantity entry.  Tables nested by
# (run, sensor); each leaf is a {value, error} inline table.
[results."20251111-181940".all]
"lightdata.n_events"        = { value = 1.234e6 }
"lightdata.afterpulse_frac" = { value = 0.0124, error = 0.0003 }
"recodata.n_rings"          = { value = 412 }

[results."20251111-181940"."1350"]
"recodata.ex_gap.n_gamma"   = { value = 12.3,  error = 0.5 }
"recodata.ex_gap.sigma_R"   = { value = 1.85,  error = 0.07 }

[results."20251111-181940"."1375"]
"recodata.ex_gap.n_gamma"   = { value = 11.8,  error = 0.5 }
```

Reading is then ``tomllib.load() → dict["results"][run][sensor][q]``.
``error`` is optional (omitted ↔ 0, mirroring the existing dataclass
default).  Comments above an entry can carry units / context — they
survive ``tomlkit`` write-back.

### C++ side

The writers (and existing post-processing macros) keep calling
``AnalysisResults::update(...)``; only the **backing** changes.
Drop ``ROOT::TTree`` from ``include/analysis_results.h``, add a
small ``toml++``-backed reader/writer that:

  - Reads the file into a ``ResultMap`` (one-shot, on construction
    or lazily on ``load()``).
  - On ``update()``, merges entries into the in-memory map and
    rewrites the whole file via ``toml::table`` → string → atomic
    ``rename``.  Same upsert semantics as today.
  - Preserves comments by going through ``toml::parse`` (which
    keeps trivia) rather than building a fresh document.

The Python side (``qa_quicklook``) gets a new ``rundb.results_load``
helper using stdlib ``tomllib`` for the fast read path — symmetric
to ``rundb.load_database``.

### Migration plan

  1. Add ``results.toml`` reader/writer alongside the existing ROOT
     code.  Both backends present, ``update()`` writes to both
     temporarily.
  2. Migrate readers (one ROOT macro at a time) to read the TOML
     instead.
  3. Drop the ROOT-TTree path once nothing reads it.  Convert
     ``<data_repository>/standard_results.toml`` once via a one-shot
     script.

Until step 3, ``.root`` and ``.toml`` are kept in sync, so a partial
migration is always safe to roll back.

### Open questions

  - Does any *external* consumer (off-tree macro, plotting notebook,
    student's repo) still rely on the ``.root`` file?  If yes, keep
    the dual-backend phase longer; if no, the migration shrinks to
    one PR.
  - Sensor names other than ``"all"`` / ``"1350"`` / ``"1375"`` —
    fine in TOML (quoted keys), but worth pinning the naming
    convention so trend plots aren't full of typos.

## Provenance: which program wrote the entry

Cross-cuts both the run database and the proposed ``results.toml``.
Today an entry is just ``value`` (+ ``error``); nothing records
which program put it there, so when two numbers disagree we have no
trail.

Keep it minimal: **one short string per write event saying what
produced it**.  No version, no operator login, no hostname — those
are forensic luxuries we can layer on later if a use case demands
them.

### Source values

A small open-ended vocabulary, lowercase, one word::

    "lightdata"        — produced by the lightdata_writer binary
    "recodata"         — recodata_writer
    "recotrack"        — recotrackdata_writer
    "calibration"      — pulser_calib_writer
    "dashboard"        — manual edit via the GUI
    "photon_number"    — named ROOT macro
    "sheets"           — pulled from the shared Google Sheet (future)

Same shape on both sides: the writers and the dashboard pass their
own name; macros pass the macro stem; the import paths pass the
transport name.

### Where the source lands: per-write audit log

Two parallel append-only logs (one per data file) keep the data
files themselves flat and diffable::

    # run-lists/2025.database.audit.toml
    [[entry]]
    at        = "2025-05-28T10:30:11"
    source    = "dashboard"
    run       = "20251111-181940"
    field     = "quality"
    old_value = "need QA"
    new_value = "good"

    # run-lists/2025.results.audit.toml
    [[entry]]
    at        = "2025-11-12T03:15:22"
    source    = "recodata"
    run       = "20251111-181940"
    sensor    = "1350"
    quantity  = "recodata.ex_gap.n_gamma"
    value     = 12.3
    error     = 0.5

Why a sibling log instead of inlining ``source`` on every leaf:
the data files stay scannable (a ``[runs.X]`` table is small enough
to read at a glance), forward-inheritance / auto-pin need no schema
change, and "show history of this field" becomes a trivial
tail-match in the dashboard.

### C++ side — landed

Header-only helper at ``include/utility/audit.h``::

    namespace util::audit {
        std::filesystem::path sibling_audit_path(const std::string& primary);
        void log(const std::string& audit_path,
                 const std::string& source,    // "lightdata", "recodata", ...
                 const std::string& run,
                 const std::string& sensor,
                 const std::string& quantity,
                 double value, double error = 0.0);
    }

Append-only, open-append + ``std::mutex`` for in-process thread
safety; POSIX ``O_APPEND`` covers cross-process atomicity for the
small (~150 B) blocks we emit.

``AnalysisResults::update`` grew an optional ``source`` parameter:
when non-empty, every key in the batch generates an ``[[entry]]``
in ``<basename>.audit.toml`` next to the primary file.  The four
writers (lightdata / recodata / recotrack / calibration) all pass
their own name.

### Python (dashboard) side — landed

  - ``rundb.update_run_field(..., source="dashboard")`` now appends
    one block per edit to ``<db>.audit.toml`` via the new private
    ``_append_audit_entry``.  Default source is ``"dashboard"`` to
    match the most common caller (the GUI); pass ``source=""`` to
    suppress logging (internal migrations etc.).
  - Audit failures log to stderr but never block the primary edit.
  - "Show history" UI not built yet — tail the sibling file by hand
    for now.

### Open questions

  - ~~**Old entries with no provenance.**~~  **Backfilled** —
    a one-shot migration was run that emitted one ``[[entry]]`` per
    (run, field, value) pair from ``run-lists/*.database.toml`` with
    ``source = "legacy"`` and the current value as ``new_value``.
    The audit log now distinguishes pre-history records from
    dashboard-sourced ones.
  - **``rundb.results_update`` Python wrapper.**  Not built — the
    C++ writers feed AnalysisResults directly today.  When the
    dashboard ever needs to write results (e.g. operator-entered
    fit overrides), this is the symmetrical entry point.
  - **"Show history" UI.**  Still pending — tail the sibling
    ``*.audit.toml`` by hand for now.  Right-click on a runcard
    field → filter the audit log down to that (run, field) is the
    target shape.


## Cross-shifter sync

Multiple operators at the beam test should see a consistent view of
the run state — who started which writer on what run, current
status, latest run-database edits, named runlists, quality tags.
Today everything is local to each operator's `~/.cache/qa_quicklook/`
and their own working tree.

Candidate transport (in order of fit, not just complexity):

  1. **Google Sheets mirror** *(preferred)* — a small background
     pusher reads the run database + lock states and writes them to
     a shared Google Sheet.  Every operator (and any interested
     onlooker) sees the same state in their browser; no infra to
     deploy; the sheet itself handles concurrency / history / access
     control / archival forever.  Push-only model from the dashboard
     (each operator's local state is still authoritative); the sheet
     is the *observable* surface, not a source of truth.  Auth via
     a service-account JSON key kept in the operator's home dir,
     never committed.
  2. **Shared on-disk state** — point `joblock.CACHE_DIR` at a
     network mount (NFS / shared FS).  Cheap; needs the mount to
     exist; race-prone if two operators trigger the same writer.
  3. **Per-shifter HTTP gossip** — a tiny long-poll endpoint each
     dashboard hosts; peers register and pull state.  No central
     server; needs every operator on the same network segment.
  4. **Central state daemon** — one daemon at the beam-test location
     owns lock files + database edits; dashboards talk to it.  Right
     answer if we want a single source of truth; cost is a service
     to run.

Out of scope for the operator dashboard.  Mentioned here so it
doesn't get lost.

## Other longer-term items

  - **Per-run progress events** via the lapsed `[QA] {json}` push
    protocol from the writers (currently the dashboard only sees the
    raw stdout/stderr stream).
  - **Embedded ROOT canvases** — current `Inspect` button opens
    TBrowser as a separate process; embedding a `TRootEmbeddedCanvas`
    in a Qt tab is possible via PyROOT but has thread-loop costs that
    aren't worth it until the workflow demands it.
  - **Cross-run trend plots** in the QA tab — pick a metric (σ,
    n_triggers, n_rings, …) and a filter (e.g. all runs at −28 °C);
    plot vs run id or vs a chosen scan variable.
  - **Quality-tagging cockpit** integrated with the Runlist tab —
    set quality/notes from inside the dashboard; would slot into the
    runcard editor once it lands.

---

## Decisions captured 2026-05-29

### Cross-shifter sync — Google Sheets push (shipped 2026-05-29)

Implementation landed.  Five-worksheet push (`runs`, `runlists`,
`jobs`, `audit`, `meta`), driven by a `QThread` worker
(`qa_quicklook/sheets_worker.py`) that ticks every
`[sheets_sync].push_interval_s` seconds and skips the network round-
trip when no source file has changed.  Reverse-merge via cell-diff
against a `~/.cache/qa_quicklook/sheets_last_pushed.json` baseline:
Sheet-side edits that don't match the current local TOML get
replayed through `rundb.update_run_field(..., source="sheet")` so
they pick up the same audit log + Show-history surface as a
dashboard edit.  Last-write-wins on conflicts.  Auth is a
service-account JSON key shared with the Sheet as Editor —
operator setup runbook lives in `qa_quicklook/README.md` →
"Cross-shifter sync (Google Sheets)".

**Column-add policy (2026-05-29):** new tracked quantities (= new
columns on the runs worksheet) can ONLY be added from the dashboard,
not from the Sheet.  Two enforcement layers:

  1. The dashboard's runcard `+ Field` button calls
     `rundb.add_schema_extra` which writes to `[schema] extra_fields`
     in the year's database TOML.  The Sheet picks up the extension
     on the next push because `Snapshot.schema_extras_by_year` drives
     the per-year column list.
  2. The Phase D.14 integrity check rejects Sheet-side column
     additions (or renames) as `header drift on Runs (YYYY)`.  When
     it fires, the push goes into hard-reset mode: skip the reverse-
     merge, clear the snapshot, rewrite the canonical local state
     over whatever the operator did on the Sheet.  Status bar +
     audit log carry the trace.

This keeps the schema mutations centralised + auditable while still
letting the Sheet be a real edit surface for VALUES on existing
columns.

Deferred to follow-ups (not blockers for v1):

  - **Editor identity** on Sheet-side edits — currently logged as
    bare `source="sheet"`.  Adding `drive.metadata.readonly` scope
    + a `revisions.list` query per push would let us attribute to
    the Google user, at the cost of a wider IAM scope.  Re-open if
    operators ask for per-edit attribution.
  - **Structural fields** (`radiators`, etc.) skip the reverse-merge —
    they're JSON-encoded into a single cell on push but the round-
    trip risk for partial edits is too high for v1.  Operators
    edit structural fields via the dashboard.  Add a parser pass
    if the workflow demands editing them in the Sheet.
  - **DeveloperMetadata** for per-cell push watermarks — the
    cell-diff snapshot approach in the shipped design sidesteps
    this entirely.  Worth revisiting only if the snapshot turns out
    to be a fragility source (e.g. operators wipe `~/.cache`
    routinely).

### Cross-run trend plots — MVP

**MVP plotset**: `N_γ per ring`, `σ_single`, `χ²/ndf` — all three vs
run id, three independent figures.  These are the headline reco
observables that track campaign health most directly.  *Not* a
fully-operator-driven axis picker (would balloon the UI before we
know what shifters actually scan).  Cadence: refresh on QA-tab
focus + manual refresh button.  Source = AnalysisResults reader
already plumbed in `qa_quicklook/rundb.py`.

### "Show history" right-click on runcard fields

UX/scope deferred to a follow-up DISCUSSION pass — the row was
parked rather than decided.  Minimum we know: it tails the audit
log filtered to `(run, field)`.  Open: popup-vs-modal, diff vs
plain list, whether revert lives on the same widget.

### Operator fit-override propagation (`rundb.results_update`)

Scope deferred to follow-up DISCUSSION pass.  Open: per-field
set/get only, or batch + diff-preview before commit; how the audit
log records "operator override" vs "writer regeneration".

**Decision (2026-05-30, CLEAN_OFF C0.4)** — covers both the
"Show history" right-click and `results_update` above:

1. **Show history = non-modal popup, read-only in v1.**  A small
   frameless popup anchored under the clicked field, dismissed on
   focus-out — the operator glances at the `(run, field)` history while
   continuing to work; a modal would block the dashboard for a
   read-only glance.  **No one-click revert in v1**: an accidental
   click overwriting current data is a footgun, and the audit tail
   already shows the prior value to copy-paste.  A guarded revert
   (confirm dialog) can follow if operators ask.

2. **`results_update` = per-field audit rows, with a source tag.**
   One row per changed field, not a batched diff — the audit log's
   purpose is field-level provenance ("who set beam=OFF on run X,
   when"), which a single-row diff destroys.  Each row carries a
   `source` discriminator (`operator` vs `writer`) so a manual
   override is distinguishable from a writer regeneration; this is
   what lets the history popup colour operator edits differently from
   writer fills.

CD-cluster steps for the right-click history (backlog.6) and the
results reverse-merge (backlog.25) cite this.

### Quality-tagging cockpit

Shape deferred.  Two candidate shapes on the table:
(a) **side panel in Runlist** with thumbnails + 1-key quality
assignment ('g' / 'b' / 't'); (b) **modal launched from row** that
runs the QA view inline.  Decision once the runcard editor lands.
