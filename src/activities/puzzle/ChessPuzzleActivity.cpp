#include "ChessPuzzleActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "PieceBitmaps.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr char kSetsDir[] = "/pondemate/sets";
constexpr char kProgressDir[] = "/pondemate/progress";
constexpr char kSettingsPath[] = "/pondemate/settings.txt";
constexpr char kLogPath[] = "/pondemate/log.txt";
constexpr char kSetSuffix[] = ".txt";

struct TestPosition {
  const char* name;
  const char* fen;
  int perftDepth;  // kept shallow so it stays quick on the device
};

constexpr TestPosition kPositions[] = {
    {"P1 start", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 3},
    {"P2 kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2},
};
constexpr int kPositionCount = sizeof(kPositions) / sizeof(kPositions[0]);

// PieceBitmaps.h only carries pre-shifted bitmaps for shifts {1,3,5,7}, which assume the board top
// at y = 10. Following the theme's topPadding would push other themes onto the per-pixel fallback.
constexpr int kBoardTop = 10;
// Marks live in the 3px band inside each square; the piece box starts at (3, 3).
constexpr int kMarkWidth = 3;
constexpr int kCornerArm = 14;

const char* const kPieceNames[7] = {"", "Pawn", "Knight", "Bishop", "Rook", "Queen", "King"};

// Sets every pixel whose bit is on in a PIECE_SIZE x PIECE_SIZE 1bpp bitmap.
void drawPieceBits(const GfxRenderer& renderer, const int x, const int y, const uint8_t* bits, const bool black) {
  for (int row = 0; row < PIECE_SIZE; row++) {
    const uint8_t* rowBits = bits + row * PIECE_ROW_BYTES;
    for (int col = 0; col < PIECE_SIZE; col++) {
      if (rowBits[col >> 3] & (0x80 >> (col & 7))) renderer.drawPixel(x + col, y + row, black);
    }
  }
}

// Writes a pre-rotated, pre-shifted piece straight into the Portrait framebuffer: the halo mask is
// OR-ed in (bits set = white), then the ink mask is cleared out (bits clear = black). Returns false
// when the bitmap for this shift is missing or the box is not fully inside the panel, so the caller
// can fall back to drawPieceBits().
bool blitPiece(const GfxRenderer& renderer, const int x, const int y, const uint8_t piece) {
  if (renderer.getOrientation() != GfxRenderer::Portrait) return false;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  const int panelHeight = renderer.getDisplayHeight();
  const int widthBytes = renderer.getDisplayWidthBytes();
  // A tiled grayscale strip redirects drawing to a band buffer; only blit the full framebuffer.
  if (frameBuffer == nullptr || renderer.getWriteTarget() != frameBuffer || renderer.getWriteRows() != panelHeight) {
    return false;
  }
  if (x < 0 || y < 0) return false;

  const uint8_t* ink = PIECE_INK_SHIFTED[piece & 15][y & 7];
  const uint8_t* halo = PIECE_HALO_SHIFTED[piece & 15][y & 7];
  if (ink == nullptr || halo == nullptr) return false;

  // Portrait: phyX = y, phyY = panelHeight - 1 - x.
  const int firstRow = panelHeight - x - PIECE_SIZE;
  const int firstByte = y >> 3;
  if (firstRow < 0 || firstByte + PIECE_SHIFTED_ROW_BYTES > widthBytes) return false;

  for (int row = 0; row < PIECE_SIZE; row++) {
    uint8_t* dst = frameBuffer + (firstRow + row) * widthBytes + firstByte;
    const uint8_t* inkRow = ink + row * PIECE_SHIFTED_ROW_BYTES;
    const uint8_t* haloRow = halo + row * PIECE_SHIFTED_ROW_BYTES;
    for (int i = 0; i < PIECE_SHIFTED_ROW_BYTES; i++) {
      dst[i] = static_cast<uint8_t>((dst[i] | haloRow[i]) & ~inkRow[i]);
    }
  }
  return true;
}

void squareName(const uint8_t sq, char* out) {
  out[0] = static_cast<char>('a' + (sq & 7));
  out[1] = static_cast<char>('1' + (sq >> 3));
  out[2] = '\0';
}

// Top-left pixel of a board square as drawn (black at the bottom when !whiteAtBottom).
void squareOrigin(const uint8_t sq, const bool whiteAtBottom, const int square, int& x, int& y) {
  const int col = whiteAtBottom ? (sq & 7) : 7 - (sq & 7);
  const int row = whiteAtBottom ? 7 - (sq >> 3) : (sq >> 3);
  x = col * square;
  y = kBoardTop + row * square;
}

// Last-move mark: an L in each corner, 2px black inside a 1px white line on the square's edge so it
// never merges with the board border or a neighbouring square's frame. Stays within the 3px band
// outside the piece box.
void drawCorners(const GfxRenderer& renderer, const int x, const int y, const int size) {
  const int far = size - 1;
  const int armFar = size - kCornerArm;
  // White outer line.
  renderer.fillRect(x, y, kCornerArm, 1, false);
  renderer.fillRect(x, y, 1, kCornerArm, false);
  renderer.fillRect(x + armFar, y, kCornerArm, 1, false);
  renderer.fillRect(x + far, y, 1, kCornerArm, false);
  renderer.fillRect(x, y + far, kCornerArm, 1, false);
  renderer.fillRect(x, y + armFar, 1, kCornerArm, false);
  renderer.fillRect(x + armFar, y + far, kCornerArm, 1, false);
  renderer.fillRect(x + far, y + armFar, 1, kCornerArm, false);
  // Black L, 2px, just inside.
  constexpr int kInk = kMarkWidth - 1;
  const int inkFar = size - kMarkWidth;
  renderer.fillRect(x + 1, y + 1, kCornerArm - 1, kInk);
  renderer.fillRect(x + 1, y + 1, kInk, kCornerArm - 1);
  renderer.fillRect(x + armFar, y + 1, kCornerArm - 1, kInk);
  renderer.fillRect(x + inkFar, y + 1, kInk, kCornerArm - 1);
  renderer.fillRect(x + 1, y + inkFar, kCornerArm - 1, kInk);
  renderer.fillRect(x + 1, y + armFar, kInk, kCornerArm - 1);
  renderer.fillRect(x + armFar, y + inkFar, kCornerArm - 1, kInk);
  renderer.fillRect(x + inkFar, y + armFar, kInk, kCornerArm - 1);
}

// Inverts a logical size x size square in the Portrait framebuffer by XOR-ing whole bytes.
// Logical (x, y) maps to physical (phyX = y, phyY = panelHeight - 1 - x), so the square covers
// physical rows panelHeight - x - size .. panelHeight - x - 1 and bits y .. y + size - 1 of each row.
// Returns false (nothing drawn) when the framebuffer cannot be written directly.
bool invertSquare(const GfxRenderer& renderer, const int x, const int y, const int size) {
  if (renderer.getOrientation() != GfxRenderer::Portrait) return false;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  const int panelHeight = renderer.getDisplayHeight();
  const int panelWidth = renderer.getDisplayWidth();
  const int widthBytes = renderer.getDisplayWidthBytes();
  if (frameBuffer == nullptr || renderer.getWriteTarget() != frameBuffer || renderer.getWriteRows() != panelHeight) {
    return false;
  }
  const int firstRow = panelHeight - x - size;
  const int lastBit = y + size - 1;
  if (x < 0 || y < 0 || firstRow < 0 || lastBit >= panelWidth) return false;

  for (int row = 0; row < size; row++) {
    uint8_t* line = frameBuffer + (firstRow + row) * widthBytes;
    for (int byte = y >> 3; byte <= (lastBit >> 3); byte++) {
      const int from = byte * 8 > y ? byte * 8 : y;
      const int to = byte * 8 + 7 < lastBit ? byte * 8 + 7 : lastBit;
      const uint8_t mask = static_cast<uint8_t>((0xFF >> (from & 7)) & (0xFF << (7 - (to & 7))));
      line[byte] ^= mask;
    }
  }
  return true;
}

pondemate::Dir toDir(const ChessPuzzleActivity::Action action) {
  switch (action) {
    case ChessPuzzleActivity::Action::Up:
      return pondemate::Dir::Up;
    case ChessPuzzleActivity::Action::Down:
      return pondemate::Dir::Down;
    case ChessPuzzleActivity::Action::Left:
      return pondemate::Dir::Left;
    default:
      return pondemate::Dir::Right;
  }
}

bool isDirection(const ChessPuzzleActivity::Action action) {
  return action == ChessPuzzleActivity::Action::Up || action == ChessPuzzleActivity::Action::Down ||
         action == ChessPuzzleActivity::Action::Left || action == ChessPuzzleActivity::Action::Right;
}

// Reads a small text file into `buf` (NUL-terminated). Returns false when missing or empty.
bool readSmallFile(const char* path, char* buf, const size_t size) {
  if (!Storage.exists(path)) return false;
  const size_t n = Storage.readFileToBuffer(path, buf, size, size - 1);
  buf[n < size ? n : size - 1] = '\0';
  return n > 0;
}

bool writeSmallFile(const char* path, const char* text) {
  HalFile file;
  if (!Storage.openFileForWrite("PONDE", path, file)) {
    LOG_ERR("PONDE", "Cannot write %s", path);
    return false;
  }
  const size_t len = strlen(text);
  return file.write(text, len) == len;
}

}  // namespace

