// Host tests for src/activities/puzzle/PuzzleCore against whole puzzle sets.
//
//   test/host/pondemate/run.sh [sets_dir]    (default ~/work/pondemate-data/sets)
//
// For every puzzle: the record parses; replaying the solution gives Correct
// until a final Solved; at every player turn the candidate list holds every
// legal move in the documented order, a wrong non-mating move is rejected and
// leaves the position unchanged, and any other mating move is accepted as
// Solved. The first puzzle of each set is printed with its numbered candidate
// lists. Exits non-zero on any failure.

#include <cstdio>
#include <cstring>

#include "ChessCore.h"
#include "PuzzleCore.h"

using pondemate::PlayResult;
using pondemate::PuzzleRecord;
using pondemate::PuzzleSession;

namespace {

constexpr int kMaxReported = 20;
int failures = 0;

void fail(const char* set, const char* id, const char* what) {
  if (failures < kMaxReported) printf("  FAIL %s %s: %s\n", set, id, what);
  failures++;
}

int typeRank(const int type) {
  constexpr int kRank[7] = {0, 5, 4, 3, 2, 1, 0};  // K, Q, R, B, N, P
  return kRank[type];
}

int promoRank(const int promotion) {
  constexpr int kRank[7] = {0, 0, 1, 3, 2, 0, 0};  // Q, N, R, B
  return kRank[promotion & 7];
}

// Independent check of the documented order (not the implementation's key).
bool inOrder(const chess::Position& position, const chess::Move& a, const chess::Move& b) {
  const int ta = typeRank(chess::pieceType(position.squares[a.from]));
  const int tb = typeRank(chess::pieceType(position.squares[b.from]));
  if (ta != tb) return ta < tb;
  if (a.from != b.from) return a.from < b.from;
  if (a.to != b.to) return a.to < b.to;
  return promoRank(a.promotion) < promoRank(b.promotion);
}

bool matesAfter(const chess::Position& position, const chess::Move& move) {
  static chess::MoveList replies;
  chess::Position after = position;
  chess::Undo undo;
  chess::makeMove(after, move, undo);
  if (!chess::isInCheck(after)) return false;
  replies.clear();
  chess::generateLegalMoves(after, replies);
  return replies.count == 0;
}

struct SetStats {
  int puzzles = 0;
  int failed = 0;
  int turns = 0;
  int altMatePositions = 0;
  int altMateMoves = 0;
  // Button presses to enter the expected move: snap cursor vs the 006 list.
  long snapSum = 0;
  long listSum = 0;
  int snapMax = 0;
  int listMax = 0;
  int snapHist[8] = {};  // 0-4, 5-8, 9-12, 13-16, 17-20, 21-24, 25-28, 29+
  int listHist[8] = {};
  int unreachable = 0;
  // 008: the same start squares, snap without wrap-around.
  long oldSum = 0;
  int oldMax = 0;
  int oldHist[8] = {};
  int startHits = 0;
  // Presses that leave the cursor where it is, over every target square and direction.
  long pressesTried = 0;
  long stuckNow = 0;
  long stuckOld = 0;
  // Turns where the current rule needs more / fewer presses than the 10/14 008 rule.
  int moreThanOld = 0;
  int fewerThanOld = 0;
  int worstDelta = 0;
  char worstId[pondemate::kMaxIdChars + 1] = "";
  int worstPly = 0;
  int worstNow = 0;
  int worstOld = 0;
  char worstKeys[256] = "";
};

chess::Move cands[chess::kMaxMoves];
chess::MoveList legal;
int shownMarks = 0;

void checkMarks(const PuzzleSession& session, const chess::Move& move, const char* set, const char* id) {
  char san[16];
  char hidden[16];
  session.displayText(move, san, sizeof(san), false);
  session.displayText(move, hidden, sizeof(hidden), true);
  if (strpbrk(hidden, "+#x") != nullptr) fail(set, id, "hideMarks left + # or x");
  if ((strchr(san, '=') != nullptr) != (strchr(hidden, '=') != nullptr)) fail(set, id, "hideMarks dropped '='");
  if (strpbrk(san, "+#x=") != nullptr && shownMarks < 12) {
    const bool interesting = strchr(san, '=') != nullptr || strchr(san, '#') != nullptr || shownMarks < 8;
    if (interesting) {
      printf("  displayText: %-8s -> hideMarks %s\n", san, hidden);
      shownMarks++;
    }
  }
}

using pondemate::Dir;
constexpr Dir kDirs[4] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};

