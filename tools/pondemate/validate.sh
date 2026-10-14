#!/bin/sh
# Builds the ChessCore-based validator on the host and checks every set file.
#
#   tools/pondemate/validate.sh [sets_dir]     (default ~/work/pondemate-data/sets)
#
# Files named mate_*.txt also get the checkmate check. Exits non-zero if any
# puzzle fails.
set -e
cd "$(dirname "$0")"
REPO_ROOT="$(cd ../.. && pwd)"
SETS_DIR="${1:-$HOME/work/pondemate-data/sets}"
BUILD_DIR="${TMPDIR:-/tmp}/$(basename "${CXX:-c++}")-pondemate-validate-$(echo "$REPO_ROOT" | cksum | cut -d" " -f1)"
mkdir -p "$BUILD_DIR"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -I"$REPO_ROOT/lib/ChessCore" \
  "$REPO_ROOT/lib/ChessCore/ChessCore.cpp" validate.cpp -o "$BUILD_DIR/validate"

status=0
for set in "$SETS_DIR"/*.txt; do
  case "$(basename "$set")" in
    mate_*) "$BUILD_DIR/validate" --mate "$set" || status=1 ;;
    *) "$BUILD_DIR/validate" "$set" || status=1 ;;
  esac
done
exit $status
