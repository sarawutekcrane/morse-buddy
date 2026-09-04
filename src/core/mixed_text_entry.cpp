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
  Adafruit_ST7789& tft = Display::tft();
  Display::clearContentArea();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print(g_config.title);

  tft.setCursor(2, Display::kStatusBarHeight + 16);
  tft.print(g_buffer);

  switch (g_state) {
    case State::EMPTY:
      tft.print('_');
      if (g_errorMessage != nullptr) {
        tft.setCursor(2, Display::kStatusBarHeight + 40);
        tft.print(g_errorMessage);
      }
      break;
    case State::PREVIEW: {
      char c = charsetAt(g_config.charset, g_previewIndex);
      tft.print('[');
      tft.print(c);
      tft.print(']');
      break;
    }
    case State::MORSE:
      tft.print('[');
      tft.print(g_morsePattern);
      tft.print(']');
      break;
    case State::CONFIRM:
      tft.setCursor(2, Display::kStatusBarHeight + 40);
      tft.print(g_confirmSaveSelected ? "> Save    Cancel" : "  Save  > Cancel");
      break;
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
}

void tick() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
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

  if (g_state == State::MORSE && g_morsePatternLen > 0) {
    uint32_t now = millis();
    if (now - g_lastMorseReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeMorseChar();
    }
  }

  render();
}

bool isFinished() { return g_finished; }
MixedTextEntryResult result() { return g_result; }
const char* getValue() { return g_buffer; }

}  // namespace MixedTextEntry
