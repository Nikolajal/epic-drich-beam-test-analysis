#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Ensure the real ROOT libraries are in LD_LIBRARY_PATH
# (needed when the sandbox environment overrides ROOTSYS)
_ROOT_EXEC="$(which root 2>/dev/null)"
if [ -n "$_ROOT_EXEC" ]; then
    _ROOT_LIB="$(dirname "$(dirname "$(realpath "$_ROOT_EXEC")")")/lib"
    [ -d "$_ROOT_LIB" ] && export LD_LIBRARY_PATH="$_ROOT_LIB:${LD_LIBRARY_PATH:-}"
fi
unset _ROOT_EXEC _ROOT_LIB

usage() {
    echo "Usage: $0 --data <folder> --run <run_id> [options]"
    echo ""
    echo "Required:"
    echo "  --data <folder>   Data folder"
    echo "  --run <run_id>    Run ID"
    echo ""
    echo "Options:"
    echo "  --conf <file>     Config file (default: ringtrack.conf)"
    echo "  --output <dir>    Output directory (default: <repo>/plots/<run_id>/<datetime>)"
    echo "  --analysis        Run ringtrack_analysis (default if no macro option given)"
    echo "  --draw            Run ringtrack_draw"
    echo "  --draw-optional   Run ringtrack_draw_optional"
    echo "  --display         Run ringtrack_draw_display"
    echo "  --timing          Run ringtrack_timing"
    echo "  --mult-windows    Run ringtrack_mult_windows"
    echo "  --noring          Run ringtrack_noring"
    echo "  --notrack         Run ringtrack_notrack (no-track event study)"
    echo "  --all             Run all macros"
    echo ""
    echo "Examples:"
    echo "  $0 --data /data --run run123"
    echo "  $0 --data /data --run run123 --conf ringtrack.conf --all"
    echo "  $0 --data /data --run run123 --conf ringtrack.conf --output /custom/output --draw --display"
    exit 1
}

if [ "$#" -lt 1 ]; then
    usage
fi

DATA_FOLDER=""
RUN_ID=""
CONF="ringtrack.conf"
OUTPUT_DIR=""
RUN_ANALYSIS=false
RUN_DRAW=false
RUN_DRAW_OPTIONAL=false
RUN_DISPLAY=false
RUN_TIMING=false
RUN_MULT_WINDOWS=false
RUN_NORING=false
RUN_NOTRACK=false
RUN_BEAM=false

while [ "$#" -gt 0 ]; do
    case $1 in
        --data)           DATA_FOLDER="$2"; shift 2 ;;
        --run)            RUN_ID="$2";      shift 2 ;;
        --conf)           CONF="$2";        shift 2 ;;
        --output)         OUTPUT_DIR="$2";  shift 2 ;;
        --analysis)       RUN_ANALYSIS=true;      shift ;;
        --draw)           RUN_DRAW=true;           shift ;;
        # --draw-optional: macro does not exist, kept for compatibility but no-op
        --draw-optional)  shift ;;
        --display)        RUN_DISPLAY=true;        shift ;;
        --timing)         RUN_TIMING=true;         shift ;;
        --mult-windows)   RUN_MULT_WINDOWS=true;   shift ;;
        --noring)         RUN_NORING=true;          shift ;;
        --notrack)        RUN_NOTRACK=true;        shift ;;
        --beam)           RUN_BEAM=true;           shift ;;
        --all)
            RUN_ANALYSIS=true
            RUN_DRAW=true
            RUN_DISPLAY=true
            RUN_TIMING=true
            RUN_MULT_WINDOWS=true
            RUN_NORING=true
            RUN_NOTRACK=true
            RUN_BEAM=true
            shift ;;
        *)
            echo "Unknown option: $1"
            usage ;;
    esac
done

if [ -z "$DATA_FOLDER" ] || [ -z "$RUN_ID" ]; then
    echo "Error: --data and --run are required"
    usage
fi

# default: run analysis only if no macro flag is given
if ! $RUN_ANALYSIS && ! $RUN_DRAW && ! $RUN_DISPLAY && ! $RUN_TIMING && ! $RUN_MULT_WINDOWS && ! $RUN_NORING && ! $RUN_NOTRACK && ! $RUN_BEAM; then
    RUN_ANALYSIS=true
fi

# output dir: repo/plots/run_id/datetime if not specified
if [ -z "$OUTPUT_DIR" ]; then
    DATETIME="$(date +%Y%m%d_%H%M%S)"
    OUTPUT_DIR="$REPO_ROOT/plots/$RUN_ID/$DATETIME"
fi
mkdir -p "$OUTPUT_DIR"

# copy the conf used for this run, then use the stable copy for all macros
cp "$CONF" "$OUTPUT_DIR/run.conf"
CONF="$OUTPUT_DIR/run.conf"

# log file with datetime in the output directory
LOG_FILE="$OUTPUT_DIR/run_${DATETIME}.log"
exec > >(tee "$LOG_FILE") 2>&1

echo "========================================"
echo "Data folder: $DATA_FOLDER"
echo "Run ID:      $RUN_ID"
echo "Config:      $CONF  →  $OUTPUT_DIR/run.conf"
echo "Output dir:  $OUTPUT_DIR"
echo "Log:         $LOG_FILE"
echo "========================================"

if $RUN_ANALYSIS; then
    echo "-> Running ringtrack_analysis"
    root -l -b -q "ringtrack_analysis.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\", \"$OUTPUT_DIR\")"
fi

if $RUN_DRAW; then
    echo "-> Running ringtrack_draw"
    root -l -b -q "ringtrack_draw.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\", \"$OUTPUT_DIR\")"
fi


if $RUN_DISPLAY; then
    echo "-> Running ringtrack_draw_display"
    root -l -b -q "ringtrack_draw_display.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\", \"$OUTPUT_DIR\")"
fi

if $RUN_TIMING; then
    echo "-> Running ringtrack_timing"
    root -l -b -q "ringtrack_timing.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\", \"$OUTPUT_DIR\")"
fi

if $RUN_MULT_WINDOWS; then
    echo "-> Running ringtrack_mult_windows"
    root -l -b -q "ringtrack_mult_windows.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\", \"$OUTPUT_DIR\")"
fi

if $RUN_NORING; then
    echo "-> Running ringtrack_noring"
    root -l -b -q "ringtrack_noring.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\", \"$OUTPUT_DIR\")"
fi

if $RUN_NOTRACK; then
    echo "-> Running ringtrack_notrack"
    root -l -b -q "ringtrack_notrack.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\", \"$OUTPUT_DIR\")"
fi

if $RUN_BEAM; then
    echo "-> Running ringtrack_beam"
    root -l -b -q "ringtrack_beam.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\", \"$OUTPUT_DIR\")"
fi

echo "========================================"
echo "All done. Output: $OUTPUT_DIR"
echo "========================================"
