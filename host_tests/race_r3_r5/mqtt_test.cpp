// HOST TEST (not firmware): the MQTT connection worker in mqtt_manager.cpp
// against a scripted broker (fake_net/). ACTUAL mqtt_manager.cpp, crc32.cpp
// and packet_codec.cpp; Settings, hooks, Identity, Presence and WifiManager
// are small stubs below, and every call into them from any thread other than
// the main task is counted as a violation of the ownership rule.
//
//   mqtt_test manual      deterministic: the worker's jobs run when the test
//                         says (pumpWorker()), simulated time
//   mqtt_test threads N   a real worker thread, real time, gates holding the
//                         worker inside one network step, and an N-iteration
//                         randomized lifecycle stress (also built with TSan)
//
// Not covered here: ESP32 timing, lwIP/Wi-Fi behaviour, the real broker.
#include "fake_net.h"

#include <stdarg.h>
#include <stdio.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <string>
#include <vector>

// ---- Arduino surface (thread-safe: the worker thread reads the clock) ------
static std::atomic<uint32_t> g_simMs(100000);
static std::atomic<bool> g_realTime(false);
static const auto g_t0 = std::chrono::steady_clock::now();
uint32_t millis() {
  if (g_realTime.load())
    return static_cast<uint32_t>(
        100000 + std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_t0).count());
  return g_simMs.load();
}
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static std::atomic<uint32_t> g_rng(12345);
uint32_t esp_random() {
  uint32_t x = g_rng.load(), y;
  do {
    y = x;
    y ^= y << 13;
    y ^= y >> 17;
    y ^= y << 5;
  } while (!g_rng.compare_exchange_weak(x, y));
  return y;
}
long random(long maxExclusive) { return maxExclusive > 0 ? static_cast<long>(esp_random() % maxExclusive) : 0; }

#include "core/mqtt_manager.cpp"

// ---- ownership checks ---------------------------------------------------------
static std::thread::id g_mainThread = std::this_thread::get_id();
static std::atomic<int> g_offMainCalls(0);
static void mainOnly() {
  if (std::this_thread::get_id() != g_mainThread) g_offMainCalls++;
}

// ---- stubs ------------------------------------------------------------------------
void (*g_hostSerialSink)(const char* line) = nullptr;
HostSerial Serial;

static std::vector<Settings::FamilyGroup> g_groups;
static std::string g_myName = "Alice";
static std::atomic<bool> g_wifi(true);
namespace Settings {
uint8_t getGroupCount() {
  mainOnly();
  return static_cast<uint8_t>(g_groups.size());
}
FamilyGroup getGroup(uint8_t i) {
  mainOnly();
  FamilyGroup g{};
  if (i < g_groups.size()) g = g_groups[i];
  return g;
}
const char* getMyName() {
  mainOnly();
  return g_myName.c_str();
}
uint8_t getMyColorIndex() {
  mainOnly();
  return 3;
}
}  // namespace Settings

static AppService g_svc{nullptr, nullptr};
static SettingsChangeHook g_settingsHook = nullptr;
static BeforeSleepHook g_sleepHook = nullptr;
static ConnectivityStatusFn g_connFn = nullptr;
bool registerAppService(const AppService& s) {
  g_svc = s;
  return true;
}
bool registerSettingsChangeHook(SettingsChangeHook fn) {
  g_settingsHook = fn;
  return true;
}
bool registerBeforeSleepHook(BeforeSleepHook fn) {
  g_sleepHook = fn;
  return true;
}
void registerConnectivityStatusProvider(ConnectivityStatusFn fn) { g_connFn = fn; }
static std::vector<std::string> g_packets;  // "group|topic suffix|len" of dispatched non-presence packets
static void onPacket(const char* group, const char* topic, const uint8_t*, size_t len) {
  mainOnly();
  g_packets.push_back(std::string(group) + "|" + topic + "|" + std::to_string(len));
}
NetworkPacketHandlerFn getNetworkPacketHandler(uint8_t) {
  mainOnly();
  return onPacket;
}

namespace Identity {
const char* deviceId() { return "AABBCCDDEEFF"; }  // immutable after init: the worker may read it
}  // namespace Identity

namespace WifiManager {
bool isConnected() {
  mainOnly();
  return g_wifi.load();
}
bool isNtpSynced() { return true; }
uint32_t getUnixTime() {
  mainOnly();
  return 1790000000u + millis() / 1000;
}
}  // namespace WifiManager

static std::vector<std::string> g_presenceIn;  // "group|payload"
static bool g_radioAvailable = true;
namespace Presence {
void handleIncoming(const char* group, const char* payload, uint16_t len) {
  mainOnly();
  g_presenceIn.push_back(std::string(group) + "|" + std::string(payload, len));
}
void buildOwnPresencePayload(char* out, size_t n, const char* status, uint32_t ts) {
  mainOnly();
  snprintf(out, n, "MBP1|%s|%s|%s|%d|%lu|%u", Identity::deviceId(), g_myName.c_str(), status, g_radioAvailable ? 1 : 0,
           static_cast<unsigned long>(ts), 3u);
}
static bool publishStatus(const char* group, const char* status) {
  char p[110];
  buildOwnPresencePayload(p, sizeof(p), status, WifiManager::getUnixTime());
  char t[48];
  snprintf(t, sizeof(t), "presence/%s", Identity::deviceId());
  return MqttManager::publishRaw(group, t, p, true, 1);
}
bool publishOnline(const char* group) {
  mainOnly();
  return publishStatus(group, "ONLINE");
}
void publishOffline(const char* group) {
  mainOnly();
  publishStatus(group, "OFFLINE");
}
void republishOwnPresenceAllGroups() {
  for (auto& g : g_groups)
    if (MqttManager::isGroupReady(g.code)) publishOnline(g.code);
}
}  // namespace Presence

// ---- test plumbing -------------------------------------------------------------
static int g_pass = 0, g_fail = 0;
static const char* g_test = "";
#define CHECK(cond, ...)                                       \
  do {                                                         \
    if (cond) {                                                \
      g_pass++;                                                \
    } else {                                                   \
      g_fail++;                                                \
      printf("FAIL [%s] line %d: ", g_test, __LINE__);         \
      printf(__VA_ARGS__);                                     \
      printf("\n");                                            \
    }                                                          \
  } while (0)

using namespace MqttManager;

static const uint32_t kA = FakeNet::ip4(18, 158, 167, 130);
static const uint32_t kB = FakeNet::ip4(3, 64, 233, 141);
static const uint32_t kBad = FakeNet::ip4(3, 122, 167, 42);
static const uint32_t kBad2 = FakeNet::ip4(18, 157, 221, 89);
static const uint32_t kC = FakeNet::ip4(35, 158, 1, 2);

static void tick() { g_svc.tick(); }
static void advance(uint32_t ms) { g_simMs += ms; }
static void pumpWorker() {
  Job j;
  while (xQueueReceive(g_jobQueue, &j, 0) == pdTRUE) runJob(j);
}
static int indexOfGroup(const char* code) {
  for (size_t i = 0; i < g_groups.size(); i++)
    if (strcmp(g_groups[i].code, code) == 0) return static_cast<int>(i);
  return -1;
}
static void addGroup(const char* code) {
  mainOnly();
  Settings::FamilyGroup g{};
  g.configured = true;
  snprintf(g.name, sizeof(g.name), "G-%s", code);
  snprintf(g.code, sizeof(g.code), "%s", code);
  g_groups.push_back(g);
  SettingsChangeInfo info{SET_GROUP_ADDED, static_cast<uint8_t>(g_groups.size() - 1), {0}};
  snprintf(info.group_code, sizeof(info.group_code), "%s", code);
  g_settingsHook(info);
}
static void deleteGroup(const char* code) {
  int i = indexOfGroup(code);
  if (i < 0) return;
  SettingsChangeInfo info{SET_GROUP_DELETED, static_cast<uint8_t>(i), {0}};
  snprintf(info.group_code, sizeof(info.group_code), "%s", code);
  g_settingsHook(info);  // before erasing, as settings.cpp does
  g_groups.erase(g_groups.begin() + i);
}
static std::string clientIdOf(const char* code) {
  char b[24];
  buildClientId(code, b, sizeof(b));
  return b;
}
static int count(const std::vector<FakeNet::Event>& ev, const std::string& kind, const std::string& id = "",
                 bool (*pred)(const FakeNet::Event&) = nullptr) {
  int n = 0;
  for (auto& e : ev)
    if (e.kind == kind && (id.empty() || e.clientId == id) && (pred == nullptr || pred(e))) n++;
  return n;
}
static bool isOnline(const FakeNet::Event& e) { return e.payload.find("|ONLINE|") != std::string::npos; }
static bool isOffline(const FakeNet::Event& e) { return e.payload.find("|OFFLINE|") != std::string::npos; }
static bool isClear(const FakeNet::Event& e) { return e.payload.empty() && e.topic.find("/presence/") != std::string::npos; }
static bool cleanConnect(const FakeNet::Event& e) { return e.cleanSession; }
static bool sessionConnect(const FakeNet::Event& e) { return !e.cleanSession; }

static void drainQueuesFreeing() {
  if (g_jobQueue == nullptr || g_resultQueue == nullptr) return;
  Job j;
  while (xQueueReceive(g_jobQueue, &j, 0) == pdTRUE) delete j.conn;
  Result r;
  while (xQueueReceive(g_resultQueue, &r, 0) == pdTRUE) delete r.conn;
}
static void resetAll() {
  drainQueuesFreeing();
  for (auto& c : g_clients) {
    delete c.conn;
    c = GroupClient();
  }
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) bumpGeneration(i);
  for (auto& e : g_endpoints) e = Endpoint();
  g_maintenanceMode = false;
  g_workerSuspended.store(false);
  g_wifiWasConnected = true;
  releaseQueuedPayloads();
  FakeNet::reset();
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dnsAnswers.push_back(kA);
  }
  g_groups.clear();
  g_presenceIn.clear();
  g_packets.clear();
  g_offMainCalls = 0;
  g_wifi = true;
  g_myName = "Alice";
  g_radioAvailable = true;
}
// One group to Ready (manual mode).
static void connectGroup(const char* code) {
  if (indexOfGroup(code) < 0) addGroup(code);
  tick();
  pumpWorker();
  tick();
}
static void setScript(uint32_t ip, const FakeNet::Script& s) {
  std::lock_guard<std::mutex> l(FakeNet::st().m);
  FakeNet::st().scripts[ip] = s;
}
static void setDns(std::vector<uint32_t> answers, bool fail = false) {
  std::lock_guard<std::mutex> l(FakeNet::st().m);
  FakeNet::st().dnsAnswers.assign(answers.begin(), answers.end());
  FakeNet::st().dnsFail = fail;
}
static int overlaps() {
  std::lock_guard<std::mutex> l(FakeNet::st().m);
  return FakeNet::st().sameIdOverlap;
}
static int concurrentUse() {
  std::lock_guard<std::mutex> l(FakeNet::st().m);
  return FakeNet::st().concurrentUse;
}
static int useAfterClose() {
  std::lock_guard<std::mutex> l(FakeNet::st().m);
  return FakeNet::st().useAfterClose;
}
static int liveSessions(const std::string& id) {
  std::lock_guard<std::mutex> l(FakeNet::st().m);
  return FakeNet::st().liveSessions[id];
}
static GroupClient* slotOf(const char* code) { return findClientSlot(code); }

