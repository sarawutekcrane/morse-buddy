// HOST TEST (not production). Same-screen guess-history browsing:
// the shared NumberGuessing helper and Play Solo's Guess screen, compiled
// from the ACTUAL src/core/number_guessing.cpp together with the ACTUAL
// src/core/menu.cpp (real navigation stack). Two-stage input as elsewhere.
#include "stubs.h"
#include "core/menu.cpp"
#include "core/number_guessing.cpp"
#include <cstdio>
#include <string>

static int g_fail = 0, g_pass = 0;
static std::string g_test;
#define CHECK(c, ...)                                                        \
  do {                                                                       \
    if (c) { g_pass++; } else {                                              \
      g_fail++; printf("FAIL [%s] line %d: ", g_test.c_str(), __LINE__);     \
      printf(__VA_ARGS__); printf("\n");                                     \
    }                                                                        \
  } while (0)

void T_initServices();
static void tick(int n = 1) { for (int i = 0; i < n; i++) { Menu::tick(); T::now += 5; } }
static void press(InputEventType t, uint32_t at, uint32_t dur = 80) {
  if (static_cast<int32_t>(at - T::now) > 0) { printf("FIXTURE ERROR: future timestamp\n"); exit(3); }
  T::raw.push_back(InputEvent(t, 0, dur, at));
}
static void rot(int8_t v) { T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, v, 0, 0)); }
static void shortP() { press(InputEventType::ENCODER_SHORT, T::now); }
static void dotPress() { press(InputEventType::DOT_PRESS_START, T::now); }
static void dotRelease(uint32_t dur = 80) { press(InputEventType::DOT_RELEASE, T::now, dur); }

static const int lh = 23;
static int guessY() { return Display::kScreenHeight - Display::lineHeight(); }
static int rowY(int fromBottom) { return guessY() - fromBottom * Display::lineHeight(); }  // 1 = just above Guess
static int visibleRows() { return (guessY() - (Display::kStatusBarHeight + 2)) / Display::lineHeight(); }
static std::string histRow(uint16_t v, uint8_t a, uint8_t b) {
  char t[32]; snprintf(t, sizeof(t), "%u %u %u %u %uA%uB", v / 1000 % 10, v / 100 % 10, v / 10 % 10, v % 10, a, b);
  return t;
}
static bool markerAt(int y) { for (auto& d : T::screen) if (d.text == ">" && d.y == y && d.x == 2) return true; return false; }
static int markerCount() { int n = 0; for (auto& d : T::screen) if (d.text == ">") n++; return n; }
static bool labelHighlighted() { return T::colorOf("Guess: ", guessY()) == ST77XX_YELLOW; }
static bool labelPlain() { return T::colorOf("Guess: ", guessY()) == 0xFFFF; }

// Solo puzzle with secret 9876 and `n` retained wrong attempts 1000+i (i=0 oldest).
static void soloWith(uint16_t n) {
  T::reset(); Menu::init(); g_stackSize = 1; Menu::consumeJustEntered();
  memset(&g_solo, 0, sizeof(g_solo));
  g_solo.hasPuzzle = true; g_solo.secret[0] = 9; g_solo.secret[1] = 8; g_solo.secret[2] = 7; g_solo.secret[3] = 6;
  for (uint16_t i = 0; i < n; i++) {
    g_solo.history[i] = GuessEntry{static_cast<uint16_t>(1000 + i), 0, 0};
  }
  g_solo.historyCount = n; g_solo.totalAttempts = n;
  Menu::pushScreen(screenSoloGuess); tick(2);
}
// Draft: two confirmed digits 3,5; preview at the lowest available digit (0).
static void draft35() {
  g_digitEntry.digits[0] = 3; g_digitEntry.digits[1] = 5; g_digitEntry.count = 2;
  g_digitEntry.previewDigit = 0; g_digitEntry.dotHeld = false; g_digitEntry.deleteFiredThisPress = false;
  g_soloGuessDirty = true; tick();
}

