// HOST TEST STUBS (not production).
#include "stubs.h"
#include <Arduino.h>
#include <algorithm>
#include "core/display.h"
#include "core/menu.h"
#include "core/enigma.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/identity_color.h"
#include "core/morse.h"
#include "core/mqtt_manager.h"
#include "core/notifications.h"
#include "core/packet_codec.h"
#include "core/presence.h"
#include "core/settings.h"
#include "core/storage_init.h"
#include "core/text_message.h"
#include "core/ui_scratch.h"
#include "core/wifi_manager.h"
#include "font_adv.h"

namespace T {
uint32_t now = 1000;
std::vector<Draw> screen, drawLog;
std::deque<InputEvent> inputQ, raw;
int updateCount = 0;
bool connected = true;
std::deque<bool> publishScript;
std::vector<Pub> pubs;
std::function<void()> duringPublish;
std::function<void(const Pub&)> onPublished;
std::vector<Rec> store;
bool appendResult = true, updateResult = true;
int appendCalls = 0, updateCalls = 0;
std::function<void()> duringUpdate;
std::vector<std::string> seq;
int notifications = 0;
std::vector<std::string> indexLoads;
uint32_t unixTime = 1700000000u;
std::function<void(const std::string&)> onDraw;
void reset() {
  screen.clear(); drawLog.clear(); inputQ.clear(); raw.clear(); updateCount = 0;
  connected = true; publishScript.clear(); pubs.clear(); duringPublish = nullptr; onPublished = nullptr;
  store.clear(); appendResult = updateResult = true; appendCalls = updateCalls = 0; duringUpdate = nullptr;
  seq.clear(); notifications = 0; indexLoads.clear(); onDraw = nullptr; unixTime = 1700000000u;
}
std::string visibleJoined() { std::string s; for (auto& d : screen) { s += d.text; s += '|'; } return s; }
std::string rowAt(int y) {
  std::vector<Draw> r; for (auto& d : screen) if (d.y == y) r.push_back(d);
  std::sort(r.begin(), r.end(), [](const Draw& a, const Draw& b) { return a.x < b.x; });
  std::string out; for (auto& d : r) { if (!out.empty()) out += ' '; out += d.text; } return out;
}
uint16_t colorOf(const std::string& text, int y) { for (auto& d : screen) if (d.text == text && d.y == y) return d.color; return 0; }
bool visible(const std::string& s) { for (auto& d : screen) if (d.text == s) return true; return false; }
}  // namespace T

uint32_t millis() { return T::now; }
long random(long m) { static long s = 7; s = (s * 1103515245 + 12345) & 0x7fffffff; return s % m; }

static void eraseRect(int x, int y, int w, int h) {
  T::screen.erase(std::remove_if(T::screen.begin(), T::screen.end(), [&](const Draw& d) {
    return d.x >= x && d.x < x + w && d.y >= y && d.y < y + h; }), T::screen.end());
}
void Adafruit_ST7789::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t) { eraseRect(x, y, w, h); }
static Adafruit_ST7789 g_tft;
static void draw(int x, int y, const char* t, uint16_t color = 0xFFFF) {
  Draw d{T::now, x, y, t, color};
  T::screen.push_back(d); T::drawLog.push_back(d); T::seq.push_back(std::string("D:") + t);
  if (T::onDraw) { auto f = T::onDraw; f(t); }
}
namespace Display {
Adafruit_ST7789& tft() { return g_tft; }
void clearContentArea() { eraseRect(0, kStatusBarHeight, kScreenWidth, kScreenHeight); T::drawLog.push_back({T::now, 0, kStatusBarHeight, "#CLEAR"}); }
void drawStatusBar() {}
void setFont(Font) {}
int16_t lineHeight() { return 23; }  // FreeSans9pt7b: ink 17 + 2/5 leading (display.cpp computePrimaryMetrics)
int16_t textWidth(const char* t) { int w = 0; for (; *t; t++) if (*t >= 0x20 && *t <= 0x7E) w += kAdv[*t - 0x20]; return static_cast<int16_t>(w); }
void printLine(int16_t x, int16_t y, const char* t) { draw(x, y, t); }
void printLineColored(int16_t x, int16_t y, const char* t, uint16_t c) { draw(x, y, t, c); }
void drawLockIcon(int16_t, int16_t, bool, uint16_t) {}
void drawSelectionCursor(int16_t x, int16_t y) { eraseRect(x, y, 1, 1); T::screen.push_back({T::now, x, y, "#CURSOR"}); T::drawLog.push_back({T::now, x, y, "#CURSOR"}); }
void drawSenderDivider(int16_t x, int16_t y, const char* t, uint16_t) { draw(x, y, t); }
uint8_t wrapText(const char* text, int16_t, uint16_t* starts, uint16_t* lens, uint8_t maxLines) {
  if (maxLines == 0) return 0; starts[0] = 0; lens[0] = static_cast<uint16_t>(strlen(text)); return 1;
}
bool wrapLineAt(const char* text, size_t from, int16_t, uint16_t* s, uint16_t* l) {
  size_t n = strlen(text); if (from >= n) return false; *s = static_cast<uint16_t>(from); *l = static_cast<uint16_t>(n - from); return true;
}
}  // namespace Display

