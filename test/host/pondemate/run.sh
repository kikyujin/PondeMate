#!/bin/sh
# Builds and runs the PuzzleCore host tests against the PondeMate puzzle sets.
#
#   test/host/pondemate/run.sh [sets_dir]    (default ~/work/pondemate-data/sets)
#
# PuzzleCore and ChessCore are plain C++, so no device and no PlatformIO needed.
set -e
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../../.. && pwd)"
SETS_DIR="${1:-$HOME/work/pondemate-data/sets}"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-pondemate-tests-$(echo "$REPO_ROOT" | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  -I"$REPO_ROOT/lib/ChessCore" -I"$REPO_ROOT/src/activities/puzzle" \
  "$REPO_ROOT/lib/ChessCore/ChessCore.cpp" "$REPO_ROOT/src/activities/puzzle/PuzzleCore.cpp" test_puzzle_core.cpp \
  -o "$BUILD_DIR/test_puzzle_core"
"$BUILD_DIR/test_puzzle_core" "$SETS_DIR"