// Network operations performed while the main task ran `fn` (no worker runs in manual mode unless pumped).
template <class F>
static int networkOpsDuring(F fn) {
  size_t before = FakeNet::events().size();
  fn();
  std::vector<FakeNet::Event> ev = FakeNet::events();
  int n = 0;
  for (size_t i = before; i < ev.size(); i++) {
    const std::string& k = ev[i].kind;
    if (k == "connect" || k == "connect-failed" || k == "subscribe") n++;
  }
  std::lock_guard<std::mutex> l(FakeNet::st().m);
  return n + 0 * FakeNet::st().dnsQueries;
}

// =============================================================================
// Manual (deterministic) tests
// =============================================================================
static void testSetupContract() {
  g_test = "M1 setup on the worker: one connection per group, unchanged client id, will, keepalive, clean session, topics, QoS and ONLINE; the main task sees it only when ready";
  resetAll();
  addGroup("FAMCODE1");
  int mainOps = networkOpsDuring([] { tick(); });
  GroupClient* c = slotOf("FAMCODE1");
  CHECK(c != nullptr && c->jobInFlight && c->conn == nullptr && mainOps == 0,
        "after the first tick: job in flight %d, no connection %d, network operations in the tick %d", c && c->jobInFlight,
        c && c->conn == nullptr, mainOps);
  CHECK(!isGroupConnected("FAMCODE1") && !isGroupReady("FAMCODE1") && g_connFn() == CONN_WIFI_ONLY,
        "before setup: connected %d ready %d status %u", isGroupConnected("FAMCODE1"), isGroupReady("FAMCODE1"), g_connFn());
  pumpWorker();
  CHECK(!isGroupConnected("FAMCODE1"), "a finished setup is not visible to the main task before its result is taken");
  tick();
  CHECK(isGroupConnected("FAMCODE1") && isGroupReady("FAMCODE1") && g_connFn() == CONN_ONLINE && g_liveConns.load() == 1,
        "after adoption: connected %d ready %d status %u live connections %d", isGroupConnected("FAMCODE1"),
        isGroupReady("FAMCODE1"), g_connFn(), g_liveConns.load());
  std::vector<FakeNet::Event> ev = FakeNet::events();
  std::string id = clientIdOf("FAMCODE1");
  const FakeNet::Event* conn = nullptr;
  std::vector<std::string> subs;
  std::vector<int> qos;
  const FakeNet::Event* online = nullptr;
  for (auto& e : ev) {
    if (e.kind == "connect") conn = &e;
    if (e.kind == "subscribe") {
      subs.push_back(e.topic);
      qos.push_back(e.qos);
    }
    if (e.kind == "publish" && isOnline(e)) online = &e;
  }
  char crc[16];
  snprintf(crc, sizeof(crc), "%08lX", static_cast<unsigned long>(Crc32::computeStr("FAMCODE1")));
  CHECK(conn != nullptr && conn->clientId == std::string("AABBCCDDEEFF_") + crc && !conn->cleanSession &&
            conn->keepAlive == 20 && conn->ip == kA,
        "CONNECT: id %s (expected AABBCCDDEEFF_%s), cleanSession %d, keepAlive %d, address ok %d",
        conn ? conn->clientId.c_str() : "-", crc, conn ? conn->cleanSession : -1, conn ? conn->keepAlive : -1,
        conn && conn->ip == kA);
  CHECK(conn != nullptr && conn->willTopic == "morsebuddy/FAMCODE1/presence/AABBCCDDEEFF" && conn->willRetained &&
            conn->willQos == 1 && conn->willPayload.find("MBP1|AABBCCDDEEFF|Alice|OFFLINE|0|") == 0 &&
            conn->willPayload.substr(conn->willPayload.size() - 2) == "|3",
        "will: %s retained %d qos %d payload %s", conn ? conn->willTopic.c_str() : "-", conn ? conn->willRetained : 0,
        conn ? conn->willQos : -1, conn ? conn->willPayload.c_str() : "-");
  const char* expSuffix[10] = {"presence/+", "msg/AABBCCDDEEFF", "broadcast", "r2/+", "r2q/AABBCCDDEEFF",
                               "radio/busy/AABBCCDDEEFF", "radio/busy_reply/AABBCCDDEEFF", "radio/session",
                               "radio/audio/AABBCCDDEEF" /* truncated by 1 byte, reproduced as before */,
                               "radio/audio/broadcast"};
  const int expQos[10] = {1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
  bool topicsOk = subs.size() == 10;
  for (size_t i = 0; topicsOk && i < 10; i++)
    topicsOk = subs[i] == std::string("morsebuddy/FAMCODE1/") + expSuffix[i] && qos[i] == expQos[i];
  CHECK(topicsOk, "10 subscriptions in the established order and QoS (got %zu)", subs.size());
  CHECK(online != nullptr && online->topic == "morsebuddy/FAMCODE1/presence/AABBCCDDEEFF" && online->retained &&
            online->qos == 1 && online->payload.find("MBP1|AABBCCDDEEFF|Alice|ONLINE|1|") == 0,
        "ONLINE presence published last, retained QoS1: %s", online ? online->payload.c_str() : "-");
  CHECK(c->conn != nullptr && c->conn->mqtt.timeout() == static_cast<int>(kMqttCommandTimeoutMs),
        "an adopted connection carries the main task's 500 ms command timeout (%d)", c->conn ? c->conn->mqtt.timeout() : -1);
  CHECK(g_offMainCalls.load() == 0, "Settings/Presence/WifiManager calls off the main task: %d", g_offMainCalls.load());
  // publishes go through the adopted connection
  bool pub = publishRaw("FAMCODE1", "broadcast", "hello", false, 1);
  CHECK(pub && count(FakeNet::events(), "publish", id) == 2, "publishRaw on the adopted connection: %d", pub);
}

static void testLongGroupCode() {
  g_test = "M1b a 32-character group code: will and presence-clear topics are complete (were cut at 48 bytes before this fix)";
  resetAll();
  const char* code = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345";
  connectGroup(code);
  std::string expect = std::string("morsebuddy/") + code + "/presence/AABBCCDDEEFF";
  std::string will;
  for (auto& e : FakeNet::events())
    if (e.kind == "connect") will = e.willTopic;
  deleteGroup(code);
  tick();
  pumpWorker();
  int clears = 0;
  for (auto& e : FakeNet::events())
    if (e.kind == "publish" && isClear(e) && e.topic == expect) clears++;
  CHECK(will == expect && clears == 1, "will topic %s, record clears on the full topic %d", will.c_str(), clears);
}

static void testFailureBackoff() {
  g_test = "M2 failed attempts (TCP refused, no CONNACK) retry after 2, 4, 8, 16, 32 then 60 s (+-20%), never from the main task; success resets";
  resetAll();
  FakeNet::Script dead;
  dead.tcpOk = false;
  setScript(kA, dead);
  addGroup("G2");
  uint32_t expected[7] = {2000, 4000, 8000, 16000, 32000, 60000, 60000};
  bool ok = true;
  for (int k = 0; k < 7; k++) {
    tick();
    pumpWorker();
    uint32_t now = millis();
    tick();
    GroupClient* c = slotOf("G2");
    uint32_t delayMs = c->nextAttemptMs - now;
    bool inRange = delayMs >= expected[k] * 4 / 5 && delayMs <= expected[k] * 6 / 5;
    // no job before the deadline
    advance(delayMs - 1);
    tick();
    bool early = c->jobInFlight;
    advance(1);
    if (!inRange || early || c->consecutiveFailures != k + 1) {
      ok = false;
      printf("   attempt %d: delay %u (expected ~%u), job early %d, failures %u\n", k + 1, delayMs, expected[k], early,
             c->consecutiveFailures);
    }
  }
  CHECK(ok, "retry schedule");
  CHECK(count(FakeNet::events(), "connect") == 0, "no CONNECT succeeded while TCP fails");
  // CONNACK missing: -9, returnCode 6 (not a refusal)
  FakeNet::Script noAck;
  noAck.connackError = LWMQTT_MISSING_OR_WRONG_PACKET;
  setScript(kA, noAck);
  advance(70000);
  tick();
  pumpWorker();
  tick();
  GroupClient* c = slotOf("G2");
  CHECK(c->consecutiveFailures == 8 && !isGroupConnected("G2"), "a missing CONNACK is a failed attempt (failures %u)",
        c->consecutiveFailures);
  setScript(kA, FakeNet::Script());
  advance(70000);
  connectGroup("G2");
  CHECK(isGroupReady("G2") && c->consecutiveFailures == 0, "success resets the failure count (%u)", c->consecutiveFailures);
  CHECK(g_offMainCalls.load() == 0, "calls off the main task: %d", g_offMainCalls.load());
}

static void testSetupStageFailures() {
  g_test = "M2b a failed SUBSCRIBE or a failed ONLINE publish fails the attempt: closed, never adopted, counted, retried";
  for (int which = 0; which < 2; which++) {
    resetAll();
    FakeNet::Script s;
    if (which == 0) s.failSubscribeAt = 3;
    else s.failPublishOnline = true;
    setScript(kA, s);
    addGroup("SF");
    tick();
    pumpWorker();
    tick();
    std::string id = clientIdOf("SF");
    Endpoint* ep = findEndpoint(kA);
    CHECK(ep != nullptr && ep->failures == 1 && !ep->everOk, "the endpoint is marked failed, not good (failures %u, ok %d)",
          ep ? ep->failures : 0, ep ? ep->everOk : -1);
    CHECK(!isGroupConnected("SF") && slotOf("SF")->consecutiveFailures == 1 && liveSessions(id) == 0 && g_liveConns.load() == 0,
          "%s failure: connected %d, failures %u, live sessions %d, live connections %d", which ? "ONLINE" : "SUBSCRIBE",
          isGroupConnected("SF"), slotOf("SF")->consecutiveFailures, liveSessions(id), g_liveConns.load());
    setScript(kA, FakeNet::Script());
    advance(3000);
    tick();
    pumpWorker();
    tick();
    CHECK(isGroupReady("SF"), "retried and ready");
  }
}

static void testEndpoints() {
  g_test = "M3 endpoints: a failing address is avoided while another one DNS returned has worked; DNS down uses the last working one; avoidance expires; only DNS answers are used";
  resetAll();
  setDns({kB});
  connectGroup("E1");
  CHECK(isGroupReady("E1"), "connected through B");
  FakeNet::Script dead;
  dead.tcpOk = false;
  setScript(kBad, dead);
  setDns({kBad});
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dropNext.insert(clientIdOf("E1"));
  }
  tick();  // loss
  CHECK(!isGroupConnected("E1"), "connection loss seen");
  advance(6000);
  tick();
  pumpWorker();
  tick();  // DNS says kBad: tried, fails
  std::vector<FakeNet::Event> ev = FakeNet::events();
  advance(3000);
  tick();
  pumpWorker();
  tick();  // kBad avoided, B known good
  ev = FakeNet::events();
  uint32_t lastIp = 0;
  for (auto& e : ev)
    if (e.kind == "connect") lastIp = e.ip;
  CHECK(isGroupReady("E1") && lastIp == kB, "after kBad failed, the next attempt used B (ready %d, B %d)", isGroupReady("E1"),
        lastIp == kB);
  // DNS down: last working endpoint
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dropNext.insert(clientIdOf("E1"));
  }
  setDns({}, true);
  tick();
  advance(6000);
  tick();
  pumpWorker();
  tick();
  CHECK(isGroupReady("E1"), "with DNS down, the last working address (B) is used");
  // avoidance expires: kBad (now fixed) is used again once its window passed
  setScript(kBad, FakeNet::Script());
  setDns({kBad});
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dropNext.insert(clientIdOf("E1"));
  }
  tick();
  advance(31000);
  tick();
  pumpWorker();
  tick();
  ev = FakeNet::events();
  for (auto& e : ev)
    if (e.kind == "connect") lastIp = e.ip;
  CHECK(isGroupReady("E1") && lastIp == kBad, "after its 30 s avoidance, the DNS answer is used again (%d)", lastIp == kBad);
  // no DNS and nothing known: dns stage, no connection attempt
  resetAll();
  setDns({}, true);
  addGroup("E2");
  tick();
  pumpWorker();
  tick();
  CHECK(!isGroupConnected("E2") && count(FakeNet::events(), "connect") == 0 && slotOf("E2")->consecutiveFailures == 1,
        "DNS down with no known address: a failed attempt, no TCP");
  bool onlyDns = true;
  for (auto& e : FakeNet::events())
    if (e.kind == "connect" && e.ip != kA && e.ip != kB && e.ip != kBad) onlyDns = false;
  CHECK(onlyDns, "every address used came from DNS");
}

