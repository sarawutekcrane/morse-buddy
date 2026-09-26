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
// Unaffected by Feature Fix #4.9 -- only how they gate a TEST's High Score
// eligibility (see startNewTest()/handleChallengeEvent() below) changed.
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
// Feature Fix #4.9 Part D8: per-level BEST COMPLETED 10-QUESTION TEST score
// (0..100), replacing the old per-question high score entirely. Deliberately
// NEW NVS keys -- the old "prHi1"/"prHi2"/"prHi3" (best single QUESTION
// score) had different semantics and are left untouched/ignored, never
// migrated or reused, per the spec's explicit instruction.
// =============================================================================
uint8_t g_testHighScore[3] = {0, 0, 0};

void loadTestHighScores() {
  Preferences& p = Storage::core();
  g_testHighScore[0] = p.isKey("prTstHi1") ? p.getUChar("prTstHi1") : 0;
  g_testHighScore[1] = p.isKey("prTstHi2") ? p.getUChar("prTstHi2") : 0;
  g_testHighScore[2] = p.isKey("prTstHi3") ? p.getUChar("prTstHi3") : 0;
}
void saveTestHighScore(uint8_t levelIdx) {
  const char* keys[3] = {"prTstHi1", "prTstHi2", "prTstHi3"};
  Storage::core().putUChar(keys[levelIdx], g_testHighScore[levelIdx]);
}

// =============================================================================
// Challenge (prompt) text + audio preview. Feature Fix #4.9 Part D1/D9:
// the challenge is now shown to the user as PLAINTEXT (the thing to
// translate INTO Morse from memory), never as Morse up front -- see
// drawQuestionOrFeedback() below, which no longer calls buildRawMorse() for
// the normal (non-Reveal-held) prompt view.
// =============================================================================
constexpr size_t kChallengeCap = 24;  // longest Level 3 sentence + NUL
char g_challengeText[kChallengeCap];

// Feature Fix #4.9 Part D14: single derived worst-case bound shared by
// every buffer that might ever hold a full raw-Morse rendering of
// kChallengeCap-1 characters -- every one of those characters could
// theoretically encode to the longest supported pattern
// (Morse::kMaxPatternLength) with a "/ " word-separator token after it, so
// this can never be exceeded regardless of which dataset entry is actually
// shown, without hardcoding an arbitrary guessed size.
constexpr size_t kRawMorseCap = (kChallengeCap - 1) * (Morse::kMaxPatternLength + 2) + 1;

void loadChallengeByIndex(uint8_t level, uint8_t datasetIndex, char* out, size_t outCap) {
  if (level == 1) {
    out[0] = kLevel1Charset[datasetIndex];
    out[1] = '\0';
  } else if (level == 2) {
    strncpy(out, kLevel2Words[datasetIndex], outCap - 1);
    out[outCap - 1] = '\0';
  } else {
    strncpy(out, kLevel3Sentences[datasetIndex], outCap - 1);
    out[outCap - 1] = '\0';
  }
}

// Same raw-Morse builder shape as Text/Enigma's (duplicated locally per
// established Phase 3 precedent -- each mode's compose/display helpers are
// private to its own file). Used for: (a) the Reveal-held prompt view
// (Part D10), and (b) Audio Preview's tone sequence below. NEVER used for
// the normal prompt view any more (Part D9).
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
// Feature Fix #4.9 Part D6: explicit test-session state machine, replacing
// the old streak/time-scored design entirely. QUESTION: user is keying an
// answer to the current (fixed) prompt. FEEDBACK: brief "Correct"/"Wrong"
// display, input ignored (Part D18). COMPLETE: stable 10-question summary
// (Part D21), input limited to ENCODER_SHORT (new test) / Back (leave).
// =============================================================================
enum class TestPhase : uint8_t { QUESTION, FEEDBACK, COMPLETE };
TestPhase g_phase = TestPhase::QUESTION;

constexpr uint8_t kQuestionsPerTest = 10;
constexpr uint32_t kFeedbackDurationMs = 1400;  // Part D18: ~1200-1500ms

uint8_t g_sessionLevel = 1;  // Part D3: frozen at test start, never changed mid-test
// Part D5: exactly 10 unique dataset indices, preselected once per test
// with esp_random() (a real ESP32 random source, not unseeded Arduino
// random()) via rejection sampling -- every dataset here has >=20 entries,
// comfortably more than kQuestionsPerTest, so this never loops unbounded.
uint8_t g_testQuestionIndices[kQuestionsPerTest];
uint8_t g_questionIndex = 0;  // 0..kQuestionsPerTest-1
uint8_t g_correctCount = 0;
uint8_t g_wrongCount = 0;
// Part D11/D12: starts true; cleared (and never re-set) the moment Audio
// Preview is enabled for the session, or the moment Reveal is ACTUALLY
// activated (not merely enabled) during the test.
bool g_testHighScoreEligible = true;
uint32_t g_feedbackUntilMs = 0;
bool g_lastAnswerCorrect = false;
uint8_t g_testScore = 0;      // set once, at test completion (Part D7)
bool g_newHighAchieved = false;

