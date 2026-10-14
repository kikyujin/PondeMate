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
