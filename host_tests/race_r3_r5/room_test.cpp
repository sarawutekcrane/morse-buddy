// HOST TEST (not production). The Race Room and Guess screens on the room
// protocol (design r8): R1 (participant list focus/scroll), R2 (Start Round
// enters Guess) and the R3-R5 UI (scores beside names, the confirmed Leave
// row, Back as navigation only, status rows, start right, discovery/Join,
// claims and outcomes). The device under test is the ACTUAL src/core/race.cpp
// + menu.cpp with its ACTUAL RaceProtocol engine (race_protocol.cpp), linked
// with the actual number_guessing, packet_codec and presence; peers are
// further ACTUAL RaceProtocol::Engine instances connected by race_fixture.h's
// broker model. Presence names enter through the real presence handler.
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

extern int g_invitePending;
void Fx::deliverToDevice(const Fx::Msg& m) {
  if (m.kind == RaceProtocol::kKindState) handleRaceStatePacket(m.group.c_str(), m.topic.c_str(), m.payload.data(), m.payload.size());
  else handleRaceRequestPacket(m.group.c_str(), m.topic.c_str(), m.payload.data(), m.payload.size());
}

// The no-throw allocation race.cpp uses for its engine: counted, and made to
// fail on request (replaces the global nothrow operator new for this binary;
// the throwing operator new it forwards to pairs with the default delete).
static bool g_failNothrowNew = false;
static int g_nothrowNews = 0;
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  g_nothrowNews++;
  if (g_failNothrowNew) return nullptr;
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void T_initServices();

// Test-only reboot of the race service: the firmware never frees the engine
// (it lives for the device's lifetime); a device reboot starts with none.
static void rebootRaceService() {
  delete g_engine;
  g_engine = nullptr;
  raceServiceInit();
}
// One main-loop iteration: background services (the race engine), the
// broker's deliveries, the peers' timers, then the active screen.
static void loopOnce() {
  raceServiceTick();
  Fx::pump();
  for (auto* p : Fx::peers) if (p->online) p->eng.tick();
  Fx::pump();
  Menu::tick();
  T::now += 5;
}
static void tick(int n = 1) { for (int i = 0; i < n; i++) loopOnce(); }
static void settleMs(uint32_t ms) { uint32_t end = T::now + ms; while (static_cast<int32_t>(T::now - end) < 0) { T::now += 95; loopOnce(); } }
static void press(InputEventType t, uint32_t at, uint32_t dur = 80) {
  if (static_cast<int32_t>(at - T::now) > 0) { printf("FIXTURE ERROR: future timestamp\n"); exit(3); }
  T::raw.push_back(InputEvent(t, 0, dur, at));
}
static void rot(int8_t v) { T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, v, 0, 0)); }
static void shortP() { press(InputEventType::ENCODER_SHORT, T::now); }
static void backP() { press(InputEventType::ENCODER_LONG, T::now); }
static ScreenHandlerFn top() { return g_stack[g_stackSize - 1]; }

static const int lh = 23;
static int titleY() { return Display::kStatusBarHeight + 2; }
static int actionY() { return titleY() + lh; }
static int statusY() { return titleY() + 2 * lh; }
static int rowsTopY() { return titleY() + 3 * lh; }
static int rowY(int r) { return rowsTopY() + r * lh; }
static int guessRowY() { return Display::kScreenHeight - Display::lineHeight(); }
static int fitRows() { return (Display::kScreenHeight - rowsTopY()) / lh; }
static int statePubs() { int n = 0; for (auto& p : T::pubs) if (p.topic.rfind("r2/", 0) == 0 && p.result) n++; return n; }
static const RaceProtocol::RoomState* devRoom() { return g_engine->view(); }

static void parentStub() {}  // the Training Game menu under Race

static void presenceName(const char* id, const char* name) {
  char p[110];
  snprintf(p, sizeof(p), "MBP1|%s|%s|ONLINE|0|%lu|255", id, name, (unsigned long)T::unixTime);
  Presence::handleIncoming("GROUP1", p, static_cast<uint16_t>(strlen(p)));
}

static const char* kPeerIds[] = {"0EE000000001", "0EE000000002", "0EE000000003", "0EE000000004",
                                 "0EE000000005", "0EE000000006"};
static const char* kPeerNames[] = {"Alice", "Bob", "Carol", "Dave", "Erin", "Frank"};
static std::vector<std::unique_ptr<Fx::Peer>> g_peers;
static Fx::Peer* peer(int i) { return g_peers[i].get(); }

// A fresh device (empty mb_race, engine booted) on its Room screen, with
// `n` named peers online.
static void freshDevice(int n) {
  T::reset(); Fx::reset(); g_peers.clear();
  Storage::race().kv.clear(); Storage::race().failWrite = nullptr;
  Menu::init(); g_stackSize = 1; Menu::consumeJustEntered();
  g_roundId[0] = '\0'; g_pendingAutoEnterGuess = false; clearRaceGuessHistory(); clearRoomSolvedNotice();
  invalidateGuessOutcome(); g_roomPttActive = false;
  rebootRaceService();
  leaveRaceMode();
  closeRaceShortWindow();
  strcpy(g_groupCode, "GROUP1");
  for (int i = 0; i < n; i++) {
    g_peers.emplace_back(new Fx::Peer(kPeerIds[i]));
    Fx::peers.push_back(g_peers.back().get());
    g_peers.back()->start();
    presenceName(kPeerIds[i], kPeerNames[i]);
  }
  Menu::pushScreen(parentStub);
  Menu::pushScreen(screenRaceRoom);
  tick();
}
static void peersJoin(int from, int to) {
  for (int i = from; i < to; i++) {
    CHECK(peer(i)->eng.join("GROUP1", Identity::deviceId()), "fixture: peer %d join", i);
    tick(2);
  }
}
// The device creates a room (real Invite press) and `extra` peers join it.
static void ownerLobby(int extra) {
  freshDevice(extra);
  shortP(); tick(2);  // Invite to Play -> room created
  CHECK(g_engine->isCoordinator() && devRoom() && devRoom()->version == 1, "fixture: create");
  peersJoin(0, extra);
  CHECK(devRoom() && RaceProtocol::activeCount(*devRoom()) == extra + 1, "fixture: %d joined", extra);
  T::now += 2000; tick(2);  // past the Invite replay window and any notice
}
// Peer 0 coordinates a room; the device joins it through its discovered row.
static void memberOf(int peersN) {
  freshDevice(peersN);
  CHECK(peer(0)->eng.createRoom("GROUP1"), "fixture: peer room"); tick(2);
  for (int i = 1; i < peersN; i++) { CHECK(peer(i)->eng.join("GROUP1", kPeerIds[0]), "fixture: peer join"); tick(2); }
  CHECK(T::rowAt(rowY(0)) == "Join Alice (" + std::to_string(peersN) + ")", "discovered row '%s'", T::rowAt(rowY(0)).c_str());
  rot(+1); tick(); shortP(); tick(4);
  CHECK(g_engine->state() == RaceProtocol::MS_MEMBER && devRoom(), "fixture: device joined");
  T::now += 2000; tick(2);
}