void selectUniqueQuestionIndices(uint8_t level, uint8_t* out, uint8_t count) {
  uint8_t poolSize = (level == 1) ? kLevel1CharsetLen : (level == 2) ? kLevel2WordCount : kLevel3SentenceCount;
  for (uint8_t i = 0; i < count; i++) {
    uint8_t candidate;
    bool duplicate;
    do {
      candidate = static_cast<uint8_t>(esp_random() % poolSize);
      duplicate = false;
      for (uint8_t j = 0; j < i; j++) {
        if (out[j] == candidate) {
          duplicate = true;
          break;
        }
      }
    } while (duplicate);
    out[i] = candidate;
  }
}

// =============================================================================
// Screen/answer-compose state
// =============================================================================
enum class PracticeCursor : uint8_t { CHALLENGE, ANSWER };
PracticeCursor g_cursor = PracticeCursor::ANSWER;
bool g_revealHeld = false;

// Decoded plaintext, kept ONLY for correctness comparison against
// g_challengeText (Part D14) -- never shown to the user directly any more.
char g_answerDecoded[kChallengeCap];
uint8_t g_answerDecodedLen = 0;

// Part D13/D14: the RAW Morse the user has actually keyed, for CONFIRMED
// (finalized) characters only -- this is what the user actually sees (see
// drawQuestionOrFeedback()'s answerContent). Built incrementally from the
// exact symbols entered (never a corrected/regenerated pattern), in the
// same "PAT PAT / PAT" shape buildRawMorse() itself produces, so an
// invalid entered pattern still displays exactly what was keyed instead of
// silently becoming something else.
char g_answerRaw[kRawMorseCap];
uint16_t g_answerRawLen = 0;

char g_answerPattern[Morse::kMaxPatternLength + 1];
uint8_t g_answerPatternLen = 0;
uint32_t g_lastAnswerReleaseMs = 0;
// Hardware Fix #4.7e: true from DOT_PRESS_START until the matching
// DOT_RELEASE -- see text_message.cpp's identical field for the full
// rationale (real-hardware "A" == ".-" producing "ET" because the idle
// finalizer could fire while the following DASH was still held). Driven
// purely by semantic InputEvents, never a raw GPIO read. Feature Fix #4.9
// Part D15: preserved byte-for-byte -- this file never touches input.cpp.
bool g_answerKeyHeld = false;
Morse::WordGapState g_answerWordGap;
// True while the pattern currently being keyed (g_answerPattern) is known
// to start a new word -- captured once, at that pattern's FIRST symbol
// (see captureAnswerWordBoundaryOnSymbolStart()), and left untouched by
// symbol 2/3/... of the same pattern. Consumed as both an ASCII space (in
// the decoded buffer) and a "/ " token (in the raw buffer), only by
// finalizeAnswerChar()'s NORMAL letter finalization; a delete prosign or a
// special-command clear discard it without ever writing either (Hardware
// Fix #4.2).
bool g_answerPatternStartsNewWord = false;

// Hardware Fix #4.9a: fixed-capacity rollback checkpoints, one per
// CONFIRMED (finalized) character, so the delete prosign can restore both
// the visible RAW answer (g_answerRaw) and the internal grading state
// (g_answerDecoded) together, to exactly the state that existed
// immediately before that character was committed. Since a word-boundary
// ASCII space (in g_answerDecoded) and its "/ " token (in g_answerRaw)
// are written atomically together with the character that follows them in
// finalizeAnswerChar(), restoring to the pre-commit checkpoint
// automatically undoes those too -- never leaving a dangling separator
// behind. No heap; capacity is kChallengeCap, the same bound
// g_answerDecoded itself uses, since a legitimate answer can never have
// more confirmed characters than the challenge text has bytes.
struct AnswerCheckpoint {
  uint8_t decodedLen;
  uint16_t rawLen;
};
AnswerCheckpoint g_answerCheckpoints[kChallengeCap];
uint8_t g_answerCheckpointCount = 0;

// Dirty-gated redraw flag (Hardware Fix #1's original pattern) -- declared
// here, before startNewTest()/evaluateAnswer()/finishTest()/
// advanceAfterFeedback() below, all of which set it directly on any state
// transition that needs a redraw.
bool g_practiceDirty = true;

void resetAnswerCompose() {
  g_answerDecodedLen = 0;
  g_answerDecoded[0] = '\0';
  g_answerRawLen = 0;
  g_answerRaw[0] = '\0';
  g_answerPatternLen = 0;
  g_answerPattern[0] = '\0';
  g_answerPatternStartsNewWord = false;
  Morse::cancelWordGap(&g_answerWordGap);
  // Hardware Fix #4.9a: no confirmed characters survive into a new
  // question/test, so no stale checkpoint can ever be popped against the
  // wrong answer.
  g_answerCheckpointCount = 0;
  // Hardware Fix #4.7e: reset held state here too -- resetAnswerCompose()
  // runs both on special-command hold and at the start of every new
  // question, so no stale true can ever survive into a new question/test.
  g_answerKeyHeld = false;
}