// ---------------------------------------------------------------------------
// Input

// Standard: front Back/OK/Up/Down, side Left/Right (the X3's side Up key sits on the left edge).
// hjkl: front h/j/k/l = Left/Down/Up/Right, side OK/Back.
ChessPuzzleActivity::Action ChessPuzzleActivity::readAction() const {
  using Button = MappedInputManager::Button;
  const bool hjkl = layout == Layout::Hjkl;
  if (mappedInput.wasPressed(Button::Back)) return hjkl ? Action::Left : Action::Back;
  if (mappedInput.wasPressed(Button::Confirm)) return hjkl ? Action::Down : Action::Ok;
  if (mappedInput.wasPressed(Button::Left)) return Action::Up;
  if (mappedInput.wasPressed(Button::Right)) return hjkl ? Action::Right : Action::Down;
  if (mappedInput.wasPressed(Button::Up)) return hjkl ? Action::Ok : Action::Left;
  if (mappedInput.wasPressed(Button::Down)) return hjkl ? Action::Back : Action::Right;
  return Action::None;
}

// ---------------------------------------------------------------------------
// Storage

// One "key=value" per line; a missing "dither=" line means 25%.
void ChessPuzzleActivity::loadSettings() {
  char buf[48];
  layout = Layout::Standard;
  darkDither50 = false;
  if (!readSmallFile(kSettingsPath, buf, sizeof(buf))) return;
  if (strstr(buf, "hjkl") != nullptr) layout = Layout::Hjkl;
  if (strstr(buf, "dither=50") != nullptr) darkDither50 = true;
}

