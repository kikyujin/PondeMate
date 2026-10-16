#pragma once

// Puzzle logic for PondeMate: parsing one line of a puzzle set file, ordering
// the candidate moves and judging the player's moves.
//
// Plain C++17 on top of ChessCore (no Arduino, no CrossPoint headers, no heap)
// so it can be tested on the host against whole puzzle sets
// (test/host/pondemate/run.sh).

#include <ChessCore.h>

#include <cstddef>
#include <cstdint>

namespace pondemate {

// One puzzle per line: PuzzleId<TAB>Rating<TAB>FEN<TAB>Moves, space padded to
// 255 bytes plus '\n'. Moves are UCI; the first one is the opponent's.
constexpr int kRecordBytes = 256;
constexpr int kMaxIdChars = 15;
constexpr int kMaxFenChars = 95;
constexpr int kMaxPuzzleMoves = 40;
constexpr int kUciBytes = 6;  // "e7e8q" + NUL

struct PuzzleRecord {
  char id[kMaxIdChars + 1];
  int rating;
  char fen[kMaxFenChars + 1];
  char moves[kMaxPuzzleMoves][kUciBytes];
  int moveCount;
};

// Parses one 256-byte record. Fails on a wrong length or terminator, a field
// count other than four, or a move that is not UCI-shaped.
bool parseRecord(const char* rec, size_t len, PuzzleRecord& out);

// Directions as seen on screen.
enum class Dir : uint8_t { Up, Down, Left, Right };

// Snap-cursor step from square `cur` among `targets`, directions as drawn on
// screen (whiteAtBottom = false means the board is flipped, black at the bottom).
// Left/Right pick a file: the next screen column that way holding a target,
// wrapping past the edge, landing on its target nearest the cursor's row (a tie
// goes to the lower one on screen). Up/Down pick a square in the cursor's
// column: the next target that way, wrapping to the far end of the column.
// Returns `cur` when there is nowhere to go (no other column with a target for
// Left/Right, no other target in the column for Up/Down).
uint8_t snap(uint8_t cur, Dir d, const uint8_t* targets, int n, bool whiteAtBottom);

enum class PlayResult : uint8_t {
  Solved,   // the move mates, or it is the last move of the solution
  Correct,  // the expected move with more to come; the opponent's reply has been played
  Wrong,    // anything else; the position is left as it was
};

class PuzzleSession {
 public:
  // Sets up the FEN, checks every solution move is legal in turn, then plays
  // the opponent's first move. Returns false if the line does not replay.
  bool begin(const PuzzleRecord& record);

  // Every legal move in the current position, in a fixed mechanical order so
  // the list gives nothing away: piece type (K, Q, R, B, N, P), then from
  // square a1..h8, then to square a1..h8, then promotion (Q, N, R, B).
  // Returns the number written (at most `max`).
  int candidates(chess::Move* out, int max) const;

  // SAN for `move` in the current position. With hideMarks, '+', '#' and 'x'
  // are dropped ("Qxf7+" -> "Qf7"); '=' of a promotion is kept.
  void displayText(const chess::Move& move, char* buf, size_t n, bool hideMarks) const;

  PlayResult play(const chess::Move& move);

  // Squares of the player's pieces that have at least one legal move, ascending.
  int movablePieces(uint8_t* out) const;
  // Legal destination squares of the piece on `from`, ascending, each once
  // (the four promotions to one square count as one).
  int destinations(uint8_t from, uint8_t* out) const;
  // The legal moves from `from` to `to`: Q, N, R, B for a promotion, else one.
  int promotionChoices(uint8_t from, uint8_t to, chess::Move* out) const;
  // Where the cursor starts: the movable piece closest (Chebyshev distance) to
  // the last move's destination, the lower square on a tie.
  uint8_t initialCursor() const;
  // Where the destination cursor starts after grabbing `from`: the destination
  // on the same file with the smallest rank difference, otherwise the closest
  // destination (Chebyshev distance); the lower square on a tie. Board
  // coordinates, independent of how the board is drawn.
  uint8_t initialDestination(uint8_t from) const;

  const chess::Position& position() const { return pos; }
  bool playerIsWhite() const { return playerWhite; }
  // From/to squares of the last move on the board: the opponent's first move
  // after begin(), then each reply.
  uint8_t lastFrom() const { return lastMoveFrom; }
  uint8_t lastTo() const { return lastMoveTo; }
  // Index into the record's moves of the move the player should play next.
  int nextPly() const { return ply; }
  int mistakes() const { return wrongCount; }
  bool solved() const { return isSolved; }
  const PuzzleRecord& record() const { return rec; }

 private:
  bool findLegal(const chess::Position& position, const char* uci, chess::Move& out) const;
  bool isMate(const chess::Position& position) const;

  PuzzleRecord rec{};
  chess::Position pos;
  // ~1KB scratch for move generation; a member so callers never put it on the stack.
  mutable chess::MoveList scratch;
  int ply = 0;
  int wrongCount = 0;
  bool playerWhite = true;
  bool isSolved = false;
  uint8_t lastMoveFrom = 0;
  uint8_t lastMoveTo = 0;
};

}  // namespace pondemate
