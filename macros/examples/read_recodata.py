#!/usr/bin/env python3
import sys
import uproot

if len(sys.argv) < 3:
    sys.exit("Usage: read_recodata.py <data_folder> <run_tag>")

data_folder, run_tag = sys.argv[1], sys.argv[2]
INFILE = f"{data_folder}/{run_tag}/recodata.root"

with uproot.open(INFILE) as f:
    tree = f["recodata"]
    print(f"Entries: {tree.num_entries}")

    arrays = tree.arrays(entry_stop=10)

for iev in range(len(arrays)):
    n_hits = len(arrays["recodata.rollover"][iev])
    n_triggers = len(arrays["triggers.index"][iev])
    print(f"\n--- Entry {iev}  ({n_hits} hits, {n_triggers} triggers) ---")

    for ih in range(n_hits):
        print(
            f"  hit[{ih:3d}]"
            f"  x={arrays['recodata.hit_x'][iev][ih]:7.2f}"
            f"  y={arrays['recodata.hit_y'][iev][ih]:7.2f}"
            f"  coarse={arrays['recodata.coarse'][iev][ih]}"
            f"  fine={arrays['recodata.fine'][iev][ih]}"
            f"  gidx={arrays['recodata.global_index'][iev][ih]}"
            f"  mask=0x{arrays['recodata.hit_mask'][iev][ih]:08x}"
        )
    for it in range(n_triggers):
        print(
            f"  trg[{it}]"
            f"  index={arrays['triggers.index'][iev][it]}"
            f"  coarse={arrays['triggers.coarse'][iev][it]}"
            f"  fine_time={arrays['triggers.fine_time'][iev][it]:.2f} ns"
        )
