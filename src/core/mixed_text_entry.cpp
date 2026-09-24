#include "core/mixed_text_entry.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/input.h"
#include "core/morse.h"
#include "core/settings.h"

namespace {

enum class State : uint8_t { EMPTY, PREVIEW, MORSE, CONFIRM };

const char kGeneralNameChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 .,-_!?@#";
const char kGroupCodeChars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

uint16_t charsetLength(FieldCharset cs) {
  switch (cs) {
    case FieldCharset::GENERAL_NAME:
      return static_cast<uint16_t>(strlen(kGeneralNameChars));
    case FieldCharset::GROUP_CODE:
      return static_cast<uint16_t>(strlen(kGroupCodeChars));
    case FieldCharset::WIFI_PASSWORD:
      return 95;  // printable ASCII 32..126
  }
  return 0;
}

char charsetAt(FieldCharset cs, int16_t index) {
  switch (cs) {
    case FieldCharset::GENERAL_NAME:
      return kGeneralNameChars[index];
    case FieldCharset::GROUP_CODE:
      return kGroupCodeChars[index];
    case FieldCharset::WIFI_PASSWORD:
      return static_cast<char>(32 + index);
  }
  return '?';
}

MixedTextEntryConfig g_config;
char g_buffer[65];
uint8_t g_length = 0;

State g_state = State::EMPTY;
int16_t g_previewIndex = -1;

char g_morsePattern[Morse::kMaxPatternLength + 1];
uint8_t g_morsePatternLen = 0;
uint32_t g_lastMorseReleaseMs = 0;

bool g_confirmSaveSelected = true;
const char* g_errorMessage = nullptr;

bool g_finished = false;
MixedTextEntryResult g_result = MixedTextEntryResult::NONE;

bool g_dirty = true;

// Hardware Fix #3A/#3: render() no longer clears/redraws the whole content
// area on every dirty tick, and (Fix #3) the input row itself is split
// into a stable confirmed PREFIX (g_buffer) and a dynamic SUFFIX
// (cursor/preview/morse marker) so a plain preview-character change
// (e.g. ABCDEF[G] -> ABCDEF[H]) redraws only the suffix cell -- the
// confirmed "ABCDEF" is never touched. g_needsFullRedraw gates the
// one-time clear+title+input+control draw (start()). If the row's
// leading-clip window is active (long strings), a full input-row redraw
// is used instead, because the visible coordinate mapping can shift
// (Global Invariant 12) -- prefix/suffix X positions are only stable
// when nothing is clipped.
bool g_needsFullRedraw = true;
char g_lastPrefix[65] = {0};
char g_lastSuffix[24] = {0};
bool g_lastNeededClip = false;
char g_lastClippedLine[96] = {0};
char g_lastControlLine[40] = {0};

void appendConfirmedChar(char c) {
  if (g_length < g_config.maxLength) {
    g_buffer[g_length++] = c;
    g_buffer[g_length] = '\0';
  }
}

void deletePreviousConfirmedChar() {
  if (g_length > 0) {
    g_length--;
    g_buffer[g_length] = '\0';
  }
}

void resetMorsePattern() {
  g_morsePatternLen = 0;
  g_morsePattern[0] = '\0';
}

void finalizeMorseChar() {
  char c = Morse::decodePattern(g_morsePattern);
  appendConfirmedChar(c);
  resetMorsePattern();
  g_state = State::EMPTY;
}

void cancelWholeEdit() {
  g_finished = true;
  g_result = MixedTextEntryResult::CANCELLED;
}

void handleEmpty(const InputEvent& e) {
  g_errorMessage = nullptr;
  if (e.type == InputEventType::ENCODER_ROTATE) {
    g_state = State::PREVIEW;
    g_previewIndex = 0;
  } else if (e.type == InputEventType::DOT_RELEASE) {
    if (e.durationMs >= Morse::kSpecialCommandMs) {
      deletePreviousConfirmedChar();
    } else {
      Morse::SymbolClass sc = Morse::classifyPress(e.durationMs, Settings::getWpm());
      resetMorsePattern();
      g_morsePattern[0] = (sc == Morse::SymbolClass::DOT) ? '.' : '-';
      g_morsePattern[1] = '\0';
      g_morsePatternLen = 1;
      g_lastMorseReleaseMs = millis();
      g_state = State::MORSE;
    }
  } else if (e.type == InputEventType::ENCODER_SHORT) {
    if (g_length >= g_config.minLength) {
      g_state = State::CONFIRM;
      g_confirmSaveSelected = true;
    }
  } else if (e.type == InputEventType::ENCODER_LONG) {
    cancelWholeEdit();
  }
}

void handlePreview(const InputEvent& e) {
  if (e.type == InputEventType::ENCODER_ROTATE) {
    int16_t newIndex = static_cast<int16_t>(g_previewIndex + e.value);
    uint16_t len = charsetLength(g_config.charset);
    if (newIndex < 0) {
      g_state = State::EMPTY;
      g_previewIndex = -1;
    } else if (newIndex >= static_cast<int16_t>(len)) {
      g_previewIndex = 0;
    } else {
      g_previewIndex = newIndex;
    }
  } else if (e.type == InputEventType::DOT_RELEASE) {
    appendConfirmedChar(charsetAt(g_config.charset, g_previewIndex));
    g_state = State::EMPTY;
    g_previewIndex = -1;
  } else if (e.type == InputEventType::ENCODER_LONG) {
    cancelWholeEdit();
  }
}

void handleMorse(const InputEvent& e) {
  if (e.type == InputEventType::DOT_RELEASE) {
    Morse::SymbolClass sc = Morse::classifyPress(e.durationMs, Settings::getWpm());
    if (sc == Morse::SymbolClass::SPECIAL_COMMAND) {
      // No defined action mid-accumulation; ignored (delete is Empty-state only).
      return;
    }
    if (g_morsePatternLen < Morse::kMaxPatternLength) {
      g_morsePattern[g_morsePatternLen++] = (sc == Morse::SymbolClass::DOT) ? '.' : '-';
      g_morsePattern[g_morsePatternLen] = '\0';
    }
    g_lastMorseReleaseMs = millis();
    if (Morse::isDeletePattern(g_morsePattern)) {
      deletePreviousConfirmedChar();
      resetMorsePattern();
      g_state = State::EMPTY;
    }
  } else if (e.type == InputEventType::ENCODER_LONG) {
    cancelWholeEdit();
  }
}

void handleConfirm(const InputEvent& e) {
  if (e.type == InputEventType::ENCODER_ROTATE) {
    g_confirmSaveSelected = !g_confirmSaveSelected;
  } else if (e.type == InputEventType::DOT_RELEASE && Input::isMenuConfirm(e)) {
    if (g_confirmSaveSelected) {
      if (g_config.validator != nullptr && !g_config.validator(g_buffer)) {
        g_errorMessage = "Not allowed";
        g_state = State::EMPTY;
      } else {
        g_finished = true;
        g_result = MixedTextEntryResult::SAVED;
      }
    } else {
      g_finished = true;
      g_result = MixedTextEntryResult::CANCELLED;
    }
  } else if (e.type == InputEventType::ENCODER_LONG) {
    g_state = State::EMPTY;  // returns to editing
  }
}

// Splits the input row into its stable confirmed prefix (the buffer, as
// typed so far) and its dynamic suffix (cursor/preview/morse marker).
// CONFIRM state shows the plain buffer with no suffix at all -- matches
// original behavior of showing the plain value being saved.
void computeParts(char* prefixOut, size_t prefixCap, char* suffixOut, size_t suffixCap) {
  strncpy(prefixOut, g_buffer, prefixCap - 1);
  prefixOut[prefixCap - 1] = '\0';

  switch (g_state) {
    case State::EMPTY:
      snprintf(suffixOut, suffixCap, "_");
      break;
    case State::PREVIEW: {
      char c = charsetAt(g_config.charset, g_previewIndex);
      snprintf(suffixOut, suffixCap, "[%c]", c);
      break;
    }
    case State::MORSE:
      snprintf(suffixOut, suffixCap, "[%s]", g_morsePattern);
      break;
    case State::CONFIRM:
      suffixOut[0] = '\0';
      break;
  }
}

// Builds the control row's text: the validation error (EMPTY state only)
// or the Save/Cancel toggle line (CONFIRM state only), empty otherwise.
void computeControlLine(char* out, size_t outCap) {
  if (g_state == State::EMPTY && g_errorMessage != nullptr) {
    strncpy(out, g_errorMessage, outCap - 1);
    out[outCap - 1] = '\0';
  } else if (g_state == State::CONFIRM) {
    snprintf(out, outCap, "%s", g_confirmSaveSelected ? "> Save    Cancel" : "  Save  > Cancel");
  } else {
    out[0] = '\0';
  }
}

void render() {
  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t titleY = Display::kStatusBarHeight + 2;
  int16_t inputY = static_cast<int16_t>(titleY + lh);
  int16_t controlY = static_cast<int16_t>(inputY + lh + 8);

  char prefix[65];
  char suffix[24];
  computeParts(prefix, sizeof(prefix), suffix, sizeof(suffix));

  char combined[96];
  snprintf(combined, sizeof(combined), "%s%s", prefix, suffix);
  int16_t avail = static_cast<int16_t>(Display::kScreenWidth - 2);
  bool needsClip = Display::textWidth(combined) > avail;

  // Keep the actively-edited tail visible rather than clipping it
  // off-screen: a WiFi Password (up to 64 chars) or Group Code (up to 32)
  // can exceed the PRIMARY font's visible width. Drop leading characters,
  // not trailing ones, so the part the user is actively typing (or about
  // to save) stays on screen (Hardware Fix #1). When clipping isn't
  // needed, clippedLine is just the unclipped combined string.
  char clippedLine[96];
  if (needsClip) {
    const char* visible = combined;
    while (visible[0] != '\0' && Display::textWidth(visible) > avail) visible++;
    strncpy(clippedLine, visible, sizeof(clippedLine) - 1);
  } else {
    strncpy(clippedLine, combined, sizeof(clippedLine) - 1);
  }
  clippedLine[sizeof(clippedLine) - 1] = '\0';

  char controlLine[40];
  computeControlLine(controlLine, sizeof(controlLine));

  int16_t prefixW = Display::textWidth(prefix);
  int16_t suffixX = static_cast<int16_t>(2 + prefixW);

  if (g_needsFullRedraw) {
    // First render after start(): everything is new -- title included.
    Display::clearContentArea();
    Display::printLine(2, titleY, g_config.title);
    if (needsClip) {
      Display::printLine(2, inputY, clippedLine);
    } else {
      Display::printLine(2, inputY, prefix);
      if (suffix[0] != '\0') Display::printLine(suffixX, inputY, suffix);
    }
    // Control row is handled uniformly by the unconditional diff below
    // (g_lastControlLine starts empty, so a non-empty controlLine here
    // will correctly be drawn there without a redundant double-draw).
    g_needsFullRedraw = false;
  } else if (needsClip || g_lastNeededClip) {
    // The leading-clip window is active now, or was active last render:
    // the visible coordinate mapping may have shifted (Global Invariant
    // 12), so fall back to a full input-row redraw rather than risk a
    // partial update landing at the wrong X. Still only touches the input
    // row, never the title.
    if (strcmp(clippedLine, g_lastClippedLine) != 0) {
      Display::tft().fillRect(0, inputY, Display::kScreenWidth, lh, ST77XX_BLACK);
      Display::printLine(2, inputY, clippedLine);
    }
  } else if (strcmp(prefix, g_lastPrefix) != 0) {
    // The confirmed prefix itself changed (append/delete/Morse-finalize):
    // not the common "same layout" case, so redraw the whole row -- still
    // far lighter than a full-screen or even full-content-area clear.
    Display::tft().fillRect(0, inputY, Display::kScreenWidth, lh, ST77XX_BLACK);
    Display::printLine(2, inputY, prefix);
    if (suffix[0] != '\0') Display::printLine(suffixX, inputY, suffix);
  } else if (strcmp(suffix, g_lastSuffix) != 0) {
    // The hot path this fix targets: only the dynamic suffix
    // (cursor/preview/morse marker) changed, e.g. ABCDEF[G] -> ABCDEF[H].
    // The confirmed prefix is provably unchanged and at the same X, so it
    // is never touched -- only the suffix's own cell (sized to cover both
    // its old and new glyph extents) is erased and redrawn.
    int16_t oldSuffixW = Display::textWidth(g_lastSuffix);
    int16_t newSuffixW = Display::textWidth(suffix);
    int16_t eraseW = static_cast<int16_t>((oldSuffixW > newSuffixW ? oldSuffixW : newSuffixW) + 4);
    int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - suffixX);
    if (eraseW > maxW) eraseW = maxW;
    if (eraseW < 0) eraseW = 0;
    Display::tft().fillRect(suffixX, inputY, eraseW, lh, ST77XX_BLACK);
    if (suffix[0] != '\0') Display::printLine(suffixX, inputY, suffix);
  }

