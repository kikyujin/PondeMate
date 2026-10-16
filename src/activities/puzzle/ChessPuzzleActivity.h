#pragma once
#include <ChessCore.h>

#include <cstdint>

#include "PuzzleCore.h"
#include "activities/Activity.h"

// PondeMate: chess puzzles for the X3.
// A set picker lists /pondemate/sets/*.txt; a puzzle screen lets the player
// grab a piece and pick its destination with a cursor that snaps to legal
// choices; a debug screen keeps the Phase 1 perft check.
class ChessPuzzleActivity final : public Activity {
 public:
  enum class Layout : uint8_t { Standard, Hjkl };
  enum class Action : uint8_t { None, Up, Down, Left, Right, Ok, Back };

 private:
  enum class Mode : uint8_t { Picker, Puzzle, Debug };
  enum class Phase : uint8_t { SelectPiece, SelectDest, SelectPromo, Solved, Complete, Error };

  static constexpr int kMaxSets = 12;
  static constexpr int kSetNameBytes = 40;
  static constexpr int kLineBytes = 72;

  Mode mode = Mode::Picker;
  Layout layout = Layout::Standard;
  bool darkDither50 = false;  // dark squares at 50% instead of 25%

  // Set picker. Rows: the sets, then "Buttons", "Debug: dither", "Debug: perft".
  char setNames[kMaxSets][kSetNameBytes] = {};
  int setTotals[kMaxSets] = {};
  int setNext[kMaxSets] = {};
  int setCount = 0;
  int pickerIndex = 0;

  // Puzzle screen. ~1.9KB with the session; members because the loop task
  // stack is small and ChessCore already uses ~2KB of it.
  int setIndex = 0;
  int puzzleIndex = 0;
  char recordBuf[pondemate::kRecordBytes] = {};
  pondemate::PuzzleRecord record{};
  pondemate::PuzzleSession session;
  bool hasPuzzle = false;
  Phase phase = Phase::SelectPiece;
  uint8_t cursor = 0;
  uint8_t grabbed = 0;
  uint8_t targets[64] = {};
  int targetCount = 0;
  chess::Move promoChoices[4] = {};
  int promoCount = 0;
  int promoIndex = 0;
  // Text prepared in loop() so render() only draws.
  char cursorText[kLineBytes] = {};
  char statusText[kLineBytes] = {};

  // Debug (Phase 1 perft check).
  static constexpr int kMaxSanMoves = 16;
  static constexpr int kSanBytes = 10;  // moveToSan() needs at least 10 bytes
  int positionIndex = 0;
  chess::Position position;
  chess::MoveList legalMoves;  // ~1KB
  char san[kMaxSanMoves][kSanBytes] = {};
  int sanCount = 0;
  bool perftDone = false;
  int perftDepth = 0;
  uint64_t perftNodes = 0;
  uint32_t perftMs = 0;

  // The next render uses HALF_REFRESH so the previous screen does not linger; set when the
  // screen kind changes (and on entry, and on moving to the next puzzle), cleared by render().
  bool halfRefreshPending = false;

  // Diagnostics. render() fills these; loop() logs them.
  uint32_t renderStackHW = 0;  // bytes (StackType_t is uint8_t on ESP32)
  uint32_t refreshMs = 0;
  bool refreshWasHalf = false;
  volatile uint32_t renderSeq = 0;
  uint32_t loggedRenderSeq = 0;

  Action readAction() const;
  void loadSettings();
  void saveSettings() const;
  void scanSets();
  bool openPuzzle(int set, int index);
  void startTurn();
  void updateCursorText();
  void handlePicker(Action action);
  void handlePuzzle(Action action);
  void handleDebug(Action action);
  void recordSolved();
  void logDiag(const char* scene) const;
  void logRefresh();

  void loadPosition(int index);
  void runPerft();

  void drawBoard(const chess::Position& board, bool whiteAtBottom, bool marks) const;
  void drawHints(const char* back, const char* ok, const char* up, const char* down, const char* left,
                 const char* right, int y) const;
  void renderPicker() const;
  void renderPuzzle() const;
  void renderDebug() const;

 public:
  explicit ChessPuzzleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ChessPuzzle", renderer, mappedInput) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