// Called on every DOT_PRESS_START, before the new symbol is accepted into
// the pattern buffer (Hardware Fix #4.2). Only captures whether the
// pattern now starting is a new word -- and only at that pattern's FIRST
// symbol (g_answerPatternLen == 0); symbol 2/3/... of a multi-symbol
// character (e.g. W = .--) must never re-resolve or overwrite this flag.
// Never mutates the answer draft itself -- see finalizeAnswerChar(). Level
// 3 sentences need real word boundaries (e.g. "THE SUN IS HOT" keyed as
// four separate words, not one run-on), but a boundary must only ever be
// committed at normal letter finalization -- never merely because the user
// paused before Submit (which would otherwise wrongly fail the exact
// strcmp() against g_challengeText), and never for a delete prosign.
//
// Hardware Fix #4.7d: takes the physical DOT_PRESS_START event timestamp
// (pressMs) instead of reading millis() itself -- same invariant as
// text_message.cpp/enigma.cpp's identical helpers.
void captureAnswerWordBoundaryOnSymbolStart(uint32_t pressMs) {
  if (g_answerPatternLen != 0) return;
  g_answerPatternStartsNewWord =
      Morse::consumeWordBoundaryOnSymbolStart(&g_answerWordGap, Settings::getWpm(), pressMs);
}

void loadCurrentQuestion() {
  loadChallengeByIndex(g_sessionLevel, g_testQuestionIndices[g_questionIndex], g_challengeText, sizeof(g_challengeText));
  resetAnswerCompose();
  // Hardware Fix #4.3 issue B: default focus to ANSWER on every new
  // question so the user can start keying Morse immediately without first
  // having to rotate the encoder away from CHALLENGE. CHALLENGE remains
  // reachable by rotating there (for Audio Preview / Reveal Answer).
  g_cursor = PracticeCursor::ANSWER;
  g_revealHeld = false;
  if (g_audioPreview) playChallengeAudio();
}

bool g_practiceNeedsFullRedraw = true;

// Feature Fix #4.9 Part D2/D3/D5/D23/D24: a fresh, independent 10-question
// test, discarding anything in progress -- called on every Practice entry
// (Menu::consumeJustEntered(), never resuming) and every "Again" from the
// COMPLETE screen (same session level, new randomized question set,
// counters reset, persistent per-level Test High Score preserved).
void startNewTest() {
  g_sessionLevel = Settings::getPracticeLevel();  // frozen for the whole test (Part D3)
  selectUniqueQuestionIndices(g_sessionLevel, g_testQuestionIndices, kQuestionsPerTest);
  g_questionIndex = 0;
  g_correctCount = 0;
  g_wrongCount = 0;
  // Part D11: Audio Preview being enabled for the session disqualifies High
  // Score from the very start (every question's answer would be played
  // audibly). Part D12: Reveal only disqualifies once ACTUALLY activated,
  // handled separately in handleChallengeEvent() below.
  g_testHighScoreEligible = !g_audioPreview;
  g_testScore = 0;
  g_newHighAchieved = false;
  g_phase = TestPhase::QUESTION;
  loadCurrentQuestion();
  g_practiceNeedsFullRedraw = true;
  g_practiceDirty = true;
}