const char* dirName(const Dir d) {
  switch (d) {
    case Dir::Up:
      return "up";
    case Dir::Down:
      return "down";
    case Dir::Left:
      return "left";
    default:
      return "right";
  }
}

// Simulator key for each screen direction in the standard layout: the front
// third/fourth buttons (BTN_LEFT/BTN_RIGHT) are up/down, the side buttons
// (BTN_UP on the left edge, BTN_DOWN on the right edge) are left/right.
const char* simKey(const Dir d) {
  switch (d) {
    case Dir::Up:
      return "LEFT";
    case Dir::Down:
      return "RIGHT";
    case Dir::Left:
      return "UP";
    default:
      return "DOWN";
  }
}

void squareName(const uint8_t sq, char* out) {
  out[0] = static_cast<char>('a' + (sq & 7));
  out[1] = static_cast<char>('1' + (sq >> 3));
  out[2] = '\0';
}

// Independent of PuzzleCore: screen column/row (row 0 at the top).
void cellOf(const uint8_t sq, const bool whiteAtBottom, int& col, int& row) {
  col = whiteAtBottom ? (sq & 7) : 7 - (sq & 7);
  row = whiteAtBottom ? 7 - (sq >> 3) : (sq >> 3);
}

int histBucket(const int presses) {
  if (presses <= 4) return 0;
  const int b = (presses - 1) / 4;
  return b > 7 ? 7 : b;
}

struct Reach {
  int dist[64];
  int parent[64];
  Dir via[64];
};

using SnapFn = uint8_t (*)(uint8_t, Dir, const uint8_t*, int, bool);

// The 10/14 008 rule (snap toward the nearest target, no wrap-around), kept for
// comparing press counts.
uint8_t snapNoWrap(const uint8_t cur, const Dir d, const uint8_t* targets, const int n, const bool whiteAtBottom) {
  int c0, r0;
  cellOf(cur, whiteAtBottom, c0, r0);
  int best = -1;
  if (d == Dir::Left || d == Dir::Right) {
    const int curOrder = r0 * 8 + c0;
    int bestOrder = d == Dir::Right ? 64 : -1;
    for (int i = 0; i < n; ++i) {
      int c, r;
      cellOf(targets[i], whiteAtBottom, c, r);
      const int order = r * 8 + c;
      if (d == Dir::Right ? (order > curOrder && order < bestOrder) : (order < curOrder && order > bestOrder)) {
        bestOrder = order;
        best = targets[i];
      }
    }
    return best < 0 ? cur : static_cast<uint8_t>(best);
  }
  int bestScore = 0, bestMain = 0, bestSide = 0;
  for (int i = 0; i < n; ++i) {
    if (targets[i] == cur) continue;
    int c, r;
    cellOf(targets[i], whiteAtBottom, c, r);
    const int along = d == Dir::Up ? r0 - r : r - r0;
    const int side = c > c0 ? c - c0 : c0 - c;
    if (along <= 0) continue;
    const int score = along + 2 * side;
    const bool better = best < 0 || score < bestScore || (score == bestScore && along < bestMain) ||
                        (score == bestScore && along == bestMain && side < bestSide) ||
                        (score == bestScore && along == bestMain && side == bestSide && targets[i] < best);
    if (better) {
      best = targets[i];
      bestScore = score;
      bestMain = along;
      bestSide = side;
    }
  }
  return best < 0 ? cur : static_cast<uint8_t>(best);
}

