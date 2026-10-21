// HOST TEST (not production). The ACTUAL src/core/identity.cpp: Race sequence
// reservation (Identity::reserveCounter) next to the shared message-id counter
// (Identity::nextId), near UINT32_MAX, through exhaustion and reboots.
// Storage::core() is the in-memory, fault-injecting fake Preferences; a
// "reboot" re-runs Identity::init() over the same stored keys.
#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/identity.h"
#include "core/storage_init.h"

HostWiFi WiFi;
HostSerial Serial;
uint32_t millis() { return 0; }
static Preferences g_core;
namespace Storage {
Preferences& core() { return g_core; }
}  // namespace Storage

static int g_fail = 0, g_pass = 0;
static std::string g_test;
#define CHECK(c, ...)                                                        \
  do {                                                                       \
    if (c) { g_pass++; } else {                                              \
      g_fail++; printf("FAIL [%s] line %d: ", g_test.c_str(), __LINE__);     \
      printf(__VA_ARGS__); printf("\n");                                     \
    }                                                                        \
  } while (0)

static void reboot() { Identity::init(); }  // RAM state is rebuilt; stored keys persist
static uint32_t idCounter() { return g_core.getULong("idCounter", 0); }

// Every Race number handed out, across the whole scenario (reboots included).
static std::set<uint32_t> g_race;
static uint32_t g_lastRace = 0;
static int g_reused = 0, g_decreased = 0;
static bool race(uint32_t* out = nullptr) {
  uint32_t v = 0;
  if (!Identity::reserveCounter(&v)) return false;
  if (!g_race.insert(v).second) g_reused++;
  if (v <= g_lastRace) g_decreased++;
  g_lastRace = v;
  if (out) *out = v;
  return true;
}

static void interleaved_near_max() {
  g_test = "S1 counter near UINT32_MAX; numbers already used by Race (all from idCounter, as in the previous candidate)";
  g_core.kv.clear();
  const uint32_t start = 0xFFFFFFFFu - 6;
  g_core.putULong("idCounter", start);
  // Race numbers used before this firmware: every one came from idCounter, so all are <= start.
  for (uint32_t s = start - 3; s <= start; s++) g_race.insert(s);
  g_lastRace = start;
  reboot();

  g_test = "S2 interleave nextId() and Race reservations across the wrap of the message-id counter";
  char id[32];
  uint32_t v = 0;
  CHECK(race(&v) && v == start + 1, "first Race number %u (expected %u)", v, start + 1);
  Identity::nextId(id, sizeof(id));
  Identity::nextId(id, sizeof(id));
  CHECK(race(&v), "race 2");
  for (int i = 0; i < 4; i++) Identity::nextId(id, sizeof(id));  // the message-id counter passes UINT32_MAX
  bool raceOk = race(&v);
  CHECK(!raceOk || v > start, "a Race number came back below the used range after nextId() wrapped: %u", v);
  reboot();
  for (int i = 0; i < 3; i++) Identity::nextId(id, sizeof(id));
  while (race()) {
    Identity::nextId(id, sizeof(id));
    if (g_race.size() > 64) break;  // guard: exhaustion must come within the 6 numbers left
  }
  CHECK(g_reused == 0 && g_decreased == 0, "%d Race numbers reused, %d decreased", g_reused, g_decreased);
  CHECK(g_lastRace == 0xFFFFFFFFu, "Race did not end exactly at UINT32_MAX (last %u)", g_lastRace);

  g_test = "S3 exhausted: refused (no wrap), also after reboots and further nextId() calls";
  uint32_t got = 12345;
  CHECK(!Identity::reserveCounter(&got) && got == 12345, "a number was returned after exhaustion: %u", got);
  reboot();
  Identity::nextId(id, sizeof(id));
  CHECK(!Identity::reserveCounter(&got) && got == 12345, "a number was returned after reboot: %u", got);
  reboot();
  CHECK(!race(), "a Race number after a second reboot");
  CHECK(g_reused == 0 && g_decreased == 0, "%d reused, %d decreased", g_reused, g_decreased);
  printf("   Race numbers handed out: %zu (incl. %d pre-existing); message-id counter now %u\n", g_race.size(), 4,
         idCounter());
}