void finalizeAnswerChar() {
  char c = Morse::decodePattern(g_answerPattern);
  // NORMAL character finalization is the only place a word boundary is
  // ever actually committed (Hardware Fix #4.2) -- and only as one atomic
  // write together with the character it separates, so a boundary is
  // never left dangling with no room for the letter that was supposed to
  // follow it.
  bool needsSpace = g_answerPatternStartsNewWord && g_answerDecodedLen > 0 && g_answerDecoded[g_answerDecodedLen - 1] != ' ';

  // Hardware Fix #4.9a: snapshot both buffers' lengths BEFORE this
  // character mutates either of them, so a later delete prosign can
  // restore this exact pre-commit state -- see the checkpoint push below.
  uint8_t preDecodedLen = g_answerDecodedLen;
  uint16_t preRawLen = g_answerRawLen;
  bool committed = false;

  size_t needed = needsSpace ? 2 : 1;
  if (g_answerDecodedLen + needed <= sizeof(g_answerDecoded) - 1) {
    if (needsSpace) g_answerDecoded[g_answerDecodedLen++] = ' ';
    g_answerDecoded[g_answerDecodedLen++] = c;
    g_answerDecoded[g_answerDecodedLen] = '\0';
    committed = true;
  } else if (g_answerDecodedLen < sizeof(g_answerDecoded) - 1) {
    g_answerDecoded[g_answerDecodedLen++] = c;
    g_answerDecoded[g_answerDecodedLen] = '\0';
    committed = true;
  }

  // Feature Fix #4.9 Part D13/D14: raw display buffer mirrors
  // buildRawMorse()'s own "pattern pattern / pattern" shape, but is built
  // incrementally from the EXACT symbols the user actually keyed
  // (g_answerPattern) -- never a corrected/regenerated pattern -- so an
  // invalid pattern still displays exactly what was entered rather than
  // being silently replaced with something valid.
  size_t patLen = strlen(g_answerPattern);
  size_t extra = (g_answerRawLen > 0 ? 1 : 0) + (needsSpace ? 2 : 0) + patLen;
  if (g_answerRawLen + extra < sizeof(g_answerRaw)) {
    if (g_answerRawLen > 0) g_answerRaw[g_answerRawLen++] = ' ';
    if (needsSpace) {
      g_answerRaw[g_answerRawLen++] = '/';
      g_answerRaw[g_answerRawLen++] = ' ';
    }
    memcpy(g_answerRaw + g_answerRawLen, g_answerPattern, patLen);
    g_answerRawLen = static_cast<uint16_t>(g_answerRawLen + patLen);
    g_answerRaw[g_answerRawLen] = '\0';
  }

  // Hardware Fix #4.9a: only a character that actually landed in
  // g_answerDecoded needs a rollback point -- push the PRE-commit lengths
  // for both buffers as one checkpoint, so a later delete prosign can
  // restore both together (see handleAnswerEvent()'s delete branch).
  if (committed && g_answerCheckpointCount < kChallengeCap) {
    g_answerCheckpoints[g_answerCheckpointCount].decodedLen = preDecodedLen;
    g_answerCheckpoints[g_answerCheckpointCount].rawLen = preRawLen;
    g_answerCheckpointCount++;
  }

  g_answerPatternStartsNewWord = false;
  g_answerPatternLen = 0;
  g_answerPattern[0] = '\0';
  Morse::armWordGap(&g_answerWordGap, g_lastAnswerReleaseMs);
}

void finishTest() {
  g_phase = TestPhase::COMPLETE;
  uint8_t levelIdx = static_cast<uint8_t>(g_sessionLevel - 1);
  // Part D7: accuracy-only scoring -- correct=10pts, wrong=0pts, no time
  // bonus. Range 0..100.
  g_testScore = static_cast<uint8_t>(g_correctCount * 10);
  g_newHighAchieved = false;
  // Part D8/D12: only an eligible (unassisted) test can ever raise the
  // persistent per-level Test High Score.
  if (g_testHighScoreEligible && g_testScore > g_testHighScore[levelIdx]) {
    g_testHighScore[levelIdx] = g_testScore;
    saveTestHighScore(levelIdx);
    g_newHighAchieved = true;
  }
  g_practiceNeedsFullRedraw = true;
  g_practiceDirty = true;
}

void advanceAfterFeedback() {
  if (static_cast<uint8_t>(g_questionIndex + 1) >= kQuestionsPerTest) {
    finishTest();
  } else {
    g_questionIndex++;
    loadCurrentQuestion();
    g_phase = TestPhase::QUESTION;
  }
  g_practiceDirty = true;
}

// Feature Fix #4.9 Part D16: mirrors Hardware Fix #4.8b's Safe Send/Submit
// invariant (Parts C10-C12) -- never evaluate a half-keyed symbol. While
// DOT/DASH is still physically held, this Submit click is simply ignored
// (no force-finalize, no evaluation); otherwise any pending final pattern
// is finalized BEFORE the answer is evaluated, so a correctly-keyed final
// symbol followed immediately by Encoder Submit can never be wrongly
// scored Wrong just because the idle finalizer hadn't run yet.
void evaluateAnswer() {
  if (g_answerKeyHeld) return;
  if (g_answerPatternLen > 0) finalizeAnswerChar();

  bool correct = strcmp(g_answerDecoded, g_challengeText) == 0;
  if (correct) {
    g_correctCount++;
  } else {
    g_wrongCount++;
  }
  g_lastAnswerCorrect = correct;
  g_phase = TestPhase::FEEDBACK;
  g_feedbackUntilMs = millis() + kFeedbackDurationMs;
  g_practiceDirty = true;
}

