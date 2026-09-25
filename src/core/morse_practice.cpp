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

// Hardware Fix #4.1: confirmed in ListMenu::SelectionMode::IN_PLACE, so
// these callbacks save the value and stay on this same picker screen --
// no Menu::goBack(), no clearContentArea(); ListMenu's badge-cell-only
// redraw updates just the "*" in place.
void audioPreviewOff() {
  g_audioPreview = false;
  Storage::core().putBool("prAudioPv", false);
}
void audioPreviewOn() {
  g_audioPreview = true;
  Storage::core().putBool("prAudioPv", true);
}
const SettingItem kAudioPreviewItems[] = {{"Off", audioPreviewOff}, {"On", audioPreviewOn}};
// Hardware Fix #4 issue 1: the picker opens on the saved value and marks it
// with a "*" badge (ListMenu's existing per-item BadgeFn mechanism) so the
// active setting is visible independent of the "> " cursor.
bool audioPreviewBadgeOff() { return !g_audioPreview; }
bool audioPreviewBadgeOn() { return g_audioPreview; }
const BadgeFn kAudioPreviewBadges[] = {audioPreviewBadgeOff, audioPreviewBadgeOn};
ListMenu g_audioPreviewListMenu;
void screenAudioPreviewPicker() {
  if (Menu::consumeJustEntered()) {
    g_audioPreviewListMenu.configure(kAudioPreviewItems, 2, kAudioPreviewBadges, g_audioPreview ? 1 : 0,
                                      ListMenu::SelectionMode::IN_PLACE);
  }
  Display::drawStatusBar();
  g_audioPreviewListMenu.tick("Audio Preview");
}

