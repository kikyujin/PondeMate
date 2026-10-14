// Validates PondeMate puzzle set files with the device's ChessCore.
//
//   validate [--mate] [--show N] set.txt...
//
// Every line must be exactly 256 bytes ending in '\n' and hold
// PuzzleId<TAB>Rating<TAB>FEN<TAB>Moves. The FEN must parse and every UCI move
// must be legal in turn (it is converted to SAN before being played). With
// --mate the position after the last move must be checkmate. The first N
// puzzles of each file (default 3) are printed for a human check.
//
// Exits non-zero if any puzzle fails.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ChessCore.h"

namespace {

constexpr int kLineBytes = 256;
constexpr int kMaxReportedFailures = 20;
constexpr int kMaxPlies = 64;

struct Failure {
  char id[16];
  char reason[160];
};

Failure failures[kMaxReportedFailures];
int reportedFailures = 0;
chess::MoveList legal;  // ~1KB; static so the layout matches the device's habits

void fail(const char* id, const char* reason) {
  if (reportedFailures < kMaxReportedFailures) {
    snprintf(failures[reportedFailures].id, sizeof(failures[0].id), "%s", id);
    snprintf(failures[reportedFailures].reason, sizeof(failures[0].reason), "%s", reason);
  }
  reportedFailures++;
}

// Splits `line` in place on TABs. Returns the number of fields found.
int splitTabs(char* line, char* fields[], int maxFields) {
  int count = 0;
  fields[count++] = line;
  for (char* p = line; *p != '\0'; ++p) {
    if (*p == '\t') {
      *p = '\0';
      if (count == maxFields) return maxFields + 1;
      fields[count++] = p + 1;
    }
  }
  return count;
}

void trimRight(char* s) {
  size_t n = strlen(s);
  while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
}

// Returns true when the puzzle is valid. Fills the SAN of each ply and the FEN
// after the opponent's first move for the human-readable listing.
bool checkPuzzle(const char* id, const char* fen, char* moves, bool expectMate, char san[][10], int& plies,
                 char* fenAfterFirst) {
  char reason[160];
  chess::Position position;
  if (!chess::parseFen(fen, position)) {
    fail(id, "parseFen failed");
    return false;
  }

  plies = 0;
  for (char* token = strtok(moves, " "); token != nullptr; token = strtok(nullptr, " ")) {
    if (plies >= kMaxPlies) {
      fail(id, "too many plies");
      return false;
    }
    legal.clear();
    chess::generateLegalMoves(position, legal);
    int match = -1;
    for (int i = 0; i < legal.count; ++i) {
      char uci[6];
      chess::moveToString(legal.moves[i], uci);
      if (strcmp(uci, token) == 0) {
        match = i;
        break;
      }
    }
    if (match < 0) {
      snprintf(reason, sizeof(reason), "ply %d: %s is not legal", plies + 1, token);
      fail(id, reason);
      return false;
    }
    const chess::Move move = legal.moves[match];
    chess::moveToSan(position, move, san[plies]);
    chess::Undo undo;
    chess::makeMove(position, move, undo);
    if (plies == 0) chess::positionToFen(position, fenAfterFirst);
    plies++;
  }
  if (plies < 2) {
    fail(id, "fewer than 2 plies");
    return false;
  }

  if (expectMate) {
    legal.clear();
    chess::generateLegalMoves(position, legal);
    if (!chess::isInCheck(position) || legal.count != 0) {
      snprintf(reason, sizeof(reason), "not mate after last move (check=%d, legal=%d)",
               chess::isInCheck(position) ? 1 : 0, legal.count);
      fail(id, reason);
      return false;
    }
  }
  return true;
}

// Returns the number of failed puzzles in the file, or -1 if it cannot be read.
int validateFile(const char* path, bool expectMate, int show) {
  FILE* file = fopen(path, "rb");
  if (file == nullptr) {
    printf("%s: cannot open\n", path);
    return -1;
  }

  const int failuresBefore = reportedFailures;
  char line[kLineBytes + 1];
  int count = 0;
  size_t got;
  while ((got = fread(line, 1, kLineBytes, file)) > 0) {
    count++;
    char id[16];
    snprintf(id, sizeof(id), "line%d", count);
    if (got != kLineBytes || line[kLineBytes - 1] != '\n') {
      fail(id, got != kLineBytes ? "short line (file size not a multiple of 256)" : "byte 256 is not '\\n'");
      continue;
    }
    line[kLineBytes - 1] = '\0';
    if (memchr(line, '\n', kLineBytes - 1) != nullptr) {
      fail(id, "newline inside the line");
      continue;
    }

    char* fields[4];
    if (splitTabs(line, fields, 4) != 4) {
      fail(id, "expected 4 TAB-separated fields");
      continue;
    }
    snprintf(id, sizeof(id), "%s", fields[0]);
    trimRight(fields[3]);

    char san[kMaxPlies][10];
    char fenAfterFirst[96] = {};
    int plies = 0;
    if (!checkPuzzle(id, fields[2], fields[3], expectMate, san, plies, fenAfterFirst)) continue;

    if (count <= show) {
      printf("  #%d %s  rating %s\n", count, id, fields[1]);
      printf("     opponent: %s\n", san[0]);
      printf("     position: %s\n", fenAfterFirst);
      printf("     solution:");
      for (int i = 1; i < plies; ++i) printf(" %s", san[i]);
      printf("\n");
    }
  }
  fclose(file);

  const int failed = reportedFailures - failuresBefore;
  printf("%s: %d puzzles, %d failed%s\n", path, count, failed, expectMate ? " (mate check on)" : "");
  return failed;
}

}  // namespace

int main(int argc, char** argv) {
  bool expectMate = false;
  int show = 3;
  int files = 0;
  int totalFailed = 0;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--mate") == 0) {
      expectMate = true;
    } else if (strcmp(argv[i], "--show") == 0 && i + 1 < argc) {
      show = atoi(argv[++i]);
    } else {
      files++;
      const int failed = validateFile(argv[i], expectMate, show);
      totalFailed += failed < 0 ? 1 : failed;
    }
  }
  if (files == 0) {
    fprintf(stderr, "usage: %s [--mate] [--show N] set.txt...\n", argv[0]);
    return 2;
  }

  if (reportedFailures > 0) {
    printf("failures (first %d):\n", kMaxReportedFailures);
    const int shown = reportedFailures < kMaxReportedFailures ? reportedFailures : kMaxReportedFailures;
    for (int i = 0; i < shown; ++i) printf("  %s: %s\n", failures[i].id, failures[i].reason);
  }
  printf("total failed: %d\n", totalFailed);
  return totalFailed == 0 ? 0 : 1;
}
