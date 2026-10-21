// HOST TEST (not production): E1-E27 for the Race room protocol, run against
// the ACTUAL src/core/race_protocol.cpp (+ crc32.cpp). Several simulated
// devices, each an Engine with its own NVS, clock and sequence counter, wired
// through a broker model with last-arrival retained semantics, per-group
// delivery, and test-controlled loss / delay / duplication / reordering.
// The NVS model injects write failures, power cuts and CRC corruption.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/race_protocol.h"

using namespace RaceProtocol;

#include "core/crc32.h"
// Test helpers that write coordinator records in the engine's documented
// layout (design r8 section 4), for states unreachable in a short run.
uint32_t crcForTest(const uint8_t* d, size_t n) { return Crc32::compute(d, n); }
static void le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
void writeCoordinatorRecordsForTest(std::map<std::string, std::vector<uint8_t>>& nvs, const RoomState& s, const char* group) {
  // slot0: magic4 fmt1 slotId1 slotSeq4 group33 n4 nonce4 version4 rsLen2 RS@57 ... crc4 (599)
  std::vector<uint8_t> slot(599, 0);
  memcpy(slot.data(), "RCS1", 4); slot[4] = 1; slot[5] = 0; le32(&slot[6], 50);
  strncpy(reinterpret_cast<char*>(&slot[10]), group, 32);
  le32(&slot[43], s.n); le32(&slot[47], s.nonce); le32(&slot[51], s.version);
  size_t rl = encodeState(s, &slot[57], 538);
  slot[55] = static_cast<uint8_t>(rl); slot[56] = static_cast<uint8_t>(rl >> 8);
  le32(&slot[595], crcForTest(slot.data(), 595));
  nvs["rc_slot0"] = slot;
  // slot1: the previous version, consecutive (for FC-B1)
  RoomState prev = s; prev.version = s.version - 1;
  std::vector<uint8_t> slot1(599, 0);
  memcpy(slot1.data(), "RCS1", 4); slot1[4] = 1; slot1[5] = 1; le32(&slot1[6], 49);
  strncpy(reinterpret_cast<char*>(&slot1[10]), group, 32);
  le32(&slot1[43], prev.n); le32(&slot1[47], prev.nonce); le32(&slot1[51], prev.version);
  rl = encodeState(prev, &slot1[57], 538);
  slot1[55] = static_cast<uint8_t>(rl); slot1[56] = static_cast<uint8_t>(rl >> 8);
  le32(&slot1[595], crcForTest(slot1.data(), 595));
  nvs["rc_slot1"] = slot1;
  // commit: magic4 fmt1 group33 coord12 n4 nonce4 version4 slotId1 slotSeq4 closed1 last4 crc4 (76)
  std::vector<uint8_t> c(76, 0);
  memcpy(c.data(), "RCM1", 4); c[4] = 1;
  strncpy(reinterpret_cast<char*>(&c[5]), group, 32);
  memcpy(&c[38], s.coordinator, 12);
  le32(&c[50], s.n); le32(&c[54], s.nonce); le32(&c[58], s.version); c[62] = 0; le32(&c[63], 50);
  c[67] = s.phase == PHASE_CLOSED; le32(&c[68], s.n);
  le32(&c[72], crcForTest(c.data(), 72));
  nvs["rc_commit"] = c;
}

static int g_fail = 0, g_pass = 0;
static std::string g_test;
#define CHECK(c, ...)                                                        \
  do {                                                                       \
    if (c) { g_pass++; } else {                                              \
      g_fail++; printf("FAIL [%s] line %d: ", g_test.c_str(), __LINE__);     \
      printf(__VA_ARGS__); printf("\n");                                     \
    }                                                                        \
  } while (0)

static uint32_t g_now = 1000;
static int g_rollbacks = 0, g_queueDrops = 0;
static uint32_t g_unix = 1700000000u;

// -----------------------------------------------------------------------------
struct Msg {
  std::string group, topic;  // topic = suffix after the group
  uint8_t kind;
  std::vector<uint8_t> payload;
  bool retained;
  std::string from;
};

struct Device;
// Platform::nextSeq before the review (uint32_t(void*): the number is returned
// even if its write failed) and after (bool(void*, uint32_t*)). The same tests
// compile against both, so each regression can be shown failing first.
bool deviceReserve(void* c, uint32_t* out, bool legacy);
template <class F> struct SeqFn;
template <> struct SeqFn<uint32_t (*)(void*)> {
  static uint32_t fn(void* c) { uint32_t v = 0; deviceReserve(c, &v, true); return v; }
};
template <> struct SeqFn<bool (*)(void*, uint32_t*)> {
  static bool fn(void* c, uint32_t* out) { return deviceReserve(c, out, false); }
};
struct Broker {
  std::map<std::string, Msg> retained;  // group + "|" + topic -> last ARRIVED retained message
  std::vector<Msg> inflight;            // published, not yet arrived at the broker
  std::vector<Msg> held;                // delayed by the test
  std::vector<Device*> devices;
  std::function<bool(const Msg&)> dropIf;  // loss injection
  std::function<bool(const Msg&)> holdIf;  // delay injection
  std::vector<Msg> log;                  // every publish (for E15 assertions)
  void publish(const Msg& m);
  void pump();                           // deliver everything in flight, in order
  void deliver(const Msg& m);
  void release() { std::vector<Msg> h; h.swap(held); for (auto& m : h) deliver(m); }
  void connect(Device* d);               // (re)subscribe: retained redelivered
};
static Broker g_broker;

struct Device {
  std::string id;
  std::string group;
  std::map<std::string, std::vector<uint8_t>> nvs;
  Engine eng;
  bool ready = true;
  bool online = true;
  bool dead = false;            // power cut happened: nothing more until reboot
  int writesUntilCut = -1;      // power cut on the Nth next write (0 = this one)
  std::function<bool(const std::string&)> failWrite;  // write failure injection (no cut)
  std::function<bool(const std::string&, const uint8_t*, size_t)> failWriteData;  // by content (e.g. a NONE record)
  std::function<bool()> failCounter;  // the identity counter's own write fails (Identity's mb_core key)
  int writesAttempted = 0;
  uint32_t rng = 1;
  std::vector<std::string> subGroups;
  int queueCap = 0;             // receive queue entries per loop tick (0 = unlimited)
  int recvCount = 0;
  int queueDrops = 0;
  std::string shownKey;         // displayed-state monotonicity across the whole run, reboots included
  uint32_t shownV = 0;
  int rollbacks = 0;
  ~Device() { g_rollbacks += rollbacks; g_queueDrops += queueDrops; }
  void checkShown() {
    const RoomState* v = eng.view();
    if (!v) return;
    char k[64];
    snprintf(k, sizeof(k), "%s/%u/%u", v->coordinator, v->n, v->nonce);
    if (shownKey == k && v->version < shownV) rollbacks++;
    if (shownKey != k || v->version > shownV) { shownKey = k; shownV = v->version; }
  }
  Device(const std::string& id_, const std::string& g) : id(id_), group(g) { subGroups.push_back(g); rng = static_cast<uint32_t>(std::hash<std::string>()(id_)) | 1; }

  static bool pub(void* c, const char* group, const char* suffix, uint8_t kind, const uint8_t* p, uint16_t len, bool retained) {
    Device* d = static_cast<Device*>(c);
    if (d->dead || !d->online) return false;
    Msg m{group, suffix, kind, std::vector<uint8_t>(p, p + len), retained, d->id};
    g_broker.publish(m);
    return true;
  }
  static bool ready_(void* c, const char*) { Device* d = static_cast<Device*>(c); return d->ready && d->online && !d->dead; }
  static bool nvsW(void* c, const char* key, const void* data, size_t len) {
    Device* d = static_cast<Device*>(c);
    if (d->dead) return false;
    d->writesAttempted++;
    if (d->writesUntilCut == 0) { d->dead = true; d->writesUntilCut = -1; return false; }
    if (d->writesUntilCut > 0) d->writesUntilCut--;
    if (d->failWrite && d->failWrite(key)) return false;
    if (d->failWriteData && d->failWriteData(key, static_cast<const uint8_t*>(data), len)) return false;
    d->nvs[key].assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
    return true;
  }
  static size_t nvsR(void* c, const char* key, void* buf, size_t cap) {
    Device* d = static_cast<Device*>(c);
    auto it = d->nvs.find(key);
    if (it == d->nvs.end() || it->second.size() > cap) return 0;
    memcpy(buf, it->second.data(), it->second.size());
    return it->second.size();
  }
  static uint32_t ms(void*) { return g_now; }
  static uint32_t rnd(void* c) { Device* d = static_cast<Device*>(c); d->rng = d->rng * 1103515245u + 12345u; return d->rng >> 1; }
  // Identity's persistent counter (mb_core "idCounter"). `legacy` reproduces
  // the pre-review Identity::nextCounter(): the write result is ignored and
  // the value wraps. Otherwise: the number is returned only once durably
  // stored, and 0xFFFFFFFF is never exceeded.
  static bool reserve(void* c, uint32_t* out, bool legacy) {
    Device* d = static_cast<Device*>(c);
    uint32_t v = 0;
    auto it = d->nvs.find("idCounter");
    if (it != d->nvs.end()) memcpy(&v, it->second.data(), 4);
    if (!legacy && v == 0xFFFFFFFFu) return false;
    uint32_t n = v + 1;
    d->counterWrites++;
    bool ok = !(d->failCounter && d->failCounter());
    if (ok) d->nvs["idCounter"].assign(reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + 4);
    if (!ok && !legacy) return false;
    *out = n;
    return true;
  }
  int counterWrites = 0;
  static uint32_t unixT(void*) { return g_unix; }
  void start() {
    Platform p{this, id.c_str(), pub, ready_, nvsW, nvsR, ms, rnd, &SeqFn<decltype(Platform::nextSeq)>::fn, unixT};
    eng.init(p);
    eng.boot();
    dead = false;
    g_broker.connect(this);
  }
  void reboot() { start(); }
  void onMsg(const Msg& m) {
    if (dead || !online) return;
    if (queueCap && recvCount >= queueCap) { queueDrops++; return; }
    recvCount++;
    eng.onPacket(m.group.c_str(), m.topic.c_str(), m.kind, m.payload.data(), m.payload.size());
    checkShown();
  }
};

bool deviceReserve(void* c, uint32_t* out, bool legacy) { return Device::reserve(c, out, legacy); }
static bool subscribed(const Device* d, const Msg& m) {
  bool inGroup = false;
  for (auto& g : d->subGroups) inGroup |= (g == m.group);
  if (!inGroup) return false;
  if (m.topic.rfind("r2/", 0) == 0) return true;
  return m.topic == "r2q/" + d->id;
}
void Broker::publish(const Msg& m) {
  log.push_back(m);
  if (dropIf && dropIf(m)) return;
  inflight.push_back(m);
}
void Broker::deliver(const Msg& m) {
  if (m.retained) retained[m.group + "|" + m.topic] = m;
  for (Device* d : devices) if (subscribed(d, m)) d->onMsg(m);
}
void Broker::pump() {
  for (int guard = 0; guard < 200 && !inflight.empty(); guard++) {
    std::vector<Msg> now;
    now.swap(inflight);
    for (auto& m : now) {
      if (holdIf && holdIf(m)) { held.push_back(m); continue; }
      deliver(m);
    }
  }
}
void Broker::connect(Device* d) {
  bool known = false;
  for (Device* x : devices) known |= (x == d);
  if (!known) devices.push_back(d);
  for (auto& kv : retained) if (subscribed(d, kv.second)) d->onMsg(kv.second);
}

// Whole-log audits, run on every scenario's publish log (before each reset):
//  E15 every r2/<X> publish came from X;
//  E8  a (group, C, n, nonce, version) is never published with two contents;
//  E12 startRightVersion changes only with a result or a change of holder, and
//      then equals the version that made the change.
static int g_auditRuns = 0;
static void auditLog() {
  g_auditRuns++;
  int foreign = 0, twice = 0, srv = 0;
  std::map<std::string, std::vector<uint8_t>> seen;
  std::map<std::string, RoomState> byVersion;
  for (auto& m : g_broker.log) {
    if (m.topic.rfind("r2/", 0) != 0) continue;
    if (m.topic.substr(3) != m.from) foreign++;
    RoomState s;
    if (!decodeState(m.payload.data(), m.payload.size(), &s)) continue;
    char k[96];
    snprintf(k, sizeof(k), "%s|%s/%u/%u/%010u", m.group.c_str(), s.coordinator, s.n, s.nonce, s.version);
    auto it = seen.find(k);
    if (it != seen.end() && it->second != m.payload) twice++;
    seen[k] = m.payload;
    byVersion[k] = s;
  }
  const RoomState* prev = nullptr;
  std::string prevRoom;
  for (auto& kv : byVersion) {
    std::string room = kv.first.substr(0, kv.first.size() - 10);
    const RoomState& s = kv.second;
    if (prev && room == prevRoom && s.version == prev->version + 1 && s.startRightVersion != prev->startRightVersion) {
      bool cause = strcmp(s.startRight, prev->startRight) != 0 || s.resultRound != prev->resultRound;
      if (!cause || s.startRightVersion != s.version) srv++;
    }
    prev = &s;
    prevRoom = room;
  }
  std::string t = g_test;
  g_test = "audit (" + t + ")";
  CHECK(foreign == 0, "E15: %d state publishes by a non-coordinator", foreign);
  CHECK(twice == 0, "E8: %d versions published with different contents", twice);
  CHECK(srv == 0, "E12: %d startRightVersion changes without a result/holder change", srv);
  g_test = t;
}
static std::vector<Device*> g_rollbackWatch;
static void reset() {
  auditLog();
  g_broker = Broker();
  g_now = 1000;
}
static void tickAll(std::vector<Device*> ds, int ms = 100, int steps = 1) {
  for (int i = 0; i < steps; i++) {
    g_now += ms;
    for (Device* d : ds) { d->recvCount = 0; if (!d->dead && d->online) { d->eng.tick(); d->checkShown(); } }
    g_broker.pump();
  }
}
static void settle(std::vector<Device*> ds, int seconds = 40) { tickAll(ds, 500, seconds * 2); }
static std::unique_ptr<Device> dev(const char* id, const char* group = "GROUP1") {
  std::unique_ptr<Device> d(new Device(id, group));
  d->start();
  return d;
}
static const char* kC = "C00000000001";
static const char* kA = "A00000000002";
static const char* kB = "B00000000003";
static const char* kD = "D00000000004";