// Checks one step against the file-then-square rule, worked out here from the
// screen cells: Left/Right go to the next column that way holding a target
// (wrapping), onto the target nearest the current row (the lower one on a tie);
// Up/Down go to the next target that way in the same column (wrapping). Staying
// put is only allowed when no other column (Left/Right) or no other target in
// the column (Up/Down) exists. Counts presses that stay put, now and with the
// 10/14 rule (snapNoWrap).
void checkStep(const uint8_t cur, const Dir d, const uint8_t next, const uint8_t* targets, const int n,
               const bool whiteAtBottom, SetStats& stats, const char* set, const char* id) {
  stats.pressesTried++;
  if (snapNoWrap(cur, d, targets, n, whiteAtBottom) == cur) stats.stuckOld++;
  if (next == cur) stats.stuckNow++;

  int c0, r0;
  cellOf(cur, whiteAtBottom, c0, r0);
  int col[64], row[64];
  for (int i = 0; i < n; ++i) cellOf(targets[i], whiteAtBottom, col[i], row[i]);

  int want = -1;
  if (d == Dir::Left || d == Dir::Right) {
    // Nearest column that way, else the far column on the other side.
    int nearCol = -1, farCol = -1;
    for (int i = 0; i < n; ++i) {
      const int c = col[i];
      if (c == c0) continue;
      const bool ahead = d == Dir::Right ? c > c0 : c < c0;
      if (ahead) {
        if (nearCol < 0 || (d == Dir::Right ? c < nearCol : c > nearCol)) nearCol = c;
      } else if (farCol < 0 || (d == Dir::Right ? c < farCol : c > farCol)) {
        farCol = c;
      }
    }
    const int target = nearCol >= 0 ? nearCol : farCol;
    if (target >= 0) {
      int bestOff = 99, bestRow = -1;
      for (int i = 0; i < n; ++i) {
        if (col[i] != target) continue;
        const int off = row[i] > r0 ? row[i] - r0 : r0 - row[i];
        if (off < bestOff || (off == bestOff && row[i] > bestRow)) {
          bestOff = off;
          bestRow = row[i];
          want = targets[i];
        }
      }
    }
    if (next != cur) {
      int c1, r1;
      cellOf(next, whiteAtBottom, c1, r1);
      if (c1 == c0) fail(set, id, "left/right stayed in the same column");
    } else if (target >= 0) {
      fail(set, id, "left/right stayed put with another column holding a target");
    }
  } else {
    const bool up = d == Dir::Up;
    int nearRow = -1, farRow = -1;
    for (int i = 0; i < n; ++i) {
      if (targets[i] == cur || col[i] != c0 || row[i] == r0) continue;
      const int r = row[i];
      if (up ? r < r0 : r > r0) {
        if (nearRow < 0 || (up ? r > nearRow : r < nearRow)) nearRow = r;
      } else if (farRow < 0 || (up ? r > farRow : r < farRow)) {
        farRow = r;
      }
    }
    const int target = nearRow >= 0 ? nearRow : farRow;
    for (int i = 0; i < n && target >= 0; ++i) {
      if (col[i] == c0 && row[i] == target) want = targets[i];
    }
    if (next != cur) {
      int c1, r1;
      cellOf(next, whiteAtBottom, c1, r1);
      if (c1 != c0) fail(set, id, "up/down left the column");
    } else if (target >= 0) {
      fail(set, id, "up/down stayed put with another target in the column");
    }
  }
  if (want < 0) want = cur;
  if (next != want) fail(set, id, "snap differs from the file-then-square rule");
}

// Breadth-first search over `step` moves from `start` through `targets`. With
// `stats`, every step is also checked against the pressed direction (checkStep).
void explore(const uint8_t start, const uint8_t* targets, const int n, const bool whiteAtBottom, Reach& r,
             const char* set, const char* id, const SnapFn step = pondemate::snap, SetStats* stats = nullptr) {
  for (int i = 0; i < 64; ++i) r.dist[i] = r.parent[i] = -1;
  int queue[64];
  int head = 0, tail = 0;
  r.dist[start] = 0;
  queue[tail++] = start;
  while (head < tail) {
    const uint8_t cur = static_cast<uint8_t>(queue[head++]);
    for (const Dir d : kDirs) {
      const uint8_t next = step(cur, d, targets, n, whiteAtBottom);
      if (stats != nullptr) checkStep(cur, d, next, targets, n, whiteAtBottom, *stats, set, id);
      if (next == cur) continue;
      bool isTarget = false;
      for (int i = 0; i < n; ++i) isTarget = isTarget || targets[i] == next;
      if (!isTarget) fail(set, id, "snap left the target set");
      if (r.dist[next] < 0) {
        r.dist[next] = r.dist[cur] + 1;
        r.parent[next] = cur;
        r.via[next] = d;
        queue[tail++] = next;
      }
    }
  }
}

