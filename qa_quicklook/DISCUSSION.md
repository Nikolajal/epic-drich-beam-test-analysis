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
> * Atomic flock + tmp+rename for ``update()`` — **DONE.**  The TOML
>   backend acquires an exclusive ``flock(LOCK_EX)`` on a sidecar
>   ``<fPath>.lock`` (RAII ``LockGuard``) around the whole
>   load → merge → write → rename → audit cycle
>   (``src/analysis_results.cxx`` ``AnalysisResults::update``), so
>   concurrent ``lightdata_writer`` / ``recodata_writer`` finishes
>   serialise instead of clobbering each other.
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

### Persistence + git policy for ``standard_results.toml`` (open — 2026-06-02)

The TOML migration shipped the backend (commit 76363b9) but **never
completed step 3's relocation**: the file still lives at
``Data/standard_results.toml`` — i.e. inside the blanket
``Data`` ``.gitignore`` rule (``.gitignore:3``), so it is untracked.
The proposed gitted home (``run-lists/<year>.results.toml``, line ~216
above, the results twin of ``<year>.database.toml``) was never built.

**Should it be persistent + gitted?  Recommendation: yes to both.**

Findings that drive this:

- **Already persistent, but only implicitly.**  Retention prunes
  per-run ``Data/<run>/`` dirs; this file sits at the ``Data/`` *root*
  and has zero references in the retention code, so it survives
  prunes.  ``update()`` is **upsert** (load → patch → rewrite,
  ``analysis_results.h:116``), not regenerate-from-scratch, so a
  pipeline run on run B never drops run A's row.  Proof: pruned
  benchmark ``20251111-164951`` (raw data gone, see
  ``standard-verification-run`` memory) is still present in the file.
  The persistence is real but *incidental* — a future relocation
  could silently start dropping pruned-run rows.  Make the guarantee
  explicit when relocating.
- **"Safe to regenerate" (the file header) is a trap.**  Regeneration
  needs un-pruned raw data — which retention deletes — AND is
  regime-dependent: ``n_gamma`` comes out stale/zero in QA mode vs
  standard mode (see ``recodata-rings-need-hough-trigger`` memory).
  So the file is NOT freely reproducible.  A derived artifact that is
  expensive-or-impossible to reproduce, tiny (≈1.6 KB), text, and
  explicitly *git-mergeable* (line 212) is exactly what you commit.
  It is also the only durable computed-results record once raw data is
  pruned, and it feeds the publication robustness record.

**How (do NOT ``git add -f`` into ``Data/``):**

- *Minimal:* add ``!Data/standard_results.toml`` after the ``Data``
  rule.  One line; downside is a lone tracked file in an otherwise
  ignored tree (surprising to the next person).
- *Clean (preferred):* finish the migration's step 3 — relocate to
  ``run-lists/standard_results.toml`` (or per-year
  ``run-lists/<year>.results.toml`` to match the database split), so
  config ("what was set up") and results ("what came out") both live
  versioned in ``run-lists/``, where the Sheets pipeline already
  reads.  Costs a path update across the readers/writers:
  ``include/analysis_results.h``, ``src/recodata_writer.cxx``,
  ``qa_quicklook/{qa,multi_run_scatter,scatter_view,cross_run_trends,
  sheets_sync}.py``, and ``rundb.results_load``.

**Caveat regardless of approach:** gitting does not launder staleness.
The ``n_gamma`` regime-dependency means committed values may be stale;
git gives *provenance* (when/what changed), not *correctness*.  Treat
a committed value as an archival record, not a validated one.

