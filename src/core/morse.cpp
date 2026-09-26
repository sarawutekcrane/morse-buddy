#include "core/morse.h"

#include <string.h>

namespace Morse {

uint16_t ditMs(uint8_t wpm) {
  if (wpm < kMinWpm) wpm = kMinWpm;
  if (wpm > kMaxWpm) wpm = kMaxWpm;
  return static_cast<uint16_t>(1200 / wpm);
}

uint16_t letterGapMs(uint8_t wpm) {
  return static_cast<uint16_t>(3 * ditMs(wpm));
}

uint16_t wordGapMs(uint8_t wpm) {
  return static_cast<uint16_t>(7 * ditMs(wpm));
}

SymbolClass classifyPress(uint32_t pressDurationMs, uint8_t wpm) {
  if (pressDurationMs >= kSpecialCommandMs) return SymbolClass::SPECIAL_COMMAND;

  // Hardware Fix #4.7d Part G: the DOT/DASH boundary is the midpoint
  // between the nominal 1-dit DOT and 3-dit DASH (2 dit), not 1 dit --
  // real-hardware testing at WPM 8 (dit = 150ms) found an ordinary human
  // short tap (~150-200ms) was already landing past a 1-dit boundary and
  // being misclassified as DASH. Bounded to at most 75% of
  // kSpecialCommandMs so this more forgiving DOT range can never eat into
  // (or eliminate) the DASH range that must still exist below the fixed
  // 2000ms special-command threshold, even at very low WPM where 2 dit
  // alone could otherwise approach or exceed it.
  uint32_t threshold = static_cast<uint32_t>(2 * ditMs(wpm));
  uint32_t maxThreshold = (static_cast<uint32_t>(kSpecialCommandMs) * 3u) / 4u;
  if (threshold > maxThreshold) threshold = maxThreshold;

  if (pressDurationMs < threshold) return SymbolClass::DOT;
  return SymbolClass::DASH;
}

namespace {
struct MorseEntry {
  const char* pattern;
  char ch;
};

// A-Z, 0-9, and the supported punctuation set (Addendum 9.2).
const MorseEntry kTable[] = {
    {".-", 'A'},    {"-...", 'B'},  {"-.-.", 'C'},  {"-..", 'D'},   {".", 'E'},
    {"..-.", 'F'},  {"--.", 'G'},   {"....", 'H'},  {"..", 'I'},    {".---", 'J'},
    {"-.-", 'K'},   {".-..", 'L'},  {"--", 'M'},    {"-.", 'N'},    {"---", 'O'},
    {".--.", 'P'},  {"--.-", 'Q'},  {".-.", 'R'},   {"...", 'S'},   {"-", 'T'},
    {"..-", 'U'},   {"...-", 'V'},  {".--", 'W'},   {"-..-", 'X'},  {"-.--", 'Y'},
    {"--..", 'Z'},
    {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'}, {"....-", '4'},
    {".....", '5'}, {"-....", '6'}, {"--...", '7'}, {"---..", '8'}, {"----.", '9'},
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-.-.--", '!'},
    {"-....-", '-'}, {".--.-.", '@'},
};
const size_t kTableSize = sizeof(kTable) / sizeof(kTable[0]);
}  // namespace

char decodePattern(const char* pattern) {
  for (size_t i = 0; i < kTableSize; i++) {
    if (strcmp(kTable[i].pattern, pattern) == 0) return kTable[i].ch;
  }
  return '?';
}

bool encodeChar(char c, char* outPattern, uint8_t outPatternSize) {
  if (outPattern == nullptr || outPatternSize == 0) return false;
  outPattern[0] = '\0';
  char upper = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  for (size_t i = 0; i < kTableSize; i++) {
    if (kTable[i].ch == upper) {
      strncpy(outPattern, kTable[i].pattern, outPatternSize - 1);
      outPattern[outPatternSize - 1] = '\0';
      return true;
    }
  }
  return false;
}

bool isDeletePattern(const char* pattern) {
  size_t len = strlen(pattern);
  if (len != 8) return false;
  for (size_t i = 0; i < len; i++) {
    if (pattern[i] != '.') return false;
  }
  return true;
}

void armWordGap(WordGapState* state, uint32_t symbolReleaseMs) {
  state->pending = true;
  state->lastSymbolReleaseMs = symbolReleaseMs;
}

void cancelWordGap(WordGapState* state) { state->pending = false; }

bool consumeWordBoundaryOnSymbolStart(WordGapState* state, uint8_t wpm, uint32_t nowMs) {
  if (!state->pending) return false;
  bool boundary = (nowMs - state->lastSymbolReleaseMs) >= wordGapMs(wpm);
  state->pending = false;
  return boundary;
}

}  // namespace Morse
