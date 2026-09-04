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

}  // namespace Morse