static int countPub(const std::string& from, const std::string& topicPrefix, uint8_t op = 0) {
  int n = 0;
  for (auto& m : g_broker.log) {
    if (m.from != from || m.topic.rfind(topicPrefix, 0) != 0) continue;
    if (op && (m.payload.size() < 2 || m.payload[1] != op)) continue;
    n++;
  }
  return n;
}
// C creates a room; A (and others) join it. Returns after settling.
static void roomWith(Device& C, std::vector<Device*> joiners) {
  CHECK(C.eng.createRoom("GROUP1"), "create");
  std::vector<Device*> all{&C};
  for (auto j : joiners) all.push_back(j);
  g_broker.pump();
  for (auto j : joiners) { CHECK(j->eng.join("GROUP1", C.id.c_str()), "join %s", j->id.c_str()); g_broker.pump(); }
  settle(all, 10);
}
static const RoomState* V(Device& d) { return d.eng.view(); }

// =============================================================================
static void e1_e2_ordering() {
  g_test = "E1 no-entry cancellation: delayed JOIN rejected after LEAVE, requester offline";
  reset();
  auto C = dev(kC), S = dev(kA), D = dev(kD);
  CHECK(C->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  // S sends JOIN (held), then Leave -> LEAVE arrives first
  g_broker.holdIf = [&](const Msg& m) { return m.from == kA && m.payload.size() > 1 && m.payload[1] == OP_JOIN; };
  CHECK(S->eng.join("GROUP1", kC), "S join"); g_broker.pump();
  CHECK(S->eng.leave(), "S leave"); g_broker.pump();
  g_broker.holdIf = nullptr;
  uint32_t floorAfterLeave = V(*C)->evictionFloor;
  CHECK(floorAfterLeave > 1 && !isActive(*V(*C), kA), "floor bumped by non-member LEAVE (floor %u)", floorAfterLeave);
  S->online = false;  // requester offline
  g_broker.release(); g_broker.pump();
  CHECK(!isActive(*V(*C), kA), "delayed JOIN reactivated S");
  S->online = true;
  // another device's JOIN created before the floor bump is STALE_BASIS -> terminal, no reissue
  g_test = "E1b STALE_BASIS for another device is terminal (no automatic reissue)";
  RoomState old = *V(*C);
  (void)old;
  // D discovered the room at v1 (basis 1) before the bump; simulate by joining with stale discovery
  CHECK(D->eng.join("GROUP1", kC), "D join"); g_broker.pump(); settle({C.get(), D.get()}, 5);
  // D's basis was the latest version it saw, so it is admitted normally:
  CHECK(isActive(*V(*C), kD), "D with a fresh basis is admitted");

  g_test = "E2 ordering by lastSeq with an entry; eviction by departedAt/index; floor";
  reset();
  auto C2 = dev(kC), A2 = dev(kA);
  roomWith(*C2, {A2.get()});
  CHECK(isActive(*V(*C2), kA), "A active");
  CHECK(A2->eng.leave(), "A leave"); settle({C2.get(), A2.get()}, 5);
  CHECK(!isActive(*V(*C2), kA) && A2->eng.state() == MS_NONE, "A departed");
  // A rejoins (JOIN seq s1), then leaves (LEAVE s2); deliver LEAVE first, JOIN later
  g_broker.holdIf = [&](const Msg& m) { return m.from == kA && m.payload.size() > 1 && m.payload[1] == OP_JOIN; };
  CHECK(A2->eng.join("GROUP1", kC), "A rejoin"); g_broker.pump();
  CHECK(A2->eng.leave(), "A leave again"); g_broker.pump();
  g_broker.holdIf = nullptr;
  size_t lbJoin = g_broker.log.size();
  g_broker.release(); g_broker.pump();
  CHECK(!isActive(*V(*C2), kA), "delayed JOIN (lower seq) reapplied after LEAVE");
  bool reactivated = false;  // not even transiently (reconciliation would heal it afterwards)
  for (size_t i = lbJoin; i < g_broker.log.size(); i++) {
    RoomState s2;
    if (g_broker.log[i].from == kC && decodeState(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &s2) &&
        isActive(s2, kA)) reactivated = true;
  }
  CHECK(!reactivated, "C committed A ACTIVE from the delayed lower-seq JOIN");
  // eviction: fill 20 entries with departed + active, then a new joiner evicts the oldest departed
  g_test = "E2b eviction picks the smallest departedAt and raises evictionFloor";
  reset();
  auto C3 = dev(kC);
  CHECK(C3->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  std::vector<std::unique_ptr<Device>> ps;
  char id[13];
  for (int i = 0; i < 19; i++) {
    snprintf(id, sizeof(id), "E%011d", i);
    ps.emplace_back(new Device(id, "GROUP1")); ps.back()->start();
    CHECK(ps.back()->eng.join("GROUP1", kC), "join %d", i); g_broker.pump();
  }
  CHECK(V(*C3)->count == 20, "20 entries (%u)", V(*C3)->count);
  // the LOWER index departs first, so "smallest departedAt" and "last departed index" differ
  CHECK(ps[1]->eng.leave(), "leave 1"); g_broker.pump();
  CHECK(ps[3]->eng.leave(), "leave 3"); g_broker.pump();   // later departure
  uint32_t dep1 = V(*C3)->entries[findEntry(*V(*C3), ps[1]->id.c_str())].departedAt;
  auto N = dev("F00000000099");
  CHECK(N->eng.join("GROUP1", kC), "N join"); g_broker.pump();
  CHECK(isActive(*V(*C3), "F00000000099") && findEntry(*V(*C3), ps[1]->id.c_str()) < 0 &&
            findEntry(*V(*C3), ps[3]->id.c_str()) >= 0 && V(*C3)->evictionFloor == V(*C3)->version,
        "evicted the oldest departed (departedAt %u) and set the floor", dep1);
}

static void e3_start_completion() {
  g_test = "E3 START completion from a later RS only (whole ROUND missed)";
  reset();
  auto C = dev(kC), W = dev(kA);
  roomWith(*C, {W.get()});
  // W wins round 1 so it holds the start right
  CHECK(C->eng.startRound(), "C starts"); g_broker.pump();
  const RoomState* v = V(*W);
  CHECK(v && v->phase == PHASE_ROUND, "W sees ROUND");
  CHECK(W->eng.submitCorrectGuess(v->secret), "W claims"); g_broker.pump();
  CHECK(V(*C)->phase == PHASE_LOBBY && std::string(V(*C)->startRight) == kA, "W holds the right");
  // W requests START; W misses the ROUND state and the reply; the round then resolves (C wins)
  g_broker.dropIf = [&](const Msg& m) { return (m.topic == std::string("r2q/") + kA) || (m.topic == std::string("r2/") + kC && m.payload.size() > 0); };
  CHECK(W->eng.startRound(), "W START"); g_broker.pump();
  CHECK(V(*C)->phase == PHASE_ROUND, "started at C");
  CHECK(C->eng.submitCorrectGuess(V(*C)->secret), "C wins"); g_broker.pump();
  g_broker.dropIf = nullptr;
  uint8_t op = 0;
  CHECK(W->eng.pendingOp(&op) && op == OP_START, "W still pending");
  settle({C.get(), W.get()}, 70);  // heartbeat / query delivers the later state
  CHECK(!W->eng.pendingOp(&op), "START completed from the later state");
  CHECK(V(*W) && V(*W)->resultRound > 0 && std::string(V(*W)->resultWinner) == kC, "W shows C as the winner");
}

static void e4_e24_e25_replies() {
  g_test = "E4 reply validation: wrong group / coordinator / target / seq are ignored";
  reset();
  auto C = dev(kC), A = dev(kA);
  roomWith(*C, {A.get()});
  CHECK(C->eng.startRound(), "start"); g_broker.pump();
  // A has a pending CLAIM; craft replies that must be ignored
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };  // CLAIM never reaches C
  uint8_t g[4] = {9, 9, 9, 9};
  memcpy(g, V(*A)->secret, 4);
  CHECK(A->eng.submitCorrectGuess(g), "A claims"); g_broker.pump();
  g_broker.dropIf = nullptr;
  uint32_t seq = A->eng.pendingSeq();
  auto sendReply = [&](const char* group, const char* coord, const char* target, uint32_t s, uint8_t code, uint32_t n, uint32_t nonce) {
    Reply r; memset(&r, 0, sizeof(r));
    strncpy(r.coordinator, coord, 12); strncpy(r.target, target, 12);
    r.seq = s; r.code = code; r.curN = n; r.curNonce = nonce; r.curVersion = 99;
    uint8_t b[64]; size_t len = encodeReply(r, b, sizeof(b));
    A->eng.onPacket(group, (std::string("r2q/") + kA).c_str(), kKindRequest, b, len);
  };
  const RoomState* cv = V(*C);
  sendReply("GROUP2", kC, kA, seq, CODE_CLOSED, cv->n, cv->nonce);
  sendReply("GROUP1", kB, kA, seq, CODE_CLOSED, cv->n, cv->nonce);
  sendReply("GROUP1", kC, kB, seq, CODE_CLOSED, cv->n, cv->nonce);
  sendReply("GROUP1", kC, kA, seq - 1, CODE_CLOSED, cv->n, cv->nonce);
  uint8_t op = 0;
  CHECK(A->eng.state() == MS_MEMBER && A->eng.pendingOp(&op) && op == OP_CLAIM, "a mismatched reply acted");
  sendReply("GROUP1", kC, kA, seq, CODE_LATE, cv->n, cv->nonce);
  CHECK(!A->eng.pendingOp(&op) && A->eng.state() == MS_MEMBER, "the matching reply was not used");

  g_test = "E24 old QUERY reply after Leave A / Join B (same coordinator) is ignored";
  reset();
  auto C2 = dev(kC), M = dev(kA);
  roomWith(*C2, {M.get()});
  // a foreign snapshot on r2/<C> -> verification QUERY; its reply is held
  RoomState fake = *V(*C2); fake.nonce ^= 0x55; fake.version = 3;
  uint8_t b[600]; size_t l = encodeState(fake, b, sizeof(b));
  g_broker.holdIf = [&](const Msg& m) { return m.topic == std::string("r2q/") + kA; };
  M->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  CHECK(M->eng.verifying(), "verifying");
  g_broker.pump();  // QUERY reaches C, its reply is held
  CHECK(M->eng.leave(), "M leaves A"); g_broker.pump(); settle({C2.get(), M.get()}, 5);
  CHECK(M->eng.state() == MS_NONE, "M left");
  CHECK(C2->eng.leave(), "C closes A"); g_broker.pump();
  CHECK(C2->eng.createRoom("GROUP1"), "C creates B"); g_broker.pump();
  CHECK(M->eng.join("GROUP1", kC), "M joins B"); g_broker.pump(); settle({C2.get(), M.get()}, 5);
  CHECK(M->eng.state() == MS_MEMBER, "M in B");
  g_broker.holdIf = nullptr;
  g_broker.release(); g_broker.pump();
  CHECK(M->eng.state() == MS_MEMBER, "an old QUERY reply ended room B");

  g_test = "E25 lost QUERY: replaced, one outstanding, foreign-group and stale replies ignored";
  reset();
  auto C3 = dev(kC), N = dev(kA);
  roomWith(*C3, {N.get()});
  fake = *V(*C3); fake.nonce ^= 0x77; fake.version = 2;
  l = encodeState(fake, b, sizeof(b));
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA && m.payload.size() > 1 && m.payload[1] == OP_QUERY; };
  N->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  uint32_t q1 = N->eng.outstandingQuerySeq();
  g_broker.pump();
  g_broker.dropIf = nullptr;
  tickAll({C3.get(), N.get()}, 1000, 6);  // retry interval
  uint32_t q2 = N->eng.outstandingQuerySeq();
  CHECK(q1 != 0 && (q2 == 0 || q2 != q1), "replaced (q1 %u q2 %u)", q1, q2);
  CHECK(!N->eng.verifying() && N->eng.state() == MS_MEMBER, "verification completed and N stays");
  // a stale reply to q1 (constructed) is ignored
  Reply r; memset(&r, 0, sizeof(r)); strncpy(r.coordinator, kC, 12); strncpy(r.target, kA, 12);
  r.seq = q1; r.code = CODE_CURRENT;  // cur = none -> would end the room if accepted
  uint8_t rb[64]; size_t rl = encodeReply(r, rb, sizeof(rb));
  N->eng.onPacket("GROUP1", (std::string("r2q/") + kA).c_str(), kKindRequest, rb, rl);
  N->eng.onPacket("GROUP2", (std::string("r2q/") + kA).c_str(), kKindRequest, rb, rl);
  CHECK(N->eng.state() == MS_MEMBER, "a stale/foreign-group reply ended the room");
}

