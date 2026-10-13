#pragma once
#include <ChessCore.h>

#include <cstdint>

#include "activities/Activity.h"

// Chess puzzle screen for the X3 puzzle machine (PondeMate).
// Step 1: loads a FEN position with ChessCore, shows the pieces as letters,
// the legal move count and a few SAN moves, and runs perft on OK.
class ChessPuzzleActivity final : public Activity {
  static constexpr int kMaxSanMoves = 16;
  static constexpr int kSanBytes = 10;  // moveToSan() needs at least 10 bytes

  int positionIndex = 0;
  chess::Position position;
  // ~1KB: a member rather than a local because the loop task stack is small.
  chess::MoveList legalMoves;
  char san[kMaxSanMoves][kSanBytes] = {};
  int sanCount = 0;

  bool perftDone = false;
  int perftDepth = 0;
  uint64_t perftNodes = 0;
  uint32_t perftMs = 0;

  void loadPosition(int index);
  void runPerft();

 public:
  explicit ChessPuzzleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ChessPuzzle", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
