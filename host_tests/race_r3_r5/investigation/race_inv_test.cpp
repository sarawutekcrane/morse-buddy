// INVESTIGATION REPRO (host only, not production; not part of run_all.sh).
// Reuses room_test.cpp's fixture (ACTUAL race.cpp + menu.cpp + race_protocol.cpp
// + identity.cpp; peers are ACTUAL engines) by including it with its main()
// renamed. Each case records the CURRENT behaviour of commit 30ed514.
#define main room_test_main
#include "../room_test.cpp"
#undef main

// B = this device (member); A = peer 0 (coordinator, "Alice"). Secret 9876;
// the guess 9 6 8 0 scores 1A2B.
static bool hasGuess9680() {
  return g_raceHistoryCount == 1 && g_raceHistory[0].guessValue == 9680 && g_raceHistory[0].a == 1 && g_raceHistory[0].b == 2;
}
static void roundWithOneGuess() {
  memberOf(1);
  peer(0)->script = {9, 8, 7, 6};
  CHECK(peer(0)->eng.startRound(), "fixture: start"); tick(3);
  CHECK(top() == screenRaceGuess, "fixture: auto-entered Guess");
  uint8_t d[4] = {9, 6, 8, 0}; memcpy(g_digitEntry.digits, d, 4); g_digitEntry.count = 4; g_raceGuessDirty = true; tick();
  T::now += 20; shortP(); tick(2);
  CHECK(hasGuess9680(), "fixture: 9 6 8 0 -> 1A2B recorded (count %u)", g_raceHistoryCount);
}
// Room: rotate to "Leave room", Short, then Yes (rotate to Yes + DOT release).
static void leaveYes() {
  for (int i = 0; i < 6; i++) rot(+1);
  tick(); shortP(); tick(2);
  CHECK(top() == Menu::confirmPromptScreen && T::visible("Leave room?"), "prompt shown");
  rot(+1); press(InputEventType::DOT_RELEASE, T::now); tick(2);
}
static void leaveNo() {
  for (int i = 0; i < 6; i++) rot(+1);
  tick(); shortP(); tick(2);
  press(InputEventType::DOT_RELEASE, T::now); tick(2);  // default answer: No
}
// Back from the Room to the menu below Race, then re-enter Race (Room).
static void backToMenuAndReenter() {
  backP(); tick(2);
  CHECK(top() == parentStub, "Back left Race");
  T::now += 1000; tick();
  Menu::pushScreen(screenRaceRoom); tick(2);
}
static void joinAliceAndEnterRound() {
  CHECK(T::rowAt(rowY(0)).rfind("Join Alice", 0) == 0, "Join row '%s'", T::rowAt(rowY(0)).c_str());
  rot(+1); tick(); shortP(); tick(6);
  // A member joining a live round: Continue (or auto-entry) opens Guess.
  if (top() == screenRaceRoom) { while (resolveRoomFocus() != -1) { rot(-1); tick(); } shortP(); tick(3); }
}