// =============================================================================
// Shared helper (actual NumberGuessing::handleHistoryBrowseEvent)
// =============================================================================
static void h_helper() {
  using namespace NumberGuessing;
  HistoryBrowseState b; DigitEntryState d;
  InputEvent dn(InputEventType::ENCODER_ROTATE, -1, 0, 0), up(InputEventType::ENCODER_ROTATE, +1, 0, 0);

  g_test = "H1 decreasing at the lowest available digit enters browsing; above it, digit selection is untouched";
  resetHistoryBrowse(&b); resetDigitEntry(&d);  // preview 0, no digits
  CHECK(handleHistoryBrowseEvent(&b, d, dn, 3) == BrowseResult::CONSUMED && b.browsing && b.selected == -1, "enter");
  resetHistoryBrowse(&b); d.previewDigit = 4;
  CHECK(handleHistoryBrowseEvent(&b, d, dn, 3) == BrowseResult::PASS && !b.browsing, "above lowest");

  g_test = "H2 lowest AVAILABLE digit when 0 is already used";
  resetHistoryBrowse(&b); resetDigitEntry(&d); d.digits[0] = 0; d.count = 1; d.previewDigit = 1;
  CHECK(handleHistoryBrowseEvent(&b, d, dn, 3) == BrowseResult::CONSUMED && b.browsing, "1 is lowest when 0 used");
  resetHistoryBrowse(&b); d.digits[0] = 0; d.digits[1] = 1; d.count = 2; d.previewDigit = 3;
  CHECK(handleHistoryBrowseEvent(&b, d, dn, 3) == BrowseResult::PASS, "3 is not lowest (2 available)");
  d.previewDigit = 2;
  CHECK(handleHistoryBrowseEvent(&b, d, dn, 3) == BrowseResult::CONSUMED && b.browsing, "2 is lowest");

  g_test = "H3 complete draft: decreasing enters browsing; increasing passes (unchanged)";
  resetHistoryBrowse(&b); resetDigitEntry(&d); d.count = 4; d.previewDigit = 7;
  CHECK(handleHistoryBrowseEvent(&b, d, up, 3) == BrowseResult::PASS && !b.browsing, "increase");
  CHECK(handleHistoryBrowseEvent(&b, d, dn, 3) == BrowseResult::CONSUMED && b.browsing, "decrease");

  g_test = "H4 DOT held at the boundary: leaving the digit cell is refused";
  resetHistoryBrowse(&b); resetDigitEntry(&d); d.dotHeld = true;
  CHECK(handleHistoryBrowseEvent(&b, d, dn, 3) == BrowseResult::CONSUMED && !b.browsing, "refused");

  g_test = "H5 rows: newest, older, clamp at oldest retained; back down to front, then digit cell";
  resetHistoryBrowse(&b); resetDigitEntry(&d);
  handleHistoryBrowseEvent(&b, d, dn, 3);
  for (int i = 0; i < 5; i++) handleHistoryBrowseEvent(&b, d, dn, 3);
  CHECK(b.selected == 2, "clamped at oldest (selected %d)", b.selected);
  handleHistoryBrowseEvent(&b, d, up, 3); handleHistoryBrowseEvent(&b, d, up, 3);
  CHECK(b.browsing && b.selected == 0, "newest");
  handleHistoryBrowseEvent(&b, d, up, 3);
  CHECK(b.browsing && b.selected == -1, "front");
  CHECK(handleHistoryBrowseEvent(&b, d, up, 3) == BrowseResult::RETURNED && !b.browsing, "returned");

  g_test = "H6 empty history: front only; further decreasing stays";
  resetHistoryBrowse(&b); resetDigitEntry(&d);
  handleHistoryBrowseEvent(&b, d, dn, 0); handleHistoryBrowseEvent(&b, d, dn, 0);
  CHECK(b.browsing && b.selected == -1, "front only");

  g_test = "H7 Short while browsing returns; Back always passes";
  CHECK(handleHistoryBrowseEvent(&b, d, InputEvent(InputEventType::ENCODER_LONG), 0) == BrowseResult::PASS && b.browsing,
        "Back");
  CHECK(handleHistoryBrowseEvent(&b, d, InputEvent(InputEventType::ENCODER_SHORT), 0) == BrowseResult::RETURNED &&
            !b.browsing,
        "Short");

  g_test = "H8 DOT pressed while browsing: press and its release swallowed even after returning";
  resetHistoryBrowse(&b); resetDigitEntry(&d);
  handleHistoryBrowseEvent(&b, d, dn, 2);
  CHECK(handleHistoryBrowseEvent(&b, d, InputEvent(InputEventType::DOT_PRESS_START), 2) == BrowseResult::CONSUMED, "press");
  handleHistoryBrowseEvent(&b, d, up, 2);  // back to digit editing
  CHECK(!b.browsing && handleHistoryBrowseEvent(&b, d, InputEvent(InputEventType::DOT_RELEASE, 0, 80, 1), 2) ==
                           BrowseResult::CONSUMED,
        "release swallowed");
  CHECK(handleHistoryBrowseEvent(&b, d, InputEvent(InputEventType::DOT_RELEASE, 0, 80, 2), 2) == BrowseResult::PASS,
        "a later normal release passes");

  g_test = "H9 window follows the selection";
  resetHistoryBrowse(&b); b.browsing = true; b.selected = 6;
  clampHistoryBrowse(&b, 10, 4);
  CHECK(b.scroll == 3, "scroll %u", b.scroll);
  b.selected = 1; clampHistoryBrowse(&b, 10, 4);
  CHECK(b.scroll == 1, "scroll %u", b.scroll);
  b.selected = 20; clampHistoryBrowse(&b, 10, 4);
  CHECK(b.selected == 9 && b.scroll == 6, "selected %d scroll %u", b.selected, b.scroll);
}

