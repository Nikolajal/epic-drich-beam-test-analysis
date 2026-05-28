# qa_quicklook — Quick Local QA dashboard

Operator-time cockpit for ePIC dRICH beam-test analysis.  Sits on
the analysis machine; opens fast; no service to spin up.

## Run

```
./scripts/qa_quicklook              # bootstrap (.venv on first run) + launch GUI
./scripts/qa_quicklook --setup      # bootstrap only
./scripts/qa_quicklook --test       # run the unit-test suite
./scripts/qa_quicklook --reinstall  # rebuild the .venv from scratch
```

The first invocation creates `.venv/`, installs
`qa_quicklook/requirements.txt`, runs the smoke tests, then opens the
panel.  Subsequent invocations skip straight to the GUI.

## Tabs

The top bar splits into three high-level tabs (**Run Info**, **QA**,
**Settings**); **Run Info** in turn nests the operator's three live
panels — **Run Manager**, **Database**, **Runlists**.

| Tab | What it does |
|---|---|
| **Run Manager** | **Sidebar:** run picker (newest-first), **run-info card** (Beam · Detector · Radiators) with the **edit-in-place** flow — `✎ Edit` toggles each cell into a pre-filled input, `✓ Save` pops a **cascade-vs-pin** dialog so the operator picks whether downstream merged views shift or stay; for runs not in the DB, the same card shows `✚ Add to database` (chronologically-inserted, with an upstream-insert warning when applicable). Quick **set-quality** dropdown; **data preview** (`fine_calib.toml`, `lightdata.root`, `recodata.root`, `recotrackdata.root`, `pulser_calib_qa.root`) with "Show params" popups via uproot. **Active-runs panel** showing every writer currently `RUNNING` across all dashboard instances (per-run Stop). **Right pane:** per-writer collapsible cards (calibration · lightdata · recodata · recotrack) with a 2-column-of-pairs form filling the horizontal space; calibration card exposes `--anchor-device / --anchor-chip / --anchor-eo-channel / --pulser-frequency-hz` as per-launch overrides (sentinel = use TOML default). Per-run mutex refuses concurrent writers on the same dataset. **Top bar:** `🔍 Inspect` → spawns the standalone `qa_tbrowser` binary (`build/bin/qa_tbrowser`) with all the run's `.root` files pre-attached; `📥 Download` → rsync wrapper that pops a first-use address dialog (with **Test SSH key auth** probe) and persists the host/path back into `qa_quicklook.toml`. **Log dock:** terminal palette, `\r`-progress-bar aware. Writers spawn detached (`subprocess.Popen + start_new_session`); a new dashboard instance re-attaches to surviving writers via `~/.cache/qa_quicklook/jobs/` locks + `~/.cache/qa_quicklook/logs/`. |
| **QA** | Per-pipeline-step sub-tabs (Lightdata · Recodata · Recotrack · Pulser calib · Macros), each rendering the writer's `qa/<step>/*.pdf` artefacts inline (`QPdfView`) **and** an uproot-driven thumbnail grid that mirrors the writer's `TDirectory` tree — collapsible nested cards, default collapsed. Click a thumbnail → interactive matplotlib modal with `log Y / log X / auto-range / Fit ▾ / clear fit / Open in ROOT ↗` (the last spawns `qa_tcanvas` which owns its own `TApplication` so the canvas survives — no Terminal involved). **Monitor mode** poll-watches the run dir's mtimes and rebuilds the page on change (calm green pulse when on; red pulse on poll error; smooth `QVariantAnimation` between colours). Dark-mode TH2 colormap uses a custom two-stop coral → green gradient masking zero bins to transparent for legibility against the dark surround. |
| **Database** | Browse / edit `run-lists/<year>.database.toml` (forward-inheritance via `rundb.load_database`). Picker chooses among multiple campaign DBs. Filterable list with colour-coded quality chips (right-aligned), most-recent run at the top; search shows "N of M matched" + which field tripped each match. Right pane: editable runcard, fields tagged `✎ own` / `↪ inherited`, matched-by-search field tinted, **auto-pin** on edit so downstream merged views stay stable. Buttons: **Detect runs** (scan `Data/`, chronologically-insert missing ids), **Save selection…** (multi-select → named entry in `<year>.runlists.toml`). Every edit lands in `<db>.audit.toml` with `source="dashboard"` (or `"legacy"` for one-shot migrated entries). |
| **Runlists** | Browse and edit named runlists (`<year>.runlists.toml`) that group runs for later analysis steps. Add / remove runs, +/− columns, atomic write-back. |
| **Settings** | Every `conf/*.toml` rendered as bubble cards: 2–3-column grid of params with the inline TOML comment as quick explanation, units to the right of each input, dropdowns for known-enum fields, table-arrays full-row-spanning. Pair-to-pair tables get chip-grid / master-detail views (Mapping). Top-of-page **setting-set picker** (default / 2023 / 2024 / 2025 / working) switches the `conf/` symlink targets; in-set direct edits when the master points at a real set, promote-to-working only from defaults. Comment-preserving write-back, per-file disk watch, conflict banner. Includes a **Calibration** section that surfaces `conf/calib/calibration_conf.toml` (lives outside the standard setting-set layout — edited in place). Readout config has a **chip-overlap validation** banner that flags any (device, chip) pair claimed by more than one role (cherenkov / timing / tracking). App Settings (the dashboard's `qa_quicklook.toml`) anchored at the bottom — exposes `[ui].theme` (UI palette) and `[ui].plots_theme` (separate QA plot theme; `follow` / `light` / `dark`). |
| **Advanced QA** *(hidden by default)* | Plots and values pulled from bespoke ROOT macros. Toggle `[ui] show_advanced_qa = true` in App Settings. |

Longer-term items (cross-shifter sync via Google Sheets, embedded ROOT canvases, …) live in [`DISCUSSION.md`](DISCUSSION.md).

## Setting-set system

`conf/` is structured as:

```
conf/
  defaults/        ← pristine baseline; committed, never overwritten by the GUI
  sets/2023/       ← named bundles — only files that differ from default
  sets/2024/
  sets/2025/
  working/         ← per-operator scratch; .gitignored
  framer_conf.toml ← MASTER symlink — what the C++ writers read
  …
  calib/calibration_conf.toml  ← edited in place (not under set management)
```

The master symlinks point at `defaults/` by default.  Switching a set
in the dashboard repoints the masters at that set's files (falling
back to `defaults/` where the set doesn't override).  Editing in the
GUI promotes to `working/` and repoints the master there, so the
named sets stay pristine.

## Theme

`[ui].theme` in `qa_quicklook/qa_quicklook.toml`:

- `system` → follow the OS appearance (default, reacts to changes live)
- `light` / `dark` → force a mode

`[ui].plots_theme` decouples the matplotlib QA plots from the UI
palette — useful when projector / printer / contrast preferences
differ.  `follow` (default) tracks the UI; `light` / `dark` pin.

Palette: coral (`#FF6B6B`) · green (`#0BDA51`) · sky (`#AFDBF5`) ·
warm grey (`#9B8E8E`) · ink (`#2A2929`).

## Provenance

Every dashboard-driven edit to a run-database field, and every C++
writer's publish into `<repo>/standard_results.{root,toml}`, lands
in a sibling `<file>.audit.toml` with the timestamp, source
(`dashboard` / `lightdata` / `recodata` / `recotrack` / `calibration` /
`legacy`), run id, field name, old value, and new value.
`scripts/backfill_audit_legacy.py` is the one-shot migration that
tags pre-history records with `source="legacy"`.

## Layout

```
qa_quicklook/
  __init__.py
  app.py             MainWindow with top-bar tab shell + plot-theme push
  settings.py        Settings tab + per-file warning surface
  runmanager.py      Run Manager tab: cards, run-info edit flow, SSH probe
  runlist.py         Database tab (filterable runcard editor)
  runlists.py        Runlists tab (named groupings of runs)
  qa.py              QA tab + interactive matplotlib modal + qa_tcanvas wrapper
  datainspect.py     Canonical-file preview + embedded Config/ reader
  thumbs.py          uproot histogram → matplotlib thumbnail
  readout_validate.py  Chip-overlap detector for readout_config.toml
  sanity.py          V_bias × T expected-band lookup
  rundb.py           Run-database parser w/ forward-inheritance + audit log
  writers.py         Writer catalog (FlagSpec / WriterSpec)
  joblock.py         Per-(writer, run) lock files for cross-instance state
  runner.py          QProcess wrapper (line / progress streaming)
  dbworker.py        Background TOML writer pool (keeps GUI responsive)
  download.py        rsync wrapper + SSH key-auth probe + address persistence
  conf_layout.py     Setting-set symlink / defaults / working overlay
  toml_form.py       Recursive bubble-card form widget
  toml_model.py      Pure tomlkit helpers (walker, setter, ##-cutoff, …)
  theme.py           Palette + light/dark QSS + system-follow
  qa_quicklook.toml  Dashboard-local config (rsync, theme, plots_theme, vbias bands)
  requirements.txt   PySide6 + tomlkit + uproot + matplotlib
```