void handleAnswerEvent(const InputEvent& e) {
  if (e.type == InputEventType::DOT_PRESS_START) {
    // Hardware Fix #4.7d: use the physical accepted press time, not
    // whenever this event happens to be processed -- same catch-up
    // finalization invariant as text_message.cpp's identical handler.
    // Order matters: finalizeAnswerChar() arms the word-gap timer from the
    // previous letter's real release time, which this new pressMs must be
    // compared against, so catch-up finalization must happen BEFORE
    // capturing the word boundary for the newly starting letter.
    uint32_t pressMs = e.eventMs != 0 ? e.eventMs : millis();
    if (g_answerPatternLen > 0 && (pressMs - g_lastAnswerReleaseMs) >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeAnswerChar();
    }
    captureAnswerWordBoundaryOnSymbolStart(pressMs);
    // Hardware Fix #4.7e: mark the key held only after the catch-up
    // finalize/word-boundary-capture above have used the pre-press state.
    g_answerKeyHeld = true;
  } else if (e.type == InputEventType::DOT_RELEASE) {
    // Hardware Fix #4.7e: clear held state before any early-return path
    // below, so it can never remain stuck true after a real release.
    g_answerKeyHeld = false;
    if (e.durationMs >= Morse::kSpecialCommandMs) {
      resetAnswerCompose();
      return;
    }
    Morse::SymbolClass sc = Morse::classifyPress(e.durationMs, Settings::getWpm());
    if (g_answerPatternLen < Morse::kMaxPatternLength) {
      g_answerPattern[g_answerPatternLen++] = (sc == Morse::SymbolClass::DOT) ? '.' : '-';
      g_answerPattern[g_answerPatternLen] = '\0';
    }
    // Hardware Fix #4.7d: store the physical accepted release time, not
    // processing-time millis().
    g_lastAnswerReleaseMs = e.eventMs != 0 ? e.eventMs : millis();
    if (Morse::isDeletePattern(g_answerPattern)) {
      // Hardware Fix #4.9a: roll back to the checkpoint saved immediately
      // before the last CONFIRMED character was committed, restoring BOTH
      // g_answerDecoded (grading state) and g_answerRaw (the visible
      // answer) together -- a delete prosign must never leave the two
      // disagreeing. Because a word-boundary ASCII space and its "/ "
      // raw token are written atomically together with the character that
      // follows them in finalizeAnswerChar(), restoring to the pre-commit
      // checkpoint automatically undoes those too, never leaving a
      // dangling separator behind. If nothing has been confirmed yet, this
      // is a safe no-op -- no underflow. The delete prosign itself was
      // never committed to g_answerRaw in the first place (it only ever
      // lived in g_answerPattern up to this point), so there is nothing of
      // its own to strip back out of the raw buffer here.
      g_answerPatternStartsNewWord = false;
      if (g_answerCheckpointCount > 0) {
        g_answerCheckpointCount--;
        const AnswerCheckpoint& cp = g_answerCheckpoints[g_answerCheckpointCount];
        g_answerDecodedLen = cp.decodedLen;
        g_answerDecoded[g_answerDecodedLen] = '\0';
        g_answerRawLen = cp.rawLen;
        g_answerRaw[g_answerRawLen] = '\0';
      }
      g_answerPatternLen = 0;
      g_answerPattern[0] = '\0';
      Morse::cancelWordGap(&g_answerWordGap);
    }
  } else if (e.type == InputEventType::ENCODER_SHORT) {
    evaluateAnswer();
  }
}

void handleChallengeEvent(const InputEvent& e) {
  if (e.type == InputEventType::DOT_PRESS_START) {
    if (g_audioPreview) playChallengeAudio();
    if (g_revealAnswer) {
      g_revealHeld = true;
      // Feature Fix #4.9 Part D12: only ACTUAL activation of Reveal
      // invalidates High Score eligibility for this test -- the Reveal
      // SETTING merely being enabled is not enough on its own. Sticky for
      // the rest of the test; never re-set true once cleared.
      g_testHighScoreEligible = false;
    }
  } else if (e.type == InputEventType::DOT_RELEASE) {
    g_revealHeld = false;
  }
  // Encoder short: no effect on the Challenge cursor.
}

// =============================================================================
// Rendering (Feature Fix #4.9 Part D20). 240x135 TFT, priority order:
// 1. "Lx Qn/10" status line (always shown, 1 row)
// 2. "C:x W:y High:zz" counter line (always shown, 1 row)
// 3. plaintext Prompt (wrapped, Reveal-held view shows raw Morse instead)
// 4. raw Morse Answer (wrapped)
// 5. feedback ("Correct"/"Wrong", exactly 1 row, only during FEEDBACK)
// Long Level 3 prompts and raw Morse answers wrap via the existing
// Display::wrapText() infrastructure, never silently truncated -- on this
// tiny screen not every wrapped row may fit at once, so Prompt keeps its
// FIRST rows (natural top-down reading) and Answer keeps its LAST rows
// (so the active caret stays visible) when budget runs out, exactly like
// the previous Challenge/Answer priority scheme this replaces. Dirty-
// gated with per-row diffing, same architecture as before -- never a
// continuous full-screen repaint.
// =============================================================================
constexpr uint8_t kMaxPromptRows = 4;
constexpr uint8_t kMaxAnswerRows = 5;

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

char g_lastStatusLine[24] = {0};
char g_lastCounterLine[32] = {0};
constexpr uint8_t kNoRowCount = 0xFF;
uint8_t g_lastPromptShownRows = kNoRowCount;
uint8_t g_lastAnswerShownRows = kNoRowCount;
uint8_t g_lastFeedbackShownRows = kNoRowCount;
char g_lastPromptRowText[kMaxPromptRows][64] = {{0}};
char g_lastAnswerRowText[kMaxAnswerRows][64] = {{0}};
char g_lastFeedbackText[16] = {0};
bool g_lastPromptMarker = false;
bool g_lastAnswerMarker = false;

