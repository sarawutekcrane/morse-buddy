// HOST TEST (not production). Phase 2C-2 Number Guessing Friend.
// Compiles the ACTUAL src/core/menu.cpp and src/core/number_guessing_friend.cpp
// into this translation unit (so the real navigation stack and Friend's
// internal state are inspectable), and links the actual number_guessing.cpp
// and packet_codec.cpp. Menu is NOT stubbed. Input is a two-stage model:
// test presses go to T::raw and only Input::update() converts them.
//
// -DBASELINE: compile only the observable-behaviour reproduction tests
// (no new API) so they can run against the 230999b baseline sources.
#include "stubs.h"
#include "core/menu.cpp"
#include "core/number_guessing_friend.cpp"
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

// ---- harness helpers ---------------------------------------------------------
static const char* nameOf(ScreenHandlerFn f) {
  if (f == mainMenuScreen) return "Main";
  if (f == screenFriendChat) return "Chat";
#ifndef BASELINE
  if (f == trampolineOpenChat) return "OpenTramp";
#endif
  if (f == screenSetManuallyOrRandom) return "SetMR";
  if (f == trampolineSetManually) return "ManTramp";
  if (f == trampolineRandomize) return "RandTramp";
  if (f == screenManualEntry) return "Manual";
  if (f == screenRandomConfirm) return "Random";
  if (f == screenQuickSwitch) return "QS";
  if (f == quickSwitchText) return "QSText";
  if (f == quickSwitchEnigma) return "QSEnigma";
  if (f == screenAnswer) return "Answer";
  if (f == screenFriendResult) return "Result";
  if (f == screenFriendRecipient) return "Recipient";
  if (f == recipientTrampoline) return "RecipTramp";
  if (f == Menu::confirmPromptScreen) return "Prompt";
  return "?";
}
static std::string stackStr() {
  std::string s;
  for (uint8_t i = 0; i < g_stackSize; i++) { if (i) s += ">"; s += nameOf(g_stack[i]); }
  return s;
}
static void tick(int n = 1) { for (int i = 0; i < n; i++) { Menu::tick(); T::now += 5; } }
// Causality (input.cpp): a sampler reads atMs with millis() before queuing,
// so an event can never carry a timestamp later than the clock at pop time.
static void press(InputEventType t, uint32_t at, uint32_t dur = 80) {
  if (static_cast<int32_t>(at - T::now) > 0) { printf("FIXTURE ERROR: future timestamp %u > now %u\n", at, T::now); exit(3); }
  T::raw.push_back(InputEvent(t, 0, dur, at));
}
static void dot(uint32_t at, uint32_t dur = 80) { press(InputEventType::DOT_RELEASE, at, dur); }
static void shortP(uint32_t at) { press(InputEventType::ENCODER_SHORT, at); }
static void back(uint32_t at) { press(InputEventType::ENCODER_LONG, at); }
static void rot(int8_t v) { T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, v, 0, 0)); }
static int count(const std::string& what) { int n = 0; for (auto& s : T::seq) if (s == what) n++; return n; }
static int pubCalls() { return static_cast<int>(T::pubs.size()); }
static int seqIndex(const std::string& what, int from = 0) {
  for (int i = from; i < static_cast<int>(T::seq.size()); i++) if (T::seq[i] == what) return i;
  return -1;
}
static uint32_t lastDrawTime(const std::string& text) {
  uint32_t t = 0; bool any = false;
  for (auto& d : T::drawLog) if (d.text == text) { t = d.t; any = true; }
  return any ? t : 0xFFFFFFFFu;
}
static bool drawn(const std::string& text) { for (auto& d : T::drawLog) if (d.text == text) return true; return false; }
// Solo-style answer geometry, derived from the display API (not hardcoded).
static int guessRowY() { return Display::kScreenHeight - Display::lineHeight(); }
static int historyRowY(int fromBottom) { return guessRowY() - fromBottom * Display::lineHeight(); }  // 1 = newest
static int maxHistoryRows(bool notice) {
  int top = Display::kStatusBarHeight + 2 + (notice ? Display::lineHeight() : 0);
  return (guessRowY() - top) / Display::lineHeight();
}
static std::string histRow(uint16_t v, uint8_t a, uint8_t b) {
  char t[32]; snprintf(t, sizeof(t), "%u %u %u %u %uA%uB", v / 1000 % 10, v / 100 % 10, v / 10 % 10, v % 10, a, b);
  return t;
}
// Actual Presence::handleIncoming() with an MBP1 payload.
static void presenceMsg(const char* group, const char* id, const char* name, const char* status, int radio = 0) {
  char p[110]; snprintf(p, sizeof(p), "MBP1|%s|%s|%s|%d|%lu|255", id, name, status, radio, (unsigned long)T::unixTime);
  Presence::handleIncoming(group, p, static_cast<uint16_t>(strlen(p)));
}
void T_initServices();
void T_fireSettingsChange(const SettingsChangeInfo& info);
static void deleteGroup(const char* group) {
  SettingsChangeInfo info{}; info.event = SET_GROUP_DELETED; strncpy(info.group_code, group, sizeof(info.group_code) - 1);
  T_fireSettingsChange(info);
}
static int visibleY(const std::string& text) { for (auto& d : T::screen) if (d.text == text) return d.y; return -1; }

static void resetAll() {
  T::reset();
  Menu::init();
  g_stackSize = 1;
  Menu::consumeJustEntered();
  strcpy(g_selectedGroupCode, "GROUP1");
  strcpy(g_selectedContactKey, "BOBKEY");
#ifndef BASELINE
  g_challengeFlowPushed = false;
  g_friendMenuDotGuard = DrawnBeforeGuard{};
  g_manualDotGuard = DrawnBeforeGuard{};
  g_randomDotGuard = DrawnBeforeGuard{};
  g_answerShortGuard = DrawnBeforeGuard{};
  g_resultDotGuard = DrawnBeforeGuard{};
  g_resultStatusValid = false;
#endif
}
static void openChat() {
  Menu::pushScreen(screenFriendChat);
  tick(2);
}
// A received LOCKED challenge with secret 1234 (actual handleGameArrival()).
static void receiveChallenge(const char* id = "CHAL0000000000000000001") {
  uint8_t secret[4] = {1, 2, 3, 4};
  uint8_t payload[4];
  size_t n = encodeChallengePayload(secret, payload, sizeof(payload));
  PacketCodec::MessageEnvelope env;
  memset(&env, 0, sizeof(env));
  strncpy(env.message_id, id, sizeof(env.message_id) - 1);
  env.schema_version = 1;
  env.message_type = PacketCodec::MSG_TYPE_GAME;
  strcpy(env.sender_device_id, "BOBDEVICE");
  strcpy(env.group_code, "GROUP1");
  env.timestamp = 1700000000u;
  handleGameArrival("GROUP1", "BOBKEY", env, env.timestamp, payload, static_cast<uint16_t>(n));
  markIndexDirty();
}
// From an open chat with one received challenge as the newest message:
// focus it (rotate) and Short -> Answer. Leaves the Answer screen drawn.
static void openAnswer() {
  rot(-1);
  tick();
  shortP(T::now);
  tick(2);
}
static void setDigits(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  g_digitEntry.digits[0] = a; g_digitEntry.digits[1] = b; g_digitEntry.digits[2] = c; g_digitEntry.digits[3] = d;
  g_digitEntry.count = 4;
}
static FriendGameState storedState(const char* group) {
  FriendGameState s; memset(&s, 0, sizeof(s));
  for (auto& r : T::store) if (r.hdr.direction == MessageStore::Direction::SENT && !strcmp(r.ref.group_code, group))
    decodeFriendLocal(r.local.data(), static_cast<uint16_t>(r.local.size()), &s);
  return s;
}
static FriendGameState storedGame() {
  FriendGameState s; memset(&s, 0, sizeof(s));
  for (auto& r : T::store) if (r.hdr.direction == MessageStore::Direction::RECEIVED)
    decodeFriendLocal(r.local.data(), static_cast<uint16_t>(r.local.size()), &s);
  return s;
}
// Chat -> new-challenge choice (Set Manually / Randomize), drawn.
static void toSetMR() {
  shortP(T::now); tick(2);   // empty line -> choice
}


// ---- Phase 2C-2 F1/F3 helpers ---------------------------------------------
// Sender side: a SENT challenge to BOBKEY in `group`, stored by the actual
// sendChallenge(). Returns its message id.
static std::string storeSentChallenge(const char* group, const char* contact = "BOBKEY") {
  uint8_t secret[4] = {1, 2, 3, 4};
  sendChallenge(group, contact, secret);
  return T::store.back().env.message_id;
}
// The solver's (contact's) completed result, one chunk, via the actual
// handleGameResultChunkPacket().
static void deliverResult(const char* group, const std::string& msgId, uint16_t attempts, const char* solver = "BOBKEY") {
  FriendEntry e[3] = {{5678, 0, 0}, {1243, 2, 2}, {1234, 4, 0}};
  uint8_t buf[kChunkMaxPayload];
  size_t n = buildChunkPayload(msgId.c_str(), solver, 0, 1, attempts, e, 3, buf, sizeof(buf));
  handleGameResultChunkPacket(group, "msg/x", buf, n);
}
static int loadsFor(const char* gc) {
  int n = 0;
  for (auto& s : T::indexLoads) if (s == gc) n++;
  return n;
}


// ---- actual Text / Enigma chat screens ---------------------------------------
static void typeDot() {  // one real DOT keypress (press + 60 ms release), then a tick
  press(InputEventType::DOT_PRESS_START, T::now); T::now += 60; dot(T::now, 60); tick();
}
// Opens the actual chat through its real navigateToChatDirect(); returns the
// chat screen's handler (top of the real stack) for identity checks.
static ScreenHandlerFn openRealChat(bool enigma, const char* group = "GROUP1", const char* contact = "BOBKEY") {
  if (enigma) Enigma::navigateToChatDirect(group, contact); else TextMessage::navigateToChatDirect(group, contact);
  tick(3);
  return g_stack[g_stackSize - 1];
}
static int cursorRowY(int composeY) {  // the history selection cursor, if any (not the compose-row cursor)
  for (auto& d : T::screen) if (d.text == "#CURSOR" && d.y != composeY) return d.y;
  return -1;
}
static int composeCursorY() {
  int y = -1; for (auto& d : T::screen) if (d.text == "#CURSOR" && d.y > y) y = d.y; return y;
}

