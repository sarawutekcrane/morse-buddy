// HOST TEST (not production). Same-screen guess-history browsing on the
// Race Guess screen, compiled from the ACTUAL src/core/race.cpp with the
// ACTUAL src/core/menu.cpp, number_guessing.cpp, packet_codec.cpp,
// presence.cpp and race_protocol.cpp. Rounds, results and closures come from
// a peer coordinator that is itself an ACTUAL RaceProtocol::Engine, through
// race_fixture.h's broker model (ported from the pre-room-protocol suite:
// same cases RB1-RB12; packet injections replaced by the peer's real actions).
#include "stubs.h"
#include "core/menu.cpp"
#include "core/race.cpp"
#include <cstdio>
#include <string>
#include "race_fixture.h"

static int g_fail = 0, g_pass = 0;
static std::string g_test;
#define CHECK(c, ...)                                                        \
  do {                                                                       \
    if (c) { g_pass++; } else {                                              \
      g_fail++; printf("FAIL [%s] line %d: ", g_test.c_str(), __LINE__);     \
      printf(__VA_ARGS__); printf("\n");                                     \
    }                                                                        \
  } while (0)

void Fx::deliverToDevice(const Fx::Msg& m) {
  if (m.kind == RaceProtocol::kKindState) handleRaceStatePacket(m.group.c_str(), m.topic.c_str(), m.payload.data(), m.payload.size());
  else handleRaceRequestPacket(m.group.c_str(), m.topic.c_str(), m.payload.data(), m.payload.size());
}
void T_initServices();

// Test-only reboot of the race service: the firmware never frees the engine
// (it lives for the device's lifetime); a device reboot starts with none.
static void rebootRaceService() {
  delete g_engine;
  g_engine = nullptr;
  raceServiceInit();
}
static void tick(int n = 1) {
  for (int i = 0; i < n; i++) {
    raceServiceTick(); Fx::pump();
    for (auto* p : Fx::peers) if (p->online) p->eng.tick();
    Fx::pump(); Menu::tick(); T::now += 5;
  }
}
static void press(InputEventType t, uint32_t at, uint32_t dur = 80) {
  if (static_cast<int32_t>(at - T::now) > 0) { printf("FIXTURE ERROR: future timestamp\n"); exit(3); }
  T::raw.push_back(InputEvent(t, 0, dur, at));
}
static void rot(int8_t v) { T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, v, 0, 0)); }
static void shortP() { press(InputEventType::ENCODER_SHORT, T::now); }
// CLAIM requests this device has sent (its only "publish" while guessing).
static int claims() {
  int n = 0;
  for (auto& p : T::pubs) if (p.topic.rfind("r2q/", 0) == 0 && p.data.size() > 7 && p.data[7] == RaceProtocol::OP_CLAIM) n++;
  return n;
}

static int guessY() { return Display::kScreenHeight - Display::lineHeight(); }
static int rowY(int fromBottom) { return guessY() - fromBottom * Display::lineHeight(); }
static int visibleRows(bool notice) {
  return (guessY() - (Display::kStatusBarHeight + 2 + (notice ? Display::lineHeight() : 0))) / Display::lineHeight();
}
static std::string histRow(uint16_t v, uint8_t a, uint8_t b) {
  char t[32]; snprintf(t, sizeof(t), "%u %u %u %u %uA%uB", v / 1000 % 10, v / 100 % 10, v / 10 % 10, v % 10, a, b);
  return t;
}
static bool markerAt(int y) { for (auto& d : T::screen) if (d.text == ">" && d.y == y && d.x == 2) return true; return false; }
static int markerCount() { int n = 0; for (auto& d : T::screen) if (d.text == ">") n++; return n; }
static bool labelLit() { return T::colorOf("Guess: ", guessY()) == ST77XX_YELLOW; }

static void roomStub() {}  // stands in for the Room under Guess

