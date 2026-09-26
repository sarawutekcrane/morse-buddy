#include "core/mixed_text_entry.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/input.h"
#include "core/morse.h"
#include "core/settings.h"

namespace {

enum class State : uint8_t { EMPTY, PREVIEW, MORSE, CONFIRM };

// The control row shows one of four things: nothing, a validation error
// (EMPTY state), the "reached max length" info line (EMPTY state, Hardware
// Fix #4.7b -- deliberately its own kind, not ERROR: reaching the bound is
// informational, never an error tone/state), or the Save/Cancel selector
// (CONFIRM state). Save/Cancel is tracked as its own kind (rather than
// folded into a single string) because its two labels are fixed text --
// only the "> " marker between them ever moves -- so a plain toggle never
// needs to touch label pixels (Hardware Fix #3 corrective).
enum class ControlKind : uint8_t { NONE, ERROR, MAX_LENGTH, CONFIRM };

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
ControlKind g_lastControlKind = ControlKind::NONE;
char g_lastErrorText[40] = {0};
bool g_lastConfirmSaveSelected = true;

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
  // Hardware Fix #4.7b: once the buffer has reached g_config.maxLength, no
  // further character can actually be appended (appendConfirmedChar()
  // already refuses), so entering PREVIEW/MORSE to compose one anyway is
  // pure UI noise -- a fifth preview character that can never be saved.
  // The one exception is the long-DOT/DASH delete command below, which
  // must keep working unconditionally so the user can shorten the name and
  // resume editing.
  bool atMax = (g_length >= g_config.maxLength);
  if (e.type == InputEventType::ENCODER_ROTATE) {
    if (atMax) return;  // no fifth-character preview once max length is reached
    g_state = State::PREVIEW;
    g_previewIndex = 0;
  } else if (e.type == InputEventType::DOT_RELEASE) {
    if (e.durationMs >= Morse::kSpecialCommandMs) {
      deletePreviousConfirmedChar();  // delete must still work at max length
    } else if (!atMax) {
      Morse::SymbolClass sc = Morse::classifyPress(e.durationMs, Settings::getWpm());
      resetMorsePattern();
      g_morsePattern[0] = (sc == Morse::SymbolClass::DOT) ? '.' : '-';
      g_morsePattern[1] = '\0';
      g_morsePatternLen = 1;
      // Hardware Fix #4.7d: store the physical accepted release time, not
      // processing-time millis() -- this seeds the same g_lastMorseReleaseMs
      // handleMorse()'s catch-up check and tick()'s idle finalizer both
      // measure against.
      g_lastMorseReleaseMs = e.eventMs != 0 ? e.eventMs : millis();
      g_state = State::MORSE;
    }
    // else: at max length and this is a normal (non-delete) press -- no
    // new Morse character can be appended, so it is simply ignored.
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
  if (e.type == InputEventType::DOT_PRESS_START) {
    // Hardware Fix #4.7d Part F: catch-up finalization using the physical
    // accepted press time. Fix #4.7b's reliable edge capture means a busy
    // main loop can now drain "release / >=3dit physical gap / press" all
    // in one tick; without this check the new press would just keep
    // accumulating symbols onto the character the user considered already
    // finished. The later DOT_RELEASE in the same drained batch is then
    // processed under the NEW current state (g_state == EMPTY once
    // finalizeMorseChar() runs) and begins the next character normally --
    // MixedTextEntry has no natural-text word-gap insertion, so nothing
    // else is needed here.
    uint32_t pressMs = e.eventMs != 0 ? e.eventMs : millis();
    if (g_morsePatternLen > 0 && (pressMs - g_lastMorseReleaseMs) >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeMorseChar();
    }
    return;
  }
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
    // Hardware Fix #4.7d: store the physical accepted release time, not
    // processing-time millis().
    g_lastMorseReleaseMs = e.eventMs != 0 ? e.eventMs : millis();
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
      // Hardware Fix #4.7b: no editing cursor once the buffer has reached
      // maxLength -- there is nothing left to type into it.
      if (g_length >= g_config.maxLength) {
        suffixOut[0] = '\0';
      } else {
        snprintf(suffixOut, suffixCap, "_");
      }
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

ControlKind computeControlKind() {
  if (g_state == State::EMPTY && g_errorMessage != nullptr) return ControlKind::ERROR;
  // Hardware Fix #4.7b: informational only (never ERROR) once EMPTY-state
  // editing has reached maxLength -- checked after g_errorMessage so a
  // real validator failure (which also lands back in EMPTY) still takes
  // priority on the one tick it's shown.
  if (g_state == State::EMPTY && g_length >= g_config.maxLength) return ControlKind::MAX_LENGTH;
  if (g_state == State::CONFIRM) return ControlKind::CONFIRM;
  return ControlKind::NONE;
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

  ControlKind controlKind = computeControlKind();
  bool firstDraw = g_needsFullRedraw;

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
    // Control row is handled uniformly by the block below (g_lastControlKind
    // starts at NONE, so a real kind here is correctly treated as new).
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

  // Control row (Hardware Fix #3 corrective): a kind change (including
  // first draw, since g_lastControlKind starts at NONE) or an error-text
  // change redraws the whole row; a plain Save/Cancel toggle within the
  // same CONFIRM kind only moves the "> " marker between the two fixed
  // label positions -- "Save" and "Cancel" are never repainted.
  int16_t markerW = static_cast<int16_t>(Display::textWidth(">") + 4);
  int16_t saveMarkerX = 2;
  int16_t saveLabelX = static_cast<int16_t>(saveMarkerX + markerW);
  int16_t saveLabelW = Display::textWidth("Save");
  int16_t gapW = Display::textWidth("    ");
  int16_t cancelMarkerX = static_cast<int16_t>(saveLabelX + saveLabelW + gapW);
  int16_t cancelLabelX = static_cast<int16_t>(cancelMarkerX + markerW);

  // Hardware Fix #4.7b: MAX_LENGTH reuses the same single-line "message on
  // the control row" mechanism ERROR already used (g_lastErrorText as the
  // generic "last shown message" buffer) rather than duplicating it --
  // it's a different ControlKind (never tagged/treated as an error), just
  // the same neutral text-row drawing/diffing underneath.
  char maxLenMsg[24];
  snprintf(maxLenMsg, sizeof(maxLenMsg), "Max %u chars", static_cast<unsigned>(g_config.maxLength));
  const char* controlMessage = nullptr;
  if (controlKind == ControlKind::ERROR) controlMessage = g_errorMessage;
  else if (controlKind == ControlKind::MAX_LENGTH) controlMessage = maxLenMsg;

  bool kindChanged = (controlKind != g_lastControlKind);
  bool messageTextChanged = (controlMessage != nullptr) && strcmp(controlMessage, g_lastErrorText) != 0;

  if (kindChanged || messageTextChanged) {
    // PRIMARY is a GFX custom font and never draws with an opaque
    // background, so the row is explicitly erased before its replacement
    // is drawn.
    if (!firstDraw) Display::tft().fillRect(0, controlY, Display::kScreenWidth, lh, ST77XX_BLACK);
    if (controlKind == ControlKind::ERROR || controlKind == ControlKind::MAX_LENGTH) {
      Display::printLine(2, controlY, controlMessage);
    } else if (controlKind == ControlKind::CONFIRM) {
      if (g_confirmSaveSelected) Display::printLine(saveMarkerX, controlY, ">");
      Display::printLine(saveLabelX, controlY, "Save");
      if (!g_confirmSaveSelected) Display::printLine(cancelMarkerX, controlY, ">");
      Display::printLine(cancelLabelX, controlY, "Cancel");
    }
  } else if (controlKind == ControlKind::CONFIRM && g_confirmSaveSelected != g_lastConfirmSaveSelected) {
    int16_t oldMarkerX = g_lastConfirmSaveSelected ? saveMarkerX : cancelMarkerX;
    int16_t newMarkerX = g_confirmSaveSelected ? saveMarkerX : cancelMarkerX;
    Display::tft().fillRect(oldMarkerX, controlY, markerW, lh, ST77XX_BLACK);
    Display::tft().fillRect(newMarkerX, controlY, markerW, lh, ST77XX_BLACK);
    Display::printLine(newMarkerX, controlY, ">");
  }

  g_lastControlKind = controlKind;
  if (controlMessage != nullptr) {
    strncpy(g_lastErrorText, controlMessage, sizeof(g_lastErrorText) - 1);
    g_lastErrorText[sizeof(g_lastErrorText) - 1] = '\0';
  } else {
    g_lastErrorText[0] = '\0';
  }
  if (controlKind == ControlKind::CONFIRM) g_lastConfirmSaveSelected = g_confirmSaveSelected;

  g_lastNeededClip = needsClip;
  strncpy(g_lastClippedLine, clippedLine, sizeof(g_lastClippedLine) - 1);
  g_lastClippedLine[sizeof(g_lastClippedLine) - 1] = '\0';
  strncpy(g_lastPrefix, prefix, sizeof(g_lastPrefix) - 1);
  g_lastPrefix[sizeof(g_lastPrefix) - 1] = '\0';
  strncpy(g_lastSuffix, suffix, sizeof(g_lastSuffix) - 1);
  g_lastSuffix[sizeof(g_lastSuffix) - 1] = '\0';
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
  g_lastControlKind = ControlKind::NONE;
  g_lastErrorText[0] = '\0';
  g_lastConfirmSaveSelected = true;
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
