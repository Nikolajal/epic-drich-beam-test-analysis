# `include/writers/` — top-level writer entry points

Four independent writer binaries land their public function signatures
here.  Each header declares a single `btana::<name>_writer(...)` free
function the corresponding executable in `macros/utilities/` calls.
There is **no umbrella header** — the writers don't share an
interface and a `writers.h` re-export would carry zero value.

| Header | Binary | Reads | Writes |
|---|---|---|---|
| [`pulser_calib.h`](pulser_calib.h) | `pulser_calib_writer` | ALCOR FIFO files + `conf/calib/calibration_conf.toml` | `<run>/fine_calib.toml` + `<run>/pulser_calib_qa.root` |
| [`lightdata.h`](lightdata.h)   | `lightdata_writer`   | ALCOR FIFOs + `fine_calib.toml` + streaming + trigger + readout configs | `<run>/lightdata.root` |
| [`recodata.h`](recodata.h)     | `recodata_writer`    | `lightdata.root` + `fine_calib.toml` + recodata config | `<run>/recodata.root` |
| [`recotrackdata.h`](recotrackdata.h) | `recotrackdata_writer` | `recodata.root` + ALTAI `tracks.txt` | `<run>/recotrackdata.root` |

## Sub-directories

Two writers have grown a sibling directory of small helper headers
(the other two stay single-file):

- [`lightdata/`](lightdata) — per-trigger QA helpers
- [`recodata/`](recodata) — radial-fit + σ(N) extraction (Phase 1 of the recodata modularisation; see top-level `DISCUSSION.md`)

Implementation files live under `src/` (or `src/writers/` for the
sub-staged writers) — keep this directory header-only so consumers
build fast.

## Public contract

Each writer is a free function (not a class) so the binary's `main()`
stays trivial — parse CLI, call the function, return.  The
calibration writer's signature carries a fistful of overrides:

```cpp
void pulser_calib_writer(
    const std::string &data_repository,
    const std::string &run_name,
    const std::string &calib_config_file = "conf/calib/calibration_conf.toml",
    bool force_rebuild = false,
    int max_spill = -1,
    int anchor_device_override = -1,
    int anchor_chip_override = -1,
    int anchor_eo_channel_override = -1,
    double pulser_period_cc_override = -1.0);
```

Sentinel `-1` / `-1.0` keeps cfg-from-TOML behaviour; non-sentinel
values override per-launch (operator-driven from the dashboard's
Calibration card without touching `calibration_conf.toml`).

## Conventions

1. **ROOT-bound wrappers are non-copyable and non-movable.**  Any
   wrapper that binds branch pointers into a `TTree` (`AlcorRecodata`,
   `AlcorRecotrackdata`, and any future sibling) MUST declare its copy
   + move ctors and assignment operators `= delete` **on the base
   class**.  Copying such a wrapper double-binds the same branches; the
   second destructor then corrupts the tree.  The deletion is an
   invariant of the type — not a per-call-site precaution — so it lives
   at the base where a subclass or a downstream signature can't forget
   it.  Reader/writer intent is additionally carried by a runtime
   `read_only_` flag (set by the reader factory) whose mutators
   `assert` against accidental writes through a read view.  See
   top-level `DISCUSSION.md` § D-08 for the decision record.

## Design discussion

Open questions + active hypotheses for each writer live in
[`DISCUSSION.md`](DISCUSSION.md) next to this README.  The top-level
[`../../DISCUSSION.md`](../../DISCUSSION.md) hub indexes that file
alongside the other satellites.
