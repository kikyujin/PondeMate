#pragma once
#include "activities/Activity.h"

// Chess puzzle screen for the X3 puzzle machine.
// Step 0: "Hello world" -- draws an 8x8 board and echoes button presses,
// to confirm the screen size, square size and input mapping.
class ChessPuzzleActivity final : public Activity {
  int presses = 0;
  const char* lastButton = "-";

 public:
  explicit ChessPuzzleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ChessPuzzle", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
