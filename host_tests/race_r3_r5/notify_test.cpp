// HOST TEST (not production). Unread marking and Main Menu badges
// (hardware finding 3 on 30ed514). Compiles the ACTUAL src/core/menu.cpp,
// notifications.cpp and text_message.cpp into this translation unit, and
// links the actual enigma.cpp (its chat and its PK_MESSAGE handler).
// Built with -DREAL_NOTIFICATIONS: the shared stubs then omit their
// Notifications stub and scan the fake message store for recounts.
#include "stubs.h"
#include "core/menu.cpp"
#include "core/notifications.cpp"
#include "core/text_message.cpp"
#include "core/enigma.h"
#include <cstdio>
#include <string>
#include <vector>

// ---- the MessageStore/Storage/Sleep/sound hooks notifications.cpp needs ----
namespace MessageStore {
void updateLocalFlags(const MessageRef& ref, uint16_t setMask, uint16_t clearMask) {
  for (auto& r : T::store)
    if (r.ref.sequence == ref.sequence && !strcmp(r.ref.group_code, ref.group_code) && !strcmp(r.ref.contact_key, ref.contact_key))
      r.hdr.flags = static_cast<uint16_t>((r.hdr.flags | setMask) & ~clearMask);
}
void setOnEvictedCallback(EvictedCallback) {}
}  // namespace MessageStore
namespace Sleep { void notifyActivity() {} }
void playTone(uint16_t, uint16_t, SoundClass) {}
static Preferences g_notifyPrefs;
namespace Storage { Preferences& notify() { return g_notifyPrefs; } }

// NV: the notification internals the robustness cases use. -DNOTIFY_V1
// builds the same file against the first candidate's notifications.cpp (no
// load state, tick or summary state) for before/after evidence: there the
// shim supplies inert stand-ins and only observable behaviour decides.
#ifdef NOTIFY_V1
namespace NV {
int g_loadState = 0;
bool g_saveDirty = false;
enum { LOAD_OK = 0, LOAD_RETRY_CURRENT = 1, LOAD_RETRY_LEGACY = 2, SUMMARY_FREE = 0, SUMMARY_COUNTED = 1, SUMMARY_UNRESOLVED = 2 };
const uint32_t kRetryMs = 10000;
inline void serviceTick() {}
inline void resetRetryClock() {}
inline int stateField(const Notifications::Summary* x) { return x->active ? 1 : 0; }
inline void resetVolatile() { memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries)); }
}  // namespace NV
#else
namespace NV {
using Notifications::g_loadState;
using Notifications::g_saveDirty;
using Notifications::LOAD_OK;
using Notifications::LOAD_RETRY_CURRENT;
using Notifications::LOAD_RETRY_LEGACY;
using Notifications::SUMMARY_FREE;
using Notifications::SUMMARY_COUNTED;
using Notifications::SUMMARY_UNRESOLVED;
using Notifications::kRetryMs;
using Notifications::serviceTick;
inline void resetRetryClock() { Notifications::g_nextRetryMs = T::now; }
inline int stateField(const Notifications::Summary* x) { return x->state; }
// Every piece of volatile notification state goes back to its power-on value:
// a reboot keeps only the simulated durable storage (g_notifyPrefs, T::store).
// Heap objects (sweep, job, read-up list) are freed before their pointers
// are cleared. -DNOTIFY_HEAD builds against 23293c5's notifications.cpp
// (no jobs or read-ups) for before/after evidence.
inline void resetVolatile() {
#ifndef NOTIFY_HEAD
  if (Notifications::g_sweep != nullptr) MessageStore::endConversationWalk(Notifications::g_sweep->walk);
#endif
  delete Notifications::g_sweep;
  Notifications::g_sweep = nullptr;
#ifndef NOTIFY_HEAD
  Notifications::g_recountRetryMs = 0;
  Notifications::g_recountFailures = 0;
  delete Notifications::g_job;
  Notifications::g_job = nullptr;
  delete Notifications::g_readUps;
  Notifications::g_readUps = nullptr;
  Notifications::g_readUpsDirty = false;
  Notifications::g_nextStepMs = T::now;
  Notifications::g_saveDueMs = 0;
  Notifications::g_readUpsDueMs = 0;
#endif
  memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries));
  Notifications::g_loadState = Notifications::LOAD_OK;
  Notifications::g_saveDirty = false;
  Notifications::g_reconcileCursor = 0;
  Notifications::g_raceInvitePending = false;
  resetRetryClock();
}
}  // namespace NV
#endif
static int g_fail = 0, g_pass = 0;
static std::string g_test;
#define CHECK(c, ...)                                                        \
  do {                                                                       \
    if (c) { g_pass++; } else {                                              \
      g_fail++; printf("FAIL [%s] line %d: ", g_test.c_str(), __LINE__);     \
      printf(__VA_ARGS__); printf("\n");                                     \
    }                                                                        \
  } while (0)
static void tick(int n = 1) { for (int i = 0; i < n; i++) { Menu::tick(); T::now += 5; } }
static void press(InputEventType t, uint32_t dur = 80) { T::raw.push_back(InputEvent(t, 0, dur, T::now)); }
static void rot(int8_t v) { T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, v, 0, 0)); }
void T_initServices();

