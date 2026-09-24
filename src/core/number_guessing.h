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

// Hardware Fix #3: shared partial-redraw renderer for a digit-entry row
// ("<label><confirmed digits><active preview digit><underscores>", e.g.
// "Guess: 12_ _"). Every caller (Play Solo, Play with Friend's answer/
// manual-secret-entry, Race's guess screen) previously rebuilt this whole
// 4-digit string into one snprintf and redrew it as a single line on every
// encoder rotate, which visibly re-flickers the confirmed digits even
// though only the active (not-yet-confirmed) digit actually changes. This
// renderer instead diffs the confirmed-digit prefix, the single active
// digit, and the trailing underscore tail independently, so a plain
// preview-digit rotation (12_ _ -> 123_'s middle step, e.g. previewing 2
// then 3 before confirming) only erases/redraws that one glyph cell.
struct DigitRowRenderState {
  char lastPrefix[16] = {0};
  char lastActive[2] = {0};
  char lastTail[8] = {0};
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