// =============================================================================
// Play Solo Guess screen (actual screenSoloGuess)
// =============================================================================
static void s_solo() {
  const int vr = visibleRows();
  g_test = "S1 browse into history on the Guess screen; label highlighted at the Guess-row front";
  soloWith(7); draft35();
  DigitEntryState before = g_digitEntry;
  rot(-1); tick();
  CHECK(labelHighlighted() && markerCount() == 0, "front: label colour %04x", T::colorOf("Guess: ", guessY()));
  rot(-1); tick();
  CHECK(markerAt(rowY(1)) && T::rowAt(rowY(1)) == "> " + histRow(1006, 0, 0) && labelPlain(),
        "newest selected: '%s'", T::rowAt(rowY(1)).c_str());
  g_test = "S2 older rows scroll the window; clamped at the oldest retained row";
  for (int i = 0; i < 10; i++) { rot(-1); tick(); }
  CHECK(markerAt(rowY(vr)) && T::rowAt(rowY(vr)) == "> " + histRow(1000, 0, 0), "oldest at top: '%s'",
        T::rowAt(rowY(vr)).c_str());
  CHECK(T::rowAt(rowY(1)) == histRow(static_cast<uint16_t>(1000 + vr - 1), 0, 0), "window bottom: '%s'",
        T::rowAt(rowY(1)).c_str());
  CHECK(T::rowAt(guessY()).rfind("Guess:", 0) == 0, "Guess row still shown");
  g_test = "S3 reverse returns through newer rows to the front, then to the same digit position and preview";
  for (int i = 0; i < 7; i++) { rot(+1); tick(); }
  CHECK(labelHighlighted() && T::rowAt(rowY(1)) == histRow(1006, 0, 0), "front, window back at newest");
  rot(+1); tick();
  CHECK(labelPlain() && !g_soloBrowse.browsing && memcmp(&before, &g_digitEntry, sizeof(before)) == 0 &&
            T::rowAt(guessY()) == "Guess:  3 5 0 _",
        "draft changed or not returned: '%s'", T::rowAt(guessY()).c_str());
  rot(+1); tick();
  CHECK(g_digitEntry.previewDigit == 1 && g_digitEntry.count == 2, "digit selection does not resume");

  g_test = "S4 Short while browsing: nothing submitted, no Exit prompt, digit editing resumes";
  soloWith(3); draft35();
  rot(-1); rot(-1); tick();
  shortP(); tick(2);
  CHECK(g_solo.historyCount == 3 && g_solo.totalAttempts == 3 && g_stack[g_stackSize - 1] == screenSoloGuess &&
            !g_soloBrowse.browsing && g_digitEntry.count == 2,
        "stack top %p", (void*)g_stack[g_stackSize - 1]);
  g_test = "S4b complete draft: Short while browsing does not submit";
  soloWith(3); g_digitEntry.digits[0] = 1; g_digitEntry.digits[1] = 2; g_digitEntry.digits[2] = 3;
  g_digitEntry.digits[3] = 4; g_digitEntry.count = 4; g_soloGuessDirty = true; tick();
  rot(-1); tick(); shortP(); tick(2);
  CHECK(g_solo.historyCount == 3 && g_digitEntry.count == 4, "submitted from browsing");
  shortP(); tick(2);
  CHECK(g_solo.historyCount == 4, "a later Short in digit editing still submits");

  g_test = "S5 DOT pressed while browsing, released after returning: no digit confirmed or deleted";
  soloWith(3); draft35();
  rot(-1); tick();
  dotPress(); tick();
  rot(+1); tick();
  T::now += 700; tick(3);  // well past the 500 ms hold-delete threshold
  dotRelease(700); tick(2);
  CHECK(g_digitEntry.count == 2 && g_digitEntry.digits[0] == 3 && g_digitEntry.digits[1] == 5 && !g_digitEntry.dotHeld,
        "draft changed: count %u", g_digitEntry.count);
  dotPress(); T::now += 60; dotRelease(60); tick(2);
  CHECK(g_digitEntry.count == 3, "a normal later DOT no longer confirms");

  g_test = "S6 DOT held at the boundary: no browsing; the confirm applies to the visible digit";
  soloWith(3); draft35();
  dotPress(); tick();
  rot(-1); tick();
  CHECK(!g_soloBrowse.browsing && g_digitEntry.previewDigit == 0, "entered browsing while DOT held");
  T::now += 60; dotRelease(60); tick();
  CHECK(g_digitEntry.count == 3 && g_digitEntry.digits[2] == 0, "confirm lost");

  g_test = "S7 queued detents and Short in one batch: ends in digit editing, nothing submitted";
  soloWith(5); draft35();
  rot(-1); rot(-1); rot(-1); rot(-1); shortP(); tick(2);
  CHECK(!g_soloBrowse.browsing && g_solo.historyCount == 5 && g_digitEntry.count == 2, "state");
  g_test = "S8 after a submit the newest result is shown at the bottom and browsing is off";
  soloWith(5); draft35();
  rot(-1); rot(-1); rot(-1); tick();
  rot(+1); rot(+1); rot(+1); tick();
  g_digitEntry.digits[2] = 1; g_digitEntry.digits[3] = 2; g_digitEntry.count = 4; g_soloGuessDirty = true; tick();
  shortP(); tick(2);
  CHECK(g_solo.historyCount == 6 && T::rowAt(rowY(1)) == histRow(3512, 0, 0) && markerCount() == 0,
        "newest row: '%s'", T::rowAt(rowY(1)).c_str());

  g_test = "S9 capacity: with 255 retained attempts the oldest retained (first) attempt is reachable, not beyond";
  soloWith(255); draft35();
  for (int i = 0; i < 300; i++) rot(-1);
  tick();
  CHECK(g_soloBrowse.selected == 254 && T::rowAt(rowY(vr)) == "> " + histRow(1000, 0, 0), "selected %d '%s'",
        g_soloBrowse.selected, T::rowAt(rowY(vr)).c_str());
  g_test = "S10 empty history: front only, then back to editing";
  soloWith(0); draft35();
  rot(-1); rot(-1); tick();
  CHECK(labelHighlighted() && markerCount() == 0, "front");
  rot(+1); tick();
  CHECK(labelPlain() && !g_soloBrowse.browsing, "returned");
  g_test = "S11 Back while browsing leaves the screen as before";
  soloWith(3); draft35(); rot(-1); tick();
  uint8_t depth = g_stackSize;
  press(InputEventType::ENCODER_LONG, T::now); tick();
  CHECK(g_stackSize == depth - 1, "Back did not exit");
  g_test = "S11b re-entering Solo Guess after leaving while browsing starts in digit editing";
  rot(-1); rot(-1); tick();  // (stack top is below Guess now; these go to that screen)
  memset(&g_soloBrowse, 0, sizeof(g_soloBrowse)); g_soloBrowse.browsing = true; g_soloBrowse.selected = 1;
  Menu::pushScreen(screenSoloGuess); tick(2);
  CHECK(!g_soloBrowse.browsing && markerCount() == 0 && !labelHighlighted(), "browse state survived re-entry");
  g_test = "S13 a DOT pressed AND released while browsing does not swallow the next normal DOT";
  soloWith(3); draft35();
  rot(-1); tick();
  dotPress(); T::now += 60; dotRelease(60); tick(2);
  rot(+1); tick();
  CHECK(!g_soloBrowse.browsing && g_digitEntry.count == 2, "browse DOT acted");
  dotPress(); T::now += 60; dotRelease(60); tick(2);
  CHECK(g_digitEntry.count == 3 && g_digitEntry.digits[2] == 0, "next normal DOT swallowed: count %u", g_digitEntry.count);
  g_test = "S14 two fresh Shorts in ONE batch while browsing, complete draft: first returns, second submits once";
  soloWith(3);
  g_digitEntry.digits[0] = 1; g_digitEntry.digits[1] = 2; g_digitEntry.digits[2] = 3; g_digitEntry.digits[3] = 4;
  g_digitEntry.count = 4; g_soloGuessDirty = true; tick();
  rot(-1); rot(-1); tick();  // browsing (newest row), entered in an earlier tick
  CHECK(g_soloBrowse.browsing && g_soloBrowse.selected == 0, "precondition: browsing");
  T::now += 30; shortP(); T::now += 40; shortP();
  CHECK(T::raw.size() == 2, "precondition: both Shorts queued for one tick");
  tick();  // one Menu::tick() -> one Input::update() drains both
  CHECK(!g_soloBrowse.browsing && g_solo.totalAttempts == 4 && g_solo.historyCount == 4 &&
            g_stack[g_stackSize - 1] == screenSoloGuess && g_digitEntry.count == 0,
        "attempts %u count %u", g_solo.totalAttempts, g_digitEntry.count);
  tick(2);
  CHECK(g_solo.totalAttempts == 4 && T::rowAt(rowY(1)) == histRow(1234, 0, 0), "not exactly one submission: %u '%s'",
        g_solo.totalAttempts, T::rowAt(rowY(1)).c_str());
  g_test = "S15 two fresh Shorts in ONE batch while browsing, incomplete draft: first returns, second opens Exit";
  soloWith(3); draft35();
  rot(-1); tick();  // browsing (Guess-row front)
  CHECK(g_soloBrowse.browsing && g_soloBrowse.selected == -1, "precondition: browsing");
  T::now += 30; shortP(); T::now += 40; shortP();
  tick();
  CHECK(!g_soloBrowse.browsing && g_stack[g_stackSize - 1] == Menu::confirmPromptScreen && g_solo.totalAttempts == 3 &&
            g_digitEntry.count == 2,
        "stack top is not the Exit prompt / state changed (attempts %u)", g_solo.totalAttempts);
  g_test = "S12 secret never drawn";
  bool leaked = false;
  for (auto& d : T::drawLog) if (d.text.find("9876") != std::string::npos || d.text == "4A0B") leaked = true;
  CHECK(!leaked, "secret drawn");
}

int main() {
  T_initServices();
  h_helper();
  s_solo();
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
