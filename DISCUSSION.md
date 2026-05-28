# Project design log

Local, never-published notes covering four kinds of project state.  Each
section is self-contained; entries are organised so any single one can be
promoted to a GitHub issue when it's worth collaborating on.

| Section | What it holds | Removal trigger |
|---|---|---|
| [Satellite discussions](#satellite-discussions--hub) | Pointers to per-area `DISCUSSION.md` files scattered through the tree.  This file is the **hub** — every other DISCUSSION.md should be reachable from one click here. | Satellite file added / removed → update the hub. |
| [Triage taxonomy](#triage-taxonomy) | Project-wide convention for tagging backlog items (Bug / Liability / Vulnerability / Patch / Feature / Schema) + the priority formula.  Defines the format used by [`BACKLOG.md`](BACKLOG.md). | Taxonomy change → update the chapter + retag BACKLOG.md. |
| [Design discussions](#design-discussions) | Open architectural questions — **decision needed before any code change**.  Each `D-XX` is a self-contained proposal with options + recommendation. | Decision made → entry deleted (the resulting change goes into TODOs or directly into a PR). |
| [TODOs](#todos--concrete-fixes-in-the-queue) | Concrete code-work items.  No design decision pending — just hands on the keyboard.  Source files carry `CODE_REVIEW §X.Y` breadcrumbs at the original finding sites. | Fix lands in `main` → row removed. |
| [Attention points](#attention-points--latent-issues-to-be-careful-about) | Latent caveats in the codebase that don't need a design discussion but **do** need a heads-up so they don't get propagated or forgotten. | Caveat resolved or formally captured as a design question / TODO. |
| [Coding conventions](#coding-conventions) | Naming + style reference for the project.  No removal trigger — this is reference material. |

> File is `.gitignore`d (see `.gitignore`).  When an entry is worth
> collaborating on, open a GitHub issue with the appropriate label
> (`design` / `enhancement` / `bug`) and drop the entry from here.

---

## Satellite discussions — hub

The project keeps per-area `DISCUSSION.md` files next to the code they
discuss, so design notes live where the implementation lives.  This
hub is the **single landing page** — start here, jump to the area you
care about.  Whenever a new satellite lands, add a row.

| File | Scope | Highlights |
|---|---|---|
| [`include/triggers/DISCUSSION.md`](include/triggers/DISCUSSION.md) | Community-facing reference for the **triggers subsystem** as a whole (TriggerEvent schema, registry, sequencing). | TriggerEvent's physical-origin limitation; future schema-extension proposal (GlobalIndex / device-fifo-channel triple). |
| [`include/triggers/streaming/DISCUSSION.md`](include/triggers/streaming/DISCUSSION.md) | Deep-dive on the **DCR-weighted streaming + Hough trigger** stages.  Section refs match per-stage headings here. | D-12 v1 design + Hough §2.3 sub-cell refinement (sliding-window, SAT, padding); §2.6 live-QA pipeline (V1 shipped); §2.7 frames-within-spill multithreading. |
| [`include/writers/DISCUSSION.md`](include/writers/DISCUSSION.md) | Open questions specific to the **writers** (pulser_calib, lightdata, recodata, recotrackdata). | Pulser ±0.5 cc satellite hypothesis; regime-2 slip vs coarse-edge quantisation; fine-band filter as IRLS candidate. |
| [`qa_quicklook/DISCUSSION.md`](qa_quicklook/DISCUSSION.md) | Longer-term roadmap for the **operator dashboard**.  Items the dashboard *should* do but aren't pressing. | PDF publication contract; AnalysisResults dual-backend (ROOT → TOML); audit log + Show-history UI; cross-shifter sync candidates. |

**Conventions for the satellites:**

- One satellite per major subdirectory (`include/triggers/`, `qa_quicklook/`, …) — not per file.
- Open items in satellites should also appear in the hub-level [`BACKLOG.md`](BACKLOG.md) so the queue stays single-source.
- The satellite holds the *narrative* (why, options, history); the
  backlog row holds the *tag* + the *priority*.  Cross-link by
  free-text reference, not by mechanical sync.
- When a satellite's discussion section closes (decision taken,
  feature shipped), strike it through but keep the prose — it's
  historical context for the next reader.

---

## Design discussions

Open architectural questions.  Each `D-XX` is a self-contained proposal —
File, observation, options, recommendation, decision needed — ready to be
moved into a GitHub issue when it gets actively worked on.

e
**Observation:**  
`TH1::AddDirectory(false)` is called globally (line 35 of
`src/lightdata_writer.cxx`), which correctly detaches new histograms from the
current TFile directory so they are not double-deleted.  However, histograms
are still allocated with bare `new TH1F / new TH2F` (~60 sites in
`lightdata_writer.cxx` alone) and the only deletion is via `outfile->Close()`
(which calls `TDirectoryFile::Delete` on its owned objects) or — for histograms
that were `Write()`d and then never attached — implicit memory leaks if an
exception or early return fires.

In the current code, the leak is bounded per-run (histograms are local to a
function scope and ROOT closes the file at the end), but it complicates
analysis-macro reuse, and the pattern is error-prone when the function grows.
On a runlist driver looping over runs the leak compounds linearly with the
number of runs processed.

**Options:**

| Option | Mechanism | Trade-off |
|--------|-----------|-----------|
| **A — keep bare `new`, document ownership** | Add a comment block above each histogram batch explaining the lifecycle | Zero effort; fragile; does not survive copy-paste into new functions |
| **B — `std::unique_ptr` with custom deleter** | `auto h = std::unique_ptr<TH1F, void(*)(TH1F*)>(new TH1F(...), [](TH1F *p){ p->Write(); delete p; });` | RAII write-then-delete; verbose; ROOT's `Write()` must be called before the file closes |
| **C — helper wrapper** | Write a small `root_hist<T>` RAII wrapper (or use ROOT's `std::unique_ptr`-aware factory if available) that writes on destruction iff the file is still open | Cleanest long-term; requires a new utility header; non-trivial if histograms must be written in a specific order |

**Recommendation:** Option A for now (with a clear comment block), with Option C
as a future improvement when the macro layer is refactored.  Option B's inline
lambda pattern is error-prone because ROOT's write order can matter for
directory structure.

**Decision needed:** Preferred approach for the analysis macros where histograms
are long-lived?  Any preference for Option C with a shared `root_utils.h`?

---

### D-03 — Ring centre-finding algorithm improvement → moved

**Migrated to [`include/triggers/streaming/DISCUSSION.md`](include/triggers/streaming/DISCUSSION.md) § 2.3
("Sub-cell centre refinement")** as part of the streaming-Hough stage
treatment.  The Hough refinement is the natural home for this discussion
now that the trigger subsystem owns the ring-finder pipeline end-to-end.

---

### D-04 — `fit_circle` review → moved

**Migrated to [`include/triggers/streaming/DISCUSSION.md`](include/triggers/streaming/DISCUSSION.md) § 2.4
(`fit_circle` role and audit)**.  Co-located with the streaming-Hough
stage that consumes it — the next physical use of `fit_circle` is the
centre refinement described in § 2.3, so the audit lives next to its
client rather than as a standalone parking-lot item.

---

### D-05 — Introduce an `event` class as the home for per-burst analysis primitives

**Status: design proposal — needs scope decision.**

**Context:**
Today the analysis pipeline carries data at three granularities:

| Granularity | Owner | What it groups |
|-------------|-------|----------------|
| Spill | `AlcorSpilldata` | every hit + every trigger from a beam spill (~seconds) |
| Frame | `AlcorLightdata` (one per frame slot inside a spill) | hits within one 1024-cc (~3.2 µs) readout window |
| hit   | `AlcorFinedata`, `AlcorRecodata`  | a single TDC channel firing |

The grouping that's **missing** is a free-floating temporal cluster — "every
hit within ±Δt of a chosen reference time", where the reference is not
necessarily a trigger.  Such clusters are the natural home for analysis
primitives that work on a small set of hits:

- ring fit / centre refinement (see [D-03](#d-03--ring-centre-finding-algorithm-improvement))
- circle fit on assigned hits (see [D-04](#d-04--fit_circle-review))
- background hit exclusion / iterative re-association
- mean-time / time-of-arrival reconstruction
- afterpulse-tail removal
- cross-talk neighbour pruning
- single-photon vs. multi-photon classification

Currently each macro and each pipeline stage builds this cluster ad-hoc
(usually by filtering on `hit.time` against some reference, then iterating
over the survivors).  The filter logic and the analysis primitive end up
tangled in the same function.

**Proposal:**

```cpp
/// @brief A free-floating temporal cluster of ALCOR hits — the natural
///        granularity for per-burst analysis primitives (ring fit,
///        background exclusion, time-of-arrival, …).
///
/// Constructed from any hit container plus a reference time and a time
/// window.  The reference time is *free* — it can come from a hardware
/// trigger, a configured-as-trigger channel, an external signal, or
/// (for triggerless studies) the first hit in a frame.
class Event
{
public:
    /// Time window in clock cycles or ns; explicit at construction.
    Event(const std::vector<AlcorFinedata> &hits,
          double t_ref,
          double dt_minus, double dt_plus);

    // --- View accessors
    [[nodiscard]] const std::vector<AlcorFinedata> &hits() const noexcept;
    [[nodiscard]] double t_ref()     const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool empty()       const noexcept;

    // --- Analysis primitives (own the small algorithms)
    /// Least-squares circle fit over the event's Hit positions.
    /// Currently delegates to ::fit_circle (see D-04 for the review queue).
    [[nodiscard]] std::optional<CircleFitResults> fit_circle(
        std::array<float,3> initial_guess,
        bool fix_xy = true) const;

    /// Hough-transform ring centre (delegates to mist::ring_finding).
    [[nodiscard]] std::optional<mist::ring_finding::RingResult>
    find_ring_hough(const mist::ring_finding::HoughTransform &ht,
                    float threshold_fraction = 0.3f,
                    int min_hits = 5) const;

    /// Returns a new event with hits matching a predicate removed.
    /// Used by callers to strip afterpulses / electrical cross-talk /
    /// configured background channels before running the primitive above.
    template <typename Pred>
    [[nodiscard]] Event without(Pred &&pred) const;

    /// Returns a new event with hits NOT matching the predicate removed.
    /// (Equivalent to .without([&](auto &h){ return !pred(h); }).)
    template <typename Pred>
    [[nodiscard]] Event keep(Pred &&pred) const;

    /// Mean / median / first-Hit time over the event's hits.
    [[nodiscard]] double mean_time()  const;
    [[nodiscard]] double first_time() const;

private:
    std::vector<AlcorFinedata> hits_;
    double t_ref_;
};
```

#### Where it would land in the codebase

- A new `include/event.h` (top level — domain type, not a utility).
- Owns the *primitives* that today live as free functions under the
  `include/util/` umbrella ([`circle_fit.h`](include/util/circle_fit.h)
  for `fit_circle`, [`ring_model.h`](include/util/ring_model.h) for
  `fit_ring_integral`).  The free functions stay (other call sites use
  them) but `Event::fit_circle` becomes the recommended entry point —
  consistent with the "domain-data carries its analysis API" pattern
  established by the post-Phase-5 header reorganisation.
- Lives **outside** the `include/util/` umbrella because it's a
  pipeline-level concept (groups hits, drives per-burst analyses), not a
  helper.

#### Coupling with already-open discussions

| Discussion | Interaction |
|------------|-------------|
| [D-03 — ring centre-finding](#d-03--ring-centre-finding-algorithm-improvement) | `Event` would be the natural target of the refinement step (Hough → `Event` → `fit_circle` or NN). |
| [D-04 — `fit_circle` review](#d-04--fit_circle-review) | `Event::fit_circle` is the new method; its implementation either calls the existing `::fit_circle` or replaces it.  The review and the introduction of `Event` should land together so the new method's signature is the one that exits review. |

#### Open scope questions

| ID    | Question                                                                                                                                                                                                                                                                                                                                                              |
|-------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Q-05.A | **Hit ownership.** Does `Event` *own* its Hit vector (copy at construction) or *view* it (e.g. `std::span<const AlcorFinedata>` of the parent container)?  Ownership is simpler and survives parent destruction; view is cheaper and matches the "small temporal slice" use case.                                                                                                  |
| Q-05.B | **Mutating vs. functional API.** The sketch above is purely functional (`without`, `keep` return new events).  Faster alternative: mutate in place.  Functional is safer for chained analyses; mutating is faster when memory pressure matters.                                                                                                                                            |
| Q-05.C | **Where do triggers fit in?** Should `Event` carry the trigger that defined its `t_ref` (a `std::optional<TriggerEvent>` field), or is the reference time always just a number?  Carrying the trigger is more informative; keeping it numeric makes the class compatible with triggerless analyses (time-cluster self-trigger, see the README's "In development" section).                  |
| Q-05.D | **Construction sites.** Does `Event` get constructed by the writers (one event per natural cluster, stored in the recodata branch), or only on demand by analysis code (macros and downstream)?  Storing them makes downstream code thinner; not storing them keeps the schema unchanged.                                                                                                  |
| Q-05.E | **Templating on the Hit type.** The sketch uses `AlcorFinedata`.  Should `Event` be templated so it also works on `AlcorRecodata` (post-reconstruction hits with positions) and `AlcorRecotrackdata` (track-matched)?  Probably yes — but the analysis primitives need to be valid for whichever Hit type is parametrised.                                                              |

**Recommendation:**
Land a minimal first version that:
- Owns the Hit vector (Q-05.A → own, deferring the view optimisation).
- Has the functional API (Q-05.B → `without`/`keep` return new events).
- Holds `t_ref` as a plain `double` (Q-05.C → numeric, with the trigger
  passed alongside when needed).
- Is constructed on demand by analysis code (Q-05.D → no schema change).
- Is templated on the Hit type (Q-05.E → templated, with the primitives
  guarded by SFINAE / `if constexpr` for Hit types that don't carry the
  required field).

That first version absorbs the ring fit + circle fit (closing D-03 + D-04
in the same PR) and gives the macros a uniform entry point.  Optimisations
(view-of-parent, mutating variants) and storage integration land in a
follow-up if the simple version proves too costly or too limiting.

---

### D-08 — Two-layer data/wrapper separation for ROOT-bound classes

**Pilot landed for `AlcorSpilldata` + `AlcorSpilldataStruct`** —
see [`include/alcor_spilldata.h`](include/alcor_spilldata.h) /
[`src/alcor_spilldata.cxx`](src/alcor_spilldata.cxx).  POD is move-only,
wrapper is non-movable, branch-pointer slots moved off the struct into
the wrapper, all dead by-value getters / setters / unreliable `operator+`
overloads removed.  Closes §2.4, §2.5, §3.7, and the F1/F2/F3 latent
traps for the spill class.

**Remaining classes to migrate** (same pattern, each independent):

| Class | Pointer slots | Touches | Notes |
|---|---|---|---|
| `AlcorRecodata` | 2 | ~10 recodata-consuming macros + `recodata_writer` | Bulk of the macro work |
| `AlcorRecotrackdata` | 1 | ~3 files | Smallest |
| `AlcorData` | 0 (defaulted copy + branch binding) | Held by reference in `AlcorDataStreamer`; future signature change would silently dangle | Audit: full two-layer or just mark non-copyable? |

**Open question still pending decisions:** Reader-vs-writer split for the
remaining wrappers — runtime flag (small surface, asserts on misuse) or
compile-time `ReadOnly*` subclass (zero chance of misuse, more types).
DISCUSSION's original recommendation was "runtime flag for the pilot;
introduce subclass later if read-only sites concentrate."  The pilot
went with neither — `AlcorSpilldata` is already split between
`prepare_tree_fill` (writer-only) and `link_to_tree` (reader path).
Revisit if `AlcorRecodata`'s reader/writer surfaces diverge enough to
warrant separating them.

**Related gotcha discovered during the pilot:** `AlcorDataStreamer`
exhibits the same family of "branches binding to addresses that move
under you" trap as the F1/F2/F3 cases.  Its move-ctor / move-assign now
re-link branch addresses via `data.link_to_tree(tree)` and the framer's
ctor was reverted to `emplace_back` + conditional `pop_back` so the move
never happens in the canonical path.  See the Attention-points table
below for the full forensic note.  Future ROOT-bound wrapper classes
should follow the AlcorSpilldata rule: **non-copyable + non-movable**.

---

### D-09 — Framer worker-pool + thread-count formula

**Status: ✅ resolved.**

- **Problem 1 (thread-count formula).**  The old formula
  `8 * std::min(n_usable, 2u)` was algebraically a hard ceiling of 16 on
  every machine with ≥ 4 cores regardless of `n_usable`.  Replaced with an
  explicit three-way clamp:
  `min({requested ? requested : max(1u, n_usable), kMaxWorkers, n_streams})`
  where `kMaxWorkers = 16` is a named constant.  Now scales properly to
  the cap on bigger nodes, drops to fewer workers when stream count is
  small, and respects the explicit user override.
  See [`src/parallel_streaming_framer.cxx:565-595`](src/parallel_streaming_framer.cxx:565).

- **Problem 2 (per-spill `std::async` overhead).**  Instrumented with
  `steady_clock::now()` brackets around the spawn loop and the wait loop.
  Smoke-test measurement on a realistic workload (16 workers, 240 streams):
  `spawn=0.17 ms, work=3656.91 ms` → **0.0% spawn overhead**.  170 µs of
  pthread create vs 3.66 s of work — well below the 1% threshold for
  "worth a pool."  The current `std::async` model is fine; no
  `WorkerPool` warranted.  Instrumentation removed after the measurement
  was recorded; restore it (see git history of `parallel_streaming_framer.cxx`
  around `next_spill()`) if the spawn-vs-work ratio is ever in question
  on a different node / spill profile.

---

### D-10 — Shared lookup-table state (Mapping, RunInfo)

**Files of concern:**
[`include/mapping.h:344–376`](include/mapping.h:344) + [`src/mapping.cxx:145–233`](src/mapping.cxx:145) (static maps mutated by `load_calib()`); [`include/util/config_reader.h:390–391`](include/util/config_reader.h:390) + [`src/config_reader.cxx:456–457`](src/config_reader.cxx:456) (`RunInfo::run_info_database`).

**Observation:**
Two related architectural concerns about the lookup state consumed by the framer/writer hot paths.

#### Mutable static state (§3.3, §3.5)

`Mapping` exposes five `static std::map<…>` members mutated by `load_calib()` (a non-static method) and read concurrently by every framer worker via `get_position_from_finedata`, `assign_position`, …  No mutex protects them.

In the current flow `load_calib` runs once before workers spawn — but the design invites disaster (any future "reload calibration mid-run" hook is a race), and two `Mapping` instances silently share state.

Same family in `RunInfo::run_info_database` — static maps mutated by `read_database`, read by `get_run_info`.  Lower impact today (typically read at program start) but the same fragility plus C++ static-init-order across TUs.

#### Lookup type performance (§3.4)

`Mapping::index_to_hit_xy` is `std::map<int, std::array<float,2>>` keyed by `4 * channel_ordinal` — dense small integers (0…~8192).  `std::map` for ~2k entries on the hot path is wasteful (log-N cache-unfriendly tree walks per lookup).  `hit_xy_to_index` uses `std::array<float,2>` as key → relies on exact floating-point equality (fragile).

**Options:**

| Concern | Option | Trade-off |
|---|---|---|
| **State** | A — make members non-static (per-instance); callers hold a `Mapping` reference | Removes all global state; threading-safe by construction.  Caller-side API change: `Mapping::method()` → `mapping_instance.method()`. |
| | B — protect with `shared_mutex` + atomic "frozen" flag (mirror calibration-table pattern, §3.1) | Preserves API; lock-free reads once frozen.  Static state remains. |
| **Lookup type for `index_to_hit_xy`** | A — `std::unordered_map<int, std::array<float,2>>` | Trivial swap; ~5–10× faster lookups; preserves API. |
| | B — `std::vector<std::array<float,2>>` indexed by `channel_ordinal` | Fastest (direct index); requires knowing max `channel_ordinal` at construction; gaps zero-initialised. |
| | C — keep `std::map` for ordered iteration | Only if any consumer actually iterates in order — audit needed. |
| **`hit_xy_to_index` robustness** | D — rehash on rounded-integer-mm key | Independent of above; fixes the float-equality fragility regardless. |

**Recommendation:** state → A (per-instance); lookup → A (or B if max `channel_ordinal` is fixed at construction); D for the inverse map.

**Decision needed:**
1. Static → per-instance migration — `Mapping` and `RunInfo` together, or staggered?
2. Lookup type for `index_to_hit_xy` — `unordered_map`, `vector`, or stay with `map` (audit needed)?
3. Make `hit_xy_to_index` robust now (D), or defer until the broader fit/circle work in D-04?

---

### D-11 — Streaming-trigger pipeline review

**Status: review — multiple open questions, one immediate bug to triage.**

**Files of concern:**
- [`src/lightdata_writer.cxx:1303–1592`](src/lightdata_writer.cxx:1303) — `run_streaming_trigger()` (the per-frame online evaluator).
- [`src/lightdata_writer.cxx:478, 368`](src/lightdata_writer.cxx:478) — the two configuration knobs (`threshold`, `time_window_ns`) hardcoded at the call site.
- [`src/lightdata_writer.cxx:666–676`](src/lightdata_writer.cxx:666) — the call site inside the per-spill, per-frame post-processing loop.
- [`include/writers/lightdata.h:82–99`](include/writers/lightdata.h:82) — public declaration.

#### Pipeline overview

For every frame in the spill, after the framer has produced
`cherenkov_hits`, the streaming trigger runs:

1. **Time-sort** the frame's Cherenkov hits (afterpulse-tagged hits are
   skipped in the main loop).
2. **Sliding window** (`std::deque`) over the time-ordered hits.  Front =
   oldest, back = newest.  A hit is evicted from the front whenever
   `(current_time - front_time) > time_window_ns`.  Insertion is always at
   the back.  Both O(1).
3. **Cluster detection.**  When `window.size() >= threshold` we enter the
   "in cluster" state.  The `peak_count` and `peak_times` snapshot is
   updated every time the window size grows past its previous peak.
4. **Cluster end.**  Triggered either by a hit that drops the window size
   back below threshold (eviction from the front faster than new hits
   arrive), OR by end-of-frame.  Fires `end_of_cluster()`:
   - QA fills: leading-edge Δt, cleaned-times Δt, split-half mean/median,
     TDC step-size diagnostics (catches near-zero steps = TDC walk
     artefacts).
   - Outlier rejection (`clean_times`): recursive endpoint removal when one
     endpoint creates a gap >2× larger than the other half's range.
   - Emits a `TriggerEvent{_TRIGGER_STREAMING_RING_FOUND_, peak_count,
     median_of_peak_times}` into the frame.
5. **Carry-over.**  Hits still in the window at end-of-frame are
   re-injected at the start of the next frame's window, time-shifted by
   `-frame_length_ns` so the window timing stays continuous across frame
   boundaries.

#### Inventory of components

| Component | Status |
|---|---|
| Time-window sliding-deque | Clean implementation; O(1) insert/evict; correct invariants. |
| Outlier rejection (`clean_times`) | Recursive min-IQR with `kOutlierRatio = 2.0f`.  Heuristic — works for sparse DCR but no formal characterisation. |
| Split-half mean / median sigma | `leave_one_out_mean()` + odd/even median diff.  Standard sub-Poissonian σ estimator.  Statistically sound. |
| Trigger time | Median of `peak_times` (snapshot at maximum occupancy).  Robust to outliers; not necessarily the first-photon time. |
| TDC-walk diagnostic | `h_tdc_step_sizes`, `h_tdc_zero_times`, `h_tdc_zero_cluster_size` — flags steps < 0.1 ps (TDC firing the same value twice).  Useful. |
| Afterpulse exclusion | `is_afterpulse()` check at the top of the loop.  Correct — AP should not trigger. |
| QA histograms | 13 hists declared at `lightdata_writer.cxx:355–377`, written to the `Streaming Trigger` directory at `:1199–1235`. |

#### Issues

1. **Threshold hardcoded to 10000** (`lightdata_writer.cxx:478`).
   ```cpp
   const int threshold = 10000; //5; //std::max(1, static_cast<int>(std::ceil(cherenkov_fraction * n_active_cherenkov_channels)));
   ```
   Unreachable on real data — the trigger never fires on any current
   beam-test dataset.  The two commented-out alternatives suggest this was
   bumped for debugging and never reverted.  See the bottom of this row
   for fix options.

2. **`time_window_ns = 5.f` hardcoded** at `lightdata_writer.cxx:368`.
   Belongs in a config block (`framer_cfg`?  a new `[lightdata.streaming]`
   TOML section?) — same class of issue as the timing-chip layout we
   tracked under D-07.  At minimum a `static_assert` + named constant
   would make the value greppable.

3. **No spatial constraint at trigger time.**  The cluster is built purely
   on temporal coincidence.  In a high-DCR environment, threshold = 5
   hits in a 5 ns window can fire from random coincidences with no ring
   structure.  Downstream the Hough transform filters by ring shape but
   the trigger has already been emitted by then.  This is by design (the
   purpose is to capture *every* frame where a ring *might* exist) but
   worth noting that the trigger rate is dominated by accidentals when
   threshold is low.

4. **`peak_times` semantics tied to maximum occupancy, not cluster
   boundaries.**  `peak_times` is the snapshot at the moment the window
   was largest.  If the cluster has a long tail, hits beyond the peak
   never enter the QA histograms.  Probably intentional — focuses QA on
   the densest part — but worth documenting.

5. **Carry-over re-injection assumes the next frame starts at
   `frame_length_ns` after the current frame.**  At
   `lightdata_writer.cxx:1590` the carry-over time is shifted by
   `-frame_length_ns`.  Correct under the standard ALCOR continuous-time
   model but breaks if frame boundaries ever become non-uniform (e.g.
   dropped frames).  Not currently triggerable.

6. **Window-size eviction is hit-driven, not time-driven.**  An eviction
   only happens when a NEW hit comes in (it triggers the `while` loop at
   `:1554`).  If a cluster fires near end-of-frame and then no more hits
   arrive, the cluster ends only via the end-of-frame `if (in_cluster)
   end_of_cluster()` at `:1584`.  This is correct but means the cluster
   timing reported can drift up to a frame length if the last cluster has
   no trailing eviction trigger.

#### Open questions

| ID | Question |
|---|---|
| Q-11.A | Threshold formula: literal (`5`) or dynamic (`max(1, ceil(0.004 × n_active_cherenkov))`)?  The dynamic form adapts to active-channel count; the literal is simpler.  Probably want dynamic with a floor at 5–10 to avoid fluctuating cluster size at low channel counts. |
| Q-11.B | Should `time_window_ns` and `threshold` (and `cherenkov_fraction`) move into the framer config (`framer_conf.toml` → new `[streaming_trigger]` table) so they are written into the output file's Config dir alongside the other knobs? |
| Q-11.C | Spatial pre-filter at trigger time?  A coarse "all hits within R mm" check (centroid of window's xy positions) could halve the false-positive rate at low threshold, at the cost of looking up `mapping.get_position(...)` per hit.  Worth a benchmark. |
| Q-11.D | Trigger-time choice — `median_of(peak_times)` versus `mean_of(cleaned_times)` versus first-cross-threshold time.  Median is robust; mean of cleaned matches the Hough-stage time reference; first-cross is the earliest-photon time, useful for time-of-flight studies. |
| Q-11.E | Hough downstream — see [D-03](#d-03--ring-centre-finding-algorithm-improvement).  The streaming trigger feeds candidate frames to Hough; if D-03 lands a sub-cell refinement step, the streaming trigger's `trigger_time` becomes the input to that refinement. |
| Q-11.F | Carry-over indexing: hits are re-injected with `index = -1` as a sentinel for "not in this frame's `cherenkov_finedata_hits` array."  The QA path doesn't currently use the index, but if a future change does, the sentinel will silently break it.  Worth a typed wrapper. |

#### Immediate action — superseded

The literal-`5` / `ceil(0.004 × N)` choice is replaced by the DCR-driven
weighted-score design captured in [D-12](#d-12--dcr-driven-streaming-trigger-with-per-channel-inverse-dcr-weighting).
The current hardcoded `threshold = 10000` should be left as-is (i.e. the
trigger stays inert) until D-12 lands.

---

### D-12 — DCR-driven streaming trigger with per-channel inverse-DCR weighting

**Status: v1 LANDED 2026-05-26 (subsequently split by stage in the
streaming-trigger consolidation).  Code lives in
[`include/triggers/streaming/score.{h,cxx}`](include/triggers/streaming/score.h)
(stage 1 — DCR-weighted score) and
[`include/triggers/streaming/hough.{h,cxx}`](include/triggers/streaming/hough.h)
(stage 2 — Hough ring finder).  Community-facing reference lives in
[`include/triggers/streaming/DISCUSSION.md`](include/triggers/streaming/DISCUSSION.md).
The entry below is preserved for the implementation-decision history;
see the public doc for the current state.**

**Files of concern:**
- [`src/lightdata_writer.cxx:478`](src/lightdata_writer.cxx:478) — the `threshold = 10000` literal that D-12 replaces.
- [`src/lightdata_writer.cxx:1303–1592`](src/lightdata_writer.cxx:1303) — `run_streaming_trigger()`, currently a free function inside the writer.
- [`include/parallel_streaming_framer.h`](include/parallel_streaming_framer.h) — host of the DCR QA infrastructure (`h_afterpulse_dt`, first-frames noise sampling).

#### Goal

Replace plain hit counting with a **likelihood-weighted score** that:
1. Down-weights noisy channels in proportion to their measured DCR.
2. Sets the threshold from the measured noise distribution itself, not a fudge constant.
3. Carries the QA infrastructure to **measure misfire and missed-fire rates directly** as a function of threshold, so the operating point is chosen on data, not on a target number pulled from the air.

#### Why weighted (not just DCR-thresholded counting)

Signal hits are physically per-channel — only sensors on the ring fire — but **we don't know which channels are on the ring** (finding the ring is the downstream problem the trigger feeds).  Plain counting weights every channel equally; a single hot pixel can drive false positives.

Under the simplifying noise model:
- Per-channel Poisson with rate $\lambda_i$
- Uniformly-likely signal across channels (rough but better than "uniform noise importance")

the Neyman–Pearson optimal pre-filter in the small-signal limit reduces to thresholding $S = \sum_{\mathrm{hits}} 1/\lambda_i$.  Noisy channels contribute less per hit; quiet channels carry the evidence.

#### The score

$$S = \sum_{i \in \mathrm{window}} w_i, \qquad w_i = \frac{1}{\lambda_i}$$

Under pure-noise hypothesis $H_0$ in a window of width $T$:

$$\mathbb{E}[S] = T \cdot N_{\mathrm{ch}}, \qquad \mathrm{Var}[S] = T \cdot \sum_i \frac{1}{\lambda_i}$$

Mean is detector-size-only — independent of the DCR distribution.  Variance is dominated by the quietest channels (which are the evidence-rich ones).  Threshold is then a Gaussian / Poisson tail at a target false-positive rate, measured directly from QA below (no need to assume Gaussianity analytically).

#### QA design — measure the threshold rather than derive it

Two histograms, **same axis**, populated in different windows of the spill:

| Histogram | Filled from | Tells you |
|---|---|---|
| `h_streaming_score_noise` | First-frames noise sample (beam-off; already tagged by `_first_frames_trigger`) | Pure-noise score distribution.  $\int_{S\geq S_\star} \mathrm{d}P$ → **misfire (false-positive) probability** |
| `h_streaming_score_data` | Beam-on portion of the spill | Signal+noise score distribution.  $\frac{\int_{S\geq S_\star}}{\int}$ → **acceptance**; $1 - \mathrm{acceptance}$ → missed-fire rate |

Pick the threshold by sliding $S_\star$ along the x-axis and reading the two integrals — gives a ROC curve on real data.  No analytic Poisson tails needed; no fudge constant.

#### Per-channel DCR source

The framer's existing **`h_afterpulse_dt`** (same-channel Δt distribution, filled in the framer's ALCOR-Hit branch) is the natural DCR source — the flat tail beyond the afterpulse window is pure DCR by construction.

**Update cadence**: read off the same QA histogram values that already exist; refresh per spill / per run as naturally as the QA itself updates.  DCR variance across a fill isn't expected to be large enough to warrant anything fancier.

**Storage**: no separate calibration file — the DCR values live in the QA plot itself.  The trigger reads them at startup (or at spill boundary for per-spill refresh) and builds the weight table on the fly.

**Floor**: cap channel DCR at 1 kHz from below (i.e. $w_i \le 10^{-3}$ s).  Channels measured below 1 kHz get clamped — this includes the "dead-quiet, hasn't fired in the calibration window" case.  No need to special-case dead channels; the framer already excludes them upstream.

#### Architectural move

`run_streaming_trigger()` currently lives **inside `lightdata_writer.cxx`** (declared in [`include/writers/lightdata.h:82–99`](include/writers/lightdata.h:82), implemented at [`:1303`](src/lightdata_writer.cxx:1303)).  It's a self-contained algorithm that doesn't belong embedded in the writer.  D-12 is the natural moment to extract it.

Proposed home: a new dedicated file, probably one of:
- `include/streaming_trigger.h` + `src/streaming_trigger.cxx`
- `include/util/streaming_trigger.h` (header-only if the inline path is fast enough)
- `include/triggers/streaming.h` (organise by category if more trigger algorithms are coming)

See open question Q-12.C below.

#### Decisions taken

| ID | Decision |
|---|---|
| D-12.1 | Score = $\sum 1/\lambda_i$.  Pure inverse-DCR weighting; signal-per-channel weighting deferred (we don't know which channels are signal — that's what the trigger feeds). |
| D-12.2 | DCR floor at 1 kHz → max weight $10^{-3}$ s.  Hard cap. |
| D-12.3 | DCR source = **`h_dcr_per_channel`** — the lightdata writer's existing TProfile, filled at the existing DCR-QA site (`src/lightdata_writer.cxx:948`) on first-frames frames only.  TProfile's bin content is the mean per-frame hit count; the trigger divides by frame duration to recover $\lambda_c$.  Three earlier attempts — (i) the framer's 1D aggregate `h_afterpulse_dt`, (ii) per-channel near/far Δt-window TH1Fs in the framer, (iii) a parallel first-frames hit counter in the framer — were all rejected at implementation time once it became clear that `h_dcr_per_channel` already measures the right quantity and was just sitting there. |
| D-12.4 | Update cadence = whatever the DCR QA plot updates at; no separate logic.  Variance assumed small over a fill. |
| D-12.5 | Threshold is chosen empirically from the two QA score histograms below, not from an analytic Poisson tail. |
| D-12.6 | Streaming trigger code extracted out of `lightdata_writer.cxx` into a new dedicated translation unit: `include/triggers/streaming.h` + `src/triggers/streaming.cxx`.  A new `triggers/` subdirectory is introduced to anticipate future trigger algorithms (timing-detector trigger, coincidence trigger, etc. — see the follow-ons section). |
| D-12.7 | Score-histogram x-axis: **$n_\sigma$ deviation from noise expectation** = $(S - \mathbb{E}[S]) / \sigma_S$.  Dimensionless, run-independent, threshold is read directly as "fire at $n_\sigma = k$".  Same axis for both `h_streaming_score_noise` and `h_streaming_score_data` so the misfire and acceptance integrals at threshold $n_\sigma^\star$ are directly comparable. |
| D-12.8 | **Noise sample window** stays at the current `first_frames_trigger = 5000` frames (~16 ms / spill, ~3.2 × 10⁶ trigger-windows / spill → 1 ppm tail visible per-spill, 1 ppb after ~100 spills).  Sufficient for any realistic operating point; no need to bump and pay the beam-time cost. |
| D-12.9 | **Threshold lives in a dedicated config file** (likely `conf/streaming_trigger.toml`, or a `[streaming_trigger]` table inside `framer_conf.toml` — to be decided when the file is added).  The framer reads it at startup; the trigger fires or doesn't.  Binary decision. |
| D-12.11 | **Trigger subsystem gets a git-tracked design doc** at [`include/triggers/DISCUSSION.md`](include/triggers/DISCUSSION.md).  D-05 (two-mode config, landed) and D-12 (this) are project-wide-relevant rather than personal-log entries, so they belong in the source tree alongside the code that implements them — not in this local-only file.  This local DISCUSSION.md keeps the *implementation-tracking* view; the in-tree document is the community-facing design reference. |
| D-12.10 | **`TriggerEvent` schema is NOT extended.**  Adding `peak_nsigma` to the emitted event would defeat the writer's whole purpose — the lightdata writer only keeps interesting (= fired) frames; any kept frame is by construction above threshold, so a per-event $n_\sigma$ field is dead weight.  The workflow is:<br>1. Framer runs with the threshold from the config file (initial value is a guess — e.g. $n_\sigma^\star = 3$ — refined later from QA).<br>2. While running, the framer **always** fills the two QA score histograms regardless of whether the threshold is crossed:<br>&nbsp;&nbsp;• `h_streaming_score_noise` — populated during the first-frames window (~16 ms / spill, no hardware trigger fired).<br>&nbsp;&nbsp;• `h_streaming_score_data` — populated during the data-taking window (when hardware triggers are firing — i.e., the post-first-frames part of the spill).<br>3. Frames whose score crosses threshold emit a `TriggerEvent`; the writer keeps them.  Frames below are dropped (the writer's purpose).<br>4. After the run, inspect the two QA histograms.  If the current threshold sits in the wrong place (too many fakes, or visible signal being cut), update the config file and re-run.  The QA is a **correction loop** on the initial guess, not the primary mechanism. |

#### Open questions

_All open questions from the design pass have been resolved.  The next decision points come at implementation time (where exactly the TOML config table lives, layout of `triggers/streaming.{h,cxx}`, etc.) — captured as the implementation lands rather than pre-emptively here._

#### Possible improvements (deferred, not in v1)

- **Conservative DCR estimator.** Mismeasuring a channel's DCR by 2× scales its false-positive contribution by 2×.  Using the 75th-percentile (rather than mean) of the noise-window distribution would bias estimates *high* and make the trigger conservative against under-measured noise.  Not in v1 — adds estimator complexity.
- **Crosstalk correction.** Crosstalk inflates apparent per-channel rates non-Poissonianly.  Either subtract it before computing weights, or carry a per-channel multiplicity factor.  Not in v1 — needs the crosstalk macro pipeline plumbed into the calibration path.
- **Signal-aware weighting.** Once a real ring-finder reports back which channels carry signal in a given event class, the weight could become $w_i = s_i / \lambda_i$ for the full LR optimum.  Bootstrap: run v1 → collect a ring sample → build per-channel signal histogram → re-trigger with full weighting.  Multi-pass calibration.

#### Related, but separate TODOs

- **Extension to timing / tracking detectors.**  Currently the streaming trigger is Cherenkov-only.  The weighting framework handles other detectors cleanly — each channel has its own $\lambda$ — but the operating point and signal model differ.  Tracked as a separate todo (below) once D-12 ships for Cherenkov.

---

## TODOs — concrete fixes in the queue

Code-work items that don't need a design decision — just hands on the keyboard
when there's a spare hour.  Each entry should be removed once it lands in
`main`.  Source files carry `CODE_REVIEW §X.Y` breadcrumbs at the original
finding sites for the items below.

### Low-risk small fixes

| ID | What | Where |
|----|------|-------|
| §util-rename | Rename `include/util/` → `include/utility/` and `src/util/` → `src/utility/` so the subdirectory name matches its umbrella header [`utility.h`](include/utility.h).  Currently inconsistent (umbrella is `utility.h`, subdir is `util/`); the trigger subsystem now mirrors the pattern correctly (`triggers.h` ↔ `triggers/`).  Cross-cutting rename — every `#include "util/..."` line in the codebase needs updating, plus the CMake source list.  Mechanical but touches a lot of files; do in one focused commit. | `include/util/`, `src/`, every `#include "util/…"` site |
| §subfolder-docs | Add a **`README.md`** (current status + macro conventions) to every subfolder under `include/`, and a **`DISCUSSION.md`** (implementation todos / wishlist / open design discussions) wherever there's enough to track.  Convention: README is mandatory and reference-shaped; DISCUSSION is optional and tracks deferred work.  Per-subfolder breakdown:<br>• **`include/util/README.md`** — file-by-file status (`bit_ops`, `global_index`, `toml_utils`, `circle_fit`, `ring_model`, `root_io`, `root_draw`, `root_hist`); header-only convention; "add to `utility.h` umbrella when adding a new file" rule.<br>• ✅ **`include/triggers/README.md`** — done 2026-05-26 (subsystem overview pointing to `streaming/` for the algorithm pipeline).  `DISCUSSION.md` covers D-05 schema + `TriggerEvent` caveats; streaming pipeline moved to `streaming/DISCUSSION.md`.<br>• **`include/writers/README.md`** — three writers' current status (`lightdata`, `recodata`, `recotrackdata`), pipeline shape, caveats: `lightdata_writer` two-pass §4.7 partial tree-cache mitigation, D-12 streaming-trigger calibration workflow, Hough-downstream's separate `hough_threshold` formula, `h_dcr_per_channel` reuse contract that D-12 depends on, `TriggerEvent` schema lacking physical origin.  **`DISCUSSION.md`** — §4.7 full restructure migrates here.<br>*(The "category grouping" pattern at `writers/` has no umbrella header to carry cross-writer notes — README is the right place.  Same applies to any future subfolder that follows this pattern.)* | `include/util/`, ~~`include/triggers/`~~, `include/writers/` |

_§3.8 (pre-index `find_best_trigger`) closed by the 2026-05-26 trigger
config schema rework — the entire scoring/multimap design was superseded by a
two-mode `(DeviceTrigger, ChannelTrigger)` split with O(1) hash lookups.  See
the in-code docs in [`include/triggers.h`](include/triggers.h) and the new
`pack_channel_key` / `TriggerConfigSet` helpers; the schema notes in
[`conf/trigger_conf.toml`](conf/trigger_conf.toml) summarise the
TOML shapes._

### Medium restructures

| ID | What | Where |
|----|------|-------|
| §4.7 | `recodata_writer` does two full `GetEntry` passes over the spill tree — first to collect per-channel offsets, then to write.  **Minimum mitigation landed 2026-05-26**: 50 MB tree cache enabled before the first pass.  The proper fix (compute offsets in the same pass, or cache `(global_index, dt)` pairs to avoid re-reading the tree) is still open. | `src/recodata_writer.cxx` — cache setup near :659, two GetEntry sites at :666 (calibration loop) and inside the second loop at :736 |
| §recodata-modularise | `recodata_writer.cxx` was 2152 lines (all in one function with 10 lambdas).  **Phase 1 landed 2026-05-27**: extracted the CB+pol3 radial fit and the one-param LOO σ(N) fit into `src/writers/recodata/{radial_fit,sigma_vs_n_fit}.cxx` (with declarations in matching headers under `include/writers/recodata/`).  File dropped to 1609 lines; bit-identical output verified vs `Data/20251111-164951/baseline_pre_refactor/`.  **Remaining work**: per-frame compute lambdas (`compute_ring_fit_pure`, `process_frame_pure`, `drain_frame_result`, `fill_ring_hists`) — tightly coupled to 30+ closures, needs context-struct design; lightdata_writer.cxx (1599 lines) follows the same shape. | `src/recodata_writer.cxx` (orchestrator); `include/writers/recodata/` + `src/writers/recodata/` (extracted helpers) |

### Pervasive sweeps (multi-file)

_All previously-tracked pervasive sweeps (§6.10 `Clone()`-without-`SetDirectory(nullptr)`, §6.11 chained `Form()`, §7.4 dead `/* */` blocks, §7.5 stale-comment sweep) have landed.  New sweeps land here when surfaced._

### Verify-then-fix

_§6.2 (TH3F X-axis overflow on Phase-5 fills) closed — the affected histograms in `fine_calibration_timing.cpp` now Fill through `tdc_ordinal()`, and `tools/check_qa.py` provides ongoing detection of this class.  New items land here when discovered._

### Detector-evolution deprecation queue

| When | What | Where |
|------|------|-------|
| When the final 64-ch detector goes live | Flip `gidx::kUsesSplitInTwo` to `false`; run regression tests; mark the framer's split-in-two input adapter `[[deprecated]]`. | `include/util/global_index.h` (the flag); `src/parallel_streaming_framer.cxx` (the adapter) |

### Follow-ons gated by other work

| ID | What | Blocked by |
|----|------|------------|
| §12-ext | Extend the DCR-weighted streaming trigger (D-12) to timing and tracking detectors.  The weighting framework is detector-agnostic — each channel has its own $\lambda$ — but the operating point and signal model differ per detector.  Needs separate threshold tuning + QA histograms per detector class. | D-12 lands for Cherenkov first. |

---

## Attention points — latent issues to be careful about

Known caveats in the current codebase that don't need a design discussion but
*do* need a heads-up so they don't get propagated or forgotten.

| Where | What | Why |
|-------|------|-----|
| `include/triggers.h` — `TriggerEvent` schema | The stored struct is `{uint8_t index, uint16_t coarse, float fine_time, bool is_secondary}`. There is **no detector-side origin** — no `device`, no `fifo`, no `chip`, no `channel`. | Hardware triggers physically originate on dedicated trigger channels (the framer uses `chip = 24`, FIFOs 96–99 — see `parallel_streaming_framer.cxx`). The framer resolves each incoming trigger word against the parsed config — O(1) device-mode lookup for hardware-tagged words, O(1) channel-mode lookup for data-tagged words forced into the trigger path — and emits a `TriggerEvent` that keeps only the resolved `index`. The physical origin is **dropped before storage**. Consequences:<br>• Cannot retroactively ask "which physical channel emitted this trigger?" from a stored `TriggerEvent` — the data is gone unless framing is rerun.<br>• Trigger-latency vs. channel-position studies require either an extended schema or a parallel rerun of `parallel_streaming_framer` with extra bookkeeping.<br>• The "unknown trigger" branch (next row) is a direct symptom: with no `device` field, the framer stuffs `current_device` into the `coarse` (timestamp) field to keep the source ID at all.<br>Future schema bump: add a `GlobalIndex` (TDC-level or channel-level) or an unpacked `{device, fifo, channel}` triple. ROOT schema-evolution can keep old trees readable; new trees would carry the origin natively. |
| `src/parallel_streaming_framer.cxx:458` | `TriggerEvent` for "unknown" hardware triggers stuffs `current_device` into the `coarse` (timestamp) field. | Symptom of the missing-origin schema (row above). Any consumer that reads `coarse` as a timestamp on an "unknown trigger" event will get a device ID instead. Should be revisited together with the `TriggerEvent` schema extension. |
| `include/alcor_data_streamer.h:50–74` + `src/alcor_data_streamer.cxx:65–96` — `AlcorDataStreamer` move-ctor / move-assign | The move-ctor and move-assign now **re-link branch addresses** via `data.link_to_tree(tree)` after the data field is moved. **Do not remove this call.** | When a moved-from `AlcorDataStreamer` is later destroyed, ROOT's branches on the destination's tree would otherwise still point at the source's freed `data` fields.  The previous code (no re-link) crashed with "pure virtual function called" inside `TTree::GetEntry` once the freed memory got recycled — the trigger was the per-spill `push_back(std::move(current_stream))` ctor pattern introduced by §1.3.  Belt+suspenders fix landed: the framer's ctor was also reverted to `emplace_back` + conditional `pop_back` so the move never happens in the canonical path; the re-link defence covers any other moves. |
| `include/alcor_spilldata.h` — `AlcorSpilldata` is non-copyable AND non-movable | Branch addresses bound to the wrapper's `_ptr_` slots; any move would leave them dangling.  Same trap as the streamer row above, just enforced compile-time instead of patched-at-move. | Future ROOT-bound wrapper classes (post-D-08 migration for `AlcorRecodata`/`AlcorRecotrackdata`) should follow the same rule: non-copyable + non-movable, held by reference / `unique_ptr` / member.  Do not relax this without proving the branch-address-stability invariant. |
| `include/writers/` has no umbrella header (and shouldn't get one) | The pattern `<subdir>/` ↔ `<subdir>.h` (e.g. `util/` ↔ `utility.h`, `triggers/` ↔ `triggers.h`) applies to subsystems with **cross-cutting types or helpers** that consumers want to pull in as one set.  `writers/` is a **category folder** for independent pipeline-stage entry points (`lightdata.h`, `recodata.h`, `recotrackdata.h`) that share no types and no interface — each is a `void <name>_writer(...)` taking its own config bundle.  Adding a `writers.h` umbrella would force-fit a pattern that has no payload here: there's nothing to re-export.  When in doubt, ask: *is there a shared type or symbol the umbrella would re-export?*  If no, leave the subdir flat. | Three organisational patterns coexist in this repo on purpose: (1) **umbrella + helpers** (`utility.h`/`util/`), (2) **subsystem types + algorithms** (`triggers.h`/`triggers/`), (3) **category grouping** (`writers/`).  Each fits its role; don't mechanically replicate (1)–(2) where (3) is correct. |
| `tools/lint_codebase.py` + `tools/check_qa.py` | Two repository-side checks that catch the bug classes surfaced by the Phase-5 migration: histogram fills with the wrong GlobalIndex accessor (sparse packed values landing in overflow), debug-leftover histograms (`test`/`test2`), commented-out one-line function calls (silent disables), and legacy `/4`-style bit-bashing formulas.  See [README.md → Repository-side checks](README.md#repository-side-checks-tools). | **Run both after any change that touches histograms, GlobalIndex usage, or per-channel TProfile axes.**  The lint exits nonzero on findings; check_qa exits nonzero on any unexpected EMPTY / OVERFLOW.  Suppression markers: `// LINT-OK:` / `// LINT-OK-FILE:` (lint) and `--known-empty` / `--known-overflow` (check_qa).  These guards exist *because* the per-channel TProfile bugs slipped past local verification — re-run them on every PR that touches anything related. |

---

## 2026-05-27 — Autonomous repo-wide sweep

Performed while the user was away.  Decisions locked beforehand: full
repo scope, aggressive comment purge, bugs fixed on the work tree
(no commits), per-area DISCUSSION.md updates rather than a separate
findings doc.

### Bugs fixed on the work tree

| File | Line(s) before | What |
|---|---|---|
| `src/recodata_writer.cxx` | ~568–604 | **Division by zero** in fine-time offset accumulator (`offset_value /= offset_participants` when all samples failed the |·| > 30 ns outlier cut → 0 participants).  Restructured: compute mean only after the >=20-sample gate.  Also **deleted the dead debug block** (`temy_testt`, three `mist::logger::debug("Save face")` calls, duplicate `set_param2` site) that the agent flagged. |
| `src/triggers/streaming/hough.cxx` | 233, 286 | **Division by zero** when emitting `_TRIGGER_HOUGH_RING_FOUND_`: `hough_trigger_time[i] / hough_trigger_hits[i]` with no guard on `hough_trigger_hits[i] > 0`.  Guarded.  In practice MIST's `find_rings` should never return a ring with empty `hit_indices`, but a 0-divide would publish NaN into the trigger record. |
| `src/parallel_streaming_framer.cxx` | 276 | **Signedness trap** — `static_cast<size_t>(_current_spill)` when `_current_spill == -1` produces SIZE_MAX, then compared against vector size.  Current code was de-facto safe (SIZE_MAX > size always false → no correction applied), but the intent was opaque.  Added an explicit `_current_spill >= 0` guard. |
| `src/alcor_recotrackdata.cxx` | 34, 46 | Replaced `std::cerr` with `mist::logger::error` — inconsistent with the rest of the codebase and not captured by logging infrastructure. |
| `include/triggers/registry.h` | 70 | Same — replaced `std::cerr` with `mist::logger::warning`. |

### Stale comments / dated breadcrumbs purged

| File | What |
|---|---|
| `src/lightdata_writer.cxx` | (a) Garbled comment "Add the plot to dinamically determine the timing cuts > determine the highest bin..." — rewritten as a proper TODO with a DISCUSSION pointer.  (b) Six dated "removed 2026-05-26" / "Phase F (2026-05-27)" / "Phase G1 (2026-05-27)" history-of-removal breadcrumbs deleted; the surrounding facts retained where useful. |
| `src/recodata_writer.cxx` | "Smallest-r channel diagnostic removed / PDU 99 phantom-position investigation" block deleted.  The Mapping fix is documented elsewhere. |
| `src/alcor_finedata.cxx` | Garbled `TODO: merge with alcor data, no sense to have this overhead << it no makes perfect sense, data compression` — rewritten as two clean DISCUSSION pointers. |
| `src/triggers/streaming/hough.cxx` + `include/triggers/streaming/hough.h` | "No `fit_circle` here" lengthy historical note + the dead `ring_X/Y/R_first/_second` struct fields (never referenced after the fit was moved to recodata).  Replaced with a brief factual note pointing at recodata's refinement path. |
| `include/writers/recodata.h` | Deleted unused `BTANA_CROSS_TALK_DEADTIME` macro (no callers anywhere in the repo). |
| `src/writers/pulser_calib_writer.cxx` + `include/writers/pulser_calib.h` | **Both top-of-file docs were severely stale** (described the rejected lightdata-coupled architecture, per-spill weighted-mean method, mean-subtraction gauge, etc.).  Rewritten to match the actual implementation: per-channel closed-form Cholesky of a 9-param normal equations system, regime-2 slip correction, fine-band ingest filter. |
| `include/parallel_streaming_framer.h` | `h_afterpulse_dt` range docstring said "Range 0..1024 cc" but the histogram is 1024 bins on [0, 32768] cc.  Corrected. |
| `conf/framer_conf.toml`, `conf/streaming.toml` | "Phase 2/4 of the streaming-trigger consolidation" historical breadcrumbs replaced with factual present-tense descriptions.  The streaming.toml comment claiming the Hough stage "refines each ring's centre with a least-squares `fit_circle`" was contradicted by the current code — fixed (refinement now happens in `recodata_writer`). |

### New design notes added to per-area DISCUSSION.md files

| Doc | What |
|---|---|
| **`include/writers/DISCUSSION.md`** (new) | Three open items for the pulser pipeline: (1) the ±0.5 cc satellites in published b — origin unknown, possibly coarse-edge quantisation artefacts, possibly real sub-cc-granularity slip; (2) regime-2 slip detection uses `raw mod T` which is sensitive to coarse-edge quantisation and may be over-snapping; (3) the fine-band ingest filter is a pragmatic band-aid — the robust answer is for the fit to identify these as outliers (IRLS / M-estimator).  Also records the regime-1 removal rationale. |
| `include/triggers/DISCUSSION.md` | §3 hardcoded `[0, 99]` trigger index allocation tracked in two places (`events.h` macros + `config.cxx` array bound).  §4 implicit `time_window_ns` coupling between streaming score and Hough stages. |

### New feature implemented (per user request — task #37)

- `pulser_calib_writer` now discards hits whose `fine` is outside `[fine_min_valid, fine_max_valid]` (default `[20, 160]`) at FIFO ingest.  Knobs added to `CalibConfigStruct`, surfaced in `conf/calib/calibration_conf.toml`, snapshotted into the QA `Config/` directory, and the rejected count is written to `RunSummary/hits_rejected_fine_out_of_band`.  Tracked as a design item in `include/writers/DISCUSSION.md` for follow-up (fit-side outlier rejection is the robust answer).

### Items flagged by agents but NOT actioned (kept as low-priority context)

These are real observations but either too risky to fix blind, well-understood and intentional, or out of sweep scope:

- **Magic numbers in `lightdata_writer.cxx`** — `kTimingChip0AliveChannels=32` / `kTimingChip1AliveChannels=31` (line 41–42, campaign-specific dead-channel layout), `kDeltaTimingCenter/Window/Sigma` (line 51–53, timing calibration), `kArcDistBinsPerSide=30` (line 474), `window_size=50.0ns` for non-ring triggers (line 1015), `-100 ns` sideband offset (line 1028).  All intentional per surrounding comments.  Add config knobs only when a campaign demands re-tuning.
- **Magic numbers in `recodata_writer.cxx`** — `kCentreXyHalfRangeMm=25.f`, hit-position null-zone `< 5 mm`, radial fit acceptance bands `±5/±10 mm`.  Same story.
- **Magic detector geometry in `alcor_recotrackdata.h`** (lines 305–316) — plane distances and pixel resolutions.  Will need to move to config when the campaign changes; not now.
- **`AlcorFinedata::generate_calibration` is known-broken** — superseded for pulser runs by `pulser_calib_writer`.  Tracked.
- **`Mapping::pdu_rotation[pdu]` unchecked map access** (`src/mapping.cxx:74`) — could segfault on a malformed mapping config.  Defensive guard would cost a `.contains()`.  Filed; not patched.
- **`include/triggers/streaming/DISCUSSION.md`'s "fit_circle audit"** is now moot (fit_circle removed from this stage).  Worth a follow-up pass to retire the relevant § entries when the user is back; left untouched today because the DISCUSSION is a community-facing reference and the rewrite needs review.

### Verification

After all changes: `cmake --build` clean; `pulser_calib_writer Data 20260527-073111 --force-rebuild` runs to completion with the same headline numbers as the pre-sweep run (`8212` published entries, intra-channel pair |Δ| median `0.4984 cc`, `99.7005%` filter retention).  No numerical regression from the rename / comment / bug-fix pass.

---

## Triage taxonomy

From this point forward, every backlog item — bug report, design idea,
deferred feature, refactor candidate — is filed under a single shared
taxonomy so the queue stays sortable, filterable, and free of
ambiguity.  The taxonomy lives here (top-level DISCUSSION) because it
applies to **every** subdirectory's local DISCUSSION + every per-file
comment that flags work to do.

### Categories

Two top-level groupings, three severities each.

**Fixing** — something is wrong and the *intent* is to restore the
already-designed behaviour.  No new functionality.

| Severity | Name | Definition |
|---|---|---|
| 1 | **Bug** | Narrowly scoped, doesn't hinder reco + analysis.  Typo, wrong axis range, wrong units, harmless log line. |
| 2 | **Liability** | Significant but localised, or mild but global.  Could hinder reco + analysis or part of it (silent data clipping, subtle init order, sign error in a side metric). |
| 3 | **Vulnerability** | Endangers the whole workflow.  Random event discard, UB in a hot spot, divide-by-zero in a non-guarded path, data corruption on disk. |

**Adding** — the design itself is changing.  New functionality, new
contract, or removal of old behaviour.

| Severity | Name | Definition |
|---|---|---|
| 1 | **Patch** | Quick, surgical, contained.  One config knob, one CLI flag, one helper added. |
| 2 | **Feature** | Medium impact, multiple files involved.  A new tab on the dashboard, a new QA histogram set, a new fit method. |
| 3 | **Schema** | Strong impact — reshapes a contract (file format, TOML key layout, TDirectory tree, published function signature, pipeline stage).  Downstream consumers may break. |

### Rating axes

| Axis | Definition | Formula | Range |
|---|---|---|---|
| **IMPACT** | Operator-facing value when fixed / landed.  1 = cosmetic / docs / UX polish; 2 = affects one writer output, one analysis step, one tab; 3 = affects the analysis chain end-to-end, the database, or every shifter. | categorical | 1 · 2 · 3 |
| **FILES (F)** | How many distinct files the change touches.  Captures coordination / review surface, saturates because beyond ~10 files the marginal pain levels off. | `6 · [File / (File + 1) − 0.5]` | [0, 3] for File ≥ 1 |
| **LOC (L)** | Total lines edited / added / deleted, log scale (one decade = one point of pain), clamped because beyond ~1000 LOC the absolute count stops mattering for ranking. | `min( log₁₀(LOC + 1),  3 )` | [0, 3] for LOC ≥ 1 |

### Filter tags (do not enter Priority)

| Tag | Values |
|---|---|
| **SCOPE** | `NOW` (this shift) · `CAMPAIGN` (before next beam test) · `LATER` (background) |
| **STATUS** | `READY` (path is known, can code) · `INVESTIGATE` (need to confirm hypothesis, measure, or decide first) |
| **DOMAIN** | `C++` (writers, headers, ROOT helpers) · `Dash` (qa_quicklook, scripts) · `Macro` (macros/) · `Conf` (conf/, run-lists/) · `Doc` (*.md, Doxygen) · `CI` (.github/, scripts/check_*.sh) |

### Priority formula

For **READY** items only:

    Priority = (Severity × Impact) / max(F + L, 1)            ∈ [≈0.17, 9]

- Multiplicative numerator (`Sev × Imp`) so the worst items
  *compound* rather than just *rank* above the rest — a Vulnerability
  with chain-wide impact (9) towers over an isolated Bug (1) by 9×,
  matching the operator's gut sense of "drop everything and fix
  this."
- Additive denominator (`F + L`) so a cheap fix gets a clean
  multiplicative boost.
- `max(F + L, 1)` floor pins the maximum priority at exactly 9 (Sev =
  3, Imp = 3, smallest possible cost) instead of letting tiny-cost
  critical fixes inflate past the rest of the scale.

For **INVESTIGATE** items: no Priority computed.  They live in a
**Purgatory** section, sorted by `Sev × Imp` (range 1–9, "value if it
pans out").  Once an INVESTIGATE item graduates to READY (someone
estimated F + L), it moves to the main backlog and gets a real
Priority.

### Tie breaking

With continuous F and L scores, exact ties on Priority are vanishingly
unlikely (you'd need identical integer Files *and* identical LOC to
several decimals).  If one occurs, break by SCOPE: `NOW > CAMPAIGN >
LATER`.  Expected to be theoretical only.

### Compact tag line

Every triaged item carries one greppable header:

```
[<TYPE> · F<f> L<l> I<i> · <SCOPE> · <STATUS> · <DOMAIN> · P <p>]  <short title>
```

Examples:

```
[Vuln · F1.0 L1.49 I3 · NOW · READY · C++ · P 2.58]    pulser_calib: anchor channel auto-detect
[Bug  · F0.0 L0.30 I1 · LATER · READY · Doc · P 0.77]   README: stray Oxford comma
[Schema · F2.5 L3.00 I2 · LATER · INVEST · C++ · P  ?]  D-08 round 2: AlcorRecodata two-layer migration
[Feature · F0.0 L2.40 I2 · CAMPAIGN · READY · Dash · P 1.18]  qa: per-field audit history dialog
```

The `P x.xx` suffix is computed once at filing time so a grep + sort
gives a backlog ordering without recomputing the formula every read.
INVESTIGATE items show `P ?` deliberately — anything else would lie
about the cost estimate not existing yet.

### Workflow

1. **Filing.**  Anyone notices something → tag with `TYPE`,
   `IMPACT`, `SCOPE`, `STATUS`, `DOMAIN`.  If READY, estimate F + LOC
   and compute Priority.  If INVESTIGATE, leave them as `?` and drop
   into Purgatory.
2. **Sorting.**  Main backlog sorted by Priority descending.
   Purgatory sorted by `Sev × Imp` descending.
3. **Filtering.**  Operator picks a session focus by SCOPE (e.g.,
   "everything NOW") or DOMAIN (e.g., "C++ only this morning") and
   walks down the sorted list.
4. **Graduating.**  An INVESTIGATE item moves to READY when someone
   estimates its cost — at which point it gets a Priority and joins
   the main backlog at its earned position.
5. **Dropping.**  Items can be explicitly **DROPPED** with a one-line
   rationale (kept visible so a future reader doesn't reopen them).

### Why this scheme

- **The fix-vs-add split** maps to the operational question "is this
  paying down debt or building new?" — directly the planning
  question for a shift / a week / a campaign.
- **Three severities** is enough granularity for triage without
  inviting bucket-boundary debates.
- **Continuous F + LOC** kills "is it 5 files or 6?" arguments.
- **Sev × Impact** in the numerator means the worst items rank
  *exponentially* above the rest, not just *linearly* — matching the
  operator's gut sense.
- **INVESTIGATE → Purgatory** is honest about cost uncertainty.  A
  faked Priority on an unscoped item misleads planning; "no priority,
  estimate before queuing" is correct.
- **Filter tags** are filters, not scores — they keep prioritization
  one-dimensional while still letting the operator focus.

### Worked examples on real items

| Item | TYPE | Sev | Imp | Files | F | LOC | L | Priority |
|---|---|---|---|---|---|---|---|---|
| pulser_calib `b` ±0.5 cc satellites | Liability | 2 | 3 | 1 | 0.00 | 30 | 1.49 | (2·3)/1.49 ≈ **4.03** |
| README typo | Bug | 1 | 1 | 1 | 0.00 | 1 | 0.30 | 1/max(0.30,1) = **1.00** |
| D-08 round 2 (AlcorRecodata) | Schema | 3 | 2 | 11 | 2.50 | 1000 | 3.00 | (3·2)/5.50 ≈ **1.09** |
| Show-history UI (qa_quicklook) | Feature | 2 | 2 | 1 | 0.00 | 250 | 2.40 | (2·2)/2.40 ≈ **1.67** |
| Run-info edit cascade prompt | Feature | 2 | 2 | 2 | 1.00 | 200 | 2.30 | (2·2)/3.30 ≈ **1.21** |

