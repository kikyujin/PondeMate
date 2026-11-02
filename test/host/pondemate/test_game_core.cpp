// Host tests for src/activities/puzzle/GameCore: the 128-byte game file lines
// and the move line under the board.
//
//   test/host/pondemate/run.sh    (builds and runs this before the puzzle tests)
//
// Exits non-zero on any failure.

#include <cstdio>
#include <cstring>

#include "ChessCore.h"
#include "GameCore.h"

namespace {

int failures = 0;
int checks = 0;

void expect(const bool ok, const char* what) {
  checks++;
  if (!ok) {
    printf("  FAIL %s\n", what);
    failures++;
  }
}

// `text` space padded to 127 bytes plus '\n', like pgn2game.py writes it.
void makeLine(const char* text, char* line) {
  memset(line, ' ', pondemate::kGameLineBytes);
  memcpy(line, text, strlen(text));
  line[pondemate::kGameLineBytes - 1] = '\n';
}

bool header(const char* text, pondemate::GameHeader& out) {
  char line[pondemate::kGameLineBytes];
  makeLine(text, line);
  return pondemate::parseGameHeader(line, sizeof(line), out);
}

bool move(const char* text, pondemate::GameMove& out) {
  char line[pondemate::kGameLineBytes];
  makeLine(text, line);
  return pondemate::parseGameMove(line, sizeof(line), out);
}

constexpr char kStartFen[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

void testHeader() {
  pondemate::GameHeader h;
  expect(header("#\tmkida\tlichess AI level 3\t0-1\t2026.09.12\t82\tw", h), "header: good line");
  expect(strcmp(h.white, "mkida") == 0 && strcmp(h.black, "lichess AI level 3") == 0, "header: names");
  expect(strcmp(h.result, "0-1") == 0 && strcmp(h.date, "2026.09.12") == 0, "header: result, date");
  expect(h.plies == 82 && h.whiteAtBottom, "header: plies, orientation w");
  expect(header("#\ta\tb\t1/2-1/2\t?\t0\tb", h) && !h.whiteAtBottom && h.plies == 0, "header: orientation b, 0 plies");

  expect(!header("#\ta\tb\t0-1\t2026.09.12\t82", h), "header: 6 fields rejected");
  expect(!header("#\ta\tb\t0-1\t2026.09.12\t82\tw\tx", h), "header: 8 fields rejected");
  expect(!header("x\ta\tb\t0-1\t2026.09.12\t82\tw", h), "header: no # rejected");
  expect(!header("#\ta\tb\t2-0\t2026.09.12\t82\tw", h), "header: bad result rejected");
  expect(!header("#\ta\tb\t0-1\t2026.09.12\t8x\tw", h), "header: bad plies rejected");
  expect(!header("#\ta\tb\t0-1\t2026.09.12\t82\tx", h), "header: bad orientation rejected");
  expect(!header("#\t\tb\t0-1\t2026.09.12\t82\tw", h), "header: empty name rejected");
  expect(!header("#\tabcdefghijklmnopqrstuvwxyzABCDEF\tb\t0-1\t?\t1\tw", h), "header: 32-char name rejected");
  expect(!header("#\ta\tb\t0-1\t?\t10000\tw", h), "header: too many plies rejected");

  char line[pondemate::kGameLineBytes];
  makeLine("#\ta\tb\t0-1\t?\t1\tw", line);
  expect(!pondemate::parseGameHeader(line, sizeof(line) - 1, h), "header: 127 bytes rejected");
  line[sizeof(line) - 1] = ' ';
  expect(!pondemate::parseGameHeader(line, sizeof(line), h), "header: no newline rejected");
}

void testMove() {
  pondemate::GameMove m;
  char text[160];
  snprintf(text, sizeof(text), "0\t-\t-\t%s\t-\t-", kStartFen);
  expect(move(text, m), "move: ply 0");
  expect(m.ply == 0 && m.san[0] == '\0' && m.uci[0] == '\0' && m.eval[0] == '\0' && m.mark[0] == '\0',
         "move: ply 0 dashes become empty");
  expect(strcmp(m.fen, kStartFen) == 0, "move: ply 0 FEN");

  expect(move("5\tBf4\tc1f4\trnbqkb1r/ppp1pppp/3p1n2/8/3PPB2/8/PPP2PPP/RN1QKBNR b KQkq - 1 3\t-0.76\t?", m),
         "move: eval and mark");
  expect(m.ply == 5 && strcmp(m.san, "Bf4") == 0 && strcmp(m.uci, "c1f4") == 0, "move: ply, SAN, UCI");
  expect(strcmp(m.eval, "-0.76") == 0 && strcmp(m.mark, "?") == 0, "move: eval, mark values");
  expect(move("64\tb1=Q#\tb2b1q\t6k1/pp3p1p/8/5p2/P1P3n1/8/5bP1/1q5K w - - 0 33\t#-3\t??", m), "move: promotion, mate eval");
  expect(strcmp(m.uci, "b2b1q") == 0 && strcmp(m.eval, "#-3") == 0 && strcmp(m.mark, "??") == 0,
         "move: promotion values");
  expect(move("9\tO-O\te1g1\t8/8/8/8/8/8/8/5RK1 b - - 1 5\t12\t!?", m), "move: integer eval");

  const char* good = "5\tBf4\tc1f4\t8/8/8/8/8/8/8/8 b - - 1 3\t-0.76\t?";
  expect(move(good, m), "move: baseline");
  expect(!move("5\tBf4\tc1f4\t8/8/8/8/8/8/8/8 b - - 1 3\t-0.76", m), "move: 5 fields rejected");
  expect(!move("5\tBf4\tc1f4\t8/8/8/8/8/8/8/8 b - - 1 3\t-0.76\t?\tx", m), "move: 7 fields rejected");
  expect(!move("5\tBf4\tc1f9\t8/8/8/8/8/8/8/8 b - - 1 3\t-\t-", m), "move: bad UCI rejected");
  expect(!move("5\te8=K\te7e8k\t8/8/8/8/8/8/8/8 b - - 1 3\t-\t-", m), "move: bad promotion piece rejected");
  expect(!move("5\t-\tc1f4\t8/8/8/8/8/8/8/8 b - - 1 3\t-\t-", m), "move: missing SAN rejected");
  expect(!move("0\te4\te2e4\t8/8/8/8/8/8/8/8 b - - 1 3\t-\t-", m), "move: ply 0 with a move rejected");
  expect(!move("5\tBf4\tc1f4\t8/8/8/8/8/8/8/8 b - - 1 3\t-0.7x\t-", m), "move: bad eval rejected");
  expect(!move("5\tBf4\tc1f4\t8/8/8/8/8/8/8/8 b - - 1 3\t#\t-", m), "move: bare # eval rejected");
  expect(!move("5\tBf4\tc1f4\t8/8/8/8/8/8/8/8 b - - 1 3\t-\t?!?", m), "move: bad mark rejected");
  expect(!move("5\tBf4\tc1f4\t\t-\t-", m), "move: empty FEN rejected");
  expect(!move("x5\tBf4\tc1f4\t8/8/8/8/8/8/8/8 b - - 1 3\t-\t-", m), "move: bad ply rejected");

  char line[pondemate::kGameLineBytes + 1];
  makeLine(good, line);
  expect(!pondemate::parseGameMove(line, pondemate::kGameLineBytes - 1, m), "move: 127 bytes rejected");
  line[pondemate::kGameLineBytes] = '\n';
  expect(!pondemate::parseGameMove(line, pondemate::kGameLineBytes + 1, m), "move: 129 bytes rejected");
  makeLine(good, line);
  line[100] = '\x80';
  expect(!pondemate::parseGameMove(line, pondemate::kGameLineBytes, m), "move: non-ASCII byte rejected");
  makeLine(good, line);
  line[3] = '\n';
  expect(!pondemate::parseGameMove(line, pondemate::kGameLineBytes, m), "move: newline inside rejected");
}

void testFormat() {
  pondemate::GameMove m;
  chess::Position after;
  char buf[64];

  expect(move("23\tNf3\tg1f3\t8/8/8/8/8/5N2/8/8 b - - 1 12\t-0.76\t?", m) && chess::parseFen(m.fen, after),
         "format: white move parses");
  pondemate::formatGameMove(m, after, 58, buf, sizeof(buf));
  expect(strcmp(buf, "12. Nf3?  -0.76  (23/58)") == 0, "format: white move with mark and eval");

  expect(move("24\tNf6\tg8f6\t8/8/5n2/8/8/5N2/8/8 w - - 2 13\t-\t-", m) && chess::parseFen(m.fen, after),
         "format: black move parses");
  pondemate::formatGameMove(m, after, 58, buf, sizeof(buf));
  expect(strcmp(buf, "12... Nf6  (24/58)") == 0, "format: black move without mark or eval");

  char text[160];
  snprintf(text, sizeof(text), "0\t-\t-\t%s\t-\t-", kStartFen);
  expect(move(text, m) && chess::parseFen(m.fen, after), "format: start parses");
  pondemate::formatGameMove(m, after, 58, buf, sizeof(buf));
  expect(strcmp(buf, "Start  (0/58)") == 0, "format: start");

  expect(pondemate::gameLineOffset(0) == 128 && pondemate::gameLineOffset(5) == 768, "offset of a ply's line");
}

}  // namespace

int main() {
  printf("sizeof(GameHeader) = %zu, sizeof(GameMove) = %zu\n", sizeof(pondemate::GameHeader),
         sizeof(pondemate::GameMove));
  testHeader();
  testMove();
  testFormat();
  printf("game files: %d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