static void write_failures() {
  g_test = "S4 Race write failing (fresh device): no number; after recovery numbers continue, none reused";
  g_core.kv.clear(); g_race.clear(); g_lastRace = 0; g_reused = g_decreased = 0;
  char id[32];
  g_core.putULong("idCounter", 5);  // as left by the earlier candidate: Race numbers 1..5 may have been used
  for (uint32_t s = 1; s <= 5; s++) g_race.insert(s);
  g_lastRace = 5;
  reboot();
  g_core.failWrite = [](const char*) { return true; };
  uint32_t got = 777;
  CHECK(!Identity::reserveCounter(&got) && got == 777, "a number was returned although nothing could be stored: %u", got);
  g_core.failWrite = nullptr;
  uint32_t v = 0;
  CHECK(race(&v) && v == 6, "first Race number after recovery %u (expected 6)", v);
  for (int i = 0; i < 3; i++) Identity::nextId(id, sizeof(id));
  reboot();
  CHECK(race(&v) && v > 6, "next Race number %u (expected > 6)", v);
  CHECK(g_reused == 0 && g_decreased == 0, "%d reused, %d decreased", g_reused, g_decreased);

  g_test = "S5 message-id counter already at UINT32_MAX before the first Race reservation: refused, not wrapped";
  g_core.kv.clear();
  g_core.putULong("idCounter", 0xFFFFFFFFu);
  reboot();
  got = 4242;
  CHECK(!Identity::reserveCounter(&got) && got == 4242, "a Race number after UINT32_MAX: %u", got);
}

static void read_failures() {
  g_test = "S6 stored raceSeq unreadable: refused (never restarted from 0 or from idCounter)";
  g_core.kv.clear();
  g_core.putULong("idCounter", 10);
  uint32_t v = 0;
  CHECK(Identity::reserveCounter(&v) && v == 11, "migration %u", v);
  CHECK(Identity::reserveCounter(&v) && v == 12, "next %u", v);
  g_core.failRead = [](const char* k) { return strcmp(k, "raceSeq") == 0; };
  uint32_t got = 99;
  CHECK(!Identity::reserveCounter(&got) && got == 99, "a number despite an unreadable raceSeq: %u", got);
  g_core.failRead = nullptr;
  CHECK(Identity::reserveCounter(&v) && v == 13, "after the read recovers %u", v);

  g_test = "S7 idCounter unreadable at migration: refused (never started from 0)";
  g_core.kv.clear();
  g_core.putULong("idCounter", 50);
  g_core.failRead = [](const char* k) { return strcmp(k, "idCounter") == 0; };
  got = 99;
  CHECK(!Identity::reserveCounter(&got) && got == 99, "a number despite an unreadable idCounter: %u", got);
  g_core.failRead = nullptr;
  CHECK(Identity::reserveCounter(&v) && v == 51, "migration after recovery %u", v);
}

// Migration source protection: idCounter (nextId()'s counter) is the only
// record of which Race numbers the earlier candidate may have used, so it must
// not change -- above all, not wrap -- before raceSeq is stored.
static uint32_t raceSeqStored() { return g_core.getULong("raceSeq", 0); }

