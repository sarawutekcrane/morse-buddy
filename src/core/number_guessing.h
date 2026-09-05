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

}  // namespace NumberGuessing
