#!/bin/bash
# =============================================================================
#  run_all.sh — runs run.sh for each conf (from folder, list, or generated variants)
#
#  Modes:
#    --conf-dir <dir>     iterate over all .conf files in that folder
#    --confs f1 f2 ...    explicit list of conf files
#    --variants <base>    generate track-cut variants from <base>, run them, then delete
#
#  Examples:
#    ./run_all.sh --data /data --run run123 --variants conf/1track.conf --analysis
#    ./run_all.sh --data /data --run run123 --conf-dir conf/ --all
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# ---------------------------------------------------------------------------
#  Helper: set/override a key in a conf file
# ---------------------------------------------------------------------------
set_conf() {
    local file="$1" key="$2" val="$3"
    if grep -qE "^[[:space:]]*${key}[[:space:]]*=" "$file"; then
        sed -i "s|^\([[:space:]]*\)${key}\([[:space:]]*\)=.*|\1${key}\2= ${val}|" "$file"
    else
        echo "${key} = ${val}" >> "$file"
    fi
}

usage() {
    echo "Usage: $0 --data <folder> --run <run_id> [options] [macro flags]"
    echo ""
    echo "Required:"
    echo "  --data <folder>     Cartella dati"
    echo "  --run  <run_id>     Run ID"
    echo ""
    echo "Conf mode (choose one):"
    echo "  --conf-dir <dir>    Itera su tutti i .conf nella cartella (default: ./conf)"
    echo "  --confs f1 [f2...]  Lista esplicita di conf"
    echo "  --variants <base>   Genera varianti track-cut da <base>, poi le cancella"
    echo ""
    echo "Options:"
    echo "  --output <dir>      Base output dir (default: <repo>/plots/<run>/<datetime>)"
    echo ""
    echo "Macro flags (passed to run.sh, default: --analysis):"
    echo "  --analysis  --draw  --display  --timing  --mult-windows  --all"
    echo ""
    echo "Examples:"
    echo "  $0 --data /data --run 042 --variants conf/1track.conf --analysis"
    echo "  $0 --data /data --run 042 --conf-dir conf/ --all"
    exit 1
}

# ---------------------------------------------------------------------------
#  Argument parsing
# ---------------------------------------------------------------------------
DATA_FOLDER=""
RUN_ID=""
CONF_DIR=""
CONF_LIST=()
VARIANTS_BASE=""
BASE_OUTPUT=""
MACRO_FLAGS=()

while [ "$#" -gt 0 ]; do
    case $1 in
        --data)      DATA_FOLDER="$2";    shift 2 ;;
        --run)       RUN_ID="$2";         shift 2 ;;
        --conf-dir)  CONF_DIR="$2";       shift 2 ;;
        --variants)  VARIANTS_BASE="$2";  shift 2 ;;
        --output)    BASE_OUTPUT="$2";    shift 2 ;;
        --confs)
            shift
            while [ "$#" -gt 0 ] && [[ "$1" != --* ]]; do
                CONF_LIST+=("$1"); shift
            done ;;
        --analysis|--draw|--draw-optional|--display|--timing|--mult-windows|--all)
            MACRO_FLAGS+=("$1"); shift ;;
        -h|--help) usage ;;
        *) echo "Opzione sconosciuta: $1"; usage ;;
    esac
done

if [ -z "$DATA_FOLDER" ] || [ -z "$RUN_ID" ]; then
    echo "Error: --data and --run are required"; usage
fi