// =============================================================================
// Reproduction tests: observable behaviour only (compile on baseline too)
// =============================================================================
static void r_playDoubleConfirm() {
  g_test = "R1 chat Short opens Manual/Random directly; double DOT pushes one trampoline";
  resetAll(); openChat();
  shortP(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat>SetMR" && T::visible("Set Manually") && T::visible("Randomize") &&
            !T::visible("Open Chat"),
        "stack %s: %s", stackStr().c_str(), T::visibleJoined().c_str());
  dot(T::now); T::now += 1; dot(T::now); tick(4);
  CHECK(stackStr() == "Main>Chat>SetMR>Manual", "stack %s", stackStr().c_str());
}
static void r_cancel() {
  g_test = "R2 Back on the choice cancels to the originating chat";
  resetAll(); openChat();
  shortP(T::now); tick(2);
  back(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat", "stack %s", stackStr().c_str());
}
static void r_chatDoubleShort() {
  g_test = "R3 two empty-line Shorts in one Friend-chat batch open one choice";
  resetAll(); openChat();
  shortP(T::now); T::now += 1; shortP(T::now); tick(3);
  CHECK(stackStr() == "Main>Chat>SetMR", "stack %s", stackStr().c_str());
}
static void r_quickSwitchQueuedDot() {
  g_test = "R4 DOT released during the challenge send does not open Text";
  resetAll(); openChat(); toSetMR();
  dot(T::now); tick(3);  // Set Manually -> Manual entry
  CHECK(stackStr() == "Main>Chat>SetMR>Manual", "stack %s", stackStr().c_str());
  setDigits(5, 6, 7, 8);
  tick();
  T::duringPublish = [] { dot(T::now); T::now += 300; };
  dot(T::now); tick();   // send
  T::duringPublish = nullptr;
  tick(4);
  CHECK(count("nav:Text") == 0, "Quick Switch acted on a DOT pressed during the send");
  CHECK(stackStr() == "Main>Chat>QS", "stack %s", stackStr().c_str());
}
static void r_manualBackThenDot() {
  g_test = "R5 Back then DOT in one batch on Manual entry sends nothing";
  resetAll(); openChat(); toSetMR();
  dot(T::now); tick(3);
  setDigits(5, 6, 7, 8); tick();
  back(T::now); T::now += 1; dot(T::now); tick(3);
  CHECK(T::appendCalls == 0 && count("nextId") == 0 && pubCalls() == 0, "a challenge was sent after Back");
  CHECK(stackStr() == "Main>Chat>SetMR", "stack %s", stackStr().c_str());
}
static void r_randomQueuedDot() {
  g_test = "R6 Random confirm: a DOT queued before its first draw sends nothing";
  resetAll(); openChat(); toSetMR();
  rot(+1); tick();
  dot(T::now); T::now += 1; dot(T::now); tick(4);
  CHECK(count("nextId") == 0 && T::appendCalls == 0, "random challenge sent from a DOT pressed before the screen was shown");
  CHECK(stackStr() == "Main>Chat>SetMR>Random", "stack %s", stackStr().c_str());
}
static void r_solveDoubleShort() {
  g_test = "R7 two Shorts on a correct guess: one append/attempt/save/chunk set";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  CHECK(stackStr() == "Main>Chat>Answer", "stack %s", stackStr().c_str());
  setDigits(1, 2, 3, 4); tick();
  shortP(T::now); T::now += 1; shortP(T::now); tick(3);
  FriendGameState s = storedGame();
  CHECK(s.historyCount == 1 && s.totalAttempts == 1, "history %u attempts %u", s.historyCount, s.totalAttempts);
  CHECK(T::updateCalls == 1, "saves %d", T::updateCalls);
  CHECK(pubCalls() == 1, "chunk publishes %d", pubCalls());
  CHECK(stackStr() == "Main>Chat>Result", "stack %s", stackStr().c_str());
}
static void r_wrongGuessVisible() {
  g_test = "R8 wrong guess becomes a Solo-style history row; stale Shorts neither submit nor open Exit";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 4, 3); tick();
  T::duringUpdate = [] { shortP(T::now); T::now += 200; };
  shortP(T::now); T::now += 1; shortP(T::now); tick();
  T::duringUpdate = nullptr;
  tick(3);
  CHECK(T::rowAt(historyRowY(1)) == histRow(1243, 2, 2), "newest row above Guess: '%s' | %s",
        T::rowAt(historyRowY(1)).c_str(), T::visibleJoined().c_str());
  CHECK(T::rowAt(guessRowY()).rfind("Guess:", 0) == 0, "Guess row missing: '%s'", T::rowAt(guessRowY()).c_str());
  CHECK(stackStr() == "Main>Chat>Answer" && g_activeGame.historyCount == 1, "stale Short acted: %s, history %u",
        stackStr().c_str(), g_activeGame.historyCount);
}
static void r_reopenShowsHistory() {
  g_test = "R12 reopening an active game shows its stored guesses as history rows";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  const uint8_t guesses[3][4] = {{5, 6, 7, 8}, {1, 2, 4, 3}, {2, 1, 3, 4}};
  for (auto& g : guesses) { setDigits(g[0], g[1], g[2], g[3]); tick(); shortP(T::now); tick(2); }
  back(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat", "stack %s", stackStr().c_str());
  rot(-1); tick(); shortP(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat>Answer", "reopen: %s", stackStr().c_str());
  CHECK(T::rowAt(historyRowY(3)) == histRow(5678, 0, 0) && T::rowAt(historyRowY(2)) == histRow(1243, 2, 2) &&
            T::rowAt(historyRowY(1)) == histRow(2134, 2, 2),
        "rows: '%s' / '%s' / '%s'", T::rowAt(historyRowY(3)).c_str(), T::rowAt(historyRowY(2)).c_str(),
        T::rowAt(historyRowY(1)).c_str());
}
static void r_recipientLive() {
  g_test = "R13 open Friend recipient list follows presence without re-entry";
  resetAll(); strcpy(g_selectedGroupCode, "GRPR13");
  Menu::pushScreen(screenFriendRecipient); tick(2);
  CHECK(!T::visible("Bob"), "precondition");
  presenceMsg("GRPR13", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  CHECK(T::visible("Bob"), "new online contact not shown: %s", T::visibleJoined().c_str());
  T::unixTime += 30; presenceMsg("GRPR13", "BOBDEV000001", "Bob", "OFFLINE"); tick(2);
  CHECK(T::visible("Bob (offline)") && !T::visible("Bob"), "offline not shown: %s", T::visibleJoined().c_str());
  T::unixTime += 30; presenceMsg("GRPR13", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  CHECK(T::visible("Bob") && !T::visible("Bob (offline)"), "back online not shown: %s", T::visibleJoined().c_str());
  deleteGroup("GRPR13");
}
static void r_resultDotDuringSend() {
  g_test = "R9 DOT released during the chunk send does not dismiss the result";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 3, 4); tick();
  T::duringPublish = [] { dot(T::now); T::now += 300; };
  shortP(T::now); tick();
  T::duringPublish = nullptr;
  tick(3);
  CHECK(stackStr() == "Main>Chat>Result", "stack %s", stackStr().c_str());
}
static void r_resultDoubleConfirm() {
  g_test = "R10 double DOT on the result screen pops once";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 3, 4); tick();
  shortP(T::now); tick(3);
  dot(T::now); T::now += 1; dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Chat", "stack %s", stackStr().c_str());
}
static void r_recipientDirect() {
  g_test = "R14 choosing a friend opens Set Manually / Randomize / Open Chat directly";
  resetAll(); strcpy(g_selectedGroupCode, "GRPR14");
  Menu::pushScreen(screenFriendRecipient); tick(2);
  presenceMsg("GRPR14", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Recipient>SetMR" && T::visible("Set Manually") && T::visible("Randomize") &&
            T::visible("Open Chat"),
        "stack %s: %s", stackStr().c_str(), T::visibleJoined().c_str());
  dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Recipient>SetMR>Manual", "Set Manually: %s", stackStr().c_str());
  setDigits(5, 6, 7, 8); tick(); dot(T::now); tick(2);
  CHECK(stackStr() == "Main>Recipient>QS" && T::appendCalls == 1, "after send: %s", stackStr().c_str());
  back(T::now); tick(2);
  CHECK(stackStr() == "Main>Recipient", "Back from Quick Switch: %s", stackStr().c_str());
  deleteGroup("GRPR14");
}
static void r_senderChatRefresh() {
  g_test = "R16 sender's open Friend chat shows the completed result without re-entry";
  resetAll(); std::string id = storeSentChallenge("GROUP1"); openChat();
  CHECK(T::visibleJoined().find("waiting") != std::string::npos, "precondition: %s", T::visibleJoined().c_str());
  deliverResult("GROUP1", id, 3); tick(2);
  CHECK(T::visibleJoined().find("solved (3)") != std::string::npos, "still stale: %s", T::visibleJoined().c_str());
}
static void r_realChatQueuedShorts() {
  for (int m = 0; m < 2; m++) {
    const bool enigma = (m == 1);
    g_test = std::string("R17 ") + (enigma ? "Enigma" : "Text") + ": two Shorts in one batch open one challenge choice";
    resetAll(); ScreenHandlerFn chat = openRealChat(enigma);
    uint8_t depth = g_stackSize;
    shortP(T::now); T::now += 1; shortP(T::now); tick(3);
    CHECK(g_stackSize == depth + 1 && g_stack[depth - 1] == chat, "depth %u -> %u", depth, g_stackSize);
    g_test = std::string("R18 ") + (enigma ? "Enigma" : "Text") +
             ": Short then Back in one batch (pre-existing outgoing-loop path) ends on the chat, nothing sent";
    resetAll(); chat = openRealChat(enigma);
    depth = g_stackSize;
    shortP(T::now); T::now += 1; back(T::now); tick(3);
    CHECK(g_stackSize == depth && g_stack[depth - 1] == chat && TextMessage::isConversationOpen("GROUP1", "BOBKEY") &&
              T::appendCalls == 0,
          "depth %u top-is-chat %d open %d", g_stackSize, g_stack[g_stackSize - 1] == chat,
          TextMessage::isConversationOpen("GROUP1", "BOBKEY"));
  }
}
static void r_chatDoubleShortToAnswer() {
  g_test = "R11 two history Shorts in one chat batch open one Answer, no Exit prompt";
  resetAll(); receiveChallenge(); openChat();
  rot(-1); tick();
  shortP(T::now); T::now += 1; shortP(T::now); tick(3);
  CHECK(stackStr() == "Main>Chat>Answer", "stack %s", stackStr().c_str());
}

#ifndef BASELINE
// =============================================================================
// Menu: navigation epoch and opt-in tickOwned() (actual menu.cpp)
// =============================================================================
static void dummyA() {}
static void dummyB() {}
static void trampAB() { Menu::goBack(); Menu::pushScreen(dummyB); }
static std::vector<std::string> g_filterSeen;
static bool recordFilter(const InputEvent& e) {
  g_filterSeen.push_back(std::to_string(static_cast<int>(e.type)) + "@" + std::to_string(e.eventMs));
  return false;
}
static bool dropFirstDot(const InputEvent& e) {
  static bool dropped = false;
  if (e.eventMs == 0xFFFF) { dropped = false; return false; }
  if (!dropped && e.type == InputEventType::DOT_RELEASE) { dropped = true; return true; }
  return false;
}
static const SettingItem kAB[] = {{"A", dummyA}, {"B", dummyB}};
static const SettingItem kTramp[] = {{"T", trampAB}};

static void m_epoch() {
  g_test = "M1 navigationEpoch: actual stack changes only";
  resetAll();
  uint32_t e0 = Menu::navigationEpoch();
  Menu::init(); CHECK(Menu::navigationEpoch() == e0 + 1, "init");
  Menu::goBack(); CHECK(Menu::navigationEpoch() == e0 + 1, "goBack at root changed epoch");
  Menu::pushScreen(dummyA); CHECK(Menu::navigationEpoch() == e0 + 2, "push");
  Menu::pushScreen(nullptr); CHECK(Menu::navigationEpoch() == e0 + 2, "null push changed epoch");
  uint8_t depth = g_stackSize;
  uint32_t before = Menu::navigationEpoch();
  Menu::goBack(); Menu::pushScreen(dummyB);
  CHECK(g_stackSize == depth && Menu::navigationEpoch() != before, "goBack+push not detected");
  while (g_stackSize < kMaxStackDepth) Menu::pushScreen(dummyA);
  before = Menu::navigationEpoch();
  Menu::pushScreen(dummyB);
  CHECK(Menu::navigationEpoch() == before, "push at max depth changed epoch");
}
static void m_tickOwned() {
  g_test = "M2 tickOwned: filter once in FIFO order, stop after push, no draw, rest stays queued";
  resetAll();
  ListMenu lm; lm.configure(kAB, 2);
  lm.tickOwned("T", nullptr);  // first draw
  CHECK(lm.hasDrawnSinceConfigure(), "first draw not recorded");
  g_filterSeen.clear();
  rot(+1); dot(10); back(11); dot(12);
  size_t drawsBefore = T::drawLog.size();
  bool nav = lm.tickOwned("T", recordFilter);
  CHECK(nav, "navigation not reported");
  CHECK(g_filterSeen.size() == 2, "filter saw %zu events", g_filterSeen.size());
  CHECK(g_stack[g_stackSize - 1] == dummyB && g_stackSize == 2, "pushed %s depth %u", nameOf(g_stack[g_stackSize - 1]), g_stackSize);
  CHECK(T::drawLog.size() == drawsBefore, "outgoing list drew after navigating");
  CHECK(T::inputQ.size() == 2 && T::inputQ[0].type == InputEventType::ENCODER_LONG && T::inputQ[1].eventMs == 12,
        "remaining queue not intact/in order");

  g_test = "M3 tickOwned: Back stops; goBack at root does not count as navigation";
  resetAll();
  ListMenu lm2; lm2.configure(kAB, 2); lm2.tickOwned("T", nullptr);
  T::inputQ.clear();
  back(20); rot(+1);
  nav = lm2.tickOwned("T", nullptr);  // root: goBack is a no-op
  CHECK(!nav && T::inputQ.empty() && lm2.selectedIndex() == 1, "root Back stopped processing");
  Menu::pushScreen(dummyA);
  back(21); rot(+1);
  nav = lm2.tickOwned("T", nullptr);
  CHECK(nav && T::inputQ.size() == 1 && g_stackSize == 1, "Back did not stop (queue %zu)", T::inputQ.size());

  g_test = "M4 tickOwned: push of a trampoline that later keeps depth unchanged";
  resetAll();
  ListMenu lm3; lm3.configure(kTramp, 1); lm3.tickOwned("T", nullptr);
  dot(30); dot(31);
  nav = lm3.tickOwned("T", nullptr);
  CHECK(nav && g_stackSize == 2 && g_stack[1] == trampAB && T::inputQ.size() == 1, "double confirm pushed twice");

  g_test = "M5 tickOwned: a dropped event consumes only itself and keeps order";
  resetAll();
  ListMenu lm4; lm4.configure(kAB, 2); lm4.tickOwned("T", nullptr);
  InputEvent resetMarker(InputEventType::NONE, 0, 0, 0xFFFF); dropFirstDot(resetMarker);
  dot(40); rot(+1); dot(42);
  nav = lm4.tickOwned("T", dropFirstDot);
  CHECK(nav && g_stack[g_stackSize - 1] == dummyB, "drop reordered or lost events (top %s)", nameOf(g_stack[g_stackSize - 1]));

  g_test = "M6 legacy tick(): unchanged (still handles the whole batch)";
  resetAll();
  ListMenu lm5; lm5.configure(kAB, 2); lm5.tick("T");
  dot(50); dot(51);
  lm5.tick("T");
  CHECK(g_stackSize == 3, "legacy tick no longer double-pushes (depth %u)", g_stackSize);

  g_test = "M7 IN_PLACE selection does not stop tickOwned";
  resetAll();
  static int inPlaceCalls = 0; inPlaceCalls = 0;
  static const SettingItem kIP[] = {{"X", [] { inPlaceCalls++; }}};
  ListMenu lm6; lm6.configure(kIP, 1, nullptr, 0, ListMenu::SelectionMode::IN_PLACE); lm6.tickOwned("T", nullptr);
  dot(60); dot(61);
  nav = lm6.tickOwned("T", nullptr);
  CHECK(!nav && inPlaceCalls == 2, "IN_PLACE stopped (%d calls)", inPlaceCalls);
}

// =============================================================================
// Drawn-before guard: timestamps (actual guard code)
// =============================================================================
static InputEvent sAt(uint32_t at) { return InputEvent(InputEventType::ENCODER_SHORT, 0, 0, at); }
static void bindAt(DrawnBeforeGuard& g, uint32_t t) { g = DrawnBeforeGuard{}; armGuard(g); T::now = t; bindGuardAfterDraw(g); }

static void g_timestamps() {
  DrawnBeforeGuard g;
  g_test = "G1 UNTIL_DRAW drops everything; OFF drops nothing";
  g = DrawnBeforeGuard{}; T::now = 500;
  CHECK(!guardDrops(g, sAt(1)), "OFF dropped");
  armGuard(g); CHECK(guardDrops(g, sAt(499)) && guardDrops(g, sAt(600)), "UNTIL_DRAW passed an event");

  g_test = "G2 equal boundary is dropped; one ms later passes";
  bindAt(g, 1000); T::now = 1010;
  CHECK(guardDrops(g, sAt(1000)), "equal timestamp passed");
  CHECK(!guardDrops(g, sAt(1001)), "later press dropped");
  CHECK(guardDrops(g, sAt(999)), "earlier press passed");

  g_test = "G3 legitimate timestamp 0 (rollover) classified like any other value";
  bindAt(g, 0xFFFFFFF0u); T::now = 20;  // bound 16ms before rollover
  CHECK(!guardDrops(g, sAt(0)), "fresh press at eventMs 0 dropped");
  CHECK(guardDrops(g, sAt(0xFFFFFFF0u)), "boundary not dropped across wrap");
  CHECK(guardDrops(g, sAt(0xFFFFFF00u)), "pre-wrap stale press passed");
  bindAt(g, 5); T::now = 9;
  CHECK(guardDrops(g, sAt(0)), "stale press at eventMs 0 passed");

  g_test = "G4 delayed enqueue of an old timestamp after drains is still dropped";
  bindAt(g, 2000);
  for (int i = 0; i < 50; i++) { T::now += 7; tickGuard(g); }  // many empty drains
  CHECK(guardDrops(g, sAt(1990)), "late-published stale press passed");
  CHECK(!guardDrops(g, sAt(2001)), "fresh press dropped");
  CHECK(guardDrops(g, sAt(1995)), "a later stale press passed after a fresh one (no ordering assumed)");

  g_test = "G5 long idle around 2^31 and 2^32: no blackout for fresh presses";
  const uint32_t B = 0x12345678u;
  bindAt(g, B);
  const uint32_t stepMs = 1u << 20;
  uint64_t elapsed = 0;
  auto idleTo = [&](uint64_t target) { while (elapsed + stepMs <= target) { T::now += stepMs; elapsed += stepMs; tickGuard(g); }
                                       uint32_t rest = static_cast<uint32_t>(target - elapsed); T::now += rest; elapsed += rest; tickGuard(g); };
  const uint64_t marks[] = {0x7FFFFFFFull - 10, 0x80000000ull, 0x80000000ull + 10, 0xC0000000ull, 0xFFFFFFFFull - 5,
                            0x100000000ull, 0x100000000ull + 7, 0x180000000ull};
  for (uint64_t m : marks) {
    idleTo(m);
    bool fresh = !guardDrops(g, sAt(T::now - 3));
    CHECK(fresh, "fresh press dropped %llu ms after the bound", static_cast<unsigned long long>(m));
  }
  bindAt(g, B); elapsed = 0;
  idleTo(0x80000000ull + 10);
  CHECK(guardDrops(g, sAt(B - 1)), "stale press (age < 2^32) passed after 2^31 idle");
  CHECK(guardDrops(g, sAt(B)), "boundary passed after long idle");
}

// =============================================================================
// Friend transitions (candidate)
// =============================================================================
static void f_flowAdmission() {
  g_test = "F1 startChallengeFlow: duplicate ignored only until something navigates";
  resetAll(); openChat();
  invokeEmptyLineAction(0, "GROUP1", "BOBKEY");
  invokeEmptyLineAction(0, "GROUP1", "BOBKEY");  // same outgoing batch (Text/Enigma loop shape)
  CHECK(stackStr() == "Main>Chat>SetMR", "duplicate admitted: %s", stackStr().c_str());
  tick();  // the choice runs (no navigation)
  invokeEmptyLineAction(0, "GROUP1", "BOBKEY");
  CHECK(stackStr() == "Main>Chat>SetMR", "admitted while the choice is still top: %s", stackStr().c_str());
  Menu::goBack();  // e.g. Text's same-batch Back (documented remaining issue) or the choice's own Back
  CHECK(stackStr() == "Main>Chat", "stack %s", stackStr().c_str());
  invokeEmptyLineAction(0, "GROUP1", "BOBKEY");
  CHECK(stackStr() == "Main>Chat>SetMR", "new flow refused after navigation: %s", stackStr().c_str());

  g_test = "F1b failed push (stack full) does not block later flows";
  resetAll();
  while (g_stackSize < kMaxStackDepth) Menu::pushScreen(screenFriendChat);
  invokeEmptyLineAction(0, "GROUP1", "BOBKEY");
  CHECK(!g_challengeFlowPushed, "flag set without a push");
  Menu::goBack();
  invokeEmptyLineAction(0, "GROUP1", "BOBKEY");
  CHECK(g_stack[g_stackSize - 1] == screenSetManuallyOrRandom, "later flow blocked");
}
static void f_cancelQueued() {
  g_test = "F2 Back on the choice with a queued Back: the choice stops, the chat gets the second Back";
  resetAll(); openChat();
  shortP(T::now); tick(2);
  back(T::now); T::now += 400; back(T::now); tick();
  CHECK(stackStr() == "Main>Chat" && T::inputQ.size() == 1 && T::inputQ[0].type == InputEventType::ENCODER_LONG,
        "stack %s queue %zu", stackStr().c_str(), T::inputQ.size());
  tick();
  CHECK(stackStr() == "Main", "queued Back not delivered to the chat: %s", stackStr().c_str());
  g_test = "F2b Back on the choice then a queued DOT: chat ignores it, flow not restarted";
  resetAll(); openChat();
  shortP(T::now); tick(2);
  back(T::now); T::now += 1; dot(T::now); tick(4);
  CHECK(stackStr() == "Main>Chat", "stack %s", stackStr().c_str());
}
static void f_menuGuard() {
  g_test = "F3 choice: DOT released before its first draw is dropped; later DOT acts";
  resetAll(); openChat();
  shortP(T::now); T::now += 1; dot(T::now); tick();  // Short opens the choice; DOT stays queued (chat stops)
  CHECK(stackStr() == "Main>Chat>SetMR", "stack %s", stackStr().c_str());
  tick(2);
  CHECK(stackStr() == "Main>Chat>SetMR", "pre-draw DOT acted: %s", stackStr().c_str());
  uint32_t drewAt = lastDrawTime("Number Guessing");
  dot(drewAt); tick();
  CHECK(stackStr() == "Main>Chat>SetMR", "equal-boundary DOT acted");
  dot(drewAt + 1); tick(3);
  CHECK(stackStr() == "Main>Chat>SetMR>Manual", "later DOT ignored: %s", stackStr().c_str());
}
static void f_challengeOutcomes() {
  struct Case { bool connected; bool pub; bool store; const char* title; bool pending; };
  const Case cases[] = {
      {true, true, true, "Server acknowledged", false},  {true, true, false, "Server acked; not saved", false},
      {true, false, true, "Unconfirmed: queued", true},  {true, false, false, "Unconfirmed; not saved", true},
      {false, true, true, "Offline: queued", true},      {false, true, false, "Offline; not saved", true},
  };
  for (const Case& c : cases) {
    g_test = std::string("F4 challenge outcome: ") + c.title;
    resetAll(); openChat(); toSetMR();
    dot(T::now); tick(3);
    setDigits(5, 6, 7, 8); tick();
    T::connected = c.connected; T::publishScript = {c.pub}; T::appendResult = c.store;
    size_t seqStart = T::seq.size();
    dot(T::now); tick(3);
    CHECK(stackStr() == "Main>Chat>QS", "stack %s", stackStr().c_str());
    CHECK(T::visible(c.title), "title missing: %s", T::visibleJoined().c_str());
    CHECK(!T::visible("Challenge Sent"), "unconditional title still shown");
    CHECK(pubCalls() == (c.connected ? 1 : 0), "publish calls %d", pubCalls());
    if (c.store) CHECK(!T::store.empty() && ((T::store.back().hdr.flags & MessageStore::FLAG_PENDING_OUTBOX) != 0) == c.pending,
                       "pending flag wrong");
    int iSend = seqIndex("D:Sending...", static_cast<int>(seqStart)), iId = seqIndex("nextId", static_cast<int>(seqStart));
    int iApp = seqIndex("append", static_cast<int>(seqStart));
    CHECK(iSend >= 0 && iSend < iId && iId < iApp, "Sending... not drawn before nextId/publish/store");
  }
  g_test = "F4b encode failure title (not reachable with the fixed buffers; table entry checked directly)";
  ChallengeSendOutcome enc = {false, PublishAttempt::NOT_ATTEMPTED, false};
  CHECK(!strcmp(challengeOutcomeTitle(enc), "Not sent: encode failed"), "encode failure title");
}
static void f_quickSwitch() {
  g_test = "F5 Quick Switch: stale DOT (incl. late-published) dropped; later DOT opens Text";
  resetAll(); openChat(); toSetMR();
  dot(T::now); tick(3);
  setDigits(5, 6, 7, 8); tick();
  uint32_t pressedDuringSend = 0;
  T::duringPublish = [&] { pressedDuringSend = T::now; dot(T::now); T::now += 300; };
  dot(T::now); tick();
  T::duringPublish = nullptr;
  tick(3);
  CHECK(stackStr() == "Main>Chat>QS" && count("nav:Text") == 0, "queued DOT acted: %s", stackStr().c_str());
  dot(pressedDuringSend + 1); tick(2);  // old timestamp published after Quick Switch drew
  CHECK(count("nav:Text") == 0, "late-published stale DOT acted");
  dot(T::now); tick(3);
  CHECK(g_stackSize == 3 && nameOf(g_stack[0]) == std::string("Main") && nameOf(g_stack[2]) == std::string("?"),
        "fresh DOT did not open the Text chat: %s", stackStr().c_str());  // actual TextMessage::navigateToChatDirect()
}
static void f_manualGuard() {
  g_test = "F6 Manual: stale release reaches digit entry but never sends; fresh one sends";
  resetAll(); openChat(); toSetMR();
  dot(T::now); tick(3);
  uint32_t drawnAt = T::now - 5;
  setDigits(5, 6, 7, 8); tick();
  press(InputEventType::DOT_PRESS_START, drawnAt - 90); tick();
  CHECK(g_digitEntry.dotHeld, "press start not delivered");
  dot(drawnAt - 2); tick(2);
  CHECK(T::appendCalls == 0 && count("nextId") == 0, "stale DOT sent the challenge");
  CHECK(!g_digitEntry.dotHeld && g_digitEntry.count == 4, "stale release did not reach digit entry");
  dot(T::now); tick(2);
  CHECK(count("nextId") == 1 && T::appendCalls == 1, "fresh DOT did not send");
}
static void f_random() {
  g_test = "F7 Random: text/secret policy unchanged; equal boundary dropped; later sends";
  resetAll(); openChat(); toSetMR();
  rot(+1); tick();
  dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Chat>SetMR>Random", "stack %s", stackStr().c_str());
  CHECK(T::visible("Random secret ready") && T::visible("DOT: send"), "text changed: %s", T::visibleJoined().c_str());
  char secret[8]; snprintf(secret, sizeof(secret), "%u%u%u%u", g_pendingRandomSecret[0], g_pendingRandomSecret[1],
                           g_pendingRandomSecret[2], g_pendingRandomSecret[3]);
  CHECK(T::visibleJoined().find(secret) == std::string::npos, "secret displayed");
  uint32_t drewAt = lastDrawTime("DOT: send");
  dot(drewAt); tick(2);
  CHECK(count("nextId") == 0, "equal-boundary DOT sent");
  dot(drewAt + 1); tick(2);
  CHECK(count("nextId") == 1, "later DOT did not send");
  g_test = "F7b Random: Back then DOT sends nothing";
  resetAll(); openChat(); toSetMR();
  rot(+1); tick(); dot(T::now); tick(3);
  back(T::now); T::now += 1; dot(T::now); tick(3);
  CHECK(count("nextId") == 0 && stackStr() == "Main>Chat>SetMR", "stack %s", stackStr().c_str());
}
static void f_submitPrecondition() {
  g_test = "F8 submitAnswerGuess(): LOCKED + four digits required before evaluation";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 4, 3); g_digitEntry.count = 3;
  CHECK(!submitAnswerGuess() && g_activeGame.historyCount == 0, "evaluated with three digits");
  setDigits(1, 2, 3, 4); tick();
  shortP(T::now); tick(3);
  FriendGameState before = g_activeGame;
  setDigits(9, 8, 7, 6);
  CHECK(!submitAnswerGuess(), "UNLOCKED game evaluated again");
  CHECK(g_activeGame.historyCount == before.historyCount && g_activeGame.totalAttempts == before.totalAttempts &&
            T::updateCalls == 1 && pubCalls() == 1,
        "completed game changed");
}
static void f_solveFeedback() {
  g_test = "F9 solve: Saving/Sending drawn before each blocking step; one of each operation";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 3, 4); tick();
  size_t s0 = T::seq.size();
  shortP(T::now); tick();
  int iSave = seqIndex("D:Saving...", static_cast<int>(s0)), iUpd = seqIndex("update", static_cast<int>(s0));
  int iSend = seqIndex("D:Sending result...", static_cast<int>(s0)), iPub = seqIndex("publish:true", static_cast<int>(s0));
  CHECK(iSave >= 0 && iSave < iUpd && iUpd < iSend && iSend < iPub, "order %d %d %d %d", iSave, iUpd, iSend, iPub);
  CHECK(T::updateCalls == 1 && pubCalls() == 1 && g_activeGame.totalAttempts == 1, "counts");
  tick();
  CHECK(T::visible("Total attempts: 1") && !g_resultStatusValid, "result: %s", T::visibleJoined().c_str());
  CHECK(visibleY("1234 4A0B") == 16 + 23, "F2: routine success must add no status row (first row y=%d)",
        visibleY("1234 4A0B"));
}
static void f_resultOutcomes() {
  struct Case { const char* name; bool connected; std::deque<bool> script; bool save; int extra; const char* text; int calls; };
  const Case cases[] = {
      {"offline", false, {}, true, 0, "Result not sent (offline)", 0},
      {"offline, save failed", false, {}, false, 0, "Not saved; result not sent", 0},
      {"1 chunk acked, save failed", true, {true}, false, 0, "Not saved on this device", 1},
      {"2 chunks, second false", true, {true, false}, true, 24, "Result not confirmed", 2},
      {"2 chunks, first false closes connection", true, {false}, true, 24, "Result not confirmed", 2},
      {"2 chunks, second false, save failed", true, {true, false}, false, 24, "Not saved; not confirmed", 2},
  };
  for (const Case& c : cases) {
    g_test = std::string("F10 result outcome: ") + c.name;
    resetAll(); receiveChallenge(); openChat(); openAnswer();
    for (int i = 0; i < c.extra; i++) appendFriendEntry(&g_activeGame, 5678, 0, 0);
    setDigits(1, 2, 3, 4); tick();
    T::connected = c.connected; T::publishScript = c.script; T::updateResult = c.save;
    shortP(T::now); tick(2);
    CHECK(T::visible(c.text), "status missing: %s", T::visibleJoined().c_str());
    CHECK(pubCalls() == c.calls, "publishBinary calls %d", pubCalls());
    CHECK(g_resultStatus.send.calledMask == (c.calls ? (1u << c.calls) - 1 : 0u) && g_resultStatus.send.skippedMask == 0 &&
              g_resultStatus.send.connectedAtStart == c.connected,
          "outcome masks");
  }
  g_test = "F10b skipped chunk (encode failure) wording, and routine success shows nothing";
  ResultViewStatus st{true, {true, 2, 2, 1, 1}};
  char buf[40];
  CHECK(formatResultStatus(st, buf, sizeof(buf)) && !strcmp(buf, "Result could not be sent"), "%s", buf);
  st.saved = false;
  CHECK(formatResultStatus(st, buf, sizeof(buf)) && !strcmp(buf, "Not saved; result not sent"), "%s", buf);
  ResultViewStatus ok{true, {true, 3, 0, 7, 7}};
  CHECK(!formatResultStatus(ok, buf, sizeof(buf)) && buf[0] == '\0', "success produced '%s'", buf);
  g_test = "F10c 3 chunks, all published with a true return: no status row";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  for (int i = 0; i < 45; i++) appendFriendEntry(&g_activeGame, 5678, 0, 0);
  setDigits(1, 2, 3, 4); tick();
  T::publishScript = {true, true, true};
  shortP(T::now); tick(2);
  CHECK(pubCalls() == 3 && !g_resultStatusValid && T::visibleJoined().find("Result") == std::string::npos &&
            T::visibleJoined().find("saved") == std::string::npos,
        "%s", T::visibleJoined().c_str());
}
static void f_wrongGuess() {
  g_test = "F11 wrong guess: Saving... with the new row before the save; Guess row back; stale Shorts dropped";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 4, 3); tick();
  uint32_t pressedDuringSave = 0;
  T::duringUpdate = [&] { pressedDuringSave = T::now; shortP(T::now); T::now += 1; dot(T::now); T::now += 200; };
  size_t s0 = T::seq.size();
  shortP(T::now); T::now += 1; shortP(T::now); tick();
  T::duringUpdate = nullptr;
  int iRow = seqIndex("D:2A2B", static_cast<int>(s0)), iSave = seqIndex("D:Saving...", static_cast<int>(s0));
  int iUpd = seqIndex("update", static_cast<int>(s0));
  CHECK(iRow >= 0 && iSave >= 0 && iRow < iUpd && iSave < iUpd, "row/Saving... not drawn before the save (%d %d %d)",
        iRow, iSave, iUpd);
  CHECK(!T::visible("Saving...") && T::rowAt(guessRowY()).rfind("Guess:", 0) == 0, "Guess row not restored: %s",
        T::visibleJoined().c_str());
  CHECK(!T::visible("Short: continue") && T::visibleJoined().find("Attempt") == std::string::npos, "old result frame");
  tick(3);
  CHECK(stackStr() == "Main>Chat>Answer" && g_activeGame.historyCount == 1, "Short from the save acted");
  CHECK(g_digitEntry.count == 1, "D-b: DOT during the save did not enter a digit (count %u)", g_digitEntry.count);
  shortP(pressedDuringSave); tick();  // published late
  CHECK(stackStr() == "Main>Chat>Answer", "late-published stale Short opened Exit");
  shortP(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat>Answer>Prompt", "fresh Short with an incomplete entry did not open Exit: %s",
        stackStr().c_str());

  g_test = "F11b failed save: notice stays until a later save succeeds";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 4, 3); tick();
  T::updateResult = false;
  shortP(T::now); tick(2);
  int noticeY = Display::kStatusBarHeight + 2;
  CHECK(T::rowAt(noticeY) == "Last guess not saved" && T::rowAt(historyRowY(1)) == histRow(1243, 2, 2),
        "%s", T::visibleJoined().c_str());
  setDigits(5, 6, 7, 8); tick(); shortP(T::now); tick(2);
  CHECK(T::visible("Last guess not saved"), "notice cleared by another failed save");
  T::updateResult = true;
  setDigits(2, 1, 3, 4); tick(); shortP(T::now); tick(2);
  CHECK(!T::visible("Last guess not saved") && T::rowAt(historyRowY(1)) == histRow(2134, 2, 2), "%s",
        T::visibleJoined().c_str());
}
static void f_historyLayout() {
  g_test = "F22 Solo capacity from geometry: newest N rows in order directly above Guess";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  const int cap = maxHistoryRows(false);
  CHECK(cap >= 1, "no history rows fit");
  for (int i = 0; i < cap + 3; i++) appendFriendEntry(&g_activeGame, static_cast<uint16_t>(5670 + i), 0, 0);
  g_answerNeedsFullRedraw = true; g_answerDirty = true; tick();
  for (int k = 1; k <= cap; k++) {
    uint16_t v = static_cast<uint16_t>(5670 + cap + 3 - k);
    CHECK(T::rowAt(historyRowY(k)) == histRow(v, 0, 0), "row %d: '%s'", k, T::rowAt(historyRowY(k)).c_str());
  }
  CHECK(T::rowAt(historyRowY(cap + 1)).empty(), "row above capacity drawn");
  g_test = "F23 ring wrap (50-entry history ring): newest rows in chronological order";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  for (int i = 0; i < 57; i++) appendFriendEntry(&g_activeGame, static_cast<uint16_t>(1000 + i), 0, 0);
  CHECK(g_activeGame.historyCount == 50 && g_activeGame.totalAttempts == 57, "ring precondition");
  g_answerNeedsFullRedraw = true; g_answerDirty = true; tick();
  for (int k = 1; k <= cap; k++) {
    CHECK(T::rowAt(historyRowY(k)) == histRow(static_cast<uint16_t>(1057 - k), 0, 0), "row %d: '%s'", k,
          T::rowAt(historyRowY(k)).c_str());
  }
  g_test = "F24 the secret is never drawn while the game is active";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  const uint8_t g4[4][4] = {{5, 6, 7, 8}, {1, 2, 4, 3}, {4, 3, 2, 1}, {2, 1, 3, 4}};
  for (auto& g : g4) { setDigits(g[0], g[1], g[2], g[3]); tick(); shortP(T::now); tick(2); }
  bool leaked = false;
  for (auto& d : T::drawLog) if (d.text == "4A0B" || d.text.find("1234") != std::string::npos) leaked = true;
  CHECK(!leaked && g_activeGame.state == STATE_LOCKED, "secret drawn during play");
  g_test = "F25 notice row reduces visible history by one row";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  for (int i = 0; i < cap + 2; i++) appendFriendEntry(&g_activeGame, static_cast<uint16_t>(3000 + i), 0, 0);
  setDigits(5, 6, 7, 8); tick();
  T::updateResult = false; shortP(T::now); tick(2);
  CHECK(T::rowAt(historyRowY(maxHistoryRows(true))) == histRow(static_cast<uint16_t>(3000 + cap + 2 - maxHistoryRows(true) + 1), 0, 0) &&
            T::rowAt(historyRowY(maxHistoryRows(true) + 1)).empty() &&
            T::rowAt(Display::kStatusBarHeight + 2) == "Last guess not saved",
        "%s", T::visibleJoined().c_str());
}
static void f_resultScreen() {
  g_test = "F12 result screen: scrolling and Back preserved; status bound to the solve";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  for (int i = 0; i < 6; i++) appendFriendEntry(&g_activeGame, static_cast<uint16_t>(5670 + i), 0, 0);
  setDigits(1, 2, 3, 4); tick();
  T::connected = false;  // a status exists, so F13 below checks it is not inherited
  shortP(T::now); tick(2);
  T::connected = true;
  CHECK(g_resultStatusValid, "F12 precondition: status shown after an offline solve");
  uint16_t scroll = g_friendResultScroll;
  rot(-1); tick();
  CHECK(g_friendResultScroll == scroll - 1, "scroll ignored");
  back(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat", "Back: %s", stackStr().c_str());
  g_test = "F13 reopened result carries no send status";
  rot(-1); tick();
  shortP(T::now); tick(2);  // focus the (now UNLOCKED) game again
  CHECK(stackStr() == "Main>Chat>Result", "reopen: %s", stackStr().c_str());
  CHECK(!g_resultStatusValid && T::visibleJoined().find("Result") == std::string::npos, "status inherited: %s",
        T::visibleJoined().c_str());
  CHECK(visibleY("Total attempts: 7") == 16, "title");

  g_test = "F13b reopen after the result was left by a stack reset (no exit clear) shows no status";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 3, 4); tick();
  T::connected = false;  // so the solve has a (failure) status that a reopen could wrongly inherit
  shortP(T::now); tick(2);
  T::connected = true;
  CHECK(g_resultStatusValid && T::visible("Result not sent (offline)"), "solve status missing");
  Menu::init(); g_stackSize = 1;  // e.g. another screen resets the stack
  openChat(); rot(-1); tick();
  shortP(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat>Result" && T::visibleJoined().find("Result") == std::string::npos,
        "reopened view inherited a RAM-only status: %s", T::visibleJoined().c_str());
}
static void f_answerBoundaries() {
  g_test = "F14 Exit prompt: Answer stops after the push; DOT left for the prompt";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  g_digitEntry.count = 2; tick();
  shortP(T::now); T::now += 1; dot(T::now); tick();
  CHECK(stackStr() == "Main>Chat>Answer>Prompt" && T::inputQ.size() == 1, "stack %s queue %zu", stackStr().c_str(),
        T::inputQ.size());
  g_test = "F15 Back then Short: Short goes to the chat (user order preserved)";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  back(T::now); tick();
  CHECK(stackStr() == "Main>Chat", "stack %s", stackStr().c_str());
  g_test = "F16 replacing the active game resets the shown save status";
  g_answerSaved = false;
  receiveChallenge("CHAL0000000000000000002"); markIndexDirty(); tick();
  rot(-1); tick();
  shortP(T::now); tick(2);
  CHECK(g_answerSaved && !strcmp(g_activeChallengeMessageId, "CHAL0000000000000000002"), "not replaced/reset");
}
// A press released while the first draw is still in progress (after some of
// it reached the panel) is still "before the draw completed" and is dropped.
static int g_hookFired = 0;
static void f_bindAfterDraw() {
  g_test = "F18 menu guard bound only after the first draw completes";
  resetAll(); openChat();
  g_hookFired = 0;
  T::onDraw = [](const std::string& t) { if (t == "Number Guessing") { g_hookFired++; T::now += 3; dot(T::now); T::now += 3; } };
  shortP(T::now); tick(2);
  T::onDraw = nullptr;
  CHECK(g_hookFired == 1, "draw hook fired %d times (vacuous test)", g_hookFired);
  tick(3);
  CHECK(stackStr() == "Main>Chat>SetMR", "DOT released during the first draw acted: %s", stackStr().c_str());
  dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Chat>SetMR>Manual", "later DOT ignored: %s", stackStr().c_str());

  g_test = "F19 answer guard bound only after the real post-submit frame is drawn";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 4, 3); tick();
  g_hookFired = 0;
  T::onDraw = [](const std::string& t) { if (t == "Guess: " && T::updateCalls == 1) { g_hookFired++; T::now += 3; shortP(T::now); T::now += 3; } };
  shortP(T::now); tick();
  T::onDraw = nullptr;
  CHECK(g_hookFired == 1, "draw hook fired %d times (vacuous test)", g_hookFired);
  tick(3);
  CHECK(stackStr() == "Main>Chat>Answer", "Short released during the post-submit draw opened Exit: %s", stackStr().c_str());
  shortP(T::now); tick();
  CHECK(stackStr() == "Main>Chat>Answer>Prompt", "later Short ignored: %s", stackStr().c_str());

  g_test = "F20 random guard bound only after its first draw completes";
  resetAll(); openChat(); toSetMR();
  rot(+1); tick();
  g_hookFired = 0;
  T::onDraw = [](const std::string& t) { if (t == "Random secret ready") { g_hookFired++; T::now += 3; dot(T::now); T::now += 3; } };
  dot(T::now); tick(3);
  T::onDraw = nullptr;
  CHECK(g_hookFired == 1, "draw hook fired %d times (vacuous test)", g_hookFired);
  tick(2);
  CHECK(count("nextId") == 0, "DOT released during the first draw sent");
  dot(T::now); tick(2);
  CHECK(count("nextId") == 1, "later DOT did not send");

  g_test = "F21 result guard bound only after the result's first draw";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(1, 2, 3, 4); tick();
  g_hookFired = 0;
  T::onDraw = [](const std::string& t) { if (t == "Total attempts: 1") { g_hookFired++; T::now += 3; dot(T::now); T::now += 3; } };
  shortP(T::now); tick(2);
  T::onDraw = nullptr;
  CHECK(g_hookFired == 1, "draw hook fired %d times (vacuous test)", g_hookFired);
  tick(2);
  CHECK(stackStr() == "Main>Chat>Result", "DOT released during the result's first draw dismissed it");
}

