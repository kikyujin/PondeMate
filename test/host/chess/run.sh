#!/bin/sh
# Builds and runs the ChessCore rules tests on the host. No device and no
# PlatformIO needed: ChessCore is freestanding C++17.
#
#   test/host/chess/run.sh
#
# test_chess.cpp is copied unchanged from CrossPlay and includes
# "../../src/apps_local/chess/ChessCore.h". A symlink in the build dir maps
# that path to lib/ChessCore so the test source stays untouched.
#
# -O2 matters here: the deeper perft cases are slow in an unoptimised build.
set -e
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../../.. && pwd)"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-chesscore-tests-$(echo "$REPO_ROOT" | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR/inc/a/b" "$BUILD_DIR/inc/src/apps_local"
ln -sfn "$REPO_ROOT/lib/ChessCore" "$BUILD_DIR/inc/src/apps_local/chess"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror -I"$BUILD_DIR/inc/a/b" \
  "$REPO_ROOT/lib/ChessCore/ChessCore.cpp" "$REPO_ROOT/lib/ChessCore/ChessEngine.cpp" test_chess.cpp \
  -o "$BUILD_DIR/test_chess"
"$BUILD_DIR/test_chess"
