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

void screenSoloGuess() {
  if (Menu::consumeJustEntered()) NumberGuessing::resetDigitEntry(&g_digitEntry);

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
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
  NumberGuessing::tickDigitEntry(&g_digitEntry);

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  char line[32];
  snprintf(line, sizeof(line), "Attempt %d", g_solo.totalAttempts + 1);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print(line);

  char guessLine[16] = "____";
  for (uint8_t i = 0; i < g_digitEntry.count; i++) guessLine[i] = static_cast<char>('0' + g_digitEntry.digits[i]);
  if (g_digitEntry.count < 4) guessLine[g_digitEntry.count] = static_cast<char>('0' + g_digitEntry.previewDigit);
  tft.setCursor(2, Display::kStatusBarHeight + 20);
  tft.print("Guess: ");
  tft.print(guessLine);
}

void screenSoloResult() {
  if (Menu::consumeJustEntered()) {
    g_resultScrollIndex = (g_solo.historyCount > 0) ? static_cast<uint16_t>(g_solo.historyCount - 1) : 0;
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
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  char line[32];
  snprintf(line, sizeof(line), "Total attempts: %d", g_solo.totalAttempts);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print(line);

  constexpr uint16_t kRows = 4;
  uint16_t start = (g_resultScrollIndex >= kRows) ? static_cast<uint16_t>(g_resultScrollIndex - kRows + 1) : 0;
  int16_t y = Display::kStatusBarHeight + 16;
  for (uint16_t i = start; i < g_solo.historyCount && i < start + kRows; i++) {
    const GuessEntry& g = g_solo.history[i];
    snprintf(line, sizeof(line), "%s%04d %dA%dB", i == g_resultScrollIndex ? "> " : "  ", g.guessValue, g.aCount,
             g.bCount);
    tft.setCursor(2, y);
    tft.print(line);
    y += 10;
  }
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