static std::unique_ptr<Fx::Peer> g_owner, g_other;
static const char* kOwner = "0FFEE0000001";
// A joined, active round with secret 9876 (the coordinator's actual random
// draw, scripted), on its Guess screen (actual enterGuessScreen()).
static void activeRound() {
  T::reset(); Fx::reset();
  Storage::race().kv.clear(); Storage::race().failWrite = nullptr;
  Menu::init(); g_stackSize = 1; Menu::consumeJustEntered();
  g_roundId[0] = '\0'; g_pendingAutoEnterGuess = false; clearRaceGuessHistory(); clearRoomSolvedNotice(); invalidateGuessOutcome();
  rebootRaceService();
  closeRaceShortWindow();
  strcpy(g_groupCode, "GROUP1");
  g_owner.reset(new Fx::Peer(kOwner)); Fx::peers.push_back(g_owner.get()); g_owner->start();
  char p[110];
  snprintf(p, sizeof(p), "MBP1|%s|Owner|ONLINE|0|%lu|255", kOwner, (unsigned long)T::unixTime);
  Presence::handleIncoming("GROUP1", p, static_cast<uint16_t>(strlen(p)));
  CHECK(g_owner->eng.createRoom("GROUP1"), "fixture: room"); tick(2);
  CHECK(g_engine->join("GROUP1", kOwner), "fixture: join"); tick(3);
  g_owner->script = {9, 8, 7, 6};
  CHECK(g_owner->eng.startRound(), "fixture: round"); tick(2);
  CHECK(isRoundActive() && memcmp(g_secret, "\x09\x08\x07\x06", 4) == 0, "fixture: active 9876");
  g_pendingAutoEnterGuess = false;
  Menu::pushScreen(roomStub);
  enterGuessScreen();
  tick(2);
}
// Real submits of wrong guesses (all 0A0B against 9876).
static const uint16_t kGuesses[] = {123, 124, 125, 132, 134, 135, 142, 143, 145, 152, 153};
static void submit(uint16_t v) {
  g_digitEntry.digits[0] = v / 1000 % 10; g_digitEntry.digits[1] = v / 100 % 10;
  g_digitEntry.digits[2] = v / 10 % 10; g_digitEntry.digits[3] = v % 10; g_digitEntry.count = 4;
  g_raceGuessDirty = true; tick();
  shortP(); tick();
}

