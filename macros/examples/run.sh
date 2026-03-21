#!/bin/bash

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <data_folder> <run_id>"
    exit 1
fi

DATA_FOLDER="$1"
RUN_ID="$2"

echo "Running ROOT analyses..."
echo "Data folder: $DATA_FOLDER"
echo "Run ID: $RUN_ID"

echo "-> Running ringtrack_analysis"
root -l -b -q "ringtrack_analysis.cpp(\"$DATA_FOLDER\", \"$RUN_ID\")"
# root -l -b -q "ringbug.cpp(\"$DATA_FOLDER\", \"$RUN_ID\")"

# echo "-> Running ringtrack_spr"
# root -l -b -q "ringtrack_spr.cpp(\"$DATA_FOLDER\", \"$RUN_ID\")"

echo "-> Running ringtrack_draw"
root -l -b -q "ringtrack_draw.cpp(\"$DATA_FOLDER\", \"$RUN_ID\")"

echo "All analyses completed."