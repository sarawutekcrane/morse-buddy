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
// Natural-text word-gap helper (Hardware Fix #4 issue 3; revised in
// Hardware Fix #4.1 to a DEFERRED word-boundary model).
//
// Every natural-sentence Morse compose path (Text Message, Enigma, Morse
// Practice answer) already finalizes a letter itself once letterGapMs() (3
// dit) of silence has passed. This helper covers the separate step on top
// of that: once a letter has been finalized, the pause since the symbol
// that completed it MIGHT turn out to be a word gap (>= wordGapMs(), 7
// dit) -- but that can only be known for certain once the user actually
// starts a NEXT symbol, since the same pause also covers "done composing,
// about to press Send/Submit". If a space were inserted purely because
// time passed, an idle pause before Send/Submit would wrongly leave a
// trailing space in the stored/sent text (and, for Morse Practice, could
// turn a correct answer into a strcmp() mismatch).
//
// So nothing is ever mutated while idle. The pending state is only ever
// resolved -- to either exactly one space or nothing -- at the moment the
// NEXT symbol begins (DOT_PRESS_START), before that symbol is accepted.
// A small shared struct + three functions so this 3-caller timing rule
// isn't reimplemented three times with subtly different edge cases;
// callers still own their own millis() and letter-finalize logic (no
// Input/timing responsibility moves here).
// =============================================================================
struct WordGapState {
  bool pending = false;
  uint32_t lastSymbolReleaseMs = 0;
};

// Call right after finalizing a letter (pattern length back to 0), passing
// the millis() timestamp at which the symbol that completed it was
// released. Does not itself mutate compose text or commit to a space --
// see consumeWordBoundaryOnSymbolStart().
void armWordGap(WordGapState* state, uint32_t symbolReleaseMs);

// Discards a pending word boundary WITHOUT consuming it as a space. Call
// on any reset point that isn't "the user started composing the next
// letter" -- send, clear draft, delete/reset gesture, screen entry, new
// challenge/compose session, etc. -- so no stale pending state survives
// into an unrelated composition.
void cancelWordGap(WordGapState* state);

// Call exactly once, on every DOT_PRESS_START, BEFORE the new symbol is
// accepted into the pattern buffer. Returns true -- meaning "insert
// exactly one space now, then compose this symbol as the start of the
// next word" -- only if a word boundary was pending AND at least
// wordGapMs(wpm) had elapsed (at this moment) since the release of the
// letter-completing symbol. Either way, the pending state is consumed:
// a pending boundary that turns out to be too short (still the same
// word) is silently cancelled and this returns false. If nothing was
// pending, always returns false. A word space is therefore committed
// only immediately before a real next symbol -- never merely because
// time passed while idle (Hardware Fix #4.1).
bool consumeWordBoundaryOnSymbolStart(WordGapState* state, uint8_t wpm, uint32_t nowMs);

}  // namespace Morse
