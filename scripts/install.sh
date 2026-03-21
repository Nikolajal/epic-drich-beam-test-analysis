#!/bin/bash
set -e

# Get project root (one directory above scripts/)
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

#clean build directory
[ -d "$PROJECT_ROOT/build" ] && rm -rf "$PROJECT_ROOT/build"

cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR"    \
    -DCMAKE_BUILD_TYPE=Release              \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"   \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON    \
    -DCMAKE_CXX_STANDARD=17
cmake --build "$BUILD_DIR" --parallel
cmake --install "$BUILD_DIR"