void ChessPuzzleActivity::saveSettings() const {
  Storage.mkdir("/pondemate");
  char text[48];
  snprintf(text, sizeof(text), "buttons=%s\ndither=%d\n", layout == Layout::Hjkl ? "hjkl" : "standard",
           darkDither50 ? 50 : 25);
  writeSmallFile(kSettingsPath, text);
}

void ChessPuzzleActivity::scanSets() {
  setCount = 0;
  HalFile dir = Storage.open(kSetsDir);
  if (!dir || !dir.isDirectory()) {
    LOG_INF("PONDE", "No set directory %s", kSetsDir);
    return;
  }

  char name[64];
  const size_t suffixLen = strlen(kSetSuffix);
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(name, sizeof(name));
    const size_t len = strlen(name);
    if (name[0] == '.' || len <= suffixLen || strcmp(name + len - suffixLen, kSetSuffix) != 0) continue;
    if (len - suffixLen >= static_cast<size_t>(kSetNameBytes)) continue;
    const size_t bytes = entry.fileSize();
    if (bytes == 0 || bytes % pondemate::kRecordBytes != 0) continue;
    if (setCount == kMaxSets) {
      LOG_INF("PONDE", "More than %d sets, ignoring %s", kMaxSets, name);
      continue;
    }

    // Insertion by name keeps the list sorted without another pass.
    name[len - suffixLen] = '\0';
    int at = setCount;
    while (at > 0 && strcmp(setNames[at - 1], name) > 0) {
      memcpy(setNames[at], setNames[at - 1], kSetNameBytes);
      setTotals[at] = setTotals[at - 1];
      at--;
    }
    snprintf(setNames[at], kSetNameBytes, "%s", name);
    setTotals[at] = static_cast<int>(bytes / pondemate::kRecordBytes);
    setCount++;
  }

  char path[96];
  char buf[16];
  for (int i = 0; i < setCount; i++) {
    snprintf(path, sizeof(path), "%s/%s.txt", kProgressDir, setNames[i]);
    int next = readSmallFile(path, buf, sizeof(buf)) ? atoi(buf) : 0;
    if (next < 0) next = 0;
    if (next > setTotals[i]) next = setTotals[i];
    setNext[i] = next;
  }
  if (pickerIndex > setCount + 3) pickerIndex = 0;
}

bool ChessPuzzleActivity::openPuzzle(const int set, const int index) {
  setIndex = set;
  puzzleIndex = index;
  hasPuzzle = false;
  menuOpen = false;
  hintCount = 0;
  statusText[0] = '\0';
  cursorText[0] = '\0';

  char path[96];
  snprintf(path, sizeof(path), "%s/%s.txt", kSetsDir, setNames[set]);
  HalFile file;
  const bool read = Storage.openFileForRead("PONDE", path, file) &&
                    file.seek(static_cast<size_t>(index) * pondemate::kRecordBytes) &&
                    file.read(recordBuf, pondemate::kRecordBytes) == pondemate::kRecordBytes;
  if (!read || !pondemate::parseRecord(recordBuf, pondemate::kRecordBytes, record) || !session.begin(record)) {
    LOG_ERR("PONDE", "Bad puzzle %s #%d", setNames[set], index);
    phase = Phase::Error;
    snprintf(statusText, sizeof(statusText), "Cannot load puzzle #%d", index + 1);
    return false;
  }
  hasPuzzle = true;
  LOG_INF("PONDE", "Puzzle %s #%d %s (%d)", setNames[set], index, record.id, record.rating);
  startTurn();
  return true;
}

// Moves the set's progress past the current puzzle and appends a log.txt line:
// set, index, id, rating, mistakes, hints, result ("solved" or "skip").
void ChessPuzzleActivity::recordResult(const char* result) {
  setNext[setIndex] = puzzleIndex + 1;
  Storage.mkdir(kProgressDir);
  char path[96];
  char text[16];
  snprintf(path, sizeof(path), "%s/%s.txt", kProgressDir, setNames[setIndex]);
  snprintf(text, sizeof(text), "%d\n", puzzleIndex + 1);
  writeSmallFile(path, text);

  char line[kLineBytes + kSetNameBytes];
  const int len = snprintf(line, sizeof(line), "%s\t%d\t%s\t%d\t%d\t%d\t%s\n", setNames[setIndex], puzzleIndex,
                           record.id, record.rating, session.mistakes(), hintCount, result);
  HalFile log = Storage.open(kLogPath, O_RDWR | O_CREAT);
  if (!log || !log.seek(log.fileSize()) || log.write(line, len) != static_cast<size_t>(len)) {
    LOG_ERR("PONDE", "Cannot append %s", kLogPath);
  }
}

// ---------------------------------------------------------------------------
// Diagnostics (serial only)