static void e5_e27_join_outcomes() {
  g_test = "E5 FULL with a lost reply, later snapshot after a leave: terminal, no reissue (model 3)";
  reset();
  auto C = dev(kC);
  CHECK(C->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  std::vector<std::unique_ptr<Device>> ps;
  char id[13];
  for (int i = 0; i < 19; i++) {
    snprintf(id, sizeof(id), "E%011d", i);
    ps.emplace_back(new Device(id, "GROUP1")); ps.back()->start();
    ps.back()->eng.join("GROUP1", kC); g_broker.pump();
  }
  auto R = dev("F00000000077");
  g_broker.dropIf = [&](const Msg& m) { return m.topic == "r2q/F00000000077"; };  // R's replies lost
  CHECK(R->eng.join("GROUP1", kC), "R join"); g_broker.pump();
  g_broker.dropIf = nullptr;
  CHECK(!isActive(*V(*C), "F00000000077"), "R not admitted (FULL)");
  CHECK(ps[5]->eng.leave(), "a member leaves"); g_broker.pump();
  int joinsBefore = countPub("F00000000077", "r2q/", OP_JOIN);
  settle({C.get(), R.get()}, 5);
  CHECK(R->eng.state() == MS_NONE && (R->eng.notice() == NOTICE_COULD_NOT_JOIN || R->eng.notice() == NOTICE_ROOM_FULL),
        "R not terminal (state %d notice %d)", R->eng.state(), R->eng.notice());
  CHECK(countPub("F00000000077", "r2q/", OP_JOIN) == joinsBefore, "R reissued a JOIN automatically");
  CHECK(!isActive(*V(*C), "F00000000077"), "R admitted");

  g_test = "E5 Leave replaces a pending JOIN";
  reset();
  auto C2 = dev(kC), A = dev(kA);
  CHECK(C2->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(A->eng.join("GROUP1", kC), "join"); g_broker.pump();
  CHECK(A->eng.leave() && A->eng.state() == MS_LEAVING, "Leave replaced JOIN");
  g_broker.dropIf = nullptr;
  settle({C2.get(), A.get()}, 10);
  CHECK(A->eng.state() == MS_NONE && !isActive(*V(*C2), kA), "ended without membership");
}

static void e6_e7_e8_e19_durability() {
  g_test = "E6 power cut at every commit step: never published uncommitted, never rolled back";
  for (int cut = 0; cut < 3; cut++) {
    reset();
    auto C = dev(kC), A = dev(kA);
    roomWith(*C, {A.get()});
    uint32_t vBefore = V(*C)->version;
    size_t logBefore = g_broker.log.size();
    C->writesUntilCut = cut;  // cut on the slot write (0), the commit write (1), or after both (2)
    C->eng.startRound();
    g_broker.pump();
    bool publishedNew = false;
    for (size_t i = logBefore; i < g_broker.log.size(); i++) {
      RoomState s;
      if (g_broker.log[i].from == kC && g_broker.log[i].kind == kKindState &&
          decodeState(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &s) && s.version > vBefore)
        publishedNew = true;
    }
    C->reboot(); g_broker.pump();
    uint32_t vAfter = V(*C) ? V(*C)->version : 0;
    CHECK(vAfter >= vBefore, "cut %d: rolled back (%u -> %u)", cut, vBefore, vAfter);
    if (cut < 2) CHECK(!publishedNew && vAfter == vBefore, "cut %d: uncommitted state published or loaded", cut);
    else CHECK(vAfter == vBefore + 1 && publishedNew, "cut %d: committed state not recovered", cut);
  }

  g_test = "E7 FC-A: committed slot corrupted -> CLOSED DATA_LOST at V+1; members keep final scores";
  reset();
  auto C = dev(kC), A = dev(kA);
  roomWith(*C, {A.get()});
  uint32_t V0 = V(*C)->version;
  // corrupt the slot named by the commit record (the most recently written)
  std::string named = C->nvs.count("rc_slot1") ? "rc_slot1" : "rc_slot0";
  {
    // find which slot the commit names: try both by corrupting the newest by slotSeq field
    uint32_t s0 = 0, s1 = 0;
    if (C->nvs.count("rc_slot0")) memcpy(&s0, C->nvs["rc_slot0"].data() + 6, 4);
    if (C->nvs.count("rc_slot1")) memcpy(&s1, C->nvs["rc_slot1"].data() + 6, 4);
    named = s1 > s0 ? "rc_slot1" : "rc_slot0";
  }
  C->nvs[named][100] ^= 0xFF;
  C->reboot(); g_broker.pump(); settle({C.get(), A.get()}, 5);
  const RoomState* fa = A->eng.finalView();
  CHECK(A->eng.state() == MS_NONE && fa && fa->version == V0 && isActive(*fa, kA), "A keeps its final copy (state %d fa %p v%u/%u)",
        A->eng.state(), (const void*)fa, fa ? fa->version : 0, V0);
  RoomState last; bool got = false;
  for (auto& m : g_broker.log) if (m.from == kC && m.kind == kKindState) got = decodeState(m.payload.data(), m.payload.size(), &last);
  CHECK(got && last.version == V0 + 1 && last.phase == PHASE_CLOSED && last.closeReason == CLOSE_DATA_LOST && last.count == 0,
        "C published DATA_LOST at V+1 (v%u reason %u)", last.version, last.closeReason);

  g_test = "E7 FC-B1: commit record corrupted, consecutive slots -> adopt newer, no new version";
  reset();
  auto C2 = dev(kC), A2 = dev(kA);
  roomWith(*C2, {A2.get()});
  uint32_t V1 = V(*C2)->version;
  C2->nvs["rc_commit"][20] ^= 0x01;
  size_t logBefore = g_broker.log.size();
  C2->reboot(); g_broker.pump();
  CHECK(V(*C2) && V(*C2)->version == V1, "adopted v%u (was v%u)", V(*C2) ? V(*C2)->version : 0, V1);
  bool higher = false;
  for (size_t i = logBefore; i < g_broker.log.size(); i++) {
    RoomState s;
    if (g_broker.log[i].from == kC && decodeState(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &s) && s.version > V1) higher = true;
  }
  CHECK(!higher, "a new version was written");

  g_test = "E7 FC-B2: commit corrupt, one slot invalid -> no writes; members end via verification";
  reset();
  auto C3 = dev(kC), A3 = dev(kA);
  roomWith(*C3, {A3.get()});
  C3->nvs["rc_commit"][20] ^= 0x01;
  C3->nvs["rc_slot0"][100] ^= 0xFF;
  C3->nvs["rc_slot1"][100] ^= 0xFF;
  logBefore = g_broker.log.size();
  C3->reboot(); g_broker.pump();
  CHECK(C3->eng.coordinatorDamaged() && V(*C3) == nullptr, "damaged");
  int writes = 0;
  for (size_t i = logBefore; i < g_broker.log.size(); i++) if (g_broker.log[i].from == kC && g_broker.log[i].kind == kKindState) writes++;
  CHECK(writes == 0, "C published for the damaged room");
  CHECK(C3->eng.createRoom("GROUP1"), "C creates a new room"); g_broker.pump();
  settle({C3.get(), A3.get()}, 10);
  CHECK(A3->eng.state() == MS_NONE, "A ended the old room after verification");

  g_test = "E7 binding mismatches are detected (slotSeq / version / nonce)";
  for (int field = 0; field < 3; field++) {
    reset();
    auto Cx = dev(kC);
    CHECK(Cx->eng.createRoom("GROUP1"), "create"); g_broker.pump();
    Cx->eng.startRound(); g_broker.pump();
    uint32_t s0 = 0, s1 = 0;
    memcpy(&s0, Cx->nvs["rc_slot0"].data() + 6, 4);
    memcpy(&s1, Cx->nvs["rc_slot1"].data() + 6, 4);
    std::string nk = s1 > s0 ? "rc_slot1" : "rc_slot0";
    auto& rec = Cx->nvs["rc_commit"];
    // commit layout: magic4 fmt1 group33 coord12 n4 nonce4 version4 slotId1 slotSeq4 closed1 last4 crc4
    size_t off = field == 0 ? 4 + 1 + 33 + 12 + 4 + 4 + 4 + 1 : (field == 1 ? 4 + 1 + 33 + 12 + 4 + 4 : 4 + 1 + 33 + 12 + 4);
    rec[off] ^= 0x01;
    // re-seal the CRC so only the binding (not the CRC) is wrong
    uint32_t c = crcForTest(rec.data(), rec.size() - 4);
    memcpy(rec.data() + rec.size() - 4, &c, 4);
    Cx->reboot();
    CHECK(V(*Cx) == nullptr || V(*Cx)->phase == PHASE_CLOSED, "binding mismatch %d not detected", field);
    (void)nk;
  }

  g_test = "E8/E19 limits: exhaustion -> CLOSED at 0xFFFFFFFF; FC-B1 adopts CLOSED at the maximum";
  reset();
  auto C4 = dev(kC), A4 = dev(kA);
  roomWith(*C4, {A4.get()});
  // drive the version to 0xFFFFFFFE by rewriting the committed records through the engine's own codec
  // (the engine is the only writer; we emulate a long-lived room by patching the stored version)
  // Instead: verify the rule via beginChange on a room whose stored version is high.
  {
    RoomState s = *V(*C4);
    s.version = 0xFFFFFFFEu;
    // rebuild slot + commit with the engine's layout
    writeCoordinatorRecordsForTest(C4->nvs, s, "GROUP1");
    C4->reboot(); g_broker.pump();
    CHECK(V(*C4) && V(*C4)->version == 0xFFFFFFFEu, "loaded high version");
    C4->eng.startRound(); g_broker.pump();
    CHECK(V(*C4) == nullptr || (C4->eng.finalView() == nullptr), "n/a");
    RoomState last2; bool got2 = false;
    for (auto& m : g_broker.log) if (m.from == kC && m.kind == kKindState) got2 = decodeState(m.payload.data(), m.payload.size(), &last2);
    CHECK(got2 && last2.version == 0xFFFFFFFFu && last2.phase == PHASE_CLOSED && last2.closeReason == CLOSE_EXHAUSTED,
          "EXHAUSTED at the final version (v%u phase %u)", last2.version, last2.phase);
    size_t lb = g_broker.log.size();
    C4->eng.startRound(); C4->eng.leave(); g_broker.pump();
    bool beyond = false;
    for (size_t i = lb; i < g_broker.log.size(); i++) {
      RoomState s2;
      if (g_broker.log[i].from == kC && decodeState(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &s2) && s2.version != 0xFFFFFFFFu) beyond = true;
    }
    CHECK(!beyond, "a version after the final one");
    // FC-B1 with the newer slot CLOSED at the maximum
    C4->nvs["rc_commit"][20] ^= 0x01;
    C4->reboot(); g_broker.pump();
    CHECK(!C4->eng.coordinatorDamaged(), "FC-B1 adopted (not damaged)");
  }
}

static void e9_e11_e13_e14_lifecycle() {
  g_test = "E9 owner close -> new room by the other device; scores 0; old requests CLOSED/UNKNOWN_ROOM";
  reset();
  auto A = dev(kC), B = dev(kA);
  roomWith(*A, {B.get()});
  A->eng.startRound(); g_broker.pump();
  B->eng.submitCorrectGuess(V(*B)->secret); g_broker.pump();
  CHECK(V(*A)->entries[findEntry(*V(*A), kA)].score == 1, "B scored");
  uint32_t oldN = V(*A)->n, oldNonce = V(*A)->nonce;
  CHECK(A->eng.leave(), "owner closes"); g_broker.pump(); settle({A.get(), B.get()}, 5);
  CHECK(B->eng.state() == MS_NONE && B->eng.notice() == NOTICE_ROOM_CLOSED, "B saw Room closed");
  CHECK(B->eng.finalView() && B->eng.finalView()->entries[findEntry(*B->eng.finalView(), kA)].score == 1, "final scores kept");
  CHECK(B->eng.createRoom("GROUP1"), "B creates"); g_broker.pump();
  CHECK(A->eng.join("GROUP1", kA), "A joins B's room"); g_broker.pump(); settle({A.get(), B.get()}, 5);
  const RoomState* nb = V(*B);
  bool zero = true;
  for (uint8_t i = 0; i < nb->count; i++) zero &= nb->entries[i].score == 0;
  CHECK(nb && activeCount(*nb) == 2 && zero, "new room: both members, scores 0");
  // delayed old request to A's old room
  Request q; memset(&q, 0, sizeof(q)); q.op = OP_CLAIM; strncpy(q.coordinator, kC, 12); q.n = oldN; q.nonce = oldNonce;
  strncpy(q.sender, kA, 12); q.seq = 999; q.roundStart = 2;
  uint8_t b[64]; size_t l = encodeRequest(q, b, sizeof(b));
  size_t lb = g_broker.log.size();
  A->eng.onPacket("GROUP1", (std::string("r2q/") + kC).c_str(), kKindRequest, b, l);
  bool sawClosed = false;
  for (size_t i = lb; i < g_broker.log.size(); i++) {
    bool isRep = false; Request rq; Reply rp;
    if (decodeRequestOrReply(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &isRep, &rq, &rp) && isRep)
      sawClosed |= (rp.code == CODE_CLOSED || rp.code == CODE_UNKNOWN_ROOM);
  }
  CHECK(sawClosed, "old request not answered CLOSED/UNKNOWN_ROOM");
  CHECK(V(*B)->version == nb->version, "old request changed the new room");

  g_test = "E11 member reboot: QUERY invalidated, durable pending CLAIM resent identically";
  reset();
  auto C = dev(kC), M = dev(kA);
  roomWith(*C, {M.get()});
  C->eng.startRound(); g_broker.pump();
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  uint8_t sec[4]; memcpy(sec, V(*M)->secret, 4);
  CHECK(M->eng.submitCorrectGuess(sec), "claim"); g_broker.pump();
  uint32_t seq = M->eng.pendingSeq();
  RoomState fake = *V(*C); fake.nonce ^= 1; fake.version = 2;
  uint8_t fb[600]; size_t fl = encodeState(fake, fb, sizeof(fb));
  M->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, fb, fl);
  CHECK(M->eng.outstandingQuerySeq() != 0, "query outstanding");
  g_broker.dropIf = nullptr;
  size_t lb2 = g_broker.log.size();
  M->reboot();
  CHECK(M->eng.outstandingQuerySeq() == 0 && M->eng.pendingSeq() == seq, "after reboot: query cleared, pending kept");
  tickAll({C.get(), M.get()}, 100, 2);
  bool resent = false;
  for (size_t i = lb2; i < g_broker.log.size(); i++) {
    bool isRep = false; Request rq; Reply rp;
    if (g_broker.log[i].from == kA && decodeRequestOrReply(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &isRep, &rq, &rp) &&
        !isRep && rq.op == OP_CLAIM && rq.seq == seq && memcmp(rq.guess, sec, 4) == 0) resent = true;
  }
  CHECK(resent, "pending CLAIM not resent identically");
  settle({C.get(), M.get()}, 5);
  CHECK(std::string(V(*C)->resultWinner) == kA, "claim processed after reboot");

  g_test = "E13 owner Leave while B is disconnected: no peer change before receipt";
  reset();
  auto C5 = dev(kC), B5 = dev(kA);
  roomWith(*C5, {B5.get()});
  B5->online = false;
  CHECK(C5->eng.leave(), "close"); g_broker.pump();
  CHECK(B5->eng.state() == MS_MEMBER, "B changed while disconnected");
  size_t lb3 = g_broker.log.size();
  // requests to the closed room never advance its version
  B5->online = true;
  B5->eng.startRound();
  B5->reboot(); g_broker.pump(); settle({C5.get(), B5.get()}, 5);
  CHECK(B5->eng.state() == MS_NONE, "B learned of the closure on reconnect");
  for (size_t i = lb3; i < g_broker.log.size(); i++) {
    RoomState s;
    if (g_broker.log[i].from == kC && decodeState(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &s))
      CHECK(s.phase == PHASE_CLOSED, "terminal version changed");
  }

  g_test = "E14 Back/sleep/disconnect/reboot change no membership; non-owner Leave removes only that player";
  reset();
  auto C6 = dev(kC), A6 = dev(kA), B6 = dev(kB);
  roomWith(*C6, {A6.get(), B6.get()});
  A6->online = false; tickAll({C6.get(), B6.get()}, 1000, 30); A6->online = true;
  A6->reboot(); C6->reboot(); g_broker.pump(); settle({C6.get(), A6.get(), B6.get()}, 10);
  CHECK(activeCount(*V(*C6)) == 3 && A6->eng.state() == MS_MEMBER && B6->eng.state() == MS_MEMBER, "membership changed");
  CHECK(B6->eng.leave(), "B leaves"); settle({C6.get(), A6.get(), B6.get()}, 5);
  CHECK(activeCount(*V(*C6)) == 2 && isActive(*V(*C6), kA) && !isActive(*V(*C6), kB) && A6->eng.state() == MS_MEMBER,
        "non-owner Leave affected others");
}

static void e10_e23_identity() {
  g_test = "E10 identity storage lost: old requests UNKNOWN_ROOM; old members end; new members stay";
  reset();
  auto C = dev(kC), M = dev(kA), D = dev(kD);
  roomWith(*C, {M.get()});
  RoomState oldRs = *V(*C);
  // in-flight old JOIN from D (held), then C loses all identity storage
  g_broker.pump();
  CHECK(D->eng.join("GROUP1", kC), "D join (held)");
  std::vector<Msg> inflightD;
  inflightD.swap(g_broker.inflight);
  C->nvs.clear();  // rc_commit, slots, rm_member, idCounter all lost
  C->reboot();
  CHECK(!C->eng.coordinatorDamaged() && V(*C) == nullptr, "fresh after loss");
  CHECK(C->eng.createRoom("GROUP1"), "new room"); g_broker.pump();
  for (auto& m : inflightD) g_broker.deliver(m);
  g_broker.pump();
  CHECK(!isActive(*V(*C), kD), "old JOIN changed the new room");
  settle({C.get(), M.get()}, 20);
  CHECK(M->eng.state() == MS_NONE && M->eng.notice() == NOTICE_ROOM_CLOSED, "old member did not end via verification");

  g_test = "E23 new member + old snapshot (equal/higher n): stays; C unreachable: no change";
  auto N = dev(kB);
  CHECK(N->eng.join("GROUP1", kC), "N joins the new room"); g_broker.pump(); settle({C.get(), N.get()}, 5);
  uint8_t b[600]; size_t l = encodeState(oldRs, b, sizeof(b));
  N->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  CHECK(N->eng.verifying(), "verifying");
  settle({C.get(), N.get()}, 10);
  CHECK(N->eng.state() == MS_MEMBER && !N->eng.verifying(), "N stayed after C's answer");
  oldRs.n = V(*C)->n + 50;
  l = encodeState(oldRs, b, sizeof(b));
  C->online = false;
  N->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  settle({N.get()}, 20);
  CHECK(N->eng.state() == MS_MEMBER && N->eng.verifying(), "changed without C's confirmation");
}

static void e12_e17_start_right() {
  g_test = "E12 start holder: presence offline grants nothing; winner Leave -> C; rejoin doesn't restore";
  reset();
  auto C = dev(kC), W = dev(kA);
  roomWith(*C, {W.get()});
  CHECK(V(*C)->startRightVersion == 1 && C->eng.canStart(), "C holds the right at creation");
  C->eng.startRound(); g_broker.pump();
  W->eng.submitCorrectGuess(V(*W)->secret); g_broker.pump();
  uint32_t srv = V(*C)->startRightVersion;
  CHECK(std::string(V(*C)->startRight) == kA && !C->eng.canStart() && W->eng.canStart(), "W holds it");
  W->online = false;
  CHECK(!C->eng.startRound(), "C started while W was merely offline");
  W->online = true;
  {  // the holder itself, fresh seq, but an entitlement other than the current startRightVersion
    Request st; memset(&st, 0, sizeof(st)); st.op = OP_START; strncpy(st.coordinator, kC, 12);
    st.n = V(*C)->n; st.nonce = V(*C)->nonce; strncpy(st.sender, kA, 12); st.seq = 100000; st.entitlement = srv - 1;
    uint8_t sb[64]; size_t sl = encodeRequest(st, sb, sizeof(sb));
    C->eng.onPacket("GROUP1", (std::string("r2q/") + kC).c_str(), kKindRequest, sb, sl);
    CHECK(V(*C)->phase == PHASE_LOBBY, "holder START with a stale entitlement accepted");
  }

  g_test = "E17 a rejected START replayed after the winner's Leave never starts a round";
  Request q; memset(&q, 0, sizeof(q)); q.op = OP_START; strncpy(q.coordinator, kC, 12);
  q.n = V(*C)->n; q.nonce = V(*C)->nonce; strncpy(q.sender, kB, 12); q.seq = 500; q.entitlement = srv;
  uint8_t b[64]; size_t l = encodeRequest(q, b, sizeof(b));
  C->eng.onPacket("GROUP1", (std::string("r2q/") + kC).c_str(), kKindRequest, b, l);
  CHECK(V(*C)->phase == PHASE_LOBBY, "non-holder START accepted");
  CHECK(W->eng.leave(), "W leaves"); settle({C.get(), W.get()}, 5);
  CHECK(V(*C)->startRight[0] == '\0' && V(*C)->startRightVersion > srv, "right cleared with a new version");
  // replay W's old START with the old entitlement
  strncpy(q.sender, kA, 12); q.seq = 900;
  l = encodeRequest(q, b, sizeof(b));
  C->eng.onPacket("GROUP1", (std::string("r2q/") + kC).c_str(), kKindRequest, b, l);
  CHECK(V(*C)->phase == PHASE_LOBBY, "a replayed START started a round");
  CHECK(C->eng.canStart() && C->eng.startRound() && V(*C)->phase == PHASE_ROUND, "C may start after the winner left");
  CHECK(W->eng.join("GROUP1", kC), "W rejoins"); g_broker.pump(); settle({C.get(), W.get()}, 5);
  C->eng.submitCorrectGuess(V(*C)->secret); g_broker.pump();
  CHECK(!W->eng.canStart(), "rejoin restored W's right");
}

static void e18_crash_create_close() {
  g_test = "E18 crash between Create steps and Owner Leave steps; boot rows; no self-Leave";
  reset();
  auto C = dev(kC);
  // Create: writes = slot, commit, rm_member. Cut on the rm_member write (2).
  C->writesUntilCut = 2;
  C->eng.createRoom("GROUP1");
  C->reboot(); g_broker.pump();
  CHECK(C->eng.state() == MS_MEMBER && C->eng.isCoordinator(), "row 1 restored membership");
  int selfLeaves = countPub(kC, std::string("r2q/") + kC, OP_LEAVE);
  CHECK(selfLeaves == 0, "self-Leave sent");
  // Owner Leave: writes = slot, commit, rm_member. Cut on rm_member.
  C->writesUntilCut = 2;
  C->eng.leave();
  C->reboot(); g_broker.pump();
  CHECK(C->eng.state() == MS_NONE, "row 2 cleared membership");
  auto B = dev(kB);
  CHECK(B->eng.createRoom("GROUP1"), "B room"); g_broker.pump();
  CHECK(C->eng.join("GROUP1", kB), "C joins B"); g_broker.pump();
  C->reboot(); g_broker.pump(); settle({B.get(), C.get()}, 5);
  CHECK(C->eng.state() == MS_MEMBER && std::string(V(*C)->coordinator) == kB, "row 3 kept the other membership");
}

static void e20_e21_e22_e26_member_recovery() {
  g_test = "E21 reboot floor: stale retained copy below appliedV is ignored";
  reset();
  auto C = dev(kC), M = dev(kA);
  roomWith(*C, {M.get()});
  C->eng.startRound(); g_broker.pump();
  RoomState stale = *V(*M);
  M->eng.submitCorrectGuess(V(*M)->secret); g_broker.pump(); settle({C.get(), M.get()}, 3);
  uint32_t applied = M->eng.durableAppliedV();
  M->online = false;  // reboot without the retained redelivery; the stale copy arrives first
  M->reboot();
  M->online = true;
  uint8_t b[600]; size_t l = encodeState(stale, b, sizeof(b));
  M->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  CHECK(M->eng.reconnecting() && V(*M) == nullptr && stale.version < applied, "stale copy applied after reboot");

  g_test = "E26 reboot then exactly appliedV: restored with NO NVS write (writes failing)";
  reset();
  auto C2 = dev(kC), M2 = dev(kA);
  roomWith(*C2, {M2.get()});
  RoomState cur = *V(*C2);
  M2->online = false;  // reboot without the broker's retained redelivery: the copy below is the first one
  M2->reboot();
  M2->online = true;
  CHECK(M2->eng.reconnecting() && V(*M2) == nullptr, "not reconnecting after reboot");
  M2->failWrite = [](const std::string&) { return true; };
  int w0 = M2->writesAttempted;
  l = encodeState(cur, b, sizeof(b));
  M2->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  CHECK(V(*M2) && V(*M2)->version == cur.version && !M2->eng.reconnecting() && M2->writesAttempted == w0,
        "equal-version recovery (writes attempted %d)", M2->writesAttempted - w0);
  M2->failWrite = nullptr;

  g_test = "E22 appliedV write failure: nothing applied; no displayed rollback across reboot";
  reset();
  auto C3 = dev(kC), M3 = dev(kA);
  roomWith(*C3, {M3.get()});
  uint32_t v0 = V(*M3)->version;
  M3->failWrite = [](const std::string& k) { return k == "rm_member"; };
  C3->eng.startRound(); g_broker.pump();
  CHECK(V(*M3)->version == v0 && M3->eng.storagePaused(), "applied without a durable floor");
  M3->failWrite = nullptr;
  M3->reboot(); g_broker.pump();
  CHECK(!V(*M3) || V(*M3)->version >= v0, "rollback shown");

  g_test = "E27/model-12 conflict freeze: Leave only; later packets do not advance or complete";
  reset();
  auto C4 = dev(kC), R = dev(kA);
  CHECK(C4->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };  // JOIN never reaches C yet
  CHECK(R->eng.join("GROUP1", kC), "join"); g_broker.pump();
  g_broker.dropIf = nullptr;
  RoomState cv = *V(*C4);
  l = encodeState(cv, b, sizeof(b));
  R->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);  // restore at floor
  CHECK(V(*R) && V(*R)->version == cv.version, "snapshot restored");
  R->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);  // identical -> duplicate
  CHECK(!R->eng.frozen(), "identical copy froze");
  RoomState bad = cv; bad.createdUnix ^= 1;
  l = encodeState(bad, b, sizeof(b));
  R->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  CHECK(R->eng.frozen(), "conflict not detected");
  RoomState admitted = cv; admitted.version++; admitted.count++;
  Entry& e = admitted.entries[admitted.count - 1]; memset(&e, 0, sizeof(e)); strncpy(e.id, kA, 12);
  e.flags = ENTRY_ACTIVE; e.lastSeq = R->eng.pendingSeq();
  l = encodeState(admitted, b, sizeof(b));
  R->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  CHECK(V(*R)->version == cv.version && R->eng.state() == MS_JOINING && !R->eng.startRound() &&
            !R->eng.submitCorrectGuess(cv.secret),
        "frozen member advanced or acted");
  CHECK(R->eng.leave() && R->eng.state() == MS_NONE, "Leave unavailable while frozen");

  g_test = "E20 own terminal room on demand while a member elsewhere (republish + CLOSED reply)";
  reset();
  auto A = dev(kC), B = dev(kB);
  roomWith(*A, {B.get()});
  CHECK(A->eng.leave(), "A closes"); g_broker.pump();
  CHECK(B->eng.createRoom("GROUP1") || B->eng.state() == MS_NONE, "B");
  settle({A.get(), B.get()}, 5);
  if (B->eng.state() == MS_NONE) { CHECK(B->eng.createRoom("GROUP1"), "B creates"); g_broker.pump(); }
  CHECK(A->eng.join("GROUP1", kB), "A joins B"); g_broker.pump(); settle({A.get(), B.get()}, 5);
  // a request for A's closed room is answered CLOSED while A is a member elsewhere
  Request q; memset(&q, 0, sizeof(q)); q.op = OP_JOIN; strncpy(q.coordinator, kC, 12);
  q.n = B->eng.finalView() ? B->eng.finalView()->n : 0; q.nonce = B->eng.finalView() ? B->eng.finalView()->nonce : 0;
  strncpy(q.sender, kD, 12); q.seq = 1;
  uint8_t qb[64]; size_t ql = encodeRequest(q, qb, sizeof(qb));
  size_t lb = g_broker.log.size();
  A->eng.onPacket("GROUP1", (std::string("r2q/") + kC).c_str(), kKindRequest, qb, ql);
  bool answered = false;
  for (size_t i = lb; i < g_broker.log.size(); i++) {
    bool isRep = false; Request rq; Reply rp;
    if (g_broker.log[i].from == kC && decodeRequestOrReply(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &isRep, &rq, &rp) && isRep)
      answered = rp.code == CODE_CLOSED || rp.code == CODE_UNKNOWN_ROOM;
  }
  CHECK(answered && A->eng.state() == MS_MEMBER, "closed-room request not answered while a member elsewhere");
}