static void setDnsResponses(std::vector<std::vector<uint32_t>> responses, bool cacheModel) {
  std::lock_guard<std::mutex> l(FakeNet::st().m);
  FakeNet::st().dnsAnswers.clear();
  FakeNet::st().dnsResponses.assign(responses.begin(), responses.end());
  FakeNet::st().dnsCacheModel = cacheModel;
  FakeNet::st().dnsCacheValid = false;
}
static std::vector<uint32_t> connectIps(const std::string& id) {
  std::vector<uint32_t> ips;
  for (auto& e : FakeNet::events())
    if ((e.kind == "connect" || e.kind == "connect-failed") && e.clientId == id && !e.cleanSession) ips.push_back(e.ip);
  return ips;
}
static void attempt() {  // one setup attempt once its retry time has come
  GroupClient* c = nullptr;
  for (auto& g : g_clients)
    if (g.state == SlotState::kActive) c = &g;
  if (c != nullptr && c->nextAttemptMs != 0 && !deadlineReached(millis(), c->nextAttemptMs)) g_simMs = c->nextAttemptMs;
  tick();
  pumpWorker();
  tick();
}

static void testDnsSecondAddress() {
  g_test = "M3b the first address of a DNS answer accepts TCP but sends no CONNACK; the second address of the same answer is used next, with no earlier success";
  resetAll();
  FakeNet::Script noAck;
  noAck.connackError = LWMQTT_MISSING_OR_WRONG_PACKET;
  setScript(kBad, noAck);
  setDnsResponses({{kBad, kA}}, false);
  addGroup("N1");
  attempt();
  CHECK(!isGroupConnected("N1"), "first attempt (kBad) fails");
  attempt();
  std::vector<uint32_t> ips = connectIps(clientIdOf("N1"));
  CHECK(isGroupReady("N1") && ips.size() == 2 && ips[0] == kBad && ips[1] == kA,
        "ready on the second attempt through the answer's second address (ready %d, attempts %zu)", isGroupReady("N1"), ips.size());
}

static void testDnsFreshAnswer() {
  g_test = "M3c ESP32 lwIP (one cached address per lookup): after an endpoint fails, a fresh DNS answer is requested and its new address used, with no earlier success";
  resetAll();
  FakeNet::Script noAck;
  noAck.connackError = LWMQTT_MISSING_OR_WRONG_PACKET;
  setScript(kBad, noAck);
  setDnsResponses({{kBad}, {kA}}, true);
  addGroup("N2");
  attempt();
  int clearsAfterFailure;
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    clearsAfterFailure = FakeNet::st().dnsCacheClears;
  }
  attempt();
  std::vector<uint32_t> ips = connectIps(clientIdOf("N2"));
  CHECK(clearsAfterFailure >= 1 && isGroupReady("N2") && ips.size() == 2 && ips[0] == kBad && ips[1] == kA,
        "cache refresh requested %d; ready %d through the new address (attempts %zu)", clearsAfterFailure, isGroupReady("N2"),
        ips.size());
  int clearsBefore;
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    clearsBefore = FakeNet::st().dnsCacheClears;
  }
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dropNext.insert(clientIdOf("N2"));
  }
  tick();
  advance(6000);
  attempt();
  int clearsAfter;
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    clearsAfter = FakeNet::st().dnsCacheClears;
  }
  CHECK(isGroupReady("N2") && clearsAfter == clearsBefore, "no DNS refresh for a loss that was not an endpoint failure (%d)",
        clearsAfter - clearsBefore);
}

static void testDnsAllBad() {
  g_test = "M3d every address of the answer fails: both are tried, retries stay on the backoff schedule, nothing outside the answer is used";
  resetAll();
  FakeNet::Script noAck;
  noAck.connackError = LWMQTT_MISSING_OR_WRONG_PACKET;
  setScript(kBad, noAck);
  setScript(kBad2, noAck);
  setDnsResponses({{kBad, kBad2}}, false);
  addGroup("N3");
  uint32_t t0 = millis();
  for (int i = 0; i < 6; i++) attempt();
  std::vector<uint32_t> ips = connectIps(clientIdOf("N3"));
  int bad = 0, bad2 = 0, other = 0;
  for (uint32_t ip : ips) (ip == kBad ? bad : ip == kBad2 ? bad2 : other)++;
  uint32_t span = millis() - t0;
  // 6 attempts on the 2,4,8,16,32 s schedule (+-20%): at least 0.8*(2+4+8+16+32) s
  CHECK(ips.size() == 6 && bad >= 1 && bad2 >= 1 && other == 0 && slotOf("N3")->consecutiveFailures == 6 && span >= 49600,
        "attempts %zu (kBad %d, kBad2 %d, other %d), failures %u, span %u ms", ips.size(), bad, bad2, other,
        slotOf("N3")->consecutiveFailures, span);
}

static void testDnsPrefersWorking() {
  g_test = "M3e an address that has worked is preferred to one never tried: within one answer, after a failure, and with DNS down";
  for (int variant = 0; variant < 3; variant++) {
    resetAll();
    setDnsResponses({{kB}}, false);
    connectGroup("W9");  // B works
    std::string id = clientIdOf("W9");
    {
      std::lock_guard<std::mutex> l(FakeNet::st().m);
      FakeNet::st().dropNext.insert(id);
    }
    tick();
    if (variant == 0) {
      setDnsResponses({{kC, kB}}, false);  // C (never tried) first in the answer
    } else {
      advance(1000);
      noteResolvedEndpoint(kC, millis());  // a later lookup returned C; it was never tried
      g_simMs = slotOf("W9")->nextAttemptMs;
      if (variant == 1) {
        FakeNet::Script dead;
        dead.tcpOk = false;
        setScript(kBad, dead);
        setDnsResponses({{kBad}}, false);  // the next answer fails
        attempt();
      } else {
        setDnsResponses({}, false);
        std::lock_guard<std::mutex> l(FakeNet::st().m);
        FakeNet::st().dnsFail = true;  // DNS down
      }
    }
    attempt();
    std::vector<uint32_t> ips = connectIps(id);
    CHECK(isGroupReady("W9") && !ips.empty() && ips.back() == kB,
          "variant %d (%s): reconnected through B (ready %d, last address B %d)", variant,
          variant == 0 ? "same answer" : variant == 1 ? "after a failure" : "DNS down", isGroupReady("W9"),
          !ips.empty() && ips.back() == kB);
  }
}

static void testReadyLoss() {
  g_test = "M4 a ready connection lost (loop, or a failed publish) is freed at once and retried after the 5 s anti-thrash window";
  resetAll();
  connectGroup("R1");
  uint32_t readyAt = millis();
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dropNext.insert(clientIdOf("R1"));
  }
  tick();
  CHECK(!isGroupConnected("R1") && !isGroupReady("R1") && g_liveConns.load() == 0 && slotOf("R1")->conn == nullptr,
        "loss: connected %d, live connections %d", isGroupConnected("R1"), g_liveConns.load());
  tick();
  CHECK(!slotOf("R1")->jobInFlight, "no attempt inside the window");
  g_simMs = readyAt + 5000;
  tick();
  CHECK(slotOf("R1")->jobInFlight, "attempt when the window ends");
  pumpWorker();
  tick();
  CHECK(isGroupReady("R1"), "reconnected");
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().livePublishFailures = 1;
  }
  bool pub = publishRaw("R1", "broadcast", "x", false, 1);
  tick();
  CHECK(!pub && !isGroupConnected("R1") && g_liveConns.load() == 0 && useAfterClose() == 0,
        "a failed publish closes it; the next tick frees it (live %d, use after close %d)", g_liveConns.load(),
        useAfterClose());
}