namespace Input {
void update() { T::updateCount++; while (!T::raw.empty()) { T::inputQ.push_back(T::raw.front()); T::raw.pop_front(); } }
bool popEvent(InputEvent& e) { if (T::inputQ.empty()) { e = InputEvent{}; return false; } e = T::inputQ.front(); T::inputQ.pop_front(); return true; }
// Same predicates as src/core/input.cpp:843-849.
bool isMenuConfirm(const InputEvent& e) { return e.type == InputEventType::DOT_RELEASE && e.durationMs < Morse::kSpecialCommandMs; }
bool isBack(const InputEvent& e) { return e.type == InputEventType::ENCODER_LONG; }
}  // namespace Input

namespace MqttManager {
bool isGroupConnected(const char*) { return T::connected; }
bool isGroupReady(const char*) { return T::connected; }
bool publishRaw(const char*, const char* topic, const char* payload, bool retained, int qos) {
  T::seq.push_back(std::string("publishRaw ") + topic + " " + payload + (retained ? " R" : " -") + std::to_string(qos));
  return T::connected;
}
bool publishBinary(const char* group, const char* topic, const uint8_t* data, uint16_t len, bool retained, int) {
  std::vector<uint8_t> bytes(data, data + len);
  if (!T::connected) { T::pubs.push_back({topic, len, false, true, group, bytes, retained}); T::seq.push_back("publish:rejected"); return false; }
  if (T::duringPublish) T::duringPublish();
  bool r = true;
  if (!T::publishScript.empty()) { r = T::publishScript.front(); T::publishScript.pop_front(); }
  T::pubs.push_back({topic, len, r, false, group, bytes, retained});
  T::seq.push_back(r ? "publish:true" : "publish:false");
  if (!r) T::connected = false;  // MQTT 2.5.2: a failed publish closes the connection
  if (r && T::onPublished) { T::Pub p = T::pubs.back(); T::onPublished(p); }
  return r;
}
}  // namespace MqttManager

