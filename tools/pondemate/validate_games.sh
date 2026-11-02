#!/bin/sh
# Builds the game file validator on the host and checks every game file.
#
#   tools/pondemate/validate_games.sh [games_dir]     (default ~/work/pondemate-data/games)
#
# Uses the device's GameCore parser and ChessCore. Exits non-zero if any game
# fails.
set -e
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
GAMES_DIR="${1:-$HOME/work/pondemate-data/games}"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-pondemate-validate-games-$(echo "$REPO_ROOT" | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -I"$REPO_ROOT/lib/ChessCore" -I"$REPO_ROOT/src/activities/puzzle" \
  "$REPO_ROOT/lib/ChessCore/ChessCore.cpp" "$REPO_ROOT/src/activities/puzzle/GameCore.cpp" validate_games.cpp \
  -o "$BUILD_DIR/validate_games"
"$BUILD_DIR/validate_games" "$GAMES_DIR"/*.txt