static void testWifi() {
  g_test = "M5 Wi-Fi loss and recovery: a setup finished while Wi-Fi was down, or begun before a flap, is never adopted; one session per client id";
  resetAll();
  connectGroup("W1");
  addGroup("W2");
  tick();  // W2's job queued
  g_wifi = false;
  tick();
  pumpWorker();  // W2 completes while Wi-Fi is "down"
  tick();
  CHECK(!isGroupConnected("W2") && g_liveConns.load() == 1, "a setup finished with Wi-Fi down is closed, not adopted (live %d)",
        g_liveConns.load());
  g_wifi = true;
  tick();  // recovery: W1's adopted connection closed, both attempt at once
  CHECK(!isGroupConnected("W1") && slotOf("W1")->jobInFlight && slotOf("W2")->jobInFlight,
        "recovery: adopted connections closed and every group attempts at once");
  pumpWorker();
  tick();
  CHECK(isGroupReady("W1") && isGroupReady("W2") && overlaps() == 0 && g_liveConns.load() == 2,
        "both ready again, overlapping sessions %d, live %d", overlaps(), g_liveConns.load());
  // flap while a job is in flight
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dropNext.insert(clientIdOf("W1"));
  }
  tick();
  advance(6000);
  tick();  // W1 job queued
  g_wifi = false;
  tick();
  g_wifi = true;
  tick();  // recovery cancels W1's job (stale)
  CHECK(slotOf("W1")->jobInFlight, "the job stays outstanding until its result returns");
  pumpWorker();  // canceled before any network step
  tick();
  CHECK(slotOf("W1")->jobInFlight, "a fresh attempt is queued only after the stale result came back");
  pumpWorker();
  tick();
  CHECK(isGroupReady("W1") && overlaps() == 0 && g_liveConns.load() == 2, "ready, overlapping sessions %d", overlaps());
}

static void testDeleteDuringSetup() {
  g_test = "M6 deleting a group while its setup is queued or finished-but-not-taken: never adopted, never reconnects, cleanup only after the setup result, one session per client id";
  // (a) job queued, not started
  resetAll();
  addGroup("D1");
  tick();
  int mainOps = networkOpsDuring([] { deleteGroup("D1"); });
  CHECK(mainOps == 0 && slotOf("D1") == nullptr, "deletion does no network work on the main task");
  pumpWorker();  // the setup sees its cancellation: no network step
  tick();        // result back: cleanup queued
  pumpWorker();  // cleanup: cleanSession connect + disconnect
  tick();
  std::vector<FakeNet::Event> ev = FakeNet::events();
  std::string id = clientIdOf("D1");
  CHECK(count(ev, "connect", id, sessionConnect) == 0 && count(ev, "subscribe", id) == 0 &&
            count(ev, "connect", id, cleanConnect) == 1,
        "canceled before any network step; one cleanSession connect (session connects %d, cleanSession %d)",
        count(ev, "connect", id, sessionConnect), count(ev, "connect", id, cleanConnect));
  for (int i = 0; i < 20; i++) {
    advance(10000);
    tick();
    pumpWorker();
  }
  CHECK(count(FakeNet::events(), "connect", id) == 1, "a deleted group never reconnects");
  // (b) setup finished, result not yet taken
  resetAll();
  addGroup("D2");
  tick();
  pumpWorker();  // result with a live connection waits in the queue
  deleteGroup("D2");
  tick();  // result: kept for the cleanup, which is queued now
  pumpWorker();
  tick();
  ev = FakeNet::events();
  id = clientIdOf("D2");
  int iDisc = -1, iClean = -1, iOffline = -1, iClear = -1;
  for (size_t i = 0; i < ev.size(); i++) {
    if (ev[i].clientId != id) continue;
    if (ev[i].kind == "publish" && isOffline(ev[i])) iOffline = static_cast<int>(i);
    if (ev[i].kind == "publish" && isClear(ev[i])) iClear = static_cast<int>(i);
    if (ev[i].kind == "disconnect" && iDisc < 0) iDisc = static_cast<int>(i);
    if (ev[i].kind == "connect" && ev[i].cleanSession) iClean = static_cast<int>(i);
  }
  CHECK(!isGroupConnected("D2") && iOffline >= 0 && iClear > iOffline && iDisc > iClear && iClean > iDisc &&
            overlaps() == 0 && g_liveConns.load() == 0,
        "OFFLINE, record clear, disconnect, then the cleanSession connect (%d %d %d %d), overlaps %d, live %d", iOffline,
        iClear, iDisc, iClean, overlaps(), g_liveConns.load());
  CHECK(g_offMainCalls.load() == 0, "calls off the main task: %d", g_offMainCalls.load());
}

static void testDeleteConnected() {
  g_test = "M7 deleting a connected group: OFFLINE, record clear, disconnect and session cleanup all on the worker";
  resetAll();
  connectGroup("C1");
  connectGroup("C2");
  size_t before = FakeNet::events().size();
  deleteGroup("C1");
  tick();
  bool none = FakeNet::events().size() == before;
  CHECK(none && !isGroupConnected("C1") && isGroupReady("C2"), "the deletion and the next tick send nothing themselves");
  pumpWorker();
  std::vector<FakeNet::Event> ev = FakeNet::events();
  std::string id = clientIdOf("C1");
  CHECK(count(ev, "publish", id, isOffline) == 1 && count(ev, "publish", id, isClear) == 1 &&
            count(ev, "connect", id, cleanConnect) == 1 && liveSessions(id) == 0 && g_liveConns.load() == 1,
        "cleanup done on the worker (offline %d clear %d cleanSession %d live sessions %d)", count(ev, "publish", id, isOffline),
        count(ev, "publish", id, isClear), count(ev, "connect", id, cleanConnect), liveSessions(id));
}

static void testReplacement() {
  g_test = "M8 replacing a group (delete + add the same code) while its setup is outstanding: the new setup runs after the old cleanup; one session per client id";
  resetAll();
  addGroup("X1");
  tick();
  pumpWorker();  // old setup finished, result pending
  deleteGroup("X1");
  addGroup("X1");  // same code, new slot
  tick();          // old result taken (kept for cleanup); cleanup queued
  pumpWorker();
  tick();          // cleanup result frees the old slot; only then is the new setup queued
  pumpWorker();
  tick();
  std::vector<FakeNet::Event> ev = FakeNet::events();
  std::string id = clientIdOf("X1");
  int iClean = -1, iNew = -1, sessions = 0;
  for (size_t i = 0; i < ev.size(); i++) {
    if (ev[i].clientId != id || ev[i].kind != "connect") continue;
    if (ev[i].cleanSession) iClean = static_cast<int>(i);
    else if (++sessions == 2) iNew = static_cast<int>(i);
  }
  CHECK(isGroupReady("X1") && iClean >= 0 && iNew > iClean && overlaps() == 0 && liveSessions(id) == 1,
        "new group ready %d after the cleanup (%d < %d), overlaps %d, live sessions %d", isGroupReady("X1"), iClean, iNew,
        overlaps(), liveSessions(id));
}

static void testReplacementQueued() {
  g_test = "M8b replacing a group while its setup is still queued: the new setup waits for the old cleanup; one session per client id";
  resetAll();
  addGroup("X2");
  tick();  // old setup queued, not started
  deleteGroup("X2");
  addGroup("X2");
  tick();
  GroupClient* n = slotOf("X2");
  CHECK(n != nullptr && !n->jobInFlight, "no setup for the new group while the old one drains");
  for (int i = 0; i < 4; i++) {
    pumpWorker();
    tick();
  }
  std::string id = clientIdOf("X2");
  CHECK(isGroupReady("X2") && overlaps() == 0 && liveSessions(id) == 1, "ready %d, overlapping sessions %d, live sessions %d",
        isGroupReady("X2"), overlaps(), liveSessions(id));
}

static void testStaleResultNewOccupant() {
  g_test = "M9 a result from a slot's previous group is never adopted by the group now in that slot";
  resetAll();
  addGroup("OLD");
  tick();
  uint8_t slot = slotIndex(*slotOf("OLD"));
  uint32_t oldGen = g_slotGeneration[slot].load();
  deleteGroup("OLD");
  pumpWorker();
  tick();
  pumpWorker();  // cleanup done
  tick();        // its result frees the slot (since the session-cleanup fix the slot stays reserved until then)
  addGroup("NEW");
  CHECK(slotOf("NEW") != nullptr && slotIndex(*slotOf("NEW")) == slot, "NEW reuses the slot");
  tick();  // NEW's job queued
  // A late result carrying OLD's generation (and a live connection) arrives.
  Conn* fake = new Conn();
  snprintf(fake->group_code, sizeof(fake->group_code), "OLD");
  Result r;
  memset(&r, 0, sizeof(r));
  r.type = JobType::kSetup;
  r.slot = slot;
  r.generation = oldGen;
  r.ok = true;
  r.conn = fake;
  xQueueSend(g_resultQueue, &r, 0);
  tick();
  CHECK(!isGroupConnected("NEW") && slotOf("NEW")->conn == nullptr, "not adopted for NEW");
  pumpWorker();
  tick();
  CHECK(isGroupReady("NEW") && g_liveConns.load() == 1 && overlaps() == 0 && slotOf("NEW")->jobInFlight == false,
        "NEW connects through its own single setup (live %d, overlapping sessions %d)", g_liveConns.load(), overlaps());
}