void drawQuestionOrFeedback() {
  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t contentTop = Display::kStatusBarHeight + 2;
  int16_t contentHeight = Display::kScreenHeight - contentTop;
  uint16_t totalLines = (contentHeight > 0) ? static_cast<uint16_t>(contentHeight / lh) : 0;
  if (totalLines < 4) totalLines = 4;  // status + counter + >=1 prompt + >=1 answer

  int16_t markerW = static_cast<int16_t>(Display::textWidth(">") + 4);
  int16_t contentX = static_cast<int16_t>(2 + markerW);
  int16_t rowWidth = static_cast<int16_t>(Display::kScreenWidth - contentX);

  char statusLine[24];
  snprintf(statusLine, sizeof(statusLine), "L%u Q%u/%u", static_cast<unsigned>(g_sessionLevel),
           static_cast<unsigned>(g_questionIndex + 1), static_cast<unsigned>(kQuestionsPerTest));

  char counterLine[32];
  snprintf(counterLine, sizeof(counterLine), "C:%u W:%u High:%u", static_cast<unsigned>(g_correctCount),
           static_cast<unsigned>(g_wrongCount), static_cast<unsigned>(g_testHighScore[g_sessionLevel - 1]));

  bool showingFeedback = (g_phase == TestPhase::FEEDBACK);
  char feedbackLine[16];
  if (showingFeedback) {
    snprintf(feedbackLine, sizeof(feedbackLine), "%s", g_lastAnswerCorrect ? "Correct" : "Wrong");
  } else {
    feedbackLine[0] = '\0';
  }

  // Part D9/D10: normal prompt view is PLAINTEXT; only while Reveal is
  // actually held does it show the raw Morse instead (inverted from the
  // old Morse-first design this replaces).
  char promptContent[kRawMorseCap];
  if (g_revealHeld) {
    buildRawMorse(g_challengeText, promptContent, sizeof(promptContent));
  } else {
    strncpy(promptContent, g_challengeText, sizeof(promptContent) - 1);
    promptContent[sizeof(promptContent) - 1] = '\0';
  }
  bool promptMarker = (g_cursor == PracticeCursor::CHALLENGE);

  // Part D13: the visible Answer is the raw Morse actually keyed --
  // confirmed characters (g_answerRaw) plus the in-progress pattern
  // currently being keyed, exactly as it's typed.
  char answerContent[kRawMorseCap + Morse::kMaxPatternLength + 2];
  {
    size_t rawLen = strlen(g_answerRaw);
    memcpy(answerContent, g_answerRaw, rawLen);
    size_t pos = rawLen;
    if (g_answerPatternLen > 0) {
      if (pos > 0) answerContent[pos++] = ' ';
      memcpy(answerContent + pos, g_answerPattern, g_answerPatternLen);
      pos += g_answerPatternLen;
    }
    answerContent[pos] = '\0';
  }
  bool answerMarker = (g_cursor == PracticeCursor::ANSWER);

  char promptRowsFull[kMaxPromptRows][64];
  uint8_t promptRowCountFull = wrapIntoRows(promptContent, rowWidth, promptRowsFull, kMaxPromptRows);
  char answerRowsFull[kMaxAnswerRows][64];
  uint8_t answerRowCountFull = wrapIntoRows(answerContent, rowWidth, answerRowsFull, kMaxAnswerRows);

  // Status + counter lines are always shown (priorities 1/2, non-
  // negotiable); Prompt/Answer are each guaranteed >=1 row, with any
  // leftover budget favoring whichever section is currently focused, then
  // the other -- same distribution scheme the previous Challenge/Answer
  // design used. Feedback (priority 5, exactly 0 or 1 row) is reserved
  // off the top when active, never competing for extra rows.
  int32_t budget = static_cast<int32_t>(totalLines) - 2 - 1 - 1 - (showingFeedback ? 1 : 0);
  if (budget < 0) budget = 0;
  uint8_t promptShown = 1;
  uint8_t answerShown = 1;
  uint8_t feedbackShown = showingFeedback ? 1 : 0;

  if (g_cursor == PracticeCursor::ANSWER) {
    uint8_t extraAnswer = static_cast<uint8_t>((answerRowCountFull - 1 < budget) ? answerRowCountFull - 1 : budget);
    answerShown = static_cast<uint8_t>(answerShown + extraAnswer);
    budget -= extraAnswer;
    uint8_t extraPrompt = static_cast<uint8_t>((promptRowCountFull - 1 < budget) ? promptRowCountFull - 1 : budget);
    promptShown = static_cast<uint8_t>(promptShown + extraPrompt);
    budget -= extraPrompt;
  } else {
    uint8_t extraPrompt = static_cast<uint8_t>((promptRowCountFull - 1 < budget) ? promptRowCountFull - 1 : budget);
    promptShown = static_cast<uint8_t>(promptShown + extraPrompt);
    budget -= extraPrompt;
    uint8_t extraAnswer = static_cast<uint8_t>((answerRowCountFull - 1 < budget) ? answerRowCountFull - 1 : budget);
    answerShown = static_cast<uint8_t>(answerShown + extraAnswer);
    budget -= extraAnswer;
  }

  // Prompt keeps its FIRST rows if clamped (natural top-down reading);
  // Answer keeps its LAST rows (so the active caret stays visible).
  uint8_t answerStart = static_cast<uint8_t>(answerRowCountFull - answerShown);

  int16_t statusY = contentTop;
  int16_t counterY = static_cast<int16_t>(statusY + lh);
  int16_t promptY = static_cast<int16_t>(counterY + lh);
  int16_t answerY = static_cast<int16_t>(promptY + promptShown * lh);
  int16_t feedbackY = static_cast<int16_t>(answerY + answerShown * lh);

  bool firstDraw = g_practiceNeedsFullRedraw;
  bool layoutChanged = firstDraw || promptShown != g_lastPromptShownRows || answerShown != g_lastAnswerShownRows ||
                       feedbackShown != g_lastFeedbackShownRows;

  if (layoutChanged) {
    Display::clearContentArea();
    Display::printLine(2, statusY, statusLine);
    Display::printLine(2, counterY, counterLine);
    for (uint8_t i = 0; i < promptShown; i++) {
      int16_t y = static_cast<int16_t>(promptY + i * lh);
      if (i == 0 && promptMarker) Display::printLine(2, y, ">");
      Display::printLine(contentX, y, promptRowsFull[i]);
    }
    for (uint8_t i = 0; i < answerShown; i++) {
      int16_t y = static_cast<int16_t>(answerY + i * lh);
      if (i == 0 && answerMarker) Display::printLine(2, y, ">");
      Display::printLine(contentX, y, answerRowsFull[answerStart + i]);
    }
    if (feedbackShown > 0) Display::printLine(2, feedbackY, feedbackLine);
    g_practiceNeedsFullRedraw = false;
  } else {
    if (strcmp(statusLine, g_lastStatusLine) != 0) {
      int16_t oldW = Display::textWidth(g_lastStatusLine);
      int16_t newW = Display::textWidth(statusLine);
      int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
      int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - 2);
      if (eraseW > maxW) eraseW = maxW;
      Display::tft().fillRect(2, statusY, eraseW, lh, ST77XX_BLACK);
      Display::printLine(2, statusY, statusLine);
    }
    if (strcmp(counterLine, g_lastCounterLine) != 0) {
      int16_t oldW = Display::textWidth(g_lastCounterLine);
      int16_t newW = Display::textWidth(counterLine);
      int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
      int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - 2);
      if (eraseW > maxW) eraseW = maxW;
      Display::tft().fillRect(2, counterY, eraseW, lh, ST77XX_BLACK);
      Display::printLine(2, counterY, counterLine);
    }
    for (uint8_t i = 0; i < promptShown; i++) {
      int16_t y = static_cast<int16_t>(promptY + i * lh);
      if (strcmp(promptRowsFull[i], g_lastPromptRowText[i]) != 0) {
        Display::tft().fillRect(0, y, Display::kScreenWidth, lh, ST77XX_BLACK);
        if (i == 0 && promptMarker) Display::printLine(2, y, ">");
        Display::printLine(contentX, y, promptRowsFull[i]);
      }
    }
    if (promptMarker != g_lastPromptMarker) {
      Display::tft().fillRect(2, promptY, markerW, lh, ST77XX_BLACK);
      if (promptMarker) Display::printLine(2, promptY, ">");
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
    if (answerMarker != g_lastAnswerMarker) {
      Display::tft().fillRect(2, answerY, markerW, lh, ST77XX_BLACK);
      if (answerMarker) Display::printLine(2, answerY, ">");
    }
    if (feedbackShown > 0 && strcmp(feedbackLine, g_lastFeedbackText) != 0) {
      int16_t oldW = Display::textWidth(g_lastFeedbackText);
      int16_t newW = Display::textWidth(feedbackLine);
      int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
      int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - 2);
      if (eraseW > maxW) eraseW = maxW;
      Display::tft().fillRect(2, feedbackY, eraseW, lh, ST77XX_BLACK);
      Display::printLine(2, feedbackY, feedbackLine);
    }
  }

  strncpy(g_lastStatusLine, statusLine, sizeof(g_lastStatusLine) - 1);
  g_lastStatusLine[sizeof(g_lastStatusLine) - 1] = '\0';
  strncpy(g_lastCounterLine, counterLine, sizeof(g_lastCounterLine) - 1);
  g_lastCounterLine[sizeof(g_lastCounterLine) - 1] = '\0';
  for (uint8_t i = 0; i < promptShown; i++) {
    strncpy(g_lastPromptRowText[i], promptRowsFull[i], sizeof(g_lastPromptRowText[i]) - 1);
    g_lastPromptRowText[i][sizeof(g_lastPromptRowText[i]) - 1] = '\0';
  }
  for (uint8_t i = 0; i < answerShown; i++) {
    strncpy(g_lastAnswerRowText[i], answerRowsFull[answerStart + i], sizeof(g_lastAnswerRowText[i]) - 1);
    g_lastAnswerRowText[i][sizeof(g_lastAnswerRowText[i]) - 1] = '\0';
  }
  strncpy(g_lastFeedbackText, feedbackLine, sizeof(g_lastFeedbackText) - 1);
  g_lastFeedbackText[sizeof(g_lastFeedbackText) - 1] = '\0';
  g_lastPromptMarker = promptMarker;
  g_lastAnswerMarker = answerMarker;
  g_lastPromptShownRows = promptShown;
  g_lastAnswerShownRows = answerShown;
  g_lastFeedbackShownRows = feedbackShown;
}