static void migration_order() {
  char id[32];
  uint32_t got;

  g_test = "S8 nextId() before the first Race reservation, idCounter = UINT32_MAX (the reported counterexample), with boot";
  g_core.kv.clear(); g_core.failWrite = nullptr; g_core.failRead = nullptr;
  g_core.putULong("idCounter", 0xFFFFFFFFu);
  reboot();
  Identity::nextId(id, sizeof(id));  // message ids continue (wrapping, as before)
  got = 4242;
  CHECK(!Identity::reserveCounter(&got) && got == 4242, "Race number %u after nextId() wrapped the migration source", got);
  reboot();
  CHECK(!Identity::reserveCounter(&got) && got == 4242, "Race number %u after a reboot", got);

  g_test = "S8b same, nextId() before any init() migration attempt (nextId() itself protects the source)";
  g_core.kv.clear();
  g_core.putULong("idCounter", 0xFFFFFFFFu);
  Identity::nextId(id, sizeof(id));  // no reboot since the key was set
  got = 4242;
  CHECK(!Identity::reserveCounter(&got) && got == 4242, "Race number %u: migrated from the wrapped value", got);

  g_test = "S9 migration write failure, then nextId(), then reboot: the source is unchanged until raceSeq is stored";
  g_core.kv.clear();
  g_core.putULong("idCounter", 0xFFFFFFF0u);
  g_core.failWrite = [](const char*) { return true; };  // every mb_core write fails
  reboot();                                             // migration at boot fails
  for (int i = 0; i < 40; i++) Identity::nextId(id, sizeof(id));
  CHECK(!g_core.isKey("raceSeq") && idCounter() == 0xFFFFFFF0u, "source changed while raceSeq was not stored: idCounter %u",
        idCounter());
  got = 4242;
  CHECK(!Identity::reserveCounter(&got) && got == 4242, "a Race number while nothing can be stored: %u", got);
  g_core.failWrite = nullptr;
  reboot();  // migration succeeds now
  CHECK(g_core.isKey("raceSeq") && raceSeqStored() == 0xFFFFFFF0u, "migration after recovery stored %u", raceSeqStored());
  uint32_t v = 0;
  CHECK(Identity::reserveCounter(&v) && v == 0xFFFFFFF1u, "first Race number after recovery %u", v);
  for (int i = 0; i < 40; i++) Identity::nextId(id, sizeof(id));  // idCounter wraps freely now
  CHECK(Identity::reserveCounter(&v) && v == 0xFFFFFFF2u, "Race number after nextId() wrapped: %u", v);

  g_test = "S9b only the raceSeq write fails (idCounter writes would succeed): nextId() still leaves the source alone";
  g_core.kv.clear();
  g_core.putULong("idCounter", 0xFFFFFFFFu);
  g_core.failWrite = [](const char* k) { return strcmp(k, "raceSeq") == 0; };
  reboot();
  Identity::nextId(id, sizeof(id));
  CHECK(idCounter() == 0xFFFFFFFFu, "idCounter wrapped (now %u) before raceSeq was stored", idCounter());
  g_core.failWrite = nullptr;
  reboot();
  got = 4242;
  CHECK(!Identity::reserveCounter(&got) && got == 4242, "Race number %u after recovery", got);

  g_test = "S10 exhausted migration, then nextId() (wraps idCounter), then reboots: Race stays exhausted";
  g_core.kv.clear();
  g_core.putULong("idCounter", 0xFFFFFFFFu);
  reboot();
  CHECK(raceSeqStored() == 0xFFFFFFFFu, "exhausted migration not stored (raceSeq %u)", raceSeqStored());
  for (int i = 0; i < 5; i++) Identity::nextId(id, sizeof(id));
  CHECK(idCounter() < 0xFFFFFFFFu, "precondition: nextId() wraps idCounter as before (now %u)", idCounter());
  reboot(); reboot();
  got = 4242;
  CHECK(!Identity::reserveCounter(&got) && got == 4242 && raceSeqStored() == 0xFFFFFFFFu,
        "Race number %u after the source wrapped (raceSeq %u)", got, raceSeqStored());

  g_test = "S11 idCounter unreadable at boot: no migration from a default; nextId() leaves the source alone";
  g_core.kv.clear();
  g_core.putULong("idCounter", 1000);
  g_core.failRead = [](const char* k) { return strcmp(k, "idCounter") == 0; };
  reboot();
  Identity::nextId(id, sizeof(id));
  CHECK(!g_core.isKey("raceSeq"), "migrated from an unreadable idCounter (raceSeq %u)", raceSeqStored());
  got = 4242;
  CHECK(!Identity::reserveCounter(&got) && got == 4242, "Race number %u from an unreadable source", got);
  g_core.failRead = nullptr;
  CHECK(idCounter() == 1000, "idCounter changed while raceSeq was not stored (now %u)", idCounter());
  reboot();
  CHECK(Identity::reserveCounter(&v) && v == 1001, "first Race number after the read recovered: %u", v);
}

// Per-call read faults: the n-th getULong() of one key within an operation
// fails (returns its default, as nvs_get_u32 errors do). Every outcome must be
// either the exact stored value used correctly, or a refusal that leaves the
// output and both stored counters unchanged -- never a failed read's default
// taken as stored data.
enum class ReadFault { NONE, FIRST, SECOND, BOTH };
static const char* faultName(ReadFault f) {
  return f == ReadFault::NONE ? "no fault" : f == ReadFault::FIRST ? "first read fails" : f == ReadFault::SECOND ? "second read fails" : "both reads fail";
}
static void injectReads(const char* key, ReadFault f) {
  std::string k = key;
  auto n = std::make_shared<int>(0);
  g_core.failRead = [k, f, n](const char* kk) {
    if (k != kk) return false;
    int i = (*n)++;
    return (f == ReadFault::FIRST && i == 0) || (f == ReadFault::SECOND && i == 1) || (f == ReadFault::BOTH && i <= 1);
  };
}
static const uint32_t kValues[] = {0u, 50u, 0xFFFFFFFFu};
static const ReadFault kFaults[] = {ReadFault::NONE, ReadFault::FIRST, ReadFault::SECOND, ReadFault::BOTH};

