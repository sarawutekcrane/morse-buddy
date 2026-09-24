#pragma once
// Number Guessing shared mechanics (Addendum section 12; Phase 3 sections
// 8-9) + Play Solo. Play with Friend lives in number_guessing_friend.*
// since it needs the network/storage/empty-compose-hook machinery Solo
// doesn't.

#include <stdint.h>

#include "core/input.h"

namespace NumberGuessing {

constexpr uint8_t kSecretDigits = 4;
constexpr uint16_t kMaxAttempts = 255;

struct GuessResult {
  uint8_t a;
  uint8_t b;
};
GuessResult evaluate(const uint8_t secret[kSecretDigits], const uint8_t guess[kSecretDigits]);

// Shared digit-entry component (Addendum 9.5: DOT <500ms confirms the
// rotated-to digit, DOT held >=500ms deletes the previous digit, firing
// once at 500ms while still held; release after a delete does not also
// confirm). Duplicate digits are skipped while rotating.
struct DigitEntryState {
  uint8_t digits[kSecretDigits];
  uint8_t count;
  int8_t previewDigit;
  bool dotHeld;
  uint32_t dotPressStartMs;
  bool deleteFiredThisPress;
};

void resetDigitEntry(DigitEntryState* state);
void handleDigitEntryEvent(DigitEntryState* state, const InputEvent& e);
void tickDigitEntry(DigitEntryState* state);  // call every frame to catch the 500ms hold-delete edge

// Hardware Fix #4 issue 7: shared partial-redraw renderer for a digit-entry
// row ("<label><4 fixed digit cells>", e.g. "Guess: 12_ _"), used by every
// caller with a DigitEntryState (Play Solo, Play with Friend's answer/
// manual-secret-entry, Race's guess screen). Earlier revisions accumulated
// each cell's X from the previous cells' actual rendered width, which is
// unsafe on PRIMARY's proportional font -- a digit whose glyph happens to
// be narrower/wider than another could visibly shift every cell after it.
// This renderer instead reserves kSecretDigits FIXED-X cells sized from the
// single widest glyph any cell can ever show (0-9 or the '_' placeholder)
// plus padding, so no cell's position ever depends on another cell's
// content. Each cell (confirmed digit / active preview digit / not-yet-
// reached placeholder) is diffed and redrawn completely independently.
struct DigitRowRenderState {
  char lastCell[kSecretDigits] = {0, 0, 0, 0};
  bool neverDrawn = true;
};

// Call once when the owning screen is (re)entered, before the first
// renderDigitRow() call, so the next call does a full initial draw.
void resetDigitRowRenderState(DigitRowRenderState* rs);

// Draws/updates the row at (x, y) using the widget's current font
// (caller must have already called Display::setFont()).
void renderDigitRow(DigitRowRenderState* rs, int16_t x, int16_t y, const char* labelPrefix,
                    const DigitEntryState& state);

}  // namespace NumberGuessing
