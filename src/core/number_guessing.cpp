#include "core/number_guessing.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/storage_init.h"

namespace NumberGuessing {

GuessResult evaluate(const uint8_t secret[kSecretDigits], const uint8_t guess[kSecretDigits]) {
  GuessResult r{0, 0};
  bool secretUsed[kSecretDigits] = {false, false, false, false};
  bool guessUsed[kSecretDigits] = {false, false, false, false};
  for (uint8_t i = 0; i < kSecretDigits; i++) {
    if (guess[i] == secret[i]) {
      r.a++;
      secretUsed[i] = true;
      guessUsed[i] = true;
    }
  }
  for (uint8_t i = 0; i < kSecretDigits; i++) {
    if (guessUsed[i]) continue;
    for (uint8_t j = 0; j < kSecretDigits; j++) {
      if (!secretUsed[j] && guess[i] == secret[j]) {
        r.b++;
        secretUsed[j] = true;
        break;
      }
    }
  }
  return r;
}

namespace {
int8_t firstAvailableDigit(const DigitEntryState& state) {
  for (int8_t d = 0; d < 10; d++) {
    bool used = false;
    for (uint8_t i = 0; i < state.count; i++) {
      if (state.digits[i] == d) used = true;
    }
    if (!used) return d;
  }
  return 0;
}

int8_t nextAvailableDigit(const DigitEntryState& state, int8_t current, int8_t direction) {
  for (uint8_t tries = 0; tries < 10; tries++) {
    current = static_cast<int8_t>((current + direction + 10) % 10);
    bool used = false;
    for (uint8_t i = 0; i < state.count; i++) {
      if (state.digits[i] == current) used = true;
    }
    if (!used) return current;
  }
  return current;
}
}  // namespace

void resetDigitEntry(DigitEntryState* state) {
  state->count = 0;
  state->previewDigit = firstAvailableDigit(*state);
  state->dotHeld = false;
  state->dotPressStartMs = 0;
  state->deleteFiredThisPress = false;
}

void handleDigitEntryEvent(DigitEntryState* state, const InputEvent& e) {
  if (e.type == InputEventType::ENCODER_ROTATE) {
    state->previewDigit = nextAvailableDigit(*state, state->previewDigit, static_cast<int8_t>(e.value));
  } else if (e.type == InputEventType::DOT_PRESS_START) {
    state->dotHeld = true;
    state->dotPressStartMs = millis();
    state->deleteFiredThisPress = false;
  } else if (e.type == InputEventType::DOT_RELEASE) {
    state->dotHeld = false;
    if (!state->deleteFiredThisPress) {
      // Short press: confirm the currently previewed digit.
      if (state->count < 4) {
        state->digits[state->count++] = static_cast<uint8_t>(state->previewDigit);
        state->previewDigit = firstAvailableDigit(*state);
      }
    }
  }
}

void tickDigitEntry(DigitEntryState* state) {
  if (!state->dotHeld || state->deleteFiredThisPress) return;
  if (millis() - state->dotPressStartMs >= 500) {
    if (state->count > 0) {
      state->count--;
      state->previewDigit = firstAvailableDigit(*state);
    }
    state->deleteFiredThisPress = true;
  }
}

void resetDigitRowRenderState(DigitRowRenderState* rs) {
  for (auto& c : rs->lastCell) c = 0;
  rs->neverDrawn = true;
}

// Widest glyph any cell can ever show (0-9 or the '_' placeholder) plus
// padding -- the single source of truth for the fixed cell pitch, so no
// cell's X ever depends on another cell's actual content width (Hardware
// Fix #4 issue 7).
int16_t digitCellWidth() {
  int16_t maxW = Display::textWidth("_");
  char buf[2] = {0, 0};
  for (char c = '0'; c <= '9'; c++) {
    buf[0] = c;
    int16_t w = Display::textWidth(buf);
    if (w > maxW) maxW = w;
  }
  return static_cast<int16_t>(maxW + 6);
}

void renderDigitRow(DigitRowRenderState* rs, int16_t x, int16_t y, const char* labelPrefix,
                    const DigitEntryState& state) {
  char cells[NumberGuessing::kSecretDigits];
  for (uint8_t i = 0; i < NumberGuessing::kSecretDigits; i++) {
    if (i < state.count) {
      cells[i] = static_cast<char>('0' + state.digits[i]);
    } else if (i == state.count) {
      cells[i] = static_cast<char>('0' + state.previewDigit);
    } else {
      cells[i] = '_';
    }
  }

  int16_t lh = Display::lineHeight();
  int16_t cellW = digitCellWidth();
  int16_t digitsX = static_cast<int16_t>(x + Display::textWidth(labelPrefix) + 4);

  bool firstDraw = rs->neverDrawn;
  if (firstDraw) {
    Display::printLine(x, y, labelPrefix);
    rs->neverDrawn = false;
  }

  // Each of the kSecretDigits fixed cells is diffed and redrawn completely
  // independently -- a preview rotation, a digit confirming, or a delete
  // only ever touches the specific cell(s) whose content actually changed,
  // never a cell before or after it (the hot path this renderer targets).
  for (uint8_t i = 0; i < NumberGuessing::kSecretDigits; i++) {
    if (!firstDraw && cells[i] == rs->lastCell[i]) continue;
    int16_t cellX = static_cast<int16_t>(digitsX + i * cellW);
    if (!firstDraw) Display::tft().fillRect(cellX, y, cellW, lh, ST77XX_BLACK);
    char buf[2] = {cells[i], '\0'};
    Display::printLine(cellX, y, buf);
    rs->lastCell[i] = cells[i];
  }
}

}  // namespace NumberGuessing

