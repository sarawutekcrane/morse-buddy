// HOST TEST (not production). Private radio PTT and radio MQTT paths
// (hardware, devices A on 827cd27 and B on b573ec7: holding PTT,
// "Claiming..." went back to "Ready" before the button was released;
// "[PERF][RADIO_AUDIO] serviceTick 289 ms" with "[DIAG][MQTT] qos0 ...
// maxMs=288"). Compiles the ACTUAL src/core/radio.cpp (Talk screen),
// radio_transport.cpp (claim/grant/deny, negotiation, MQTT fallback,
// broadcast audio) and menu.cpp into this translation unit, against the
// shared stubs (stubs.cpp: Display, Input, MqttManager publishBinary()).
// The peer is modeled by delivering its packets to the registered handlers.
// The MQTT worker's publish chain (connection lent for a Race heartbeat /
// chat / Outbox publish) is modeled by stubs.cpp's asyncPublish FIFO;
// workerNext() completes its oldest job, so the tests choose exactly when a
// queued CLAIM/GRANT/RELEASE actually goes out (Gate 1).
// NOT modeled: STUN/UDP direct (fake_radio/WiFi.h never delivers UDP), I2S,
// timing on the ESP32, a real broker or a second device.
#include "stubs.h"
#include "core/menu.cpp"
#include "core/radio_transport.cpp"
#include "core/radio.cpp"
#include <esp_heap_caps.h>
#include <cstdio>
#include <string>
#include <vector>

// ---- low-memory model: the program's allocation functions, malloc-backed
// (ASan still checks every block); g_failNothrowNew makes the scalar
// nothrow new -- the one OwedRelease uses -- return nullptr, as on an ESP32
// out of heap. Array nothrow new (the pre-buffer) is not affected.
#include <cstdlib>
#include <new>
static bool g_failNothrowNew = false;
static int g_nothrowNewFailures = 0;
void* operator new(std::size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  if (g_failNothrowNew) { g_nothrowNewFailures++; return nullptr; }
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// ---- what radio.cpp / radio_transport.cpp need beyond stubs.cpp ----
namespace Sleep { void notifyActivity() {} }
void playTone(uint16_t, uint16_t, SoundClass) {}
static int g_audioActiveSets = 0;
static bool g_audioActive = false;
void setRadioAudioActive(bool a) { g_audioActive = a; g_audioActiveSets++; }
namespace Settings { bool getMuteRadioOutsideRadio() { return false; } }
static PendingRadioIntent g_intent;
const PendingRadioIntent& getPendingRadioIntent() { return g_intent; }
void clearPendingRadioIntent() { g_intent.valid = false; }
namespace Notifications {
void serviceInit() {}
uint16_t unreadCount(uint8_t) { return 0; }
}

// RadioAudio: capture delivers frames only when the test calls micFrame().
static RadioAudio::CaptureFrameFn g_capture = nullptr;
static int g_framesPlayed = 0;
namespace RadioAudio {
void startCapture(CaptureFrameFn fn) { g_capture = fn; }
void stopCapture() { g_capture = nullptr; }
void playFrame(const int16_t*, size_t) { g_framesPlayed++; }
}  // namespace RadioAudio

// MqttManager::publishBinaryNoWait() (radio audio): every attempt recorded;
// accepts while T::connected and g_noWaitAccepts (false models "socket not
// writable now / connection lent").
struct NoWaitPub { std::string topic; size_t len; bool result; std::vector<uint8_t> data; std::string group; };
static std::vector<NoWaitPub> g_noWait;
static bool g_noWaitAccepts = true;
namespace MqttManager {
bool publishBinaryNoWait(const char* group, const char* topic, const uint8_t* data, uint16_t len) {
  bool r = T::connected && g_noWaitAccepts;
  g_noWait.push_back({topic, len, r, std::vector<uint8_t>(data, data + len), group});
  return r;
}
}  // namespace MqttManager

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
void T_tickServices();
NetworkPacketHandlerFn T_packetHandler(uint8_t kind);

static const char* kGroup = "GROUP1";
static const char* kPeer = "AABBCCDDEEF0";   // the device called
static const char* kOther = "112233445566";  // a third device

static void tick(int n = 1) {
  for (int i = 0; i < n; i++) {
    T_tickServices();
    Menu::tick();
    T::now += 5;
  }
}
static void runFor(uint32_t ms) {
  uint32_t end = T::now + ms;
  while (static_cast<int32_t>(T::now - end) < 0) tick();
}
static void pttDown() { T::raw.push_back(InputEvent(InputEventType::DOT_PRESS_START, 0, 0, T::now)); }
static void pttUp(uint32_t held) { T::raw.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, held, T::now)); }

// ---- the module's own wire layout (radio_transport.cpp) ----
static std::vector<uint8_t> idPayload(uint8_t type, const char* device, const char* session) {
  std::vector<uint8_t> p(1 + 16 + 24, 0);
  p[0] = type;
  strncpy(reinterpret_cast<char*>(&p[1]), device, 15);
  strncpy(reinterpret_cast<char*>(&p[17]), session, 23);
  return p;
}
static std::string sessionOf(const T::Pub& p) {  // CLAIM/RELEASE or GRANT/DENY: header(6) type(1) device(16) session(24)
  if (p.data.size() < 6 + 41) return "";
  return std::string(reinterpret_cast<const char*>(&p.data[6 + 17]));
}
static uint8_t typeOf(const T::Pub& p) { return p.data.size() > 6 ? p.data[6] : 0xFF; }
static void deliver(uint8_t kind, const std::vector<uint8_t>& payload) {
  NetworkPacketHandlerFn fn = T_packetHandler(kind);
  if (fn != nullptr) fn(kGroup, "", payload.data(), payload.size());
}
static void peerReply(uint8_t type, const std::string& session) {  // GRANT=0, DENY=1
  deliver(PacketCodec::PK_RADIO_BUSY_REPLY, idPayload(type, kPeer, session.c_str()));
}
static std::vector<T::Pub> pubsTo(const std::string& prefix) {
  std::vector<T::Pub> out;
  for (auto& p : T::pubs)
    if (p.topic.compare(0, prefix.size(), prefix) == 0) out.push_back(p);
  return out;
}

// What actually went out to the broker, in order: synchronous publishes
// (T::onPublished) and the worker chain's publishes as workerNext() completes
// them. A queued packet is NOT on the wire until then (Gate 1).
struct WirePkt { std::string topic; std::vector<uint8_t> data; };
static std::vector<WirePkt> g_wire;
static std::string wSession(const WirePkt& w) { return w.data.size() >= 6 + 41 ? std::string(reinterpret_cast<const char*>(&w.data[6 + 17])) : ""; }
static uint8_t wType(const WirePkt& w) { return w.data.size() > 6 ? w.data[6] : 0xFF; }
static int wireCount(const std::string& prefix, int type = -1, const std::string& sid = "") {
  int n = 0;
  for (auto& w : g_wire)
    if (w.topic.compare(0, prefix.size(), prefix) == 0 && (type < 0 || wType(w) == type) && (sid.empty() || wSession(w) == sid)) n++;
  return n;
}
// The MQTT worker finishes the oldest job of the lent connection's chain
// (MqttManager: in order; the result reaches the main task on its tick).
static void workerNext(bool ok) {
  if (T::asyncPending.empty()) return;
  if (ok) g_wire.push_back({T::asyncPending.front().topic, T::asyncPending.front().data});
  T::completeAsync(ok);
}
// The worker finishes the oldest job of ONE group's chain (each group's
// connection has its own chain; they complete independently).
static void workerNextFor(const char* group, bool ok) {
  for (auto it = T::asyncPending.begin(); it != T::asyncPending.end(); ++it) {
    if (it->group != group) continue;
    T::AsyncPub p = *it;
    T::asyncPending.erase(it);
    if (ok) g_wire.push_back({p.topic, p.data});
    if (p.done != nullptr) p.done(p.ticket, ok);
    return;
  }
}
static int pendingFor(const char* group) {
  int n = 0;
  for (auto& p : T::asyncPending) n += p.group == group ? 1 : 0;
  return n;
}
static int queuedTo(const std::string& prefix) {
  int n = 0;
  for (auto& p : T::asyncPending) n += p.topic.compare(0, prefix.size(), prefix) == 0 ? 1 : 0;
  return n;
}
// A Race heartbeat (or chat / Outbox publish) that lends the connection to
// the worker: QoS1, its PUBACK wait on the worker (up to 5 s).
static void lendForWorkerPublish(const char* what) {
  T::asyncPublish = true;
  uint8_t b[4] = {1, 2, 3, 4};
  MqttManager::publishBinaryAsync(kGroup, what, b, 4, false, 1, nullptr);
}


