# CLAUDE_GUIDELINES.md

Durable reference for working on **beam-test-analysis** (ePIC dRICH beam test).
Captures the conventions, data contracts, and standing decisions agreed with the
maintainer so a fresh conversation can continue without re-deriving them.

> This file is a **convention ledger**, not API docs. When a convention changes,
> edit it here in the same change. Code structure / past fixes / git history do
> **not** belong here — git carries those.

---

## 0. Repository shape

- **C++ ROOT writers** (`src/`, `include/`) produce QA artefacts (`.root` + `.pdf`)
  per run; built with CMake. Sibling lib **mist** lives at
  `/Users/nrubini/Analysis/mist/` — *fix mist there, never in `build/_deps/mist-src/`*.
- **`qa_quicklook/`** — PySide6 + matplotlib dashboard (Python) that reads the
  writers' output and the run databases. Has its own `pytest` suite under `tests/`.
- **`macros/`** — standalone ROOT analysis macros, *separate tools* not the
  core pipeline. Three subdirs: **`examples/`** (shipped reference analyses),
  **`utilities/`** (shipped utility macros + the writer build targets wired
  into CMake), **`working/`** (per-operator **scratch**, git-ignored except
  its README — mirrors `conf/working/`). Personal/throwaway macros go in
  `macros/working/`; never commit scratch into `examples/`.
- **`conf/`** — TOML config (see §4). **`run-lists/`** — run databases / runlists.
- **`Data/`** (a.k.a. the *data repository*) — per-run output dirs + the
  cross-run results file. **Not** the git repo root.

---

## 1. Standing workflow rules

**End-to-end flow** (the §1 / §1b / §1c gates, tied together — how a change goes
from idea to released):

1. **Pick the branch** (§1c): miscellanea / small fix / docs → `dev_patch`;
   a coherent feature with its own arc → new `dev_<feature>` off `dev`. Unsure → ask.
2. **Implement + verify**: build the writers after C++ edits, run `pytest tests/`
   after Python edits; keep the suite green.
3. **Commit** (§1 gate): propose the message **+ SemVer bump**, await go-ahead,
   `git commit` on the leaf branch, **then stop**. Plain message, no attribution footer.
4. **Merge to `dev`** when the feature is complete: draft the PR body → **get it
   approved** (§1b) → `gh pr create --base dev` → `gh pr merge --merge --delete-branch`.
5. **Release** (separate maintainer gate, §1b): only when the maintainer asks to
   cut one — draft `RELEASE_NOTES`, **get them approved**, PR `dev → main`, let the
   clang-format check land, merge, annotated tag, `gh release create`.

The detailed rules:

- **Commits are Claude's to run with go-ahead; releases are maintainer-gated.**
  Stage the change, propose the commit message **and the SemVer version bump**
  (so the bump is decided at commit time, not later), and **wait for the
  maintainer's go-ahead before committing**. Once approved, `git commit` on
  the appropriate leaf branch (`dev_<feature>` or `dev_patch` — see §1c) —
  **and stop there.** Do not push, tag, open a PR, or cut a GitHub Release on
  your own. Commits accumulate; the maintainer decides when an accumulated
  set is a good cut point and asks for the release flow (§1b) explicitly.
  (Supersedes the former "never commit" rule *and* the earlier "Claude runs
  the full chain" reading.)
  - **No attribution footer.** Commit messages and PR bodies carry **no**
    `Co-Authored-By: Claude …` trailer and **no** "Generated with Claude Code" line.
    Plain messages only.
- **GUI launches the pipeline with `--clean`.** Both dashboard launch sites pass it.
- **Background addenda:** when the maintainer fires off side-requests mid-task,
  *finish the current main task first*, then pick up the queued items. New
  instructions go to the background; they don't pre-empt the active task.
- **Build & verify** the writer after C++ edits; run the relevant `pytest` after
  Python edits. Keep the suite green (326 tests as of v2.1.0).
- **Standard / benchmark run:** `20251111-164951` (raw data was pruned — needs
  re-download). Verify on pinned `20251119-010426` meanwhile. Quick test pipeline:
  `qa_pipeline … --clean --max-spill 5`.
- **Threads:** the pipeline passes `--threads 4` to the writers. Auto-detected 16
  is *slower and fatter* — don't bump it. (lightdata streaming/Hough is serial on
  the main thread, so framer parallelism caps ~1.3× regardless.)