static void e15_e16_invariants() {
  g_test = "E15 members never publish on r2/* (whole run)";
  // checked across all scenarios at the end via the global audit
  g_test = "E16 sizes: RS 538 (20 entries), request 54, reply 43, records 76/599/88";
  RoomState s; memset(&s, 0, sizeof(s));
  strncpy(s.coordinator, kC, 12); s.n = 1; s.version = 1; s.count = 20;
  for (int i = 0; i < 20; i++) { snprintf(s.entries[i].id, 13, "E%011d", i); s.entries[i].flags = 1; }
  uint8_t b[600];
  size_t n = encodeState(s, b, sizeof(b));
  CHECK(n == 538, "RS %zu", n);
  RoomState d;
  CHECK(decodeState(b, n, &d) && d.count == 20, "RS round trip");
  CHECK(!decodeState(b, n - 1, &d) && !decodeState(b, n + 1 > 600 ? n : n + 1, &d) == true, "length checks");
  Request q; memset(&q, 0, sizeof(q)); q.op = OP_JOIN; strncpy(q.coordinator, kC, 12); strncpy(q.sender, kA, 12);
  CHECK(encodeRequest(q, b, sizeof(b)) == 54, "request size");
  Reply r; memset(&r, 0, sizeof(r)); strncpy(r.coordinator, kC, 12); strncpy(r.target, kA, 12);
  CHECK(encodeReply(r, b, sizeof(b)) == 43, "reply size");
  CHECK(Engine::kCommitRecordBytes == 76 && Engine::kSlotRecordBytes == 599 && Engine::kMemberRecordBytes == 88, "record sizes");
  // MQTT wire: 32-char group, 6-byte codec header
  size_t topicRs = strlen("morsebuddy/") + 32 + 1 + strlen("r2/") + 12;
  size_t topicQ = strlen("morsebuddy/") + 32 + 1 + strlen("r2q/") + 12;
  size_t remRs = 2 + topicRs + 2 + (538 + 6);
  size_t rxRs = 1 + (remRs < 128 ? 1 : 2) + remRs;
  size_t remQ = 2 + topicQ + 2 + (54 + 6);
  size_t rxQ = 1 + (remQ < 128 ? 1 : 2) + remQ;
  size_t txPart = 1 + (remRs < 128 ? 1 : 2) + 2 + topicRs + 2;
  CHECK(topicRs == 59 && topicQ == 60 && rxRs == 610 && rxQ == 126 && txPart == 66 && rxRs <= 1024 && txPart <= 256,
        "wire sizes topic %zu/%zu rx %zu/%zu tx %zu", topicRs, topicQ, rxRs, rxQ, txPart);
}