// Replays what reached the broker (radio/busy/<peer>: CLAIM/RELEASE) into the
// receiver's ACTUAL busy handler, in wire order, and reports the receiver's
// busy session after each packet. A RELEASE for one session must never end
// another; after the last packet the receiver must be free (no reliance on
// the 5 s busy TTL).
static std::vector<std::string> replayToReceiver(bool* freeAtEnd) {
  std::vector<std::string> trace;
  RadioTransport::g_receiverBusy = RadioTransport::ReceiverBusy{};
  T::now += 10;  // no TTL expiry: every step within milliseconds
  const std::vector<WirePkt> wire = g_wire;  // the receiver's own GRANT/DENY replies are not part of the replay
  for (auto& w : wire) {
    if (w.topic.compare(0, 11, "radio/busy/") != 0) continue;
    PacketCodec::Header h;
    const uint8_t* body = nullptr;
    if (!PacketCodec::decodeHeader(w.data.data(), w.data.size(), &h, &body)) continue;
    std::vector<uint8_t> payload(body, body + h.payload_length);
    NetworkPacketHandlerFn fn = T_packetHandler(PacketCodec::PK_RADIO_BUSY);
    if (fn != nullptr) fn(kGroup, "", payload.data(), payload.size());
    trace.push_back(std::string(wType(w) == 0 ? "C(" : "R(") + wSession(w) + ")->" +
                    (RadioTransport::g_receiverBusy.busy ? RadioTransport::g_receiverBusy.sessionId : "free"));
  }
  *freeAtEnd = !RadioTransport::g_receiverBusy.busy;
  return trace;
}
// Every CLAIM on the wire is followed, later on the wire, by the RELEASE of
// the same session.
static bool everyClaimReleased(std::string* missing) {
  for (size_t i = 0; i < g_wire.size(); i++) {
    if (g_wire[i].topic.compare(0, 11, "radio/busy/") != 0 || wType(g_wire[i]) != 0) continue;
    bool found = false;
    for (size_t j = i + 1; j < g_wire.size() && !found; j++)
      found = g_wire[j].topic.compare(0, 11, "radio/busy/") == 0 && wType(g_wire[j]) == 1 && wSession(g_wire[j]) == wSession(g_wire[i]);
    if (!found) { *missing = wSession(g_wire[i]); return false; }
  }
  return true;
}
static void fillChain3() {
  lendForWorkerPublish("race/state");
  lendForWorkerPublish("msg/AABBCCDDEEF0");
  lendForWorkerPublish("msg/AABBCCDDEEF1");
}

// The Talk screen for another contact, keeping the MQTT worker chain model
// (pending jobs, the wire log) as it is.
static void openTalkKeepChain(const char* contact) {
  strcpy(g_selectedContactKey, contact);
  Menu::init();
  Menu::pushScreen(screenTalk);
  tick(2);
}

static void openTalk(const char* contact) {
  T::reset();
  g_noWait.clear();
  g_noWaitAccepts = true;
  g_capture = nullptr;
  RadioTransport::stopPrivateCall();
  RadioTransport::stopBroadcastCall();
  RadioTransport::resetPrivateCall();
  RadioTransport::g_receiverBusy = RadioTransport::ReceiverBusy{};
  RadioTransport::g_lastFailure = RadioTransport::PrivateFailure::NONE;
  RadioTransport::g_failedSessionId[0] = '\0';
  while (RadioTransport::g_owedRelease != nullptr) {
    RadioTransport::OwedRelease* r = RadioTransport::g_owedRelease;
    RadioTransport::g_owedRelease = r->next;
    delete r;
  }
  g_wire.clear();
  T::onPublished = [](const T::Pub& p) { g_wire.push_back({p.topic, p.data}); };
  T::pubs.clear();
  g_fakeHeapFree = 200 * 1024;
  g_fakeHeapLargest = 110 * 1024;
  g_fakeHeapMinFree = 100 * 1024;
  strcpy(g_selectedGroupCode, kGroup);
  strcpy(g_selectedContactKey, contact);
  Menu::init();
  Menu::pushScreen(screenTalk);
  tick(2);
}
static std::string status() {  // the Talk screen's status row
  int16_t lh = Display::lineHeight();
  return T::rowAt(Display::kStatusBarHeight + 2 + lh);
}

