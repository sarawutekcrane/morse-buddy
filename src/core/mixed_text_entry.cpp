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

void render() {
  Display::setFont(Display::Font::PRIMARY);
  Display::clearContentArea();
  int16_t lh = Display::lineHeight();
  int16_t y = Display::kStatusBarHeight + 2;
  Display::printLine(2, y, g_config.title);
  y += lh;

  char line[96];
  switch (g_state) {
    case State::EMPTY:
      snprintf(line, sizeof(line), "%s_", g_buffer);
      break;
    case State::PREVIEW: {
      char c = charsetAt(g_config.charset, g_previewIndex);
      snprintf(line, sizeof(line), "%s[%c]", g_buffer, c);
      break;
    }
    case State::MORSE:
      snprintf(line, sizeof(line), "%s[%s]", g_buffer, g_morsePattern);
      break;
    case State::CONFIRM:
      // No cursor/preview suffix while confirming -- matches original
      // behavior of showing the plain buffer value being saved.
      snprintf(line, sizeof(line), "%s", g_buffer);
      break;
  }

  // Keep the actively-edited tail (cursor/preview) visible rather than
  // clipping it off-screen: a WiFi Password (up to 64 chars) or Group Code
  // (up to 32) can now exceed the PRIMARY font's visible width, where at
  // the old compact font they usually still fit (Hardware Fix #1). Drop
  // leading characters, not trailing ones, so the part the user is
  // actively typing (or about to save) stays on screen.
  const char* visible = line;
  int16_t avail = Display::kScreenWidth - 2;
  while (visible[0] != '\0' && Display::textWidth(visible) > avail) visible++;
  Display::printLine(2, y, visible);
  y += lh;

  if (g_state == State::EMPTY && g_errorMessage != nullptr) {
    Display::printLine(2, y + 8, g_errorMessage);
  } else if (g_state == State::CONFIRM) {
    Display::printLine(2, y + 8, g_confirmSaveSelected ? "> Save    Cancel" : "  Save  > Cancel");
  }
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
