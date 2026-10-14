#include "PuzzleCore.h"

#include <cstring>

namespace pondemate {

namespace {

bool isDigit(const char c) { return c >= '0' && c <= '9'; }
bool isFile(const char c) { return c >= 'a' && c <= 'h'; }
bool isRank(const char c) { return c >= '1' && c <= '8'; }
bool isIdChar(const char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Sort key for the candidate list: piece type, from square, to square, promotion.
int orderKey(const chess::Position& position, const chess::Move& move) {
  // Indexed by ChessCore piece type (Pawn=1 .. King=6) and promotion type.
  constexpr int kTypeRank[7] = {0, 5, 4, 3, 2, 1, 0};   // K, Q, R, B, N, P
  constexpr int kPromoRank[7] = {0, 0, 1, 3, 2, 0, 0};  // none/Q, N, R, B
  const int type = chess::pieceType(position.squares[move.from]);
  return (kTypeRank[type] << 16) | (move.from << 10) | (move.to << 4) | kPromoRank[move.promotion & 7];
}

}  // namespace

bool parseRecord(const char* rec, const size_t len, PuzzleRecord& out) {
  if (rec == nullptr || len != static_cast<size_t>(kRecordBytes) || rec[kRecordBytes - 1] != '\n') return false;

  // Field boundaries: exactly three TABs and no other control characters.
  const char* tabs[3];
  int tabCount = 0;
  for (int i = 0; i < kRecordBytes - 1; ++i) {
    const char c = rec[i];
    if (c == '\t') {
      if (tabCount == 3) return false;
      tabs[tabCount++] = rec + i;
    } else if (c < ' ' || c > '~') {
      return false;
    }
  }
  if (tabCount != 3) return false;

  const char* id = rec;
  const size_t idLen = tabs[0] - id;
  if (idLen == 0 || idLen > static_cast<size_t>(kMaxIdChars)) return false;
  for (size_t i = 0; i < idLen; ++i) {
    if (!isIdChar(id[i])) return false;
  }

  const char* rating = tabs[0] + 1;
  const size_t ratingLen = tabs[1] - rating;
  if (ratingLen == 0 || ratingLen > 5) return false;
  int ratingValue = 0;
  for (size_t i = 0; i < ratingLen; ++i) {
    if (!isDigit(rating[i])) return false;
    ratingValue = ratingValue * 10 + (rating[i] - '0');
  }

  const char* fen = tabs[1] + 1;
  const size_t fenLen = tabs[2] - fen;
  if (fenLen == 0 || fenLen > static_cast<size_t>(kMaxFenChars)) return false;

  // Moves: single-space separated UCI tokens, then space padding to the end.
  const char* p = tabs[2] + 1;
  const char* end = rec + kRecordBytes - 1;
  int count = 0;
  while (p < end && *p != ' ') {
    const char* token = p;
    while (p < end && *p != ' ') ++p;
    const size_t tokenLen = p - token;
    if (count == kMaxPuzzleMoves || (tokenLen != 4 && tokenLen != 5)) return false;
    if (!isFile(token[0]) || !isRank(token[1]) || !isFile(token[2]) || !isRank(token[3])) return false;
    if (tokenLen == 5 && strchr("qrbn", token[4]) == nullptr) return false;
    memcpy(out.moves[count], token, tokenLen);
    out.moves[count][tokenLen] = '\0';
    ++count;
    if (p < end && p + 1 < end && p[1] != ' ') ++p;  // single separator before the next token
  }
  for (; p < end; ++p) {
    if (*p != ' ') return false;
  }
  if (count < 2) return false;

  memcpy(out.id, id, idLen);
  out.id[idLen] = '\0';
  out.rating = ratingValue;
  memcpy(out.fen, fen, fenLen);
  out.fen[fenLen] = '\0';
  out.moveCount = count;
  return true;
}

bool PuzzleSession::findLegal(const chess::Position& position, const char* uci, chess::Move& out) const {
  scratch.clear();
  chess::generateLegalMoves(position, scratch);
  char text[6];
  for (int i = 0; i < scratch.count; ++i) {
    chess::moveToString(scratch.moves[i], text);
    if (strcmp(text, uci) == 0) {
      out = scratch.moves[i];
      return true;
    }
  }
  return false;
}

bool PuzzleSession::isMate(const chess::Position& position) const {
  if (!chess::isInCheck(position)) return false;
  scratch.clear();
  chess::generateLegalMoves(position, scratch);
  return scratch.count == 0;
}

bool PuzzleSession::begin(const PuzzleRecord& record) {
  rec = record;
  ply = 0;
  wrongCount = 0;
  isSolved = false;
  if (rec.moveCount < 2 || !chess::parseFen(rec.fen, pos)) return false;

  // Replay the whole line once so play() can always find the opponent's replies.
  chess::Position check = pos;
  for (int i = 0; i < rec.moveCount; ++i) {
    chess::Move move;
    if (!findLegal(check, rec.moves[i], move)) return false;
    chess::Undo undo;
    chess::makeMove(check, move, undo);
  }

  chess::Move first;
  findLegal(pos, rec.moves[0], first);
  chess::Undo undo;
  chess::makeMove(pos, first, undo);
  lastMoveFrom = first.from;
  lastMoveTo = first.to;
  playerWhite = pos.whiteToMove;
  ply = 1;
  return true;
}

int PuzzleSession::candidates(chess::Move* out, const int max) const {
  scratch.clear();
  chess::generateLegalMoves(pos, scratch);

  // Insertion sort: at most a couple of hundred moves, no allocation.
  for (int i = 1; i < scratch.count; ++i) {
    const chess::Move move = scratch.moves[i];
    const int key = orderKey(pos, move);
    int j = i - 1;
    while (j >= 0 && orderKey(pos, scratch.moves[j]) > key) {
      scratch.moves[j + 1] = scratch.moves[j];
      --j;
    }
    scratch.moves[j + 1] = move;
  }

  const int count = scratch.count < max ? scratch.count : max;
  for (int i = 0; i < count; ++i) out[i] = scratch.moves[i];
  return count;
}

void PuzzleSession::displayText(const chess::Move& move, char* buf, const size_t n, const bool hideMarks) const {
  if (buf == nullptr || n == 0) return;
  char san[10];
  chess::moveToSan(pos, move, san);
  size_t w = 0;
  for (const char* p = san; *p != '\0' && w + 1 < n; ++p) {
    if (hideMarks && (*p == '+' || *p == '#' || *p == 'x')) continue;
    buf[w++] = *p;
  }
  buf[w] = '\0';
}

PlayResult PuzzleSession::play(const chess::Move& move) {
  if (isSolved) return PlayResult::Solved;

  char uci[6];
  chess::moveToString(move, uci);
  chess::Move legal;
  if (!findLegal(pos, uci, legal)) {
    ++wrongCount;
    return PlayResult::Wrong;
  }

  chess::Undo undo;
  chess::makeMove(pos, legal, undo);
  const bool expected = strcmp(uci, rec.moves[ply]) == 0;
  // Any mating move solves the puzzle, even one that is not in the solution.
  if (isMate(pos) || (expected && ply + 1 >= rec.moveCount)) {
    lastMoveFrom = legal.from;
    lastMoveTo = legal.to;
    isSolved = true;
    return PlayResult::Solved;
  }
  if (!expected) {
    chess::unmakeMove(pos, legal, undo);
    ++wrongCount;
    return PlayResult::Wrong;
  }

  // Opponent's reply; begin() has already checked it is legal.
  chess::Move reply;
  findLegal(pos, rec.moves[ply + 1], reply);
  chess::Undo replyUndo;
  chess::makeMove(pos, reply, replyUndo);
  lastMoveFrom = reply.from;
  lastMoveTo = reply.to;
  ply += 2;
  if (ply >= rec.moveCount) {
    // A line that ends on the opponent's move: nothing left for the player to find.
    isSolved = true;
    return PlayResult::Solved;
  }
  return PlayResult::Correct;
}

}  // namespace pondemate
