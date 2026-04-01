#!/bin/bash
# Start the beam test analysis web server.
# Installs dependencies if needed, then launches uvicorn on port 8000.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Install Python dependencies if missing
python3 -c "import fastapi, uvicorn" 2>/dev/null || {
    echo "Installing Python dependencies..."
    pip3 install --user -r "$SCRIPT_DIR/requirements.txt"
}

# Ensure ROOT libs are in path
ROOT_EXEC="$(which root 2>/dev/null)"
if [ -n "$ROOT_EXEC" ]; then
    ROOT_LIB="$(dirname "$(dirname "$(realpath "$ROOT_EXEC")")")/lib"
    [ -d "$ROOT_LIB" ] && export LD_LIBRARY_PATH="$ROOT_LIB:${LD_LIBRARY_PATH:-}"
fi

echo "Starting server at http://0.0.0.0:8000"
cd "$SCRIPT_DIR"
exec python3 -m uvicorn server:app --host 0.0.0.0 --port 8000
