// Validates PondeMate game files (tools/pondemate/pgn2game.py) with the
// device's line parser (GameCore) and ChessCore.
//
//   validate_games game.txt...
//
// For each file: the size is (plies + 2) * 128 bytes, every line parses, line
// n + 1 holds ply n, every FEN parses, and from ply 1 on the UCI move is legal
// in the previous line's position, its SAN matches ChessCore's, and playing it
// gives the line's FEN (pieces, side to move and castling rights; the en
// passant field is not compared because writers differ on when to fill it).
//
// Exits non-zero if any file fails.

#include <cstdio>
#include <cstring>

#include "ChessCore.h"
#include "GameCore.h"

namespace {

chess::MoveList legal;  // ~1KB; static like on the device

bool samePosition(const chess::Position& a, const chess::Position& b) {
  return memcmp(a.squares, b.squares, sizeof(a.squares)) == 0 && a.whiteToMove == b.whiteToMove &&
         a.castling == b.castling;
}

// Returns true when the file is valid; prints the first problem otherwise.
bool checkFile(const char* path, int& pliesOut, int& evalsOut, int& marksOut, int& specialsOut) {
  FILE* f = fopen(path, "rb");
  if (f == nullptr) {
    printf("%s: cannot open\n", path);
    return false;
  }
  char line[pondemate::kGameLineBytes];
  pondemate::GameHeader header;
  if (fread(line, 1, sizeof(line), f) != sizeof(line) || !pondemate::parseGameHeader(line, sizeof(line), header)) {
    printf("%s: bad header\n", path);
    fclose(f);
    return false;
  }
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  const long expected = static_cast<long>(pondemate::gameLineOffset(header.plies + 1));
  if (size != expected) {
    printf("%s: %ld bytes, expected %ld for %d plies\n", path, size, expected, header.plies);
    fclose(f);
    return false;
  }

  chess::Position before;
  chess::Position after;
  pondemate::GameMove move;
  int evals = 0, marks = 0, specials = 0;
  for (int ply = 0; ply <= header.plies; ply++) {
    fseek(f, static_cast<long>(pondemate::gameLineOffset(ply)), SEEK_SET);
    if (fread(line, 1, sizeof(line), f) != sizeof(line) || !pondemate::parseGameMove(line, sizeof(line), move)) {
      printf("%s: ply %d: bad line\n", path, ply);
      fclose(f);
      return false;
    }
    if (move.ply != ply) {
      printf("%s: line for ply %d says %d\n", path, ply, move.ply);
      fclose(f);
      return false;
    }
    if (!chess::parseFen(move.fen, after)) {
      printf("%s: ply %d: bad FEN %s\n", path, ply, move.fen);
      fclose(f);
      return false;
    }
    if (ply > 0) {
      legal.clear();
      chess::generateLegalMoves(before, legal);
      int match = -1;
      for (int i = 0; i < legal.count; i++) {
        char uci[6];
        chess::moveToString(legal.moves[i], uci);
        if (strcmp(uci, move.uci) == 0) match = i;
      }
      if (match < 0) {
        printf("%s: ply %d: %s is not legal\n", path, ply, move.uci);
        fclose(f);
        return false;
      }
      const chess::Move& m = legal.moves[match];
      char san[10];
      chess::moveToSan(before, m, san);
      if (strcmp(san, move.san) != 0) {
        printf("%s: ply %d: SAN %s, ChessCore says %s\n", path, ply, move.san, san);
        fclose(f);
        return false;
      }
      if (m.flags & (chess::FlagCastle | chess::FlagPromotion | chess::FlagEnPassant)) specials++;
      chess::Undo undo;
      chess::makeMove(before, m, undo);
      if (!samePosition(before, after)) {
        char fen[96];
        chess::positionToFen(before, fen);
        printf("%s: ply %d: %s gives %s, file has %s\n", path, ply, move.uci, fen, move.fen);
        fclose(f);
        return false;
      }
      evals += move.eval[0] != '\0';
      marks += move.mark[0] != '\0';
    }
    before = after;
  }
  fclose(f);
  pliesOut = header.plies;
  evalsOut = evals;
  marksOut = marks;
  specialsOut = specials;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  int ok = 0, failed = 0, longest = 0, withEval = 0;
  for (int i = 1; i < argc; i++) {
    int plies = 0, evals = 0, marks = 0, specials = 0;
    if (checkFile(argv[i], plies, evals, marks, specials)) {
      ok++;
      if (plies > longest) longest = plies;
      if (evals > 0) withEval++;
      const char* name = strrchr(argv[i], '/');
      printf("%s: %d plies OK (eval %d, marks %d, castle/promo/ep %d)\n", name ? name + 1 : argv[i], plies, evals,
             marks, specials);
    } else {
      failed++;
    }
  }
  printf("games: %d OK, %d failed, longest %d plies, %d with eval\n", ok, failed, longest, withEval);
  return failed == 0 && ok > 0 ? 0 : 1;
}
