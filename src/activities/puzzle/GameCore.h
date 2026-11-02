#pragma once

// Game files for the PondeMate game viewer: parsing one 128-byte line and
// formatting the move line under the board.
//
// Plain C++17 on top of ChessCore (no Arduino, no CrossPoint headers, no heap)
// so it can be tested on the host (test/host/pondemate/run.sh) and shared with
// tools/pondemate/validate_games.cpp.
//
// File layout (written by tools/pondemate/pgn2game.py), ASCII, every line
// exactly 128 bytes: up to 127 bytes padded with spaces, then '\n'.
//   line 0      #<TAB>White<TAB>Black<TAB>Result<TAB>Date<TAB>plies<TAB>w|b
//   line 1      0<TAB>-<TAB>-<TAB>start FEN<TAB>-<TAB>-
//   line n + 1  n<TAB>SAN<TAB>UCI<TAB>FEN after the move<TAB>eval<TAB>mark
// Ply n is at byte (n + 1) * 128, so the viewer reads one line per move.

#include <ChessCore.h>

#include <cstddef>
#include <cstdint>

namespace pondemate {

constexpr int kGameLineBytes = 128;
constexpr int kMaxGamePlies = 9999;
constexpr int kMaxNameChars = 31;
constexpr int kMaxResultChars = 7;
constexpr int kMaxDateChars = 10;
constexpr int kMaxSanChars = 7;  // "exd8=Q+", "Qa1xb2#"
constexpr int kMaxEvalChars = 7;
constexpr int kMaxMarkChars = 2;
constexpr int kMaxGameFenChars = 95;  // same limit as the puzzle files

struct GameHeader {
  char white[kMaxNameChars + 1];
  char black[kMaxNameChars + 1];
  char result[kMaxResultChars + 1];
  char date[kMaxDateChars + 1];
  int plies;
  bool whiteAtBottom;
};

// SAN, UCI, eval and mark are empty where the file has "-" (ply 0 has no move).
struct GameMove {
  int ply;
  char san[kMaxSanChars + 1];
  char uci[6];
  char fen[kMaxGameFenChars + 1];
  char eval[kMaxEvalChars + 1];
  char mark[kMaxMarkChars + 1];
};

// Byte offset of the line for `ply`.
constexpr size_t gameLineOffset(const int ply) { return static_cast<size_t>(ply + 1) * kGameLineBytes; }

// Each parser fails on a wrong length or terminator, a byte that is not
// printable ASCII or TAB, a wrong field count, or a field that is empty, too
// long or malformed. The FEN is only length-checked; chess::parseFen decides
// whether it is a position.
bool parseGameHeader(const char* line, size_t len, GameHeader& out);
bool parseGameMove(const char* line, size_t len, GameMove& out);

// The line under the board: "12. Nf3?  -0.76  (23/58)", "12... Nf6  (24/58)",
// "Start  (0/58)". The move number and side come from `after`, the position
// after the move (its FEN), so games starting from any position number right.
void formatGameMove(const GameMove& move, const chess::Position& after, int plies, char* buf, size_t n);

}  // namespace pondemate