static void history() {
  g_test = "H1 Guess -> Back -> Leave (Yes, completed) -> Back -> rejoin -> same round";
  roundWithOneGuess();
  backP(); tick(2);
  CHECK(top() == screenRaceRoom, "Back to Room");
  leaveYes();
  settleMs(2000);
  CHECK(g_engine->state() == RaceProtocol::MS_NONE, "leave completed (state %d)", g_engine->state());
  backToMenuAndReenter();
  joinAliceAndEnterRound();
  printf("  H1: state=%d top=%s roundId='%s' historyCount=%u\n", g_engine->state(),
         top() == screenRaceGuess ? "Guess" : top() == screenRaceRoom ? "Room" : "?", g_roundId, g_raceHistoryCount);
  CHECK(g_engine->state() == RaceProtocol::MS_MEMBER && top() == screenRaceGuess, "rejoined the same round's Guess");
  CHECK(g_raceHistoryCount == 0, "CURRENT BEHAVIOUR: history is discarded (count %u)", g_raceHistoryCount);

  g_test = "H2 same, but the coordinator's acknowledgement is delayed: Back while 'Leaving...', re-enter, then it completes";
  roundWithOneGuess();
  backP(); tick(2);
  Fx::dropIf = [](const Fx::Msg& m) { return m.from == Identity::deviceId(); };  // the LEAVE does not reach A yet
  leaveYes();
  CHECK(g_engine->state() == RaceProtocol::MS_LEAVING, "LEAVING (state %d)", g_engine->state());
  backToMenuAndReenter();
  printf("  H2: re-entered while LEAVING: action='%s' historyCount=%u\n", T::rowAt(actionY()).c_str(), g_raceHistoryCount);
  Fx::dropIf = nullptr;
  settleMs(8000);  // the LEAVE is resent and acknowledged
  printf("  H2: after the LEAVE completes: state=%d historyCount=%u\n", g_engine->state(), g_raceHistoryCount);
  if (g_engine->state() == RaceProtocol::MS_NONE) joinAliceAndEnterRound();
  printf("  H2: after rejoin: state=%d top=%s historyCount=%u\n", g_engine->state(),
         top() == screenRaceGuess ? "Guess" : "Room", g_raceHistoryCount);
  CHECK(g_raceHistoryCount == 0, "CURRENT BEHAVIOUR: discarded once the LEAVE completes (count %u)", g_raceHistoryCount);

  g_test = "H3 the prompt answered with its default (No): Back -> re-enter -> Continue keeps the history";
  roundWithOneGuess();
  backP(); tick(2);
  leaveNo();
  CHECK(g_engine->state() == RaceProtocol::MS_MEMBER, "still a member");
  backToMenuAndReenter();
  printf("  H3: action='%s'\n", T::rowAt(actionY()).c_str());
  shortP(); tick(3);  // Continue
  printf("  H3: top=%s historyCount=%u\n", top() == screenRaceGuess ? "Guess" : "Room", g_raceHistoryCount);
  CHECK(top() == screenRaceGuess && hasGuess9680(), "history kept (count %u)", g_raceHistoryCount);

  g_test = "H3b 'Yes' attempted with two rotation detents: the prompt toggles back to No; DOT answers No";
  roundWithOneGuess();
  backP(); tick(2);
  for (int i = 0; i < 6; i++) rot(+1);
  tick(); shortP(); tick(2);
  rot(+1); rot(+1); press(InputEventType::DOT_RELEASE, T::now); tick(2);
  CHECK(g_engine->state() == RaceProtocol::MS_MEMBER && top() == screenRaceRoom, "two detents + DOT did not leave (state %d)",
        g_engine->state());
  backToMenuAndReenter();
  shortP(); tick(3);
  CHECK(top() == screenRaceGuess && hasGuess9680(), "the same round with its history (count %u)", g_raceHistoryCount);

  g_test = "H4 Leave 'Yes' never acknowledged (coordinator ignores it): Back -> re-enter -> what can B do?";
  roundWithOneGuess();
  backP(); tick(2);
  Fx::dropIf = [](const Fx::Msg& m) { return m.from == Identity::deviceId(); };
  leaveYes();
  backToMenuAndReenter();
  printf("  H4: state=%d action='%s' rows='%s'|'%s'|'%s' historyCount=%u\n", g_engine->state(), T::rowAt(actionY()).c_str(),
         T::rowAt(rowY(0)).c_str(), T::rowAt(rowY(1)).c_str(), T::rowAt(rowY(2)).c_str(), g_raceHistoryCount);
  shortP(); tick(3);
  printf("  H4: Short on the action -> top=%s\n", top() == screenRaceGuess ? "Guess" : top() == screenRaceRoom ? "Room" : "other");
  Fx::dropIf = nullptr;

  g_test = "H5 a reboot of B in the round (no Leave): Reconnecting -> same round";
  roundWithOneGuess();
  backP(); tick(2);
  rebootRaceService(); tick(2);
  for (auto& kv : Fx::retained) Fx::deliverToDevice(kv.second);
  tick(4);
  printf("  H5: after an engine reboot (Reconnecting, then the room state): state=%d historyCount=%u\n", g_engine->state(),
         g_raceHistoryCount);
  CHECK(g_raceHistoryCount == 0, "CURRENT BEHAVIOUR: discarded by Reconnecting (count %u); a power cycle loses RAM anyway",
        g_raceHistoryCount);
}

static void scores() {
  g_test = "S1 B wins round 1, then round 2: the committed and published score is 1, then 2, on both engines";
  memberOf(1);
  for (int round = 1; round <= 2; round++) {
    if (round == 1) { CHECK(peer(0)->eng.startRound(), "start 1"); }
    else { shortP(); tick(); }  // B holds the start right after winning
    tick(4);
    CHECK(top() == screenRaceGuess, "Guess %d", round);
    memcpy(g_digitEntry.digits, g_secret, 4); g_digitEntry.count = 4; T::now += 20; shortP(); tick(2);
    settleMs(3000);
    const RaceProtocol::RoomState* a = peer(0)->eng.view();
    const RaceProtocol::RoomState* b = devRoom();
    int ia = RaceProtocol::findEntry(*a, Identity::deviceId()), ib = RaceProtocol::findEntry(*b, Identity::deviceId());
    printf("  S1 round %d: A(coordinator) score=%u version=%u | B(member) score=%u version=%u | B row '%s'\n", round,
           a->entries[ia].score, a->version, b->entries[ib].score, b->version, T::rowAt(rowY(1)).c_str());
    CHECK(a->entries[ia].score == round && b->entries[ib].score == round && a->version == b->version,
          "round %d: scores A=%u B=%u", round, a->entries[ia].score, b->entries[ib].score);
    CHECK(g_rowScore[1] == round, "the Room row model holds %u (g_rowScore)", g_rowScore[1]);
  }
  // Durable: the coordinator's committed slot record (what a reboot reloads).
  peer(0)->eng = RaceProtocol::Engine(); peer(0)->start(); tick(4);  // coordinator reboot: same NVS
  const RaceProtocol::RoomState* a = peer(0)->eng.view();
  int ia = a ? RaceProtocol::findEntry(*a, Identity::deviceId()) : -1;
  CHECK(ia >= 0 && a->entries[ia].score == 2, "after the coordinator reboots: score %u", ia >= 0 ? a->entries[ia].score : 999);
}

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Identity::init();
  T_initServices();
  history();
  scores();
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