---

## 1a. The "Clean-up sweep" (a named routine)

When the maintainer says **"clean-up sweep"**, **"the sweep"**, **"full sweep"**,
or **"the usual sweep"**, run this whole routine — it's the multi-round pass we've
done several times. Scale the fan-out to the size of the recent change (a few
parallel `Explore` agents for a big batch; inline for a small one).

1. **Bug round** — re-read the code touched since the last sweep for *correctness*
   bugs (logic, lifetimes, races, off-by-one, error-swallowing). Don't just lint;
   look for real defects. Fan out over subsystems when the surface is wide.
2. **Stale code / docs round** — dead code, orphaned helpers, stale comments
   (and **no dated comments** — strip any you add or find), out-of-date
   DISCUSSION/README/CLAUDE_GUIDELINES, broken cross-links, config drift
   (defaults vs `conf/QA` vs header). **Read every comment in the units in
   scope, not just the lines you touched** — a wrong or misleading comment is a
   defect, not cosmetic.
3. **Config-driven & canonical-pattern round** — *working-but-hardcoded is NOT
   stale, so it needs its own lens or it sails through* (three earlier sweeps
   missed exactly this). Hunt:
   - **(a) Magic numbers / hardcoded values a config already owns** — channel→
     sensor ranges (`<=1024`), thresholds, geometry, window sizes. Push them to
     the TOML and read via the typed reader (`readout_config_reader`,
     `…_conf_reader`). **Magic numbers should be minimal-to-none.**
   - **(b) Deviations from the canonical data flow** — every analysis posts
     results to `AnalysisResults` / `standard_results.toml`; channel→sensor
     resolves via `sensor_for(device)`, never a range; tunables are config-driven.
4. **BACKLOG cross-check** — close rows shipped since last time, drop duplicates,
   file genuinely-new findings as READY rows. **Compact the DROPPED ledger**:
   shipped/completed work is removed (git is the record); keep only terse
   one-line "won't-fix + re-add-if-X" entries so they don't reappear as
   "discoveries" next sweep.
5. **Temp / artefact purge** — delete scratch + ephemeral files (roadmaps like
   the retired `CLEAN_OFF.md`, one-off logs) **only after** folding any still-live
   content into a durable home (BACKLOG / DISCUSSION / this file / memory). Fix
   any dangling pointers to the deleted file.
6. **Verify green** — build the writers + run `pytest tests/` before declaring
   done. Never report "swept" on a red tree.
7. **Formatter pass** — last, after all substantive changes have settled. Run
   `clang-format -i --style=file` across `include/`, `src/`, `macros/` (skip
   `build/`, `_deps/`). The CI workflow (`.github/workflows/clang-format.yml`)
   is the ground truth; local clang-format 22.1.6 matches Ubuntu's apt
   clang-format-18 on this codebase under our `.clang-format`. Stage the
   reformat as part of the sweep commit, or — if the sweep batched multiple
   logical commits — as its own trailing `style: apply clang-format` commit
   on the same `dev_patch` leaf. Running this last (after rounds 1–6) is
   important because earlier rounds can introduce drift; formatting first
   would force a re-pass.
8. **Report** — what changed, what was closed/dropped, what was deferred and why.

Standing constraints during a sweep: obey §2 (never prune non-`.qa_managed` runs,
never delete calibration/raw dirs), and **don't commit mid-sweep** — a sweep stages
changes; the commit comes at the end through the §1 gate (propose message + SemVer
bump, await go-ahead, commit on the appropriate `dev_*` leaf (§1c), stop — the
release stays maintainer-gated per §1b).