// =============================================================================
static void r1() {
  g_test = "R1-1 rows fit the area below the status row; scores beside names; title shows the range";
  ownerLobby(4);  // self + 4 = 5 participants (+ Leave row)
  const int rows = fitRows();
  CHECK(rows == 2, "rows %d", rows);
  CHECK(T::rowAt(titleY()) == "Race Room  1-2/5" && T::rowAt(actionY()) == "> Start Round", "title '%s' action '%s'",
        T::rowAt(titleY()).c_str(), T::rowAt(actionY()).c_str());
  CHECK(T::rowAt(rowY(0)) == "DEADBEEFAAAA (You) 0" && T::rowAt(rowY(1)) == "Alice 0" && T::rowAt(rowY(2)).empty(),
        "rows '%s' / '%s' / '%s'", T::rowAt(rowY(0)).c_str(), T::rowAt(rowY(1)).c_str(), T::rowAt(rowY(2)).c_str());
  bool scoreRight = false;
  for (auto& d : T::screen) if (d.y == rowY(1) && d.text == "0") scoreRight = d.x + Display::textWidth("0") == Display::kScreenWidth - 2;
  CHECK(scoreRight, "score not right-aligned");
  CHECK(Display::textWidth("Race Room  19-20/20") <= Display::kScreenWidth - 2, "widest title fits");

  g_test = "R1-2 rotation moves the focus down the list and scrolls; clamps at the Leave row; back up to the action";
  size_t pubs0 = T::pubs.size();
  rot(+1); tick();
  CHECK(T::rowAt(rowY(0)) == "> DEADBEEFAAAA (You) 0" && T::rowAt(actionY()) == "Start Round", "focus self: '%s' '%s'",
        T::rowAt(rowY(0)).c_str(), T::rowAt(actionY()).c_str());
  rot(+1); rot(+1); tick();
  CHECK(T::rowAt(titleY()) == "Race Room  2-3/5" && T::rowAt(rowY(0)) == "Alice 0" && T::rowAt(rowY(1)) == "> Bob 0",
        "scrolled: '%s' | '%s' | '%s'", T::rowAt(titleY()).c_str(), T::rowAt(rowY(0)).c_str(), T::rowAt(rowY(1)).c_str());
  for (int i = 0; i < 8; i++) rot(+1);
  tick();
  CHECK(T::rowAt(titleY()) == "Race Room  5-5/5" && T::rowAt(rowY(0)) == "Dave 0" && T::rowAt(rowY(1)) == "> Leave room",
        "clamped at the Leave row: '%s' '%s' '%s'", T::rowAt(titleY()).c_str(), T::rowAt(rowY(0)).c_str(), T::rowAt(rowY(1)).c_str());
  for (int i = 0; i < 5; i++) rot(-1);
  tick();
  CHECK(T::rowAt(rowY(0)) == "> DEADBEEFAAAA (You) 0" && T::rowAt(titleY()) == "Race Room  1-2/5", "up: '%s'",
        T::rowAt(rowY(0)).c_str());
  rot(-1); rot(-1); rot(-1); tick();
  CHECK(T::rowAt(actionY()) == "> Start Round" && T::rowAt(rowY(0)) == "DEADBEEFAAAA (You) 0", "back on action");
  CHECK(T::pubs.size() == pubs0 && devRoom()->phase == RaceProtocol::PHASE_LOBBY && !g_roomPttActive && top() == screenRaceRoom,
        "rotation acted");

  g_test = "R1-3 Short with a participant focused only returns focus to the action (no Start); the next Short starts";
  rot(+1); rot(+1); tick();
  uint32_t v0 = devRoom()->version;
  shortP(); tick();
  CHECK(devRoom()->version == v0 && top() == screenRaceRoom && T::rowAt(actionY()) == "> Start Round", "Short on a participant acted");
  T::now += 20; shortP(); tick();
  CHECK(devRoom()->phase == RaceProtocol::PHASE_ROUND && top() == screenRaceGuess, "Start after returning");

  g_test = "R1-4 same in one batch: [+1, +1, Short] -> no Start; [+1, Short, Short] -> exactly one Start";
  ownerLobby(4);
  v0 = devRoom()->version;
  rot(+1); rot(+1); shortP(); tick();
  CHECK(devRoom()->version == v0 && top() == screenRaceRoom && T::rowAt(actionY()) == "> Start Round", "batch 1");
  T::now += 20; rot(+1); shortP(); T::now += 30; shortP(); tick();
  CHECK(devRoom()->version == v0 + 1 && devRoom()->phase == RaceProtocol::PHASE_ROUND && top() == screenRaceGuess,
        "batch 2: v%u", devRoom()->version);

  g_test = "R1-5 focus is kept by device identity when someone joins; the view follows it";
  ownerLobby(3);  // self, Alice, Bob, Carol
  rot(+1); rot(+1); rot(+1); tick();  // Bob
  CHECK(T::rowAt(rowY(1)) == "> Bob 0", "precondition '%s'", T::rowAt(rowY(1)).c_str());
  g_peers.emplace_back(new Fx::Peer(kPeerIds[3])); Fx::peers.push_back(peer(3)); peer(3)->start();
  presenceName(kPeerIds[3], kPeerNames[3]);
  peersJoin(3, 4);
  CHECK(strcmp(g_roomFocusId, kPeerIds[1]) == 0 && T::rowAt(rowY(1)) == "> Bob 0" && T::rowAt(titleY()) == "Race Room  2-3/5",
        "after join: '%s' '%s'", T::rowAt(titleY()).c_str(), T::rowAt(rowY(1)).c_str());

  g_test = "R1-6 a participant leaving (through its own Leave): identity tracking and clamp to a neighbour";
  // Alice (listed before the focus) leaves: the focus stays on Bob.
  CHECK(peer(0)->eng.leave(), "Alice leave"); tick(3);
  CHECK(strcmp(g_roomFocusId, kPeerIds[1]) == 0 && T::rowAt(rowY(0)) == "> Bob 0", "focus moved off Bob: '%s' '%s'",
        g_roomFocusId, T::rowAt(rowY(0)).c_str());
  // the focused participant (Bob) leaves: clamp to the neighbour now at its index
  CHECK(peer(1)->eng.leave(), "Bob leave"); tick(3);
  CHECK(strcmp(g_roomFocusId, kPeerIds[2]) == 0 && T::rowAt(rowY(0)) == "> Carol 0", "clamped: '%s' / '%s'", g_roomFocusId,
        T::rowAt(rowY(0)).c_str());
  CHECK(peer(2)->eng.leave() && peer(3)->eng.leave(), "others leave"); tick(4);
  CHECK(strcmp(g_roomFocusId, kLeaveRowId) == 0 && T::rowAt(rowY(1)) == "> Leave room" && T::rowAt(titleY()) == "Race Room",
        "clamped to the last row: '%s' '%s'", g_roomFocusId, T::rowAt(rowY(1)).c_str());

  g_test = "R1-7 DOT (PTT) works with a participant focused and does not move the focus";
  ownerLobby(4);
  rot(+1); rot(+1); tick();
  press(InputEventType::DOT_PRESS_START, T::now); tick();
  CHECK(g_roomPttActive && strcmp(g_roomFocusId, kPeerIds[0]) == 0, "PTT start");
  press(InputEventType::DOT_RELEASE, T::now); tick();
  CHECK(!g_roomPttActive && strcmp(g_roomFocusId, kPeerIds[0]) == 0 && devRoom()->phase == RaceProtocol::PHASE_LOBBY, "PTT stop");

  g_test = "R1-8 re-entering the Room (Guess -> Back) starts on the action row";
  ownerLobby(4);
  shortP(); tick();  // Start Round -> Guess
  CHECK(top() == screenRaceGuess, "precondition");
  backP(); tick(2);
  rot(+1); rot(+1); rot(+1); tick();
  enterGuessScreen(); tick();
  backP(); tick(2);
  CHECK(top() == screenRaceRoom && g_roomFocusId[0] == '\0' && T::rowAt(actionY()) == "> Continue",
        "focus '%s' action '%s'", g_roomFocusId, T::rowAt(actionY()).c_str());

  g_test = "R1-9 the room closing rebuilds the list and resets the focus to the action";
  memberOf(3);
  rot(+1); rot(+1); rot(+1); tick();
  CHECK(peer(0)->eng.leave(), "owner closes"); tick(3);
  CHECK(g_roomFocusId[0] == '\0' || g_rowKind[resolveRoomFocus()] == RoomRowKind::FINAL, "focus '%s'", g_roomFocusId);
  CHECK(T::rowAt(actionY()).find("Invite to Play") != std::string::npos && T::rowAt(statusY()) == "Room closed",
        "after close '%s' / '%s'", T::rowAt(actionY()).c_str(), T::rowAt(statusY()).c_str());

  g_test = "R1-10 a name change redraws only that row";
  ownerLobby(4);
  rot(+1); rot(+1); rot(+1); tick();  // window: Alice, > Bob
  size_t before = T::drawLog.size();
  presenceName(kPeerIds[0], "Alicia"); tick();
  int redraws = 0; bool bobRedrawn = false;
  for (size_t i = before; i < T::drawLog.size(); i++) {
    if (T::drawLog[i].text == "Alicia") redraws++;
    if (T::drawLog[i].text == "> Bob") bobRedrawn = true;
  }
  CHECK(redraws == 1 && !bobRedrawn && T::rowAt(rowY(0)) == "Alicia 0", "redraws %d bob %d", redraws, bobRedrawn);
}