int main() {
  T::reset();
  T_initServices();

  g_test = "RP1 PTT pressed: CLAIM to radio/busy/<peer> with a fresh session id; Claiming...";
  openTalk(kPeer);
  pttDown(); tick();
  auto claims = pubsTo(std::string("radio/busy/") + kPeer);
  std::string sid = claims.empty() ? "" : sessionOf(claims[0]);
  CHECK(claims.size() == 1 && typeOf(claims[0]) == 0 && !sid.empty() &&
            RadioTransport::getPrivateState() == RadioTransport::PrivateState::CLAIMING && status() == "Claiming...",
        "claims %zu sid '%s' status '%s'", claims.size(), sid.c_str(), status().c_str());

  g_test = "RP2 no GRANT within 1 s while the PTT is held: CLAIM retried every 250 ms, then 'No response' (not 'Ready') while held, RELEASE for that session; the release returns to Ready";
  runFor(1100);
  claims = pubsTo(std::string("radio/busy/") + kPeer);
  int claimCount = 0, releaseCount = 0;
  bool sameSession = true;
  for (auto& c : claims) {
    if (typeOf(c) == 0) claimCount++;
    if (typeOf(c) == 1) releaseCount++;
    if (sessionOf(c) != sid) sameSession = false;
  }
  CHECK(claimCount >= 4 && claimCount <= 5 && releaseCount == 1 && sameSession, "claims %d (want 4-5), releases %d, one session %d",
        claimCount, releaseCount, sameSession);
  CHECK(RadioTransport::getPrivateState() == RadioTransport::PrivateState::IDLE &&
            RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NO_RESPONSE && status() == "No response" &&
            g_capture == nullptr && !g_audioActive,
        "state %u failure %u status '%s' capture %d audioActive %d", static_cast<unsigned>(RadioTransport::getPrivateState()),
        static_cast<unsigned>(RadioTransport::getPrivateFailure()), status().c_str(), g_capture != nullptr, g_audioActive);
  runFor(3000);  // still held: the status stays, nothing more is sent, no retry loop
  size_t pubsHeld = T::pubs.size();
  runFor(3000);
  CHECK(status() == "No response" && T::pubs.size() == pubsHeld, "after 6 s held: status '%s', publishes %zu -> %zu", status().c_str(),
        pubsHeld, T::pubs.size());
  g_test = "RP2b a GRANT for the timed-out session arriving late is not acted on (released already)";
  peerReply(0, sid); tick();
  CHECK(RadioTransport::getPrivateState() == RadioTransport::PrivateState::IDLE && g_capture == nullptr &&
            status() == "No response",
        "late GRANT: state %u capture %d status '%s'", static_cast<unsigned>(RadioTransport::getPrivateState()),
        g_capture != nullptr, status().c_str());
  pttUp(7200); tick(2);
  CHECK(status() == "Ready" && RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NONE,
        "after the release: status '%s' failure %u", status().c_str(), static_cast<unsigned>(RadioTransport::getPrivateFailure()));

  g_test = "RP3 every CLAIM publish refused (no connection): 'Send failed' while held";
  openTalk(kPeer);
  pttDown(); tick();
  T::connected = false;  // stubs: isGroupReady() true at the press, then every publish refused
  runFor(1100);
  CHECK(RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NO_RESPONSE ||
            RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::SEND_FAILED,
        "failure %u", static_cast<unsigned>(RadioTransport::getPrivateFailure()));
  T::connected = true;
  pttUp(1200); tick(2);
  openTalk(kPeer);
  T::publishScript = {false, false, false, false, false, false};  // every CLAIM refused; the first refusal closes the connection
  pttDown(); tick();
  runFor(500);
  CHECK(RadioTransport::g_call.claimSendFails >= 2 && RadioTransport::g_call.claimSendFails == RadioTransport::g_call.claimSends &&
            RadioTransport::g_call.claimPublished == 0 && RadioTransport::g_call.claimQueued == 0,
        "diagnostic counters while refused: sends %u fails %u published %u queued %u",
        static_cast<unsigned>(RadioTransport::g_call.claimSends), static_cast<unsigned>(RadioTransport::g_call.claimSendFails),
        static_cast<unsigned>(RadioTransport::g_call.claimPublished), static_cast<unsigned>(RadioTransport::g_call.claimQueued));
  runFor(600);
  CHECK(RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::SEND_FAILED && status() == "Send failed",
        "all refused: failure %u status '%s'", static_cast<unsigned>(RadioTransport::getPrivateFailure()), status().c_str());
  T::connected = true;
  pttUp(1200); tick(2);
  CHECK(status() == "Ready", "released: '%s'", status().c_str());

  g_test = "RP4 GRANT within the window: Connecting..., capture on, no failure; held past 1 s it stays in the call (no timeout once granted); the release sends RELEASE";
  openTalk(kPeer);
  pttDown(); tick();
  sid = sessionOf(pubsTo(std::string("radio/busy/") + kPeer)[0]);
  runFor(400);
  peerReply(0, sid); tick();
  bool negotiating = RadioTransport::getPrivateState() == RadioTransport::PrivateState::NEGOTIATING && g_capture != nullptr &&
                     status() == "Connecting...";
  runFor(3000);  // negotiation times out to MQTT fallback (no UDP here); keep-alive CLAIMs every 2 s
  bool relay = RadioTransport::getPrivateState() == RadioTransport::PrivateState::MQTT_FALLBACK && status() == "Talking (relay)";
  CHECK(negotiating && relay && RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NONE,
        "negotiating %d, relay after 3 s %d (state %u, status '%s')", negotiating, relay,
        static_cast<unsigned>(RadioTransport::getPrivateState()), status().c_str());
  g_test = "RP4b a duplicate GRANT for the live session (the receiver re-grants each keep-alive CLAIM) changes nothing: no renegotiation, the call stays on the relay";
  peerReply(0, sid); tick();
  CHECK(RadioTransport::getPrivateState() == RadioTransport::PrivateState::MQTT_FALLBACK && status() == "Talking (relay)",
        "after a duplicate GRANT: state %u status '%s'", static_cast<unsigned>(RadioTransport::getPrivateState()), status().c_str());
  g_test = "RP4c the release sends RELEASE for the session and returns to Ready";
  size_t before = T::pubs.size();
  pttUp(3500); tick(2);
  bool releaseSent = false;
  for (size_t i = before; i < T::pubs.size(); i++)
    if (T::pubs[i].topic == std::string("radio/busy/") + kPeer && typeOf(T::pubs[i]) == 1 && sessionOf(T::pubs[i]) == sid) releaseSent = true;
  CHECK(releaseSent && status() == "Ready" && g_capture == nullptr, "RELEASE %d status '%s'", releaseSent, status().c_str());

  g_test = "RP5 a GRANT for another session (stale) or from another device is ignored; still Claiming...";
  openTalk(kPeer);
  pttDown(); tick();
  sid = sessionOf(pubsTo(std::string("radio/busy/") + kPeer)[0]);
  peerReply(0, "STALE-SESSION-ID");
  deliver(PacketCodec::PK_RADIO_BUSY_REPLY, idPayload(0, kOther, sid.c_str()));
  tick();
  CHECK(RadioTransport::getPrivateState() == RadioTransport::PrivateState::CLAIMING && status() == "Claiming...",
        "state %u status '%s'", static_cast<unsigned>(RadioTransport::getPrivateState()), status().c_str());
  pttUp(300); tick(2);

  g_test = "RP6 DENY: BUSY (no mic) while held (receiver-authoritative, unchanged); the release returns to Ready";
  openTalk(kPeer);
  pttDown(); tick();
  sid = sessionOf(pubsTo(std::string("radio/busy/") + kPeer)[0]);
  peerReply(1, sid); tick();
  bool busy = status() == "BUSY (no mic)";
  runFor(1500);
  bool stillBusy = status() == "BUSY (no mic)" && RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NONE;
  pttUp(1600); tick(2);
  CHECK(busy && stillBusy && status() == "Ready", "busy %d still busy after 1.5 s %d, released '%s'", busy, stillBusy, status().c_str());

  g_test = "RP7 released before any reply: RELEASE, Ready, and no timeout or failure afterwards";
  openTalk(kPeer);
  pttDown(); tick();
  runFor(300);
  pttUp(300); tick(2);
  runFor(1500);
  CHECK(status() == "Ready" && RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NONE &&
            pubsTo(std::string("radio/busy/") + kPeer).back().data[6] == 1,
        "status '%s' failure %u", status().c_str(), static_cast<unsigned>(RadioTransport::getPrivateFailure()));

  g_test = "RP8 a new press after a failed one starts a fresh claim (new session), the failure cleared";
  openTalk(kPeer);
  pttDown(); tick();
  std::string first = sessionOf(pubsTo(std::string("radio/busy/") + kPeer)[0]);
  runFor(1100);
  pttUp(1100); tick(2);
  pttDown(); tick();
  auto all = pubsTo(std::string("radio/busy/") + kPeer);
  std::string second = sessionOf(all.back());
  CHECK(!second.empty() && second != first && typeOf(all.back()) == 0 && status() == "Claiming..." &&
            RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NONE,
        "sessions '%s' -> '%s' status '%s'", first.c_str(), second.c_str(), status().c_str());
  pttUp(100); tick(2);

  g_test = "RP9 receiver side: a CLAIM while not ready gets no reply; when ready GRANT on radio/busy_reply/<claimer> with the session; another claimer while busy gets DENY; the matching RELEASE frees it";
  openTalk(kPeer);
  T::connected = false;
  deliver(PacketCodec::PK_RADIO_BUSY, idPayload(0, kPeer, "S-ONE"));
  size_t notReadyReplies = pubsTo("radio/busy_reply/").size();
  T::connected = true;
  deliver(PacketCodec::PK_RADIO_BUSY, idPayload(0, kPeer, "S-ONE"));
  auto replies = pubsTo(std::string("radio/busy_reply/") + kPeer);
  bool granted = replies.size() == 1 && typeOf(replies[0]) == 0 && sessionOf(replies[0]) == "S-ONE";
  deliver(PacketCodec::PK_RADIO_BUSY, idPayload(0, kOther, "S-TWO"));
  auto otherReplies = pubsTo(std::string("radio/busy_reply/") + kOther);
  bool denied = otherReplies.size() == 1 && typeOf(otherReplies[0]) == 1;
  deliver(PacketCodec::PK_RADIO_BUSY, idPayload(1, kPeer, "S-OLD"));  // a RELEASE for another session: ignored
  deliver(PacketCodec::PK_RADIO_BUSY, idPayload(0, kOther, "S-THREE"));
  bool stillDenied = pubsTo(std::string("radio/busy_reply/") + kOther).back().data[6] == 1;
  deliver(PacketCodec::PK_RADIO_BUSY, idPayload(1, kPeer, "S-ONE"));
  deliver(PacketCodec::PK_RADIO_BUSY, idPayload(0, kOther, "S-FOUR"));
  bool grantedAfter = pubsTo(std::string("radio/busy_reply/") + kOther).back().data[6] == 0;
  CHECK(notReadyReplies == 0 && granted && denied && stillDenied && grantedAfter,
        "not ready: %zu replies; granted %d; other denied %d; stale RELEASE ignored %d; granted after the matching RELEASE %d",
        notReadyReplies, granted, denied, stillDenied, grantedAfter);

  g_test = "RP10 MQTT fallback audio goes through publishBinaryNoWait() to radio/audio/<peer>: one attempt per frame, a skipped frame is never retried (nothing stale later)";
  openTalk(kPeer);
  pttDown(); tick();
  sid = sessionOf(pubsTo(std::string("radio/busy/") + kPeer)[0]);
  peerReply(0, sid); tick();
  runFor(2100);  // to MQTT fallback (pre-buffer flushed)
  g_noWait.clear();
  int16_t frame[RadioAudio::kFrameSamples] = {0};
  for (int i = 0; i < 10; i++) { if (g_capture) g_capture(frame, RadioAudio::kFrameSamples); }
  g_noWaitAccepts = false;
  for (int i = 0; i < 5; i++) { if (g_capture) g_capture(frame, RadioAudio::kFrameSamples); }
  g_noWaitAccepts = true;
  runFor(200);
  int ok = 0, skipped = 0;
  bool topicOk = true;
  for (auto& w : g_noWait) {
    if (w.result) ok++; else skipped++;
    if (w.topic != std::string("radio/audio/") + kPeer) topicOk = false;
  }
  bool noSyncAudio = pubsTo("radio/audio/").empty();
  CHECK(ok == 10 && skipped == 5 && g_noWait.size() == 15 && topicOk && noSyncAudio,
        "sent %d skipped %d attempts %zu topic ok %d, synchronous audio publishes %zu (want 0)", ok, skipped, g_noWait.size(), topicOk,
        pubsTo("radio/audio/").size());
  pttUp(2500); tick(2);

  g_test = "RP11 Everyone (broadcast) audio also uses publishBinaryNoWait()";
  openTalk(MessageStore::kEveryone);
  pttDown(); tick();
  g_noWait.clear();
  for (int i = 0; i < 3; i++) { if (g_capture) g_capture(frame, RadioAudio::kFrameSamples); }
  CHECK(g_noWait.size() == 3 && g_noWait[0].topic == "radio/audio/broadcast" && pubsTo("radio/audio/").empty() &&
            status() == "Talking...",
        "attempts %zu status '%s'", g_noWait.size(), status().c_str());
  pttUp(100); tick(2);

  g_test = "RP12 pre-buffer sized to the heap: 100 frames with ample heap; the device A figures (82368 free, 42996 largest) give 67; under 10 frames' worth none (the call goes ahead)";
  CHECK(RadioTransport::preBufferFramesFor(200 * 1024, 110 * 1024) == 100 &&
            RadioTransport::preBufferFramesFor(82368, 42996) == 67 &&
            RadioTransport::preBufferFramesFor(32 * 1024 + 9 * 640, 100000) == 0 &&
            RadioTransport::preBufferFramesFor(200 * 1024, 5000) == 0,
        "frames: ample %u, device A %u, low %u, fragmented %u", RadioTransport::preBufferFramesFor(200 * 1024, 110 * 1024),
        RadioTransport::preBufferFramesFor(82368, 42996), RadioTransport::preBufferFramesFor(32 * 1024 + 9 * 640, 100000),
        RadioTransport::preBufferFramesFor(200 * 1024, 5000));
  g_test = "RP12b the pre-buffer holds what the heap allows and is flushed on MQTT fallback; with no room the call still connects";
  openTalk(kPeer);
  g_fakeHeapFree = 82368;
  g_fakeHeapLargest = 42996;
  pttDown(); tick();
  sid = sessionOf(pubsTo(std::string("radio/busy/") + kPeer)[0]);
  peerReply(0, sid); tick();
  g_noWait.clear();
  for (int i = 0; i < 120; i++) { if (g_capture) g_capture(frame, RadioAudio::kFrameSamples); }
  runFor(2100);
  size_t flushed = g_noWait.size();
  pttUp(2200); tick(2);
  openTalk(kPeer);
  g_fakeHeapFree = 30 * 1024;
  g_fakeHeapLargest = 30 * 1024;
  pttDown(); tick();
  sid = sessionOf(pubsTo(std::string("radio/busy/") + kPeer)[0]);
  peerReply(0, sid); tick();
  g_noWait.clear();
  for (int i = 0; i < 20; i++) { if (g_capture) g_capture(frame, RadioAudio::kFrameSamples); }
  runFor(2100);
  CHECK(flushed == 67 && g_noWait.empty() && status() == "Talking (relay)",
        "flushed %zu (want 67); low heap: buffered %zu (want 0), status '%s'", flushed, g_noWait.size(), status().c_str());
  pttUp(2200); tick(2);


  // ===========================================================================
  // Gate 1: CLAIM queued behind worker publishes vs the 1 s claim window
  // ===========================================================================
  const std::string busyTo = std::string("radio/busy/") + kPeer;

  g_test = "RQ1 PTT while a Race heartbeat holds the worker: the CLAIM is QUEUED (not on the wire, not counted as published), no retry stacks behind it; published when the heartbeat completes; GRANT 200 ms later -> call";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  pttDown(); tick();
  bool queuedNotSent = queuedTo(busyTo) == 1 && wireCount(busyTo) == 0 && RadioTransport::g_call.claimQueued == 1 &&
                       RadioTransport::g_call.claimPublished == 0 && RadioTransport::g_call.claimPublishedMs == 0 &&
                       status() == "Claiming...";
  runFor(600);  // heartbeat PUBACK takes 600 ms (hardware: normal)
  int stacked = queuedTo(busyTo);
  workerNext(true);  // heartbeat
  workerNext(true);  // the CLAIM
  tick();
  std::string sidQ = RadioTransport::g_call.session_id;
  bool publishedNow = wireCount(busyTo, 0, sidQ) == 1 && RadioTransport::g_call.claimPublished == 1 &&
                      RadioTransport::g_call.claimPublishedMs != 0 && RadioTransport::g_call.claimQueuedTicket == 0;
  runFor(200);
  peerReply(0, sidQ); tick();
  CHECK(queuedNotSent && stacked == 1 && publishedNow &&
            RadioTransport::getPrivateState() == RadioTransport::PrivateState::NEGOTIATING,
        "queued-not-sent %d, CLAIMs in the chain after 600 ms %d (want 1), published on completion %d, state %u",
        queuedNotSent, stacked, publishedNow, static_cast<unsigned>(RadioTransport::getPrivateState()));
  pttUp(900); tick(2);

  g_test = "RQ2 CLAIM published 900 ms after the press (heartbeat), GRANT 300 ms after that: 1.2 s from the press, inside the reply window counted from the publish -> call (the old window from the enqueue failed it)";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  pttDown(); tick();
  runFor(900);
  workerNext(true);
  workerNext(true);
  tick();
  sidQ = RadioTransport::g_call.session_id;
  runFor(300);
  bool stillClaiming = RadioTransport::getPrivateState() == RadioTransport::PrivateState::CLAIMING &&
                       RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NONE;
  peerReply(0, sidQ); tick();
  CHECK(stillClaiming && RadioTransport::getPrivateState() == RadioTransport::PrivateState::NEGOTIATING,
        "at 1.2 s still claiming %d; after GRANT state %u", stillClaiming,
        static_cast<unsigned>(RadioTransport::getPrivateState()));
  pttUp(1300); tick(2);

  g_test = "RQ2b CLAIM published late, then no reply: 'No response' 1 s after the publish (not after the press), retries every 250 ms once published";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  pttDown(); tick();
  runFor(800);
  workerNext(true);
  workerNext(true);
  tick();
  sidQ = RadioTransport::g_call.session_id;
  runFor(900);
  bool notYet = RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NONE;
  runFor(200);
  CHECK(notYet && RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::NO_RESPONSE &&
            status() == "No response" && wireCount(busyTo, 0, sidQ) >= 4 && wireCount(busyTo, 1, sidQ) == 1,
        "900 ms after the publish no failure yet %d; at 1.1 s failure %u status '%s'; CLAIMs on the wire %d, RELEASE %d",
        notYet, static_cast<unsigned>(RadioTransport::getPrivateFailure()), status().c_str(), wireCount(busyTo, 0, sidQ),
        wireCount(busyTo, 1, sidQ));
  pttUp(2000); tick(2);

  g_test = "RQ3 CLAIM still queued when the 2 s queue limit runs out: 'Send timeout' while held (not 'No response'); the RELEASE queues BEHIND the CLAIM (same session); when the worker gets to them they go out in that order; the receiver's GRANT is then a late reply, never acted on";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  pttDown(); tick();
  sidQ = RadioTransport::g_call.session_id;
  runFor(1900);
  bool claimingAt19 = RadioTransport::getPrivateState() == RadioTransport::PrivateState::CLAIMING;
  runFor(200);
  bool sendTimeout = RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::SEND_TIMEOUT &&
                     status() == "Send timeout" && wireCount(busyTo) == 0;
  bool orderInChain = T::asyncPending.size() == 3 && typeOf(T::Pub{T::asyncPending[1].topic, 0, true, false, "", T::asyncPending[1].data, false}) == 0 &&
                      typeOf(T::Pub{T::asyncPending[2].topic, 0, true, false, "", T::asyncPending[2].data, false}) == 1;
  runFor(2000);  // heartbeat PUBACK only at 4.1 s (degraded link)
  workerNext(true);
  workerNext(true);
  workerNext(true);
  tick();
  bool wireOrder = g_wire.size() >= 2 && wType(g_wire[g_wire.size() - 2]) == 0 && wType(g_wire.back()) == 1 &&
                   wSession(g_wire[g_wire.size() - 2]) == sidQ && wSession(g_wire.back()) == sidQ;
  peerReply(0, sidQ); tick();  // the receiver granted the late CLAIM
  CHECK(claimingAt19 && sendTimeout && orderInChain && wireOrder &&
            RadioTransport::getPrivateState() == RadioTransport::PrivateState::IDLE && g_capture == nullptr &&
            status() == "Send timeout",
        "claiming at 1.9 s %d; Send timeout at 2.1 s %d (failure %u, status '%s', on wire %d); chain order CLAIM,RELEASE %d; wire order %d; after late GRANT state %u",
        claimingAt19, sendTimeout, static_cast<unsigned>(RadioTransport::getPrivateFailure()), status().c_str(),
        wireCount(busyTo), orderInChain, wireOrder, static_cast<unsigned>(RadioTransport::getPrivateState()));
  pttUp(4500); tick(2);
  CHECK(status() == "Ready", "released: '%s'", status().c_str());

  g_test = "RQ4 several jobs ahead (heartbeat, chat, Outbox) and the PTT released while the CLAIM waits: the chain is full (4), the RELEASE is OWED and goes out right after the CLAIM is published -- CLAIM then RELEASE, same session";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  lendForWorkerPublish("msg/AABBCCDDEEF0");
  lendForWorkerPublish("msg/AABBCCDDEEF1");
  pttDown(); tick();
  sidQ = RadioTransport::g_call.session_id;
  bool fourth = queuedTo(busyTo) == 1 && T::asyncPending.size() == 4;
  runFor(300);
  pttUp(300); tick(2);
  bool owed = RadioTransport::g_owedRelease != nullptr && queuedTo(busyTo) == 1;
  workerNext(true); workerNext(true); workerNext(true);  // heartbeat, chat, Outbox
  bool noReleaseYet = wireCount(busyTo, 1) == 0;
  workerNext(true);  // the CLAIM: its result sends the owed RELEASE
  tick();
  bool claimThenRelease = g_wire.size() >= 2 && wType(g_wire[g_wire.size() - 2]) == 0 && wType(g_wire.back()) == 1 &&
                          wSession(g_wire.back()) == sidQ && wSession(g_wire[g_wire.size() - 2]) == sidQ;
  CHECK(fourth && owed && noReleaseYet && claimThenRelease && RadioTransport::g_owedRelease == nullptr && status() == "Ready",
        "CLAIM 4th in the chain %d; RELEASE owed %d; none before the CLAIM %d; CLAIM then RELEASE (same session) %d; owed cleared %d; status '%s'",
        fourth, owed, noReleaseYet, claimThenRelease, RadioTransport::g_owedRelease == nullptr, status().c_str());

  g_test = "RQ4b an owed RELEASE is dropped when the queued CLAIM failed (the connection closed: the CLAIM never reached the receiver)";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  lendForWorkerPublish("msg/AABBCCDDEEF0");
  lendForWorkerPublish("msg/AABBCCDDEEF1");
  pttDown(); tick();
  pttUp(100); tick(2);
  bool owed2 = RadioTransport::g_owedRelease != nullptr;
  workerNext(false); workerNext(false); workerNext(false); workerNext(false);
  tick();
  CHECK(owed2 && RadioTransport::g_owedRelease == nullptr && wireCount(busyTo) == 0,
        "owed %d, cleared %d, nothing on the wire %d", owed2, RadioTransport::g_owedRelease == nullptr, wireCount(busyTo));

  g_test = "RQ5 worker publish failure after a successful enqueue: counted as a send failure (not published, not 'No response'); with the connection gone the retries fail too -> 'Send failed'";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  pttDown(); tick();
  runFor(300);
  T::connected = false;  // the failed heartbeat closed the connection (MQTT 2.5.2)
  workerNext(false);
  workerNext(false);
  tick();
  bool countedFail = RadioTransport::g_call.claimSendFails >= 1 && RadioTransport::g_call.claimPublished == 0 &&
                     RadioTransport::g_call.claimQueuedTicket == 0;
  runFor(900);
  CHECK(countedFail && RadioTransport::getPrivateFailure() == RadioTransport::PrivateFailure::SEND_FAILED &&
            status() == "Send failed",
        "counted as failed %d (fails %u published %u); failure %u status '%s'", countedFail,
        static_cast<unsigned>(RadioTransport::g_call.claimSendFails), static_cast<unsigned>(RadioTransport::g_call.claimPublished),
        static_cast<unsigned>(RadioTransport::getPrivateFailure()), status().c_str());
  T::connected = true;
  pttUp(1300); tick(2);

  g_test = "RQ6 session identity: a press released while its CLAIM is queued, then a new press at once: the old CLAIM's publish does not count for the new session; wire order CLAIM(s1), RELEASE(s1), CLAIM(s2)";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  pttDown(); tick();
  std::string s1 = RadioTransport::g_call.session_id;
  runFor(100);
  pttUp(100); tick(2);
  pttDown(); tick();
  std::string s2 = RadioTransport::g_call.session_id;
  workerNext(true);  // heartbeat
  workerNext(true);  // CLAIM s1
  tick();
  bool s2NotPublished = RadioTransport::g_call.claimPublishedMs == 0 && RadioTransport::g_call.claimQueuedTicket != 0;
  workerNext(true);  // RELEASE s1
  workerNext(true);  // CLAIM s2
  tick();
  bool order = g_wire.size() == 4 && wType(g_wire[1]) == 0 && wSession(g_wire[1]) == s1 && wType(g_wire[2]) == 1 &&
               wSession(g_wire[2]) == s1 && wType(g_wire[3]) == 0 && wSession(g_wire[3]) == s2;
  CHECK(s1 != s2 && s2NotPublished && order && RadioTransport::g_call.claimPublishedMs != 0,
        "sessions differ %d; s2 not published by s1's CLAIM %d; wire order %d (wire %zu); s2 published %d", s1 != s2,
        s2NotPublished, order, g_wire.size(), RadioTransport::g_call.claimPublishedMs != 0);
  peerReply(0, s1); tick();  // a GRANT for the released session: ignored
  bool ignoredOld = RadioTransport::getPrivateState() == RadioTransport::PrivateState::CLAIMING;
  peerReply(0, s2); tick();
  CHECK(ignoredOld && RadioTransport::getPrivateState() == RadioTransport::PrivateState::NEGOTIATING,
        "GRANT(s1) ignored %d; GRANT(s2) -> state %u", ignoredOld, static_cast<unsigned>(RadioTransport::getPrivateState()));
  pttUp(500); tick(2);

  g_test = "RQ7 receiver side, delayed GRANT: the receiver's own connection is lent; the GRANT is QUEUED (with the session) and goes out when the worker gets to it";
  openTalk(kPeer);
  lendForWorkerPublish("race/state");
  deliver(PacketCodec::PK_RADIO_BUSY, idPayload(0, kPeer, "S-LATE"));
  const std::string replyTo = std::string("radio/busy_reply/") + kPeer;
  bool grantQueued = queuedTo(replyTo) == 1 && wireCount(replyTo) == 0 && RadioTransport::g_replyTicket != 0;
  workerNext(true);
  workerNext(true);
  tick();
  CHECK(grantQueued && wireCount(replyTo, 0, "S-LATE") == 1 && RadioTransport::g_replyTicket == 0,
        "GRANT queued (not on the wire) %d; after the worker: on the wire %d", grantQueued, wireCount(replyTo, 0, "S-LATE"));



  // ===========================================================================
  // Multi-session cancellation: several sessions abandoned while their
  // CLAIMs are still queued and their RELEASEs could not be queued
  // ===========================================================================
  g_test = "RQ8 two sessions released while queued with the chain full (the review's exact sequence): CLAIM(S1)..RELEASE(S1) and CLAIM(S2)..RELEASE(S2) both on the wire; replayed into the receiver, RELEASE(S1) never ends S2 and the receiver ends free";
  openTalk(kPeer);
  fillChain3();                       // 1. heartbeat, chat, Outbox
  pttDown(); tick();                  // 2. CLAIM(S1) = 4th
  std::string q1 = RadioTransport::g_call.session_id;
  bool c1Fourth = T::asyncPending.size() == 4 && queuedTo(busyTo) == 1;
  pttUp(100); tick(2);                // 3. RELEASE(S1) refused: chain full -> owed
  workerNext(true);                   // 4. heartbeat done: one slot, CLAIM(S1) still pending
  pttDown(); tick();                  // 5. CLAIM(S2) queued
  std::string q2 = RadioTransport::g_call.session_id;
  bool c2Queued = queuedTo(busyTo) == 2 && T::asyncPending.size() == 4;
  pttUp(100); tick(2);                // 6. RELEASE(S2) refused: chain full again
  for (int i = 0; i < 10 && !T::asyncPending.empty(); i++) { workerNext(true); tick(); }  // 7. everything completes
  std::string missing;
  bool paired = everyClaimReleased(&missing);
  bool freeAtEnd = false;
  std::vector<std::string> trace = replayToReceiver(&freeAtEnd);
  std::string tr;
  for (auto& t : trace) tr += t + " ";
  bool s1NeverEndsS2 = true;
  for (size_t i = 0; i + 1 < trace.size(); i++)
    if (trace[i].find("->" + q2) != std::string::npos && trace[i + 1].rfind("R(" + q1 + ")", 0) == 0 &&
        trace[i + 1].find("->" + q2) == std::string::npos)
      s1NeverEndsS2 = false;
  CHECK(c1Fourth && c2Queued && q1 != q2 && wireCount(busyTo, 0, q1) == 1 && wireCount(busyTo, 1, q1) == 1 &&
            wireCount(busyTo, 0, q2) == 1 && wireCount(busyTo, 1, q2) == 1 && paired && s1NeverEndsS2 && freeAtEnd &&
            RadioTransport::g_owedRelease == nullptr,
        "C1 4th %d; C2 queued %d; S1 claim/release %d/%d; S2 claim/release %d/%d; paired %d (missing '%s'); receiver: %s; free at end %d",
        c1Fourth, c2Queued, wireCount(busyTo, 0, q1), wireCount(busyTo, 1, q1), wireCount(busyTo, 0, q2),
        wireCount(busyTo, 1, q2), paired, missing.c_str(), tr.c_str(), freeAtEnd);

  g_test = "RQ8b three rapid PTT cycles, each released while its CLAIM is queued and the chain is full: every published CLAIM gets its own RELEASE, in order, and the receiver ends free";
  openTalk(kPeer);
  fillChain3();
  std::vector<std::string> sids;
  for (int cycle = 0; cycle < 3; cycle++) {
    pttDown(); tick();
    sids.push_back(RadioTransport::g_call.session_id);
    pttUp(80); tick(2);
    workerNext(true);  // one slot frees; the earlier CLAIMs stay pending
  }
  for (int i = 0; i < 12 && !T::asyncPending.empty(); i++) { workerNext(true); tick(); }
  paired = everyClaimReleased(&missing);
  trace = replayToReceiver(&freeAtEnd);
  int nClaims = 0, nReleases = 0;
  for (auto& sidc : sids) { nClaims += wireCount(busyTo, 0, sidc); nReleases += wireCount(busyTo, 1, sidc); }
  tr.clear();
  for (auto& t : trace) tr += t + " ";
  CHECK(sids.size() == 3 && sids[0] != sids[1] && sids[1] != sids[2] && nClaims == nReleases && nClaims >= 2 && paired && freeAtEnd &&
            RadioTransport::g_owedRelease == nullptr,
        "claims %d releases %d paired %d (missing '%s') receiver: %s free %d", nClaims, nReleases, paired, missing.c_str(), tr.c_str(),
        freeAtEnd);


  g_test = "RQ8c two sessions to DIFFERENT peers, both released while queued with the chain full: without RELEASE(S1) peer 1 would stay Busy (TTL only); both RELEASEs go out, each after its own CLAIM";
  openTalk(kPeer);
  fillChain3();
  pttDown(); tick();
  std::string p1sid = RadioTransport::g_call.session_id;
  pttUp(100); tick(2);
  workerNext(true);
  openTalkKeepChain(kOther);
  pttDown(); tick();
  std::string p2sid = RadioTransport::g_call.session_id;
  pttUp(100); tick(2);
  uint8_t owedBoth = RadioTransport::owedReleaseCount();
  for (int i = 0; i < 10 && !T::asyncPending.empty(); i++) { workerNext(true); tick(); }
  const std::string busyOther = std::string("radio/busy/") + kOther;
  paired = everyClaimReleased(&missing);
  CHECK(owedBoth == 2 && wireCount(busyTo, 0, p1sid) == 1 && wireCount(busyTo, 1, p1sid) == 1 && wireCount(busyOther, 0, p2sid) == 1 &&
            wireCount(busyOther, 1, p2sid) == 1 && paired && RadioTransport::g_owedRelease == nullptr,
        "owed at once %u (want 2); peer1 C/R %d/%d; peer2 C/R %d/%d; paired %d (missing '%s')", owedBoth, wireCount(busyTo, 0, p1sid),
        wireCount(busyTo, 1, p1sid), wireCount(busyOther, 0, p2sid), wireCount(busyOther, 1, p2sid), paired, missing.c_str());

  g_test = "RQ8d failed worker publishes / canceled loans (Wi-Fi recovery, OTA maintenance cancel the loan: every queued job reports failure): no CLAIM reached the receiver, so the owed RELEASEs are dropped (logged as not delivered), none sent, the list empties";
  openTalk(kPeer);
  fillChain3();
  for (int cycle = 0; cycle < 2; cycle++) {
    pttDown(); tick();
    pttUp(80); tick(2);
    if (cycle == 0) workerNext(true);
  }
  uint8_t owedBefore = RadioTransport::owedReleaseCount();
  T::connected = false;  // the loan canceled / connection gone
  for (int i = 0; i < 10 && !T::asyncPending.empty(); i++) { workerNext(false); tick(); }
  T::connected = true;
  CHECK(owedBefore == 2 && RadioTransport::g_owedRelease == nullptr && wireCount(busyTo, 0) == 0 && wireCount(busyTo, 1) == 0,
        "owed %u -> list empty %d; CLAIMs on wire %d, RELEASEs %d", owedBefore, RadioTransport::g_owedRelease == nullptr,
        wireCount(busyTo, 0), wireCount(busyTo, 1));

  g_test = "RQ8e OTA maintenance entered while a CLAIM is queued and the chain is full: the forced stop owes the RELEASE; if the CLAIM is still written, its RELEASE follows; a later session is unaffected";
  openTalk(kPeer);
  fillChain3();
  pttDown(); tick();
  std::string msid = RadioTransport::g_call.session_id;
  RadioTransport::setMaintenanceModeActive(true);
  bool owedOnMaint = RadioTransport::owedReleaseCount() == 1 && RadioTransport::getPrivateState() == RadioTransport::PrivateState::IDLE;
  for (int i = 0; i < 10 && !T::asyncPending.empty(); i++) { workerNext(true); tick(); }
  RadioTransport::setMaintenanceModeActive(false);
  CHECK(owedOnMaint && wireCount(busyTo, 0, msid) == 1 && wireCount(busyTo, 1, msid) == 1 && RadioTransport::g_owedRelease == nullptr,
        "owed on maintenance %d; C/R %d/%d", owedOnMaint, wireCount(busyTo, 0, msid), wireCount(busyTo, 1, msid));
  pttUp(50); tick(2);

  g_test = "RQ8f low memory: the owed entry cannot be allocated -> that RELEASE is reported LOST (explicitly, not silently), nothing crashes, an earlier session's owed RELEASE is kept and still sent";
  openTalk(kPeer);
  fillChain3();
  pttDown(); tick();
  std::string lm1 = RadioTransport::g_call.session_id;
  pttUp(80); tick(2);             // S1 owed (memory available)
  workerNext(true);
  pttDown(); tick();
  std::string lm2 = RadioTransport::g_call.session_id;
  g_failNothrowNew = true;
  int failsBefore = g_nothrowNewFailures;
  pttUp(80); tick(2);             // S2: no memory for the entry
  g_failNothrowNew = false;
  bool allocTried = g_nothrowNewFailures == failsBefore + 1 && RadioTransport::owedReleaseCount() == 1;
  for (int i = 0; i < 10 && !T::asyncPending.empty(); i++) { workerNext(true); tick(); }
  CHECK(allocTried && wireCount(busyTo, 1, lm1) == 1 && wireCount(busyTo, 0, lm2) == 1 && wireCount(busyTo, 1, lm2) == 0 &&
            RadioTransport::g_owedRelease == nullptr,
        "allocation refused once, S1 entry kept %d; RELEASE(S1) sent %d; S2 CLAIM %d, RELEASE(S2) lost %d", allocTried,
        wireCount(busyTo, 1, lm1), wireCount(busyTo, 0, lm2), wireCount(busyTo, 1, lm2) == 0);

  g_test = "RQ8g bounded: at most kMaxOwedReleases (8) entries; beyond that a RELEASE is reported lost instead of allocating";
  openTalk(kPeer);
  T::asyncPublish = true;
  T::asyncChainCap = 12;  // a model chain long enough to hold more abandoned CLAIMs than the bound
  {
    uint8_t b[4] = {1, 2, 3, 4};
    for (int i = 0; i < 2; i++) MqttManager::publishBinaryAsync(kGroup, "race/state", b, 4, false, 1, nullptr);
    int maxOwed = 0;
    for (int cycle = 0; cycle < 10; cycle++) {
      pttDown(); tick();
      // keep the chain full so each RELEASE is refused
      while (static_cast<int>(T::asyncPending.size()) < T::asyncChainCap) MqttManager::publishBinaryAsync(kGroup, "race/state", b, 4, false, 1, nullptr);
      pttUp(50); tick(2);
      if (RadioTransport::owedReleaseCount() > maxOwed) maxOwed = RadioTransport::owedReleaseCount();
      workerNext(true);  // the oldest filler done: one slot for the next CLAIM
      while (!T::asyncPending.empty() && T::asyncPending.front().topic == "race/state" && static_cast<int>(T::asyncPending.size()) >= T::asyncChainCap) workerNext(true);
    }
    for (int i = 0; i < 40 && !T::asyncPending.empty(); i++) { workerNext(true); tick(); }
    CHECK(maxOwed == 8 && RadioTransport::g_owedRelease == nullptr, "max entries %d (want 8); list empty after %d",
          maxOwed, RadioTransport::g_owedRelease == nullptr);
  }
  T::asyncChainCap = 4;


  g_test = "RQ8h sessions on two Family Groups (separate chains), both released while queued with each chain full; group 1's chain completes first: RELEASE(S1) goes out on group 1, RELEASE(S2) on group 2 once its CLAIM is out -- each owed RELEASE bound to its own CLAIM";
  openTalk(kPeer);
  {
    const char* kGroup2 = "GROUP2";
    T::asyncPublish = true;
    uint8_t b[4] = {1, 2, 3, 4};
    for (int i = 0; i < 3; i++) MqttManager::publishBinaryAsync(kGroup, "race/state", b, 4, false, 1, nullptr);
    for (int i = 0; i < 3; i++) MqttManager::publishBinaryAsync(kGroup2, "race/state", b, 4, false, 1, nullptr);
    pttDown(); tick();
    std::string g1sid = RadioTransport::g_call.session_id;
    pttUp(80); tick(2);  // owed (group 1 full)
    strcpy(g_selectedGroupCode, kGroup2);
    openTalkKeepChain(kPeer);
    pttDown(); tick();
    std::string g2sid = RadioTransport::g_call.session_id;
    pttUp(80); tick(2);  // owed (group 2 full)
    strcpy(g_selectedGroupCode, kGroup);
    uint8_t owed2 = RadioTransport::owedReleaseCount();
    while (pendingFor(kGroup) > 0) { workerNextFor(kGroup, true); tick(); }  // group 1 finishes; group 2 still full
    bool g1Released = wireCount(busyTo, 1, g1sid) == 1 && wireCount(busyTo, 1, g2sid) == 0;
    while (pendingFor(kGroup2) > 0) { workerNextFor(kGroup2, true); tick(); }
    paired = everyClaimReleased(&missing);
    CHECK(owed2 == 2 && g1Released && paired && wireCount(busyTo, 1, g2sid) == 1 && RadioTransport::g_owedRelease == nullptr,
          "owed %u; after group 1: RELEASE(S1) %d RELEASE(S2) %d (want 1/0); paired %d (missing '%s'); RELEASE(S2) at the end %d",
          owed2, wireCount(busyTo, 1, g1sid), wireCount(busyTo, 1, g2sid), paired, missing.c_str(), wireCount(busyTo, 1, g2sid));
  }

  // ===========================================================================
  // Gate 3: the three users of publishAudioPacket() -- framing, topic, the
  // receiver's dispatch by scope, drops and recovery
  // ===========================================================================
  g_test = "RA1 framing: a Race Room voice frame (race.cpp's call: radio/audio/broadcast, AUDIO_RACE_ROOM, room key) is one PK_RADIO_AUDIO packet of header + scope + sender + key + seq + length + 640 B PCM; delivered back it reaches the AUDIO_RACE_ROOM handler with the same key, sender, sequence and samples";
  openTalk(kPeer);
  static std::string rxKey, rxSender;
  static uint16_t rxSeq = 0;
  static std::vector<int16_t> rxPcm;
  static int rxCount = 0;
  rxCount = 0;
  RadioTransport::registerAudioScopeHandler(RadioTransport::AUDIO_RACE_ROOM,
                                            [](const char*, const char* sender, const char* key, uint16_t seq, const int16_t* pcm, size_t n) {
                                              rxKey = key; rxSender = sender; rxSeq = seq; rxPcm.assign(pcm, pcm + n); rxCount++;
                                            });
  int16_t voice[RadioAudio::kFrameSamples];
  for (size_t i = 0; i < RadioAudio::kFrameSamples; i++) voice[i] = static_cast<int16_t>(i * 97 - 15000);
  bool sentRace = RadioTransport::publishAudioPacket(kGroup, "radio/audio/broadcast", RadioTransport::AUDIO_RACE_ROOM,
                                                     "C0EE0000000100ABCDEF", 7, voice, RadioAudio::kFrameSamples);
  bool raceFrame = sentRace && g_noWait.size() == 1 && g_noWait[0].topic == "radio/audio/broadcast" && g_noWait[0].group == kGroup &&
                   g_noWait[0].len == PacketCodec::kHeaderSize + 1 + 16 + 24 + 2 + 2 + 640;
  PacketCodec::Header h;
  const uint8_t* body = nullptr;
  bool decoded = raceFrame && PacketCodec::decodeHeader(g_noWait[0].data.data(), g_noWait[0].data.size(), &h, &body) &&
                 h.packet_kind == PacketCodec::PK_RADIO_AUDIO;
  if (decoded) deliver(PacketCodec::PK_RADIO_AUDIO, std::vector<uint8_t>(body, body + h.payload_length));
  bool roundTrip = rxCount == 1 && rxKey == "C0EE0000000100ABCDEF" && rxSender == "DEADBEEFAAAA" && rxSeq == 7 &&
                   rxPcm.size() == RadioAudio::kFrameSamples && std::equal(rxPcm.begin(), rxPcm.end(), voice);
  CHECK(raceFrame && decoded && roundTrip, "frame on radio/audio/broadcast (%zu bytes) %d; decodes as PK_RADIO_AUDIO %d; round trip to the room handler (key/sender/seq/PCM) %d",
        g_noWait.empty() ? 0 : g_noWait[0].len, raceFrame, decoded, roundTrip);

  g_test = "RA2 drops and recovery, all three paths (Race voice, broadcast, private relay): a skipped frame returns false, is attempted once and never resent; after congestion the next frame goes out with its own (advanced) sequence";
  g_noWait.clear();
  uint16_t seq = 0;
  int raceOk = 0, raceSkipped = 0;
  for (int i = 0; i < 6; i++) {
    g_noWaitAccepts = !(i >= 2 && i < 4);
    bool ok = RadioTransport::publishAudioPacket(kGroup, "radio/audio/broadcast", RadioTransport::AUDIO_RACE_ROOM, "K", seq++, voice,
                                                 RadioAudio::kFrameSamples);
    (ok ? raceOk : raceSkipped)++;
  }
  g_noWaitAccepts = true;
  bool oneAttemptEach = g_noWait.size() == 6;
  rxCount = 0;
  for (auto& w : g_noWait) {
    if (!w.result) continue;
    if (PacketCodec::decodeHeader(w.data.data(), w.data.size(), &h, &body))
      deliver(PacketCodec::PK_RADIO_AUDIO, std::vector<uint8_t>(body, body + h.payload_length));
  }
  bool lastSeq5 = rxCount == 4 && rxSeq == 5;
  // broadcast (Everyone) and private relay use the same function with their own scope/topic
  g_noWait.clear();
  g_noWaitAccepts = false;
  bool bSkip = !RadioTransport::publishAudioPacket(kGroup, "radio/audio/broadcast", RadioTransport::AUDIO_BROADCAST, nullptr, 0, voice,
                                                   RadioAudio::kFrameSamples);
  g_noWaitAccepts = true;
  bool bOk = RadioTransport::publishAudioPacket(kGroup, "radio/audio/broadcast", RadioTransport::AUDIO_BROADCAST, nullptr, 1, voice,
                                                RadioAudio::kFrameSamples);
  std::string privTopic = std::string("radio/audio/") + kPeer;
  bool pOk = RadioTransport::publishAudioPacket(kGroup, privTopic.c_str(), RadioTransport::AUDIO_PRIVATE, nullptr, 0, voice,
                                                RadioAudio::kFrameSamples);
  bool scopes = g_noWait.size() == 3 && g_noWait[2].topic == privTopic && g_noWait[1].data.size() > 6 &&
                g_noWait[1].data[PacketCodec::kHeaderSize] == RadioTransport::AUDIO_BROADCAST &&
                g_noWait[2].data[PacketCodec::kHeaderSize] == RadioTransport::AUDIO_PRIVATE;
  CHECK(raceOk == 4 && raceSkipped == 2 && oneAttemptEach && lastSeq5 && bSkip && bOk && pOk && scopes,
        "race: sent %d skipped %d, one attempt per frame %d, received 4 with the last seq 5 %d; broadcast skip/recover %d/%d; private topic+scopes %d/%d",
        raceOk, raceSkipped, oneAttemptEach, lastSeq5, bSkip, bOk, pOk, scopes);

  g_test = "RA3 receiver dispatch by scope: broadcast frames play; private relay frames play only from the call's peer while in the relay; a Race Room frame never reaches the broadcast or private players";
  g_framesPlayed = 0;
  auto frameFor = [&](RadioTransport::AudioScope scope, const char* sender) {
    std::vector<uint8_t> p(1 + 16 + 24 + 2 + 2 + 640, 0);
    p[0] = scope;
    strncpy(reinterpret_cast<char*>(&p[1]), sender, 15);
    p[1 + 16 + 24 + 2] = 640 & 0xFF;  // PCM byte count, little-endian (radio_transport.cpp readU16())
    p[1 + 16 + 24 + 3] = 640 >> 8;
    return p;
  };
  deliver(PacketCodec::PK_RADIO_AUDIO, frameFor(RadioTransport::AUDIO_BROADCAST, kOther));
  int bPlayed = g_framesPlayed;
  RadioTransport::g_call.state = RadioTransport::PrivateState::MQTT_FALLBACK;
  strcpy(RadioTransport::g_call.peer_device_id, kPeer);
  deliver(PacketCodec::PK_RADIO_AUDIO, frameFor(RadioTransport::AUDIO_PRIVATE, kPeer));
  int pPlayed = g_framesPlayed - bPlayed;
  deliver(PacketCodec::PK_RADIO_AUDIO, frameFor(RadioTransport::AUDIO_PRIVATE, kOther));
  int pOther = g_framesPlayed - bPlayed - pPlayed;
  rxCount = 0;
  int playedBefore = g_framesPlayed;
  deliver(PacketCodec::PK_RADIO_AUDIO, frameFor(RadioTransport::AUDIO_RACE_ROOM, kOther));
  CHECK(bPlayed == 1 && pPlayed == 1 && pOther == 0 && rxCount == 1 && g_framesPlayed == playedBefore,
        "broadcast played %d; private from peer %d, from another %d; race frame to the room handler only %d (players +%d)", bPlayed,
        pPlayed, pOther, rxCount, g_framesPlayed - playedBefore);
  RadioTransport::resetPrivateCall();

  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