---

## 1b. Versioning & releases (SemVer 2.0.0)

From **v2.0.0** onward the repo follows [Semantic Versioning 2.0.0](https://semver.org).
Tags are annotated and named `vMAJOR.MINOR.PATCH` (prior tags were lightweight; the
scheme starts clean at v2.0.0). Bump rules, given a public API = the writer CLI
contract, the `conf/` TOML schema, the `AnalysisResults`/`standard_results.toml`
keys, the trigger/hit-mask scheme, and the public C++ headers:

- **MAJOR** — backward-incompatible change to any of the above (e.g. the v1→v2
  trigger-scheme + config-schema + results-backend rewrite).
- **MINOR** — backward-compatible new capability (a new QA plot, a new `conf` key
  that defaults to prior behaviour, a new dashboard tab, a new macro).
- **PATCH** — backward-compatible bug fix. The *"minor fixes are expected and
  unavoidable"* follow-ups after a release land here → **v2.0.1**, **v2.0.2**, ….
- **Pre-release** — `-alpha.N` / `-beta.N` / `-rc.N` suffixes when staging a release.

Release mechanics — **only triggered when the maintainer explicitly asks to cut a release** (§1 is the per-commit gate; this is the per-release gate):
- Draft `docs/RELEASE_NOTES_vX.Y.Z.md` covering the commits accumulated on `dev`
  since the previous tag.
- **Get the notes approved before publishing — never push notes blindly.**
  Release notes *and* PR descriptions are outward-facing, maintainer-accountable
  content. Draft them, **show the text to the maintainer, and publish only after
  explicit approval** (then run `gh release create` / `gh pr create`). Same
  publication-accountability principle as the no-attribution-footer rule (§1).
  The mechanical git ops (merge, tag, branch align) can proceed once the release
  is authorized; it's the human-readable *text* that needs sign-off first.
- Push `dev` → open the PR `dev → main` → **let the `clang-format` PR check
  finish before merging.** It auto-fixes formatting drift by pushing a
  `style: apply clang-format` commit back to `dev`; merging within ~30 s of
  opening the PR races past that fix (v2.0.0 hit exactly this — PR opened and
  merged in a 24 s window, the auto-fix never landed, dev shipped with
  unformatted code that had to be reformatted by hand in a later patch).
- Merge as a **merge commit** (not squash) so history is preserved under the tag.
- Annotated tag on the merge commit:
  `git tag -a vX.Y.Z -F docs/RELEASE_NOTES_vX.Y.Z.md`. The tag does **not** travel
  with the PR — push it separately: `git push origin vX.Y.Z`.
- **A pushed tag is NOT a GitHub Release.** The Releases UI / "Latest" badge needs
  an explicit release created from the tag:
  `gh release create vX.Y.Z -F docs/RELEASE_NOTES_vX.Y.Z.md --title vX.Y.Z --verify-tag`
  (`--verify-tag` reuses the existing tag). Skipping this leaves the prior version
  showing as "Latest" even though the new tag exists.

---

## 1c. Branch model

Four-tier layout — work fans out on leaf branches, lands on `dev`, and only
release merges touch `main`.

- **`main`** — release-only. The only commits that land here are the merge
  commits that carry an annotated release tag (§1b). No direct work, no
  feature branches based off it.
- **`dev`** — integration target. `dev_*` branches merge in here as they're
  ready; the maintainer cuts releases from `dev`'s tip (`dev` → `main` merge,
  §1b). **No direct commits on `dev`** — even one-line tweaks go through a
  leaf branch so `dev`'s history is composed of clean merge commits.
- **`dev_<feature>`** — one branch per active workstream (e.g.
  `dev_streaming_v2`, `dev_recodata_cb`, `dev_dashboard_quality_tags`).
  Branched off `dev`. Lives as long as the feature does; may span many
  commits. Merges to `dev` as a merge commit (not squash) when the feature is
  complete and verified, then the branch is deleted.