// Called from the loop task, so stackHW is the loop task's.
void ChessPuzzleActivity::logDiag(const char* scene) const {
  LOG_INF("PONDE", "diag %s stackHW=%u renderHW=%u heap=%u maxAlloc=%u minHeap=%u", scene,
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), static_cast<unsigned>(renderStackHW),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()));
}

// Logs the refresh that render() timed since the last call.
void ChessPuzzleActivity::logRefresh() {
  if (renderSeq == loggedRenderSeq) return;
  uint32_t ms;
  bool half;
  {
    RenderLock lock(*this);
    loggedRenderSeq = renderSeq;
    ms = refreshMs;
    half = refreshWasHalf;
  }
  LOG_INF("PONDE", "refresh %s ms=%lu", half ? "half" : "fast", static_cast<unsigned long>(ms));
}

// ---------------------------------------------------------------------------
// Puzzle state

void ChessPuzzleActivity::startTurn() {
  phase = Phase::SelectPiece;
  targetCount = session.movablePieces(targets);
  cursor = session.initialCursor();
  updateCursorText();
}

void ChessPuzzleActivity::updateCursorText() {
  char sq[3];
  const auto& board = session.position();
  switch (phase) {
    case Phase::SelectPiece:
      squareName(cursor, sq);
      snprintf(cursorText, sizeof(cursorText), "%s %s", kPieceNames[chess::pieceType(board.squares[cursor])], sq);
      break;
    case Phase::SelectDest: {
      chess::Move choices[4];
      if (session.promotionChoices(grabbed, cursor, choices) > 0) {
        session.displayText(choices[0], cursorText, sizeof(cursorText), true);
      }
      break;
    }
    case Phase::SelectPromo: {
      char move[16];
      session.displayText(promoChoices[promoIndex], move, sizeof(move), true);
      snprintf(cursorText, sizeof(cursorText), "%s  (promote: %d/%d)", move, promoIndex + 1, promoCount);
      break;
    }
    default:
      cursorText[0] = '\0';
      break;
  }
}

void ChessPuzzleActivity::handlePuzzle(const Action action) {
  const bool whiteAtBottom = session.playerIsWhite();

  if (action == Action::Back) {
    if (phase == Phase::SelectDest || phase == Phase::SelectPromo) {
      phase = Phase::SelectPiece;
      targetCount = session.movablePieces(targets);
      cursor = grabbed;
      updateCursorText();
      return;
    }
    if (phase == Phase::SelectPiece) {
      menuOpen = true;
      menuIndex = static_cast<int>(MenuItem::Resume);
      return;
    }
    mode = Mode::Picker;
    scanSets();
    return;
  }

  switch (phase) {
    case Phase::SelectPiece:
      if (isDirection(action)) {
        cursor = pondemate::snap(cursor, toDir(action), targets, targetCount, whiteAtBottom);
      } else if (action == Action::Ok) {
        grabbed = cursor;
        targetCount = session.destinations(grabbed, targets);
        cursor = session.initialDestination(grabbed);
        phase = Phase::SelectDest;
        statusText[0] = '\0';
      }
      updateCursorText();
      return;

    case Phase::SelectDest:
      if (isDirection(action)) {
        cursor = pondemate::snap(cursor, toDir(action), targets, targetCount, whiteAtBottom);
        updateCursorText();
        return;
      }
      if (action != Action::Ok || cursor == grabbed) return;
      promoCount = session.promotionChoices(grabbed, cursor, promoChoices);
      promoIndex = 0;
      if (promoCount > 1) {
        phase = Phase::SelectPromo;
        updateCursorText();
        return;
      }
      break;

    case Phase::SelectPromo:
      if (action == Action::Up && promoIndex > 0) promoIndex--;
      if (action == Action::Down && promoIndex + 1 < promoCount) promoIndex++;
      if (action != Action::Ok) {
        updateCursorText();
        return;
      }
      break;

    case Phase::Solved:
      if (action != Action::Ok) return;
      if (puzzleIndex + 1 >= setTotals[setIndex]) {
        phase = Phase::Complete;
        snprintf(statusText, sizeof(statusText), "Set complete");
        cursorText[0] = '\0';
      } else {
        openPuzzle(setIndex, puzzleIndex + 1);
        halfRefreshPending = true;
      }
      return;

    default:
      if (action == Action::Ok) {
        mode = Mode::Picker;
        scanSets();
      }
      return;
  }

  // A move was chosen in SelectDest or SelectPromo.
  const chess::Move move = promoChoices[promoIndex];
  playerMoveBoard = session.position();
  switch (session.play(move)) {
    case pondemate::PlayResult::Wrong:
      startTurn();
      snprintf(statusText, sizeof(statusText), "Wrong - try again (mistakes: %d)", session.mistakes());
      logDiag("wrong");
      break;
    case pondemate::PlayResult::Correct: {
      chess::Undo undo;
      chess::makeMove(playerMoveBoard, move, undo);
      playerMoveFrom = move.from;
      playerMoveTo = move.to;
      showingPlayerMove = true;
      replyTimerStarted = false;
      playerMoveSeq = renderSeq;
      startTurn();
      snprintf(statusText, sizeof(statusText), "Correct - keep going");
      logDiag("correct");
      break;
    }
    case pondemate::PlayResult::Solved:
      phase = Phase::Solved;
      cursorText[0] = '\0';
      snprintf(statusText, sizeof(statusText), "Solved! (mistakes: %d)  OK: next", session.mistakes());
      recordResult("solved");
      logDiag("solved");
      break;
  }
}