static void testMaintenance() {
  g_test = "M10 OTA maintenance: OFFLINE and disconnect for ready groups, setup in flight canceled and closed, no reconnect or cleanup until exit";
  resetAll();
  connectGroup("O1");
  addGroup("O2");
  tick();  // O2 queued
  std::string id1 = clientIdOf("O1"), id2 = clientIdOf("O2");
  setMaintenanceModeActive(true);
  std::vector<FakeNet::Event> ev = FakeNet::events();
  CHECK(count(ev, "publish", id1, isOffline) == 1 && liveSessions(id1) == 0 && !isGroupConnected("O1"),
        "O1: OFFLINE published and disconnected");
  pumpWorker();  // O2's setup: suspended, no network
  deleteGroup("O1");
  for (int i = 0; i < 5; i++) {
    advance(10000);
    tick();
    pumpWorker();
  }
  ev = FakeNet::events();
  CHECK(count(ev, "connect", id2) == 0 && count(ev, "connect", id1, cleanConnect) == 0 && g_liveConns.load() == 0,
        "during maintenance: no connect, no cleanup (O2 connects %d, O1 cleanup %d, live %d)", count(ev, "connect", id2),
        count(ev, "connect", id1, cleanConnect), g_liveConns.load());
  setMaintenanceModeActive(false);
  tick();
  pumpWorker();
  tick();
  ev = FakeNet::events();
  CHECK(isGroupReady("O2") && count(ev, "connect", id1, cleanConnect) == 1 && overlaps() == 0,
        "after exit: O2 ready %d, O1's cleanup ran %d", isGroupReady("O2"), count(ev, "connect", id1, cleanConnect));
  // a setup that finished after maintenance began is closed (with OFFLINE), not adopted
  resetAll();
  addGroup("O3");
  tick();
  pumpWorker();  // finished, result pending
  setMaintenanceModeActive(true);
  tick();
  ev = FakeNet::events();
  std::string id3 = clientIdOf("O3");
  CHECK(!isGroupConnected("O3") && liveSessions(id3) == 0 && count(ev, "publish", id3, isOffline) == 1 &&
            g_liveConns.load() == 0,
        "late result during maintenance closed with OFFLINE (live sessions %d)", liveSessions(id3));
  setMaintenanceModeActive(false);
}

static void testCleanupDuringMaintenance() {
  g_test = "M10b a deleted group's cleanup that reaches the worker during maintenance opens no connection then";
  resetAll();
  connectGroup("Q1");
  deleteGroup("Q1");  // cleanup queued with the connection
  setMaintenanceModeActive(true);
  pumpWorker();
  std::string id = clientIdOf("Q1");
  std::vector<FakeNet::Event> ev = FakeNet::events();
  CHECK(count(ev, "connect", id, cleanConnect) == 0 && liveSessions(id) == 0 && g_liveConns.load() == 0,
        "no cleanSession connect during maintenance (%d), connection closed %d", count(ev, "connect", id, cleanConnect),
        liveSessions(id) == 0);
  setMaintenanceModeActive(false);
  tick();
  // a deleted group's setup that finishes after maintenance began: closed at once (OFFLINE + clear), not held through OTA
  resetAll();
  addGroup("Q2");
  tick();
  pumpWorker();       // setup finished; its result waits
  deleteGroup("Q2");  // draining, its job still outstanding
  setMaintenanceModeActive(true);
  tick();
  std::string id2 = clientIdOf("Q2");
  ev = FakeNet::events();
  CHECK(liveSessions(id2) == 0 && g_liveConns.load() == 0 && count(ev, "publish", id2, isOffline) == 1 &&
            count(ev, "publish", id2, isClear) == 1,
        "closed during maintenance with OFFLINE %d and clear %d (live sessions %d, live connections %d)",
        count(ev, "publish", id2, isOffline), count(ev, "publish", id2, isClear), liveSessions(id2), g_liveConns.load());
  setMaintenanceModeActive(false);
  tick();
  pumpWorker();
  ev = FakeNet::events();
  CHECK(count(ev, "connect", id2, cleanConnect) == 1, "its session cleanup runs after maintenance (%d)", count(ev, "connect", id2, cleanConnect));
}

static void testCleanupAcrossMaintenance() {
  g_test = "M10c delete a connected group, its cleanup queued, OTA maintenance starts before the worker runs it, maintenance ends: the broker-session cleanup still happens, once, with no duplicate client id";
  resetAll();
  connectGroup("Q3");
  std::string id = clientIdOf("Q3");
  deleteGroup("Q3");               // cleanup queued, holding the connection
  setMaintenanceModeActive(true);  // before the worker runs it
  pumpWorker();                    // worker: OFFLINE + clear + disconnect; session cleanup skipped (suspended)
  for (int i = 0; i < 3; i++) {
    advance(1000);
    tick();
    pumpWorker();
  }
  std::vector<FakeNet::Event> ev = FakeNet::events();
  CHECK(count(ev, "connect", id, cleanConnect) == 0 && liveSessions(id) == 0 && count(ev, "publish", id, isOffline) == 1 &&
            count(ev, "publish", id, isClear) == 1,
        "during maintenance: OFFLINE %d, clear %d, no cleanSession connect (%d), no live session (%d)",
        count(ev, "publish", id, isOffline), count(ev, "publish", id, isClear), count(ev, "connect", id, cleanConnect),
        liveSessions(id));
  setMaintenanceModeActive(false);
  for (int i = 0; i < 4; i++) {
    advance(1000);
    tick();
    pumpWorker();
  }
  ev = FakeNet::events();
  CHECK(count(ev, "connect", id, cleanConnect) == 1 && count(ev, "connect", id, sessionConnect) == 1 && overlaps() == 0 &&
            liveSessions(id) == 0 && g_liveConns.load() == 0,
        "after maintenance: cleanSession connects %d (expected 1), session connects %d, overlaps %d, live sessions %d",
        count(ev, "connect", id, cleanConnect), count(ev, "connect", id, sessionConnect), overlaps(), liveSessions(id));
  // same order, and the group is added again (same code) during maintenance
  resetAll();
  connectGroup("Q4");
  id = clientIdOf("Q4");
  deleteGroup("Q4");
  setMaintenanceModeActive(true);
  pumpWorker();
  addGroup("Q4");
  tick();
  setMaintenanceModeActive(false);
  for (int i = 0; i < 6; i++) {
    advance(1000);
    tick();
    pumpWorker();
  }
  tick();
  ev = FakeNet::events();
  int iClean = -1, iNew = -1, sessions = 0;
  for (size_t i = 0; i < ev.size(); i++) {
    if (ev[i].clientId != id || ev[i].kind != "connect") continue;
    if (ev[i].cleanSession) iClean = static_cast<int>(i);
    else if (++sessions == 2) iNew = static_cast<int>(i);
  }
  CHECK(isGroupReady("Q4") && iClean >= 0 && iNew > iClean && overlaps() == 0 && liveSessions(id) == 1,
        "re-added during maintenance: session cleanup %d before the new session %d, ready %d, overlaps %d, live sessions %d",
        iClean, iNew, isGroupReady("Q4"), overlaps(), liveSessions(id));
}

static void testCleanupRetryBounded() {
  g_test = "M10d a session cleanup that fails on the network is retried on the backoff schedule, 3 attempts in all, then given up; the client id stays reserved until then";
  resetAll();
  connectGroup("Q5");
  std::string id = clientIdOf("Q5");
  FakeNet::Script dead;
  dead.tcpOk = false;
  setScript(kA, dead);
  deleteGroup("Q5");
  addGroup("Q5");  // re-added at once: must wait for the cleanup to finish or be given up
  int newSessionsWhileDraining = 0, cleanupAttempts = 0;
  for (int i = 0; i < 40; i++) {
    advance(1000);
    tick();
    pumpWorker();
    bool draining = false;
    for (auto& c : g_clients) draining = draining || (c.state == SlotState::kDraining && c.client_id == id);
    if (draining) {
      newSessionsWhileDraining = count(FakeNet::events(), "connect", id, sessionConnect) - 1;
      // TCP fails before CONNECT, so the cleanup's attempts are counted through the endpoint's failures
      Endpoint* ep = findEndpoint(kA);
      cleanupAttempts = ep != nullptr ? ep->failures : -1;
    }
  }
  bool drainingLeft = false;
  for (auto& c : g_clients) drainingLeft = drainingLeft || c.state == SlotState::kDraining;
  CHECK(!drainingLeft && newSessionsWhileDraining == 0 && cleanupAttempts == 3,
        "draining slot released %d, new sessions while draining %d, failed cleanup attempts while the client id was reserved %d (expected 3)", !drainingLeft,
        newSessionsWhileDraining, cleanupAttempts);
  setScript(kA, FakeNet::Script());
  for (int i = 0; i < 70; i++) {
    advance(1000);
    tick();
    pumpWorker();
  }
  CHECK(isGroupReady("Q5") && overlaps() == 0, "re-added group ready after the cleanup was given up (%d), overlaps %d",
        isGroupReady("Q5"), overlaps());
}

static std::vector<std::pair<uint32_t, bool>> g_doneLog;
static void onPublishDone(uint32_t ticket, bool ok) {
  mainOnly();
  g_doneLog.push_back(std::make_pair(ticket, ok));
}
static bool isChat(const FakeNet::Event& e) { return e.topic.find("/broadcast") != std::string::npos; }