void revealAnswerOff() {
  g_revealAnswer = false;
  Storage::core().putBool("prRevealAns", false);
}
void revealAnswerOn() {
  g_revealAnswer = true;
  Storage::core().putBool("prRevealAns", true);
}
const SettingItem kRevealAnswerItems[] = {{"Off", revealAnswerOff}, {"On", revealAnswerOn}};
bool revealAnswerBadgeOff() { return !g_revealAnswer; }
bool revealAnswerBadgeOn() { return g_revealAnswer; }
const BadgeFn kRevealAnswerBadges[] = {revealAnswerBadgeOff, revealAnswerBadgeOn};
ListMenu g_revealAnswerListMenu;
void screenRevealAnswerPicker() {
  if (Menu::consumeJustEntered()) {
    g_revealAnswerListMenu.configure(kRevealAnswerItems, 2, kRevealAnswerBadges, g_revealAnswer ? 1 : 0,
                                      ListMenu::SelectionMode::IN_PLACE);
  }
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
PracticeCursor g_cursor = PracticeCursor::ANSWER;
bool g_revealHeld = false;

char g_answerText[24];
uint8_t g_answerLen = 0;
char g_answerPattern[Morse::kMaxPatternLength + 1];
uint8_t g_answerPatternLen = 0;
uint32_t g_lastAnswerReleaseMs = 0;
Morse::WordGapState g_answerWordGap;
// True while the pattern currently being keyed (g_answerPattern) is known
// to start a new word -- captured once, at that pattern's FIRST symbol
// (see captureAnswerWordBoundaryOnSymbolStart()), and left untouched by
// symbol 2/3/... of the same pattern. Only consumed -- as an actual ASCII
// space, committed immediately before the decoded character -- by
// finalizeAnswerChar()'s NORMAL letter finalization; a delete prosign or
// a special-command clear discard it without ever writing a space
// (Hardware Fix #4.2).
bool g_answerPatternStartsNewWord = false;

const char* g_resultText = nullptr;
uint32_t g_resultShownUntilMs = 0;

void resetAnswerCompose() {
  g_answerLen = 0;
  g_answerText[0] = '\0';
  g_answerPatternLen = 0;
  g_answerPattern[0] = '\0';
  g_answerPatternStartsNewWord = false;
  Morse::cancelWordGap(&g_answerWordGap);
}

// Called on every DOT_PRESS_START, before the new symbol is accepted into
// the pattern buffer (Hardware Fix #4.2). Only captures whether the
// pattern now starting is a new word -- and only at that pattern's FIRST
// symbol (g_answerPatternLen == 0); symbol 2/3/... of a multi-symbol
// character (e.g. W = .--) must never re-resolve or overwrite this flag.
// Never mutates the answer draft itself -- see finalizeAnswerChar(). Level
// 3 sentences need real spaces (e.g. "THE SUN IS HOT", not "THESUNISHOT"),
// but a space must only ever be committed at normal letter finalization --
// never merely because the user paused before Submit (which would
// otherwise turn a correct "CAT" answer into "CAT " and fail
// submitAnswer()'s exact strcmp()), and never for a delete prosign.
void captureAnswerWordBoundaryOnSymbolStart() {
  if (g_answerPatternLen != 0) return;
  g_answerPatternStartsNewWord =
      Morse::consumeWordBoundaryOnSymbolStart(&g_answerWordGap, Settings::getWpm(), millis());
}

void startNewChallenge() {
  uint8_t level = Settings::getPracticeLevel();
  generateChallenge(level, g_challengeText, sizeof(g_challengeText));
  g_challengeShownAtMs = millis();
  resetAnswerCompose();
  // Hardware Fix #4.3 issue B: default focus to ANSWER on every new
  // challenge (both first entry and after Submit) so the user can start
  // keying Morse immediately without first having to rotate the encoder
  // away from CHALLENGE. CHALLENGE remains reachable by rotating there
  // (for Audio Preview / Reveal Answer).
  g_cursor = PracticeCursor::ANSWER;
  g_revealHeld = false;
  if (g_audioPreview) playChallengeAudio();
}

void finalizeAnswerChar() {
  char c = Morse::decodePattern(g_answerPattern);
  // NORMAL character finalization is the only place a word separator is
  // ever actually committed (Hardware Fix #4.2) -- and only as one atomic
  // write together with the character it separates, so a boundary is
  // never left as a trailing space with no room for the letter that was
  // supposed to follow it.
  bool needsSpace = g_answerPatternStartsNewWord && g_answerLen > 0 && g_answerText[g_answerLen - 1] != ' ';
  size_t needed = needsSpace ? 2 : 1;
  if (g_answerLen + needed <= sizeof(g_answerText) - 1) {
    if (needsSpace) g_answerText[g_answerLen++] = ' ';
    g_answerText[g_answerLen++] = c;
    g_answerText[g_answerLen] = '\0';
  } else if (g_answerLen < sizeof(g_answerText) - 1) {
    g_answerText[g_answerLen++] = c;
    g_answerText[g_answerLen] = '\0';
  }
  g_answerPatternStartsNewWord = false;
  g_answerPatternLen = 0;
  g_answerPattern[0] = '\0';
  Morse::armWordGap(&g_answerWordGap, g_lastAnswerReleaseMs);
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
  if (e.type == InputEventType::DOT_PRESS_START) {
    // Only captures whether this new pattern starts a new word (Hardware
    // Fix #4.2) -- never mutates the answer draft itself. The space (if
    // any) is committed later, only at NORMAL letter finalization; a
    // delete prosign discards the captured flag below instead of
    // consuming it as a space.
    captureAnswerWordBoundaryOnSymbolStart();
  } else if (e.type == InputEventType::DOT_RELEASE) {
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
      // A delete prosign never commits a word separator -- it must remove
      // the previous REAL confirmed character, not an auto-inserted space
      // that was never actually written to the answer draft (Hardware Fix
      // #4.2).
      g_answerPatternStartsNewWord = false;
      if (g_answerLen > 0) {
        g_answerLen--;
        g_answerText[g_answerLen] = '\0';
      }
      g_answerPatternLen = 0;
      g_answerPattern[0] = '\0';
      Morse::cancelWordGap(&g_answerWordGap);
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

bool g_practiceDirty = true;
bool g_practiceWasShowingResult = false;

// Hardware Fix #4.4 issue E: Level 3 challenges rendered as RAW Morse (the
// default Typing Display view) can be far wider than one physical row --
// "THE SUN IS HOT" alone expands to dozens of dot/dash/slash characters --
// and "Wrong: <challenge>" can do the same. Challenge, Answer, and Result
// are each word-wrapped by real pixel width into as many rows as they
// actually need (Display::wrapText(), Hardware Fix #4.4 issue A's fixed
// primitive), bounded by small per-section caps so the fixed Title/High
// Score rows and the other sections always keep some room on this
// otherwise very tight 135px-tall screen. Answer is prioritized for
// space (it's the row the user is actively keying), then Challenge, then
// the transient Result banner -- if a pathological case still doesn't
// fit, the LAST wrapped rows of Answer are kept visible (so the caret
// stays visible) and the FIRST wrapped rows of Challenge/Result are shown.
constexpr uint8_t kMaxChallengeRows = 4;
constexpr uint8_t kMaxAnswerRows = 3;
constexpr uint8_t kMaxResultRows = 3;

// Wraps `text` into at most `maxRows` rows (each already clipped to what
// Display::printLine() can draw in full -- see wrapText()'s own
// kPrintLineMaxChars guarantee), copying each row directly into
// outRows[i]. Returns the row count produced (always >= 1, even for an
// empty string, so a focused-but-empty section still has a row to put
// the ">" marker on).
uint8_t wrapIntoRows(const char* text, int16_t maxWidthPx, char outRows[][64], uint8_t maxRows) {
  uint16_t starts[8];
  uint16_t lens[8];
  uint8_t cap = (maxRows < 8) ? maxRows : 8;
  uint8_t n = Display::wrapText(text, maxWidthPx, starts, lens, cap);
  if (n == 0) {
    outRows[0][0] = '\0';
    return 1;
  }
  for (uint8_t i = 0; i < n; i++) {
    size_t l = lens[i];
    if (l >= 64) l = 63;
    memcpy(outRows[i], text + starts[i], l);
    outRows[i][l] = '\0';
  }
  return n;
}

// Hardware Fix #3's per-row independent diffing is preserved and extended:
// any change to how many rows Challenge/Answer/Result actually need
// (g_lastChallengeRowCount et al.) shifts every row below it, so that
// triggers one full mid-screen repaint (rare -- only on a new challenge,
// a reveal toggle, or an answer/result that crosses a wrap boundary);
// otherwise each row's own text is diffed independently, so typing a
// Morse answer still normally repaints only the one row it changed.
bool g_practiceNeedsFullRedraw = true;
char g_lastPracticeTitleLine[40] = {0};
char g_lastPracticeHiLine[24] = {0};
constexpr uint8_t kNoRowCount = 0xFF;
uint8_t g_lastChallengeRowCount = kNoRowCount;
uint8_t g_lastAnswerRowCount = kNoRowCount;
uint8_t g_lastResultRowCount = kNoRowCount;
char g_lastChallengeRowText[kMaxChallengeRows][64] = {{0}};
char g_lastAnswerRowText[kMaxAnswerRows][64] = {{0}};
char g_lastResultRowText[kMaxResultRows][64] = {{0}};
bool g_lastPracticeChallengeMarker = false;
bool g_lastPracticeAnswerMarker = false;

void screenPractice() {
  if (Menu::consumeJustEntered()) {
    g_streak = 0;
    startNewChallenge();
    g_practiceDirty = true;
    g_practiceWasShowingResult = false;
    g_practiceNeedsFullRedraw = true;
  }

  Input::update();
  InputEvent e;
  bool hadEvent = false;
  while (Input::popEvent(e)) {
    hadEvent = true;
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
  if (hadEvent) g_practiceDirty = true;

  // Word-boundary resolution is no longer a per-tick check (Hardware Fix
  // #4.1): it only happens at the next DOT_PRESS_START, in
  // handleAnswerEvent() above, so idling past 7 dit before Submit never
  // mutates the answer draft on its own.
  if (g_cursor == PracticeCursor::ANSWER && g_answerPatternLen > 0) {
    if (millis() - g_lastAnswerReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeAnswerChar();
      g_practiceDirty = true;
    }
  }

  // The result banner expires on a timer, not an input event, so its
  // visibility transition needs its own dirty trigger (Hardware Fix #1).
  bool showingResult = (g_resultText != nullptr && millis() < g_resultShownUntilMs);
  if (showingResult != g_practiceWasShowingResult) g_practiceDirty = true;
  g_practiceWasShowingResult = showingResult;

  Display::drawStatusBar();
  if (!g_practiceDirty) return;
  g_practiceDirty = false;

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t contentTop = Display::kStatusBarHeight + 2;
  int16_t contentHeight = Display::kScreenHeight - contentTop;
  uint16_t totalLines = (contentHeight > 0) ? static_cast<uint16_t>(contentHeight / lh) : 0;
  if (totalLines < 5) totalLines = 5;  // title + >=1 challenge + >=1 answer + >=0 result + high score

  int16_t markerW = static_cast<int16_t>(Display::textWidth(">") + 4);
  int16_t contentX = static_cast<int16_t>(2 + markerW);
  int16_t rowWidth = static_cast<int16_t>(Display::kScreenWidth - contentX);

  char titleLine[40];
  snprintf(titleLine, sizeof(titleLine), "Level %u  Streak %u", Settings::getPracticeLevel(), g_streak);

  // Challenge/answer content is kept separate from the ">" cursor marker
  // (Hardware Fix #3 corrective item 4): a plain ENCODER_ROTATE toggling
  // g_cursor between CHALLENGE and ANSWER must move only the marker, never
  // repaint the challenge text or the confirmed answer.
  char challengeContent[160];
  if (g_revealHeld) {
    strncpy(challengeContent, g_challengeText, sizeof(challengeContent) - 1);
    challengeContent[sizeof(challengeContent) - 1] = '\0';
  } else {
    buildRawMorse(g_challengeText, challengeContent, sizeof(challengeContent));
  }
  bool challengeMarker = (g_cursor == PracticeCursor::CHALLENGE);

  // Answer's confirmed text and its in-progress Morse suffix are combined
  // into one string here (Hardware Fix #4.4 issue E) -- unlike Enigma/Text
  // compose, g_answerText is always short enough (Level 3's longest
  // sentence is well under 24 chars) that re-wrapping the whole thing on
  // every symbol is cheap, so no separate confirmed/tail split is needed
  // to keep the hot path partial: per-row text diffing below already
  // limits the actual repaint to whichever row changed.
  char answerFull[64];
  {
    size_t plen = strlen(g_answerText);
    if (plen >= sizeof(answerFull)) plen = sizeof(answerFull) - 1;
    memcpy(answerFull, g_answerText, plen);
    answerFull[plen] = '\0';
    if (g_answerPatternLen > 0) {
      char suffix[Morse::kMaxPatternLength + 2];
      snprintf(suffix, sizeof(suffix), " %s", g_answerPattern);
      size_t suffixLen = strlen(suffix);
      if (plen + suffixLen >= sizeof(answerFull)) suffixLen = sizeof(answerFull) - 1 - plen;
      memcpy(answerFull + plen, suffix, suffixLen);
      answerFull[plen + suffixLen] = '\0';
    }
  }
  bool answerMarker = (g_cursor == PracticeCursor::ANSWER);

  char resultLine[64];
  if (showingResult) {
    snprintf(resultLine, sizeof(resultLine), "%s", g_resultText);
  } else {
    resultLine[0] = '\0';
  }

  char hiLine[24];
  snprintf(hiLine, sizeof(hiLine), "High: %u", g_highScore[Settings::getPracticeLevel() - 1]);

  char challengeRowsFull[kMaxChallengeRows][64];
  uint8_t challengeRowCountFull = wrapIntoRows(challengeContent, rowWidth, challengeRowsFull, kMaxChallengeRows);
  char answerRowsFull[kMaxAnswerRows][64];
  uint8_t answerRowCountFull = wrapIntoRows(answerFull, rowWidth, answerRowsFull, kMaxAnswerRows);
  char resultRowsFull[kMaxResultRows][64];
  uint8_t resultRowCountFull = showingResult ? wrapIntoRows(resultLine, rowWidth, resultRowsFull, kMaxResultRows) : 0;

  // Fits Challenge/Answer/Result into whatever's left after Title and High
  // Score's fixed single rows, prioritizing Answer (the row actively being
  // keyed -- "answer entry must keep the currently typed end visible"),
  // then Challenge, then the transient Result banner. On this 135px-tall
  // screen these three will usually all fit fully (Level 1/2 challenges
  // and any Letters-Only view never wrap at all); only a Level 3 sentence
  // shown as RAW Morse can be wide enough to need this budget to matter.
  int32_t budget = static_cast<int32_t>(totalLines) - 2;
  if (budget < 0) budget = 0;
  uint8_t answerShown = static_cast<uint8_t>((answerRowCountFull < budget) ? answerRowCountFull : budget);
  budget -= answerShown;
  uint8_t challengeShown = static_cast<uint8_t>((challengeRowCountFull < budget) ? challengeRowCountFull : budget);
  budget -= challengeShown;
  uint8_t resultShown = static_cast<uint8_t>((resultRowCountFull < budget) ? resultRowCountFull : budget);
  budget -= resultShown;

  // If Answer or Result had to be clamped, keep their LAST rows visible
  // (the active caret for Answer; the tail for Result) rather than losing
  // the most current text; Challenge keeps its FIRST rows (natural
  // top-down reading order) if it has to be clamped.
  uint8_t answerStart = static_cast<uint8_t>(answerRowCountFull - answerShown);
  uint8_t resultStart = static_cast<uint8_t>(resultRowCountFull - resultShown);

  int16_t titleY = contentTop;
  int16_t challengeY = static_cast<int16_t>(titleY + lh);
  int16_t answerY = static_cast<int16_t>(challengeY + challengeShown * lh);
  int16_t resultY = static_cast<int16_t>(answerY + answerShown * lh);
  int16_t hiY = static_cast<int16_t>(resultY + resultShown * lh);

  bool firstDraw = g_practiceNeedsFullRedraw;
  // Any change to how many rows Challenge/Answer/Result actually need
  // shifts every row below it, so that forces one full mid-screen repaint
  // (Hardware Fix #4.4 issue E) -- rare: only a new challenge, a reveal
  // toggle, or text crossing a wrap boundary triggers it.
  bool layoutChanged = firstDraw || challengeShown != g_lastChallengeRowCount ||
                       answerShown != g_lastAnswerRowCount || resultShown != g_lastResultRowCount;

  if (layoutChanged) {
    Display::clearContentArea();
    if (titleLine[0] != '\0') Display::printLine(2, titleY, titleLine);
    for (uint8_t i = 0; i < challengeShown; i++) {
      int16_t y = static_cast<int16_t>(challengeY + i * lh);
      if (i == 0 && challengeMarker) Display::printLine(2, y, ">");
      Display::printLine(contentX, y, challengeRowsFull[i]);
    }
    for (uint8_t i = 0; i < answerShown; i++) {
      int16_t y = static_cast<int16_t>(answerY + i * lh);
      if (i == 0 && answerMarker) Display::printLine(2, y, ">");
      Display::printLine(contentX, y, answerRowsFull[answerStart + i]);
    }
    for (uint8_t i = 0; i < resultShown; i++) {
      int16_t y = static_cast<int16_t>(resultY + i * lh);
      Display::printLine(2, y, resultRowsFull[resultStart + i]);
    }
    if (hiLine[0] != '\0') Display::printLine(2, hiY, hiLine);
    g_practiceNeedsFullRedraw = false;
  } else {
    if (strcmp(titleLine, g_lastPracticeTitleLine) != 0) {
      int16_t oldW = Display::textWidth(g_lastPracticeTitleLine);
      int16_t newW = Display::textWidth(titleLine);
      int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
      int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - 2);
      if (eraseW > maxW) eraseW = maxW;
      Display::tft().fillRect(2, titleY, eraseW, lh, ST77XX_BLACK);
      if (titleLine[0] != '\0') Display::printLine(2, titleY, titleLine);
    }
    // Stable layout: each row's own text is diffed independently, so
    // typing a Morse answer symbol -- or Level 1/2's common case where
    // nothing here ever wraps -- normally repaints only the one row that
    // actually changed (Hardware Fix #3's original intent, preserved).
    for (uint8_t i = 0; i < challengeShown; i++) {
      int16_t y = static_cast<int16_t>(challengeY + i * lh);
      if (strcmp(challengeRowsFull[i], g_lastChallengeRowText[i]) != 0) {
        Display::tft().fillRect(0, y, Display::kScreenWidth, lh, ST77XX_BLACK);
        if (i == 0 && challengeMarker) Display::printLine(2, y, ">");
        Display::printLine(contentX, y, challengeRowsFull[i]);
      }
    }
    if (challengeMarker != g_lastPracticeChallengeMarker) {
      Display::tft().fillRect(2, challengeY, markerW, lh, ST77XX_BLACK);
      if (challengeMarker) Display::printLine(2, challengeY, ">");
    }
    for (uint8_t i = 0; i < answerShown; i++) {
      int16_t y = static_cast<int16_t>(answerY + i * lh);
      const char* text = answerRowsFull[answerStart + i];
      if (strcmp(text, g_lastAnswerRowText[i]) != 0) {
        Display::tft().fillRect(0, y, Display::kScreenWidth, lh, ST77XX_BLACK);
        if (i == 0 && answerMarker) Display::printLine(2, y, ">");
        Display::printLine(contentX, y, text);
      }
    }
    if (answerMarker != g_lastPracticeAnswerMarker) {
      Display::tft().fillRect(2, answerY, markerW, lh, ST77XX_BLACK);
      if (answerMarker) Display::printLine(2, answerY, ">");
    }
    for (uint8_t i = 0; i < resultShown; i++) {
      int16_t y = static_cast<int16_t>(resultY + i * lh);
      const char* text = resultRowsFull[resultStart + i];
      if (strcmp(text, g_lastResultRowText[i]) != 0) {
        int16_t oldW = Display::textWidth(g_lastResultRowText[i]);
        int16_t newW = Display::textWidth(text);
        int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
        int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - 2);
        if (eraseW > maxW) eraseW = maxW;
        Display::tft().fillRect(2, y, eraseW, lh, ST77XX_BLACK);
        if (text[0] != '\0') Display::printLine(2, y, text);
      }
    }
    if (strcmp(hiLine, g_lastPracticeHiLine) != 0) {
      int16_t oldW = Display::textWidth(g_lastPracticeHiLine);
      int16_t newW = Display::textWidth(hiLine);
      int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
      int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - 2);
      if (eraseW > maxW) eraseW = maxW;
      Display::tft().fillRect(2, hiY, eraseW, lh, ST77XX_BLACK);
      if (hiLine[0] != '\0') Display::printLine(2, hiY, hiLine);
    }
  }

  strncpy(g_lastPracticeTitleLine, titleLine, sizeof(g_lastPracticeTitleLine) - 1);
  g_lastPracticeTitleLine[sizeof(g_lastPracticeTitleLine) - 1] = '\0';
  strncpy(g_lastPracticeHiLine, hiLine, sizeof(g_lastPracticeHiLine) - 1);
  g_lastPracticeHiLine[sizeof(g_lastPracticeHiLine) - 1] = '\0';
  for (uint8_t i = 0; i < challengeShown; i++) {
    strncpy(g_lastChallengeRowText[i], challengeRowsFull[i], sizeof(g_lastChallengeRowText[i]) - 1);
    g_lastChallengeRowText[i][sizeof(g_lastChallengeRowText[i]) - 1] = '\0';
  }
  for (uint8_t i = 0; i < answerShown; i++) {
    strncpy(g_lastAnswerRowText[i], answerRowsFull[answerStart + i], sizeof(g_lastAnswerRowText[i]) - 1);
    g_lastAnswerRowText[i][sizeof(g_lastAnswerRowText[i]) - 1] = '\0';
  }
  for (uint8_t i = 0; i < resultShown; i++) {
    strncpy(g_lastResultRowText[i], resultRowsFull[resultStart + i], sizeof(g_lastResultRowText[i]) - 1);
    g_lastResultRowText[i][sizeof(g_lastResultRowText[i]) - 1] = '\0';
  }
  g_lastPracticeChallengeMarker = challengeMarker;
  g_lastPracticeAnswerMarker = answerMarker;
  g_lastChallengeRowCount = challengeShown;
  g_lastAnswerRowCount = answerShown;
  g_lastResultRowCount = resultShown;
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