static void r2() {
  g_test = "R2-1 Start Round enters Guess exactly once; one committed ROUND; Back shows Continue";
  ownerLobby(1);
  uint8_t depth = g_stackSize;
  int sp = statePubs();
  shortP(); tick();
  CHECK(top() == screenRaceGuess && g_stackSize == depth + 1 && statePubs() == sp + 1 &&
            devRoom()->phase == RaceProtocol::PHASE_ROUND && g_guessNoticeText == nullptr && !g_pendingAutoEnterGuess,
        "depth %u pubs %d", g_stackSize, statePubs() - sp);
  tick(3);
  CHECK(top() == screenRaceGuess && g_stackSize == depth + 1 && T::rowAt(guessRowY()).rfind("Guess:", 0) == 0, "Guess not stable");

  g_test = "R2-2 the starter's own state echo (and a peer's view of it) does not enter Guess again";
  settleMs(4000);
  CHECK(g_stackSize == depth + 1 && !g_pendingAutoEnterGuess && peer(0)->eng.view()->phase == RaceProtocol::PHASE_ROUND,
        "echo re-entered");
  backP(); tick(2);
  CHECK(top() == screenRaceRoom && T::rowAt(actionY()) == "> Continue" && g_stackSize == depth, "Back to Room");

  g_test = "R2-3 history browsing works on the Guess screen entered by Start Round";
  ownerLobby(1);
  shortP(); tick(); tick();
  uint8_t wrong[4] = {static_cast<uint8_t>((g_secret[0] + 1) % 10), g_secret[0], g_secret[1], g_secret[2]};
  memcpy(g_digitEntry.digits, wrong, 4);
  g_digitEntry.count = 4; g_raceGuessDirty = true; tick();
  T::now += 20; shortP(); tick();
  rot(-1); rot(-1); tick();
  CHECK(g_raceBrowse.browsing && g_raceBrowse.selected == 0 && T::rowAt(guessRowY() - lh).rfind(">", 0) == 0,
        "browse on new Guess");

  g_test = "R2-4 publish unconfirmed: still enters Guess once (committed); republished until its echo; no rollback";
  ownerLobby(1);
  depth = g_stackSize;
  T::publishScript.push_back(false);
  shortP(); tick();
  CHECK(top() == screenRaceGuess && g_stackSize == depth + 1 && devRoom()->phase == RaceProtocol::PHASE_ROUND,
        "unconfirmed path");
  CHECK(peer(0)->eng.view()->phase == RaceProtocol::PHASE_LOBBY, "peer saw the lost publish");
  T::connected = true;
  settleMs(8000);
  CHECK(peer(0)->eng.view()->phase == RaceProtocol::PHASE_ROUND && devRoom()->phase == RaceProtocol::PHASE_ROUND &&
            top() == screenRaceGuess && g_stackSize == depth + 1,
        "not republished / navigation changed");
  for (auto& d : T::drawLog) CHECK(d.text.find("elivered") == std::string::npos, "no delivery wording");

  g_test = "R2-5 not Ready: refused on the Room, nothing committed, no navigation";
  ownerLobby(1);
  depth = g_stackSize;
  uint32_t v0 = devRoom()->version;
  T::connected = false;
  shortP(); tick();
  CHECK(top() == screenRaceRoom && g_stackSize == depth && devRoom()->version == v0 &&
            T::rowAt(actionY()) == "Connecting, try again",
        "refusal '%s'", T::rowAt(actionY()).c_str());
  T::connected = true;

  g_test = "R2-6 [Start Short, second Short] in one batch: one Guess push; the queued Short is dropped (replay window)";
  ownerLobby(1);
  depth = g_stackSize;
  v0 = devRoom()->version;
  shortP(); T::now += 1; shortP(); tick(); tick(2);
  CHECK(top() == screenRaceGuess && g_stackSize == depth + 1 && devRoom()->version == v0 + 1, "depth %u", g_stackSize);
  T::now += 300; shortP(); tick();
  CHECK(top() == Menu::confirmPromptScreen, "a later Short is processed by Guess (Exit prompt for an empty draft)");

  g_test = "R2-7 input left after Start Round belongs to Guess: [Short, -1] -> Guess front (browsing), not the Room focus";
  ownerLobby(4);
  shortP(); rot(-1); tick(); tick();
  CHECK(top() == screenRaceGuess && g_raceBrowse.browsing && g_raceBrowse.selected == -1 && g_roomFocusId[0] == '\0',
        "browsing %d focus '%s'", g_raceBrowse.browsing, g_roomFocusId);

  g_test = "R2-8 member: Join stays on the Room; a peer's round enters Guess automatically; Continue re-enters";
  memberOf(1);
  CHECK(top() == screenRaceRoom && T::rowAt(actionY()) == "Waiting for Alice", "after Join: '%s'", T::rowAt(actionY()).c_str());
  CHECK(peer(0)->eng.startRound(), "peer start"); tick(3);
  CHECK(top() == screenRaceGuess, "auto-entry on the coordinator's round");
  backP(); tick(2);
  CHECK(top() == screenRaceRoom && T::rowAt(actionY()) == "> Continue", "'%s'", T::rowAt(actionY()).c_str());
  shortP(); tick();
  CHECK(top() == screenRaceGuess, "Continue");
}