static void testAsyncPublish() {
  g_test = "M17 async publish: the connection is lent to the worker; publishes started meanwhile join the loan in order (presence too), a synchronous one is refused, each callback comes once, the connection comes back after the last";
  resetAll();
  g_doneLog.clear();
  connectGroup("A1");
  std::string id = clientIdOf("A1");
  int connectsBefore = count(FakeNet::events(), "connect", id);
  uint32_t t = publishBinaryAsync("A1", "broadcast", reinterpret_cast<const uint8_t*>("hello"), 5, false, 1, onPublishDone);
  bool lentState = t != 0 && slotOf("A1")->conn == nullptr && slotOf("A1")->lentPublishes == 1;
  bool stillUp = isGroupConnected("A1") && isGroupReady("A1") && g_connFn() == CONN_ONLINE;
  uint32_t t2 = publishBinaryAsync("A1", "broadcast", reinterpret_cast<const uint8_t*>("y"), 1, false, 1, onPublishDone);
  bool syncRefused = !publishBinary("A1", "broadcast", reinterpret_cast<const uint8_t*>("x"), 1, false, 1) && t2 != 0 && t2 != t &&
                     publishRaw("A1", "broadcast", "p", true, 1) && slotOf("A1")->lentPublishes == 3;
  for (int i = 0; i < 5; i++) {
    advance(2000);
    tick();  // no loop() on the lent connection, no setup for the group
  }
  bool noCallbackYet = g_doneLog.empty() && count(FakeNet::events(), "connect", id) == connectsBefore;
  pumpWorker();
  std::string order;
  for (auto& e : FakeNet::events())
    if (e.kind == "publish" && e.clientId == id && isChat(e)) order += e.payload;
  bool publishedOnWorker = order == "hello" "y" "p";
  tick();
  CHECK(lentState && stillUp && syncRefused && noCallbackYet && publishedOnWorker,
        "lent %d, still connected/ready/ON %d, sync refused / async+presence joined %d, nothing before the worker ran %d, published in order '%s'",
        lentState, stillUp, syncRefused, noCallbackYet, order.c_str());
  CHECK(g_doneLog.size() == 2 && g_doneLog[0].first == t && g_doneLog[1].first == t2 && g_doneLog[0].second &&
            g_doneLog[1].second && slotOf("A1")->conn != nullptr && slotOf("A1")->lentPublishes == 0 && isGroupReady("A1") &&
            publishRaw("A1", "broadcast", "z", false, 1),
        "callbacks once each in order (%zu), connection back and usable", g_doneLog.size());

  g_test = "M17c the connection comes back only with the chain's LAST result: after the first, the worker still holds it (the main task touches nothing)";
  resetAll();
  g_doneLog.clear();
  connectGroup("A8");
  publishBinaryAsync("A8", "broadcast", reinterpret_cast<const uint8_t*>("1"), 1, false, 1, onPublishDone);
  publishBinaryAsync("A8", "broadcast", reinterpret_cast<const uint8_t*>("2"), 1, false, 1, onPublishDone);
  {
    Job j;
    if (xQueueReceive(g_jobQueue, &j, 0) == pdTRUE) runJob(j);  // the worker publishes the first only
  }
  tick();
  bool heldAfterFirst = g_doneLog.size() == 1 && slotOf("A8")->conn == nullptr && slotOf("A8")->lentPublishes == 1 &&
                        isGroupConnected("A8") && !publishBinary("A8", "broadcast", reinterpret_cast<const uint8_t*>("x"), 1, false, 1);
  pumpWorker();
  tick();
  CHECK(heldAfterFirst && g_doneLog.size() == 2 && slotOf("A8")->conn != nullptr && slotOf("A8")->lentPublishes == 0 &&
            isGroupReady("A8") && concurrentUse() == 0,
        "held after the first result %d, back and ready after the last %d", heldAfterFirst,
        slotOf("A8")->conn != nullptr && isGroupReady("A8"));

  g_test = "M17d a group deleted (not lent): no publish is queued on its connection before its cleanup takes it";
  resetAll();
  g_doneLog.clear();
  connectGroup("A9");
  deleteGroup("A9");
  bool refused = publishBinaryAsync("A9", "broadcast", reinterpret_cast<const uint8_t*>("x"), 1, false, 1, onPublishDone) == 0;
  for (int i = 0; i < 4; i++) {
    tick();
    pumpWorker();
  }
  CHECK(refused && g_doneLog.empty() && count(FakeNet::events(), "publish", "", isChat) == 0, "refused %d, callbacks %zu", refused,
        g_doneLog.size());

  g_test = "M17b the loan is bounded: 4 publishes per group, the 5th is refused (nothing waits); another group lends on its own";
  resetAll();
  g_doneLog.clear();
  connectGroup("A4");
  connectGroup("A5");
  int accepted = 0;
  for (int i = 0; i < 5; i++)
    accepted += publishBinaryAsync("A4", "broadcast", reinterpret_cast<const uint8_t*>("c"), 1, false, 1, onPublishDone) != 0 ? 1 : 0;
  bool other = publishBinaryAsync("A5", "broadcast", reinterpret_cast<const uint8_t*>("d"), 1, false, 1, onPublishDone) != 0;
  pumpWorker();
  tick();
  CHECK(accepted == 4 && other && g_doneLog.size() == 5 && slotOf("A4")->conn != nullptr && slotOf("A5")->conn != nullptr,
        "accepted %d (want 4), other group %d, callbacks %zu", accepted, other, g_doneLog.size());
  CHECK(concurrentUse() == 0 && overlaps() == 0 && g_offMainCalls.load() == 0, "concurrent %d overlaps %d off-main %d",
        concurrentUse(), overlaps(), g_offMainCalls.load());

  g_test = "M18 async publish fails (no PUBACK): the callback says so, the closed connection is freed, the group reconnects after its window";
  resetAll();
  g_doneLog.clear();
  connectGroup("A2");
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().livePublishFailures = 1;
  }
  t = publishBinaryAsync("A2", "broadcast", reinterpret_cast<const uint8_t*>("hello"), 5, false, 1, onPublishDone);
  uint32_t tChained = publishBinaryAsync("A2", "broadcast", reinterpret_cast<const uint8_t*>("again"), 5, false, 1, onPublishDone);
  pumpWorker();
  tick();
  // The chained publish finds the connection closed: it fails without a network attempt.
  bool failedSeen = g_doneLog.size() == 2 && !g_doneLog[0].second && !g_doneLog[1].second && tChained != 0 &&
                    count(FakeNet::events(), "publish", clientIdOf("A2"), isChat) == 1 && useAfterClose() == 0 &&
                    !isGroupConnected("A2") && g_liveConns.load() == 0;
  advance(6000);
  tick();
  pumpWorker();
  tick();
  CHECK(t != 0 && failedSeen && isGroupReady("A2") && overlaps() == 0, "failure reported (both, the second without a publish) and freed %d, reconnected %d",
        failedSeen, isGroupReady("A2"));

  g_test = "M18b repeated PUBACK timeouts: each closes the connection and reconnects through the backoff, never two sessions, the main task never publishes on a lent client";
  resetAll();
  g_doneLog.clear();
  connectGroup("A6");
  std::string id6 = clientIdOf("A6");
  int reconnects = 0;
  for (int round = 0; round < 3; round++) {
    {
      std::lock_guard<std::mutex> l(FakeNet::st().m);
      FakeNet::st().livePublishFailures = 1;
    }
    publishBinaryAsync("A6", "r2/AABBCCDDEEFF", reinterpret_cast<const uint8_t*>("heartbeat"), 9, true, 1, onPublishDone);
    pumpWorker();
    tick();
    bool closed = !isGroupConnected("A6");
    for (int i = 0; i < 40 && !isGroupReady("A6"); i++) {
      advance(1000);
      tick();
      pumpWorker();
      tick();
    }
    if (closed && isGroupReady("A6")) reconnects++;
  }
  CHECK(reconnects == 3 && g_doneLog.size() == 3 && overlaps() == 0 && liveSessions(id6) == 1 && concurrentUse() == 0 &&
            g_offMainCalls.load() == 0,
        "reconnected after each timeout %d/3, callbacks %zu, overlaps %d, live sessions %d", reconnects, g_doneLog.size(),
        overlaps(), liveSessions(id6));

  g_test = "M18c Wi-Fi recovery while three publishes are chained: the rest are not published, every callback says failed, the connection is closed once, the group reconnects";
  resetAll();
  g_doneLog.clear();
  connectGroup("A7");
  std::string id7 = clientIdOf("A7");
  for (int i = 0; i < 3; i++) publishBinaryAsync("A7", "broadcast", reinterpret_cast<const uint8_t*>("q"), 1, false, 1, onPublishDone);
  g_wifi = false;
  tick();
  g_wifi = true;
  tick();
  // The loan is canceled: a new publish must not join it (its job would carry
  // the new generation and go out on the stale connection).
  bool refusedOnCanceled = publishBinaryAsync("A7", "broadcast", reinterpret_cast<const uint8_t*>("late"), 4, false, 1, onPublishDone) == 0;
  pumpWorker();
  tick();
  bool canceled = g_doneLog.size() == 3 && !g_doneLog[0].second && !g_doneLog[1].second && !g_doneLog[2].second &&
                  count(FakeNet::events(), "publish", id7, isChat) == 0;
  for (int i = 0; i < 6; i++) {
    advance(1000);
    tick();
    pumpWorker();
    tick();
  }
  CHECK(canceled && refusedOnCanceled && isGroupReady("A7") && liveSessions(id7) == 1 && overlaps() == 0 && useAfterClose() == 0,
        "canceled publishes not sent, all failed %d, a later publish refused %d, reconnected %d, live sessions %d", canceled,
        refusedOnCanceled, isGroupReady("A7"), liveSessions(id7));

  g_test = "M19 the group deleted / Wi-Fi recovering / OTA maintenance starting while its connection is lent: the callback still comes once; no duplicate client id";
  for (int variant = 0; variant < 3; variant++) {
    resetAll();
    g_doneLog.clear();
    connectGroup("A3");
    std::string id3 = clientIdOf("A3");
    t = publishBinaryAsync("A3", "broadcast", reinterpret_cast<const uint8_t*>("hello"), 5, false, 1, onPublishDone);
    bool refusedAfter = true;
    if (variant == 0) {
      deleteGroup("A3");
      refusedAfter = publishBinaryAsync("A3", "broadcast", reinterpret_cast<const uint8_t*>("late"), 4, false, 1, onPublishDone) == 0;
    } else if (variant == 1) {
      g_wifi = false;
      tick();
      g_wifi = true;
      tick();
    } else {
      setMaintenanceModeActive(true);
    }
    tick();
    bool waited = count(FakeNet::events(), "connect", id3, cleanConnect) == 0 && count(FakeNet::events(), "connect", id3) == 1;
    pumpWorker();
    for (int i = 0; i < 6; i++) {
      advance(1000);
      tick();
      pumpWorker();
    }
    std::vector<FakeNet::Event> ev = FakeNet::events();
    bool ok;
    if (variant == 0) ok = count(ev, "connect", id3, cleanConnect) == 1 && count(ev, "publish", id3, isOffline) == 1 && liveSessions(id3) == 0;
    else if (variant == 1) ok = isGroupReady("A3") && liveSessions(id3) == 1;
    else ok = !isGroupConnected("A3") && liveSessions(id3) == 0 && count(ev, "publish", id3, isOffline) >= 1;
    CHECK(t != 0 && waited && ok && refusedAfter && g_doneLog.size() == 1 && overlaps() == 0,
          "%s: nothing started on its client id while lent %d, outcome %d, callbacks %zu, overlaps %d",
          variant == 0 ? "deleted" : variant == 1 ? "Wi-Fi recovery" : "maintenance", waited, ok, g_doneLog.size(), overlaps());
    if (variant == 2) setMaintenanceModeActive(false);
  }
}