// Up/Down wrap, OK runs the item, Back is Resume.
void ChessPuzzleActivity::handleMenu(const Action action) {
  if (action == Action::Up) menuIndex = (menuIndex + kMenuItems - 1) % kMenuItems;
  if (action == Action::Down) menuIndex = (menuIndex + 1) % kMenuItems;
  if (action == Action::Back) {
    menuOpen = false;
    return;
  }
  if (action != Action::Ok) return;

  menuOpen = false;
  switch (static_cast<MenuItem>(menuIndex)) {
    case MenuItem::Resume:
      return;
    case MenuItem::Hint: {
      // The solution's piece only; the destination stays for the player to find.
      chess::Move hint;
      if (!session.expectedMove(hint)) return;
      hintCount++;
      cursor = hint.from;
      char sq[3];
      squareName(cursor, sq);
      snprintf(cursorText, sizeof(cursorText), "Hint: %s %s",
               kPieceNames[chess::pieceType(session.position().squares[cursor])], sq);
      LOG_INF("PONDE", "hint %d on %s", hintCount, record.id);
      return;
    }
    case MenuItem::Skip:
      recordResult("skip");
      LOG_INF("PONDE", "skip %s", record.id);
      if (puzzleIndex + 1 >= setTotals[setIndex]) {
        phase = Phase::Complete;
        snprintf(statusText, sizeof(statusText), "Set complete");
        cursorText[0] = '\0';
      } else {
        openPuzzle(setIndex, puzzleIndex + 1);
        halfRefreshPending = true;
      }
      return;
    case MenuItem::Sets:
      mode = Mode::Picker;
      scanSets();
      return;
  }
}

// Removes every *.txt under the progress directory, one directory scan per file so nothing is
// removed while the directory is open. log.txt, settings.txt and the sets are not touched.
void ChessPuzzleActivity::resetProgress() {
  char name[64];
  char path[96];
  int removed = 0;
  const size_t suffixLen = strlen(kSetSuffix);
  for (int guard = 0; guard < 256; guard++) {
    path[0] = '\0';
    {
      HalFile dir = Storage.open(kProgressDir);
      if (!dir || !dir.isDirectory()) break;
      for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
        if (entry.isDirectory()) continue;
        entry.getName(name, sizeof(name));
        const size_t len = strlen(name);
        if (len <= suffixLen || strcmp(name + len - suffixLen, kSetSuffix) != 0) continue;
        snprintf(path, sizeof(path), "%s/%s", kProgressDir, name);
        break;
      }
    }
    if (path[0] == '\0') break;
    if (!Storage.remove(path)) {
      LOG_ERR("PONDE", "Cannot remove %s", path);
      break;
    }
    removed++;
  }
  LOG_INF("PONDE", "reset progress: removed %d files", removed);
}

void ChessPuzzleActivity::handlePicker(const Action action) {
  if (confirmingReset) {
    if (action == Action::Ok) {
      resetProgress();
      scanSets();
    }
    if (action == Action::Ok || action == Action::Back) confirmingReset = false;
    return;
  }
  const int rows = setCount + 4;
  if (action == Action::Up) pickerIndex = pickerIndex > 0 ? pickerIndex - 1 : rows - 1;
  if (action == Action::Down) pickerIndex = pickerIndex + 1 < rows ? pickerIndex + 1 : 0;
  if (action != Action::Ok) return;

  if (pickerIndex < setCount) {
    mode = Mode::Puzzle;
    if (setNext[pickerIndex] >= setTotals[pickerIndex]) {
      setIndex = pickerIndex;
      puzzleIndex = setTotals[pickerIndex] - 1;
      hasPuzzle = false;
      phase = Phase::Complete;
      cursorText[0] = '\0';
      snprintf(statusText, sizeof(statusText), "Set complete");
    } else {
      openPuzzle(pickerIndex, setNext[pickerIndex]);
    }
  } else if (pickerIndex == setCount) {
    layout = layout == Layout::Standard ? Layout::Hjkl : Layout::Standard;
    saveSettings();
  } else if (pickerIndex == setCount + 1) {
    darkDither50 = !darkDither50;
    saveSettings();
  } else if (pickerIndex == setCount + 2) {
    confirmingReset = true;
  } else {
    mode = Mode::Debug;
    loadPosition(0);
  }
}

void ChessPuzzleActivity::handleDebug(const Action action) {
  if (action == Action::Back) {
    mode = Mode::Picker;
    scanSets();
    return;
  }
  if (action == Action::Up || action == Action::Down) {
    const int step = action == Action::Up ? -1 : 1;
    loadPosition((positionIndex + step + kPositionCount) % kPositionCount);
  } else if (action == Action::Ok) {
    // Holding the render lock (taken by loop()) keeps the screen untouched until perft finishes.
    runPerft();
  }
}

// ---------------------------------------------------------------------------
// Debug: Phase 1 perft check