// =============================================================================
static void r3_r5() {
  g_test = "U1 a member's correct guess: CLAIM, 'Solved! 4A0B', 'Confirming...', then the confirmed score";
  memberOf(1);
  peer(0)->script = {9, 8, 7, 6};
  CHECK(peer(0)->eng.startRound(), "start"); tick(3);
  CHECK(top() == screenRaceGuess && memcmp(g_secret, "\x09\x08\x07\x06", 4) == 0, "secret 9876");
  Fx::dropIf = [](const Fx::Msg& m) { return m.from == Identity::deviceId(); };  // hold the CLAIM back
  uint8_t d[4] = {9, 8, 7, 6}; memcpy(g_digitEntry.digits, d, 4); g_digitEntry.count = 4;
  T::now += 20; shortP(); tick(2);
  CHECK(top() == screenRaceRoom && T::rowAt(statusY()) == "Solved! 4A0B" && T::rowAt(actionY()) == "Confirming...",
        "status '%s' action '%s'", T::rowAt(statusY()).c_str(), T::rowAt(actionY()).c_str());
  Fx::dropIf = nullptr;
  settleMs(5000);  // resend
  CHECK(devRoom()->phase == RaceProtocol::PHASE_LOBBY && std::string(devRoom()->resultWinner) == Identity::deviceId(),
        "claim not confirmed");
  CHECK(T::rowAt(actionY()) == "> Start Round" && T::rowAt(rowY(1)) == "DEADBEEFAAAA (You) 1",
        "winner holds the start right; score '%s' / action '%s'", T::rowAt(rowY(1)).c_str(), T::rowAt(actionY()).c_str());

  g_test = "U2 the winner holds the start right: the coordinator waits for it; a member START enters Guess on arrival";
  CHECK(!peer(0)->eng.canStart() && !peer(0)->eng.startRound(), "coordinator started over the holder");
  shortP(); tick();
  CHECK(T::rowAt(actionY()) == "Starting..." || top() == screenRaceGuess, "'%s'", T::rowAt(actionY()).c_str());
  tick(4);
  CHECK(top() == screenRaceGuess && peer(0)->eng.view()->phase == RaceProtocol::PHASE_ROUND, "member START");

  g_test = "U3 another member wins: 'Solved by Alice: <secret>' on Guess, then back to the Room; score beside the name";
  memberOf(1);
  peer(0)->script = {1, 2, 3, 4};
  peer(0)->eng.startRound(); tick(3);
  CHECK(top() == screenRaceGuess, "precondition");
  CHECK(peer(0)->eng.submitCorrectGuess(peer(0)->eng.view()->secret), "Alice solves"); tick(2);
  CHECK(T::visible("Solved by Alice: 1234"), "outcome not shown");
  T::now += 1600; tick(2);
  CHECK(top() == screenRaceRoom && T::rowAt(rowY(0)) == "Alice 1" && T::rowAt(actionY()) == "Waiting for Alice",
        "'%s' '%s'", T::rowAt(rowY(0)).c_str(), T::rowAt(actionY()).c_str());

  g_test = "U4 a late claim: 'Already solved'; no score";
  memberOf(2);
  peer(0)->eng.startRound(); tick(3);
  uint8_t sec[4]; memcpy(sec, g_secret, 4);
  Fx::dropIf = [](const Fx::Msg& m) { return m.from == Identity::deviceId(); };
  memcpy(g_digitEntry.digits, sec, 4); g_digitEntry.count = 4; T::now += 20; shortP(); tick(2);
  CHECK(peer(1)->eng.submitCorrectGuess(sec), "Bob claims first"); tick(2);
  Fx::dropIf = nullptr;
  settleMs(5000);
  CHECK(T::rowAt(actionY()) == "Waiting for Bob" && !g_engine->pendingOp(nullptr), "late claim still pending: '%s'",
        T::rowAt(actionY()).c_str());
  CHECK(devRoom()->entries[RaceProtocol::findEntry(*devRoom(), Identity::deviceId())].score == 0 &&
            std::string(devRoom()->resultWinner) == kPeerIds[1],
        "late claim scored");

  g_test = "U5 Leave row (member): prompt; No keeps membership; Yes leaves only this device";
  memberOf(2);
  for (int i = 0; i < 6; i++) rot(+1);
  tick();
  CHECK(T::rowAt(rowY(1)) == "> Leave room" || T::visible("> Leave room"), "Leave row focused");
  shortP(); tick(2);
  CHECK(top() == Menu::confirmPromptScreen && T::visible("Leave room?"), "prompt");
  press(InputEventType::DOT_RELEASE, T::now); tick(2);  // default answer: No
  CHECK(top() == screenRaceRoom && g_engine->state() == RaceProtocol::MS_MEMBER &&
            RaceProtocol::isActive(*peer(0)->eng.view(), Identity::deviceId()),
        "No changed membership");
  for (int i = 0; i < 6; i++) rot(+1);
  tick(); shortP(); tick();
  rot(+1); press(InputEventType::DOT_RELEASE, T::now); tick(2);  // Yes
  CHECK(g_engine->state() == RaceProtocol::MS_LEAVING || g_engine->state() == RaceProtocol::MS_NONE, "Yes did not leave");
  settleMs(1000);
  CHECK(g_engine->state() == RaceProtocol::MS_NONE && !RaceProtocol::isActive(*peer(0)->eng.view(), Identity::deviceId()) &&
            RaceProtocol::isActive(*peer(0)->eng.view(), kPeerIds[1]) && peer(1)->eng.state() == RaceProtocol::MS_MEMBER,
        "member Leave");
  CHECK(T::rowAt(actionY()) == "> Invite to Play", "'%s'", T::rowAt(actionY()).c_str());

  g_test = "U6 owner Leave: the prompt says it closes the room; Yes closes it for everyone (final scores kept)";
  ownerLobby(2);
  for (int i = 0; i < 6; i++) rot(+1);
  tick(); shortP(); tick(2);
  CHECK(top() == Menu::confirmPromptScreen && T::visible("Leave room?") && T::visible("Closes it for everyone"), "owner prompt");
  rot(+1); press(InputEventType::DOT_RELEASE, T::now); tick(4);  // Yes
  CHECK(g_engine->state() == RaceProtocol::MS_NONE && peer(0)->eng.state() == RaceProtocol::MS_NONE &&
            peer(1)->eng.state() == RaceProtocol::MS_NONE && peer(0)->eng.notice() == RaceProtocol::NOTICE_ROOM_CLOSED &&
            peer(0)->eng.finalView() && RaceProtocol::activeCount(*peer(0)->eng.finalView()) == 3,
        "owner Leave");

  g_test = "U7 Back is navigation only: Room -> Back -> re-enter keeps membership, scores and the peers' view";
  memberOf(1);
  uint32_t v = peer(0)->eng.view()->version;
  backP(); tick(2);
  CHECK(top() == parentStub, "Back left the Room");
  settleMs(2000);
  Menu::pushScreen(screenRaceRoom); tick(2);
  CHECK(g_engine->state() == RaceProtocol::MS_MEMBER && peer(0)->eng.view()->version == v &&
            RaceProtocol::isActive(*peer(0)->eng.view(), Identity::deviceId()) && T::rowAt(actionY()) == "Waiting for Alice",
        "Back changed membership");

  g_test = "U8 reboot: 'Reconnecting...' (Leave only) until the room state; disconnect changes nothing";
  Fx::deviceOnline = false;
  rebootRaceService();  // reboot of the engine with the same NVS
  tick(2);
  CHECK(T::rowAt(actionY()) == "Reconnecting..." && T::rowAt(statusY()) == "Reconnecting...", "'%s' '%s'",
        T::rowAt(actionY()).c_str(), T::rowAt(statusY()).c_str());
  settleMs(20000);
  CHECK(RaceProtocol::isActive(*peer(0)->eng.view(), Identity::deviceId()), "disconnect removed the device");
  Fx::deviceOnline = true;
  for (auto& kv : Fx::retained) Fx::deliverToDevice(kv.second);  // reconnect: retained states
  tick(3);
  CHECK(T::rowAt(actionY()) == "Waiting for Alice" && T::rowAt(statusY()).empty(), "recovered: '%s' '%s'",
        T::rowAt(actionY()).c_str(), T::rowAt(statusY()).c_str());

  g_test = "U9 coordinator unavailable: the pending CLAIM waits (no timeout, no takeover); 'Waiting for Alice' after 15 s";
  memberOf(2);
  peer(0)->eng.startRound(); tick(3);
  CHECK(top() == screenRaceGuess, "precondition");
  peer(0)->online = false;  // the coordinator disappears (no Leave)
  memcpy(g_digitEntry.digits, g_secret, 4); g_digitEntry.count = 4; T::now += 20; shortP(); tick(2);
  CHECK(T::rowAt(actionY()) == "Confirming..." && T::rowAt(statusY()) == "Solved! 4A0B", "'%s' '%s'",
        T::rowAt(actionY()).c_str(), T::rowAt(statusY()).c_str());
  settleMs(16000);
  CHECK(T::rowAt(statusY()) == "Waiting for Alice" && g_engine->state() == RaceProtocol::MS_MEMBER &&
            g_engine->claimPending(devRoom()->roundStart) && T::rowAt(actionY()) == "Confirming...",
        "'%s' '%s'", T::rowAt(statusY()).c_str(), T::rowAt(actionY()).c_str());
  settleMs(60000);
  CHECK(g_engine->claimPending(devRoom()->roundStart) && peer(1)->eng.state() == RaceProtocol::MS_MEMBER, "gave up / took over");
  peer(0)->online = true;
  settleMs(35000);  // resend backoff is capped at 30 s
  CHECK(std::string(devRoom()->resultWinner) == Identity::deviceId() && T::rowAt(statusY()).empty(),
        "not completed after the coordinator returned: '%s'", T::rowAt(statusY()).c_str());

  g_test = "U10 frozen (same-version conflict): 'Room data inconsistent', 'Leave only', no Start/Continue";
  memberOf(1);
  {
    RaceProtocol::RoomState s = *devRoom(); s.createdUnix ^= 1;
    uint8_t b[600]; size_t l = RaceProtocol::encodeState(s, b, sizeof(b));
    char topic[24]; snprintf(topic, sizeof(topic), "r2/%s", kPeerIds[0]);
    handleRaceStatePacket("GROUP1", topic, b, l);
  }
  peer(0)->eng.startRound(); tick(3);
  CHECK(top() == screenRaceRoom && T::rowAt(statusY()) == "Room data inconsistent" && T::rowAt(actionY()) == "Leave only",
        "'%s' '%s'", T::rowAt(statusY()).c_str(), T::rowAt(actionY()).c_str());
  shortP(); tick();
  CHECK(top() == screenRaceRoom, "acted while frozen");

  g_test = "U10b conflict detected while on Guess: the round ends there ('Round ended'); a correct guess is not sent";
  memberOf(1);
  peer(0)->eng.startRound(); tick(3);
  CHECK(top() == screenRaceGuess, "precondition");
  {
    RaceProtocol::RoomState s = *devRoom(); s.createdUnix ^= 1;
    uint8_t b[600]; size_t l = RaceProtocol::encodeState(s, b, sizeof(b));
    char topic[24]; snprintf(topic, sizeof(topic), "r2/%s", kPeerIds[0]);
    handleRaceStatePacket("GROUP1", topic, b, l);
  }
  memcpy(g_digitEntry.digits, g_secret, 4); g_digitEntry.count = 4;
  tick();
  CHECK(T::visible("Round ended"), "frozen round still playable");
  T::now += 20; shortP(); tick(2);
  CHECK(!g_engine->pendingOp(nullptr) && peer(0)->eng.view()->phase == RaceProtocol::PHASE_ROUND, "claim sent while frozen");

  g_test = "U11 storage failure while applying: 'Storage error - room paused'; nothing applied";
  memberOf(1);
  Storage::race().failWrite = [](const char* k) { return strcmp(k, "rm_member") == 0; };
  uint32_t shownV = devRoom()->version;
  peer(0)->eng.startRound(); tick(3);
  CHECK(T::rowAt(statusY()) == "Storage error - room paused" && devRoom()->version == shownV && top() == screenRaceRoom,
        "'%s' v%u", T::rowAt(statusY()).c_str(), devRoom()->version);
  Storage::race().failWrite = nullptr;

  g_test = "U12 discovery: a new room in the group sets the Race invite notification and lists 'Join <name> (n)'";
  freshDevice(1);
  g_invitePending = 0;
  CHECK(peer(0)->eng.createRoom("GROUP1"), "create"); tick(3);
  CHECK(g_invitePending == 1, "notification %d", g_invitePending);
  CHECK(T::rowAt(rowY(0)) == "Join Alice (1)" && T::rowAt(actionY()) == "> Invite to Play", "'%s'", T::rowAt(rowY(0)).c_str());

  g_test = "U13 room full: 'Room full'; not admitted";
  freshDevice(6);
  CHECK(peer(0)->eng.createRoom("GROUP1"), "create"); tick(2);
  for (int i = 1; i < 6; i++) { peer(i)->eng.join("GROUP1", kPeerIds[0]); tick(2); }
  // fill to 20 with synthetic members, through the coordinator's real request path
  for (int i = 0; i < 14; i++) {
    RaceProtocol::Request q; memset(&q, 0, sizeof(q)); q.op = RaceProtocol::OP_JOIN;
    strncpy(q.coordinator, kPeerIds[0], 12); q.n = peer(0)->eng.view()->n; q.nonce = peer(0)->eng.view()->nonce;
    snprintf(q.sender, sizeof(q.sender), "F%011d", i); q.seq = 1; q.basis = peer(0)->eng.view()->version;
    uint8_t b[64]; size_t l = RaceProtocol::encodeRequest(q, b, sizeof(b));
    char topic[24]; snprintf(topic, sizeof(topic), "r2q/%s", kPeerIds[0]);
    peer(0)->eng.onPacket("GROUP1", topic, RaceProtocol::kKindRequest, b, l);
  }
  tick(3);
  CHECK(RaceProtocol::activeCount(*peer(0)->eng.view()) == 20, "20 active (%u)", RaceProtocol::activeCount(*peer(0)->eng.view()));
  rot(+1); tick(); shortP(); tick(4);
  CHECK(g_engine->state() == RaceProtocol::MS_NONE && T::rowAt(statusY()) == "Room full", "'%s' state %d",
        T::rowAt(statusY()).c_str(), g_engine->state());
}

