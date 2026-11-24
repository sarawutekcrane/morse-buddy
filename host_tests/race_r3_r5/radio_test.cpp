// HOST TEST (not production). Private radio PTT and radio MQTT paths
// (hardware, devices A on 827cd27 and B on b573ec7: holding PTT,
// "Claiming..." went back to "Ready" before the button was released;
// "[PERF][RADIO_AUDIO] serviceTick 289 ms" with "[DIAG][MQTT] qos0 ...
// maxMs=288"). Compiles the ACTUAL src/core/radio.cpp (Talk screen),
// radio_transport.cpp (claim/grant/deny, negotiation, MQTT fallback,
// broadcast audio) and menu.cpp into this translation unit, against the
// shared stubs (stubs.cpp: Display, Input, MqttManager publishBinary()).
// The peer is modeled by delivering its packets to the registered handlers.
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
struct NoWaitPub { std::string topic; size_t len; bool result; };
static std::vector<NoWaitPub> g_noWait;
static bool g_noWaitAccepts = true;
namespace MqttManager {
bool publishBinaryNoWait(const char*, const char* topic, const uint8_t*, uint16_t len) {
  bool r = T::connected && g_noWaitAccepts;
  g_noWait.push_back({topic, len, r});
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
  runFor(1100);
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

  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