  if (strcmp(controlLine, g_lastControlLine) != 0) {
    // PRIMARY is a GFX custom font and never draws with an opaque
    // background, so the row is explicitly erased before its replacement
    // is drawn.
    Display::tft().fillRect(0, controlY, Display::kScreenWidth, lh, ST77XX_BLACK);
    if (controlLine[0] != '\0') Display::printLine(2, controlY, controlLine);
  }

  g_lastNeededClip = needsClip;
  strncpy(g_lastClippedLine, clippedLine, sizeof(g_lastClippedLine) - 1);
  g_lastClippedLine[sizeof(g_lastClippedLine) - 1] = '\0';
  strncpy(g_lastPrefix, prefix, sizeof(g_lastPrefix) - 1);
  g_lastPrefix[sizeof(g_lastPrefix) - 1] = '\0';
  strncpy(g_lastSuffix, suffix, sizeof(g_lastSuffix) - 1);
  g_lastSuffix[sizeof(g_lastSuffix) - 1] = '\0';
  strncpy(g_lastControlLine, controlLine, sizeof(g_lastControlLine) - 1);
  g_lastControlLine[sizeof(g_lastControlLine) - 1] = '\0';
}

}  // namespace

namespace MixedTextEntry {

void start(const MixedTextEntryConfig& config, const char* initialValue) {
  g_config = config;
  strncpy(g_buffer, initialValue != nullptr ? initialValue : "", sizeof(g_buffer) - 1);
  g_buffer[sizeof(g_buffer) - 1] = '\0';
  g_length = static_cast<uint8_t>(strlen(g_buffer));

  g_state = State::EMPTY;
  g_previewIndex = -1;
  resetMorsePattern();
  g_confirmSaveSelected = true;
  g_errorMessage = nullptr;
  g_finished = false;
  g_result = MixedTextEntryResult::NONE;
  g_dirty = true;
  g_needsFullRedraw = true;
  g_lastPrefix[0] = '\0';
  g_lastSuffix[0] = '\0';
  g_lastNeededClip = false;
  g_lastClippedLine[0] = '\0';
  g_lastControlLine[0] = '\0';
}

void tick() {
  Input::update();
  InputEvent e;
  bool hadEvent = false;
  while (Input::popEvent(e)) {
    hadEvent = true;
    switch (g_state) {
      case State::EMPTY:
        handleEmpty(e);
        break;
      case State::PREVIEW:
        handlePreview(e);
        break;
      case State::MORSE:
        handleMorse(e);
        break;
      case State::CONFIRM:
        handleConfirm(e);
        break;
    }
  }
  // Every popped event here changes visible state (preview index, pattern,
  // buffer, or the Save/Cancel toggle) or is about to leave this screen
  // entirely, so a coarse "any event -> dirty" is correct, not just cheap.
  if (hadEvent) g_dirty = true;

  if (g_state == State::MORSE && g_morsePatternLen > 0) {
    uint32_t now = millis();
    if (now - g_lastMorseReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeMorseChar();
      g_dirty = true;
    }
  }

  // Every screen that delegates to this shared widget (My Name, WiFi
  // Password, Group Name, Group Code, ...) still shows the standard status
  // bar, but none of them call Display::drawStatusBar() themselves -- this
  // is the single choke point they all go through, so it belongs here
  // (Hardware Fix #1 correction). drawStatusBar() is internally dirty-gated,
  // so calling it unconditionally every tick is safe/cheap.
  Display::drawStatusBar();

  if (!g_dirty) return;
  g_dirty = false;
  render();
}

bool isFinished() { return g_finished; }
MixedTextEntryResult result() { return g_result; }
const char* getValue() { return g_buffer; }

}  // namespace MixedTextEntry
