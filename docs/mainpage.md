\mainpage ePIC dRICH Beam Test Analysis

A C++/ROOT framework for processing and analysing ePIC dRICH beam-test data,
covering the full chain from raw ALCOR readout to reconstructed Cherenkov rings.

## Data levels

| Level | Class | Description |
|-------|-------|-------------|
| Raw | `alcor_data` | Direct ALCOR decoder output |
| Light | `alcor_lightdata` | Time-clustered, trigger-tagged hits |
| Reco | `alcor_recodata` | Position-mapped reconstructed hits |
| Track | `alcor_recotrackdata` | Track-matched hits from ALTAI telescope |

## Key components

- @ref mapping — Channel-to-position lookup chain (device/chip/EO → x,y mm)
- @ref alcor_data_streamer — Sequential ROOT TTree reader
- @ref run_info — TOML-based run metadata and run-list database
- @ref readout_config_list — Readout role assignment and conflict resolution

## Repository

Source: https://github.com/Nikolajal/epic-drich-beam-test-analysis