static int devRequests() {
  int n = 0;
  for (auto& p : T::pubs) if (p.topic.rfind("r2q/", 0) == 0 && p.data.size() > 7 && p.data[7] != RaceProtocol::OP_REPLY) n++;
  return n;
}
static void review() {
  g_test = "U15 actual identity.cpp Race counter write failing (Storage::core): Join sends nothing, shows the storage error";
  freshDevice(1);
  CHECK(peer(0)->eng.createRoom("GROUP1"), "peer room"); tick(2);
  Storage::core().failWrite = [](const char* k) { return strcmp(k, "raceSeq") == 0; };  // Race's own counter
  int r0 = devRequests();
  rot(+1); tick(); shortP(); tick(3);
  CHECK(devRequests() == r0 && g_engine->state() == RaceProtocol::MS_NONE, "JOIN sent with an unreserved number (%d)", devRequests() - r0);
  CHECK(T::rowAt(statusY()) == "Storage error" || T::rowAt(actionY()) == "Couldn't join", "'%s' / '%s'",
        T::rowAt(statusY()).c_str(), T::rowAt(actionY()).c_str());
  Storage::core().failWrite = nullptr;
  T::now += 2000; tick(2);
  rot(+1); tick(); shortP(); tick(4);
  CHECK(g_engine->state() == RaceProtocol::MS_MEMBER, "Join after recovery");

  g_test = "U16 actual identity.cpp Race counter exhausted: Invite to Play refused, no wrap (both before and after migration)";
  // (a) first reservation, migrating from a message-id counter already at 0xFFFFFFFF
  freshDevice(0);
  Storage::core().kv.clear();
  Storage::core().putULong("idCounter", 0xFFFFFFFFu);
  shortP(); tick(2);
  // the exhausted migration is stored (raceSeq = 0xFFFFFFFF), so it outlives idCounter wrapping later
  CHECK(!g_engine->hasRoom() && statePubs() == 0 && Storage::core().getULong("raceSeq", 0) == 0xFFFFFFFFu &&
            Storage::core().getULong("idCounter", 0) == 0xFFFFFFFFu,
        "room created from an exhausted counter (raceSeq %u)", (unsigned)Storage::core().getULong("raceSeq", 0));
  // (b) Race's own counter at 0xFFFFFFFF, with the message-id counter low (e.g. wrapped by nextId())
  freshDevice(0);
  Storage::core().kv.clear();
  Storage::core().putULong("raceSeq", 0xFFFFFFFFu);
  Storage::core().putULong("idCounter", 3);
  T::now += 2000; tick(2);
  shortP(); tick(2);
  CHECK(!g_engine->hasRoom() && statePubs() == 0 && Storage::core().getULong("raceSeq", 0) == 0xFFFFFFFFu,
        "room created with an exhausted Race counter (raceSeq now %u)", (unsigned)Storage::core().getULong("raceSeq", 0));
  Storage::core().kv.clear();

  g_test = "U17 frozen member's Leave with the NONE write failing: shown as leaving + storage error, not as exited";
  memberOf(1);
  {
    RaceProtocol::RoomState s = *devRoom(); s.createdUnix ^= 1;
    uint8_t b[600]; size_t l = RaceProtocol::encodeState(s, b, sizeof(b));
    char topic[24]; snprintf(topic, sizeof(topic), "r2/%s", kPeerIds[0]);
    handleRaceStatePacket("GROUP1", topic, b, l);
  }
  tick();
  Storage::race().failWrite = [](const char* k) {
    if (strcmp(k, "rm_member") != 0) return false;
    return true;  // every member-record write fails (LEAVING and NONE)
  };
  for (int i = 0; i < 6; i++) rot(+1);
  tick(); shortP(); tick(2);
  rot(+1); press(InputEventType::DOT_RELEASE, T::now); tick(3);  // Yes
  CHECK(T::rowAt(actionY()) != "> Invite to Play" && g_engine->state() != RaceProtocol::MS_NONE,
        "exit shown while nothing was stored: '%s'", T::rowAt(actionY()).c_str());
  CHECK(T::rowAt(statusY()).rfind("Storage error", 0) == 0 || T::rowAt(actionY()).rfind("Storage error", 0) == 0,
        "no storage error shown: status '%s' action '%s'", T::rowAt(statusY()).c_str(), T::rowAt(actionY()).c_str());
  Storage::race().failWrite = nullptr;
  settleMs(3000);
  for (int i = 0; i < 6; i++) rot(+1);
  tick(); shortP(); tick(2); rot(+1); press(InputEventType::DOT_RELEASE, T::now); tick(3);  // Leave again (if still offered)
  settleMs(3000);
  CHECK(g_engine->state() == RaceProtocol::MS_NONE && T::rowAt(actionY()) == "> Invite to Play", "not exited after recovery: '%s'",
        T::rowAt(actionY()).c_str());
}

