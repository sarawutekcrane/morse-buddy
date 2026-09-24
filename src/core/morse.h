#pragma once
// Morse timing, encode/decode tables (Addendum section 9.1/9.2, Phase 0).

#include <stdint.h>

namespace Morse {

constexpr uint8_t kMinWpm = 1;
constexpr uint8_t kMaxWpm = 50;
constexpr uint8_t kWpmWarnThreshold = 35;  // warn at >=35, still allow

constexpr uint16_t kSpecialCommandMs = 2000;  // >=2000ms DOT/DASH hold = special command

// ditMs = 1200 / WPM ; letterGapMs = 3 * ditMs ; wordGapMs = 7 * ditMs
uint16_t ditMs(uint8_t wpm);
uint16_t letterGapMs(uint8_t wpm);
uint16_t wordGapMs(uint8_t wpm);

enum class SymbolClass : uint8_t { DOT, DASH, SPECIAL_COMMAND };

// Classifies a DOT/DASH press duration in a Morse-entry context:
//   < ditMs           -> DOT
//   ditMs .. <2000ms  -> DASH   (no fixed 600ms ceiling)
//   >= 2000ms         -> SPECIAL_COMMAND
SymbolClass classifyPress(uint32_t pressDurationMs, uint8_t wpm);

// Longest supported pattern (a-z/0-9/punctuation) is 6 symbols; the delete
// prosign is 8 dots, so buffers must accommodate at least 8 + NUL.
constexpr uint8_t kMaxPatternLength = 8;

// Decodes a completed '.'/'-' pattern to its uppercase character.
// Returns '?' for any completed pattern outside the supported table
// (A-Z, 0-9, . , ? ! - @).
char decodePattern(const char* pattern);

// Encodes a supported character (case-insensitive letters, digits, and the
// supported punctuation) into its dot/dash pattern. outPattern must be at
// least kMaxPatternLength+1 bytes. Returns false (and sets outPattern to an
// empty string) if the character is not supported.
bool encodeChar(char c, char* outPattern, uint8_t outPatternSize);

// True if `pattern` is exactly eight dots: the delete-previous-character
// prosign, intercepted before normal decoding (Addendum section 9.2).
bool isDeletePattern(const char* pattern);

// =============================================================================
// Natural-text word-gap helper (Hardware Fix #4 issue 3).
//
// Every natural-sentence Morse compose path (Text Message, Enigma, Morse
// Practice answer) already finalizes a letter itself once letterGapMs() (3
// dit) of silence has passed. This helper covers the separate step on top
// of that: once a letter has been finalized, a word space is due after a
// further wordGapMs() (7 dit) of silence measured from the release of the
// symbol that completed that letter -- NOT from when finalize happened --
// unless a new symbol starts first. A small shared struct + three
// functions so this 3-caller timing rule isn't reimplemented three times
// with subtly different edge cases; callers still own their own millis()
// and letter-finalize logic (no Input/timing responsibility moves here).
// =============================================================================
struct WordGapState {
  bool pending = false;
  uint32_t lastSymbolReleaseMs = 0;
};

// Call right after finalizing a letter (pattern length back to 0), passing
// the millis() timestamp at which the symbol that completed it was
// released.
void armWordGap(WordGapState* state, uint32_t symbolReleaseMs);

// Cancels a pending word gap immediately. Call this on every
// DOT_PRESS_START (not DOT_RELEASE) so a user starting the next letter's
// first symbol can never have their hold time itself cross the 7-dit
// threshold and wrongly insert a space mid-press. Also call on any reset
// point (send, clear draft, screen entry, challenge reset, etc.) so no
// stale pending state survives into an unrelated composition.
void cancelWordGap(WordGapState* state);

// Call once per tick while composing. Returns true exactly once when
// wordGapMs(wpm) has elapsed since lastSymbolReleaseMs, and clears
// `pending` itself -- the caller does not need to call cancelWordGap()
// after acting on a true result.
bool wordGapDue(WordGapState* state, uint8_t wpm, uint32_t nowMs);

}  // namespace Morse