// Feature Fix #4.9 Part D21: stable completion summary -- drawn fresh
// every time (COMPLETE is static once shown; nothing changes until
// ENCODER_SHORT starts a new test or Back leaves Practice, both of which
// leave this screen entirely), so no per-row diffing is needed here. Never
// auto-dismisses.
void drawCompleteScreen() {
  Display::setFont(Display::Font::COMPACT);  // denser font -- guarantees room for every required line on 135px
  int16_t lh = Display::lineHeight();
  int16_t y = Display::kStatusBarHeight + 2;
  Display::clearContentArea();

  char line[32];
  snprintf(line, sizeof(line), "Level %u Complete", static_cast<unsigned>(g_sessionLevel));
  Display::printLine(2, y, line);
  y = static_cast<int16_t>(y + lh);

  snprintf(line, sizeof(line), "Score: %u/100", static_cast<unsigned>(g_testScore));
  Display::printLine(2, y, line);
  y = static_cast<int16_t>(y + lh);

  snprintf(line, sizeof(line), "Correct: %u  Wrong: %u", static_cast<unsigned>(g_correctCount),
           static_cast<unsigned>(g_wrongCount));
  Display::printLine(2, y, line);
  y = static_cast<int16_t>(y + lh);

  snprintf(line, sizeof(line), "High: %u", static_cast<unsigned>(g_testHighScore[g_sessionLevel - 1]));
  Display::printLine(2, y, line);
  y = static_cast<int16_t>(y + lh);

  if (!g_testHighScoreEligible) {
    // Part D12: assisted (Reveal actually used, or Audio Preview was on)
    // -- Correct/Wrong/Score above are still genuine, but this test could
    // never raise the persistent High Score.
    Display::printLine(2, y, "Assisted");
  } else if (g_newHighAchieved) {
    Display::printLine(2, y, "New High!");
  }
}