// Appends the simulator keys for the shortest path to `target`.
void appendPath(const Reach& r, const uint8_t target, char* out, const size_t n) {
  Dir dirs[64];
  int count = 0;
  for (int sq = target; r.parent[sq] >= 0; sq = r.parent[sq]) dirs[count++] = r.via[sq];
  for (int i = count - 1; i >= 0; --i) {
    strncat(out, simKey(dirs[i]), n - strlen(out) - 1);
    strncat(out, " ", n - strlen(out) - 1);
  }
}

bool sameSet(const uint8_t* got, const int n, const bool* want) {
  bool seen[64] = {};
  for (int i = 0; i < n; ++i) {
    if (seen[got[i]] || !want[got[i]]) return false;
    seen[got[i]] = true;
  }
  int wantCount = 0;
  for (int i = 0; i < 64; ++i) wantCount += want[i] ? 1 : 0;
  return wantCount == n;
}

// Checks the snap-cursor helpers at one player turn and returns the presses
// needed to enter `move`; fills `keys` with the simulator key sequence.
// Destination selection starts on initialDestination(); `oldPresses` is the
// 008 count (same starts, snap without wrap-around), `startHit` whether the
// starting destination already is `move.to`.
int cursorTurn(const PuzzleSession& session, const chess::MoveList& legalMoves, const chess::Move& move,
               const char* set, const char* id, SetStats& stats, char* keys, const size_t keysLen,
               int* oldPresses = nullptr, bool* startHit = nullptr) {
  const bool wb = session.playerIsWhite();
  uint8_t movable[64];
  const int nm = session.movablePieces(movable);
  bool fromWanted[64] = {};
  for (int i = 0; i < legalMoves.count; ++i) fromWanted[legalMoves.moves[i].from] = true;
  if (!sameSet(movable, nm, fromWanted)) fail(set, id, "movablePieces != legal move origins");

  const uint8_t start = session.initialCursor();
  if (!fromWanted[start]) fail(set, id, "initialCursor is not a movable piece");
  static Reach pieces;
  static Reach oldPieces;
  explore(start, movable, nm, wb, pieces, set, id, pondemate::snap, &stats);
  explore(start, movable, nm, wb, oldPieces, set, id, snapNoWrap);
  for (int i = 0; i < nm; ++i) {
    if (pieces.dist[movable[i]] < 0) {
      char a[3], b[3];
      squareName(start, a);
      squareName(movable[i], b);
      char what[64];
      snprintf(what, sizeof(what), "piece %s unreachable from %s", b, a);
      fail(set, id, what);
      stats.unreachable++;
    }
  }

  static Reach dest;
  static Reach oldDest;
  int destPresses = -1;
  int oldDestPresses = -1;
  for (int i = 0; i < nm; ++i) {
    uint8_t targets[64];
    const int nd = session.destinations(movable[i], targets);
    bool toWanted[64] = {};
    for (int k = 0; k < legalMoves.count; ++k) {
      if (legalMoves.moves[k].from == movable[i]) toWanted[legalMoves.moves[k].to] = true;
    }
    if (!sameSet(targets, nd, toWanted)) fail(set, id, "destinations != legal destinations");
    const uint8_t first = session.initialDestination(movable[i]);
    if (!toWanted[first]) fail(set, id, "initialDestination is not a destination");
    explore(first, targets, nd, wb, dest, set, id, pondemate::snap, &stats);
    for (int k = 0; k < nd; ++k) {
      if (dest.dist[targets[k]] < 0) {
        char a[3], b[3];
        squareName(movable[i], a);
        squareName(targets[k], b);
        char what[64];
        snprintf(what, sizeof(what), "destination %s unreachable when grabbing %s", b, a);
        fail(set, id, what);
        stats.unreachable++;
      }
    }
    if (movable[i] == move.from) {
      destPresses = dest.dist[move.to];
      explore(first, targets, nd, wb, oldDest, set, id, snapNoWrap);
      oldDestPresses = oldDest.dist[move.to];
      if (startHit != nullptr) *startHit = first == move.to;
      if (keys != nullptr) {
        keys[0] = '\0';
        appendPath(pieces, move.from, keys, keysLen);
        strncat(keys, "CONFIRM ", keysLen - strlen(keys) - 1);
        appendPath(dest, move.to, keys, keysLen);
        strncat(keys, "CONFIRM", keysLen - strlen(keys) - 1);
      }
    }
  }

  chess::Move choices[4];
  const int nc = session.promotionChoices(move.from, move.to, choices);
  int promoPresses = 0;
  if (nc > 1) {
    constexpr uint8_t kOrder[4] = {chess::Queen, chess::Knight, chess::Rook, chess::Bishop};
    if (nc != 4) fail(set, id, "promotion without four choices");
    for (int i = 0; i < nc && i < 4; ++i) {
      if (choices[i].promotion != kOrder[i]) fail(set, id, "promotion choices not Q, N, R, B");
      if (choices[i].promotion == move.promotion) {
        promoPresses = i + 1;
        if (keys != nullptr) {
          for (int k = 0; k < i; ++k) strncat(keys, " RIGHT", keysLen - strlen(keys) - 1);
          strncat(keys, " CONFIRM", keysLen - strlen(keys) - 1);
        }
      }
    }
  } else if (nc != 1) {
    fail(set, id, "promotionChoices did not find the move");
  }

  if (pieces.dist[move.from] < 0 || destPresses < 0) return -1;
  if (oldPresses != nullptr) {
    *oldPresses = oldDestPresses < 0 || oldPieces.dist[move.from] < 0
                      ? -1
                      : oldPieces.dist[move.from] + 1 + oldDestPresses + 1 + promoPresses;
  }
  return pieces.dist[move.from] + 1 + destPresses + 1 + promoPresses;
}

