#!/usr/bin/env bash
#
# Download a test-beam run from eicserv01 into the local Data/ tree.
#
# Usage:  scripts/download_run.sh <run-id>
#   e.g.  scripts/download_run.sh 20260603-140749
#
# Pulls everything except the bulky online-processing tree.

set -euo pipefail

RUN="${1:?usage: $(basename "$0") <run-id>   e.g. 20260603-140749}"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE="eic@eicserv01:/data/2026-testbeam/preparation/physics/${RUN}/"
LOCAL="${REPO}/Data/${RUN}/"

mkdir -p "${LOCAL}"
echo "rsync ${REMOTE} -> ${LOCAL}"
#  Download ONLY *.root files: skip the online-processing tree, recurse into
#  every other directory (--include='*/'), keep *.root, drop everything else.
rsync -av \
    --exclude='process-online/' \
    --include='*/' \
    --include='*.root' \
    --exclude='*' \
    "${REMOTE}" "${LOCAL}"