void screenPractice() {
  if (Menu::consumeJustEntered()) {
    // Feature Fix #4.9 Part D24: every entry starts a completely fresh
    // test -- never resumes an incomplete one.
    startNewTest();
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
    if (g_phase == TestPhase::COMPLETE) {
      // Part D22: ENCODER_SHORT starts a new test at the same level;
      // ENCODER_LONG/Back (handled above via Input::isBack()) leaves
      // Practice. Rotation/DOT presses have no effect here.
      if (e.type == InputEventType::ENCODER_SHORT) startNewTest();
      continue;
    }
    if (g_phase == TestPhase::FEEDBACK) {
      // Part D18: input is ignored entirely during feedback (Back is
      // still handled above, since it must always be able to leave).
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
  //
  // Hardware Fix #4.7e: also gated on !g_answerKeyHeld -- see
  // text_message.cpp's identical check for the full rationale (a held
  // DASH could otherwise age past letterGapMs() measured from the
  // PREVIOUS release and get the pending letter wrongly finalized before
  // the DASH's own release ever arrived).
  if (g_phase == TestPhase::QUESTION && g_cursor == PracticeCursor::ANSWER && !g_answerKeyHeld &&
      g_answerPatternLen > 0) {
    if (millis() - g_lastAnswerReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeAnswerChar();
      g_practiceDirty = true;
    }
  }

  // The feedback phase expires on a timer, not an input event -- checked
  // every tick regardless of hadEvent, same as the transient banner this
  // replaces used to be.
  if (g_phase == TestPhase::FEEDBACK && millis() >= g_feedbackUntilMs) {
    advanceAfterFeedback();
  }

  Display::drawStatusBar();
  if (!g_practiceDirty) return;
  g_practiceDirty = false;

  if (g_phase == TestPhase::COMPLETE) {
    drawCompleteScreen();
    return;
  }
  drawQuestionOrFeedback();
}

// Storage::init() runs from setup() after every global constructor has
// already run, so NVS reads must happen in an AppService.init callback
// (invoked by initRegisteredServices(), also from setup(), after Storage::
// init()) rather than directly in the Registrar constructor below.
void serviceInit() {
  loadPracticeSettings();
  loadTestHighScores();
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