static void read_faults_reservation() {
  for (uint32_t val : kValues) for (ReadFault f : kFaults) {
    char name[160];
    snprintf(name, sizeof(name), "S12 raceSeq reservation, stored %u, %s", val, faultName(f));
    g_test = name;
    g_core.kv.clear(); g_core.failWrite = nullptr; g_core.failRead = nullptr;
    g_core.putULong("idCounter", 7);
    g_core.putULong("raceSeq", val);
    injectReads("raceSeq", f);
    const uint32_t sentinel = 0xA5A5A5A5u;
    uint32_t out = sentinel;
    bool ok = Identity::reserveCounter(&out);
    g_core.failRead = nullptr;
    uint32_t seqNow = g_core.getULong("raceSeq", 0), idNow = g_core.getULong("idCounter", 0);
    if (val == 0xFFFFFFFFu) {
      CHECK(!ok && out == sentinel && seqNow == val && idNow == 7, "exhausted: ok %d out %u raceSeq %u", ok, out, seqNow);
    } else if (f == ReadFault::NONE) {
      CHECK(ok && out == val + 1 && seqNow == val + 1, "ok %d out %u raceSeq %u (expected %u)", ok, out, seqNow, val + 1);
    } else {
      bool refusedClean = !ok && out == sentinel && seqNow == val && idNow == 7;
      bool exact = ok && out == val + 1 && seqNow == val + 1 && idNow == 7;
      CHECK(refusedClean || exact, "a failed read's default was used: ok %d out %u raceSeq %u (stored %u)", ok, out, seqNow, val);
    }
    uint32_t again = sentinel;  // after the fault: the sequence continues from the stored value
    bool ok2 = Identity::reserveCounter(&again);
    if (val == 0xFFFFFFFFu) CHECK(!ok2 && again == sentinel, "number %u after exhaustion", again);
    else CHECK(ok2 && again == (ok ? val + 2 : val + 1), "after recovery %u (stored was %u)", again, val);
  }
}

static void read_faults_migration() {
  for (uint32_t val : kValues) for (ReadFault f : kFaults) {
    char name[160];
    snprintf(name, sizeof(name), "S13 idCounter migration at boot, stored %u, %s", val, faultName(f));
    g_test = name;
    g_core.kv.clear(); g_core.failWrite = nullptr; g_core.failRead = nullptr;
    g_core.putULong("idCounter", val);
    injectReads("idCounter", f);
    reboot();  // init(): migration
    g_core.failRead = nullptr;
    bool migrated = g_core.isKey("raceSeq");
    uint32_t seqNow = g_core.getULong("raceSeq", 0xDEADBEEFu), idNow = g_core.getULong("idCounter", 0);
    if (f == ReadFault::NONE) {
      CHECK(migrated && seqNow == val && idNow == val, "migration stored %u (expected %u)", seqNow, val);
    } else {
      CHECK((!migrated && idNow == val) || (migrated && seqNow == val && idNow == val),
            "a failed read's default was migrated: raceSeq %u, idCounter %u (stored %u)", seqNow, idNow, val);
    }
    const uint32_t sentinel = 0xA5A5A5A5u;
    uint32_t out = sentinel;
    bool ok = Identity::reserveCounter(&out);  // no faults: migrates now if it did not before
    if (val == 0xFFFFFFFFu) CHECK(!ok && out == sentinel && g_core.getULong("raceSeq", 0) == val, "number %u from an exhausted source", out);
    else CHECK(ok && out == val + 1, "first Race number %u (expected %u)", out, val + 1);
  }
}

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);  // FAIL lines survive a later crash
  interleaved_near_max();
  write_failures();
  read_failures();
  migration_order();
  read_faults_reservation();
  read_faults_migration();
  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