static void rb() {
  const int vr = visibleRows(false);
  g_test = "RB1 cap 8 (10 submitted): newest..oldest RETAINED; the 2 dropped guesses are not reachable";
  activeRound();
  for (int i = 0; i < 10; i++) submit(kGuesses[i]);
  CHECK(g_raceHistoryCount == 8, "count %u", g_raceHistoryCount);
  int pubs = claims();
  rot(-1); tick();
  CHECK(labelLit() && markerCount() == 0, "front");
  rot(-1); tick();
  CHECK(markerAt(rowY(1)) && T::rowAt(rowY(1)) == "> " + histRow(152, 0, 0), "newest '%s'", T::rowAt(rowY(1)).c_str());
  for (int i = 0; i < 20; i++) rot(-1);
  tick();
  CHECK(g_raceBrowse.selected == 7 && T::rowAt(rowY(vr)) == "> " + histRow(125, 0, 0), "oldest retained: sel %d '%s'",
        g_raceBrowse.selected, T::rowAt(rowY(vr)).c_str());
  for (int i = 0; i < 9; i++) rot(+1);
  tick();
  CHECK(!g_raceBrowse.browsing && !labelLit() && g_digitEntry.count == 0 && T::rowAt(rowY(1)) == histRow(152, 0, 0),
        "returned");
  CHECK(claims() == pubs, "browsing published");

  g_test = "RB2 Short while browsing: no evaluation, no publish, no Exit prompt";
  activeRound();
  submit(123); submit(124);
  g_digitEntry.digits[0] = 9; g_digitEntry.digits[1] = 8; g_digitEntry.digits[2] = 7; g_digitEntry.digits[3] = 6;
  g_digitEntry.count = 4; g_raceGuessDirty = true; tick();  // the CORRECT guess, complete
  pubs = claims();
  rot(-1); rot(-1); tick();
  T::now += 20; shortP(); tick(2);
  CHECK(g_stack[g_stackSize - 1] == screenRaceGuess && isRoundActive() && claims() == pubs &&
            g_raceHistoryCount == 2 && !g_raceBrowse.browsing && g_digitEntry.count == 4,
        "acted: claims %d", claims() - pubs);
  g_test = "RB2b empty draft: Short while browsing does not open Exit";
  activeRound(); submit(123);
  rot(-1); tick(); shortP(); tick(2);
  CHECK(g_stack[g_stackSize - 1] == screenRaceGuess, "Exit prompt opened");
  g_test = "RB2c after returning, Short submits the complete correct guess as before (solve path intact)";
  activeRound(); submit(123);
  g_digitEntry.digits[0] = 9; g_digitEntry.digits[1] = 8; g_digitEntry.digits[2] = 7; g_digitEntry.digits[3] = 6;
  g_digitEntry.count = 4; g_raceGuessDirty = true; tick();
  rot(-1); tick(); rot(+1); tick();
  T::now += 20; shortP(); tick(2);
  CHECK(g_stack[g_stackSize - 1] == roomStub && claims() == 1,  // the claim may already be confirmed within this tick
        "solve: claims %d", claims());
  tick(3);
  CHECK(std::string(g_engine->view()->resultWinner) == Identity::deviceId() && g_engine->view()->phase == RaceProtocol::PHASE_LOBBY,
        "claim not confirmed");

  g_test = "RB3 a new round while browsing (the result was missed) ends browsing and clears history";
  activeRound();
  for (int i = 0; i < 6; i++) submit(kGuesses[i]);
  for (int i = 0; i < 1 + vr + 1; i++) rot(-1);  // one row past the full window
  tick();
  CHECK(g_raceBrowse.browsing && g_raceBrowse.scroll > 0, "precondition");
  {  // the owner solves (and so holds the start right) and starts the next round; this
     // device misses the result state and receives the new round directly
    Fx::dropIf = [](const Fx::Msg& m) { return m.from == kOwner && m.kind == RaceProtocol::kKindState; };
    CHECK(g_owner->eng.submitCorrectGuess(g_owner->eng.view()->secret), "owner solves"); Fx::pump();
    Fx::dropIf = nullptr;
    g_owner->script = {1, 2, 3, 4};
    CHECK(g_owner->eng.startRound(), "owner starts the next round");
  }
  tick(2);
  CHECK(memcmp(g_secret, "\x01\x02\x03\x04", 4) == 0 && isRoundActive() && !T::visible("Solved by Owner: 1234") &&
            !T::visible("Solved by Owner: 9876"),
        "new round not bound, or an outcome shown for the missed result");
  CHECK(!g_raceBrowse.browsing && g_raceBrowse.scroll == 0 && g_raceHistoryCount == 0 && markerCount() == 0 &&
            !labelLit(),
        "browse after new round: %d", g_raceBrowse.browsing);
  rot(-1); tick();
  CHECK(labelLit(), "browsing an empty new-round history reaches only the front");

  g_test = "RB4 the room closes (owner Leave) while browsing: 'Round ended', input frozen, browsing gone";
  activeRound();
  for (int i = 0; i < 3; i++) submit(kGuesses[i]);
  rot(-1); rot(-1); tick();
  CHECK(g_raceBrowse.browsing, "precondition");
  {
    // another coordinator's room closing in the same group: ignored
    g_other.reset(new Fx::Peer("0FFEE0000002")); Fx::peers.push_back(g_other.get()); g_other->start();
    CHECK(g_other->eng.createRoom("GROUP1"), "other room"); tick();
    CHECK(g_other->eng.leave(), "other close"); tick();
    CHECK(isRoundActive() && g_raceBrowse.browsing, "foreign closure acted");
    CHECK(g_owner->eng.leave(), "owner closes");
    Fx::pump();
    CHECK(g_engine->state() == RaceProtocol::MS_NONE, "closure not applied by the engine");
  }
  tick();
  CHECK(!g_raceBrowse.browsing && T::visible("Round ended") && markerCount() == 0, "%s", T::visibleJoined().c_str());
  rot(+1); rot(-1); rot(-1); tick();
  CHECK(!g_raceBrowse.browsing && T::visible("Round ended"), "rotation acted while frozen");

  g_test = "RB5 external solve while browsing: frozen result, rotation ignored, secret shown only after the round";
  activeRound();
  for (int i = 0; i < 3; i++) submit(kGuesses[i]);
  bool leakedDuring = false;
  for (auto& d : T::drawLog) if (d.text.find("9876") != std::string::npos || d.text == "4A0B") leakedDuring = true;
  rot(-1); rot(-1); tick();
  CHECK(!leakedDuring, "secret drawn during the active round");
  CHECK(g_owner->eng.submitCorrectGuess(g_owner->eng.view()->secret), "owner solves");
  tick();
  CHECK(!g_raceBrowse.browsing && T::visible("Solved by Owner: 9876") && markerCount() == 0, "%s",
        T::visibleJoined().c_str());
  rot(-1); rot(-1); tick();
  CHECK(!g_raceBrowse.browsing, "rotation acted while frozen");

  g_test = "RB6 notice appearing/expiring keeps the selected row visible";
  activeRound();
  for (int i = 0; i < 6; i++) submit(kGuesses[i]);
  for (int i = 0; i < 1 + vr; i++) rot(-1);  // the top row of the full window
  tick();
  CHECK(markerAt(rowY(vr)) && g_raceBrowse.scroll == 0, "precondition sel %d scroll %u", g_raceBrowse.selected,
        g_raceBrowse.scroll);
  setGuessNotice("Join not confirmed"); g_raceGuessDirty = true; tick();
  const int nr = visibleRows(true);
  CHECK(T::visible("Join not confirmed") && markerAt(rowY(nr)) && g_raceBrowse.scroll == 1 &&
            T::rowAt(rowY(nr)) == "> " + histRow(kGuesses[6 - vr], 0, 0),
        "with notice: %s", T::visibleJoined().c_str());
  T::now += 1600; tick(2);
  // Expiry gives the row back; the window moves minimally (scroll kept), the selection stays marked.
  CHECK(!T::visible("Join not confirmed") && g_raceBrowse.selected == vr - 1 && g_raceBrowse.scroll == 1 &&
            markerCount() == 1 && T::rowAt(rowY(vr - 1)) == "> " + histRow(kGuesses[6 - vr], 0, 0) &&
            T::rowAt(rowY(vr)) == histRow(kGuesses[6 - vr - 1], 0, 0),
        "after expiry: %s", T::visibleJoined().c_str());

  g_test = "RB7 the replay filter still runs first: a replayed Short while browsing is dropped, not a 'return'";
  activeRound(); submit(123);
  rot(-1); tick();
  uint32_t t0 = T::now; openRaceShortWindow(t0); finishRaceScreenTick();
  press(InputEventType::ENCODER_SHORT, t0); tick();
  CHECK(g_raceBrowse.browsing, "replayed Short reached the browse handler");
  T::now += 20; shortP(); tick();
  CHECK(!g_raceBrowse.browsing, "fresh Short did not return");

  g_test = "RB8 Back while browsing leaves Guess for the Room; re-entry starts in digit editing";
  activeRound(); submit(123); submit(124);
  rot(-1); rot(-1); tick();
  press(InputEventType::ENCODER_LONG, T::now); tick();
  CHECK(g_stack[g_stackSize - 1] == roomStub, "Back did not leave Guess");
  enterGuessScreen(); tick(2);
  CHECK(!g_raceBrowse.browsing && markerCount() == 0 && !labelLit() && g_raceHistoryCount == 2 &&
            T::rowAt(rowY(1)) == histRow(124, 0, 0),
        "%s", T::visibleJoined().c_str());

  g_test = "RB9 DOT pressed while browsing, released after returning: no confirm, no hold-delete";
  activeRound(); submit(123);
  g_digitEntry.digits[0] = 3; g_digitEntry.digits[1] = 5; g_digitEntry.count = 2; g_digitEntry.previewDigit = 0;
  g_raceGuessDirty = true; tick();
  rot(-1); tick();
  press(InputEventType::DOT_PRESS_START, T::now); tick();
  rot(+1); tick();
  T::now += 700; tick(2);
  press(InputEventType::DOT_RELEASE, T::now, 700); tick(2);
  CHECK(g_digitEntry.count == 2 && g_digitEntry.digits[1] == 5, "draft changed: %u", g_digitEntry.count);
  g_test = "RB10 two fresh Shorts in ONE batch while browsing, complete wrong draft: first returns, second submits once";
  activeRound(); submit(123);
  g_digitEntry.digits[0] = 0; g_digitEntry.digits[1] = 1; g_digitEntry.digits[2] = 2; g_digitEntry.digits[3] = 4;
  g_digitEntry.count = 4; g_raceGuessDirty = true; tick();
  rot(-1); rot(-1); tick();
  T::now += 30;
  {
    int pubs0 = claims();
    CHECK(g_raceBrowse.browsing && g_raceShortWindow == RaceShortWindow::CLOSED, "precondition");
    shortP(); T::now += 40; shortP();
    CHECK(T::raw.size() == 2, "precondition: both Shorts queued for one tick");
    tick();
    CHECK(!g_raceBrowse.browsing && g_raceHistoryCount == 2 && isRoundActive() &&
              claims() == pubs0 && g_stack[g_stackSize - 1] == screenRaceGuess && g_digitEntry.count == 0,
          "history %u", g_raceHistoryCount);
    tick(2);
    CHECK(g_raceHistoryCount == 2 && T::rowAt(rowY(1)) == histRow(124, 0, 0), "not exactly one submission: '%s'",
          T::rowAt(rowY(1)).c_str());
  }
  g_test = "RB11 same, CORRECT draft, after an earlier action's replay window ended: exactly one solve";
  activeRound(); submit(123);
  {
    uint32_t ws = T::now; openRaceShortWindow(ws); T::now += 10; finishRaceScreenTick();  // e.g. an earlier Start/Join
    T::now += 10;
    g_digitEntry.digits[0] = 9; g_digitEntry.digits[1] = 8; g_digitEntry.digits[2] = 7; g_digitEntry.digits[3] = 6;
    g_digitEntry.count = 4; g_raceGuessDirty = true; tick();
    rot(-1); rot(-1); tick();
    T::now += 30;
    int pubs0 = claims();
    CHECK(g_raceBrowse.browsing && g_raceShortWindow == RaceShortWindow::BOUNDED &&
              static_cast<int32_t>(T::now - g_raceShortWindowEndMs) > 0,
          "precondition: bounded window whose end precedes both Shorts");
    shortP(); T::now += 40; shortP();
    tick();
    int solved = claims() - pubs0;
    CHECK(solved == 1 && g_stack[g_stackSize - 1] == roomStub &&
              g_raceShortWindow != RaceShortWindow::CLOSED,
          "claims sent %d", solved);
  }
  g_test = "RB12 two fresh Shorts in ONE batch while browsing, incomplete draft: first returns, second opens Exit";
  activeRound(); submit(123);
  g_digitEntry.digits[0] = 3; g_digitEntry.digits[1] = 5; g_digitEntry.count = 2; g_digitEntry.previewDigit = 0;
  g_raceGuessDirty = true; tick();
  rot(-1); tick();
  T::now += 30;
  {
    int pubs0 = claims();
    CHECK(g_raceBrowse.browsing && g_raceShortWindow == RaceShortWindow::CLOSED, "precondition");
    shortP(); T::now += 40; shortP();
    tick();
    CHECK(!g_raceBrowse.browsing && g_stack[g_stackSize - 1] == Menu::confirmPromptScreen && g_raceHistoryCount == 1 &&
              claims() == pubs0 && g_digitEntry.count == 2,
          "stack top not Exit prompt (history %u)", g_raceHistoryCount);
  }
}

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);  // FAIL lines survive a later crash
  Identity::init();  // the actual identity.cpp: device id from the (fake) MAC
  T_initServices();
  rb();
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