static void sizes() {
  g_test = "U14 actual wire sizes through race.cpp's publish path (20 entries; worst-case 32-char group)";
  ownerLobby(0);
  const RaceProtocol::RoomState* v = devRoom();
  for (int i = 0; i < 19; i++) {
    RaceProtocol::Request q; memset(&q, 0, sizeof(q)); q.op = RaceProtocol::OP_JOIN;
    strncpy(q.coordinator, Identity::deviceId(), 12); q.n = v->n; q.nonce = v->nonce;
    snprintf(q.sender, sizeof(q.sender), "F%011d", i); q.seq = 1; q.basis = devRoom()->version;
    uint8_t b[64]; size_t l = RaceProtocol::encodeRequest(q, b, sizeof(b));
    CHECK(l == RaceProtocol::kRequestBytes, "request %zu", l);
    char topic[24]; snprintf(topic, sizeof(topic), "r2q/%s", Identity::deviceId());
    handleRaceRequestPacket("GROUP1", topic, b, l);
  }
  tick(2);
  CHECK(devRoom()->count == 20, "count %u", devRoom()->count);
  const T::Pub* rs = nullptr; const T::Pub* rep = nullptr;
  for (auto& p : T::pubs) {
    if (p.topic.rfind("r2/", 0) == 0) rs = &p;
    if (p.topic.rfind("r2q/", 0) == 0) rep = &p;
  }
  CHECK(rs && rs->data.size() == 544 && rs->retained && rs->topic == std::string("r2/") + Identity::deviceId(),
        "RS publish %zu bytes", rs ? rs->data.size() : 0);
  CHECK(rep && rep->data.size() == 6 + 43 && !rep->retained, "reply publish %zu bytes", rep ? rep->data.size() : 0);
  // MQTT 2.5.2 framing with the worst-case 32-character group: "morsebuddy/<group>/<suffix>"
  size_t topicRs = 11 + 32 + 1 + rs->topic.size(), topicQ = 11 + 32 + 1 + rep->topic.size();
  size_t remRs = 2 + topicRs + 2 + rs->data.size(), rxRs = 1 + (remRs < 128 ? 1 : 2) + remRs;
  size_t remRep = 2 + topicQ + 2 + rep->data.size(), rxRep = 1 + (remRep < 128 ? 1 : 2) + remRep;
  size_t remReq = 2 + topicQ + 2 + 6 + RaceProtocol::kRequestBytes, rxReq = 1 + (remReq < 128 ? 1 : 2) + remReq;
  size_t txPart = 1 + (remRs < 128 ? 1 : 2) + 2 + topicRs + 2;
  printf("   wire: RS topic %zu, payload %zu, RX %zu; request RX %zu; reply payload %zu, RX %zu; TX part %zu\n", topicRs,
         rs->data.size(), rxRs, rxReq, rep->data.size(), rxRep, txPart);
  CHECK(topicRs == 59 && topicQ == 60 && rxRs == 610 && rxReq == 126 && rxRep == 115 && txPart == 66 && rxRs <= 1024 &&
            txPart <= 256 && topicQ < 64,
        "wire sizes");
}