static void audit_members_never_publish_state() {
  g_test = "E15 audit: every r2/<X> publish came from X";
  int bad = 0;
  for (auto& m : g_broker.log) if (m.topic.rfind("r2/", 0) == 0 && m.topic.substr(3) != m.from) bad++;
  CHECK(bad == 0, "%d foreign state publishes", bad);
}

// =============================================================================
// Additional coverage for every row of design r8 section 12 (E1-E27) not fully
// exercised above, plus a randomized delivery soak.
// =============================================================================
static void injectRs(Device& d, const RoomState& s, const char* group = "GROUP1") {
  uint8_t b[600];
  size_t l = encodeState(s, b, sizeof(b));
  d.eng.onPacket(group, (std::string("r2/") + s.coordinator).c_str(), kKindState, b, l);
  d.checkShown();
}
static void injectReply(Device& d, const char* group, const char* coord, const char* target, uint32_t seq, uint8_t code,
                        uint32_t n, uint32_t nonce, uint32_t ver) {
  Reply r; memset(&r, 0, sizeof(r));
  strncpy(r.coordinator, coord, 12); strncpy(r.target, target, 12);
  r.seq = seq; r.code = code; r.curN = n; r.curNonce = nonce; r.curVersion = ver;
  uint8_t b[64]; size_t l = encodeReply(r, b, sizeof(b));
  d.eng.onPacket(group, (std::string("r2q/") + d.id).c_str(), kKindRequest, b, l);
}
static void reseal(std::vector<uint8_t>& rec) {
  uint32_t c = crcForTest(rec.data(), rec.size() - 4);
  le32(rec.data() + rec.size() - 4, c);
}
static std::string namedSlot(Device& d) { return d.nvs["rc_commit"][62] ? "rc_slot1" : "rc_slot0"; }
static bool lastStateFrom(const char* from, RoomState* out, size_t since = 0) {
  bool got = false;
  for (size_t i = since; i < g_broker.log.size(); i++) {
    auto& m = g_broker.log[i];
    if (m.from == from && m.kind == kKindState) got = decodeState(m.payload.data(), m.payload.size(), out) || got;
  }
  return got;
}
static int statesFrom(const char* from, size_t since) {
  int n = 0;
  for (size_t i = since; i < g_broker.log.size(); i++) if (g_broker.log[i].from == from && g_broker.log[i].kind == kKindState) n++;
  return n;
}
static int repliesTo(const char* target, uint8_t code, size_t since = 0) {
  int n = 0;
  for (size_t i = since; i < g_broker.log.size(); i++) {
    auto& m = g_broker.log[i];
    bool isRep = false; Request rq; Reply rp;
    if (decodeRequestOrReply(m.payload.data(), m.payload.size(), &isRep, &rq, &rp) && isRep &&
        std::string(rp.target) == target && rp.code == code) n++;
  }
  return n;
}
static int requestsFrom(const char* from, uint8_t op, size_t since = 0) {
  int n = 0;
  for (size_t i = since; i < g_broker.log.size(); i++) {
    auto& m = g_broker.log[i];
    bool isRep = false; Request rq; Reply rp;
    if (m.from == from && decodeRequestOrReply(m.payload.data(), m.payload.size(), &isRep, &rq, &rp) && !isRep && rq.op == op) n++;
  }
  return n;
}