static const char* kG = "GROUP1";
static const char* kBob = "B0B000000001";
static uint32_t g_msgNo = 0;
static PacketCodec::MessageEnvelope envOf(uint8_t type) {
  PacketCodec::MessageEnvelope env{};
  snprintf(env.message_id, sizeof(env.message_id), "MSG%020u", ++g_msgNo);
  env.schema_version = 1; env.message_type = type;
  strncpy(env.sender_device_id, kBob, sizeof(env.sender_device_id) - 1);
  strncpy(env.sender_name_cache, "Bob", sizeof(env.sender_name_cache) - 1);
  strncpy(env.group_code, kG, sizeof(env.group_code) - 1);
  env.timestamp = 1700000100u + g_msgNo;
  return env;
}
// A received message through the ACTUAL PK_MESSAGE dispatcher (Text's or Enigma's own arrival handler).
static MessageRef arrive(uint8_t type, const char* body) {
  PacketCodec::MessageEnvelope env = envOf(type);
  uint8_t wire[512];
  size_t n = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>(body), static_cast<uint16_t>(strlen(body)),
                                              wire, sizeof(wire));
  PacketCodec::Header h; const uint8_t* payload = nullptr;
  PacketCodec::decodeHeader(wire, n, &h, &payload);
  handleIncomingMessagePacket(kG, "direct", payload, h.payload_length);
  return T::store.back().ref;
}
// A Friend game record, stored and announced exactly as number_guessing_friend.cpp does.
static MessageRef arriveGame() {
  PacketCodec::MessageEnvelope env = envOf(PacketCodec::MSG_TYPE_GAME);
  uint8_t wire[256];
  const char* body = "GAME";
  size_t n = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>(body), 4, wire, sizeof(wire));
  MessageRef ref;
  MessageStore::appendStoredMessage(kG, kBob, MessageStore::Direction::RECEIVED, MessageStore::FLAG_UNREAD, env.timestamp, wire,
                                    static_cast<uint16_t>(n), nullptr, 0, &ref);
  Notifications::onMessageArrived(kG, kBob, ref, Notifications::BADGE_TEXT | Notifications::BADGE_ENIGMA,
                                  TextMessage::isConversationOpen(kG, kBob));
  return ref;
}
// A received message from any contact, stored and announced as Text's arrival handler does.
static MessageRef arriveFrom(const char* contact, uint8_t type) {
  PacketCodec::MessageEnvelope env = envOf(type);
  strncpy(env.sender_device_id, contact, sizeof(env.sender_device_id) - 1);
  uint8_t wire[256];
  const char* body = "HI";
  size_t n = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>(body), 2, wire, sizeof(wire));
  MessageRef ref;
  MessageStore::appendStoredMessage(kG, contact, MessageStore::Direction::RECEIVED, MessageStore::FLAG_UNREAD, env.timestamp, wire,
                                    static_cast<uint16_t>(n), nullptr, 0, &ref);
  Notifications::onMessageArrived(kG, contact, ref, type == PacketCodec::MSG_TYPE_ENIGMA ? Notifications::BADGE_ENIGMA
                                                                                          : Notifications::BADGE_TEXT, false);
  return ref;
}
// The first half of an arrival only: the message is stored unread (as every
// arrival handler stores it), then the device loses power before
// Notifications::onMessageArrived() runs.
static MessageRef storeOnlyFrom(const char* contact, uint8_t type) {
  PacketCodec::MessageEnvelope env = envOf(type);
  strncpy(env.sender_device_id, contact, sizeof(env.sender_device_id) - 1);
  uint8_t wire[256];
  size_t n = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>("HI"), 2, wire, sizeof(wire));
  MessageRef ref;
  MessageStore::appendStoredMessage(kG, contact, MessageStore::Direction::RECEIVED, MessageStore::FLAG_UNREAD, env.timestamp, wire,
                                    static_cast<uint16_t>(n), nullptr, 0, &ref);
  return ref;
}
static bool unread(const MessageRef& ref) {
  for (auto& r : T::store) if (r.ref.sequence == ref.sequence) return (r.hdr.flags & MessageStore::FLAG_UNREAD) != 0;
  return false;
}
static bool textBadge() { return Notifications::hasAnyUnreadText(); }
static bool enigmaBadge() { return Notifications::hasAnyUnreadEnigma(); }
static void menuStub() {}
static void reset() {
  T::reset();
  Menu::init(); g_stackSize = 1; Menu::consumeJustEntered();
  NV::resetVolatile();
  g_notifyPrefs.kv.clear();
  g_notifyPrefs.failRead = nullptr; g_notifyPrefs.failWrite = nullptr;
  g_msgNo = 0;
  Menu::pushScreen(menuStub);
}
static void openText() { TextMessage::navigateToChatDirect(kG, kBob); tick(3); }
static void openEnigma() { Enigma::navigateToChatDirect(kG, kBob); tick(3); }
static void back() { T::now += 500; press(InputEventType::ENCODER_LONG); tick(3); }

// A reboot of the notification service: RAM cleared, the stored record loaded again.
static void reboot() {
  NV::resetVolatile();  // durable storage (g_notifyPrefs, T::store) is untouched
  Notifications::loadSummaries();
}
// One retry interval (kRetryMs + 1 ms) of main-loop passes, 25 ms apart, each
// calling the notification service tick (as main.cpp's loop() does).
static void notifyTick(int n = 1) {
  for (int i = 0; i < n; i++) {
    uint32_t end = T::now + NV::kRetryMs + 1;
    while (static_cast<int32_t>(end - T::now) > 0) { T::now += 25; NV::serviceTick(); }
  }
}
// The device stays on for 3 s: deferred writes (summary after 2 s, read-up
// record after 1 s) are made, and background work advances.
static void stayOn() {
  uint32_t end = T::now + 3000;
  while (static_cast<int32_t>(end - T::now) > 0) { T::now += 25; NV::serviceTick(); }
}
static int stateOf(const char* contact) {
  Notifications::Summary* x = Notifications::findSummary(kG, contact);
  return x ? NV::stateField(x) : -1;
}
static uint16_t countOf(const char* contact, int slot) {
  Notifications::Summary* x = Notifications::findSummary(kG, contact);
  return x ? x->unread[slot] : 0xFFFF;
}

// The record layout earlier firmware stored under "summary".
struct OldSummary {
  bool active;
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  uint16_t unread_count;
  uint8_t badge_mask;
};

