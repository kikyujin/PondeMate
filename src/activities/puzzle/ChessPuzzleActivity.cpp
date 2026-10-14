#include "ChessPuzzleActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstdio>

#include "PieceBitmaps.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

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

}  // namespace

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
}

void ChessPuzzleActivity::onEnter() {
  Activity::onEnter();
  loadPosition(0);
  requestUpdateAndWait();
}

void ChessPuzzleActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasPressed(Button::Back)) {
    onGoHome();
    return;
  }

  int step = 0;
  if (mappedInput.wasPressed(Button::Up)) step = -1;
  if (mappedInput.wasPressed(Button::Down)) step = 1;
  if (step != 0) {
    {
      RenderLock lock(*this);
      loadPosition((positionIndex + step + kPositionCount) % kPositionCount);
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasPressed(Button::Confirm)) {
    // Holding the render lock keeps the screen untouched until perft finishes.
    {
      RenderLock lock(*this);
      runPerft();
    }
    requestUpdate();
  }
}

void ChessPuzzleActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Board: as large as the width allows, squares of equal integer size.
  const int square = pageWidth / 8;
  const int boardSize = square * 8;
  const int boardX = (pageWidth - boardSize) / 2;
  const int boardY = metrics.topPadding;

  const int pieceInset = (square - PIECE_SIZE) / 2;
  const unsigned long boardStart = millis();
  int blitted = 0;
  int fallbacks = 0;

  // Row 0 on screen is rank 8, so a8 is the top-left (light) square.
  for (int row = 0; row < 8; row++) {
    for (int file = 0; file < 8; file++) {
      const int x = boardX + file * square;
      const int y = boardY + row * square;
      const bool dark = ((row + file) & 1) != 0;
      if (dark) {
        renderer.fillRectDither(x, y, square, square, Color::LightGray);
      }

      const uint8_t piece = position.squares[(7 - row) * 8 + file];
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
  renderer.drawRect(boardX, boardY, boardSize, boardSize, 2, true);
  if (fallbacks > 0) {
    LOG_INF("CHESS", "fallback per-pixel: %d pieces", fallbacks);
  }
  LOG_INF("CHESS", "board render %lu ms (blit %d, per-pixel %d)", static_cast<unsigned long>(millis() - boardStart),
          blitted, fallbacks);

  // Text area below the board.
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int smallLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  int y = boardY + boardSize + metrics.verticalSpacing * 2;

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

  const auto labels = mappedInput.mapLabels("Back", "perft", "P1/P2", "P1/P2");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