void ChessPuzzleActivity::loadPosition(const int index) {
  positionIndex = index;
  if (!chess::parseFen(kPositions[index].fen, position)) {
    LOG_ERR("CHESS", "Bad FEN: %s", kPositions[index].fen);
    chess::setStartPosition(position);
  }
  legalMoves.clear();
  chess::generateLegalMoves(position, legalMoves);

  sanCount = legalMoves.count < kMaxSanMoves ? legalMoves.count : kMaxSanMoves;
  for (int i = 0; i < sanCount; i++) {
    chess::moveToSan(position, legalMoves.moves[i], san[i]);
  }
  perftDone = false;
}

// The root ply walks the member move list so chess::perft() only needs
// depth - 1 levels of its ~1KB stack MoveList.
void ChessPuzzleActivity::runPerft() {
  const int depth = kPositions[positionIndex].perftDepth;
  const unsigned long start = millis();
  uint64_t nodes = 0;
  for (int i = 0; i < legalMoves.count; i++) {
    chess::Undo undo;
    chess::makeMove(position, legalMoves.moves[i], undo);
    nodes += chess::perft(position, depth - 1);
    chess::unmakeMove(position, legalMoves.moves[i], undo);
  }
  perftMs = static_cast<uint32_t>(millis() - start);
  perftDepth = depth;
  perftNodes = nodes;
  perftDone = true;
  LOG_INF("CHESS", "perft(%d) %s = %llu in %lu ms", depth, kPositions[positionIndex].name,
          static_cast<unsigned long long>(nodes), static_cast<unsigned long>(perftMs));
  LOG_INF("PONDE", "perft %s d%d nodes=%llu ms=%lu stackHW=%u heap=%u maxAlloc=%u minHeap=%u",
          kPositions[positionIndex].name, depth, static_cast<unsigned long long>(nodes),
          static_cast<unsigned long>(perftMs), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()),
          static_cast<unsigned>(ESP.getMinFreeHeap()));
}

// ---------------------------------------------------------------------------
// Lifecycle

void ChessPuzzleActivity::onEnter() {
  Activity::onEnter();
  loadSettings();
  scanSets();
  halfRefreshPending = true;
  requestUpdateAndWait();
  logDiag("picker");
}