void printDirections(const PuzzleSession& session) {
  const bool wb = session.playerIsWhite();
  uint8_t movable[64];
  const int nm = session.movablePieces(movable);
  const uint8_t start = session.initialCursor();
  char from[3], last[3];
  squareName(start, from);
  squareName(session.lastTo(), last);
  int c, r;
  cellOf(start, wb, c, r);
  printf("    orientation check (%s at the bottom): last move to %s, cursor %s at screen col %d row %d\n",
         wb ? "white" : "black", last, from, c, r);
  for (const Dir d : kDirs) {
    const uint8_t next = pondemate::snap(start, d, movable, nm, wb);
    char to[3];
    squareName(next, to);
    int c1, r1;
    cellOf(next, wb, c1, r1);
    printf("      %-5s -> %s (col %d row %d)%s\n", dirName(d), to, c1, r1, next == start ? "  [no move]" : "");
  }
}

void runPuzzle(const char* set, const char* line, SetStats& stats, const bool show) {
  PuzzleRecord record;
  if (!pondemate::parseRecord(line, pondemate::kRecordBytes, record)) {
    fail(set, "?", "parseRecord failed");
    stats.failed++;
    return;
  }
  const char* id = record.id;
  const int failuresBefore = failures;

  static PuzzleSession session;
  if (!session.begin(record)) {
    fail(set, id, "begin failed");
    stats.failed++;
    return;
  }
  if (show) {
    printf("  first puzzle %s (rating %d), player is %s\n", id, record.rating,
           session.playerIsWhite() ? "white" : "black");
  }

  for (int guard = 0; guard < pondemate::kMaxPuzzleMoves; ++guard) {
    const chess::Position before = session.position();
    const int ply = session.nextPly();
    const char* expected = record.moves[ply];
    stats.turns++;

    // 3. candidates == all legal moves, in order.
    const int n = session.candidates(cands, chess::kMaxMoves);
    legal.clear();
    chess::generateLegalMoves(before, legal);
    if (n != legal.count) fail(set, id, "candidate count != legal move count");
    for (int i = 1; i < n; ++i) {
      if (!inOrder(before, cands[i - 1], cands[i])) {
        fail(set, id, "candidates out of order");
        break;
      }
    }

    int expectedIndex = -1;
    int wrongIndex = -1;
    int altMates = 0;
    for (int i = 0; i < n; ++i) {
      char uci[6];
      chess::moveToString(cands[i], uci);
      const bool isExpected = strcmp(uci, expected) == 0;
      if (isExpected) {
        expectedIndex = i;
        continue;
      }
      if (matesAfter(before, cands[i])) {
        // 5. another mating move must be accepted.
        PuzzleSession copy = session;
        if (copy.play(cands[i]) != PlayResult::Solved) fail(set, id, "alternative mate not Solved");
        altMates++;
      } else if (wrongIndex < 0) {
        wrongIndex = i;
      }
    }
    if (altMates > 0) {
      stats.altMatePositions++;
      stats.altMateMoves += altMates;
    }
    if (expectedIndex < 0) {
      fail(set, id, "expected move not among candidates");
      break;
    }

    // 4. a wrong move is rejected and leaves the position unchanged.
    if (wrongIndex >= 0) {
      PuzzleSession copy = session;
      const uint64_t keyBefore = chess::positionKey(copy.position());
      if (copy.play(cands[wrongIndex]) != PlayResult::Wrong) fail(set, id, "non-mating wrong move not Wrong");
      if (chess::positionKey(copy.position()) != keyBefore) fail(set, id, "Wrong changed the position");
      if (copy.mistakes() != session.mistakes() + 1) fail(set, id, "mistakes not incremented");
    }

    // 6. hideMarks on the expected move.
    checkMarks(session, cands[expectedIndex], set, id);

    // C-2: snap cursor helpers, reachability and press counts.
    char keys[256];
    int oldPresses = -1;
    bool startHit = false;
    const int snapPresses = cursorTurn(session, legal, cands[expectedIndex], set, id, stats, keys, sizeof(keys),
                                       &oldPresses, &startHit);
    if (startHit) stats.startHits++;
    if (oldPresses >= 0 && snapPresses >= 0) {
      if (snapPresses > oldPresses) stats.moreThanOld++;
      if (snapPresses < oldPresses) stats.fewerThanOld++;
      if (snapPresses - oldPresses > stats.worstDelta) {
        stats.worstDelta = snapPresses - oldPresses;
        snprintf(stats.worstId, sizeof(stats.worstId), "%s", id);
        stats.worstPly = ply;
        stats.worstNow = snapPresses;
        stats.worstOld = oldPresses;
        snprintf(stats.worstKeys, sizeof(stats.worstKeys), "%s", keys);
      }
    }
    if (oldPresses >= 0) {
      stats.oldSum += oldPresses;
      if (oldPresses > stats.oldMax) stats.oldMax = oldPresses;
      stats.oldHist[histBucket(oldPresses)]++;
    }
    const int listPresses = expectedIndex + 1;
    if (snapPresses >= 0) {
      stats.snapSum += snapPresses;
      stats.listSum += listPresses;
      if (snapPresses > stats.snapMax) stats.snapMax = snapPresses;
      if (listPresses > stats.listMax) stats.listMax = listPresses;
      stats.snapHist[histBucket(snapPresses)]++;
      stats.listHist[histBucket(listPresses)]++;
    }
    if (show) {
      if (!session.playerIsWhite() || ply == 1) printDirections(session);
      if (ply == 1 && strcmp(id, "d7yAE") == 0) {
        // B-4-6: grab the queen on h5, then one press each way from the starting destination.
        constexpr uint8_t kH5 = 39;
        uint8_t targets[64];
        const int nd = session.destinations(kH5, targets);
        const uint8_t first = session.initialDestination(kH5);
        char from[3];
        squareName(first, from);
        printf("    d7yAE: grab h5, destination cursor starts on %s; one press from there:", from);
        for (const Dir d : kDirs) {
          char to[3];
          squareName(pondemate::snap(first, d, targets, nd, session.playerIsWhite()), to);
          printf(" %s->%s", dirName(d), to);
        }
        printf("\n      destinations:");
        for (int i = 0; i < nd; ++i) {
          char sqName[3];
          squareName(targets[i], sqName);
          printf(" %s", sqName);
        }
        printf("\n");
      }
      char sq[3];
      squareName(session.initialCursor(), sq);
      printf("    snap cursor: start %s, %d presses (10/14 008: %d, list: %d). standard keys: %s\n", sq, snapPresses,
             oldPresses, listPresses, keys);
      if (ply == 1) {
        // A cheap wrong (non-mating, not expected) move for the simulator run.
        int bestPresses = 1 << 30;
        char bestKeys[256] = "";
        char bestSan[16] = "";
        uint8_t movable[64];
        const int nm = session.movablePieces(movable);
        for (int i = 0; i < nm; ++i) {
          uint8_t targets[64];
          const int nd = session.destinations(movable[i], targets);
          for (int k = 0; k < nd; ++k) {
            chess::Move choices[4];
            session.promotionChoices(movable[i], targets[k], choices);
            char uci[6];
            chess::moveToString(choices[0], uci);
            if (strcmp(uci, expected) == 0 || matesAfter(before, choices[0])) continue;
            char wrongKeys[256];
            SetStats scratchStats;
            const int presses =
                cursorTurn(session, legal, choices[0], set, id, scratchStats, wrongKeys, sizeof(wrongKeys));
            if (presses >= 0 && presses < bestPresses) {
              bestPresses = presses;
              snprintf(bestKeys, sizeof(bestKeys), "%s", wrongKeys);
              session.displayText(choices[0], bestSan, sizeof(bestSan), true);
            }
          }
        }
        printf("    cheapest wrong move: %s, %d presses. standard keys: %s\n", bestSan, bestPresses, bestKeys);
      }
    }

    if (show) {
      printf("    ply %d, candidates %d, correct #%d:", ply, n, expectedIndex);
      for (int i = 0; i < n; ++i) {
        char text[16];
        session.displayText(cands[i], text, sizeof(text), true);
        printf("%s %d:%s", i % 10 == 0 ? "\n     " : "", i, text);
      }
      printf("\n");
    }

    // 2. play the solution.
    const bool last = ply + 1 >= record.moveCount;
    const PlayResult result = session.play(cands[expectedIndex]);
    if (last) {
      if (result != PlayResult::Solved) fail(set, id, "last move not Solved");
      break;
    }
    if (result != PlayResult::Correct) {
      fail(set, id, result == PlayResult::Solved ? "Solved before the end" : "expected move not Correct");
      break;
    }
    if (session.nextPly() != ply + 2) fail(set, id, "ply did not advance by 2");
  }
  if (!session.solved()) fail(set, id, "not solved at the end");
  if (failures != failuresBefore) stats.failed++;
}

