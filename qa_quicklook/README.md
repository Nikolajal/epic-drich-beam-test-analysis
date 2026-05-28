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

| Tab | What it does | Status |
|---|---|---|
| **Run Manager** | Sidebar: run picker, **run info** card (V_bias · beam · polarity · mirror · temperature · radiators) with a quick **set-quality** dropdown, **active-runs panel** listing every writer currently `RUNNING` across all dashboard instances (per-run Stop), **data preview** of canonical files (`fine_calib.toml`, `lightdata.root`, `recodata.root`, `recotrackdata.root`, `pulser_calib_qa.root`) with "Show params" popups that read the embedded `Config/` tree via uproot. Right pane: per-writer **collapsible cards** (calibration · lightdata · recodata · recotrack) with inputs on the left and bool flags on the right; **per-run mutex** refuses concurrent writers on the same dataset. Top bar: `🔍 Inspect` (detached `root -l --web=off … new TBrowser`). Log dock: terminal palette, `\r`-progress-bar aware. Writers spawn detached (`subprocess.Popen + start_new_session`); a new dashboard instance **re-attaches** to surviving writers via `~/.cache/qa_quicklook/jobs/` locks + `~/.cache/qa_quicklook/logs/`. | **live** |
| **QA** | Three-shape selector (Per-run thumbnails · Cross-run trends · Quality cockpit) wired to a shared run picker. Each shape is a scaffold card; implementation fills in one at a time. | skeleton |
| **Database** *(renamed from Runlist)* | Browse / edit `run-lists/2025.database.toml` (forward-inheritance). Left: filterable list with **colour-coded quality chips** (right-aligned); search shows "N of M matched" + which field tripped each match. Right: **editable runcard**, fields tagged `✎ own` / `↪ inherited`, matched-by-search field tinted, **auto-pin** on edit so downstream runs don't silently shift. Buttons: **Detect runs** (scan `Data/`, append missing), **Save selection…** (multi-select → named entry in `2025.runlists.toml`). | **live** |
| **Settings** | Every `conf/*.toml` rendered as **bubble cards**: 2-3-column grid of params with the inline TOML comment as quick explanation, **units** to the right of each input, **dropdowns** for known-enum fields. Pair-to-pair tables get **chip grid** / **master-detail** views (Mapping). Top-of-page **setting-set picker** (default / 2023 / 2024 / 2025 / working) switches the conf/ symlink targets; **in-set direct edits** when the master points at a real set, promote-to-working only from defaults. Comment-preserving write-back, per-file disk watch, conflict banner. App Settings (the dashboard's `qa_quicklook.toml`) anchored at the bottom. Sections expanded by default, still collapsible. | **live** |
| **Advanced QA** *(optional)* | Plots and values pulled from bespoke ROOT macros. Hidden by default; toggle `[ui] show_advanced_qa = true` in App Settings. | placeholder |

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

Palette: coral (`#FF6B6B`) · green (`#0BDA51`) · sky (`#AFDBF5`) ·
warm grey (`#9B8E8E`) · ink (`#2A2929`).

## Layout

```
qa_quicklook/
  __init__.py
  app.py            MainWindow with top-bar tab shell
  settings.py       Settings tab
  runmanager.py     Run Manager tab
  runner.py         QProcess wrapper (line/progress streaming)
  writers.py        Writer catalog (FlagSpec / WriterSpec)
  joblock.py        Per-(writer, run) lock files for cross-instance state
  conf_layout.py    Setting-set symlink / defaults / working overlay
  toml_form.py      Recursive bubble-card form widget
  toml_model.py     Pure tomlkit helpers (walker, setter, ##-cutoff, …)
  theme.py          Palette + light/dark QSS + system-follow
  summary.py        Per-run reader for recodata.root (uproot)
  overview.py       Run-overview card widget (RunOverviewCard)
  chain.py          WriterChain — sequenced writer launches (not yet wired)
  download.py       rsync wrapper for the (future) Download button
  qa_quicklook.toml Dashboard-local config (rsync source, theme, toggles)
  requirements.txt  PySide6 + tomlkit + uproot
```