void ChessPuzzleActivity::loop() {
  logRefresh();
  if (showingPlayerMove) {
    // The delay starts once the player's board is on the panel.
    if (!replyTimerStarted) {
      if (renderSeq == playerMoveSeq) return;
      replyTimerStarted = true;
      replyTimerStart = millis();
      return;
    }
    if (millis() - replyTimerStart < kReplyDelayMs) return;
    {
      RenderLock lock(*this);
      showingPlayerMove = false;
    }
    requestUpdate();
    return;
  }
  const Action action = readAction();
  if (action == Action::None) return;

  if (mode == Mode::Picker && action == Action::Back && !confirmingReset) {
    onGoHome();
    return;
  }
  {
    RenderLock lock(*this);
    const Mode before = mode;
    switch (mode) {
      case Mode::Picker:
        handlePicker(action);
        break;
      case Mode::Puzzle:
        if (menuOpen) {
          handleMenu(action);
        } else {
          handlePuzzle(action);
        }
        break;
      case Mode::Debug:
        handleDebug(action);
        break;
    }
    if (mode != before) {
      halfRefreshPending = true;
      if (before == Mode::Puzzle) logDiag("exit");
      if (mode == Mode::Puzzle) logDiag("enter");
      if (mode == Mode::Picker) logDiag("picker");
    }
  }
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Drawing

void ChessPuzzleActivity::drawBoard(const chess::Position& board, const bool whiteAtBottom, const bool marks) const {
  const int square = renderer.getScreenWidth() / 8;
  const int boardSize = square * 8;
  const int pieceInset = (square - PIECE_SIZE) / 2;
  const unsigned long boardStart = millis();
  int blitted = 0;
  int fallbacks = 0;
  const bool holding = marks && (phase == Phase::SelectDest || phase == Phase::SelectPromo);
  bool groundDrawn = true;
  const Color darkColor = darkDither50 ? Color::DarkGray : Color::LightGray;

  for (int row = 0; row < 8; row++) {
    for (int col = 0; col < 8; col++) {
      const int x = col * square;
      const int y = kBoardTop + row * square;
      const int sq = whiteAtBottom ? (7 - row) * 8 + col : row * 8 + (7 - col);
      // a1 is dark either way up: (row + col) is odd exactly when rank + file is even.
      const bool dark = ((row + col) & 1) != 0;
      const bool grabbedHere = holding && sq == grabbed;
      // Grabbed square: the 25% dither inverted gives a 75% ground (only x, y both even stay white)
      // whichever dither the dark squares use; the piece is then drawn normally on top so its colour
      // still reads right.
      if (grabbedHere) {
        renderer.fillRectDither(x, y, square, square, Color::LightGray);
        groundDrawn = invertSquare(renderer, x, y, square);
        if (!groundDrawn) {
          LOG_ERR("CHESS", "cannot darken the grabbed square");
          if (dark) {
            renderer.fillRectDither(x, y, square, square, darkColor);
          } else {
            renderer.fillRect(x, y, square, square, false);
          }
        }
      } else if (dark) {
        renderer.fillRectDither(x, y, square, square, darkColor);
      }

      const uint8_t piece = board.squares[sq];
      const uint8_t* ink = PIECE_INK_BY_CODE[piece & 15];
      const uint8_t* halo = PIECE_HALO_BY_CODE[piece & 15];
      if (ink == nullptr || halo == nullptr) continue;
      if (blitPiece(renderer, x + pieceInset, y + pieceInset, piece)) {
        blitted++;
        continue;
      }
      // Halo first so the white rim clears the dither, then the ink on top.
      drawPieceBits(renderer, x + pieceInset, y + pieceInset, halo, false);
      drawPieceBits(renderer, x + pieceInset, y + pieceInset, ink, true);
      fallbacks++;
    }
  }

  renderer.drawRect(0, kBoardTop, boardSize, boardSize, 2, true);

  // Marks go last so their white edge line also shows on the border squares.
  if (marks) {
    int x, y;
    squareOrigin(showingPlayerMove ? playerMoveFrom : session.lastFrom(), whiteAtBottom, square, x, y);
    drawCorners(renderer, x, y, square);
    squareOrigin(showingPlayerMove ? playerMoveTo : session.lastTo(), whiteAtBottom, square, x, y);
    drawCorners(renderer, x, y, square);
    if (!groundDrawn) {
      squareOrigin(grabbed, whiteAtBottom, square, x, y);
      renderer.drawRect(x, y, square, square, kMarkWidth, true);
    }
    const bool choosing = phase == Phase::SelectPiece || phase == Phase::SelectDest || phase == Phase::SelectPromo;
    if (choosing && !showingPlayerMove) {
      squareOrigin(cursor, whiteAtBottom, square, x, y);
      renderer.drawRect(x, y, square, square, kMarkWidth, true);
    }
  }

  if (fallbacks > 0) {
    LOG_INF("CHESS", "fallback per-pixel: %d pieces", fallbacks);
  }
  LOG_INF("CHESS", "board render %lu ms (blit %d, per-pixel %d)", static_cast<unsigned long>(millis() - boardStart),
          blitted, fallbacks);
}

// Front labels go to the theme's hint bar; the side keys are listed as a text line at `y`
// because the X3's side hints would sit on top of the board.
void ChessPuzzleActivity::drawHints(const char* back, const char* ok, const char* up, const char* down,
                                    const char* left, const char* right, const int y) const {
  const bool hjkl = layout == Layout::Hjkl;
  // mapLabels() places the logical Back/Confirm/Left/Right labels on the configured front keys.
  const auto labels = hjkl ? mappedInput.mapLabels(left, down, up, right) : mappedInput.mapLabels(back, ok, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const char* sideTop = hjkl ? ok : left;
  const char* sideBottom = hjkl ? back : right;
  if (sideTop[0] == '\0' && sideBottom[0] == '\0') return;
  char line[kLineBytes];
  snprintf(line, sizeof(line), "Side keys:  %s  |  %s", sideTop[0] ? sideTop : "-", sideBottom[0] ? sideBottom : "-");
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
}

void ChessPuzzleActivity::renderPicker() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowHeight = lineHeight + metrics.verticalSpacing;
  const int left = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.verticalSpacing;

  renderer.drawCenteredText(UI_12_FONT_ID, y, "PondeMate", true, EpdFontFamily::BOLD);
  y += rowHeight + metrics.verticalSpacing;

  const int hintsY = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight - metrics.verticalSpacing;
  if (confirmingReset) {
    y += rowHeight * 2;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "Reset progress of all sets?", true, EpdFontFamily::BOLD);
    y += rowHeight;
    renderer.drawCenteredText(UI_12_FONT_ID, y, "OK: reset   Back: cancel");
    drawHints("Cancel", "Reset", "", "", "", "", hintsY);
    return;
  }

  char line[kLineBytes + kSetNameBytes];
  if (setCount == 0) {
    renderer.drawText(UI_10_FONT_ID, left, y, "No puzzle sets in /pondemate/sets");
    y += rowHeight;
  }
  const int rows = setCount + 4;
  for (int i = 0; i < rows; i++) {
    if (i < setCount) {
      if (setNext[i] >= setTotals[i]) {
        snprintf(line, sizeof(line), "%s   done %d / %d", setNames[i], setTotals[i], setTotals[i]);
      } else {
        snprintf(line, sizeof(line), "%s   next %d / %d", setNames[i], setNext[i] + 1, setTotals[i]);
      }
    } else if (i == setCount) {
      snprintf(line, sizeof(line), "Buttons: %s", layout == Layout::Hjkl ? "hjkl" : "standard");
    } else if (i == setCount + 1) {
      snprintf(line, sizeof(line), "Debug: dither %d%%", darkDither50 ? 50 : 25);
    } else if (i == setCount + 2) {
      snprintf(line, sizeof(line), "Debug: reset progress");
    } else {
      snprintf(line, sizeof(line), "Debug: perft");
    }
    const bool selected = i == pickerIndex;
    if (selected) renderer.fillRect(0, y - 2, pageWidth, rowHeight, true);
    renderer.drawText(UI_12_FONT_ID, left, y, line, !selected, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += rowHeight;
  }

  const bool onSet = pickerIndex < setCount;
  const char* ok = onSet ? "Start" : (pickerIndex <= setCount + 1 ? "Toggle" : "Open");
  drawHints("Home", ok, "Up", "Down", "", "", hintsY);
}

void ChessPuzzleActivity::renderPuzzle() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int square = renderer.getScreenWidth() / 8;
  const bool whiteAtBottom = hasPuzzle ? session.playerIsWhite() : true;

  // The last move stays marked after Solved / Set complete; the cursor only shows while choosing.
  if (hasPuzzle) drawBoard(showingPlayerMove ? playerMoveBoard : session.position(), whiteAtBottom, true);

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = kBoardTop + square * 8 + metrics.verticalSpacing * 2;

  char line[kLineBytes + kSetNameBytes];
  if (hasPuzzle) {
    snprintf(line, sizeof(line), "%s  #%d  %s  (%d)", setNames[setIndex], puzzleIndex + 1, record.id, record.rating);
  } else {
    snprintf(line, sizeof(line), "%s", setNames[setIndex]);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, y, line, true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;

  if (menuOpen) {
    // The menu takes the place of the three text lines under the title.
    const int area = lineHeight * 3 + smallLineHeight / 2;
    const int rowHeight = area / kMenuItems;
    const int fontId = renderer.getLineHeight(UI_12_FONT_ID) <= rowHeight ? UI_12_FONT_ID : UI_10_FONT_ID;
    const int barWidth = renderer.getScreenWidth() / 2;
    const int barX = (renderer.getScreenWidth() - barWidth) / 2;
    const char* const items[kMenuItems] = {"Resume", "Hint", "Skip", "Sets"};
    for (int i = 0; i < kMenuItems; i++) {
      const bool selected = i == menuIndex;
      if (selected) renderer.fillRect(barX, y - 1, barWidth, rowHeight, true);
      renderer.drawCenteredText(fontId, y, items[i], !selected, selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      y += rowHeight;
    }
    y += area - rowHeight * kMenuItems;
    drawHints("Resume", "Select", "Up", "Down", "", "", y);
    return;
  }

  if (hasPuzzle) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, whiteAtBottom ? "White to move" : "Black to move");
  }
  y += lineHeight;
  if (cursorText[0] != '\0' && !showingPlayerMove) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, cursorText, true, EpdFontFamily::BOLD);
  }
  y += lineHeight;
  if (statusText[0] != '\0') renderer.drawCenteredText(UI_12_FONT_ID, y, statusText);
  y += lineHeight + smallLineHeight / 2;

  if (showingPlayerMove) {
    drawHints("", "", "", "", "", "", y);
    return;
  }
  switch (phase) {
    case Phase::SelectPiece:
      drawHints("Menu", "Grab", "Up", "Down", "Left", "Right", y);
      break;
    case Phase::SelectDest:
      drawHints("Pieces", "Move", "Up", "Down", "Left", "Right", y);
      break;
    case Phase::SelectPromo:
      drawHints("Pieces", "Promote", "Prev", "Next", "", "", y);
      break;
    case Phase::Solved:
      drawHints("Sets", "Next", "", "", "", "", y);
      break;
    default:
      drawHints("Sets", "Sets", "", "", "", "", y);
      break;
  }
}

