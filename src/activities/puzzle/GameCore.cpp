#include "GameCore.h"

#include <cstdio>
#include <cstring>

namespace pondemate {

namespace {

constexpr int kMaxFields = 7;

struct Field {
  const char* text;
  int len;
};

bool isDigit(const char c) { return c >= '0' && c <= '9'; }
bool isFile(const char c) { return c >= 'a' && c <= 'h'; }
bool isRank(const char c) { return c >= '1' && c <= '8'; }

// Checks the length, the terminator and the bytes, drops the space padding and
// splits on TABs. Returns the field count, or -1 when the line is malformed or
// has more than kMaxFields fields.
int splitLine(const char* line, const size_t len, Field* fields) {
  if (line == nullptr || len != kGameLineBytes || line[kGameLineBytes - 1] != '\n') return -1;
  int end = kGameLineBytes - 1;
  for (int i = 0; i < end; i++) {
    const char c = line[i];
    if (c != '\t' && (c < 32 || c > 126)) return -1;
  }
  while (end > 0 && line[end - 1] == ' ') end--;

  int count = 0;
  int start = 0;
  for (int i = 0; i <= end; i++) {
    if (i < end && line[i] != '\t') continue;
    if (count == kMaxFields) return -1;
    fields[count++] = {line + start, i - start};
    start = i + 1;
  }
  return count;
}

bool copyField(const Field& f, char* out, const int maxChars) {
  if (f.len <= 0 || f.len > maxChars) return false;
  memcpy(out, f.text, f.len);
  out[f.len] = '\0';
  return true;
}

bool isDash(const Field& f) { return f.len == 1 && f.text[0] == '-'; }

// "-" becomes an empty string.
bool copyOptional(const Field& f, char* out, const int maxChars) {
  if (isDash(f)) {
    out[0] = '\0';
    return true;
  }
  return copyField(f, out, maxChars);
}

bool parseCount(const Field& f, const int max, int& out) {
  if (f.len <= 0 || f.len > 4) return false;
  int value = 0;
  for (int i = 0; i < f.len; i++) {
    if (!isDigit(f.text[i])) return false;
    value = value * 10 + (f.text[i] - '0');
  }
  if (value > max) return false;
  out = value;
  return true;
}

bool isUci(const Field& f) {
  if (f.len != 4 && f.len != 5) return false;
  const char* s = f.text;
  if (!isFile(s[0]) || !isRank(s[1]) || !isFile(s[2]) || !isRank(s[3])) return false;
  return f.len == 4 || strchr("qrbn", s[4]) != nullptr;
}

// "0.15", "-2.34", "12", "#3", "#-3".
bool isEval(const Field& f) {
  int i = 0;
  if (f.len > 0 && f.text[0] == '#') i = 1;
  if (i < f.len && f.text[i] == '-') i++;
  const int digitsStart = i;
  while (i < f.len && isDigit(f.text[i])) i++;
  if (i == digitsStart) return false;
  if (f.text[0] == '#') return i == f.len;
  if (i == f.len) return true;
  if (f.text[i] != '.') return false;
  const int fraction = ++i;
  while (i < f.len && isDigit(f.text[i])) i++;
  return i > fraction && i == f.len;
}

bool isMark(const Field& f) {
  static constexpr const char* kMarks[] = {"!", "?", "!!", "??", "!?", "?!"};
  for (const char* mark : kMarks) {
    if (static_cast<int>(strlen(mark)) == f.len && memcmp(mark, f.text, f.len) == 0) return true;
  }
  return false;
}

bool isResult(const Field& f) {
  static constexpr const char* kResults[] = {"1-0", "0-1", "1/2-1/2", "*"};
  for (const char* result : kResults) {
    if (static_cast<int>(strlen(result)) == f.len && memcmp(result, f.text, f.len) == 0) return true;
  }
  return false;
}

}  // namespace

bool parseGameHeader(const char* line, const size_t len, GameHeader& out) {
  Field f[kMaxFields];
  if (splitLine(line, len, f) != 7) return false;
  if (f[0].len != 1 || f[0].text[0] != '#') return false;
  GameHeader h{};
  if (!copyField(f[1], h.white, kMaxNameChars) || !copyField(f[2], h.black, kMaxNameChars)) return false;
  if (!isResult(f[3]) || !copyField(f[3], h.result, kMaxResultChars)) return false;
  if (!copyField(f[4], h.date, kMaxDateChars)) return false;
  if (!parseCount(f[5], kMaxGamePlies, h.plies)) return false;
  if (f[6].len != 1 || (f[6].text[0] != 'w' && f[6].text[0] != 'b')) return false;
  h.whiteAtBottom = f[6].text[0] == 'w';
  out = h;
  return true;
}

bool parseGameMove(const char* line, const size_t len, GameMove& out) {
  Field f[kMaxFields];
  if (splitLine(line, len, f) != 6) return false;
  GameMove m{};
  if (!parseCount(f[0], kMaxGamePlies, m.ply)) return false;
  if (m.ply == 0) {
    if (!isDash(f[1]) || !isDash(f[2])) return false;
  } else if (isDash(f[1]) || !isUci(f[2])) {
    return false;
  }
  if (!copyOptional(f[1], m.san, kMaxSanChars) || !copyOptional(f[2], m.uci, 5)) return false;
  if (!copyField(f[3], m.fen, kMaxGameFenChars)) return false;
  if (!isDash(f[4]) && !isEval(f[4])) return false;
  if (!isDash(f[5]) && !isMark(f[5])) return false;
  if (!copyOptional(f[4], m.eval, kMaxEvalChars) || !copyOptional(f[5], m.mark, kMaxMarkChars)) return false;
  out = m;
  return true;
}

void formatGameMove(const GameMove& move, const chess::Position& after, const int plies, char* buf, const size_t n) {
  if (n == 0) return;
  if (move.ply == 0) {
    snprintf(buf, n, "Start  (0/%d)", plies);
    return;
  }
  // White just moved when black is to move; the FEN's move number counts up after black's move.
  const bool whiteMoved = !after.whiteToMove;
  const int number = whiteMoved ? after.fullmoveNumber : after.fullmoveNumber - 1;
  char eval[kMaxEvalChars + 3] = "";
  if (move.eval[0] != '\0') snprintf(eval, sizeof(eval), "  %s", move.eval);
  snprintf(buf, n, "%d%s %s%s%s  (%d/%d)", number, whiteMoved ? "." : "...", move.san, move.mark, eval, move.ply,
           plies);
}

}  // namespace pondemate