// =============================================================================
// Presence change generation (actual presence.cpp) and live recipient list
// =============================================================================
static uint32_t gen() { return Presence::changeGeneration(); }
static void p_generation() {
  const char* G = "GRPGEN";
  g_test = "P1 changeGeneration: exposed changes only";
  uint32_t g0 = gen();
  presenceMsg(G, "ANNDEV000001", "Ann", "ONLINE");
  CHECK(gen() != g0, "new online contact did not bump"); uint32_t g1 = gen();
  presenceMsg(G, "ANNDEV000001", "Ann", "ONLINE");
  CHECK(gen() == g1, "identical repeat (same second) bumped");
  presenceMsg(G, "ANNDEV000001", "Anna", "ONLINE");
  CHECK(gen() != g1, "name change did not bump"); g1 = gen();
  presenceMsg(G, "ANNDEV000001", "Anna", "ONLINE", 1);
  CHECK(gen() != g1, "radio change did not bump"); g1 = gen();
  T::unixTime += 5; presenceMsg(G, "ANNDEV000001", "Anna", "ONLINE", 1);
  CHECK(gen() != g1, "newer last-seen timestamp did not bump"); g1 = gen();
  presenceMsg(G, "DEADBEEFAAAA", "Me", "ONLINE");  // our own id (Identity::deviceId() stub) is ignored
  CHECK(gen() == g1, "own presence bumped");
  T::unixTime += 5; presenceMsg(G, "ANNDEV000001", "Anna", "OFFLINE");
  CHECK(gen() != g1, "online -> offline did not bump"); g1 = gen();
  presenceMsg(G, "ANNDEV000001", "Anna", "OFFLINE");
  CHECK(gen() == g1, "identical OFFLINE repeat bumped");
  deleteGroup(G);
  CHECK(gen() != g1, "group delete did not bump"); g1 = gen();
  deleteGroup(G);
  CHECK(gen() == g1, "deleting an absent group bumped");
  g_test = "P1c ONLINE then OFFLINE in the same second (no last-seen change) still bumps";
  presenceMsg(G, "EDDEV0000001", "Ed", "ONLINE"); g1 = gen();
  presenceMsg(G, "EDDEV0000001", "Ed", "OFFLINE");
  CHECK(gen() != g1, "online -> offline within the same second did not bump");
  deleteGroup(G);
  g_test = "P1b serviceInit (Recent Contacts loaded) bumps";
  g1 = gen(); T_initServices(); CHECK(gen() != g1, "load did not bump");
}
static std::string rowKey(uint8_t i) { return i < g_recipientItemCount ? g_recipientContactKeys[i] : std::string("-"); }
static std::string selectedKey() { return rowKey(g_recipientListMenu.selectedIndex()); }
static void enterRecipients(const char* group) {
  resetAll(); strcpy(g_selectedGroupCode, group);
  Menu::pushScreen(screenFriendRecipient); tick(2);
}
static void p_recipients() {
  g_test = "P2 rename updates the open list";
  enterRecipients("GRPP2");
  presenceMsg("GRPP2", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  presenceMsg("GRPP2", "BOBDEV000001", "Robert", "ONLINE"); tick(2);
  CHECK(T::visible("Robert") && !T::visible("Bob"), "%s", T::visibleJoined().c_str());
  deleteGroup("GRPP2");

  g_test = "P3 unchanged presence: no rebuild redraw (same second, newer second, other group)";
  enterRecipients("GRPP3");
  presenceMsg("GRPP3", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  size_t draws = T::drawLog.size();
  presenceMsg("GRPP3", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  CHECK(T::drawLog.size() == draws, "identical repeat redrew (%zu -> %zu)", draws, T::drawLog.size());
  uint32_t g1 = gen(); T::unixTime += 7; presenceMsg("GRPP3", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  CHECK(gen() != g1 && T::drawLog.size() == draws, "timestamp-only change redrew the list");
  presenceMsg("GRPOTHER", "ZEDDEV000001", "Zed", "ONLINE"); tick(2);
  CHECK(T::drawLog.size() == draws && !T::visible("Zed"), "other group's presence changed the list");
  deleteGroup("GRPP3"); deleteGroup("GRPOTHER");

  g_test = "P4 insertion ahead of the selection keeps the contact; queued confirm dropped; fresh one opens it";
  enterRecipients("GRPP4");
  presenceMsg("GRPP4", "BOBDEV000001", "Bob", "ONLINE");
  presenceMsg("GRPP4", "CYDEV0000001", "Cy", "ONLINE"); tick(2);
  CHECK(selectedKey() == "BOBDEV000001", "precondition: %s", selectedKey().c_str());
  dot(T::now); presenceMsg("GRPP4", "ALDEV0000001", "Al", "ONLINE"); tick(3);
  CHECK(rowKey(0) == "ALDEV0000001" && selectedKey() == "BOBDEV000001", "highlight moved: %s", selectedKey().c_str());
  CHECK(stackStr() == "Main>Recipient", "queued confirm acted after the refresh: %s", stackStr().c_str());
  dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Recipient>SetMR" && !strcmp(g_selectedContactKey, "BOBDEV000001"), "%s %s", stackStr().c_str(),
        g_selectedContactKey);
  deleteGroup("GRPP4");

  g_test = "P4b queued rotate+confirm across an insertion between rows: confirm dropped, highlight visible first";
  enterRecipients("GRPP4B");
  presenceMsg("GRPP4B", "BOBDEV000001", "Bob", "ONLINE");
  presenceMsg("GRPP4B", "CYDEV0000001", "Cy", "ONLINE"); tick(2);
  rot(+1); dot(T::now); presenceMsg("GRPP4B", "BONDEV000001", "Bon", "ONLINE"); tick(3);
  CHECK(stackStr() == "Main>Recipient" && selectedKey() == "BONDEV000001", "stack %s sel %s", stackStr().c_str(),
        selectedKey().c_str());
  deleteGroup("GRPP4B");

  g_test = "P5 selected contact goes offline: same contact stays selected";
  enterRecipients("GRPP5");
  presenceMsg("GRPP5", "ALDEV0000001", "Al", "ONLINE");
  presenceMsg("GRPP5", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  rot(+1); tick();
  CHECK(selectedKey() == "BOBDEV000001", "precondition");
  T::unixTime += 30; presenceMsg("GRPP5", "BOBDEV000001", "Bob", "OFFLINE"); tick(2);
  CHECK(T::visible("Bob (offline)") && selectedKey() == "BOBDEV000001", "sel %s", selectedKey().c_str());
  dot(T::now); tick(3);
  CHECK(!strcmp(g_selectedContactKey, "BOBDEV000001"), "opened %s", g_selectedContactKey);
  deleteGroup("GRPP5");

  g_test = "P6 selected contact evicted (no longer listed): queued confirm not applied to the replacement row";
  enterRecipients("GRPP6");
  char id[16], name[16];
  for (int i = 0; i < 20; i++) {
    snprintf(id, sizeof(id), "OLD%09d", i); snprintf(name, sizeof(name), "Old%02d", i);
    T::unixTime += 1; presenceMsg("GRPP6", id, name, "OFFLINE");
  }
  tick(2);
  for (uint8_t i = 0; i < g_recipientItemCount; i++) if (rowKey(i) == "OLD000000000") { while (selectedKey() != "OLD000000000") { rot(+1); tick(); } }
  CHECK(selectedKey() == "OLD000000000", "precondition: %s", selectedKey().c_str());
  T::unixTime += 1; dot(T::now); presenceMsg("GRPP6", "NEWDEV000001", "New", "ONLINE"); tick(3);
  bool stillListed = false;
  for (uint8_t i = 0; i < g_recipientItemCount; i++) if (rowKey(i) == "OLD000000000") stillListed = true;
  CHECK(!stillListed && g_recipientListMenu.selectedIndex() == 0, "eviction precondition / fallback row");
  CHECK(stackStr() == "Main>Recipient", "queued confirm applied to replacement row: %s", stackStr().c_str());
  deleteGroup("GRPP6");

  g_test = "P7 empty -> non-empty with a queued confirm: dropped; fresh confirm opens the new contact";
  enterRecipients("GRPP7");
  CHECK(g_recipientItemCount == 0, "precondition");
  dot(T::now); presenceMsg("GRPP7", "BOBDEV000001", "Bob", "ONLINE"); tick(3);
  CHECK(stackStr() == "Main>Recipient" && T::visible("Bob"), "%s", stackStr().c_str());
  dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Recipient>SetMR" && !strcmp(g_selectedContactKey, "BOBDEV000001"), "%s", stackStr().c_str());
  deleteGroup("GRPP7");

  g_test = "P8 non-empty -> empty (group deleted while open): rows removed, confirm does nothing, Back works";
  enterRecipients("GRPP8");
  presenceMsg("GRPP8", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  deleteGroup("GRPP8"); tick(2);
  CHECK(g_recipientItemCount == 0 && !T::visible("Bob") && T::visible("Play with Friend"), "%s", T::visibleJoined().c_str());
  dot(T::now); tick(2);
  CHECK(stackStr() == "Main>Recipient", "confirm on empty list navigated");
  back(T::now); tick(2);
  CHECK(stackStr() == "Main", "Back failed: %s", stackStr().c_str());

  g_test = "P9 Back in the same tick as a refresh: navigates, list not redrawn after";
  enterRecipients("GRPP9");
  presenceMsg("GRPP9", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  back(T::now); presenceMsg("GRPP9", "ALDEV0000001", "Al", "ONLINE");
  size_t before = T::drawLog.size(); tick();
  CHECK(stackStr() == "Main" && T::drawLog.size() == before, "stack %s draws +%zu", stackStr().c_str(),
        T::drawLog.size() - before);
  deleteGroup("GRPP9");

  g_test = "P10 confirm then rotate in one batch: the confirmed contact opens (stop-on-navigation)";
  enterRecipients("GRPP10");
  presenceMsg("GRPP10", "ALDEV0000001", "Al", "ONLINE");
  presenceMsg("GRPP10", "BOBDEV000001", "Bob", "ONLINE"); tick(2);
  dot(T::now); rot(+1); tick(3);
  CHECK(stackStr() == "Main>Recipient>SetMR" && !strcmp(g_selectedContactKey, "ALDEV0000001"), "%s %s", stackStr().c_str(),
        g_selectedContactKey);
  deleteGroup("GRPP10");
}

static void f1_entryFlows() {
  g_test = "F1a Open Chat opens the existing conversation; Back returns to the recipient list";
  resetAll(); strcpy(g_selectedGroupCode, "GROUP1");
  presenceMsg("GROUP1", "BOBKEY", "Bob", "ONLINE");
  receiveChallenge();
  Menu::pushScreen(screenFriendRecipient); tick(2);
  dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Recipient>SetMR", "%s", stackStr().c_str());
  rot(+1); tick(); rot(+1); tick(); dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Recipient>Chat" && !strcmp(g_selectedContactKey, "BOBKEY"), "Open Chat: %s",
        stackStr().c_str());
  g_test = "F1b existing challenge still answerable from the opened chat";
  rot(-1); tick(); shortP(T::now); tick(2);
  CHECK(stackStr() == "Main>Recipient>Chat>Answer", "%s", stackStr().c_str());
  back(T::now); tick(2); back(T::now); tick(2);
  CHECK(stackStr() == "Main>Recipient", "%s", stackStr().c_str());
  deleteGroup("GROUP1");

  g_test = "F1c Randomize from the recipient list: send returns to the list";
  resetAll(); strcpy(g_selectedGroupCode, "GRPF1C");
  presenceMsg("GRPF1C", "BOBDEV000001", "Bob", "ONLINE");
  Menu::pushScreen(screenFriendRecipient); tick(2);
  dot(T::now); tick(3); rot(+1); tick(); dot(T::now); tick(3);
  CHECK(stackStr() == "Main>Recipient>SetMR>Random", "%s", stackStr().c_str());
  back(T::now); tick(2);
  CHECK(stackStr() == "Main>Recipient>SetMR", "Back from Random: %s", stackStr().c_str());
  rot(+1); tick(); dot(T::now); tick(3); dot(T::now); tick(2);
  CHECK(stackStr() == "Main>Recipient>QS" && T::appendCalls == 1, "%s", stackStr().c_str());
  CHECK(T::store.back().ref.contact_key == std::string("BOBDEV000001") && T::store.back().ref.group_code == std::string("GRPF1C"),
        "challenge addressed to the wrong conversation");
  deleteGroup("GRPF1C");

  g_test = "F1d Back on the recipient-list choice returns to the list (cancel)";
  resetAll(); strcpy(g_selectedGroupCode, "GRPF1D");
  presenceMsg("GRPF1D", "BOBDEV000001", "Bob", "ONLINE");
  Menu::pushScreen(screenFriendRecipient); tick(2);
  dot(T::now); tick(3); back(T::now); tick(2);
  CHECK(stackStr() == "Main>Recipient" && T::appendCalls == 0, "%s", stackStr().c_str());
  deleteGroup("GRPF1D");

  g_test = "F1e helper-level: empty-line hook alone (dummy origin screen), no Open Chat; send/Back return to it";
  resetAll(); Menu::pushScreen(dummyA);
  invokeEmptyLineAction(0, "GROUP1", "BOBKEY"); tick(2);
  CHECK(nameOf(g_stack[g_stackSize - 1]) == std::string("SetMR") && !T::visible("Open Chat"), "%s",
        T::visibleJoined().c_str());
  back(T::now); tick(2);
  CHECK(g_stack[g_stackSize - 1] == dummyA, "Back did not return to the originating chat");
  invokeEmptyLineAction(0, "GROUP1", "BOBKEY"); tick(2);
  dot(T::now); tick(3); setDigits(5, 6, 7, 8); tick(); dot(T::now); tick(2);
  CHECK(g_stackSize >= 2 && g_stack[g_stackSize - 2] == dummyA && nameOf(g_stack[g_stackSize - 1]) == std::string("QS"),
        "after send: %s", stackStr().c_str());
}
static void f3_refresh() {
  g_test = "F3a Text chat open on the conversation refreshes to solved";
  resetAll(); std::string id = storeSentChallenge("GROUP1");
  TextMessage::navigateToChatDirect("GROUP1", "BOBKEY"); tick(3);
  CHECK(T::visibleJoined().find("waiting") != std::string::npos, "precondition: %s", T::visibleJoined().c_str());
  int before = loadsFor("GROUP1/BOBKEY");
  deliverResult("GROUP1", id, 3); tick(2);
  CHECK(T::visibleJoined().find("solved (3)") != std::string::npos, "Text chat stale: %s", T::visibleJoined().c_str());
  CHECK(loadsFor("GROUP1/BOBKEY") == before + 1, "expected exactly one index reload, got %d",
        loadsFor("GROUP1/BOBKEY") - before);
  tick(5);
  CHECK(loadsFor("GROUP1/BOBKEY") == before + 1, "continuous reloading");
  CHECK(pubCalls() == 0 || T::pubs.size() == 1, "no publish from receiving a result");

  g_test = "F3b Enigma chat open on the conversation refreshes to solved";
  resetAll(); id = storeSentChallenge("GROUP1");
  Enigma::navigateToChatDirect("GROUP1", "BOBKEY"); tick(3);
  CHECK(T::visibleJoined().find("waiting") != std::string::npos, "precondition: %s", T::visibleJoined().c_str());
  deliverResult("GROUP1", id, 4); tick(2);
  CHECK(T::visibleJoined().find("solved (4)") != std::string::npos, "Enigma chat stale: %s", T::visibleJoined().c_str());

  g_test = "F3c same peer in a different group: no reload of the open conversation";
  resetAll(); id = storeSentChallenge("GROUP1"); storeSentChallenge("GROUP2");
  TextMessage::navigateToChatDirect("GROUP2", "BOBKEY"); tick(3);
  int g2 = loadsFor("GROUP2/BOBKEY");
  deliverResult("GROUP1", id, 3); tick(3);
  CHECK(loadsFor("GROUP2/BOBKEY") == g2 && T::indexLoads.size() == static_cast<size_t>(g2), "unrelated reload");
  CHECK(storedState("GROUP1").state == STATE_UNLOCKED, "result not stored for its own conversation");

  g_test = "F3d unrelated contact in the same group: no reload";
  resetAll(); id = storeSentChallenge("GROUP1"); storeSentChallenge("GROUP1", "CARLKEY");
  TextMessage::navigateToChatDirect("GROUP1", "CARLKEY"); tick(3);
  int c = loadsFor("GROUP1/CARLKEY");
  deliverResult("GROUP1", id, 3); tick(3);
  CHECK(loadsFor("GROUP1/CARLKEY") == c, "unrelated contact reloaded");

  g_test = "F3e modeled failed update (stub returns false, keeps old record): no solved refresh, no reload";
  resetAll(); id = storeSentChallenge("GROUP1"); openChat();
  int f = loadsFor("GROUP1/BOBKEY");
  T::updateResult = false;
  deliverResult("GROUP1", id, 3); tick(3);
  CHECK(T::visibleJoined().find("waiting") != std::string::npos && T::visibleJoined().find("solved") == std::string::npos,
        "%s", T::visibleJoined().c_str());
  CHECK(loadsFor("GROUP1/BOBKEY") == f, "reload after a failed update");

  g_test = "F3f receiving a result sends nothing (no resend, no result action)";
  resetAll(); id = storeSentChallenge("GROUP1"); openChat();
  size_t pubsBefore = T::pubs.size();
  deliverResult("GROUP1", id, 3); tick(3);
  CHECK(T::pubs.size() == pubsBefore && T::appendCalls == 1, "publish/append after receiving: pubs %zu", T::pubs.size());
}

static void f1_realChats() {
  for (int m = 0; m < 2; m++) {
    const bool enigma = (m == 1);
    const std::string mode = enigma ? "Enigma" : "Text";
    g_test = "F1f " + mode + ": real empty-line Short opens the choice without Open Chat; Back returns to the chat";
    resetAll(); ScreenHandlerFn chat = openRealChat(enigma);
    uint8_t depth = g_stackSize;
    shortP(T::now); tick(2);
    CHECK(nameOf(g_stack[g_stackSize - 1]) == std::string("SetMR") && g_stack[g_stackSize - 2] == chat &&
              T::visible("Set Manually") && T::visible("Randomize") && !T::visible("Open Chat"),
          "%s", T::visibleJoined().c_str());
    back(T::now); tick(2);
    CHECK(g_stackSize == depth && g_stack[depth - 1] == chat && TextMessage::isConversationOpen("GROUP1", "BOBKEY"),
          "Back did not return to the %s chat", mode.c_str());
    g_test = "F1g " + mode + ": send from the real chat; Quick Switch Back returns to that chat";
    shortP(T::now); tick(2); dot(T::now); tick(3);
    CHECK(nameOf(g_stack[g_stackSize - 1]) == std::string("Manual"), "%s", stackStr().c_str());
    setDigits(5, 6, 7, 8); tick(); dot(T::now); tick(2);
    CHECK(nameOf(g_stack[g_stackSize - 1]) == std::string("QS") && g_stack[g_stackSize - 2] == chat &&
              T::appendCalls == 1 && T::store.back().ref.contact_key == std::string("BOBKEY") &&
              T::store.back().ref.group_code == std::string("GROUP1"),
          "after send: %s", stackStr().c_str());
    back(T::now); tick(2);
    CHECK(g_stackSize == depth && g_stack[depth - 1] == chat && TextMessage::isConversationOpen("GROUP1", "BOBKEY"),
          "Quick Switch Back did not return to the %s chat", mode.c_str());
    g_test = "F1h " + mode + ": a non-empty draft Short sends the draft (no challenge choice)";
    resetAll(); chat = openRealChat(enigma);
    typeDot(); T::now += 400; tick(3);
    shortP(T::now); tick(2);
    CHECK(g_stack[g_stackSize - 1] == chat && nameOf(g_stack[g_stackSize - 1]) != std::string("SetMR"),
          "choice opened over a draft");
  }
}
static void f3_draftAndSelection() {
  for (int m = 0; m < 2; m++) {
    const bool enigma = (m == 1);
    const std::string mode = enigma ? "Enigma" : "Text";
    g_test = "F3g " + mode + ": draft kept and editable across the solved refresh; no publish/submit/navigation";
    resetAll(); std::string id = storeSentChallenge("GROUP1");
    ScreenHandlerFn chat = openRealChat(enigma);
    typeDot(); T::now += 400; tick(3);
    int composeY = composeCursorY();
    std::string draftRow = T::rowAt(composeY);
    CHECK(draftRow.find("E") != std::string::npos, "draft not entered: '%s'", draftRow.c_str());
    uint8_t depth = g_stackSize; size_t pubs = T::pubs.size(); int appends = T::appendCalls;
    int loads = loadsFor("GROUP1/BOBKEY");
    deliverResult("GROUP1", id, 3); tick(2);
    CHECK(T::visibleJoined().find("solved (3)") != std::string::npos && T::visibleJoined().find("waiting") == std::string::npos,
          "not refreshed: %s", T::visibleJoined().c_str());
    CHECK(T::rowAt(composeY) == draftRow, "draft changed: '%s' -> '%s'", draftRow.c_str(), T::rowAt(composeY).c_str());
    CHECK(g_stackSize == depth && g_stack[depth - 1] == chat && T::pubs.size() == pubs && T::appendCalls == appends,
          "navigation/publish/submit during refresh");
    CHECK(loadsFor("GROUP1/BOBKEY") == loads + 1, "reloads %d", loadsFor("GROUP1/BOBKEY") - loads);
    tick(20);
    CHECK(loadsFor("GROUP1/BOBKEY") == loads + 1, "repeated reloads on idle ticks");
    typeDot(); T::now += 400; tick(3);
    std::string after = T::rowAt(composeY);
    CHECK(after.size() > draftRow.size() && after.find("E") != std::string::npos && after.find("E") != after.rfind("E"),
          "draft not editable after refresh: '%s' -> '%s'", draftRow.c_str(), after.c_str());

    g_test = "F3h " + mode + ": selected history item stays selected, same row, same message after refresh";
    resetAll(); id = storeSentChallenge("GROUP1");
    chat = openRealChat(enigma);
    composeY = composeCursorY();
    rot(-1); tick(2);
    int selY = cursorRowY(composeY);
    CHECK(selY >= 0 && T::rowAt(selY).find("waiting") != std::string::npos, "selection precondition: y=%d '%s'", selY,
          selY >= 0 ? T::rowAt(selY).c_str() : "");
    std::vector<std::pair<int, std::string>> before;
    for (auto& d : T::screen) if (d.text == "Friend") before.push_back({d.y, d.text});
    deliverResult("GROUP1", id, 3); tick(2);
    CHECK(cursorRowY(composeY) == selY && T::rowAt(selY).find("solved (3)") != std::string::npos,
          "selection moved or row stale: y=%d '%s'", cursorRowY(composeY), T::rowAt(selY).c_str());
    std::vector<std::pair<int, std::string>> after2;
    for (auto& d : T::screen) if (d.text == "Friend") after2.push_back({d.y, d.text});
    CHECK(before == after2, "viewport rows moved");
    shortP(T::now); tick(2);
    CHECK(nameOf(g_stack[g_stackSize - 1]) == std::string("Result"), "Short no longer acts on the selected game: %s",
          stackStr().c_str());
  }
}
static void f_layout() {
  g_test = "F17 status strings fit the 238px row (stub widths; exact check in run_all.sh)";
  const char* s[] = {"Not saved on this device", "Not saved; result not sent", "Not saved; not confirmed",
                     "Result could not be sent", "Server acked; not saved",
                     "Not sent: encode failed", "Not saved. Short: continue"};
  for (const char* t : s) CHECK(Display::textWidth(t) <= 238, "%s is %d px", t, Display::textWidth(t));
}
#endif

#ifndef BASELINE
// ---- Same-screen guess-history browsing on the Answer screen ----
static bool markerAtY(int y) { for (auto& d : T::screen) if (d.text == ">" && d.y == y && d.x == 2) return true; return false; }
static int markers() { int n = 0; for (auto& d : T::screen) if (d.text == ">") n++; return n; }
static bool labelLit() { return T::colorOf("Guess: ", guessRowY()) == ST77XX_YELLOW; }
static void draft35() {
  g_digitEntry.digits[0] = 3; g_digitEntry.digits[1] = 5; g_digitEntry.count = 2; g_digitEntry.previewDigit = 0;
  g_answerDirty = true; tick();
}
static void fb_browse() {
  const int cap = maxHistoryRows(false);
  g_test = "FB1 ring of 50 (57 attempts): browse newest..oldest retained, no further; reverse restores the draft";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  for (int i = 0; i < 57; i++) appendFriendEntry(&g_activeGame, static_cast<uint16_t>(1000 + i), 0, 0);
  g_answerNeedsFullRedraw = true; g_answerDirty = true; tick();
  draft35();
  DigitEntryState before = g_digitEntry;
  rot(-1); tick();
  CHECK(labelLit() && markers() == 0, "front not highlighted");
  rot(-1); tick();
  CHECK(markerAtY(historyRowY(1)) && T::rowAt(historyRowY(1)) == "> " + histRow(1056, 0, 0), "newest: '%s'",
        T::rowAt(historyRowY(1)).c_str());
  for (int i = 0; i < 80; i++) rot(-1);
  tick();
  CHECK(g_answerBrowse.selected == 49 && T::rowAt(historyRowY(cap)) == "> " + histRow(1007, 0, 0) &&
            T::rowAt(historyRowY(1)) == histRow(static_cast<uint16_t>(1007 + cap - 1), 0, 0),
        "oldest retained: sel %d '%s'", g_answerBrowse.selected, T::rowAt(historyRowY(cap)).c_str());
  for (int i = 0; i < 50; i++) rot(+1);
  tick();
  CHECK(labelLit() && T::rowAt(historyRowY(1)) == histRow(1056, 0, 0), "front again, window at newest");
  rot(+1); tick();
  CHECK(!g_answerBrowse.browsing && !labelLit() && memcmp(&before, &g_digitEntry, sizeof(before)) == 0,
        "draft or preview changed");
  CHECK(stackStr() == "Main>Chat>Answer" && g_activeGame.totalAttempts == 57 && pubCalls() == 0, "side effects");

  g_test = "FB2 fresh Short while browsing: no submit, no Exit, no publish; a later Short submits";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(5, 6, 7, 8); tick(); shortP(T::now); tick(2);
  setDigits(4, 3, 2, 1); g_answerDirty = true; tick();
  int pubs = pubCalls(); uint16_t att = g_activeGame.totalAttempts;
  rot(-1); rot(-1); tick();
  T::now += 20; shortP(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat>Answer" && g_activeGame.totalAttempts == att && pubCalls() == pubs &&
            !g_answerBrowse.browsing && g_digitEntry.count == 4,
        "%s attempts %u", stackStr().c_str(), g_activeGame.totalAttempts);
  shortP(T::now); tick(2);
  CHECK(g_activeGame.totalAttempts == att + 1 && T::rowAt(historyRowY(1)) == histRow(4321, 0, 4) && markers() == 0,
        "submit after browsing: '%s'", T::rowAt(historyRowY(1)).c_str());

  g_test = "FB3 empty draft (no digits): Short while browsing does not open Exit";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  rot(-1); tick(); shortP(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat>Answer", "%s", stackStr().c_str());

  g_test = "FB4 DOT pressed while browsing, released after returning: no confirm, no hold-delete";
  resetAll(); receiveChallenge(); openChat(); openAnswer(); draft35();
  rot(-1); tick();
  press(InputEventType::DOT_PRESS_START, T::now); tick();
  rot(+1); tick();
  T::now += 700; tick(2);
  dot(T::now, 700); tick(2);
  CHECK(g_digitEntry.count == 2 && g_digitEntry.digits[1] == 5, "draft changed: count %u", g_digitEntry.count);

  g_test = "FB5 notice row: the selection stays visible within the reduced window";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  for (int i = 0; i < cap + 2; i++) appendFriendEntry(&g_activeGame, static_cast<uint16_t>(3000 + i), 0, 0);
  setDigits(5, 6, 7, 8); tick();
  T::updateResult = false; shortP(T::now); tick(2);
  T::updateResult = true;
  const int nr = maxHistoryRows(true);
  CHECK(T::rowAt(Display::kStatusBarHeight + 2) == "Last guess not saved", "notice missing");
  // RAM history: 3000..3005 then the unsaved 5678 (newest). Front, then nr+1 rows: needs a scroll of 1.
  for (int i = 0; i < 1 + nr + 1; i++) rot(-1);
  tick();
  CHECK(g_activeGame.historyCount == cap + 3 && markerAtY(historyRowY(nr)) &&
            T::rowAt(historyRowY(nr)) == "> " + histRow(static_cast<uint16_t>(3000 + cap + 2 - nr), 0, 0) &&
            T::rowAt(historyRowY(1)) == histRow(static_cast<uint16_t>(3000 + cap + 1), 0, 0) && !T::visible("8") &&
            T::rowAt(Display::kStatusBarHeight + 2) == "Last guess not saved",
        "%s", T::visibleJoined().c_str());
  rot(+1); rot(+1); rot(+1); tick();  // selected 3 -> 0
  CHECK(markerAtY(historyRowY(1)) && T::rowAt(historyRowY(1)) == "> " + histRow(5678, 0, 0), "back to newest: %s",
        T::visibleJoined().c_str());

  g_test = "FB6 re-entering Answer starts in digit editing at the newest rows";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  const uint8_t g6[6][4] = {{5, 6, 7, 8}, {1, 2, 4, 3}, {4, 3, 2, 1}, {2, 1, 3, 4}, {5, 6, 8, 7}, {6, 5, 7, 8}};
  for (auto& g : g6) { setDigits(g[0], g[1], g[2], g[3]); tick(); shortP(T::now); tick(2); }
  for (int i = 0; i < 6; i++) rot(-1);
  tick();
  CHECK(g_answerBrowse.browsing && g_answerBrowse.scroll > 0, "precondition: scrolled while browsing");
  back(T::now); tick(2);
  rot(-1); tick(); shortP(T::now); tick(2);
  CHECK(stackStr() == "Main>Chat>Answer" && !g_answerBrowse.browsing && markers() == 0 && !labelLit() &&
            T::rowAt(historyRowY(1)) == histRow(6578, 0, 0),
        "%s", T::visibleJoined().c_str());

  g_test = "FB8 two fresh Shorts in ONE batch while browsing, complete draft: first returns, second submits once";
  resetAll(); receiveChallenge(); openChat(); openAnswer();
  setDigits(5, 6, 7, 8); g_answerDirty = true; tick();
  rot(-1); rot(-1); tick();  // browsing, entered in an earlier tick
  T::now += 30;
  {
    uint16_t att = g_activeGame.totalAttempts; int upd = T::updateCalls;
    CHECK(g_answerBrowse.browsing && g_answerShortGuard.state == DrawnBeforeGuard::State::BOUNDED,
          "precondition: browsing with the stale-Short guard bound (not UNTIL_DRAW, which would drop everything)");
    shortP(T::now); T::now += 40; shortP(T::now);
    CHECK(T::raw.size() == 2, "precondition: both Shorts queued for one tick");
    tick();
    CHECK(!g_answerBrowse.browsing && g_activeGame.totalAttempts == att + 1 && T::updateCalls == upd + 1 &&
              stackStr() == "Main>Chat>Answer",
          "attempts %u (was %u), updates %d (was %d), %s", g_activeGame.totalAttempts, att, T::updateCalls, upd,
          stackStr().c_str());
    tick(2);
    CHECK(g_activeGame.totalAttempts == att + 1 && T::rowAt(historyRowY(1)) == histRow(5678, 0, 0),
          "not exactly one submission: '%s'", T::rowAt(historyRowY(1)).c_str());
  }
  g_test = "FB9 two fresh Shorts in ONE batch while browsing, incomplete draft: first returns, second opens Exit";
  resetAll(); receiveChallenge(); openChat(); openAnswer(); draft35();
  rot(-1); tick();
  T::now += 30;
  {
    uint16_t att = g_activeGame.totalAttempts;
    CHECK(g_answerBrowse.browsing && g_answerShortGuard.state == DrawnBeforeGuard::State::BOUNDED, "precondition");
    shortP(T::now); T::now += 40; shortP(T::now);
    tick();
    CHECK(!g_answerBrowse.browsing && g_stack[g_stackSize - 1] == Menu::confirmPromptScreen &&
              g_activeGame.totalAttempts == att && g_digitEntry.count == 2,
          "stack %s attempts %u", stackStr().c_str(), g_activeGame.totalAttempts);
  }
  g_test = "FB7 the secret is never drawn while browsing";
  bool leaked = false;
  for (auto& d : T::drawLog) if (d.text == "4A0B" || d.text.find("1234") != std::string::npos) leaked = true;
  CHECK(!leaked, "secret drawn");
}
#endif

int main() {
  T_initServices();
  r_playDoubleConfirm(); r_cancel(); r_chatDoubleShort(); r_quickSwitchQueuedDot(); r_manualBackThenDot();
  r_randomQueuedDot(); r_solveDoubleShort(); r_wrongGuessVisible(); r_resultDotDuringSend(); r_resultDoubleConfirm();
  r_chatDoubleShortToAnswer(); r_reopenShowsHistory(); r_recipientLive(); r_recipientDirect(); r_senderChatRefresh(); r_realChatQueuedShorts();
#ifndef BASELINE
  m_epoch(); m_tickOwned(); g_timestamps();
  f_flowAdmission(); f_cancelQueued(); f_menuGuard(); f_challengeOutcomes(); f_quickSwitch(); f_manualGuard(); f_random();
  f_submitPrecondition(); f_solveFeedback(); f_resultOutcomes(); f_wrongGuess(); f_resultScreen(); f_answerBoundaries();
  f_bindAfterDraw(); f_historyLayout(); p_generation(); p_recipients(); f1_entryFlows(); f1_realChats(); f3_refresh(); f3_draftAndSelection(); f_layout(); fb_browse();
#endif
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