// =============================================================================
// Play Solo (Addendum section 12.2; Phase 3 section 9)
// =============================================================================
namespace {

using NumberGuessing::DigitEntryState;

constexpr uint16_t kMaxHistory = NumberGuessing::kMaxAttempts;

struct GuessEntry {
  uint16_t guessValue;
  uint8_t aCount;
  uint8_t bCount;
};

struct SoloState {
  bool hasPuzzle;
  uint8_t secret[4];
  bool solved;
  uint16_t totalAttempts;
  uint16_t historyCount;
  GuessEntry history[kMaxHistory];
};

SoloState g_solo;
DigitEntryState g_digitEntry;
uint16_t g_resultScrollIndex = 0;

void saveSolo() { Storage::solo().putBytes("state", &g_solo, sizeof(g_solo)); }
void loadSolo() {
  size_t got = Storage::solo().getBytes("state", &g_solo, sizeof(g_solo));
  if (got != sizeof(g_solo)) memset(&g_solo, 0, sizeof(g_solo));
}

void generateRandomSecret(uint8_t out[4]) {
  bool used[10] = {false};
  for (int i = 0; i < 4; i++) {
    int d;
    do {
      d = static_cast<int>(random(10));
    } while (used[d]);
    used[d] = true;
    out[i] = static_cast<uint8_t>(d);
  }
}

void startNewPuzzle() {
  generateRandomSecret(g_solo.secret);
  g_solo.hasPuzzle = true;
  g_solo.solved = false;
  g_solo.totalAttempts = 0;
  g_solo.historyCount = 0;
  saveSolo();
}

void screenSoloGuess();
void screenSoloResult();
void screenSoloUnsolvedMenu();
void screenSoloSolvedMenu();

void exitConfirmYes() { Menu::goBack(); }  // pops screenSoloGuess; confirmPromptScreen pops itself too

void submitGuess() {
  NumberGuessing::GuessResult r = NumberGuessing::evaluate(g_solo.secret, g_digitEntry.digits);
  if (g_solo.historyCount < kMaxHistory) {
    GuessEntry& e = g_solo.history[g_solo.historyCount++];
    e.guessValue = static_cast<uint16_t>(g_digitEntry.digits[0] * 1000 + g_digitEntry.digits[1] * 100 +
                                         g_digitEntry.digits[2] * 10 + g_digitEntry.digits[3]);
    e.aCount = r.a;
    e.bCount = r.b;
  }
  g_solo.totalAttempts++;

  if (r.a == 4) {
    g_solo.solved = true;
    saveSolo();
    Menu::goBack();
    Menu::pushScreen(screenSoloResult);
    return;
  }
  saveSolo();
  NumberGuessing::resetDigitEntry(&g_digitEntry);
}

bool g_soloGuessDirty = true;
bool g_soloGuessNeedsFullRedraw = true;
char g_lastSoloAttemptLine[32] = {0};
char g_lastSoloClueLine[24] = {0};
NumberGuessing::DigitRowRenderState g_soloGuessRowState;

void screenSoloGuess() {
  if (Menu::consumeJustEntered()) {
    NumberGuessing::resetDigitEntry(&g_digitEntry);
    g_soloGuessDirty = true;
    g_soloGuessNeedsFullRedraw = true;
    NumberGuessing::resetDigitRowRenderState(&g_soloGuessRowState);
  }

  Input::update();
  InputEvent e;
  bool hadEvent = false;
  while (Input::popEvent(e)) {
    hadEvent = true;
    if (e.type == InputEventType::ENCODER_SHORT) {
      if (g_digitEntry.count == 4) {
        submitGuess();
      } else {
        ConfirmPromptConfig cfg{"Exit this game?", nullptr, false, exitConfirmYes, nullptr};
        Menu::startConfirmPrompt(cfg);
        Menu::pushScreen(Menu::confirmPromptScreen);
      }
    } else if (Input::isBack(e)) {
      Menu::goBack();  // immediate exit, no confirmation (Phase 3 section 12)
    } else {
      NumberGuessing::handleDigitEntryEvent(&g_digitEntry, e);
    }
  }
  if (hadEvent) g_soloGuessDirty = true;

  uint8_t countBefore = g_digitEntry.count;
  NumberGuessing::tickDigitEntry(&g_digitEntry);
  if (g_digitEntry.count != countBefore) g_soloGuessDirty = true;  // hold-to-delete fired

  Display::drawStatusBar();
  if (!g_soloGuessDirty) return;
  g_soloGuessDirty = false;

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t y = Display::kStatusBarHeight + 2;

  char attemptLine[32];
  snprintf(attemptLine, sizeof(attemptLine), "Attempt %d", g_solo.totalAttempts + 1);

  bool firstDraw = g_soloGuessNeedsFullRedraw;
  if (firstDraw) {
    Display::clearContentArea();
    Display::printLine(2, y, attemptLine);
    g_soloGuessNeedsFullRedraw = false;
  } else if (strcmp(attemptLine, g_lastSoloAttemptLine) != 0) {
    Display::tft().fillRect(0, y, Display::kScreenWidth, lh, ST77XX_BLACK);
    Display::printLine(2, y, attemptLine);
  }
  strncpy(g_lastSoloAttemptLine, attemptLine, sizeof(g_lastSoloAttemptLine) - 1);
  g_lastSoloAttemptLine[sizeof(g_lastSoloAttemptLine) - 1] = '\0';
  y += lh;

  NumberGuessing::renderDigitRow(&g_soloGuessRowState, 2, y, "Guess: ", g_digitEntry);
  y += lh;

  // Hardware Fix #4 issue 8: persistent immediate clue after an incorrect
  // guess, reusing submitGuess()'s already-computed A/B result (the most
  // recent history entry) rather than duplicating the evaluate() logic.
  // Digits = A+B (total correct digits regardless of position), Pos = A
  // (correct digits in the correct position). Stays visible until the
  // next guess replaces it, or a new puzzle clears history (historyCount
  // back to 0 makes the clue disappear with it -- no separate reset
  // needed). Diffed independently so it never repaints the Guess row.
  char clueLine[24] = {0};
  if (g_solo.historyCount > 0) {
    const GuessEntry& last = g_solo.history[g_solo.historyCount - 1];
    uint8_t digitsCorrect = static_cast<uint8_t>(last.aCount + last.bCount);
    snprintf(clueLine, sizeof(clueLine), "Digits: %u  Pos: %u", digitsCorrect, last.aCount);
  }
  if (firstDraw || strcmp(clueLine, g_lastSoloClueLine) != 0) {
    if (!firstDraw) Display::tft().fillRect(0, y, Display::kScreenWidth, lh, ST77XX_BLACK);
    if (clueLine[0] != '\0') Display::printLine(2, y, clueLine);
    strncpy(g_lastSoloClueLine, clueLine, sizeof(g_lastSoloClueLine) - 1);
    g_lastSoloClueLine[sizeof(g_lastSoloClueLine) - 1] = '\0';
  }
}

// Hardware Fix #3: same three-way redraw split as ListMenu -- full draw
// only on first entry, a viewport scroll redraws just the row region, and
// a same-viewport cursor move touches only the marker cells of the old
// and new selected row. History entries are immutable once recorded (this
// is a view-only screen), so labels never need their own diff.
bool g_soloResultNeedsFullRedraw = true;
int16_t g_soloResultLastStart = -1;
uint16_t g_soloResultLastSelected = 0;

void screenSoloResult() {
  if (Menu::consumeJustEntered()) {
    g_resultScrollIndex = (g_solo.historyCount > 0) ? static_cast<uint16_t>(g_solo.historyCount - 1) : 0;
    g_soloResultNeedsFullRedraw = true;
  }
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      int32_t next = static_cast<int32_t>(g_resultScrollIndex) + e.value;
      if (next < 0) next = 0;
      if (next >= g_solo.historyCount) next = g_solo.historyCount > 0 ? g_solo.historyCount - 1 : 0;
      g_resultScrollIndex = static_cast<uint16_t>(next);
    } else if (Input::isMenuConfirm(e) || Input::isBack(e)) {
      Menu::goBack();
    }
  }

  Display::drawStatusBar();

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t titleY = Display::kStatusBarHeight + 2;
  int16_t rowsTopY = static_cast<int16_t>(titleY + lh);

  int16_t remaining = Display::kScreenHeight - rowsTopY;
  uint16_t rows = (remaining > 0) ? static_cast<uint16_t>(remaining / lh) : 0;
  if (rows == 0) rows = 1;
  int16_t start = 0;
  if (g_resultScrollIndex >= rows) start = static_cast<int16_t>(g_resultScrollIndex - rows + 1);

  bool firstDraw = g_soloResultNeedsFullRedraw;
  bool scrolled = !firstDraw && (start != g_soloResultLastStart);
  bool selectionOnlyChanged = !firstDraw && !scrolled && (g_resultScrollIndex != g_soloResultLastSelected);
  if (!firstDraw && !scrolled && !selectionOnlyChanged) return;

  int16_t markerW = static_cast<int16_t>(Display::textWidth(">") + 4);
  int16_t labelX = static_cast<int16_t>(2 + markerW);

  if (firstDraw) {
    Display::clearContentArea();
    char titleLine[32];
    snprintf(titleLine, sizeof(titleLine), "Total attempts: %d", g_solo.totalAttempts);
    Display::printLine(2, titleY, titleLine);
    for (uint16_t i = static_cast<uint16_t>(start); i < g_solo.historyCount && i < start + rows; i++) {
      int16_t rowY = static_cast<int16_t>(rowsTopY + (i - start) * lh);
      const GuessEntry& g = g_solo.history[i];
      if (i == g_resultScrollIndex) Display::printLine(2, rowY, ">");
      char line[24];
      snprintf(line, sizeof(line), "%04d %dA%dB", g.guessValue, g.aCount, g.bCount);
      Display::printLine(labelX, rowY, line);
    }
    g_soloResultNeedsFullRedraw = false;
  } else if (scrolled) {
    int16_t regionH = static_cast<int16_t>(Display::kScreenHeight - rowsTopY);
    if (regionH < 0) regionH = 0;
    Display::tft().fillRect(0, rowsTopY, Display::kScreenWidth, regionH, ST77XX_BLACK);
    for (uint16_t i = static_cast<uint16_t>(start); i < g_solo.historyCount && i < start + rows; i++) {
      int16_t rowY = static_cast<int16_t>(rowsTopY + (i - start) * lh);
      const GuessEntry& g = g_solo.history[i];
      if (i == g_resultScrollIndex) Display::printLine(2, rowY, ">");
      char line[24];
      snprintf(line, sizeof(line), "%04d %dA%dB", g.guessValue, g.aCount, g.bCount);
      Display::printLine(labelX, rowY, line);
    }
  } else if (selectionOnlyChanged) {
    int16_t oldY = static_cast<int16_t>(rowsTopY + (g_soloResultLastSelected - start) * lh);
    int16_t newY = static_cast<int16_t>(rowsTopY + (g_resultScrollIndex - start) * lh);
    Display::tft().fillRect(2, oldY, markerW, lh, ST77XX_BLACK);
    Display::tft().fillRect(2, newY, markerW, lh, ST77XX_BLACK);
    Display::printLine(2, newY, ">");
  }

  g_soloResultLastStart = start;
  g_soloResultLastSelected = g_resultScrollIndex;
}

