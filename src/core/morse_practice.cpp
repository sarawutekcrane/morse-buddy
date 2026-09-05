#include "core/morse_practice.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/morse.h"
#include "core/settings.h"
#include "core/sound_facade.h"
#include "core/storage_init.h"

namespace {

// =============================================================================
// Datasets (Addendum section 20). Level 1 is a plain 36-character charset,
// no list needed. Level 2/3 are fixed, exact lists generated once here and
// hard-coded permanently, per the Addendum's instruction — never
// re-randomized or regenerated at runtime.
// =============================================================================
constexpr const char kLevel1Charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";  // 36 choices
constexpr uint8_t kLevel1CharsetLen = 36;

constexpr const char* kLevel2Words[] = {
    "CAT",  "DOG",  "SUN",  "RUN",  "BOX",  "CUP",  "PEN",  "BED",  "CAR",  "BUS",
    "HAT",  "MAP",  "KEY",  "LEG",  "ARM",  "EAR",  "EYE",  "LIP",  "BAG",  "TOY",
    "JAR",  "LID",  "NET",  "WEB",  "OWL",  "BEE",  "ANT",  "FOX",  "COW",  "PIG",
    "HEN",  "EGG",  "FISH", "BIRD", "TREE", "BOOK", "DOOR", "LAMP", "SHOE", "RAIN",
    "SNOW", "WIND", "STAR", "MOON", "FIRE", "ROCK", "SAND", "LAKE", "HILL", "ROAD",
};
constexpr uint8_t kLevel2WordCount = sizeof(kLevel2Words) / sizeof(kLevel2Words[0]);

constexpr const char* kLevel3Sentences[] = {
    "THE SUN IS HOT",       "THE DOG CAN RUN",      "I LIKE MY CAT",        "THE SKY IS BLUE",
    "SHE HAS A RED HAT",    "WE SEE A BIG TREE",    "THE BIRD CAN FLY",     "HE HAS A NEW BIKE",
    "THE CAKE IS SWEET",    "MY DOG LIKES TO RUN",  "THE MOON IS BRIGHT",   "WE PLAY IN THE PARK",
    "THE FISH CAN SWIM",    "SHE READS A BOOK",     "THE RAIN IS COLD",     "HE DRIVES A CAR",
    "THE STAR IS FAR",      "WE EAT LUNCH AT NOON", "THE FIRE IS WARM",     "I SEE A TALL HILL",
};
constexpr uint8_t kLevel3SentenceCount = sizeof(kLevel3Sentences) / sizeof(kLevel3Sentences[0]);

// =============================================================================
// Settings: Audio Preview / Reveal Answer (Off default), persisted in the
// generic core bucket (Phase 1 provisioned no dedicated Practice bucket).
// =============================================================================
bool g_audioPreview = false;
bool g_revealAnswer = false;

void loadPracticeSettings() {
  Preferences& p = Storage::core();
  g_audioPreview = p.isKey("prAudioPv") ? p.getBool("prAudioPv") : false;
  g_revealAnswer = p.isKey("prRevealAns") ? p.getBool("prRevealAns") : false;
}

void audioPreviewOff() {
  g_audioPreview = false;
  Storage::core().putBool("prAudioPv", false);
  Menu::goBack();
}
void audioPreviewOn() {
  g_audioPreview = true;
  Storage::core().putBool("prAudioPv", true);
  Menu::goBack();
}
const SettingItem kAudioPreviewItems[] = {{"Off", audioPreviewOff}, {"On", audioPreviewOn}};
ListMenu g_audioPreviewListMenu;
void screenAudioPreviewPicker() {
  if (Menu::consumeJustEntered()) g_audioPreviewListMenu.configure(kAudioPreviewItems, 2);
  Display::drawStatusBar();
  g_audioPreviewListMenu.tick("Audio Preview");
}

void revealAnswerOff() {
  g_revealAnswer = false;
  Storage::core().putBool("prRevealAns", false);
  Menu::goBack();
}
void revealAnswerOn() {
  g_revealAnswer = true;
  Storage::core().putBool("prRevealAns", true);
  Menu::goBack();
}
const SettingItem kRevealAnswerItems[] = {{"Off", revealAnswerOff}, {"On", revealAnswerOn}};
ListMenu g_revealAnswerListMenu;
void screenRevealAnswerPicker() {
  if (Menu::consumeJustEntered()) g_revealAnswerListMenu.configure(kRevealAnswerItems, 2);
  Display::drawStatusBar();
  g_revealAnswerListMenu.tick("Reveal Answer");
}

// =============================================================================
// Per-level high score (highest single-question score, not a session total).
// =============================================================================
uint8_t g_highScore[3] = {0, 0, 0};

void loadHighScores() {
  Preferences& p = Storage::core();
  g_highScore[0] = p.isKey("prHi1") ? p.getUChar("prHi1") : 0;
  g_highScore[1] = p.isKey("prHi2") ? p.getUChar("prHi2") : 0;
  g_highScore[2] = p.isKey("prHi3") ? p.getUChar("prHi3") : 0;
}
void saveHighScore(uint8_t levelIdx) {
  const char* keys[3] = {"prHi1", "prHi2", "prHi3"};
  Storage::core().putUChar(keys[levelIdx], g_highScore[levelIdx]);
}

// =============================================================================
// Challenge generation + audio preview
// =============================================================================
char g_challengeText[24];
char g_lastChallengeText[24] = "";
uint32_t g_challengeShownAtMs = 0;
uint16_t g_streak = 0;

void generateChallenge(uint8_t level, char* out, size_t outCap) {
  char candidate[24];
  do {
    if (level == 1) {
      candidate[0] = kLevel1Charset[random(kLevel1CharsetLen)];
      candidate[1] = '\0';
    } else if (level == 2) {
      strncpy(candidate, kLevel2Words[random(kLevel2WordCount)], sizeof(candidate) - 1);
      candidate[sizeof(candidate) - 1] = '\0';
    } else {
      strncpy(candidate, kLevel3Sentences[random(kLevel3SentenceCount)], sizeof(candidate) - 1);
      candidate[sizeof(candidate) - 1] = '\0';
    }
  } while (strcmp(candidate, g_lastChallengeText) == 0);
  strncpy(out, candidate, outCap - 1);
  out[outCap - 1] = '\0';
  strncpy(g_lastChallengeText, candidate, sizeof(g_lastChallengeText) - 1);
  g_lastChallengeText[sizeof(g_lastChallengeText) - 1] = '\0';
}

// Same raw-Morse builder shape as Text/Enigma's (duplicated locally per
// established Phase 3 precedent — each mode's compose/display helpers are
// private to its own file).
void buildRawMorse(const char* text, char* out, size_t outSize) {
  size_t pos = 0;
  out[0] = '\0';
  size_t len = strlen(text);
  for (size_t i = 0; i < len && pos + 1 < outSize; i++) {
    char c = text[i];
    if (c == ' ') {
      out[pos++] = '/';
    } else {
      char pat[Morse::kMaxPatternLength + 1];
      if (Morse::encodeChar(c, pat, sizeof(pat))) {
        size_t patLen = strlen(pat);
        if (pos + patLen < outSize) {
          memcpy(out + pos, pat, patLen);
          pos += patLen;
        }
      }
    }
    if (pos + 1 < outSize) out[pos++] = ' ';
  }
  out[pos] = '\0';
}

constexpr size_t kMaxToneDurations = 300;

size_t buildToneDurations(const char* text, uint8_t wpm, uint16_t* out, size_t outCap) {
  size_t n = 0;
  bool first = true;
  uint16_t pendingGapMs = 0;
  size_t len = strlen(text);
  for (size_t ci = 0; ci < len; ci++) {
    char c = text[ci];
    if (c == ' ') {
      pendingGapMs = Morse::wordGapMs(wpm);
      continue;
    }
    char pattern[Morse::kMaxPatternLength + 1];
    if (!Morse::encodeChar(c, pattern, sizeof(pattern))) continue;
    size_t patLen = strlen(pattern);
    for (size_t si = 0; si < patLen; si++) {
      if (n + 2 > outCap) return n;
      if (!first) {
        uint16_t gap = (si == 0) ? (pendingGapMs != 0 ? pendingGapMs : Morse::letterGapMs(wpm)) : Morse::ditMs(wpm);
        out[n++] = gap;
      }
      out[n++] = (pattern[si] == '.') ? Morse::ditMs(wpm) : static_cast<uint16_t>(3 * Morse::ditMs(wpm));
      first = false;
    }
    pendingGapMs = 0;
  }
  return n;
}

void playChallengeAudio() {
  static uint16_t durations[kMaxToneDurations];
  size_t count = buildToneDurations(g_challengeText, Settings::getWpm(), durations, kMaxToneDurations);
  if (count == 0) return;
  playToneSequence(600, durations, count, SOUND_GAME);
}

// =============================================================================
// Screen state
// =============================================================================
enum class PracticeCursor : uint8_t { CHALLENGE, ANSWER };
PracticeCursor g_cursor = PracticeCursor::CHALLENGE;
bool g_revealHeld = false;

char g_answerText[24];
uint8_t g_answerLen = 0;
char g_answerPattern[Morse::kMaxPatternLength + 1];
uint8_t g_answerPatternLen = 0;
uint32_t g_lastAnswerReleaseMs = 0;

const char* g_resultText = nullptr;
uint32_t g_resultShownUntilMs = 0;

void resetAnswerCompose() {
  g_answerLen = 0;
  g_answerText[0] = '\0';
  g_answerPatternLen = 0;
  g_answerPattern[0] = '\0';
}

void startNewChallenge() {
  uint8_t level = Settings::getPracticeLevel();
  generateChallenge(level, g_challengeText, sizeof(g_challengeText));
  g_challengeShownAtMs = millis();
  resetAnswerCompose();
  g_cursor = PracticeCursor::CHALLENGE;
  g_revealHeld = false;
  if (g_audioPreview) playChallengeAudio();
}

void finalizeAnswerChar() {
  char c = Morse::decodePattern(g_answerPattern);
  if (g_answerLen < sizeof(g_answerText) - 1) {
    g_answerText[g_answerLen++] = c;
    g_answerText[g_answerLen] = '\0';
  }
  g_answerPatternLen = 0;
  g_answerPattern[0] = '\0';
}

void submitAnswer() {
  bool correct = strcmp(g_answerText, g_challengeText) == 0;
  bool scoringActive = !g_audioPreview && !g_revealAnswer;
  uint8_t levelIdx = static_cast<uint8_t>(Settings::getPracticeLevel() - 1);

  static char resultBuf[40];
  if (correct) {
    if (scoringActive) {
      uint32_t elapsedSec = (millis() - g_challengeShownAtMs) / 1000;
      int32_t score = 100 - static_cast<int32_t>(elapsedSec) * 2;
      if (score < 10) score = 10;
      if (score > 100) score = 100;
      g_streak++;
      if (static_cast<uint8_t>(score) > g_highScore[levelIdx]) {
        g_highScore[levelIdx] = static_cast<uint8_t>(score);
        saveHighScore(levelIdx);
      }
      if (g_streak >= 5) {
        uint8_t level = Settings::getPracticeLevel();
        if (level < 3) Settings::setPracticeLevel(static_cast<uint8_t>(level + 1));
        g_streak = 0;
      }
      snprintf(resultBuf, sizeof(resultBuf), "Correct! +%ld", static_cast<long>(score));
    } else {
      snprintf(resultBuf, sizeof(resultBuf), "Correct!");
    }
  } else {
    if (scoringActive) g_streak = 0;
    snprintf(resultBuf, sizeof(resultBuf), "Wrong: %s", g_challengeText);
  }
  g_resultText = resultBuf;
  g_resultShownUntilMs = millis() + 1500;
  startNewChallenge();
}

void handleAnswerEvent(const InputEvent& e) {
  if (e.type == InputEventType::DOT_RELEASE) {
    if (e.durationMs >= Morse::kSpecialCommandMs) {
      resetAnswerCompose();
      return;
    }
    Morse::SymbolClass sc = Morse::classifyPress(e.durationMs, Settings::getWpm());
    if (g_answerPatternLen < Morse::kMaxPatternLength) {
      g_answerPattern[g_answerPatternLen++] = (sc == Morse::SymbolClass::DOT) ? '.' : '-';
      g_answerPattern[g_answerPatternLen] = '\0';
    }
    g_lastAnswerReleaseMs = millis();
    if (Morse::isDeletePattern(g_answerPattern)) {
      if (g_answerLen > 0) {
        g_answerLen--;
        g_answerText[g_answerLen] = '\0';
      }
      g_answerPatternLen = 0;
      g_answerPattern[0] = '\0';
    }
  } else if (e.type == InputEventType::ENCODER_SHORT) {
    submitAnswer();
  }
}

void handleChallengeEvent(const InputEvent& e) {
  if (e.type == InputEventType::DOT_PRESS_START) {
    if (g_audioPreview) playChallengeAudio();
    if (g_revealAnswer) g_revealHeld = true;
  } else if (e.type == InputEventType::DOT_RELEASE) {
    g_revealHeld = false;
  }
  // Encoder short: no effect on the Challenge cursor.
}

void screenPractice() {
  if (Menu::consumeJustEntered()) {
    g_streak = 0;
    startNewChallenge();
  }

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) {
      Menu::goBack();
      continue;
    }
    if (e.type == InputEventType::ENCODER_ROTATE) {
      g_cursor = (g_cursor == PracticeCursor::CHALLENGE) ? PracticeCursor::ANSWER : PracticeCursor::CHALLENGE;
    } else if (g_cursor == PracticeCursor::CHALLENGE) {
      handleChallengeEvent(e);
    } else {
      handleAnswerEvent(e);
    }
  }

  if (g_cursor == PracticeCursor::ANSWER && g_answerPatternLen > 0) {
    if (millis() - g_lastAnswerReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeAnswerChar();
    }
  }

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  char line[40];
  snprintf(line, sizeof(line), "Level %u  Streak %u", Settings::getPracticeLevel(), g_streak);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print(line);

  tft.setCursor(2, Display::kStatusBarHeight + 16);
  tft.print(g_cursor == PracticeCursor::CHALLENGE ? "> " : "  ");
  if (g_revealHeld) {
    tft.print(g_challengeText);
  } else {
    char raw[160];
    buildRawMorse(g_challengeText, raw, sizeof(raw));
    tft.print(raw);
  }

  tft.setCursor(2, Display::kStatusBarHeight + 30);
  tft.print(g_cursor == PracticeCursor::ANSWER ? "> " : "  ");
  char answerLine[40];
  snprintf(answerLine, sizeof(answerLine), "%s%s%s", g_answerText, (g_answerPatternLen > 0 ? " " : ""),
           g_answerPattern);
  tft.print(answerLine);

  if (g_resultText != nullptr && millis() < g_resultShownUntilMs) {
    tft.setCursor(2, Display::kStatusBarHeight + 44);
    tft.print(g_resultText);
  }

  char hiLine[24];
  snprintf(hiLine, sizeof(hiLine), "High: %u", g_highScore[Settings::getPracticeLevel() - 1]);
  tft.setCursor(2, Display::kStatusBarHeight + 58);
  tft.print(hiLine);
}

// Storage::init() runs from setup() after every global constructor has
// already run, so NVS reads must happen in an AppService.init callback
// (invoked by initRegisteredServices(), also from setup(), after Storage::
// init()) rather than directly in the Registrar constructor below.
void serviceInit() {
  loadPracticeSettings();
  loadHighScores();
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = nullptr;
    registerAppService(svc);
    registerSettingItem(Settings::kMorsePracticeListId, SettingItem{"Audio Preview", screenAudioPreviewPicker});
    registerSettingItem(Settings::kMorsePracticeListId, SettingItem{"Reveal Answer", screenRevealAnswerPicker});
    Settings::registerMorsePracticeStartHandler(screenPractice);
  }
};
Registrar g_registrar;

}  // namespace