- **`dev_patch`** — single shared branch for miscellanea: small fixes, doc
  edits, dependency bumps, single-file tweaks that don't deserve their own
  feature branch. Branched off `dev`, re-merged into `dev` frequently, then
  realigned (`git reset --hard dev`) so it tracks `dev`'s new tip. A rolling
  staging area, not a long-lived divergence.

**Merge flow** — feature and release merges go through **GitHub PRs**; get the
PR body approved before opening it (§1b). Switch to the target branch before
merging so you're never on the branch being deleted.
1. `dev_<feature>` → `dev` — `gh pr create --base dev --head dev_<feature>`,
   then `gh pr merge --merge --delete-branch`. Merge commit (preserves feature
   history under `dev`'s log); `--delete-branch` removes the feature branch
   **local + remote** in one step. *Always delete a feature branch as part of
   merging it* — don't leave merged branches lingering.
2. `dev_patch` → `dev` — merge commit, then `dev_patch` realigns to the new
   `dev` tip (`git reset --hard dev`; push with `--force-with-lease`).
3. `dev` → `main` — **only on release** (§1b). PR → merge commit (keep `dev`,
   it's the integration branch, not deleted). The tag lands on this merge
   commit; the GitHub Release is the separate `gh release create` step.

Never delete **unmerged** branches or **collaborators'** branches
(e.g. `origin/dev_tracking`, `origin/dev_valenti`) during cleanup.

The per-commit §1 gate (propose message + SemVer bump → await go-ahead → commit
on the leaf branch → stop) applies to `dev_<feature>` and `dev_patch` alike.
Picking the branch is the first decision: if it's miscellanea / a small fix /
docs → `dev_patch`; if it's a coherent feature with its own arc → branch a new
`dev_<feature>` off `dev`. When unsure, ask.

---

## 2. Data-loss safety (retention) — HARD RULES

The dashboard's startup retention sweep **only ever touches runs the QA system
itself downloaded**. Concretely:

- A run dir with **no `.qa_managed` marker → NEVER touched.** The escape hatch
  that allowed age-based pruning of un-managed runs (`enforce_qa_managed`) was
  **removed entirely** — do not reintroduce it.
- `.qa_persistent` marker → pinned, never pruned. **`touch Data/<run>/.qa_persistent`
  before launching the dashboard** for any run you want to keep.
- The markers themselves are in the sweep's *keep* set (deleting them would
  silently un-manage a run).
- **Never delete**: calibration files, raw device dirs (`rdo-*`, `kc705-*`),
  benchmark/standard runs.
- `qa_pipeline --clean` is allowlist-protected: it removes `qa/`, writer roots,
  and `h_*.pdf` — never device dirs or calibration.
- A run dir is "real" (non-phantom) iff it is **non-empty**; prune can leave
  phantom empty folders that fool the dashboard → filtered by
  `is_populated_run_dir` / `list_populated_runs`.

---

## 3. Cross-run results contract (`standard_results.toml`)

- **Location (LOCKED):** `Data/standard_results.toml` (i.e.
  `<data_repository>/standard_results.toml`). Any comment saying `<repo>/` is
  wrong — fix it, don't follow it. The dashboard reads `Data/`.
- **Key tuple order (LOCKED):** `(run, sensor, quantity)` — **run first.**
  Serialized as `[results."<run>"."<sensor>"]`, `quantity = { value, error }`.
  Core writers publish sensor = `"all"`; per-sensor tools may publish `"1350"` /
  `"1375"`.
- The file **accumulates / merges** across tools and runs. Stale entries from old
  conventions can linger (e.g. inverted `[results."1350"."<run>"]`) — the fix for
  those is to **purge/regenerate the file**, not to "fix" a writer that is already
  run-first correct.
- **Published quantity keys** (what the dashboard / scatter can plot): `dcr.*`,
  `full.{n_gamma,sigma,mu,gs_frac,chi2_ndf}`, `in_gap.*`, `ex_gap.*`,
  `lightdata.{n_events,n_dcr_hits,n_afterpulse_hits}`,
  `recodata.{n_spills,frame_size,nominal_centre_x_mm,nominal_centre_y_mm}`,
  `recotrack.{n_frames,n_spills,n_matched_tracks}`,
  `calibration.{total_hits_read,spills_seen,n_published_tdcs}`.
- A metric in `cross_run_trends.DEFAULT_METRICS` is only live if **some writer
  publishes its `quantity` key.** (The half-built `streaming.n_fires` metric was
  *dropped* — no writer published it. Don't re-add a metric without a publisher.)

---

## 4. Config conventions (`conf/`)

- **Symlinks:** `conf/<name>.toml` → `conf/working/<name>.toml`. **Edit the
  resolved `conf/working/...` target**, not the symlink.
- **QA overlay (`conf/QA/`)** is a **full-file swap**, not a merge — the whole
  file is replaced when QA mode is active. (`readout_config.toml` currently has
  no QA overlay.)
- **`readout_config.toml`:**
  - `chips = "*"` → wildcard = **all 8 chips**. An explicit subset must be a TOML
    **array** `chips = [2, 4]` — the string `"2,4"` is *not* parsed (drops the
    device's chips). Parser is being hardened to also accept comma-strings.
  - Per-device `sensor` field (e.g. `sensor = "1350"`) is supported, with a
    `sensor_for(device)` fallback. Sensor split: **`1350` for rdos 192–195, the
    other sensor ("Quetta" per the maintainer; file currently says `1375`) for the
    rest.** Flag the 1375/"Quetta" naming mismatch rather than silently picking one.
- **Calibration / `--QA` cost levers:** keep `per_spill_calibration_update = false`
  (saves ~17 min/run). The 19 s `--QA` on-vs-off delta lives in per-hit
  `get_phase()`, not `generate_calibration`.

---

## 5. QA artefact / dashboard conventions

- **PDF naming contract:** overview rows + per-trigger fan-out match by **stripped
  basename**; the `NN_` numeric prefix orders within a stage (e.g.
  `06_dcr_per_channel`, per-trigger `07+`). Keep prefixes collision-free.
- **ROOT TDirectory tree:** the Full-Plots view **auto-discovers** by walking the
  tree. Only the *General overview rows* and the *streaming-score picker* are
  hardcoded by name — renaming those hists breaks those two consumers.
- **General overview = 4 thematic rows:** Data-taking health / Sensor health /
  Cherenkov physics / Timing-calibration.
- **Helper PDFs** (recodata `radial_fit` / `sigma_vs_n_fit`) must land under
  `qa/recodata/` so the stage-scanner sees them — not the run root.
- **Cross-tab run sync:** selecting a run in one tab (Run Manager ↔ QA) syncs the
  others (guarded against signal loops).
- **Campaign-file defaults auto-pick the newest** `YYYY.database.toml` /
  `YYYY.runlists.toml` (`newest_campaign_file`). Runlists carry a year tag, or
  `"mixed"` for cross-campaign lists.
- **`update_run_field`**: setting a field to `None` **deletes the key** (do not
  write a `None` TOML item — that throws `ConvertError`). Forward-inheritance +
  auto-pin + audit-log are part of this path.

### Plot-styling conventions
- Uniform overview tile geometry; bigger inspect-dialog + ROOT canvas sizes.
- `trigger_matrix` axis labels rotated (`LabelsOption("v")`, ROOT 6.40 has **no**
  `SetLabelAngle`) with breathing margins.
- Per-trigger consecutive-Δt: per-spill exponential fit → extrapolated Poisson
  rate, annotated at 90°, anchored at **0.65 in Y** (`y_lo·(y_hi/y_lo)^0.65`). No
  "entries" Z-axis label.
- Afterpulse hitmap: **clamp negatives to 0** (`SetMinimum(0)` + display-clone
  clamp) — negatives are sideband-subtraction statistical artefacts.
- Coverage map is the **XY** one; detector-readiness = `sum(channel_weights) /
  total_channels` (active-over-full-detector, spill-weighted — accounts for the
  always-dead rdo-193 *and* per-spill drift).
- **Streaming-score plot colours (LOCKED):** signal (data / post-first-frames) =
  **RED**, DCR (noise / first-frames) = **BLUE**, in-beam close-to-beam-bkg =
  **VIOLET**. Cut line stops in height where the text box starts. Tail S/N from
  fractions (counts are `double`, not `long long`).

---

## 6. Trigger / physics semantics

- **Hardware triggers** include **TIMING** (it *is* a hardware trigger — keep it).
- **Secondaries / non-hardware** to exclude from per-trigger Δt and the synthetic
  marker set: `FirstFrames`, `StartOfSpill`, `UNKNOWN`, `STREAMING_RING_FOUND`,
  `HOUGH_RING_FOUND`. (UNKNOWN and STREAMING_RING_FOUND are secondaries; TIMING is
  not.)
- **Rollover:** `BTANA_ALCOR_ROLLOVER_TO_CC = 32768`; per-(stream,spill)
  correction in the framer. Δt fills look up frame ±1 rollover on the *away sides*
  (`kFrameOffsetForRollover = rollover_divides_frame ? kRollover/frame_size : 0`).
- **GlobalIndex layout:** `channel_ordinal = (device-192)*256 + …`;
  `device = 192 + ordinal/256` under `gidx::kUsesSplitInTwo` (true → 256 ch/device,
  false → 512). `kChansPerDevice = kUsesSplitInTwo ? 256 : 512`.
- **ALCOR vs ALTAI:** cherenkov / timing / tracking are *one ALCOR pipeline*
  (semantic split only). **ALTAI** is an external tracker from a separate DAQ.
  ALCOR-`tracking` is 2024-only legacy.
- **Streaming regimes:** runs split into streaming-noisy (σ_S ≳ 5000, main-thread
  bound) vs dense (framer bound). The regime decides which perf lever helps.

---

## 7. Acquisition backend (v2)

- **Scheme-aware:** the `[source].address` determines the transfer backend.
  `user@host` → **rsync**; `https://…` → **curl** (stubbed — `NotImplementedError`
  for now); local path → local copy.
- Single auto-detected `[source].address`; legacy `[rsync]` block kept as a
  back-compat fallback. `list_remote_runs` must read `rsync_host_dir()` (the
  `[source]`-aware path), not legacy fields.
- The **lex-newest run id on the remote may still be acquiring** — treat everything
  *older* as sealed.

---

## 8. Code-style conventions

Detailed naming + structural rules live in the sibling
[`coding_conventions.md`](coding_conventions.md) (snake_case / PascalCase, the
local-vs-type collision rule, `BTANA_` macros, …). This section is only the
high-level habits:

- **No dates in code comments / footers** — git blame carries that.
- Match the surrounding code's comment density, naming, and idiom.
- New pure logic gets unit tests (the dashboard's data plumbing is deliberately
  separated from its Qt/matplotlib shell so it's testable).
- Backlog items that are real but out-of-scope go in `BACKLOG.md`, not inline TODOs.

---

## 9. v2 strategy

Lock structural contracts (this file) **before** tagging v2 so post-v2 changes are
additive. Retro-compat-breaking changes are prioritized *before* the tag; after
v2, prefer additive patches. The QA dashboard structure will keep evolving live —
the data contracts above are what must stay stable across the next run.

Open pre-v2 batch (see memory `pre-v2-audit-decisions`): chips array fix +
parser-harden, `Data/` comment fixes, wire `streaming.n_fires` publish, recodata
helper PDFs → `qa/`, streaming-picker in-beam curve + recolor. The macro
"dim-swap" was confirmed to be **stale data, not a code bug**.