int runSet(const char* dir, const char* name) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE* file = fopen(path, "rb");
  if (file == nullptr) {
    printf("%s: cannot open\n", path);
    failures++;
    return 1;
  }
  printf("== %s\n", name);
  SetStats stats;
  char line[pondemate::kRecordBytes];
  while (fread(line, 1, sizeof(line), file) == sizeof(line)) {
    runPuzzle(name, line, stats, stats.puzzles == 0);
    stats.puzzles++;
  }
  fclose(file);
  printf("  %s: %d puzzles, %d failed, %d player turns, alternative mates in %d positions (%d moves)\n", name,
         stats.puzzles, stats.failed, stats.turns, stats.altMatePositions, stats.altMateMoves);
  const char* buckets[8] = {"0-4", "5-8", "9-12", "13-16", "17-20", "21-24", "25-28", "29+"};
  const double turns = stats.turns ? static_cast<double>(stats.turns) : 1.0;
  printf("  presses per player move: now %.2f (max %d), 10/14 008 %.2f (max %d), 006 list %.2f (max %d); unreachable %d\n",
         stats.snapSum / turns, stats.snapMax, stats.oldSum / turns, stats.oldMax, stats.listSum / turns,
         stats.listMax, stats.unreachable);
  printf("  destination cursor starts on the answer: %d of %d turns (%.1f%%)\n", stats.startHits, stats.turns,
         100.0 * stats.startHits / turns);
  printf("  presses that stay put: now %ld, 10/14 008 %ld, of %ld (every target square x 4 directions)\n", stats.stuckNow,
         stats.stuckOld, stats.pressesTried);
  printf("  turns needing more presses than 10/14 008: %d, fewer: %d\n", stats.moreThanOld, stats.fewerThanOld);
  if (stats.worstDelta > 0) {
    printf("  largest increase: %s ply %d, %d presses (10/14 008: %d). standard keys: %s\n", stats.worstId,
           stats.worstPly, stats.worstNow, stats.worstOld, stats.worstKeys);
  }
  printf("    presses    now    008   list\n");
  for (int i = 0; i < 8; ++i) {
    printf("    %-7s %6d %6d %6d\n", buckets[i], stats.snapHist[i], stats.oldHist[i], stats.listHist[i]);
  }
  return stats.failed;
}

}  // namespace