static void testSleep() {
  g_test = "M11 before deep sleep: OFFLINE for ready groups; a queued setup never publishes ONLINE";
  resetAll();
  connectGroup("S1");
  addGroup("S2");
  tick();
  g_sleepHook();
  pumpWorker();
  std::vector<FakeNet::Event> ev = FakeNet::events();
  CHECK(count(ev, "publish", clientIdOf("S1"), isOffline) == 1 && count(ev, "connect", clientIdOf("S2")) == 0,
        "S1 OFFLINE %d, S2 connects %d", count(ev, "publish", clientIdOf("S1"), isOffline),
        count(ev, "connect", clientIdOf("S2")));
}

static void testReceive() {
  g_test = "M12 receive: messages arriving during the worker's subscribe phase and on the main loop are dispatched on the main task, with their group; a deleted group's are dropped";
  resetAll();
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().deliverOnSubscribe["presence/+"] = {{"morsebuddy/R1/presence/111111111111", "MBP1|111111111111|Bob|ONLINE|1|5|2"},
                                                      {"morsebuddy/R1/presence/222222222222", "MBP1|222222222222|Cy|ONLINE|1|5|4"}};
  }
  connectGroup("R1");
  CHECK(g_presenceIn.size() == 2 && g_presenceIn[0].find("R1|MBP1|111111111111|Bob") == 0,
        "retained presence from the subscribe phase dispatched (%zu)", g_presenceIn.size());
  uint8_t wire[16];
  PacketCodec::encodeHeader(5, 3, wire, sizeof(wire));
  wire[6] = 'a';
  wire[7] = 'b';
  wire[8] = 'c';
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().inbox[clientIdOf("R1")].push_back({"morsebuddy/R1/broadcast", std::string(reinterpret_cast<char*>(wire), 9)});
  }
  tick();
  CHECK(g_packets.size() == 1 && g_packets[0] == "R1|broadcast|3", "packet dispatched (%s)",
        g_packets.empty() ? "-" : g_packets[0].c_str());
  // queued during setup, then the group is deleted before the main task drains them
  resetAll();
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().deliverOnSubscribe["presence/+"] = {{"morsebuddy/Z9/presence/111111111111", "MBP1|111111111111|Bob|ONLINE|1|5|2"}};
  }
  addGroup("Z9");
  tick();
  pumpWorker();
  deleteGroup("Z9");
  tick();
  CHECK(g_presenceIn.empty() && queueCount() == 0, "a deleted group's queued messages are dropped (%zu dispatched)",
        g_presenceIn.size());
  pumpWorker();
  tick();
  CHECK(g_offMainCalls.load() == 0, "calls off the main task: %d", g_offMainCalls.load());
}

static void testPresenceDrift() {
  g_test = "M13 a rename while the worker was connecting: the current ONLINE presence is published at adoption";
  resetAll();
  addGroup("P1");
  tick();
  g_myName = "Alicia";
  pumpWorker();
  tick();
  std::vector<FakeNet::Event> ev = FakeNet::events();
  int alice = 0, alicia = 0;
  for (auto& e : ev) {
    if (e.kind != "publish" || !isOnline(e)) continue;
    if (e.payload.find("|Alice|") != std::string::npos) alice++;
    if (e.payload.find("|Alicia|") != std::string::npos) alicia++;
  }
  CHECK(isGroupReady("P1") && alice == 1 && alicia == 1, "ONLINE as Alice (worker) %d, then as Alicia (main) %d", alice, alicia);
  resetAll();
  connectGroup("P2");
  int online = count(FakeNet::events(), "publish", "", isOnline);
  CHECK(online == 1, "no extra publish when nothing changed (%d)", online);
}

static void testSlotsFull() {
  g_test = "M14 five groups, one deleted while its setup is outstanding and a sixth added: it connects once the slot frees";
  resetAll();
  const char* codes[5] = {"F1", "F2", "F3", "F4", "F5"};
  for (auto c : codes) connectGroup(c);
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().dropNext.insert(clientIdOf("F3"));
  }
  tick();
  advance(6000);
  tick();  // F3's setup queued
  deleteGroup("F3");
  addGroup("F6");
  CHECK(slotOf("F6") == nullptr, "no free slot while F3 drains");
  pumpWorker();
  tick();
  pumpWorker();
  tick();
  pumpWorker();
  tick();
  CHECK(isGroupReady("F6") && overlaps() == 0 && g_liveConns.load() == 5, "F6 ready %d, live %d", isGroupReady("F6"),
        g_liveConns.load());
}

static void testWorkerStartFailure() {
  g_test = "M15 if the worker task cannot be created, nothing blocks and creation is retried";
  resetAll();
  TaskHandle_t saved = g_workerTask;
  g_workerTask = nullptr;
  {
    std::lock_guard<std::mutex> l(FakeRtos::world().m);
    FakeRtos::world().failTaskCreate = true;
  }
  addGroup("K1");
  tick();
  CHECK(g_workerTask == nullptr && !isGroupConnected("K1"), "no worker yet");
  {
    std::lock_guard<std::mutex> l(FakeRtos::world().m);
    FakeRtos::world().failTaskCreate = false;
  }
  tick();
  pumpWorker();
  tick();
  CHECK(g_workerTask != nullptr && isGroupReady("K1"), "worker created on a later tick; group ready");
  (void)saved;
}

static void testBoundedWrite() {
  g_test = "M16 a write to a stalled socket gives up within the 500 ms budget (the ESP32 WiFiClient retries 10 x 1 s)";
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    CHECK(false, "socketpair");
    return;
  }
  BoundedWriteClient c;
  c.attachFd(sv[0]);
  std::vector<uint8_t> chunk(65536, 0x5a);
  bool realTime = g_realTime.load();
  g_realTime = true;
  size_t total = 0;
  auto t0 = std::chrono::steady_clock::now();
  size_t last = 1;
  for (int i = 0; i < 200 && last > 0; i++) {  // the peer never reads: the buffer fills
    last = c.write(chunk.data(), chunk.size());
    total += last;
  }
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  g_realTime = realTime;
  CHECK(last == 0 && ms < 1500, "stalled write returned 0 after %lld ms total (%zu bytes queued first)",
        static_cast<long long>(ms), total);
  size_t small = 0;
  {
    char buf[1 << 16];
    while (recv(sv[1], buf, sizeof(buf), MSG_DONTWAIT) > 0) {
    }
    small = c.write(chunk.data(), 100);
  }
  CHECK(small == 100, "after the peer reads, a write succeeds (%zu)", small);
  close(sv[0]);
  close(sv[1]);
}

// =============================================================================
// Threaded tests (a real worker thread, real time)
// =============================================================================
static bool workerIdle() {
  bool empty;
  {
    std::lock_guard<std::mutex> l(g_jobQueue->m);
    empty = g_jobQueue->items.empty();
  }
  return empty && !FakeNet::atGate();
}
template <class P>
static bool tickUntil(P pred, int ms, uint32_t* maxTickUs = nullptr) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    auto t0 = std::chrono::steady_clock::now();
    tick();
    uint32_t us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    if (maxTickUs != nullptr && us > *maxTickUs) *maxTickUs = us;
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}
static void settle() {
  // let the worker finish whatever it has, taking every result
  for (int i = 0; i < 400; i++) {
    tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    if (i > 20 && workerIdle()) {
      bool anyJob = false;
      for (auto& c : g_clients) anyJob = anyJob || c.jobInFlight;
      if (!anyJob) break;
    }
  }
}