**Churn note:** every pipeline run rewrites the file, so naive gitting
produces a diff per run.  Acceptable because the file is keyed by run
and git-mergeable by design (conflicts localise to a run's block); the
Sheets pipeline remains the *live* shared channel, git the *archival*
one.

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

## Vistar (PS + SPS) live monitor + per-run beam-condition log

**Goal.**  Bring accelerator-side observability into the same Sheets +
dashboard pipeline the Run Book already feeds.  Operators currently
flip between the dashboard, the Run Book sheet, and a separate browser
tab on the CERN Vistar page to know what the PS / SPS are doing.  Two
halves:

- **Live Vistar surface** — poll CERN's Vistar pages for the PS and
  SPS proton machines we trigger off (intensity, cycle structure,
  scheduled MD periods, beam-quality flags).  Render in a dashboard
  tab during shifts so the operator sees current beam state without
  leaving the QA cockpit.
- **Per-run beam log** — at run boundaries, snapshot the Vistar
  reading (PS + SPS) plus accelerator log lines covering the run's
  time window into a new ``beam`` worksheet keyed by run id, parallel
  to how Run Book rows feed the existing ``runs`` worksheet.  Goal is
  a durable record of "what the beam was doing for run X" without
  operators having to copy-paste manually — the post-shift "why did
  spill 17 look weird?" question answers itself.

**Ingestion pattern — mirror the Run Book.**  Reuse the
``scripts/import_runbook.py`` → ``run-lists/<year>.database.toml`` →
``sheets_sync.py`` shape:

1. A pulling helper (``scripts/import_vistar.py`` or a long-running
   ``qa_quicklook/vistar_watcher.py`` daemon, see Open questions)
   produces per-tick / per-run TOML or JSON records.
2. A new ``beam`` worksheet (or a column extension on ``runs``) is
   driven by ``sheets_sync.py``'s existing rendering machinery.
3. The dashboard surfaces the same data live (read from the local
   cache) without re-hitting Vistar.

### Open questions (original brainstorm — see "Open decisions" below for resolutions)

- **Access path.**  CERN Vistar has a public page (``https://op-webtools.web.cern.ch/vistar/vistars/``)
  and an internal API behind SSO.  Public-page screenshot scrape is
  fragile but auth-free; internal API needs a CERN service account
  or operator-side credentials proxied somehow.  Pilot probably
  starts with the public page; production may want the API.
- **Polling cadence.**  PS supercycle is ~30 s; SPS is ~30–60 s.
  Per-cycle polling is wasteful; per-run snapshot + 1 Hz live tile
  during a shift is enough.  Settle once the dashboard tab UX is
  drafted.
- **Run-anchor question.**  The "per-run beam log" needs a
  ``[run_start, run_end]`` window.  **Correction (2026-06-02):** an
  earlier draft of this note claimed ``rundb`` already carries
  ``acquisition_start`` / ``acquisition_end`` (UTC).  It does NOT —
  those fields exist nowhere in code or TOML.  What actually exists:
  ``run_id`` (``YYYYMMDD-HHMMSS``) IS the run *start* timestamp
  (almost certainly CERN-local, not UTC — must be confirmed and
  normalised to UTC), and there is NO run-end timestamp anywhere.
  The only duration proxies are ``n_spills`` (in
  ``standard_results.toml``) and the *next* run's start id.
  Establishing ``run_end`` is therefore a PREREQUISITE, not a given —
  see "Run-end source" under Open decisions below.  The window-join
  policy still holds: query the cached Vistar feed for
  ``[run_start, run_end]`` at run-close (the feed lives in a local
  file with ~minute resolution); do NOT re-poll Vistar synchronously
  at run close.
- **"Beam log" definition.**  Three candidates: (a) the accelerator
  log feed itself (LSA / eLogbook entries by accelerator ops), (b)
  Vistar-derived synthetic events (intensity drops, MD start/end),
  (c) operator-typed notes during the shift.  (b) + (c) are the
  realistic v1; (a) requires CERN-side data access agreements.
- **Sheets shape.**  New ``beam`` worksheet with columns ``run``,
  ``ps_intensity_mean``, ``sps_intensity_mean``, ``beam_quality``,
  ``md_periods_overlap``, ``operator_notes`` — vs extending the
  existing ``runs`` worksheet.  Lean toward a new worksheet so the
  Run Book schema stays focused on configuration ("what was set
  up") rather than conditions ("what actually happened").

### Open decisions (status as of 2026-06-02)

- **Access path — RESOLVED: public-page scrape.**  Pilot uses the
  public Vistar page (auth-free, fragile to layout changes).  The
  fetch layer sits behind a ``VistarSource`` interface so the
  CERN-internal API can swap in later without touching the cache,
  renderers, or dashboard tile.
- **Beam worksheet edits — RESOLVED: fully editable.**  Every beam
  column is reverse-mergeable (routes through
  ``detect_reverse_edits`` → ``rundb.update_run_field(source="sheet")``
  and is covered by ``ReverseMergeBrake``); the ``beam`` worksheet
  behaves exactly like the ``runs`` worksheet.  Accepted trade-off:
  a Sheet edit to a Vistar-*derived* column (e.g.
  ``ps_intensity_mean``) wins over the next push, so operators *can*
  clobber measured values.
- **Beam worksheet vs runs extension — RESOLVED: new ``beam``
  worksheet** (keeps config "what was set up" separate from
  conditions "what actually happened").
- **Beam-log definition — RESOLVED: v1 = (b) Vistar-derived synthetic
  events + (c) operator-typed notes.**  The accelerator log feed (a)
  is deferred — it needs CERN-side data-access agreements.
- **Run-end source — PARKED ("wait").**  Single blocker for Phase 3
  (the per-run window join).  Real choice when revisited: have the
  writer/seal step stamp ``acquisition_start``/``acquisition_end``
  (UTC) into ``standard_results.toml`` (durable; makes the corrected
  Run-anchor note true; touches the pipeline) **vs** derive
  ``run_end ≈ next_run.run_id`` (zero pipeline change; overstates
  duration across gaps / overnight / MD periods).  Phases 0–2 and the
  Phase 4 scaffolding do NOT need this; only the run-close snapshot
  does.

### Plan of action (phased)

Sequencing gate: do not start before the LICENSE / CMake-build-type
pre-campaign gaps (see ``publication-plan`` memory).  Build order is
highest-value-first; Phase 3 waits on the run-end decision.

- **Phase 0 — Prerequisites.**  Resolve run-end source; confirm the
  ``run_id`` timezone and normalise all timestamps to UTC at
  ingestion (the window-join is meaningless if Vistar UTC and a
  CERN-local ``run_id`` are compared directly).
- **Phase 1 — Fetch + local cache (the "feed").**  New
  ``qa_quicklook/vistar_feed.py``: ``fetch_vistar() -> VistarReading``
  (PS + SPS intensity, cycle structure, MD-state, quality flags)
  behind a ``VistarSource`` interface (``PublicPageSource`` first).
  Append each reading to a rolling
  ``~/.cache/qa_quicklook/vistar_feed.jsonl`` (UTC, ~1-min
  resolution).  Single source both halves read from; never re-hit
  Vistar at run-close.  Mirror ``download.py`` scheme-detection and
  ``remote_watcher.py`` stability/error discipline.
- **Phase 2 — Live dashboard tile.**  ``qa_quicklook/vistar_view.py``
  → ``VistarMonitorView(QWidget)`` and ``vistar_poller.py`` →
  ``VistarWatcherWorker(QObject)`` on its own ``QThread`` (copy the
  ``RemoteWatcherWorker`` / ``SheetsSyncWorker`` pattern).  The poller
  writes the cache; the tile reads it (~1 Hz during shifts).
- **Phase 3 — Per-run beam log → TOML.**  At run-close (hook the same
  seal signal the live monitor uses — ``remote_watcher.py``'s
  ``new_sealed_run``), query the cached feed for
  ``[run_start, run_end]``, aggregate, and write a record keyed by run
  id into a new ``run-lists/<year>.beam.toml`` (parallel to
  ``<year>.database.toml``).  Reuse ``rundb`` atomic-write + audit
  discipline (notes are operator-editable → need the audit trail).
- **Phase 4 — Sheets push.**  Extend ``sheets_sync.py``: read
  ``<year>.beam.toml`` in ``build_snapshot()``, add a ``beam``
  worksheet in ``render_worksheets()``, add beam analogues of
  ``COLUMN_GROUPS`` / ``FIELD_DISPLAY``.  v1 columns: ``run``,
  ``ps_intensity_mean``, ``sps_intensity_mean``, ``beam_quality``,
  ``md_periods_overlap``, ``operator_notes``.  Wire the worksheet into
  the reverse-merge path (fully editable, per decision above).
- **Phase 5 — Verify & document.**  Replay a recorded feed sample
  against a known window on the pinned verification run
  (``20251119-010426``) — no live CERN dependency in tests.  Update
  this section to mark shipped.

### Integration points (file map)

- **Tab registration** — ``qa_quicklook/app.py``: add ``"vistar"`` to
  ``_TAB_ORDER`` (~line 49), to ``_tab_widgets`` / ``_tab_titles``
  (~443), add ``_build_vistar_poller()`` (pattern at ~943,
  ``_build_remote_watcher``), wire reload in
  ``_on_dashboard_config_changed()``.
- **Poller template** — ``qa_quicklook/remote_watcher.py``
  (``RemoteWatcherWorker``: QTimer tick + stability quorum + error
  signals) and ``qa_quicklook/sheets_worker.py`` (``SheetsSyncWorker``).
- **Config** — new ``[vistar]`` block in
  ``qa_quicklook/qa_quicklook.toml`` (``enabled=false``,
  ``poll_interval_s``, ``source``, ``cache_path``); parse via the
  ``load_config()`` pattern in ``sheets_sync.py``.
- **Sheets machinery** — ``sheets_sync.py`` (``build_snapshot``,
  ``render_worksheets``, ``COLUMN_GROUPS``, ``FIELD_DISPLAY``,
  ``detect_reverse_edits``), ``_sheets_adapter.py`` (push/pull),
  ``rundb.py`` (``update_run_field`` for audited beam edits).

### Pilot target

2026 campaign (June +).  The live run is also the empirical
robustness data for the publication (see ``publication-plan``
memory) — the per-run beam log doubles as the durable
"what broke / what we hardened" log for the paper's robustness
section.  Do not start before the LICENSE / CMake-build-type
gap fixes; those are higher-priority pre-campaign polish.

---

## Lane failure ↔ DCR correlation (dead-lanes vs DCR)

**Branch:** ``dev_lane_fails_vs_dcr``.  **Status:** 🟢 IMPLEMENTED
(2026-06-02).  Writer scalars + dashboard view landed; C++ builds, all
322 dashboard tests green, smoke-tested on the pinned run.

**Goal.**  Correlate dark-count rate (DCR) with lane failure across
runs — does higher DCR track more lane deaths?

**Scope — narrowed 2026-06-02.**  Iterated per-lane → per-device (RDO)
→ **detector-wide (``all``) only**.  Final scope:

- **One plot: a multi-run trend** in the Multi-run tab — a single
  curve, x = mean DCR [kHz], y = detector-wide lane-failure rate, one
  point per run, connected and sorted by DCR.  Shows failure response
  as conditions (V_bias, temperature) sweep DCR across runs.
- **Single-run per-device scatter DROPPED.**  At detector level a run
  is a single point, so there is no per-run scatter — no new writer
  PDF / ``TGraph``, nothing in the per-run QA tab.
- **Per-device / per-sensor granularity DROPPED.**  Both scalars are
  published at ``sensor = "all"``; the earlier "bare device id
  (192..199) in the sensor slot" decision is now MOOT (no device
  keying needed).

### Definitions (resolved 2026-06-02)

- **Lane = FIFO** (``alcor_finedata.h:328`` ``get_lane()==get_fifo()``);
  ≤32 FIFOs/device.  Per-spill ``dead_mask[device]`` bitmask exists
  (``alcor_spilldata.h:88``; dead ⇔ start-of-spill marker with
  non-zero coarse time, ``parallel_streaming_framer.cxx``); lightdata
  already iterates spills and reads it (``lightdata_writer.cxx:820``).
- **Failure rate (detector-wide)** = **exposure-normalised** fraction
  of cherenkov lane-readout opportunities that died =
  (Σ_spills #dead cherenkov lanes) / (Σ_spills #participant cherenkov
  lanes), where a participant lane sent a start-of-spill marker and a
  dead one carried a non-zero coarse time.  Resolved the earlier
  ``n_lanes`` ambiguity this way (over a fixed lane×spill grid) so lanes
  coming online / dropping out across the fill are handled without a
  fixed lane count.  Implemented via a new symmetric
  ``AlcorSpilldata::get_dead_participants()`` (mirrors
  ``get_not_dead_participants``), tallied across the spill loop in
  ``lightdata_writer.cxx``.  Caveat: a lane that vanishes entirely (no
  start-of-spill marker at all) is invisible to both masks, so silent
  total absence is not counted as a failure.
- **DCR** = mean of the ``h_dcr_per_channel`` TProfile over channels,
  in **kHz** (run-accumulated over FirstFrames noise windows,
  ``lightdata_writer.cxx:476/772/1137``).  **Decided to publish this
  as a new kHz scalar** rather than reuse the dashboard-derived
  per-event rate (``n_dcr_hits / n_events``).  (Per-spill DCR / a
  "reference-measurement" spill was also rejected — neither exists;
  ``dcr_scan`` is only a run-DB/Sheets metadata label,
  ``sheets_sync.py:363``.)

### Published-scalar changes — rate-ification (decided 2026-06-02)

Framing: the test is **readout resilience to data rate**, sensor-
agnostic — so counts become rates and the sensor key collapses to
``"all"``.  The QA pipeline currently publishes 15 run-level scalars;
agreed changes (lightdata publish site ``lightdata_writer.cxx:3539``):

| Current scalar | What it is | Decision |
| --- | --- | --- |
| ``lightdata.n_dcr_hits`` | DCR-hitmap entry *count* | **DROP** → ``lightdata.dcr_mean_khz`` (mean of the already-kHz-scaled ``h_dcr_per_channel`` TProfile).  The data-rate x-axis. |
| ``lightdata.n_afterpulse_hits`` | subtracted afterpulse-hitmap integral (can go negative) | **→ ``lightdata.afterpulse_prob``** (DCR-subtracted afterpulse probability, %).  Implemented as the per-channel-averaged mean of the ``h_afterpulse_per_channel`` profile (each bin = 100·(P_near − P_far)); resolves the earlier denominator TBD. |
| ``lightdata.n_events`` | trigger-matrix entries ≈ frames processed (**NOT** physics events) | **KEEP + RENAME** → ``lightdata.n_selected_frames`` (frames that passed selection).  A throughput measure in its own right. |
| ``recotrack.n_frames`` | recodata-tree entry count (capped at ``--max-frames``) | **DROP** (no dashboard consumer). |
| *(new)* ``lightdata.lane_failure_rate`` | — | the resilience **y-axis** (detector-wide dead-lane fraction). |

Untouched: ``recodata.{frame_size,n_spills,nominal_centre_x/y_mm}``,
``recotrack.{n_matched_tracks,n_spills}``,
``calibration.{n_published_tdcs,spills_seen,total_hits_read}``,
``full.{n_gamma,sigma}``.

Sensor key (as implemented): the **new** resilience scalars
(``dcr_mean_khz``, ``lane_failure_rate``) are published under
``sensor = "all"`` (sensor-agnostic).  The **renamed/converted** ones
(``n_selected_frames``, ``afterpulse_prob``) keep the cherenkov
``sensor`` key they already used, to avoid disturbing the sensor
slicing dimension and its consumers — a smaller blast radius than
forcing everything to ``"all"``.

**Breakage to fix:** dropping ``n_dcr_hits`` + renaming ``n_events``
breaks the dashboard's derived ``dcr_rate`` metric
(``cross_run_trends.py:178`` ``= n_dcr_hits / n_events``).  Repoint it
to read ``dcr_mean_khz`` directly — now a published rate, no derivation.

### Multi-run view — implemented by extending the scatter view

Chose to **extend** ``scatter_view`` / ``multi_run_scatter`` rather than
add a dedicated tab (detector-wide ⇒ single series, so the per-device
grouping problem never arose).  Landed:

- ``multi_run_scatter.build_scatter`` gained an optional
  ``x_metric`` — when set, the X axis is read from
  ``standard_results.toml`` via the same ``extract_series`` extractor as
  Y, instead of a beam-info field.  ``x_field`` is now optional;
  positional callers are unaffected.
- ``scatter_view`` X combo now lists both beam-info fields **and** the
  QA metrics (tagged "(QA)"); a new **"connect"** checkbox draws a line
  through the points sorted by X (and drops the cluster jitter) — that
  is the resilience curve.  Pick Y = Lane-failure rate, X = DCR (QA),
  tick connect.
- ``lane_failure_rate`` registered as a ``MetricSpec`` and ``dcr_rate``
  repointed to the published ``dcr_mean_khz`` (no more derive).

### What landed (phases, all done)

- **Phase 1 — Writer.**  ``src/lightdata_writer.cxx``: per-spill
  dead/participant tally across the spill loop (via new
  ``AlcorSpilldata::get_dead_participants()`` in
  ``src/alcor_spilldata.cxx`` + ``include/alcor_spilldata.h``); publish
  ``dcr_mean_khz`` + ``lane_failure_rate`` at ``"all"``; rename
  ``n_events``→``n_selected_frames``, convert afterpulse →
  ``afterpulse_prob``, drop ``n_dcr_hits``.  ``recotrackdata_writer.cxx``:
  dropped ``recotrack.n_frames``.  No PDF (single-run plot dropped).
- **Phase 2 — Dashboard.**  ``cross_run_trends.py`` (metrics),
  ``multi_run_scatter.py`` (``x_metric``), ``scatter_view.py`` (X-metric
  options + connect curve).  Tests updated in
  ``tests/test_cross_run_trends.py``.
- **Phase 3 — Verify.**  C++ builds; 322 dashboard tests pass;
  smoke-tested on pinned run ``20251119-010426`` —
  ``lane_failure_rate``=0.269, ``dcr_mean_khz``=5.35 kHz,
  ``afterpulse_prob``=3.19 %, ``n_selected_frames`` preserved.  A proper
  V_bias-sweep run set (e.g. the ``vbias_min28`` runlist) is the real
  exercise of the curve.

### Known follow-ups

- **Stale dropped keys.**  ``AnalysisResults::update`` is upsert, so on
  an already-populated ``standard_results.toml`` the dropped
  ``n_dcr_hits`` / ``n_events`` (and ``recotrack.n_frames``) keys
  **persist** until the file is regenerated from empty.  Harmless
  (nothing reads them) but messy — entangled with the
  ``standard_results.toml`` persistence/regeneration decision above.
- **Persistence dependency.**  Unchanged: the multi-run curve is only as
  durable as the per-run scalars in the ungitted, prune-exposed
  ``Data/standard_results.toml``.

### Suggested commit split (on ``dev_lane_fails_vs_dcr``)

1. **Writer** — ``lightdata_writer.cxx`` + ``recotrackdata_writer.cxx``
   + ``alcor_spilldata.{h,cxx}``: the rate-ified scalars +
   ``get_dead_participants`` helper.  Self-contained.
2. **Dashboard** — ``cross_run_trends.py`` + ``multi_run_scatter.py`` +
   ``scatter_view.py`` + ``tests/test_cross_run_trends.py``: metrics
   rewire + measured-X connect curve.

---

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
