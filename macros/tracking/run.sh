#!/bin/bash

usage() {
    echo "Usage: $0 <data_folder> <run_id> [conf_file] [options]"
    echo ""
    echo "Options:"
    echo "  --analysis        Run ringtrack_analysis (default if no options given)"
    echo "  --draw            Run ringtrack_draw"
    echo "  --draw-optional   Run ringtrack_draw_optional"
    echo "  --display         Run ringtrack_draw_display"
    echo "  --all             Run all four"
    echo ""
    echo "Examples:"
    echo "  $0 /data run123                              # runs only analysis"
    echo "  $0 /data run123 ringtrack.conf --all         # runs everything"
    echo "  $0 /data run123 ringtrack.conf --draw --display"
    exit 1
}

if [ "$#" -lt 2 ]; then
    usage
fi

DATA_FOLDER="$1"
RUN_ID="$2"

# terzo argomento: se non inizia con -- è il conf, altrimenti default
if [ "$#" -ge 3 ] && [[ "$3" != --* ]]; then
    CONF="$3"
    shift 3
else
    CONF="ringtrack.conf"
    shift 2
fi

# parse options
RUN_ANALYSIS=false
RUN_DRAW=false
RUN_DRAW_OPTIONAL=false
RUN_DISPLAY=false

if [ "$#" -eq 0 ]; then
    # nessuna opzione → solo analysis
    RUN_ANALYSIS=true
else
    for arg in "$@"; do
        case $arg in
            --analysis)       RUN_ANALYSIS=true ;;
            --draw)           RUN_DRAW=true ;;
            --draw-optional)  RUN_DRAW_OPTIONAL=true ;;
            --display)        RUN_DISPLAY=true ;;
            --all)
                RUN_ANALYSIS=true
                RUN_DRAW=true
                RUN_DRAW_OPTIONAL=true
                RUN_DISPLAY=true
                ;;
            *)
                echo "Unknown option: $arg"
                usage
                ;;
        esac
    done
fi

echo "========================================"
echo "Data folder: $DATA_FOLDER"
echo "Run ID:      $RUN_ID"
echo "Config:      $CONF"
echo "========================================"

if $RUN_ANALYSIS; then
    echo "-> Running ringtrack_analysis"
    root -l -b -q "ringtrack_analysis.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\")"
    # root -l -b -q "ringbug.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\")"
fi

if $RUN_DRAW; then
    echo "-> Running ringtrack_draw"
    root -l -b -q "ringtrack_draw.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\")"
fi

if $RUN_DRAW_OPTIONAL; then
    echo "-> Running ringtrack_draw_optional"
    root -l -b -q "ringtrack_draw_optional.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\")"
fi

if $RUN_DISPLAY; then
    echo "-> Running ringtrack_draw_display"
    root -l -b -q "ringtrack_draw_display.cpp(\"$DATA_FOLDER\", \"$RUN_ID\", \"$CONF\")"
fi

echo "========================================"
echo "All done."
echo "========================================"