static void testResponsiveness() {
  g_test = "T1 the main tick stays fast while the worker is held inside DNS, TCP connect, CONNACK, a SUBSCRIBE or the ONLINE publish";
  const char* steps[5] = {"dns", "tcp", "connack", "subscribe", "online"};
  for (int s = 0; s < 5; s++) {
    resetAll();
    FakeNet::setGate(steps[s]);
    addGroup("T1");
    uint32_t maxUs = 0;
    bool held = tickUntil([] { return FakeNet::atGate(); }, 3000, &maxUs);
    // the worker is blocked in that step for 1.5 s; the main loop keeps ticking
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    int ticks = 0;
    while (std::chrono::steady_clock::now() < end) {
      auto t0 = std::chrono::steady_clock::now();
      tick();
      uint32_t us = static_cast<uint32_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
      if (us > maxUs) maxUs = us;
      ticks++;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bool notYet = !isGroupConnected("T1");
    FakeNet::release();
    bool ready = tickUntil([] { return isGroupReady("T1"); }, 3000, &maxUs);
    printf("   MEASURE T1 %-9s: %d ticks while held, longest tick %u us\n", steps[s], ticks, maxUs);
    CHECK(held && notYet && ready && maxUs < 20000, "%s: held %d, not visible while held %d, ready after release %d, longest tick %u us",
          steps[s], held, notYet, ready, maxUs);
    settle();
  }
  CHECK(concurrentUse() == 0 && g_offMainCalls.load() == 0, "concurrent client use %d, off-main calls %d", concurrentUse(),
        g_offMainCalls.load());
}

static void testDeleteWhileHeld() {
  g_test = "T2 deleting a group while the worker is inside its subscribe phase: no ONLINE, cleanup after, no overlap, no concurrent use";
  resetAll();
  FakeNet::setGate("subscribe");
  addGroup("H1");
  bool held = tickUntil([] { return FakeNet::atGate(); }, 3000);
  deleteGroup("H1");
  FakeNet::release();
  std::string id = clientIdOf("H1");
  bool cleaned = tickUntil([&] { return count(FakeNet::events(), "connect", id, cleanConnect) == 1; }, 3000);
  settle();
  std::vector<FakeNet::Event> ev = FakeNet::events();
  CHECK(held && cleaned && count(ev, "publish", id, isOnline) == 0 && overlaps() == 0 && concurrentUse() == 0 &&
            liveSessions(id) == 0 && g_liveConns.load() == 0,
        "held %d cleaned %d online %d overlaps %d concurrent %d live sessions %d live conns %d", held, cleaned,
        count(ev, "publish", id, isOnline), overlaps(), concurrentUse(), liveSessions(id), g_liveConns.load());
}

static void testRecoveryStraddle() {
  g_test = "T4 a setup that was past its last cancellation point when Wi-Fi recovered is discarded, not adopted; a fresh setup follows";
  resetAll();
  FakeNet::setGate("online");
  addGroup("V1");
  bool held = tickUntil([] { return FakeNet::atGate(); }, 3000);
  g_wifi = false;
  tick();
  g_wifi = true;
  tick();  // recovery: generation bumped while the worker is inside the ONLINE publish
  FakeNet::release();
  std::string id = clientIdOf("V1");
  bool ready = tickUntil([&] { return isGroupReady("V1") && count(FakeNet::events(), "connect", id, sessionConnect) == 2; }, 5000);
  settle();
  CHECK(held && ready && overlaps() == 0 && liveSessions(id) == 1 && g_liveConns.load() == 1,
        "held %d, ready after a second setup %d (session connects %d), overlaps %d, live sessions %d", held, ready,
        count(FakeNet::events(), "connect", id, sessionConnect), overlaps(), liveSessions(id));
}

static void testAsyncPublishHeld() {
  g_test = "T5 the main tick stays fast while the worker waits for a chat message's PUBACK; the group stays ON";
  resetAll();
  g_doneLog.clear();
  addGroup("P5");
  tickUntil([] { return isGroupReady("P5"); }, 3000);
  FakeNet::setGate("publish");
  uint32_t t = publishBinaryAsync("P5", "broadcast", reinterpret_cast<const uint8_t*>("hello"), 5, false, 1, onPublishDone);
  uint32_t maxUs = 0;
  bool held = tickUntil([] { return FakeNet::atGate(); }, 3000, &maxUs);
  bool on = true;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (std::chrono::steady_clock::now() < end) {
    auto t0 = std::chrono::steady_clock::now();
    tick();
    uint32_t us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    if (us > maxUs) maxUs = us;
    on = on && g_connFn() == CONN_ONLINE && g_doneLog.empty();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  FakeNet::release();
  bool done = tickUntil([] { return !g_doneLog.empty(); }, 3000, &maxUs);
  settle();
  printf("   MEASURE T5 publish held 1 s: longest tick %u us\n", maxUs);
  CHECK(t != 0 && held && on && done && g_doneLog.size() == 1 && g_doneLog[0].second && maxUs < 20000 && concurrentUse() == 0,
        "held %d, ON and no callback while held %d, callback once ok %d, longest tick %u us, concurrent %d", held, on, done, maxUs,
        concurrentUse());
}

// Hardware, both devices on b573ec7: the Race coordinator's retained 130-byte
// heartbeat waited 1506 ms on the main loop for its PUBACK, timed out, and
// MQTT 2.5.2 closed the connection (ON -> WIFI, reconnect). On the worker the
// wait no longer touches the loop, and a PUBACK slower than the old 1.5 s
// bound (but inside the worker's 5 s) keeps the connection. The fake broker
// does not model lwmqtt's own timer: the gate holds the PUBACK wait.
static void testSlowPubackHeartbeat() {
  g_test = "T6 a retained Race heartbeat whose PUBACK takes 3 s: the main tick stays fast, the group stays ON, no reconnect, the publish succeeds";
  resetAll();
  g_doneLog.clear();
  addGroup("P6");
  tickUntil([] { return isGroupReady("P6"); }, 3000);
  std::string id = clientIdOf("P6");
  int connectsBefore = count(FakeNet::events(), "connect", id);
  uint8_t state[130];
  memset(state, 0x5A, sizeof(state));
  FakeNet::setGate("publish");
  uint32_t t = publishBinaryAsync("P6", "r2/AABBCCDDEEFF", state, sizeof(state), true, 1, onPublishDone);
  uint32_t maxUs = 0;
  bool held = tickUntil([] { return FakeNet::atGate(); }, 3000, &maxUs);
  bool on = true;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (std::chrono::steady_clock::now() < end) {
    auto t0 = std::chrono::steady_clock::now();
    tick();
    uint32_t us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    if (us > maxUs) maxUs = us;
    on = on && g_connFn() == CONN_ONLINE;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  FakeNet::release();
  bool done = tickUntil([] { return !g_doneLog.empty(); }, 3000, &maxUs);
  settle();
  bool retainedSent = false;
  for (auto& e : FakeNet::events())
    if (e.kind == "publish" && e.clientId == id && e.retained && e.qos == 1 && e.payload.size() == sizeof(state)) retainedSent = true;
  printf("   MEASURE T6 heartbeat PUBACK held 3 s: longest tick %u us\n", maxUs);
  CHECK(t != 0 && held && on && done && g_doneLog.size() == 1 && g_doneLog[0].second && retainedSent && maxUs < 20000 &&
            count(FakeNet::events(), "connect", id) == connectsBefore && isGroupReady("P6") && concurrentUse() == 0,
        "held %d, ON throughout %d, callback ok %d, retained QoS1 sent %d, longest tick %u us, reconnects %d", held, on, done,
        retainedSent, maxUs, count(FakeNet::events(), "connect", id) - connectsBefore);
}

static void testStress(int iterations, uint32_t seed) {
  g_test = "T3 randomized lifecycle stress with a real worker: groups added/deleted/replaced, Wi-Fi flaps, maintenance, drops, publishes, deliveries";
  resetAll();
  std::mt19937 rng(seed);
  const char* pool[7] = {"S1", "S2", "S3", "S4", "S5", "S6", "S7"};
  FakeNet::Script flaky;
  flaky.connackError = LWMQTT_MISSING_OR_WRONG_PACKET;
  {
    // retained traffic delivered on the worker while it waits for SUBACKs,
    // concurrently with the main task draining the receive queue
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    FakeNet::st().deliverOnSubscribe["presence/+"] = {{"morsebuddy/S1/presence/111111111111", "MBP1|111111111111|A|ONLINE|1|1|1"},
                                                      {"morsebuddy/S1/presence/222222222222", "MBP1|222222222222|B|ONLINE|1|1|2"},
                                                      {"morsebuddy/S1/presence/333333333333", "MBP1|333333333333|C|ONLINE|1|1|3"}};
    FakeNet::st().deliverOnSubscribe["r2/+"] = {{"morsebuddy/S1/r2/ROOM", std::string("\x4d\x42\x01\x05\x00\x00", 6)}};
  }
  bool maintenance = false;
  for (int it = 0; it < iterations; it++) {
    int op = static_cast<int>(rng() % 12);
    const char* code = pool[rng() % 7];
    switch (op) {
      case 0:
      case 1:
        if (indexOfGroup(code) < 0 && g_groups.size() < Settings::kMaxGroups) addGroup(code);
        break;
      case 2:
        deleteGroup(code);
        break;
      case 3:
        g_wifi = !g_wifi.load();
        break;
      case 4:
        maintenance = !maintenance;
        setMaintenanceModeActive(maintenance);
        break;
      case 5: {
        std::lock_guard<std::mutex> l(FakeNet::st().m);
        FakeNet::st().dropNext.insert(clientIdOf(code));
        break;
      }
      case 6:
        publishBinaryAsync(code, "broadcast", reinterpret_cast<const uint8_t*>("aa"), 2, false, 1, onPublishDone);
        publishRaw(code, "broadcast", "x", false, 1);
        publishBinary(code, "radio/session", reinterpret_cast<const uint8_t*>("yy"), 2, false, 0);
        break;
      case 7: {
        std::lock_guard<std::mutex> l(FakeNet::st().m);
        FakeNet::st().inbox[clientIdOf(code)].push_back({std::string("morsebuddy/") + code + "/presence/999999999999", "MBP1|999999999999|Q|ONLINE|1|1|1"});
        break;
      }
      case 8:
        setScript(kA, (rng() % 3 == 0) ? flaky : FakeNet::Script());
        break;
      case 9:
        if (indexOfGroup(code) >= 0) {
          deleteGroup(code);
          addGroup(code);  // replacement
        }
        break;
      default:
        break;
    }
    int ticks = 1 + static_cast<int>(rng() % 6);
    for (int t = 0; t < ticks; t++) {
      tick();
      std::this_thread::sleep_for(std::chrono::microseconds(500 + rng() % 1500));
    }
  }
  if (maintenance) setMaintenanceModeActive(false);
  g_wifi = true;
  setScript(kA, FakeNet::Script());
  bool allReady = tickUntil([] {
    for (auto& g : g_groups)
      if (!isGroupReady(g.code)) return false;
    return true;
  }, 10000);
  int groups = static_cast<int>(g_groups.size());
  while (!g_groups.empty()) deleteGroup(g_groups.front().code);
  settle();
  bool sessionsClear = true;
  {
    std::lock_guard<std::mutex> l(FakeNet::st().m);
    for (auto& kv : FakeNet::st().liveSessions)
      if (kv.second != 0) sessionsClear = false;
  }
  CHECK(allReady, "every remaining group (%d) ready at the end", groups);
  CHECK(overlaps() == 0 && concurrentUse() == 0 && g_offMainCalls.load() == 0,
        "overlapping sessions %d, concurrent client use %d, off-main calls %d", overlaps(), concurrentUse(),
        g_offMainCalls.load());
  CHECK(g_liveConns.load() == 0 && sessionsClear && queueCount() == 0,
        "after deleting every group: live connections %d, broker sessions clear %d, queued %u", g_liveConns.load(),
        sessionsClear, queueCount());
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);  // LeakSanitizer exits without flushing
  std::string mode = argc > 1 ? argv[1] : "manual";
  bool threads = mode == "threads";
  {
    std::lock_guard<std::mutex> l(FakeRtos::world().m);
    FakeRtos::world().manual = !threads;
  }
  g_realTime = threads;
  resetAll();
  g_svc.init();
  if (!threads) {
    testSetupContract();
    testLongGroupCode();
    testFailureBackoff();
    testSetupStageFailures();
    testEndpoints();
    testDnsSecondAddress();
    testDnsFreshAnswer();
    testDnsAllBad();
    testDnsPrefersWorking();
    testReadyLoss();
    testWifi();
    testDeleteDuringSetup();
    testDeleteConnected();
    testReplacement();
    testReplacementQueued();
    testStaleResultNewOccupant();
    testMaintenance();
    testCleanupDuringMaintenance();
    testCleanupAcrossMaintenance();
    testAsyncPublish();
    testCleanupRetryBounded();
    testSleep();
    testReceive();
    testPresenceDrift();
    testSlotsFull();
    testWorkerStartFailure();
    testBoundedWrite();
    resetAll();
  } else {
    int n = argc > 2 ? atoi(argv[2]) : 300;
    testResponsiveness();
    testDeleteWhileHeld();
    testRecoveryStraddle();
    testAsyncPublishHeld();
    testSlowPubackHeartbeat();
    for (uint32_t seed = 1; seed <= 3; seed++) testStress(n, seed);
    settle();
    FakeRtos::stopAll();
    resetAll();
  }
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
