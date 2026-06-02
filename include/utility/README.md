# include/utility/ — focused helper headers

Small, mostly header-only utilities consumed across the framework.
Group them here so the public top-level umbrella stays readable, and
so consumers can pick the narrowest header they need.

The umbrella header [`include/utility.h`](../utility.h) re-exports
every file in this directory.  **New code should `#include
"utility/<topic>.h"` directly** — tighter compile-time deps, cleaner
intent.  The umbrella is kept as a single-include convenience for
legacy call sites only.

## File-by-file

| File | Purpose | Header-only? |
|---|---|---|
| [`bit_ops.h`](bit_ops.h) | 32-bit mask encode/decode for `GlobalIndex` packing | yes |
| [`global_index.h`](global_index.h) | Strongly-typed packed `GlobalIndex` value type — `(device, FIFO, chip, channel, TDC)` with validity bit, dual TDC-level / channel-level views, and named factories from legacy integer ids | yes |
| [`toml_utils.h`](toml_utils.h) | Cutoff-aware TOML loader for the config readers | yes |
| [`conf_path.h`](conf_path.h) | Path resolution for the writers' mode flags (`--QA`, `--calib`) | yes |
| [`config_reader.h`](config_reader.h) | Public API for every TOML-backed configuration struct (`RunInfo`, `ReadoutConfigList`, `CalibConfigStruct`, `StreamingTriggerConfigStruct`, `StreamingHoughConfigStruct`, `RecoDataConfigStruct`, …).  Heavy parsing lives in [`src/config_reader.cxx`](../../src/config_reader.cxx). | header decl + .cxx impl |
| [`circle_fit.h`](circle_fit.h) | Least-squares circle fit minimising radial residuals.  Used by `recodata_writer`'s ring refinement.  Open audit items tracked in [`include/writers/DISCUSSION.md`](../writers/DISCUSSION.md). | yes |
| [`ring_model.h`](ring_model.h) | Analytical Cherenkov-ring signal model and histogram-based ring fitter | yes |
| [`radiator_efficiency.h`](radiator_efficiency.h) | Geometric coverage map + radial efficiency helpers for the dRICH radiator analysis | yes |
| [`root_io.h`](root_io.h) | `TFile` open-or-build helper with automatic schema-version negotiation | yes |
| [`root_draw.h`](root_draw.h) | Canvas-drawing helpers (currently `draw_circle`) | yes |
| [`root_hist.h`](root_hist.h) | `RootHist<T>` — RAII wrapper for owning `TH*` objects, closes the §B-1 leak-on-exception trap from the post-migration audit | yes |

## Conventions

- **Header-only first.**  Default to inline / templated implementations
  so adding a util doesn't require touching the CMake source list.
  Exceptions are explicitly the `*_reader` family, where the parser
  logic is heavy enough that paying the link cost is preferable to
  bloating includes.
- **Add to the umbrella.**  When you add a new file under `utility/`,
  also `#include` it from
  [`include/utility.h`](../utility.h) and add a row to its table.
  The umbrella is the curated index — keep it complete.
- **Naming.**  `snake_case.h`, one topic per file.  No multi-topic
  catch-all headers.  See
  [`docs/coding_conventions.md`](../../docs/coding_conventions.md).
- **No global mutable state.**  The pre-Phase-5 `_global_rd_` /
  `_global_gen_` Mersenne-Twister sat in this directory and bit us
  with cross-test interference; it's gone.  Use a `thread_local`
  `mist::Rnd` instead (`<mist/rnd.h>`).  See the historical note in
  [`include/utility.h`](../utility.h).

## Related folders

- [`include/triggers/`](../triggers/) — trigger-subsystem types and
  helpers (their own umbrella header `triggers.h`).
- [`include/writers/`](../writers/) — pipeline-stage sink classes (no
  umbrella by design — see the rationale in the top-level
  `DISCUSSION.md`'s "Attention points" table).