[ ${#MACRO_FLAGS[@]} -eq 0 ] && MACRO_FLAGS=(--analysis)

DATETIME="$(date +%Y%m%d_%H%M%S)"
[ -z "$BASE_OUTPUT" ] && BASE_OUTPUT="$REPO_ROOT/plots/$RUN_ID/$DATETIME"

CONF_FILES=()   # file paths
CONF_NAMES=()   # human-readable names (= output subfolder)
TEMP_FILES=()   # temporary conf files, deleted after run

# ---------------------------------------------------------------------------
#  Mode --variants: generate temporary conf files from a base conf
# ---------------------------------------------------------------------------
if [ -n "$VARIANTS_BASE" ]; then
    [ ! -f "$VARIANTS_BASE" ] && { echo "Error: base conf '$VARIANTS_BASE' not found"; exit 1; }

    # ---- track variants ----
    # format: "name|key1 val1|key2 val2|..."|key1 val1|key2 val2|..."|key1 val1|key2 val2|..."
    TRACK_VARS=(
        "1track_chi2_5|require_exact_ntracks 1|require_min_ntracks 1|require_max_ntracks 0|apply_chi2_cut true|chi2_max 5.0|use_best_track_only true"
        "1track_nochi2|require_exact_ntracks 1|require_min_ntracks 1|require_max_ntracks 0|apply_chi2_cut false|use_best_track_only true"
        "2track_chi2_5|require_exact_ntracks 2|require_min_ntracks 2|require_max_ntracks 2|apply_chi2_cut true|chi2_max 5.0|use_best_track_only true"
        "multi_chi2_5|require_exact_ntracks 0|require_min_ntracks 2|require_max_ntracks 0|apply_chi2_cut true|chi2_max 5.0|use_best_track_only true"
        "any_chi2_5|require_exact_ntracks 0|require_min_ntracks 1|require_max_ntracks 0|apply_chi2_cut true|chi2_max 5.0|use_best_track_only false"
    )

    # ---- drich variants ----
    DRICH_VARS=(
        "drich_inside|plane2 DRICH|side2 INSIDE|cutg2 drich_largo"
        "drich_outside|plane2 DRICH|side2 OUTSIDE|cutg2 drich_largo"
    )

    for TVAR in "${TRACK_VARS[@]}"; do
        TNAME="${TVAR%%|*}"
        TKEYS="${TVAR#*|}"

        for DVAR in "${DRICH_VARS[@]}"; do
            DNAME="${DVAR%%|*}"
            DKEYS="${DVAR#*|}"

            VARNAME="${TNAME}_${DNAME}"
            TMPFILE="/tmp/rall_${VARNAME}_$$.conf"

            cp "$VARIANTS_BASE" "$TMPFILE"

            # common overrides (applied to all variants)
            set_conf "$TMPFILE" time_cut_min       -50
            set_conf "$TMPFILE" time_cut_max       -20
            set_conf "$TMPFILE" apply_geometric_track_selection true
            set_conf "$TMPFILE" plane1             SCINT
            set_conf "$TMPFILE" side1              INSIDE
            set_conf "$TMPFILE" cutg1              scint_largo

            # apply track-variant keys (pipe-separated "key val" pairs)
            IFS='|' read -ra KV_TRACK <<< "$TKEYS"
            for KV in "${KV_TRACK[@]}"; do
                K="${KV%% *}"; V="${KV#* }"
                set_conf "$TMPFILE" "$K" "$V"
            done

            # apply dRICH-variant keys
            IFS='|' read -ra KV_DRICH <<< "$DKEYS"
            for KV in "${KV_DRICH[@]}"; do
                K="${KV%% *}"; V="${KV#* }"
                set_conf "$TMPFILE" "$K" "$V"
            done

            CONF_FILES+=("$TMPFILE")
            CONF_NAMES+=("$VARNAME")
            TEMP_FILES+=("$TMPFILE")
        done
    done

# ---------------------------------------------------------------------------
#  Mode --confs: explicit file list
# ---------------------------------------------------------------------------
elif [ ${#CONF_LIST[@]} -gt 0 ]; then
    for F in "${CONF_LIST[@]}"; do
        CONF_FILES+=("$F")
        CONF_NAMES+=("$(basename "$F" .conf)")
    done

# ---------------------------------------------------------------------------
#  Mode --conf-dir (default: ./conf)
# ---------------------------------------------------------------------------
else
    [ -z "$CONF_DIR" ] && CONF_DIR="$SCRIPT_DIR/conf"
    for F in "$CONF_DIR"/*.conf; do
        [ -f "$F" ] || continue
        CONF_FILES+=("$F")
        CONF_NAMES+=("$(basename "$F" .conf)")
    done
fi

if [ ${#CONF_FILES[@]} -eq 0 ]; then
    echo "Error: no conf files found"; exit 1
fi

# ---------------------------------------------------------------------------
#  Print batch summary
# ---------------------------------------------------------------------------
echo "========================================"
echo "Batch run:  $DATETIME"
echo "Data:       $DATA_FOLDER"
echo "Run ID:     $RUN_ID"
echo "Macros:     ${MACRO_FLAGS[*]}"
echo "Output:     $BASE_OUTPUT"
echo "N confs:    ${#CONF_FILES[@]}"
for i in "${!CONF_NAMES[@]}"; do
    echo "  [$((i+1))] ${CONF_NAMES[$i]}"
done
echo "========================================"

# ---------------------------------------------------------------------------
#  Run
# ---------------------------------------------------------------------------
OK=0; FAIL=0

for i in "${!CONF_FILES[@]}"; do
    CONF="${CONF_FILES[$i]}"
    NAME="${CONF_NAMES[$i]}"
    OUT_DIR="$BASE_OUTPUT/$NAME"

    echo ""
    echo ">>> [$((i+1))/${#CONF_FILES[@]}] $NAME"

    bash "$SCRIPT_DIR/run.sh" \
        --data   "$DATA_FOLDER" \
        --run    "$RUN_ID" \
        --conf   "$CONF" \
        --output "$OUT_DIR" \
        "${MACRO_FLAGS[@]}"

    if [ $? -eq 0 ]; then ((OK++)); else ((FAIL++)); fi
done

# clean up temporary conf files
for TMP in "${TEMP_FILES[@]}"; do
    rm -f "$TMP"
done

echo ""
echo "========================================"
echo "Batch done.  OK=$OK  FAIL=$FAIL"
echo "Output:      $BASE_OUTPUT"
echo "========================================"