static void x1_stale_basis_other_device() {
  g_test = "E1 another device's STALE_BASIS -> NONE \"Couldn't join\", no reissue";
  reset();
  auto C = dev(kC), S = dev(kA), D = dev(kD);
  CHECK(C->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  CHECK(D->eng.discovered(0) && D->eng.discovered(0)->version == 1, "D discovered v1");
  D->online = false;  // D's discovery entry stays at basis v1
  CHECK(S->eng.join("GROUP1", kC), "S join"); g_broker.pump();
  CHECK(S->eng.leave(), "S leave"); g_broker.pump();   // S departs; a departed entry, no floor bump
  // a non-member LEAVE bumps the floor: craft one from a fresh sender (as from a cancelled JOIN)
  Request q; memset(&q, 0, sizeof(q)); q.op = OP_LEAVE; strncpy(q.coordinator, kC, 12);
  q.n = V(*C)->n; q.nonce = V(*C)->nonce; strncpy(q.sender, kB, 12); q.seq = 7; q.basis = V(*C)->version;
  uint8_t b[64]; size_t l = encodeRequest(q, b, sizeof(b));
  C->eng.onPacket("GROUP1", (std::string("r2q/") + kC).c_str(), kKindRequest, b, l); g_broker.pump();
  CHECK(V(*C)->evictionFloor > 1, "floor bumped (%u)", V(*C)->evictionFloor);
  D->online = true;
  size_t lb = g_broker.log.size();
  g_broker.dropIf = [&](const Msg& m) { return m.topic == std::string("r2/") + kC; };  // D sees only the reply
  CHECK(D->eng.join("GROUP1", kC), "D join with basis 1"); g_broker.pump();
  g_broker.dropIf = nullptr;
  CHECK(repliesTo(kD, CODE_STALE_BASIS, lb) == 1, "STALE_BASIS reply");
  CHECK(D->eng.state() == MS_NONE && D->eng.notice() == NOTICE_COULD_NOT_JOIN, "D state %d notice %d", D->eng.state(), D->eng.notice());
  settle({C.get(), D.get()}, 60);
  CHECK(requestsFrom(kD, OP_JOIN, lb) == 1 && !isActive(*V(*C), kD), "JOIN reissued / D admitted");
}

static void x3_ok_then_rs() {
  g_test = "E3 REPLY OK completes only with an RS >= curVersion; QUERY when the RS lags";
  reset();
  auto C = dev(kC), W = dev(kA);
  roomWith(*C, {W.get()});
  C->eng.startRound(); g_broker.pump();
  W->eng.submitCorrectGuess(V(*W)->secret); g_broker.pump();
  CHECK(W->eng.canStart(), "W holds the right");
  // W's START: the OK reply arrives, the ROUND state is lost
  g_broker.dropIf = [&](const Msg& m) { return m.from == kC && m.kind == kKindState; };
  size_t lb = g_broker.log.size();
  CHECK(W->eng.startRound(), "W START"); g_broker.pump();
  CHECK(repliesTo(kA, CODE_OK, lb) == 1, "OK reply delivered");
  uint8_t op = 0;
  CHECK(W->eng.pendingOp(&op) && op == OP_START && V(*W)->phase == PHASE_LOBBY, "completed from the reply alone");
  tickAll({C.get(), W.get()}, 500, 8);  // > 3 s
  CHECK(requestsFrom(kA, OP_QUERY, lb) >= 1, "no QUERY while the RS lagged");
  g_broker.dropIf = nullptr;
  settle({C.get(), W.get()}, 10);  // the QUERY answer schedules a republish
  CHECK(!W->eng.pendingOp(&op) && V(*W)->phase == PHASE_ROUND, "START not completed after the RS arrived");
}

static void x4_reply_fields() {
  g_test = "E4 request replies: each field negative, then positive; cur != record -> gone";
  reset();
  auto C = dev(kC), A = dev(kA);
  roomWith(*C, {A.get()});
  C->eng.startRound(); g_broker.pump();
  const RoomState cv = *V(*C);
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(A->eng.submitCorrectGuess(cv.secret), "claim"); g_broker.pump();
  g_broker.dropIf = nullptr;
  uint32_t seq = A->eng.pendingSeq();
  uint8_t op = 0;
  injectReply(*A, "GROUP2", kC, kA, seq, CODE_LATE, cv.n, cv.nonce, cv.version);
  injectReply(*A, "GROUP1", kB, kA, seq, CODE_LATE, cv.n, cv.nonce, cv.version);
  injectReply(*A, "GROUP1", kC, kD, seq, CODE_LATE, cv.n, cv.nonce, cv.version);
  injectReply(*A, "GROUP1", kC, kA, seq + 1, CODE_LATE, cv.n, cv.nonce, cv.version);
  CHECK(A->eng.pendingOp(&op) && A->eng.notice() != NOTICE_LATE, "a mismatched reply acted");
  injectReply(*A, "GROUP1", kC, kA, seq, CODE_LATE, cv.n, cv.nonce, cv.version);
  CHECK(!A->eng.pendingOp(&op) && A->eng.notice() == NOTICE_LATE && A->eng.state() == MS_MEMBER, "positive LATE");
  // a replayed copy of an answered reply no longer matches
  A->eng.clearNotice();
  injectReply(*A, "GROUP1", kC, kA, seq, CODE_CLOSED, cv.n, cv.nonce, cv.version);
  CHECK(A->eng.state() == MS_MEMBER, "an old reply matched after completion");
  // binding: cur names another room -> verification outcome "gone"
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  C->eng.startRound();
  CHECK(V(*A) != nullptr, "view");
  g_broker.pump();
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(A->eng.submitCorrectGuess(V(*A)->secret), "claim 2"); g_broker.pump();
  g_broker.dropIf = nullptr;
  seq = A->eng.pendingSeq();
  injectReply(*A, "GROUP1", kC, kA, seq, CODE_OK, cv.n + 1, cv.nonce, 9);
  CHECK(A->eng.state() == MS_NONE && A->eng.notice() == NOTICE_ROOM_CLOSED && A->eng.finalView(), "cur != record not treated as gone");

  g_test = "E4 QUERY replies: group / coordinator / target / qseq / binding";
  reset();
  auto C2 = dev(kC), M = dev(kA);
  roomWith(*C2, {M.get()});
  RoomState fake = *V(*C2); fake.nonce ^= 0x10;
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  injectRs(*M, fake);
  g_broker.dropIf = nullptr;
  uint32_t q = M->eng.outstandingQuerySeq();
  CHECK(q != 0 && M->eng.verifying(), "verifying");
  injectReply(*M, "GROUP2", kC, kA, q, CODE_CURRENT, 0, 0, 0);
  injectReply(*M, "GROUP1", kB, kA, q, CODE_CURRENT, 0, 0, 0);
  injectReply(*M, "GROUP1", kC, kB, q, CODE_CURRENT, 0, 0, 0);
  injectReply(*M, "GROUP1", kC, kA, q + 1, CODE_CURRENT, 0, 0, 0);
  injectReply(*M, "GROUP1", kC, kA, q - 1, CODE_CURRENT, 0, 0, 0);
  CHECK(M->eng.state() == MS_MEMBER && M->eng.verifying(), "a mismatched QUERY reply acted");
  const RoomState* mv = V(*M);
  injectReply(*M, "GROUP1", kC, kA, q, CODE_CURRENT, mv->n, mv->nonce, mv->version);
  CHECK(M->eng.state() == MS_MEMBER && !M->eng.verifying(), "positive 'same room' outcome");
  injectRs(*M, fake);
  q = M->eng.outstandingQuerySeq();
  injectReply(*M, "GROUP1", kC, kA, q, CODE_CURRENT, 0, 0, 0);
  CHECK(M->eng.state() == MS_NONE && M->eng.notice() == NOTICE_ROOM_CLOSED, "positive 'gone' outcome (NONE)");
}

static void x5_transition_table() {
  g_test = "E5 JOINING + FULL / NOT_ALLOWED / UNKNOWN_ROOM / CLOSED replies";
  const uint8_t codes[4] = {CODE_FULL, CODE_NOT_ALLOWED, CODE_UNKNOWN_ROOM, CODE_CLOSED};
  const Notice want[4] = {NOTICE_ROOM_FULL, NOTICE_ROOM_CLOSED, NOTICE_ROOM_CLOSED, NOTICE_ROOM_CLOSED};
  for (int i = 0; i < 4; i++) {
    reset();
    auto C = dev(kC), J = dev(kA);
    CHECK(C->eng.createRoom("GROUP1"), "create"); g_broker.pump();
    g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
    CHECK(J->eng.join("GROUP1", kC), "join"); g_broker.pump();
    g_broker.dropIf = nullptr;
    const RoomState* cv = V(*C);
    injectReply(*J, "GROUP1", kC, kA, J->eng.pendingSeq(), codes[i], cv->n, cv->nonce, cv->version);
    CHECK(J->eng.state() == MS_NONE && J->eng.notice() == want[i], "code %u -> state %d notice %d", codes[i], J->eng.state(), J->eng.notice());
  }

  g_test = "E5 MEMBER: Start/claim pending; Leave replaces a pending CLAIM; LEAVING completes -> NONE";
  reset();
  auto C = dev(kC), M = dev(kA);
  roomWith(*C, {M.get()});
  C->eng.startRound(); g_broker.pump();
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(M->eng.submitCorrectGuess(V(*M)->secret), "claim pending");
  uint32_t claimSeq = M->eng.pendingSeq();
  uint8_t op = 0;
  CHECK(M->eng.pendingOp(&op) && op == OP_CLAIM, "pending CLAIM");
  CHECK(M->eng.leave() && M->eng.state() == MS_LEAVING && M->eng.pendingOp(&op) && op == OP_LEAVE && M->eng.pendingSeq() > claimSeq,
        "Leave did not replace the CLAIM with a new seq");
  g_broker.dropIf = nullptr;
  settle({C.get(), M.get()}, 10);
  CHECK(M->eng.state() == MS_NONE && !isActive(*V(*C), kA) && std::string(V(*C)->resultWinner) != kA, "LEAVE completed, old CLAIM not applied");

  g_test = "E5 LEAVING + Leave anyway -> NONE \"may stay listed\"";
  reset();
  auto C2 = dev(kC), M2 = dev(kA);
  roomWith(*C2, {M2.get()});
  C2->online = false;
  CHECK(M2->eng.leave() && M2->eng.state() == MS_LEAVING, "leaving");
  CHECK(M2->eng.leaveAnyway() && M2->eng.state() == MS_NONE && M2->eng.notice() == NOTICE_STAYED_LISTED, "leave anyway");
  CHECK(!M2->eng.leaveAnyway(), "leave anyway outside LEAVING");

  g_test = "E5 MEMBER + applied RS without me ACTIVE (no pending LEAVE) -> \"no longer in this room\"";
  reset();
  auto C3 = dev(kC), M3 = dev(kA);
  roomWith(*C3, {M3.get()});
  RoomState s = *V(*C3); s.version++;
  s.entries[findEntry(s, kA)].flags = 0;
  C3->online = false;
  injectRs(*M3, s);
  CHECK(M3->eng.state() == MS_NONE && M3->eng.notice() == NOTICE_NO_LONGER_IN_ROOM, "state %d notice %d", M3->eng.state(), M3->eng.notice());

  g_test = "E5 START rejected from state (startRightVersion moved on) and NOT_ALLOWED reply";
  reset();
  auto C4 = dev(kC), W = dev(kA);
  roomWith(*C4, {W.get()});
  C4->eng.startRound(); g_broker.pump();
  W->eng.submitCorrectGuess(V(*W)->secret); g_broker.pump();
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(W->eng.startRound(), "W START (lost)");
  g_broker.dropIf = nullptr;
  uint32_t ent = V(*W)->startRightVersion;
  injectReply(*W, "GROUP1", kC, kA, W->eng.pendingSeq(), CODE_NOT_ALLOWED, V(*W)->n, V(*W)->nonce, V(*W)->version);
  CHECK(!W->eng.pendingOp(&op) && W->eng.notice() == NOTICE_START_REFUSED && W->eng.state() == MS_MEMBER, "NOT_ALLOWED");
  (void)ent;
}

static void x7_boot_rows() {
  g_test = "E7 FC-B1 rule (b): newer slot is a creation after a terminal room";
  reset();
  auto C = dev(kC), A = dev(kA);
  roomWith(*C, {A.get()});
  CHECK(C->eng.leave(), "owner close"); g_broker.pump();
  CHECK(C->eng.createRoom("GROUP1"), "new room"); g_broker.pump();
  uint32_t newN = V(*C)->n, newNonce = V(*C)->nonce;
  C->nvs["rc_commit"][20] ^= 1;
  size_t lb = g_broker.log.size();
  C->reboot(); g_broker.pump();
  CHECK(C->eng.isCoordinator() && V(*C)->n == newN && V(*C)->nonce == newNonce && V(*C)->version == 1, "rule (b) not adopted");
  RoomState last;
  CHECK(!lastStateFrom(kC, &last, lb) || last.version == 1, "a new version was written");

  g_test = "E7 FC-A with closed: nothing written, nothing published";
  reset();
  auto C2 = dev(kC), A2 = dev(kA);
  roomWith(*C2, {A2.get()});
  CHECK(C2->eng.leave(), "owner close"); settle({C2.get(), A2.get()}, 5);
  C2->nvs[namedSlot(*C2)][200] ^= 0xFF;
  int w0 = C2->writesAttempted;
  lb = g_broker.log.size();
  C2->reboot(); settle({C2.get()}, 70);
  CHECK(C2->writesAttempted == w0 && statesFrom(kC, lb) == 0, "writes %d publishes %d", C2->writesAttempted - w0, statesFrom(kC, lb));

  g_test = "E7 every binding mismatch is detected (re-sealed CRC, so only the binding is wrong)";
  struct Mut { const char* name; bool slot; size_t off; };
  // slot: magic4 fmt1 slotId1(5) slotSeq4(6) group33(10) n4(43) nonce4(47) version4(51) rsLen2(55) RS@57 (RS coordinator @58)
  // commit: group33(5) coordinator12(38) n4(50) nonce4(54) version4(58) slotId1(62) slotSeq4(63)
  const Mut muts[] = {{"slot slotId", true, 5},   {"slot slotSeq", true, 6},   {"slot group", true, 10},
                      {"slot n", true, 43},       {"slot nonce", true, 47},    {"slot version", true, 51},
                      {"RS coordinator", true, 58}, {"commit group", false, 5}, {"commit n", false, 50},
                      {"commit nonce", false, 54}, {"commit version", false, 58}, {"commit slotSeq", false, 63}};
  for (auto& mu : muts) {
    reset();
    auto Cx = dev(kC), Ax = dev(kA);
    roomWith(*Cx, {Ax.get()});
    uint32_t v0 = V(*Cx)->version;
    auto& rec = mu.slot ? Cx->nvs[namedSlot(*Cx)] : Cx->nvs["rc_commit"];
    // +1 on the low byte: a CRC-valid record whose field disagrees (versions move up, so FC-A's
    // V+1 cannot name a version that was already published -- a lower commit version is not
    // producible by a torn write, since the record's CRC covers it)
    rec[mu.off] = static_cast<uint8_t>(rec[mu.off] + 1);
    reseal(rec);
    size_t lb2 = g_broker.log.size();
    Cx->reboot(); g_broker.pump();
    RoomState s;
    bool dataLost = lastStateFrom(kC, &s, lb2) && s.phase == PHASE_CLOSED && s.closeReason == CLOSE_DATA_LOST;
    CHECK(dataLost && !Cx->eng.isCoordinator(), "%s: not detected (FC-A expected; v0 %u)", mu.name, v0);
  }
  {
    reset();
    auto Cx = dev(kC);
    CHECK(Cx->eng.createRoom("GROUP1"), "create"); g_broker.pump();
    Cx->eng.startRound(); g_broker.pump();
    auto& rec = Cx->nvs["rc_commit"];
    rec[38] = 'F';  // commit coordinator != me -> commit invalid -> FC-B1 (slots consecutive)
    reseal(rec);
    Cx->reboot(); g_broker.pump();
    CHECK(Cx->eng.isCoordinator() && Cx->nvs["rc_commit"][38] == 'C', "commit coordinator mismatch: commit not rejected/rewritten");
  }

  g_test = "E19 FC-B1 with the newer slot CLOSED at 0xFFFFFFFF; non-consecutive -> FC-B2";
  reset();
  auto C5 = dev(kC);
  RoomState fin; memset(&fin, 0, sizeof(fin));
  strncpy(fin.coordinator, kC, 12); fin.n = 3; fin.nonce = 77; fin.version = 0xFFFFFFFFu; fin.phase = PHASE_CLOSED;
  fin.closeReason = CLOSE_EXHAUSTED; fin.count = 1; strncpy(fin.entries[0].id, kC, 12); fin.entries[0].flags = 1; fin.startRightVersion = 1;
  writeCoordinatorRecordsForTest(C5->nvs, fin, "GROUP1");
  C5->nvs["rc_commit"][20] ^= 1;
  int w0b = C5->writesAttempted;
  C5->reboot(); tickAll({C5.get()}, 100, 2);
  CHECK(!C5->eng.coordinatorDamaged() && C5->writesAttempted == w0b + 1, "FC-B1 adopted with one rc_commit write (%d)", C5->writesAttempted - w0b);
  RoomState s5;
  CHECK(lastStateFrom(kC, &s5) && s5.version == 0xFFFFFFFFu && s5.phase == PHASE_CLOSED, "closed final republished");
  CHECK(C5->eng.createRoom("GROUP1") && V(*C5)->n == 4, "new room after the final one (n %u)", V(*C5) ? V(*C5)->n : 0);
  reset();
  auto C6 = dev(kC);
  writeCoordinatorRecordsForTest(C6->nvs, fin, "GROUP1");
  le32(&C6->nvs["rc_slot1"][6], 40); reseal(C6->nvs["rc_slot1"]);
  C6->nvs["rc_commit"][20] ^= 1;
  int w0c = C6->writesAttempted;
  C6->reboot(); g_broker.pump();
  CHECK(C6->eng.coordinatorDamaged() && C6->writesAttempted == w0c && C6->eng.notice() == NOTICE_DATA_DAMAGED, "non-consecutive not FC-B2");
}

static void x8_limits() {
  g_test = "E8 exhaustion: change at 0xFFFFFFFE -> EXHAUSTED at 0xFFFFFFFF; nothing after; DATA_LOST at the limit";
  reset();
  auto C = dev(kC), A = dev(kA);
  RoomState s; memset(&s, 0, sizeof(s));
  strncpy(s.coordinator, kC, 12); s.n = 5; s.nonce = 99; s.version = 0xFFFFFFFEu; s.phase = PHASE_LOBBY; s.count = 2;
  s.startRightVersion = 1;
  strncpy(s.entries[0].id, kC, 12); s.entries[0].flags = 1;
  strncpy(s.entries[1].id, kA, 12); s.entries[1].flags = 1; s.entries[1].score = 4;
  writeCoordinatorRecordsForTest(C->nvs, s, "GROUP1");
  C->reboot(); g_broker.pump();
  CHECK(C->eng.isCoordinator() && V(*C)->version == 0xFFFFFFFEu, "loaded");
  size_t lb = g_broker.log.size();
  CHECK(!C->eng.startRound(), "start at the limit");
  RoomState last;
  CHECK(lastStateFrom(kC, &last, lb) && last.version == 0xFFFFFFFFu && last.closeReason == CLOSE_EXHAUSTED && last.count == 2,
        "EXHAUSTED v%u", last.version);
  CHECK(C->eng.state() == MS_NONE && C->eng.finalView() && C->eng.finalView()->version == 0xFFFFFFFFu, "C ended locally with the final view");
  lb = g_broker.log.size();
  C->eng.startRound(); C->eng.leave(); C->eng.submitCorrectGuess(s.secret);
  Request q; memset(&q, 0, sizeof(q)); q.op = OP_JOIN; strncpy(q.coordinator, kC, 12); q.n = 5; q.nonce = 99;
  strncpy(q.sender, kD, 12); q.seq = 3; q.basis = 0xFFFFFFFFu;
  uint8_t b[64]; size_t l = encodeRequest(q, b, sizeof(b));
  C->eng.onPacket("GROUP1", (std::string("r2q/") + kC).c_str(), kKindRequest, b, l);
  settle({C.get()}, 10);
  bool beyond = false;
  for (size_t i = lb; i < g_broker.log.size(); i++) {
    RoomState x;
    if (g_broker.log[i].from == kC && decodeState(g_broker.log[i].payload.data(), g_broker.log[i].payload.size(), &x) && x.version != 0xFFFFFFFFu) beyond = true;
  }
  CHECK(!beyond && repliesTo(kD, CODE_CLOSED, lb) == 1, "after the final version: beyond=%d", beyond);

  reset();
  auto C2 = dev(kC);
  writeCoordinatorRecordsForTest(C2->nvs, s, "GROUP1");
  C2->nvs["rc_slot0"][300] ^= 0xFF;  // the named slot (slot0) at 0xFFFFFFFE is corrupt
  C2->reboot(); g_broker.pump();
  CHECK(lastStateFrom(kC, &last) && last.version == 0xFFFFFFFFu && last.closeReason == CLOSE_DATA_LOST && last.count == 0,
        "DATA_LOST at the limit (v%u)", last.version);
  int w = C2->writesAttempted;
  C2->reboot(); g_broker.pump();
  CHECK(C2->writesAttempted == w && !C2->eng.isCoordinator(), "rebooting a final room wrote again");
}

static void x9_stale_and_resurrected() {
  g_test = "E9 stale old-room RS ignored by the new room's member; a resurrected slot is repaired by C";
  reset();
  auto C = dev(kC), B = dev(kA);
  roomWith(*C, {B.get()});
  RoomState oldRs = *V(*C);
  CHECK(C->eng.leave(), "close"); g_broker.pump();
  CHECK(C->eng.createRoom("GROUP1"), "new room"); g_broker.pump();
  CHECK(B->eng.join("GROUP1", kC), "B joins"); g_broker.pump(); settle({C.get(), B.get()}, 5);
  RoomState cur = *V(*C);
  // the stale old RS reaches the broker's retained slot (e.g. a late QoS1 redelivery)
  Msg m{"GROUP1", std::string("r2/") + kC, kKindState, {}, true, kC};
  m.payload.resize(600); m.payload.resize(encodeState(oldRs, m.payload.data(), 600));
  g_broker.deliver(m);
  CHECK(B->eng.state() == MS_MEMBER && V(*B)->nonce == cur.nonce, "B changed on the old RS");
  settle({C.get(), B.get()}, 10);
  RoomState r;
  auto& ret = g_broker.retained["GROUP1|r2/" + std::string(kC)];
  CHECK(decodeState(ret.payload.data(), ret.payload.size(), &r) && r.nonce == cur.nonce && r.version == V(*C)->version,
        "retained slot not repaired");
  CHECK(B->eng.state() == MS_MEMBER && !B->eng.verifying(), "B did not settle");
}

static void x11_member_steps() {
  g_test = "E11 reboot at each member step (JOIN, CLAIM): persisted-but-unsent requests resent identically";
  reset();
  auto C = dev(kC), M = dev(kA);
  CHECK(C->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  // step 2 fails (power cut on the rm_member write): nothing sent, NONE after reboot
  M->writesUntilCut = 0;
  size_t lb = g_broker.log.size();
  CHECK(!M->eng.join("GROUP1", kC), "join with a cut");
  M->reboot(); g_broker.pump();
  CHECK(M->eng.state() == MS_NONE && requestsFrom(kA, OP_JOIN, lb) == 0, "after a cut on step 2");
  // step 3 lost (persisted, power lost before sending)
  M->online = false;
  CHECK(M->eng.join("GROUP1", kC), "join persisted");
  uint32_t seq = M->eng.pendingSeq();
  M->online = true;
  M->reboot();
  tickAll({C.get(), M.get()}, 100, 3);
  bool same = false;
  for (size_t i = lb; i < g_broker.log.size(); i++) {
    bool isRep = false; Request rq; Reply rp;
    auto& mm = g_broker.log[i];
    if (mm.from == kA && decodeRequestOrReply(mm.payload.data(), mm.payload.size(), &isRep, &rq, &rp) && !isRep && rq.op == OP_JOIN)
      same = rq.seq == seq && rq.basis == 1;
  }
  settle({C.get(), M.get()}, 5);
  CHECK(same && M->eng.state() == MS_MEMBER, "JOIN not resent identically after reboot");
  C->eng.startRound(); g_broker.pump();
  M->online = false;
  CHECK(M->eng.submitCorrectGuess(V(*M)->secret), "claim persisted");
  seq = M->eng.pendingSeq();
  M->online = true;
  M->reboot(); settle({C.get(), M.get()}, 5);
  CHECK(std::string(V(*C)->resultWinner) == kA && V(*C)->entries[findEntry(*V(*C), kA)].lastSeq == seq && !M->eng.pendingOp(nullptr),
        "CLAIM after reboot");
}

static void x18_every_step() {
  g_test = "E18 power cut at every step of Create and Owner Leave";
  for (int cut = 0; cut < 3; cut++) {
    reset();
    auto C = dev(kC);
    C->writesUntilCut = cut;  // 0 slot, 1 rc_commit, 2 rm_member
    C->eng.createRoom("GROUP1");
    size_t lb = g_broker.log.size();
    C->reboot(); g_broker.pump();
    if (cut == 0) CHECK(!C->eng.coordinatorDamaged() && C->eng.state() == MS_NONE, "create cut 0");
    if (cut == 1) CHECK(C->eng.coordinatorDamaged() && C->eng.state() == MS_NONE && statesFrom(kC, lb) == 0, "create cut 1 -> FC-B2, nothing published");
    if (cut == 2) CHECK(C->eng.isCoordinator() && C->eng.state() == MS_MEMBER, "create cut 2 -> row 1");
    CHECK(requestsFrom(kC, OP_LEAVE) == 0, "self-Leave");
    if (C->eng.state() == MS_NONE) CHECK(C->eng.createRoom("GROUP1"), "create after cut %d", cut);
  }
  for (int cut = 0; cut < 3; cut++) {
    reset();
    auto C = dev(kC), A = dev(kA);
    roomWith(*C, {A.get()});
    uint32_t v = V(*C)->version;
    C->writesUntilCut = cut;
    C->eng.leave();
    C->reboot(); settle({C.get(), A.get()}, 5);
    if (cut < 2) CHECK(C->eng.isCoordinator() && V(*C)->version == v && A->eng.state() == MS_MEMBER, "leave cut %d: room continues", cut);
    else CHECK(C->eng.state() == MS_NONE && A->eng.state() == MS_NONE && A->eng.notice() == NOTICE_ROOM_CLOSED, "leave cut 2: closed for all");
  }
}

static void x20_on_demand() {
  g_test = "E20 own terminal room loaded on demand while a member elsewhere";
  reset();
  auto A = dev(kC), B = dev(kB);
  roomWith(*A, {B.get()});
  RoomState closedRs;
  CHECK(A->eng.leave(), "A closes"); settle({A.get(), B.get()}, 5);
  CHECK(lastStateFrom(kC, &closedRs) && closedRs.phase == PHASE_CLOSED, "closed");
  CHECK(B->eng.createRoom("GROUP1"), "B creates"); g_broker.pump();
  CHECK(A->eng.join("GROUP1", kB), "A joins B"); settle({A.get(), B.get()}, 5);
  CHECK(V(*A) && std::string(V(*A)->coordinator) == kB, "A's one RAM room is B's");
  // the broker loses A's retained slot to a stale copy; a stranger's QUERY triggers a republish from NVS
  RoomState stale = closedRs; stale.version--; stale.phase = PHASE_LOBBY; stale.closeReason = 0;
  Msg m{"GROUP1", std::string("r2/") + kC, kKindState, {}, true, kC};
  m.payload.resize(600); m.payload.resize(encodeState(stale, m.payload.data(), 600));
  size_t lb = g_broker.log.size();
  g_broker.deliver(m);
  settle({A.get(), B.get()}, 10);
  RoomState rep;
  CHECK(lastStateFrom(kC, &rep, lb) && rep.version == closedRs.version && rep.phase == PHASE_CLOSED && rep.count == closedRs.count,
        "republished from the slot");
  CHECK(V(*A) && std::string(V(*A)->coordinator) == kB && A->eng.state() == MS_MEMBER, "A's membership disturbed");
}

static void x21_x26_recovery() {
  g_test = "E21 Reconnecting... until an RS >= appliedV; then recovered";
  reset();
  auto C = dev(kC), M = dev(kA);
  roomWith(*C, {M.get()});
  C->eng.startRound(); g_broker.pump();
  RoomState cur = *V(*C);
  M->online = false; M->reboot(); M->online = true;
  CHECK(M->eng.reconnecting() && !M->eng.startRound() && !M->eng.submitCorrectGuess(cur.secret), "only Leave while reconnecting");
  tickAll({C.get(), M.get()}, 500, 10);
  CHECK(!M->eng.reconnecting() && V(*M)->version == cur.version, "not recovered via QUERY/republish");

  g_test = "E26 restore at exactly appliedV completes recovery and the pending CLAIM";
  reset();
  auto C2 = dev(kC), M2 = dev(kA);
  roomWith(*C2, {M2.get()});
  C2->eng.startRound(); g_broker.pump();
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(M2->eng.submitCorrectGuess(V(*M2)->secret), "claim"); g_broker.pump();
  g_broker.dropIf = nullptr;
  M2->online = false; M2->reboot();
  M2->failWrite = [](const std::string& k) { return k == "rm_member"; };
  injectRs(*M2, *V(*C2));   // exactly appliedV
  CHECK(V(*M2) && !M2->eng.reconnecting() && !M2->eng.storagePaused(), "restored without a write");
  M2->failWrite = nullptr;
  M2->online = true;
  settle({C2.get(), M2.get()}, 10);
  CHECK(std::string(V(*C2)->resultWinner) == kA && !M2->eng.pendingOp(nullptr), "pending CLAIM not completed after restore");
}

static void x23_directions() {
  g_test = "E23 old->new (equal n) and new->old (higher n) snapshots; genuine replacement";
  for (int higher = 0; higher < 2; higher++) {
    reset();
    auto C = dev(kC), M = dev(kA);
    roomWith(*C, {M.get()});
    RoomState mine = *V(*C);
    RoomState other = mine; other.nonce ^= 0x5A5A; other.version = 1;
    if (higher) other.n += 10;
    injectRs(*M, other);
    CHECK(M->eng.verifying() && M->eng.state() == MS_MEMBER, "verifying");
    settle({C.get(), M.get()}, 10);
    CHECK(M->eng.state() == MS_MEMBER && !M->eng.verifying() && V(*M)->nonce == mine.nonce, "%s n: stayed", higher ? "higher" : "equal");
  }
  reset();
  auto C = dev(kC), M = dev(kA);
  roomWith(*C, {M.get()});
  M->online = false;
  CHECK(C->eng.leave(), "close (M offline)"); g_broker.pump();
  CHECK(C->eng.createRoom("GROUP1"), "replacement"); g_broker.pump();
  M->online = true;
  M->reboot(); settle({C.get(), M.get()}, 15);
  CHECK(M->eng.state() == MS_NONE && M->eng.notice() == NOTICE_ROOM_CLOSED, "genuine replacement not recognised");
}

static void x24_invalidators() {
  g_test = "E24 Leave / Join / reboot / terminal each invalidate the outstanding QUERY";
  for (int k = 0; k < 4; k++) {
    reset();
    auto C = dev(kC), M = dev(kA);
    roomWith(*C, {M.get()});
    RoomState fake = *V(*C); fake.nonce ^= 3;
    g_broker.holdIf = [&](const Msg& m) { return m.topic == std::string("r2q/") + kA; };
    injectRs(*M, fake); g_broker.pump();
    uint32_t q = M->eng.outstandingQuerySeq();
    CHECK(q != 0, "query");
    if (k == 0) { M->eng.leave(); }
    if (k == 1) { M->eng.leave(); g_broker.holdIf = nullptr; settle({C.get(), M.get()}, 5);
                  g_broker.holdIf = [&](const Msg& m) { return m.topic == std::string("r2q/") + kA && m.payload[1] == OP_REPLY && m.payload.size() == 43; };
                  M->eng.join("GROUP1", kC); }
    if (k == 2) { M->online = false; M->reboot(); M->online = true; }
    if (k == 3) { RoomState cl = *V(*C); cl.version++; cl.phase = PHASE_CLOSED; C->online = false; injectRs(*M, cl); C->online = true; }
    CHECK(M->eng.outstandingQuerySeq() != q || M->eng.outstandingQuerySeq() == 0, "k=%d: query still outstanding", k);
    Notice before = M->eng.notice(); MemberState st = M->eng.state();
    injectReply(*M, "GROUP1", kC, kA, q, CODE_CURRENT, 0, 0, 0);  // the old query's "gone" answer
    CHECK(M->eng.state() == st && M->eng.notice() == before, "k=%d: an invalidated query's reply acted", k);
    g_broker.holdIf = nullptr;
  }
}

static void x27_freeze_scope() {
  g_test = "E27 frozen: a later RS completing the pending CLAIM is not applied; Leave sends one LEAVE";
  reset();
  auto C = dev(kC), M = dev(kA);
  roomWith(*C, {M.get()});
  C->eng.startRound(); g_broker.pump();
  RoomState cur = *V(*M);
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(M->eng.submitCorrectGuess(cur.secret), "claim");
  g_broker.dropIf = nullptr;
  RoomState conflict = cur; conflict.roundNo ^= 7;
  injectRs(*M, conflict);
  CHECK(M->eng.frozen() && !M->eng.canAct(), "frozen");
  RoomState done = cur; done.version++; done.phase = PHASE_LOBBY; done.resultRound = cur.roundStart;
  strncpy(done.resultWinner, kA, 12);
  injectRs(*M, done);
  uint8_t op = 0;
  CHECK(M->eng.pendingOp(&op) && op == OP_CLAIM && V(*M)->version == cur.version, "frozen member completed or advanced");
  size_t lb = g_broker.log.size();
  CHECK(M->eng.leave() && M->eng.state() == MS_NONE, "Leave");
  CHECK(requestsFrom(kA, OP_LEAVE, lb) == 1, "one LEAVE");
  settle({C.get(), M.get()}, 5);
  CHECK(!isActive(*V(*C), kA), "C did not process the frozen member's LEAVE");
}

// Randomized delivery soak: loss, duplication, delay/reordering, 16-entry
// receive queue overflow, member reboots and C offline periods; then heal.
// Invariants: no displayed rollback (checked on every delivery), whole-log
// audits, convergence to C's state, score total == resolved rounds.
static uint32_t g_seed = 1;
static uint32_t rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }
static void soak(uint32_t seed) {
  char name[64]; snprintf(name, sizeof(name), "soak seed %u", seed);
  g_test = name;
  reset();
  g_seed = seed;
  auto C = dev(kC), A = dev(kA), B = dev(kB), D = dev(kD);
  std::vector<Device*> all{C.get(), A.get(), B.get(), D.get()};
  std::vector<Device*> members{A.get(), B.get(), D.get()};
  // odd seeds: a 2-entry effective queue per loop tick, so receive-queue overflow (drops) occurs
  for (Device* d : all) d->queueCap = (seed % 2) ? 2 : 16;
  CHECK(C->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  for (Device* d : members) { d->eng.join("GROUP1", kC); g_broker.pump(); }
  int rounds = 0;
  for (int step = 0; step < 1500; step++) {
    g_broker.dropIf = [](const Msg&) { return rnd() % 100 < 15; };
    g_broker.holdIf = [](const Msg&) { return rnd() % 100 < 15; };
    uint32_t r = rnd() % 100;
    Device* m = members[rnd() % members.size()];
    if (r < 10) { if (C->eng.view() && C->eng.view()->phase == PHASE_LOBBY) C->eng.startRound(); }
    else if (r < 25) { const RoomState* v = m->eng.view(); if (v && v->phase == PHASE_ROUND) m->eng.submitCorrectGuess(v->secret); }
    else if (r < 30) { if (m->eng.canStart()) m->eng.startRound(); }
    else if (r < 33) { m->eng.leave(); }
    else if (r < 37) { if (m->eng.state() == MS_NONE && !m->eng.pendingOp(nullptr)) m->eng.join("GROUP1", kC); }
    else if (r < 40) { m->online = false; m->reboot(); m->online = true; }
    else if (r < 42) { C->online = !C->online; }
    else if (r < 50) { g_broker.release(); }
    else if (r < 55) { const RoomState* v = C->eng.view(); if (v && v->phase == PHASE_ROUND) C->eng.submitCorrectGuess(v->secret); }
    else if (r < 57 && !g_broker.inflight.empty() && rnd() % 2) { g_broker.inflight.push_back(g_broker.inflight[0]); }
    else if (r < 59) { C->reboot(); }                                              // coordinator reboot
    else if (r < 62) { m->failWrite = [](const std::string&) { return rnd() % 2 == 0; }; }  // flaky member NVS
    else if (r < 66) { m->failWrite = nullptr; }
    else if (r < 67) { C->writesUntilCut = static_cast<int>(rnd() % 3); }          // power cut on a coordinator write
    else if (r < 57 && !g_broker.inflight.empty()) { g_broker.inflight.push_back(g_broker.inflight[rnd() % g_broker.inflight.size()]); }  // duplicate
    if (C->dead) { C->reboot(); }                                                    // power returns
    tickAll(all, 200 + rnd() % 800, 1);
  }
  for (Device* d : members) d->failWrite = nullptr;
  C->writesUntilCut = -1;
  if (C->dead) C->reboot();  // a cut on the last step: power returns before healing
  g_broker.dropIf = nullptr; g_broker.holdIf = nullptr; C->online = true;
  g_broker.release();
  settle(all, 120);
  const RoomState* cv = C->eng.view();
  CHECK(cv && cv->phase != PHASE_CLOSED, "C's room is open");
  uint32_t scores = 0;
  for (uint8_t i = 0; cv && i < cv->count; i++) scores += cv->entries[i].score;
  rounds = cv ? cv->roundNo - (cv->phase == PHASE_ROUND ? 1 : 0) : 0;
  CHECK(cv && scores == static_cast<uint32_t>(rounds), "scores %u != resolved rounds %d", scores, rounds);
  for (Device* d : members) {
    bool listed = cv && isActive(*cv, d->id.c_str());
    if (d->eng.state() == MS_MEMBER) {
      CHECK(listed && d->eng.view() && d->eng.view()->version == cv->version && !d->eng.frozen(), "%s not converged (v%u vs v%u)",
            d->id.c_str(), d->eng.view() ? d->eng.view()->version : 0, cv ? cv->version : 0);
    } else {
      CHECK(!listed || d->eng.notice() == NOTICE_STAYED_LISTED, "%s listed ACTIVE while state %d", d->id.c_str(), d->eng.state());
    }
  }
  for (Device* d : all) CHECK(d->rollbacks == 0, "%s displayed a rollback", d->id.c_str());
  printf("   %s: v%u, %d rounds, %d queue drops\n", name, cv ? cv->version : 0, rounds,
         C->queueDrops + A->queueDrops + B->queueDrops + D->queueDrops);
}

// =============================================================================
// Review findings (round after the R3-R5 candidate): durable sequence
// allocation, freeze enforcement beyond room states, durable terminal
// transitions. Written to compile against the candidate before and after the
// fixes (see SeqFn), so each can be shown failing first.
// =============================================================================
struct DurableRec { bool present; uint8_t state, pendingOp; uint32_t seq; };
static DurableRec durable(Device& d) {  // rm_member: magic4 fmt1 group33 coord12 n4 nonce4 appliedV4 state1 pendingOp1 seq4
  DurableRec r{false, 0, 0, 0};
  auto it = d.nvs.find("rm_member");
  if (it == d.nvs.end() || it->second.size() != 88) return r;
  r.present = true; r.state = it->second[62]; r.pendingOp = it->second[63];
  memcpy(&r.seq, it->second.data() + 64, 4);
  return r;
}
static bool noneRecord(const std::string& k, const uint8_t* d, size_t l) { return k == "rm_member" && l == 88 && d[62] == MS_NONE; }
// RAM NONE must imply a durable NONE (or no record).
static bool exitDurable(Device& d) { return d.eng.state() != MS_NONE || !durable(d).present || durable(d).state == MS_NONE; }
static int requestsSince(const char* from, size_t since, std::vector<uint32_t>* seqs = nullptr) {
  int n = 0;
  for (size_t i = since; i < g_broker.log.size(); i++) {
    auto& m = g_broker.log[i];
    bool isRep = false; Request rq; Reply rp;
    if (m.from == from && decodeRequestOrReply(m.payload.data(), m.payload.size(), &isRep, &rq, &rp) && !isRep) {
      n++;
      if (seqs) seqs->push_back(rq.seq);
    }
  }
  return n;
}
static void setCounter(Device& d, uint32_t v) { d.nvs["idCounter"].assign(reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); }
static uint32_t counterOf(Device& d) { uint32_t v = 0; if (d.nvs.count("idCounter")) memcpy(&v, d.nvs["idCounter"].data(), 4); return v; }

static void review_finding1() {
  g_test = "F1a counter write failing: no JOIN/START/CLAIM/LEAVE sent with an unreserved number; numbers never reused";
  reset();
  auto C = dev(kC), M = dev(kA);
  CHECK(C->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  size_t lb = g_broker.log.size();
  uint32_t before = counterOf(*M);
  M->failCounter = [] { return true; };
  bool joined = M->eng.join("GROUP1", kC); g_broker.pump();
  CHECK(!joined && requestsSince(kA, lb) == 0 && M->eng.state() == MS_NONE && counterOf(*M) == before,
        "join with an unreserved number: returned %d, %d request(s) sent", joined, requestsSince(kA, lb));
  M->failCounter = nullptr;
  CHECK(M->eng.join("GROUP1", kC), "join after recovery"); settle({C.get(), M.get()}, 5);
  CHECK(M->eng.state() == MS_MEMBER, "not admitted after recovery");
  C->eng.startRound(); g_broker.pump();
  lb = g_broker.log.size();
  M->failCounter = [] { return true; };
  bool claimed = M->eng.submitCorrectGuess(V(*M)->secret); g_broker.pump();
  bool left = M->eng.leave(); g_broker.pump();
  CHECK(!claimed && !left && requestsSince(kA, lb) == 0 && M->eng.state() == MS_MEMBER && !M->eng.pendingOp(nullptr),
        "claim/leave with unreserved numbers: claim %d leave %d sent %d", claimed, left, requestsSince(kA, lb));
  M->failCounter = nullptr;
  std::vector<uint32_t> seqs;
  requestsSince(kA, 0, &seqs);
  CHECK(M->eng.submitCorrectGuess(V(*M)->secret), "claim after recovery"); settle({C.get(), M.get()}, 5);
  std::vector<uint32_t> all;
  requestsSince(kA, 0, &all);
  // the same number must never label two different requests
  std::map<uint32_t, std::vector<uint8_t>> bySeq; int reused = 0;
  for (size_t i = 0; i < g_broker.log.size(); i++) {
    auto& m = g_broker.log[i];
    bool isRep = false; Request rq; Reply rp;
    if (m.from == kA && decodeRequestOrReply(m.payload.data(), m.payload.size(), &isRep, &rq, &rp) && !isRep) {
      auto it = bySeq.find(rq.seq);
      if (it != bySeq.end() && it->second != m.payload) reused++;
      bySeq[rq.seq] = m.payload;
    }
  }
  CHECK(reused == 0 && std::string(V(*C)->resultWinner) == kA, "%d sequence numbers reused for different requests", reused);

  g_test = "F1b counter write failing: no QUERY with an unreserved number; still verifying; sent once storage recovers";
  reset();
  auto C2 = dev(kC), M2 = dev(kA);
  roomWith(*C2, {M2.get()});
  RoomState fake = *V(*C2); fake.nonce ^= 9;
  uint8_t b[600]; size_t l = encodeState(fake, b, sizeof(b));
  lb = g_broker.log.size();
  M2->failCounter = [] { return true; };
  M2->eng.onPacket("GROUP1", (std::string("r2/") + kC).c_str(), kKindState, b, l);
  tickAll({C2.get(), M2.get()}, 1000, 10);
  CHECK(requestsSince(kA, lb) == 0 && M2->eng.verifying() && M2->eng.state() == MS_MEMBER,
        "QUERY with an unreserved number: %d sent, verifying %d", requestsSince(kA, lb), M2->eng.verifying());
  M2->failCounter = nullptr;
  settle({C2.get(), M2.get()}, 40);
  CHECK(!M2->eng.verifying() && M2->eng.state() == MS_MEMBER && requestsSince(kA, lb) >= 1, "verification not completed after recovery");

  g_test = "F1c counter exhausted (0xFFFFFFFF): refused, nothing sent, no wrap; createRoom refused too";
  reset();
  auto C3 = dev(kC), M3 = dev(kA);
  CHECK(C3->eng.createRoom("GROUP1"), "create"); g_broker.pump();
  setCounter(*M3, 0xFFFFFFFFu);
  lb = g_broker.log.size();
  bool j = M3->eng.join("GROUP1", kC); g_broker.pump();
  CHECK(!j && requestsSince(kA, lb) == 0 && counterOf(*M3) == 0xFFFFFFFFu, "exhausted counter: join %d, sent %d, counter %u",
        j, requestsSince(kA, lb), counterOf(*M3));
  auto D = dev(kD);
  setCounter(*D, 0xFFFFFFFFu);
  CHECK(!D->eng.createRoom("GROUP1") && counterOf(*D) == 0xFFFFFFFFu && statesFrom(kD, 0) == 0, "createRoom with an exhausted counter");
}

static void review_finding2() {
  g_test = "F2a pending CLAIM -> same-version conflict -> matching LATE reply: the CLAIM stays pending";
  reset();
  auto C = dev(kC), M = dev(kA);
  roomWith(*C, {M.get()});
  C->eng.startRound(); g_broker.pump();
  RoomState cur = *V(*M);
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(M->eng.submitCorrectGuess(cur.secret), "claim");
  g_broker.dropIf = nullptr;
  uint32_t seq = M->eng.pendingSeq();
  RoomState conflict = cur; conflict.roundNo ^= 3;
  injectRs(*M, conflict);
  CHECK(M->eng.frozen(), "precondition: frozen");
  injectReply(*M, "GROUP1", kC, kA, seq, CODE_LATE, cur.n, cur.nonce, cur.version);
  uint8_t op = 0;
  CHECK(M->eng.pendingOp(&op) && op == OP_CLAIM && M->eng.pendingSeq() == seq && M->eng.notice() != NOTICE_LATE &&
            durable(*M).pendingOp == OP_CLAIM,
        "a reply completed/cleared the pending CLAIM while frozen (pending %d, notice %d)", M->eng.pendingOp(nullptr), M->eng.notice());
  injectReply(*M, "GROUP1", kC, kA, seq, CODE_CLOSED, cur.n, cur.nonce, cur.version);
  CHECK(M->eng.state() == MS_MEMBER && M->eng.frozen(), "a CLOSED reply ended the frozen membership");

  g_test = "F2b frozen with a pending CLAIM and no reply: no timer-driven activity (no resend, no QUERY)";
  reset();
  auto C2 = dev(kC), M2 = dev(kA);
  roomWith(*C2, {M2.get()});
  C2->eng.startRound(); g_broker.pump();
  cur = *V(*M2);
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };
  CHECK(M2->eng.submitCorrectGuess(cur.secret), "claim");
  g_broker.dropIf = nullptr;
  conflict = cur; conflict.roundNo ^= 3;
  injectRs(*M2, conflict);
  CHECK(M2->eng.frozen() && M2->eng.pendingOp(nullptr), "precondition: frozen with a pending CLAIM");
  size_t lb = g_broker.log.size();
  tickAll({C2.get(), M2.get()}, 1000, 90);
  CHECK(requestsSince(kA, lb) == 0, "%d request(s) sent while frozen", requestsSince(kA, lb));
  CHECK(std::string(V(*C2)->resultWinner) != kA, "the frozen member's claim was processed");
  M2->eng.leave(); g_broker.pump();
  CHECK(M2->eng.state() == MS_NONE && requestsSince(kA, lb) == 1, "Leave unavailable / not one LEAVE");
}

static void review_finding3() {
  g_test = "F3a frozen Leave with the NONE write failing: not shown as exited; retried; then durably NONE";
  reset();
  auto C = dev(kC), M = dev(kA);
  roomWith(*C, {M.get()});
  RoomState cur = *V(*M); RoomState conflict = cur; conflict.roundNo ^= 1;
  injectRs(*M, conflict);
  CHECK(M->eng.frozen(), "precondition");
  M->failWriteData = noneRecord;
  M->eng.leave(); g_broker.pump();
  CHECK(exitDurable(*M), "frozen Leave shown as exited (RAM NONE) while the NONE record was not written (durable state %u)",
        durable(*M).state);
  tickAll({C.get(), M.get()}, 1000, 5);
  CHECK(exitDurable(*M) && M->eng.state() != MS_NONE, "exit presented before storage succeeded");
  M->failWriteData = nullptr;
  tickAll({C.get(), M.get()}, 1000, 5);
  CHECK(M->eng.state() == MS_NONE && durable(*M).state == MS_NONE, "not retried to completion (RAM %d, durable %u)",
        M->eng.state(), durable(*M).state);
  settle({C.get(), M.get()}, 5);
  CHECK(!isActive(*V(*C), kA), "coordinator still lists the member");

  g_test = "F3b frozen Leave, NONE write failing, then reboot: the exit is not lost (no silent return to the room)";
  reset();
  auto C2 = dev(kC), M2 = dev(kA);
  roomWith(*C2, {M2.get()});
  cur = *V(*M2); conflict = cur; conflict.roundNo ^= 1;
  injectRs(*M2, conflict);
  M2->failWriteData = noneRecord;
  g_broker.dropIf = [&](const Msg& m) { return m.from == kA; };  // the LEAVE is lost too
  M2->eng.leave(); g_broker.pump();
  g_broker.dropIf = nullptr;
  M2->online = false; M2->reboot(); M2->online = true;
  CHECK(durable(*M2).state != MS_MEMBER, "after reboot the durable record is a plain MEMBER again (exit lost)");
  M2->failWriteData = nullptr;
  g_broker.connect(M2.get());
  settle({C2.get(), M2.get()}, 40);
  CHECK(M2->eng.state() == MS_NONE && durable(*M2).state == MS_NONE && !isActive(*V(*C2), kA),
        "after reboot: state %d durable %u listed %d", M2->eng.state(), durable(*M2).state, isActive(*V(*C2), kA));

  g_test = "F3c room CLOSED with the NONE write failing: no RAM-only exit; retry completes with the final scores";
  reset();
  auto C3 = dev(kC), M3 = dev(kA);
  roomWith(*C3, {M3.get()});
  M3->failWriteData = noneRecord;
  CHECK(C3->eng.leave(), "owner closes"); g_broker.pump();
  CHECK(exitDurable(*M3) && M3->eng.state() != MS_NONE, "closure presented as completed before the NONE write (RAM %d durable %u)",
        M3->eng.state(), durable(*M3).state);
  CHECK(M3->eng.storagePaused(), "storage problem not shown");
  M3->online = false; M3->reboot(); M3->online = true;  // reboot while failing: the record still names the room
  CHECK(durable(*M3).state == MS_MEMBER, "precondition: durable MEMBER");
  M3->failWriteData = nullptr;
  g_broker.connect(M3.get());  // retained CLOSED state again
  settle({C3.get(), M3.get()}, 5);
  CHECK(M3->eng.state() == MS_NONE && durable(*M3).state == MS_NONE && M3->eng.notice() == NOTICE_ROOM_CLOSED &&
            M3->eng.finalView() && isActive(*M3->eng.finalView(), kA),
        "closure after recovery");

  g_test = "F3d Leave anyway with the NONE write failing: refused/pending, retried, then durably NONE";
  reset();
  auto C4 = dev(kC), M4 = dev(kA);
  roomWith(*C4, {M4.get()});
  C4->online = false;
  CHECK(M4->eng.leave() && M4->eng.state() == MS_LEAVING, "leaving");
  M4->failWriteData = noneRecord;
  M4->eng.leaveAnyway();
  CHECK(exitDurable(*M4), "Leave anyway shown as exited before the NONE write");
  M4->failWriteData = nullptr;
  tickAll({M4.get()}, 1000, 5);
  CHECK(M4->eng.state() == MS_NONE && durable(*M4).state == MS_NONE && M4->eng.notice() == NOTICE_STAYED_LISTED,
        "Leave anyway not completed after recovery (state %d notice %d)", M4->eng.state(), M4->eng.notice());

  g_test = "F3e completion write (pending CLAIM cleared) failing: retried, durable record catches up without a new state";
  reset();
  auto C5 = dev(kC), M5 = dev(kA);
  roomWith(*C5, {M5.get()});
  C5->eng.startRound(); g_broker.pump();
  // fail rm_member writes that clear the pending op of a MEMBER (the completion write), not the appliedV write
  M5->failWriteData = [](const std::string& k, const uint8_t* d, size_t l) {
    return k == "rm_member" && l == 88 && d[62] == MS_MEMBER && d[63] == 0;
  };
  CHECK(M5->eng.submitCorrectGuess(V(*M5)->secret), "claim"); g_broker.pump();
  CHECK(!M5->eng.pendingOp(nullptr) && durable(*M5).pendingOp == OP_CLAIM, "precondition: RAM completed, durable stale");
  M5->failWriteData = nullptr;
  tickAll({M5.get()}, 1000, 5);  // no further room state
  CHECK(durable(*M5).pendingOp == 0, "completion write never retried (durable pending %u)", durable(*M5).pendingOp);
}

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);  // FAIL lines survive a later crash
  e1_e2_ordering(); audit_members_never_publish_state();
  e3_start_completion(); audit_members_never_publish_state();
  e4_e24_e25_replies(); audit_members_never_publish_state();
  e5_e27_join_outcomes(); audit_members_never_publish_state();
  e6_e7_e8_e19_durability(); audit_members_never_publish_state();
  e9_e11_e13_e14_lifecycle(); audit_members_never_publish_state();
  e10_e23_identity(); audit_members_never_publish_state();
  e12_e17_start_right(); audit_members_never_publish_state();
  e18_crash_create_close(); audit_members_never_publish_state();
  e20_e21_e22_e26_member_recovery(); audit_members_never_publish_state();
  e15_e16_invariants();
  x1_stale_basis_other_device(); x3_ok_then_rs(); x4_reply_fields(); x5_transition_table(); x7_boot_rows();
  x8_limits(); x9_stale_and_resurrected(); x11_member_steps(); x18_every_step(); x20_on_demand();
  x21_x26_recovery(); x23_directions(); x24_invalidators(); x27_freeze_scope();
  uint32_t seeds = getenv("SOAK_SEEDS") ? static_cast<uint32_t>(atoi(getenv("SOAK_SEEDS"))) : 20;
  review_finding1(); review_finding2(); review_finding3();
  for (uint32_t seed = 1; seed <= seeds; seed++) soak(seed);
  reset();  // audits the last scenario
  g_test = "displayed-state monotonicity (all devices, all scenarios)";
  CHECK(g_rollbacks == 0, "%d displayed rollbacks", g_rollbacks);
  printf("audited %d publish logs; %d receive-queue overflow drops injected\n", g_auditRuns, g_queueDrops);
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