namespace MessageStore {
static uint32_t g_gen = 0;
static std::vector<ConversationIndexEntry> g_index;
static uint32_t g_seq = 100;
bool appendStoredMessage(const char* g, const char* c, Direction d, uint16_t flags, uint32_t ts, const uint8_t* wire,
                         uint16_t wireLen, const uint8_t* local, uint16_t localLen, MessageRef* out) {
  T::appendCalls++; T::seq.push_back("append");
  if (!T::appendResult) return false;
  T::Rec r{}; strncpy(r.ref.group_code, g, 32); strncpy(r.ref.contact_key, c, kContactKeyLen - 1); r.ref.sequence = ++g_seq;
  r.hdr.direction = d; r.hdr.flags = flags; r.hdr.effective_sort_timestamp = ts;
  r.wire.assign(wire, wire + wireLen); r.local.assign(local, local + localLen);
  PacketCodec::Header h; const uint8_t* p = nullptr; const uint8_t* tp = nullptr; uint16_t tl = 0;
  if (PacketCodec::decodeHeader(wire, wireLen, &h, &p) && PacketCodec::decodeMessageEnvelope(p, h.payload_length, &r.env, &tp, &tl))
    r.typePayload.assign(tp, tp + tl);
  T::store.push_back(r); if (out) *out = r.ref; g_gen++; return true;
}
static T::Rec* find(const MessageRef& ref) {
  for (auto& r : T::store) if (r.ref.sequence == ref.sequence && !strcmp(r.ref.group_code, ref.group_code) && !strcmp(r.ref.contact_key, ref.contact_key)) return &r;
  return nullptr;
}
bool loadMessage(const MessageRef& ref, StoredMessageView* v) {
  T::Rec* r = find(ref); if (!r) return false;
  v->ref = r->ref; v->header = r->hdr; v->envelope = r->env;
  v->wirePacket = r->wire.data(); v->wirePacketLen = static_cast<uint16_t>(r->wire.size());
  v->typePayload = r->typePayload.data(); v->typePayloadLen = static_cast<uint16_t>(r->typePayload.size());
  v->localPayload = r->local.data(); v->localPayloadLen = static_cast<uint16_t>(r->local.size());
  return true;
}
bool updateTypeLocalPayload(const MessageRef& ref, const uint8_t* local, uint16_t len) {
  T::updateCalls++; T::seq.push_back("update");
  if (T::duringUpdate) T::duringUpdate();
  if (!T::updateResult) return false;
  T::Rec* r = find(ref); if (!r) return false;
  r->local.assign(local, local + len); g_gen++; return true;
}
uint16_t loadConversationIndex(const char* g, const char* c) {
  T::indexLoads.push_back(std::string(g) + "/" + c);
  g_index.clear();
  for (auto& r : T::store) if (!strcmp(r.ref.group_code, g) && !strcmp(r.ref.contact_key, c)) {
    ConversationIndexEntry e{}; e.sequence = r.ref.sequence; e.effective_sort_timestamp = r.hdr.effective_sort_timestamp;
    strncpy(e.message_id, r.env.message_id, sizeof(e.message_id) - 1); e.flags = r.hdr.flags; g_index.push_back(e);
  }
  return static_cast<uint16_t>(g_index.size());
}
const ConversationIndexEntry* getIndexEntry(uint16_t i) { return i < g_index.size() ? &g_index[i] : nullptr; }
bool findMessageById(const char* g, const char* c, const char* id, MessageRef* out) {
  for (auto& r : T::store) if (!strcmp(r.ref.group_code, g) && !strcmp(r.ref.contact_key, c) && !strcmp(r.env.message_id, id)) { *out = r.ref; return true; }
  return false;
}
uint32_t getStorageChangeGeneration() { return g_gen; }
uint16_t findMessagesByPredicate(const char*, const char*, MessagePredicate, void*, MessageRef*, uint16_t, bool* rel) {
  if (rel) *rel = true; return 0;
}
bool isDuplicateAndRecord(const char*, const char*, const char*) { return false; }
void buildSenderPrefix(const PacketCodec::MessageEnvelope&, char* out, size_t n) { snprintf(out, n, "Friend"); }
}  // namespace MessageStore