int main() {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  T::reset();
  T_initServices();  // notifications.cpp serviceInit(); text/enigma register their handlers

  g_test = "NR1 rotating onto a received unread message marks it read and clears the badge";
  reset();
  MessageRef m = arrive(PacketCodec::MSG_TYPE_TEXT, "HELLO");
  CHECK(textBadge() && unread(m), "precondition: badge %d unread %d", textBadge(), unread(m));
  openText();
  CHECK(textBadge() && unread(m), "entering the chat (nothing selected) marked it read");
  rot(-1); tick(2);
  CHECK(!textBadge(), "selected: badge %d (it follows at once)", textBadge());
  stayOn();  // the flag on flash is cleared in the background
  CHECK(!textBadge() && !unread(m), "selected: badge %d unread %d", textBadge(), unread(m));
  back();
  CHECK(!textBadge() && !TextMessage::isConversationOpen(kG, kBob), "after Back: badge %d", textBadge());

  g_test = "NR2 read up to the selection: a newer message stays unread; selecting the newest reads both (finding 1 on 23293c5)";
  reset();
  MessageRef older = arrive(PacketCodec::MSG_TYPE_TEXT, "ONE");
  MessageRef newer = arrive(PacketCodec::MSG_TYPE_TEXT, "TWO");
  openText();
  rot(-1); rot(-1); tick(2);  // two detents in one batch: passes over the newest, lands on the older one
  stayOn();
  CHECK(unread(newer) && !unread(older) && textBadge(), "newer %d older %d badge %d", unread(newer), unread(older), textBadge());
  rot(+1); tick(2);  // now the newest
  stayOn();
  CHECK(!unread(newer) && !unread(older) && !textBadge(), "newer %d older %d badge %d", unread(newer), unread(older), textBadge());
  back();

  g_test = "NR3 Enigma chat: rotating onto a received Enigma message marks it read";
  reset();
  MessageRef e = arrive(PacketCodec::MSG_TYPE_ENIGMA, "XQZV");
  CHECK(enigmaBadge() && !textBadge() && unread(e), "precondition: enigma %d text %d", enigmaBadge(), textBadge());
  openEnigma();
  CHECK(enigmaBadge(), "entering the Enigma chat marked it read");
  rot(-1); tick(2);
  stayOn();
  CHECK(!enigmaBadge() && !unread(e), "selected: enigma badge %d unread %d", enigmaBadge(), unread(e));
  back();

  g_test = "NR4 separate categories: reading the Text message clears only the Text badge";
  reset();
  MessageRef t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  MessageRef e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  CHECK(textBadge() && enigmaBadge(), "precondition");
  openText();
  rot(-1); rot(-1); tick(2);  // two detents in one input batch: passes over the Enigma message without drawing it selected
  stayOn();
  CHECK(!unread(t1) && unread(e1) && !textBadge() && enigmaBadge(),
        "only the message left selected is read: text %d enigma %d badges %d/%d", unread(t1), unread(e1), textBadge(), enigmaBadge());
  rot(+1); tick(2);  // now the Enigma message (Unified Thread) is selected
  stayOn();
  CHECK(!unread(e1) && !textBadge() && !enigmaBadge(), "badges text %d enigma %d", textBadge(), enigmaBadge());
  back();
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  Notifications::clearUnread(kG, kBob, t1);  // only the Text message read
  CHECK(!textBadge() && enigmaBadge(), "Text read, Enigma unread: text badge %d enigma badge %d", textBadge(), enigmaBadge());

  g_test = "NR5 a Friend game record lights both badges; reading it clears both";
  reset();
  MessageRef g = arriveGame();
  CHECK(textBadge() && enigmaBadge(), "game record: text %d enigma %d", textBadge(), enigmaBadge());
  openEnigma();
  rot(-1); tick(2);
  stayOn();
  CHECK(!unread(g) && !textBadge() && !enigmaBadge(), "selected in Enigma: unread %d text %d enigma %d", unread(g), textBadge(),
        enigmaBadge());
  back();

  g_test = "NR6 the draft and the selection are kept: selecting (and reading) a message does not touch the draft";
  reset();
  m = arrive(PacketCodec::MSG_TYPE_TEXT, "HELLO");
  openText();
  strcpy(g_composeText, "DRAFT"); g_composeLen = 5;
  rot(-1); tick(2);
  stayOn();
  uint16_t cursor = g_historyCursor;
  CHECK(!unread(m) && strcmp(g_composeText, "DRAFT") == 0, "draft '%s'", g_composeText);
  MessageRef later = arrive(PacketCodec::MSG_TYPE_TEXT, "LATER");  // arrives while the chat is open
  tick(3);
  CHECK(g_historyCursor == cursor && MessageStore::getIndexEntry(g_historyCursor)->sequence == m.sequence,
        "selection moved: cursor %u", g_historyCursor);
  CHECK(!unread(later), "an arrival into the open chat is read (unchanged rule)");
  rot(+1); rot(+1); tick(2);  // back to compose
  CHECK(strcmp(g_composeText, "DRAFT") == 0 && g_historyCursor == kNoHistoryCursor, "draft '%s' cursor %u", g_composeText,
        g_historyCursor);
  back();

  g_test = "NR7 migration of the earlier record: per-type counts recounted from the stored messages";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  {
    OldSummary old[24];
    memset(old, 0, sizeof(old));
    old[0].active = true; strcpy(old[0].group_code, kG); strcpy(old[0].contact_key, kBob);
    old[0].unread_count = 2; old[0].badge_mask = Notifications::BADGE_TEXT | Notifications::BADGE_ENIGMA;
    g_notifyPrefs.kv.clear();
    g_notifyPrefs.putBytes("summary", old, sizeof(old));
    std::string legacyBytes = g_notifyPrefs.kv["summary"];
    memset(Notifications::g_summaries, 0xA5, sizeof(Notifications::g_summaries));
    Notifications::loadSummaries();
    CHECK(textBadge() && enigmaBadge() && Notifications::getUnreadCount(kG, kBob) == 2, "after migration: text %d enigma %d count %u",
          textBadge(), enigmaBadge(), Notifications::getUnreadCount(kG, kBob));
    CHECK(stateOf(kBob) == NV::SUMMARY_UNRESOLVED, "migration recounts in the background, never at boot: state %d",
          stateOf(kBob));
    notifyTick(1);
    CHECK(stateOf(kBob) == NV::SUMMARY_COUNTED && countOf(kBob, Notifications::SLOT_TEXT) == 1 &&
              countOf(kBob, Notifications::SLOT_ENIGMA) == 1,
          "a reliable background scan gives exact counts: state %d text %u enigma %u", stateOf(kBob), countOf(kBob, Notifications::SLOT_TEXT),
          countOf(kBob, Notifications::SLOT_ENIGMA));
    CHECK(g_notifyPrefs.isKey("summary2") && g_notifyPrefs.kv["summary"] == legacyBytes, "summary2 written, old record untouched");
    Notifications::clearUnread(kG, kBob, t1);
    CHECK(!textBadge() && enigmaBadge(), "migrated: reading the Text message leaves only Enigma: text %d enigma %d", textBadge(),
          enigmaBadge());
    stayOn();
    memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries));
    Notifications::loadSummaries();  // a reboot: summary2 is used as stored
    CHECK(!textBadge() && enigmaBadge(), "after reboot: text %d enigma %d", textBadge(), enigmaBadge());
  }

  g_test = "NR8 migration when the store scan is unreliable: UNRESOLVED (badges of the old mask lit), never an approximate count";
  reset();
  {
    OldSummary old[24];
    memset(old, 0, sizeof(old));
    old[0].active = true; strcpy(old[0].group_code, kG); strcpy(old[0].contact_key, kBob);
    old[0].unread_count = 3; old[0].badge_mask = Notifications::BADGE_ENIGMA;
    g_notifyPrefs.putBytes("summary", old, sizeof(old));
    T::scanUnreliable = true;
    reboot();
    stayOn();
    T::scanUnreliable = false;
    CHECK(enigmaBadge() && !textBadge() && stateOf(kBob) == NV::SUMMARY_UNRESOLVED, "enigma %d text %d state %d",
          enigmaBadge(), textBadge(), stateOf(kBob));
    CHECK(g_notifyPrefs.isKey("summary2"), "the UNRESOLVED state is stored (survives a reboot)");
  }

  g_test = "NR9 an unread record evicted: the conversation is recounted (in the background) from what remains";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  for (size_t i = 0; i < T::store.size(); i++)
    if (T::store[i].ref.sequence == t1.sequence) { T::store.erase(T::store.begin() + static_cast<long>(i)); break; }
  Notifications::onMessageEvicted(t1, MessageStore::FLAG_UNREAD);
  CHECK(stateOf(kBob) == NV::SUMMARY_UNRESOLVED && textBadge() && enigmaBadge(), "no recount on the eviction (append) path");
  notifyTick(1);
  CHECK(!textBadge() && enigmaBadge() && Notifications::getUnreadCount(kG, kBob) == 1, "text %d enigma %d count %u", textBadge(),
        enigmaBadge(), Notifications::getUnreadCount(kG, kBob));

  g_test = "NR10 contract: the stored type decides the category, whatever mask the caller announced";
  reset();
  {
    PacketCodec::MessageEnvelope env = envOf(PacketCodec::MSG_TYPE_ENIGMA);
    uint8_t wire[256];
    size_t n = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>("ENIG"), 4, wire, sizeof(wire));
    MessageRef ref;
    MessageStore::appendStoredMessage(kG, kBob, MessageStore::Direction::RECEIVED, MessageStore::FLAG_UNREAD, env.timestamp, wire,
                                      static_cast<uint16_t>(n), nullptr, 0, &ref);
    Notifications::onMessageArrived(kG, kBob, ref, Notifications::BADGE_TEXT, false);  // mismatched announcement
    CHECK(enigmaBadge() && !textBadge(), "type ENIGMA counted as Enigma: enigma %d text %d", enigmaBadge(), textBadge());
    Notifications::clearUnread(kG, kBob, ref);
    CHECK(!enigmaBadge() && !textBadge() && Notifications::getUnreadCount(kG, kBob) == 0, "cleared: enigma %d text %d", enigmaBadge(),
          textBadge());
  }

  // ---- migration and persistence robustness (review of the first candidate) ----
  static const char* kCarol = "CA201000000001";
  auto putLegacy = [](const char* contact, uint16_t count, uint8_t mask) {
    OldSummary old[24];
    memset(old, 0, sizeof(old));
    old[0].active = true; strcpy(old[0].group_code, kG); strcpy(old[0].contact_key, contact);
    old[0].unread_count = count; old[0].badge_mask = mask;
    g_notifyPrefs.putBytes("summary", old, sizeof(old));
  };

  g_test = "NR11 legacy record unreadable at boot: not migrated, summary2 not written; recovery migrates and merges; reboot keeps it";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  g_notifyPrefs.kv.clear();
  putLegacy(kBob, 2, Notifications::BADGE_TEXT | Notifications::BADGE_ENIGMA);
  std::string legacyBytes = g_notifyPrefs.kv["summary"];
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary") == 0; };
  reboot();
  CHECK(NV::g_loadState == NV::LOAD_RETRY_LEGACY && !g_notifyPrefs.isKey("summary2"),
        "read failure taken as an empty migration: state %d summary2 %d", NV::g_loadState, g_notifyPrefs.isKey("summary2"));
  MessageRef t2 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT2");  // arrives while the record is unreadable
  CHECK(textBadge() && !g_notifyPrefs.isKey("summary2"), "arrival tracked in RAM only: badge %d summary2 %d", textBadge(),
        g_notifyPrefs.isKey("summary2"));
  notifyTick(2);  // still failing
  CHECK(NV::g_loadState == NV::LOAD_RETRY_LEGACY && !g_notifyPrefs.isKey("summary2"), "still retrying");
  g_notifyPrefs.failRead = nullptr;  // storage recovers
  notifyTick(3);  // retry load (migrate + merge), then reconcile
  CHECK(NV::g_loadState == NV::LOAD_OK && stateOf(kBob) == NV::SUMMARY_COUNTED &&
            countOf(kBob, Notifications::SLOT_TEXT) == 2 && countOf(kBob, Notifications::SLOT_ENIGMA) == 1 &&
            g_notifyPrefs.isKey("summary2") && g_notifyPrefs.kv["summary"] == legacyBytes,
        "recovered: state %d text %u enigma %u", stateOf(kBob), countOf(kBob, Notifications::SLOT_TEXT),
        countOf(kBob, Notifications::SLOT_ENIGMA));
  reboot();
  notifyTick(1);  // the loaded count is verified (recounted) once
  CHECK(textBadge() && enigmaBadge() && countOf(kBob, Notifications::SLOT_TEXT) == 2, "after reboot: text %u",
        countOf(kBob, Notifications::SLOT_TEXT));
  Notifications::clearUnread(kG, kBob, t1); Notifications::clearUnread(kG, kBob, t2); Notifications::clearUnread(kG, kBob, e1);
  stayOn();
  reboot();
  CHECK(!textBadge() && !enigmaBadge(), "all read, after reboot: text %d enigma %d", textBadge(), enigmaBadge());

  g_test = "NR12 existing summary2 unreadable: never replaced from the (stale) legacy record; recovery merges";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");  // summary2 now holds Bob: Text 1
  stayOn();
  putLegacy(kCarol, 5, Notifications::BADGE_ENIGMA);  // a stale legacy record also present
  std::string current = g_notifyPrefs.kv["summary2"];
  legacyBytes = g_notifyPrefs.kv["summary"];
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  CHECK(NV::g_loadState == NV::LOAD_RETRY_CURRENT && Notifications::findSummary(kG, kCarol) == nullptr && !enigmaBadge(),
        "legacy data used over summary2: state %d", NV::g_loadState);
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");  // while unreadable
  CHECK(g_notifyPrefs.kv["summary2"] == current, "summary2 overwritten while unreadable");
  g_notifyPrefs.failRead = nullptr;
  notifyTick(3);
  CHECK(NV::g_loadState == NV::LOAD_OK && countOf(kBob, Notifications::SLOT_TEXT) == 1 &&
            countOf(kBob, Notifications::SLOT_ENIGMA) == 1 && stateOf(kBob) == NV::SUMMARY_COUNTED &&
            Notifications::findSummary(kG, kCarol) == nullptr && g_notifyPrefs.kv["summary"] == legacyBytes,
        "merged: text %u enigma %u carol %p", countOf(kBob, Notifications::SLOT_TEXT), countOf(kBob, Notifications::SLOT_ENIGMA),
        static_cast<void*>(Notifications::findSummary(kG, kCarol)));

  g_test = "NR13 mixed Text+Enigma, migration scan fails (UNRESOLVED), reads while unresolved, scan recovers, read all, reboot";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  g_notifyPrefs.kv.clear();
  putLegacy(kBob, 2, Notifications::BADGE_TEXT | Notifications::BADGE_ENIGMA);
  T::scanUnreliable = true;
  reboot();
  CHECK(stateOf(kBob) == NV::SUMMARY_UNRESOLVED && textBadge() && enigmaBadge(), "unresolved, both lit");
  Notifications::clearUnread(kG, kBob, t1);  // the Text message read while scans still fail
  CHECK(stateOf(kBob) == NV::SUMMARY_UNRESOLVED && textBadge() && enigmaBadge(),
        "still unresolved (no guessed decrement): text %d enigma %d", textBadge(), enigmaBadge());
  notifyTick(2);  // still unreliable: nothing changes
  T::scanUnreliable = false;
  // The recount gave way after repeated unreliable listings and waits out its
  // backoff (2 s doubling; 16 s after 20 s of failures) before reconciling.
  CHECK(Notifications::g_recountFailures > 0, "precondition: the recount never gave way");
  notifyTick(3);  // 30 s: past the backoff in force
  CHECK(stateOf(kBob) == NV::SUMMARY_COUNTED && !textBadge() && enigmaBadge(),
        "reconciled: text %d enigma %d (text %u enigma %u game %u)", textBadge(), enigmaBadge(), countOf(kBob, 0), countOf(kBob, 1),
        countOf(kBob, 2));
  Notifications::clearUnread(kG, kBob, e1);
  CHECK(!textBadge() && !enigmaBadge(), "all read: text %d enigma %d", textBadge(), enigmaBadge());
  stayOn();
  reboot();
  CHECK(!textBadge() && !enigmaBadge(), "after reboot: text %d enigma %d", textBadge(), enigmaBadge());

  g_test = "NR14 a failed save is retried; nothing unread is lost once storage recovers (and after reboot)";
  reset();
  g_notifyPrefs.failWrite = [](const char* k) { return strcmp(k, "summary2") == 0; };
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  CHECK(textBadge() && !g_notifyPrefs.isKey("summary2") && NV::g_saveDirty, "badge %d dirty %d", textBadge(),
        NV::g_saveDirty);
  notifyTick(2);
  CHECK(NV::g_saveDirty && textBadge(), "still retrying, still shown");
  g_notifyPrefs.failWrite = nullptr;
  notifyTick(1);
  CHECK(!NV::g_saveDirty && g_notifyPrefs.isKey("summary2"), "saved after recovery");
  reboot();
  CHECK(textBadge(), "after reboot the unread message is still counted");
  Notifications::clearUnread(kG, kBob, t1);
  stayOn();
  reboot();
  CHECK(!textBadge(), "read, after reboot: %d", textBadge());

  g_test = "NR15 legacy record of an unexpected length: never migrated or overwritten, retried, unread arrivals still tracked";
  reset();
  {
    std::string shortRec(100, '\x01');
    g_notifyPrefs.putBytes("summary", shortRec.data(), shortRec.size());
    reboot();
    CHECK(NV::g_loadState == NV::LOAD_RETRY_LEGACY && !g_notifyPrefs.isKey("summary2"), "state %d",
          NV::g_loadState);
    t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
    notifyTick(3);
    CHECK(textBadge() && g_notifyPrefs.kv["summary"] == shortRec && !g_notifyPrefs.isKey("summary2"),
          "badge %d legacy kept %d", textBadge(), g_notifyPrefs.kv["summary"] == shortRec);
  }

  g_test = "NR16 eviction while scans fail: UNRESOLVED (badges kept), reconciled later";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  for (size_t i = 0; i < T::store.size(); i++)
    if (T::store[i].ref.sequence == t1.sequence) { T::store.erase(T::store.begin() + static_cast<long>(i)); break; }
  T::scanUnreliable = true;
  Notifications::onMessageEvicted(t1, MessageStore::FLAG_UNREAD);
  CHECK(stateOf(kBob) == NV::SUMMARY_UNRESOLVED && textBadge() && enigmaBadge(), "unresolved, both kept");
  T::scanUnreliable = false;
  notifyTick(1);
  CHECK(stateOf(kBob) == NV::SUMMARY_COUNTED && !textBadge() && enigmaBadge(), "reconciled: text %d enigma %d",
        textBadge(), enigmaBadge());

  // ---- reads and evictions while the stored record is unreadable, no new arrivals ----
  auto eraseStored = [](const MessageRef& ref) {
    for (size_t i = 0; i < T::store.size(); i++)
      if (T::store[i].ref.sequence == ref.sequence) { T::store.erase(T::store.begin() + static_cast<long>(i)); return; }
  };

  g_test = "NR17 summary2 unreadable, the only unread message read (no RAM summary), recovery, reboot: no badge";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");  // summary2 now holds Bob: Text 1
  stayOn();
  current = g_notifyPrefs.kv["summary2"];
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  CHECK(NV::g_loadState == NV::LOAD_RETRY_CURRENT && Notifications::findSummary(kG, kBob) == nullptr, "state %d",
        NV::g_loadState);
  Notifications::clearUnread(kG, kBob, t1);
  CHECK(!unread(t1) && !textBadge() && g_notifyPrefs.kv["summary2"] == current, "read: flag %d badge %d", unread(t1), textBadge());
  g_notifyPrefs.failRead = nullptr;
  notifyTick(3);  // retry load (merge), then reconcile
  CHECK(NV::g_loadState == NV::LOAD_OK && stateOf(kBob) == NV::SUMMARY_COUNTED && countOf(kBob, Notifications::SLOT_TEXT) == 0 &&
            !textBadge(),
        "recovered: state %d text %u badge %d", stateOf(kBob), countOf(kBob, Notifications::SLOT_TEXT), textBadge());
  reboot();
  CHECK(!textBadge() && !enigmaBadge(), "after reboot: text %d enigma %d (stale count restored)", textBadge(), enigmaBadge());

  g_test = "NR18 summary2 unreadable, the only unread message evicted (no RAM summary), recovery, reboot: no badge";
  reset();
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");  // summary2 now holds Bob: Enigma 1
  stayOn();
  current = g_notifyPrefs.kv["summary2"];
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  eraseStored(e1);
  Notifications::onMessageEvicted(e1, MessageStore::FLAG_UNREAD);
  CHECK(!enigmaBadge() && g_notifyPrefs.kv["summary2"] == current, "evicted: badge %d", enigmaBadge());
  g_notifyPrefs.failRead = nullptr;
  notifyTick(3);
  CHECK(NV::g_loadState == NV::LOAD_OK && stateOf(kBob) == NV::SUMMARY_COUNTED &&
            countOf(kBob, Notifications::SLOT_ENIGMA) == 0 && !enigmaBadge(),
        "recovered: state %d enigma %u", stateOf(kBob), countOf(kBob, Notifications::SLOT_ENIGMA));
  reboot();
  CHECK(!textBadge() && !enigmaBadge(), "after reboot: text %d enigma %d (stale count restored)", textBadge(), enigmaBadge());

  g_test = "NR19 legacy record unreadable, the only unread message read before recovery, recovery, reboot: no badge";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  g_notifyPrefs.kv.clear();
  putLegacy(kBob, 1, Notifications::BADGE_TEXT);
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary") == 0; };
  reboot();
  CHECK(NV::g_loadState == NV::LOAD_RETRY_LEGACY && Notifications::findSummary(kG, kBob) == nullptr, "state %d", NV::g_loadState);
  Notifications::clearUnread(kG, kBob, t1);
  CHECK(!unread(t1) && !textBadge() && !g_notifyPrefs.isKey("summary2"), "read: flag %d badge %d", unread(t1), textBadge());
  g_notifyPrefs.failRead = nullptr;
  T::scanUnreliable = true;  // the migration's own recount fails on the first try too
  notifyTick(1);
  CHECK(NV::g_loadState == NV::LOAD_OK && stateOf(kBob) == NV::SUMMARY_UNRESOLVED, "migrated, unresolved: state %d", stateOf(kBob));
  T::scanUnreliable = false;
  notifyTick(2);
  CHECK(stateOf(kBob) == NV::SUMMARY_COUNTED && !textBadge() && !enigmaBadge(), "reconciled: state %d text %d enigma %d",
        stateOf(kBob), textBadge(), enigmaBadge());
  reboot();
  CHECK(!textBadge() && !enigmaBadge(), "after reboot: text %d enigma %d", textBadge(), enigmaBadge());

  g_test = "NR20 all 24 RAM entries in use while summary2 is unreadable; a read in a stored conversation is still corrected";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");  // summary2: Bob Text 1
  stayOn();
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  {
    char contact[16];
    for (int i = 0; i < 24; i++) {  // 24 other conversations fill every RAM entry
      snprintf(contact, sizeof(contact), "F%011d", i);
      MessageRef ghost{};
      strcpy(ghost.group_code, kG); strcpy(ghost.contact_key, contact); ghost.sequence = 900000u + static_cast<uint32_t>(i);
      Notifications::onMessageArrived(kG, contact, ghost, Notifications::BADGE_ENIGMA, false);
    }
  }
  CHECK(Notifications::findOrCreateSummary(kG, kBob) == nullptr, "precondition: RAM not full");
  Notifications::clearUnread(kG, kBob, t1);  // no RAM entry can record it
  CHECK(!unread(t1), "read");
  g_notifyPrefs.failRead = nullptr;
  notifyTick(30);  // retry load, then recount every listed conversation
  CHECK(NV::g_loadState == NV::LOAD_OK && countOf(kBob, Notifications::SLOT_TEXT) == 0 && !textBadge(),
        "recovered: Bob text %u badge %d", countOf(kBob, Notifications::SLOT_TEXT), textBadge());
  reboot();
  notifyTick(30);
  CHECK(!textBadge(), "after reboot: text %d (stale count restored)", textBadge());

  g_test = "NR21 a conversation the stored record lacks, arriving while it is unreadable, survives recovery and reboot";
  reset();
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));  // a stored record without Bob
  }
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  g_notifyPrefs.failRead = nullptr;
  notifyTick(3);
  CHECK(NV::g_loadState == NV::LOAD_OK && countOf(kBob, Notifications::SLOT_TEXT) == 1 && textBadge(),
        "recovered: Bob text %u badge %d", countOf(kBob, Notifications::SLOT_TEXT), textBadge());
  reboot();
  CHECK(textBadge(), "after reboot: unread arrival lost");
  Notifications::clearUnread(kG, kBob, t1);
  stayOn();
  reboot();
  CHECK(!textBadge(), "read, after reboot: %d", textBadge());

  // ---- reboot BEFORE recovery: RAM-only state is gone, only the stored record remains ----
  for (int evict = 0; evict < 2; evict++) {
    g_test = evict ? "NR23 summary2 unreadable, only unread message evicted, reboot before recovery, then recovery: badge clears"
                   : "NR22 summary2 unreadable, only unread message read, reboot before recovery, then recovery: badge clears";
    reset();
    t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");  // summary2: Bob Text 1 (COUNTED)
    stayOn();
    current = g_notifyPrefs.kv["summary2"];
    g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
    reboot();
    if (evict) {
      eraseStored(t1);
      Notifications::onMessageEvicted(t1, MessageStore::FLAG_UNREAD);
    } else {
      Notifications::clearUnread(kG, kBob, t1);
      CHECK(!unread(t1), "read");
    }
    reboot();  // before any retryLoad or reconcile: RAM lost
    CHECK(NV::g_loadState == NV::LOAD_RETRY_CURRENT && g_notifyPrefs.kv["summary2"] == current, "still unreadable, untouched");
    g_notifyPrefs.failRead = nullptr;  // reads restored; no arrival, no user action from here on
    notifyTick(3);
    CHECK(NV::g_loadState == NV::LOAD_OK && countOf(kBob, Notifications::SLOT_TEXT) == 0 && !textBadge(),
          "stale COUNTED kept: Bob text %u state %d badge %d", countOf(kBob, Notifications::SLOT_TEXT), stateOf(kBob), textBadge());
    reboot();
    notifyTick(3);
    CHECK(!textBadge(), "after reboot: %d", textBadge());
  }

  g_test = "NR24 recovery at boot (stored record readable again on the next boot): the stale count is verified and clears";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  stayOn();
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  Notifications::clearUnread(kG, kBob, t1);
  g_notifyPrefs.failRead = nullptr;
  reboot();  // loads fine at boot: no retryLoad involved
  CHECK(NV::g_loadState == NV::LOAD_OK, "loaded at boot");
  notifyTick(1);
  CHECK(countOf(kBob, Notifications::SLOT_TEXT) == 0 && !textBadge(), "boot-loaded stale count: text %u badge %d",
        countOf(kBob, Notifications::SLOT_TEXT), textBadge());

  g_test = "NR25 verification keeps genuine counts and badges (both categories), with no change while scans fail";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG2");
  stayOn();
  T::scanUnreliable = true;
  reboot();
  CHECK(textBadge() && enigmaBadge(), "lit while verifying");
  notifyTick(3);
  CHECK(textBadge() && enigmaBadge() && stateOf(kBob) == NV::SUMMARY_UNRESOLVED, "unreliable scan: kept lit, unresolved");
  T::scanUnreliable = false;
  notifyTick(1);
  CHECK(stateOf(kBob) == NV::SUMMARY_COUNTED && countOf(kBob, Notifications::SLOT_TEXT) == 1 &&
            countOf(kBob, Notifications::SLOT_ENIGMA) == 2 && textBadge() && enigmaBadge(),
        "verified: text %u enigma %u", countOf(kBob, Notifications::SLOT_TEXT), countOf(kBob, Notifications::SLOT_ENIGMA));

  // ---- arrival while summary2 is unreadable, then reboot BEFORE recovery ----
  g_test = "NR26 conversation absent from summary2: arrival while unreadable, reboot before recovery, recovery: badge recovers";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  Notifications::clearUnread(kG, kBob, t1);
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));  // stored record lists no conversation
  }
  current = g_notifyPrefs.kv["summary2"];
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  MessageRef c1 = arriveFrom(kCarol, PacketCodec::MSG_TYPE_TEXT);
  CHECK(textBadge(), "shown before the reboot");
  reboot();  // RAM lost before any recovery
  CHECK(g_notifyPrefs.kv["summary2"] == current, "unreadable summary2 overwritten");
  g_notifyPrefs.failRead = nullptr;  // no arrival or user action from here on
  notifyTick(4);
  CHECK(NV::g_loadState == NV::LOAD_OK && textBadge() && countOf(kCarol, Notifications::SLOT_TEXT) == 1,
        "arrival lost: badge %d carol text %u", textBadge(), countOf(kCarol, Notifications::SLOT_TEXT));
  reboot();
  notifyTick(2);
  CHECK(textBadge(), "after a second reboot: %d", textBadge());
  Notifications::clearUnread(kG, kCarol, c1);
  CHECK(!textBadge(), "read: %d", textBadge());

  g_test = "NR27 conversation in summary2 with zero stored counts: Enigma arrival while unreadable, reboot before recovery";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  Notifications::clearUnread(kG, kBob, t1);  // summary2: Bob COUNTED, all counts 0
  stayOn();
  CHECK(stateOf(kBob) == NV::SUMMARY_COUNTED && countOf(kBob, Notifications::SLOT_TEXT) == 0, "precondition");
  current = g_notifyPrefs.kv["summary2"];
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  reboot();
  CHECK(g_notifyPrefs.kv["summary2"] == current, "unreadable summary2 overwritten");
  g_notifyPrefs.failRead = nullptr;
  notifyTick(4);
  CHECK(enigmaBadge() && !textBadge() && countOf(kBob, Notifications::SLOT_ENIGMA) == 1,
        "arrival lost: enigma %d text %d (enigma %u)", enigmaBadge(), textBadge(), countOf(kBob, Notifications::SLOT_ENIGMA));
  reboot();
  notifyTick(2);
  CHECK(enigmaBadge(), "after a second reboot");

  g_test = "NR28 conversation in summary2 with another category unread: Enigma arrival while unreadable, reboot before recovery";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");  // summary2: Bob Text 1, Enigma 0
  stayOn();
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  e1 = arrive(PacketCodec::MSG_TYPE_ENIGMA, "ENIG");
  reboot();
  g_notifyPrefs.failRead = nullptr;
  notifyTick(4);
  CHECK(enigmaBadge() && textBadge() && countOf(kBob, Notifications::SLOT_ENIGMA) == 1 && countOf(kBob, Notifications::SLOT_TEXT) == 1,
        "enigma %d text %d (enigma %u text %u)", enigmaBadge(), textBadge(), countOf(kBob, Notifications::SLOT_ENIGMA),
        countOf(kBob, Notifications::SLOT_TEXT));

  g_test = "NR29 25 new conversations while unreadable (more than 24 entries), 24 read, reboot: the 25th is found by the sweep";
  reset();
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));
    g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
    reboot();
    char contact[16];
    std::vector<MessageRef> refs;
    for (int i = 0; i < 25; i++) {
      snprintf(contact, sizeof(contact), "C%011d", i);
      refs.push_back(arriveFrom(contact, PacketCodec::MSG_TYPE_TEXT));
    }
    for (int i = 0; i < 24; i++) Notifications::clearUnread(kG, refs[i].contact_key, refs[i]);  // only the 25th stays unread
    reboot();
    g_notifyPrefs.failRead = nullptr;
    notifyTick(60);
    CHECK(textBadge() && countOf("C00000000024", Notifications::SLOT_TEXT) == 1, "overflowed arrival lost: badge %d count %u",
          textBadge(), countOf("C00000000024", Notifications::SLOT_TEXT));
    reboot();
    notifyTick(60);
    CHECK(textBadge(), "after a second reboot");
  }

  g_test = "NR30 arrival while unreadable, reboot, recovery with pending2 unreadable (a leftover key is never needed)";
  reset();
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));
  }
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  arriveFrom(kCarol, PacketCodec::MSG_TYPE_TEXT);
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0 || strcmp(k, "pending2") == 0; };
  reboot();
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "pending2") == 0; };  // summary2 readable again
  notifyTick(10);
  CHECK(textBadge() && countOf(kCarol, Notifications::SLOT_TEXT) == 1, "badge %d count %u", textBadge(),
        countOf(kCarol, Notifications::SLOT_TEXT));
  g_notifyPrefs.failRead = nullptr;
  reboot();
  notifyTick(10);
  CHECK(textBadge(), "after reboot");

  g_test = "NR31 recovery whose summary2 save fails: counted in RAM meanwhile; a reboot then still recovers the badge";
  reset();
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));
  }
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  arriveFrom(kCarol, PacketCodec::MSG_TYPE_TEXT);
  reboot();
  g_notifyPrefs.failRead = nullptr;
  g_notifyPrefs.failWrite = [](const char* k) { return strcmp(k, "summary2") == 0; };
  notifyTick(4);
  CHECK(textBadge(), "recovered in RAM");
  reboot();  // the recovered state was never saved
  g_notifyPrefs.failWrite = nullptr;
  notifyTick(4);
  CHECK(textBadge() && countOf(kCarol, Notifications::SLOT_TEXT) == 1, "after reboot: badge %d count %u", textBadge(),
        countOf(kCarol, Notifications::SLOT_TEXT));

  g_test = "NR32 arrivals over two unreadable boots (pending2 unreadable in the second), then recovery: both found";
  reset();
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));
  }
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  arriveFrom(kCarol, PacketCodec::MSG_TYPE_TEXT);  // pending: Carol
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0 || strcmp(k, "pending2") == 0; };
  reboot();
  arriveFrom("DA7E00000001", PacketCodec::MSG_TYPE_ENIGMA);  // pending unreadable: overwritten (Carol's entry lost)
  g_notifyPrefs.failRead = nullptr;
  reboot();
  notifyTick(10);
  CHECK(textBadge() && enigmaBadge() && countOf(kCarol, Notifications::SLOT_TEXT) == 1, "carol lost: text %d enigma %d",
        textBadge(), enigmaBadge());

  g_test = "NR33 arrival while unreadable, no reboot: the RAM merge lights the badge at the recovery itself";
  reset();
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));
  }
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  g_notifyPrefs.failWrite = [](const char* k) { return strcmp(k, "pending2") == 0; };
  reboot();
  arriveFrom(kCarol, PacketCodec::MSG_TYPE_TEXT);
  CHECK(!g_notifyPrefs.isKey("pending2"), "precondition: pending not written");
  g_notifyPrefs.failRead = nullptr;
  g_notifyPrefs.failWrite = nullptr;
  T::listUnreliable = true;  // and no listing works yet: only the RAM merge can show it
  notifyTick(1);  // the retry load merges RAM: lit at once
  CHECK(textBadge(), "lit right after the merge: %d", textBadge());
  T::listUnreliable = false;
  notifyTick(2);
  CHECK(countOf(kCarol, Notifications::SLOT_TEXT) == 1, "counted: %u", countOf(kCarol, Notifications::SLOT_TEXT));

  // ---- recovery that must not depend on a pending marker ever being saved ----
  g_test = "NR34 Bob listed with zero counts, Bob arrival while summary2 unreadable, reboot, summary2 readable, pending2 unreadable";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  Notifications::clearUnread(kG, kBob, t1);  // summary2: Bob COUNTED, all zero
  stayOn();
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  MessageRef b2 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT2");
  reboot();
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "pending2") == 0; };
  notifyTick(10);
  CHECK(textBadge() && countOf(kBob, Notifications::SLOT_TEXT) == 1, "zero-count conversation skipped: badge %d text %u",
        textBadge(), countOf(kBob, Notifications::SLOT_TEXT));
  g_notifyPrefs.failRead = nullptr;
  reboot();
  notifyTick(10);
  CHECK(textBadge(), "after reboot: %d", textBadge());
  Notifications::clearUnread(kG, kBob, b2);
  CHECK(!textBadge(), "read: %d", textBadge());

  g_test = "NR35 pending2 not writable, arrival while summary2 unreadable, reboot before recovery, recovery";
  reset();
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));
  }
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  g_notifyPrefs.failWrite = [](const char* k) { return strcmp(k, "pending2") == 0; };
  reboot();
  arriveFrom(kCarol, PacketCodec::MSG_TYPE_ENIGMA);
  reboot();
  g_notifyPrefs.failRead = nullptr;
  g_notifyPrefs.failWrite = nullptr;
  notifyTick(10);
  CHECK(enigmaBadge() && countOf(kCarol, Notifications::SLOT_ENIGMA) == 1, "lost: badge %d enigma %u", enigmaBadge(),
        countOf(kCarol, Notifications::SLOT_ENIGMA));

  g_test = "NR36 power lost between storing the message and recording it (summary2 unreadable, new conversation), then recovery";
  reset();
  {
    Notifications::Summary none[24];
    memset(none, 0, sizeof(none));
    g_notifyPrefs.putBytes("summary2", none, sizeof(none));
  }
  g_notifyPrefs.failRead = [](const char* k) { return strcmp(k, "summary2") == 0; };
  reboot();
  storeOnlyFrom(kCarol, PacketCodec::MSG_TYPE_TEXT);
  reboot();
  g_notifyPrefs.failRead = nullptr;
  notifyTick(10);
  CHECK(textBadge() && countOf(kCarol, Notifications::SLOT_TEXT) == 1, "lost: badge %d text %u", textBadge(),
        countOf(kCarol, Notifications::SLOT_TEXT));

  g_test = "NR37 power lost between storing the message and recording it (summary2 readable, Bob listed with zero counts)";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  Notifications::clearUnread(kG, kBob, t1);
  storeOnlyFrom(kBob, PacketCodec::MSG_TYPE_ENIGMA);
  reboot();
  notifyTick(10);
  CHECK(enigmaBadge() && countOf(kBob, Notifications::SLOT_ENIGMA) == 1, "lost: badge %d enigma %u", enigmaBadge(),
        countOf(kBob, Notifications::SLOT_ENIGMA));

  g_test = "NR38 the sweep meets unreliable listings/recounts: it retries (never skips or ends early) until they are reliable";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  Notifications::clearUnread(kG, kBob, t1);
  storeOnlyFrom(kCarol, PacketCodec::MSG_TYPE_TEXT);  // power lost before it was counted
  T::scanUnreliable = true;  // phase 1: every recount unreliable
  reboot();
  notifyTick(6);
  CHECK(!textBadge(), "precondition: nothing reliable yet (badge %d)", textBadge());
  T::scanUnreliable = false;
  T::listUnreliable = true;  // phase 2: the listing itself fails
  notifyTick(6);
  CHECK(!textBadge(), "precondition: listing failed (badge %d)", textBadge());
  T::listUnreliable = false;
  notifyTick(6);
  CHECK(textBadge() && countOf(kCarol, Notifications::SLOT_TEXT) == 1, "skipped: badge %d text %u", textBadge(),
        countOf(kCarol, Notifications::SLOT_TEXT));

  g_test = "NR39 an UNRESOLVED recount whose listings stay unreliable backs off (bounded retries), then reconciles";
  reset();
  t1 = arrive(PacketCodec::MSG_TYPE_TEXT, "TEXT");
  g_notifyPrefs.kv.clear();
  putLegacy(kBob, 1, Notifications::BADGE_TEXT);
  T::scanUnreliable = true;
  reboot();
  CHECK(stateOf(kBob) == NV::SUMMARY_UNRESOLVED, "precondition: unresolved");
  T::listings = 0;
  notifyTick(12);  // 120 s of unreliable listings
  CHECK(T::listings < 150, "%u listings in 120 s (no backoff: one per 25 ms step)", T::listings);
  T::scanUnreliable = false;
  notifyTick(7);  // past the longest backoff (60 s)
  CHECK(stateOf(kBob) == NV::SUMMARY_COUNTED && textBadge(), "not reconciled: state %d badge %d", stateOf(kBob), textBadge());

  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