void ChessPuzzleActivity::renderDebug() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int square = pageWidth / 8;
  drawBoard(position, true, false);

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = kBoardTop + square * 8 + metrics.verticalSpacing * 2;

  char line[96];
  snprintf(line, sizeof(line), "%s  -  %s to move", kPositions[positionIndex].name,
           position.whiteToMove ? "white" : "black");
  renderer.drawCenteredText(UI_12_FONT_ID, y, line, true, EpdFontFamily::BOLD);
  y += lineHeight + metrics.verticalSpacing;

  snprintf(line, sizeof(line), "legal moves: %d", legalMoves.count);
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += smallLineHeight;

  // As many SAN moves as fit on one line.
  const int maxLineWidth = pageWidth - 2 * metrics.contentSidePadding;
  int length = snprintf(line, sizeof(line), "%s", sanCount > 0 ? san[0] : "-");
  for (int i = 1; i < sanCount; i++) {
    const int previous = length;
    const int added = snprintf(line + length, sizeof(line) - length, " %s", san[i]);
    if (added < 0 || length + added >= static_cast<int>(sizeof(line))) {
      line[previous] = '\0';
      break;
    }
    length += added;
    if (renderer.getTextWidth(UI_10_FONT_ID, line) > maxLineWidth) {
      line[previous] = '\0';
      break;
    }
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += smallLineHeight;

  if (perftDone) {
    snprintf(line, sizeof(line), "perft: depth %d  nodes %llu  %lu ms", perftDepth,
             static_cast<unsigned long long>(perftNodes), static_cast<unsigned long>(perftMs));
  } else {
    snprintf(line, sizeof(line), "perft: -");
  }
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += smallLineHeight * 2;

  drawHints("Sets", "perft", "P1/P2", "P1/P2", "", "", y);
}

void ChessPuzzleActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (mode) {
    case Mode::Picker:
      renderPicker();
      break;
    case Mode::Puzzle:
      renderPuzzle();
      break;
    case Mode::Debug:
      renderDebug();
      break;
  }
  const bool half = halfRefreshPending;
  halfRefreshPending = false;
  const unsigned long refreshStart = millis();
  renderer.displayBuffer(half ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  refreshMs = static_cast<uint32_t>(millis() - refreshStart);
  refreshWasHalf = half;
  renderStackHW = static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  renderSeq = renderSeq + 1;
}