static void allocation() {
  g_test = "U18 engine allocation fails at boot: 'Race unavailable', Race inert, saved records untouched, other screens work";
  memberOf(1);  // a member with saved Race records
  CHECK(g_engine->state() == RaceProtocol::MS_MEMBER, "precondition");
  std::map<std::string, std::string> saved = Storage::race().kv;
  std::map<std::string, std::string> savedCore = Storage::core().kv;
  int writes0 = Storage::race().writes;
  g_failNothrowNew = true;
  rebootRaceService();
  g_failNothrowNew = false;
  CHECK(g_engine == nullptr, "engine allocated despite the failure");
  size_t pubs0 = T::pubs.size();
  backP(); tick(2);
  Menu::pushScreen(screenRaceRoom); tick(2);
  CHECK(T::rowAt(actionY()) == "Race unavailable" && T::rowAt(rowY(0)).empty() && T::rowAt(statusY()).empty(),
        "screen: action '%s' row0 '%s' status '%s'", T::rowAt(actionY()).c_str(), T::rowAt(rowY(0)).c_str(), T::rowAt(statusY()).c_str());
  // every input and every inbound path: nothing happens, nothing crashes
  shortP(); tick(); rot(+1); tick(); shortP(); tick(); rot(-1); tick();
  press(InputEventType::DOT_PRESS_START, T::now); tick(); press(InputEventType::DOT_RELEASE, T::now); tick();
  CHECK(!g_roomPttActive, "room voice started without an engine");
  CHECK(peer(0)->eng.startRound(), "peer starts a round (its states reach the handlers)");
  settleMs(70000);  // peer heartbeats, room states, the race service tick
  {
    RaceProtocol::Request q; memset(&q, 0, sizeof(q)); q.op = RaceProtocol::OP_JOIN;
    strncpy(q.coordinator, Identity::deviceId(), 12); strncpy(q.sender, kPeerIds[0], 12); q.seq = 1;
    uint8_t b[64]; size_t l = RaceProtocol::encodeRequest(q, b, sizeof(b));
    char topic[24]; snprintf(topic, sizeof(topic), "r2q/%s", Identity::deviceId());
    handleRaceRequestPacket("GROUP1", topic, b, l);
  }
  enterGuessScreen(); tick(3);  // not reachable from the UI; must still be inert
  CHECK(top() != screenRaceGuess || T::visible("Round ended"), "Guess usable without an engine");
  T::now += 2000; tick(2);
  CHECK(T::pubs.size() == pubs0, "%zu publish(es) without an engine", T::pubs.size() - pubs0);
  CHECK(Storage::race().kv == saved && Storage::race().writes == writes0 && Storage::core().kv == savedCore,
        "saved Race records changed (writes %d)", Storage::race().writes - writes0);
  backP(); tick(2);
  CHECK(top() == parentStub, "Back did not leave the Room");
  Menu::pushScreen(screenRaceEntry); tick(2);  // Race entry still routes (to the Room) without crashing
  CHECK(T::rowAt(actionY()) == "Race unavailable", "'%s'", T::rowAt(actionY()).c_str());
  backP(); tick(2);

  g_test = "U18b next boot with memory available: the same saved membership is restored";
  rebootRaceService();
  CHECK(g_engine != nullptr && g_engine->state() == RaceProtocol::MS_MEMBER &&
            std::string(g_engine->roomGroup()) == "GROUP1",
        "membership not restored from the untouched records");
  Menu::pushScreen(screenRaceRoom); tick(2);
  for (auto& kv : Fx::retained) Fx::deliverToDevice(kv.second);
  tick(4);
  CHECK(T::rowAt(actionY()) != "Race unavailable" && devRoom() != nullptr, "'%s'", T::rowAt(actionY()).c_str());

  g_test = "U19 repeated initialization: no new allocation, same engine, no re-boot, no writes, no publishes";
  RaceProtocol::Engine* e0 = g_engine;
  int news0 = g_nothrowNews, w0 = Storage::race().writes;
  size_t p0 = T::pubs.size();
  RaceProtocol::MemberState st0 = g_engine->state();
  bool had = devRoom() != nullptr;
  for (int i = 0; i < 3; i++) raceServiceInit();
  CHECK(g_engine == e0 && g_nothrowNews == news0, "engine replaced or re-allocated (%d allocations)", g_nothrowNews - news0);
  CHECK(g_engine->state() == st0 && (devRoom() != nullptr) == had && Storage::race().writes == w0 && T::pubs.size() == p0,
        "repeated init re-booted the engine (writes %d, pubs %zu)", Storage::race().writes - w0, T::pubs.size() - p0);
  g_failNothrowNew = true;
  raceServiceInit();  // even with allocation failing, an active engine is kept
  g_failNothrowNew = false;
  CHECK(g_engine == e0, "an active engine was dropped by a repeated init");
}

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);  // FAIL lines survive a later crash
  Identity::init();  // the actual identity.cpp: device id from the (fake) MAC
  T_initServices();
  r1();
  r2();
  r3_r5();
  sizes();
  review();
  allocation();
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