void soloContinueTrampoline() {
  Menu::goBack();
  Menu::pushScreen(screenSoloGuess);
}
void soloNewPuzzleTrampoline() {
  startNewPuzzle();
  Menu::goBack();
  Menu::pushScreen(screenSoloGuess);
}
void soloViewResultTrampoline() {
  Menu::goBack();
  Menu::pushScreen(screenSoloResult);
}

const SettingItem kUnsolvedItems[] = {
    {"Continue", soloContinueTrampoline},
    {"New Random Puzzle", soloNewPuzzleTrampoline},
};
const SettingItem kSolvedItems[] = {
    {"View Last Result", soloViewResultTrampoline},
    {"New Random Puzzle", soloNewPuzzleTrampoline},
};
ListMenu g_soloMenuListMenu;

void screenSoloUnsolvedMenu() {
  if (Menu::consumeJustEntered()) g_soloMenuListMenu.configure(kUnsolvedItems, 2);
  Display::drawStatusBar();
  g_soloMenuListMenu.tick("Play Solo");
}
void screenSoloSolvedMenu() {
  if (Menu::consumeJustEntered()) g_soloMenuListMenu.configure(kSolvedItems, 2);
  Display::drawStatusBar();
  g_soloMenuListMenu.tick("Play Solo");
}

void screenSoloEntry() {
  if (!g_solo.hasPuzzle) {
    startNewPuzzle();
    Menu::goBack();
    Menu::pushScreen(screenSoloGuess);
    return;
  }
  Menu::goBack();
  Menu::pushScreen(g_solo.solved ? screenSoloSolvedMenu : screenSoloUnsolvedMenu);
}

void serviceInit() { loadSolo(); }

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = nullptr;
    registerAppService(svc);
    registerGameSubModeHandler(GameSubModes::SOLO, screenSoloEntry);
  }
};
Registrar g_registrar;

}  // namespace
