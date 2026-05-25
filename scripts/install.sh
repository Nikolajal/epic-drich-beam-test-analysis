#!/bin/bash
set -e

# Get project root (one directory above scripts/)
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

#clean build directory
[ -d "$PROJECT_ROOT/build" ] && rm -rf "$PROJECT_ROOT/build"

# Forward FETCHCONTENT_SOURCE_DIR_MIST as a -D when the caller has it set
# in the environment.  CMake < 3.24 does not promote that env var to a
# cache variable automatically; on those CMakes the override is silently
# dropped at configure time unless passed via -D.  Default usage
# (`./scripts/install.sh`) leaves the variable unset and FetchContent
# pulls the upstream pin in CMakeLists.txt.
CMAKE_EXTRA_ARGS=()
if [ -n "${FETCHCONTENT_SOURCE_DIR_MIST:-}" ]; then
    CMAKE_EXTRA_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_MIST=${FETCHCONTENT_SOURCE_DIR_MIST}")
    echo "[install.sh] Using local MIST source: ${FETCHCONTENT_SOURCE_DIR_MIST}"
fi

cmake -S "$PROJECT_ROOT" -B "$BUILD_DIR"    \
    -DCMAKE_BUILD_TYPE=Release              \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local"   \
    "${CMAKE_EXTRA_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel
cmake --install "$BUILD_DIR"