#ifdef REAL_IDENTITY
// The Race suites link the ACTUAL src/core/identity.cpp (its persistent
// counter in Storage::core(), with fault injection); its MAC comes from here.
#include <WiFi.h>
HostWiFi WiFi;
#else
namespace Identity {
const char* deviceId() { return "DEADBEEFAAAA"; }  // 12 uppercase hex chars, like identity.cpp's MAC-derived id
static uint32_t g_counter = 0;
void nextId(char* out, size_t n) { snprintf(out, n, "ID%014u", ++g_counter); T::seq.push_back("nextId"); }
}
#endif
namespace IdentityColor { bool isValid(uint8_t i) { return i < kColorCount; } uint16_t color565(uint8_t) { return 0; } }
namespace Notifications {
void onMessageArrived(const char*, const char*, const MessageRef&, uint8_t, bool) { T::notifications++; }
void clearUnread(const char*, const char*, const MessageRef&) {}
}
namespace Settings {
FamilyGroup getGroup(uint8_t) { FamilyGroup g{}; g.configured = true; strcpy(g.name, "Fam"); strcpy(g.code, "GROUP1"); return g; }
uint8_t getGroupCount() { return 1; }
const char* getMyName() { return ""; }
uint8_t getMyColorIndex() { return IdentityColor::kInvalidColor; }
TypingDisplay getTypingDisplay() { return TypingDisplay::LETTERS_ONLY; }
uint8_t getWpm() { return 15; }
bool hasCustomMyName() { return false; }
void screenRoot() {}
void screenTrainingGame() {}
}
static Preferences g_prefs;
static Preferences g_racePrefs, g_corePrefs;
namespace Storage {
Preferences& core() { return g_corePrefs; } Preferences& solo() { return g_prefs; } Preferences& recent() { return g_prefs; } Preferences& enigma() { return g_prefs; } Preferences& race() { return g_racePrefs; } }
uint32_t esp_random() { static uint32_t s = 0x1234567u; s = s * 1664525u + 1013904223u; return s; }
HostSerial Serial;
void (*g_hostSerialSink)(const char*) = nullptr;
size_t g_fakeHeapFree = 0, g_fakeHeapMinFree = 0, g_fakeHeapLargest = 0;
uint32_t g_fakeHeapCapsSeen = 0;
int g_fakeHeapQueries = 0;
static char g_scratch[2][4096];
namespace UiScratch { char* ensure(Slot s, size_t) { return g_scratch[static_cast<int>(s)]; } }
namespace WifiManager { uint32_t getUnixTime() { return T::unixTime; } bool isNtpSynced() { return true; } }
static MessageEventFn g_eventFns[16]; static RenderFn g_renderFns[16]; static MessageIconFn g_iconFns[16];
bool registerMessageType(uint8_t t, RenderFn r, MessageEventFn e, MessageIconFn i) { g_renderFns[t & 15] = r; g_eventFns[t & 15] = e; g_iconFns[t & 15] = i; return true; }
RenderFn getMessageRenderFn(uint8_t t) { return g_renderFns[t & 15]; }
MessageEventFn getMessageEventFn(uint8_t t) { return g_eventFns[t & 15]; }
MessageIconFn getMessageIconFn(uint8_t t) { return g_iconFns[t & 15]; }
static ScreenHandlerFn g_modeHandlers[8];
bool registerModeHandler(uint8_t m, ScreenHandlerFn fn) { if (m < 8) g_modeHandlers[m] = fn; return true; }
ScreenHandlerFn getModeHandler(uint8_t m) { return m < 8 ? g_modeHandlers[m] : nullptr; }
static AppService g_services[16]; static int g_serviceCount = 0;
bool registerAppService(const AppService& s) { if (g_serviceCount < 16) g_services[g_serviceCount++] = s; return true; }
void T_initServices() { for (int i = 0; i < g_serviceCount; i++) if (g_services[i].init) g_services[i].init(); }
static SettingsChangeHook g_settingsHooks[8]; static int g_settingsHookCount = 0;
bool registerSettingsChangeHook(SettingsChangeHook fn) { if (g_settingsHookCount < 8) g_settingsHooks[g_settingsHookCount++] = fn; return true; }
void T_fireSettingsChange(const SettingsChangeInfo& info) { for (int i = 0; i < g_settingsHookCount; i++) g_settingsHooks[i](info); }
static EmptyLineActionFn g_emptyLine;
void registerEmptyLineAction(EmptyLineActionFn fn) { g_emptyLine = fn; }
void invokeEmptyLineAction(uint8_t m, const char* g, const char* c) { if (g_emptyLine) g_emptyLine(m, g, c); }
bool registerGameSubModeHandler(uint8_t, ScreenHandlerFn) { return true; }
bool registerNetworkPacketHandler(uint8_t, NetworkPacketHandlerFn) { return true; }
void setPendingRadioIntent(const char*, const char*) { T::seq.push_back("radioIntent"); }