int main(int argc, char** argv) {
  const char* dir = argc > 1 ? argv[1] : nullptr;
  if (dir == nullptr) {
    fprintf(stderr, "usage: %s sets_dir\n", argv[0]);
    return 2;
  }
  printf("sizeof(PuzzleSession) = %zu, sizeof(PuzzleRecord) = %zu\n", sizeof(PuzzleSession), sizeof(PuzzleRecord));

  // A few malformed records must be rejected.
  char bad[pondemate::kRecordBytes];
  PuzzleRecord record;
  memset(bad, ' ', sizeof(bad));
  bad[sizeof(bad) - 1] = '\n';
  const char* good = "abc\t1200\t8/8/8/8/8/8/8/8 w - - 0 1\te2e4 e7e5";
  memcpy(bad, good, strlen(good));
  if (!pondemate::parseRecord(bad, sizeof(bad), record)) fail("parse", "good", "well-formed record rejected");
  if (pondemate::parseRecord(bad, sizeof(bad) - 1, record)) fail("parse", "len", "short record accepted");
  bad[10] = '\t';
  if (pondemate::parseRecord(bad, sizeof(bad), record)) fail("parse", "tabs", "five fields accepted");
  const char* badUci = "abc\t1200\t8/8/8/8/8/8/8/8 w - - 0 1\te2e9 e7e5";
  memcpy(bad, badUci, strlen(badUci));
  if (pondemate::parseRecord(bad, sizeof(bad), record)) fail("parse", "uci", "bad UCI accepted");

  // Promotion with capture and check: '=' stays, 'x' and '+' go.
  memset(bad, ' ', sizeof(bad));
  bad[sizeof(bad) - 1] = '\n';
  const char* promoLine = "promo\t1000\t3r3k/4P3/8/8/8/8/8/K7 b - - 0 1\th8g8 e7d8q";
  memcpy(bad, promoLine, strlen(promoLine));
  static PuzzleSession promo;
  if (!pondemate::parseRecord(bad, sizeof(bad), record) || !promo.begin(record)) {
    fail("promo", "promo", "fixed promotion puzzle did not load");
  } else {
    const int n = promo.candidates(cands, chess::kMaxMoves);
    for (int i = 0; i < n; ++i) {
      char uci[6];
      chess::moveToString(cands[i], uci);
      if (strcmp(uci, "e7d8q") != 0) continue;
      char san[16];
      char hidden[16];
      promo.displayText(cands[i], san, sizeof(san), false);
      promo.displayText(cands[i], hidden, sizeof(hidden), true);
      printf("promotion displayText: %s -> hideMarks %s\n", san, hidden);
      if (strcmp(hidden, "ed8=Q") != 0) fail("promo", "promo", "hideMarks on a promotion");
      if (promo.play(cands[i]) != PlayResult::Solved) fail("promo", "promo", "last move not Solved");
    }
  }

  const char* sets[] = {"mate_r1000-1500.txt", "hanging_r1000-1500.txt", "forkpin_r1000-1500.txt",
                        "endgame_r1000-1500.txt"};
  int failedPuzzles = 0;
  for (const char* name : sets) failedPuzzles += runSet(dir, name);

  printf("total: %d failed puzzles, %d failed checks\n", failedPuzzles, failures);
  return failures == 0 ? 0 : 1;
}
