#include "ChessPuzzleActivity.h"

#include <GfxRenderer.h>

#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

void ChessPuzzleActivity::onEnter() {
  Activity::onEnter();
  requestUpdateAndWait();
}

void ChessPuzzleActivity::loop() {
  using Button = MappedInputManager::Button;
  if (mappedInput.wasPressed(Button::Back)) {
    onGoHome();
    return;
  }

  const char* pressed = nullptr;
  if (mappedInput.wasPressed(Button::Confirm)) pressed = "Confirm";
  if (mappedInput.wasPressed(Button::Up)) pressed = "Up";
  if (mappedInput.wasPressed(Button::Down)) pressed = "Down";
  if (mappedInput.wasPressed(Button::Left)) pressed = "Left";
  if (mappedInput.wasPressed(Button::Right)) pressed = "Right";
  if (mappedInput.wasPressed(Button::PageBack)) pressed = "PageBack";
  if (mappedInput.wasPressed(Button::PageForward)) pressed = "PageForward";

  if (pressed != nullptr) {
    lastButton = pressed;
    presses++;
    requestUpdate();
  }
}

void ChessPuzzleActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Board: as large as the width allows, squares of equal integer size.
  const int square = pageWidth / 8;
  const int boardSize = square * 8;
  const int boardX = (pageWidth - boardSize) / 2;
  const int boardY = metrics.topPadding;

  for (int rank = 0; rank < 8; rank++) {
    for (int file = 0; file < 8; file++) {
      const int x = boardX + file * square;
      const int y = boardY + rank * square;
      const bool dark = ((rank + file) & 1) != 0;
      if (dark) {
        renderer.fillRectDither(x, y, square, square, Color::LightGray);
      }
    }
  }
  renderer.drawRect(boardX, boardY, boardSize, boardSize, 2, true);

  // Text area below the board.
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  int y = boardY + boardSize + metrics.verticalSpacing * 2;
  renderer.drawCenteredText(UI_12_FONT_ID, y, "Hello, X3 chess puzzle!");
  y += lineHeight + metrics.verticalSpacing;

  char line[64];
  snprintf(line, sizeof(line), "screen %dx%d  square %dpx", pageWidth, pageHeight, square);
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);
  y += lineHeight;

  snprintf(line, sizeof(line), "last: %s  (%d presses)", lastButton, presses);
  renderer.drawCenteredText(UI_10_FONT_ID, y, line);

  const auto labels = mappedInput.mapLabels("Back", "OK", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
