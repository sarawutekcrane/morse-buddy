// HOST TEST (not production). Unread marking against the REAL message store:
// the ACTUAL storage_messages.cpp on a real host directory (fake_fs/, every
// operation counted), with the ACTUAL menu.cpp, notifications.cpp,
// text_message.cpp and enigma.cpp. Flash truth (each record's FLAG_UNREAD) is
// read back through the real store, never through a fake.
//
// Covers hardware findings 1 and 3 on 23293c5 (legacy unread flags, reboot
// and failure recovery, bounded work per main-loop pass) and, from round 5,
// the per-message read that replaced "read up to the selection": moving the
// cursor onto a message reads exactly that message (pm_tests.inc).
#include "stubs.h"
#include "core/menu.cpp"
#include "core/notifications.cpp"
#include "core/text_message.cpp"
#include "core/input_queue.h"
#include "core/outbox.cpp"  // in this translation unit, so a reboot also resets its RAM state
#include "core/enigma.h"
#include "core/storage_messages.h"
#include "LittleFS.h"
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include "legacy_store.h"
#include <filesystem>
#include <algorithm>
#include <deque>
#include <tuple>

// Allocation-failure injection: while set, every nothrow array new fails
// (the held-input buffer, the record cache), as on a device out of heap.
static bool g_failNothrowArrayNew = false;
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  if (g_failNothrowArrayNew) return nullptr;
  try {
    return ::operator new[](n);  // paired with the ordinary delete[]
  } catch (...) {
    return nullptr;
  }
}

namespace Sleep { void notifyActivity() {} }
void playTone(uint16_t, uint16_t, SoundClass) {}
static Preferences g_notifyPrefs;
namespace Storage { Preferences& notify() { return g_notifyPrefs; } }
void T_initServices();
void T_tickServices();

static int g_fail = 0, g_pass = 0;
static std::string g_test;
#define CHECK(c, ...)                                                        \
  do {                                                                       \
    if (c) { g_pass++; } else {                                              \
      g_fail++; printf("FAIL [%s] line %d: ", g_test.c_str(), __LINE__);     \
      printf(__VA_ARGS__); printf("\n");                                     \
    }                                                                        \
  } while (0)

static const char* kG = "GROUP1";
static const char* kBob = "B0B000000001";
static const char* kCarol = "CA201000000001";
static uint32_t g_msgNo = 0;

// ---- pending reads (RAM only) ---------------------------------------------
// -DNOTIFY_READUP builds these tests against the read-up firmware (511d13e)
// for before/after evidence: its pending state was the read-up list.
#ifdef NOTIFY_READUP
static int pendingReads() {
  int n = 0;
  if (Notifications::g_readUps != nullptr)
    for (uint8_t i = 0; i < Notifications::g_readUps->count; i++) n += !Notifications::g_readUps->items[i].done;
  return n;
}
static int pendingFailures() {
  if (Notifications::g_readUps != nullptr)
    for (uint8_t i = 0; i < Notifications::g_readUps->count; i++)
      if (!Notifications::g_readUps->items[i].done) return Notifications::g_readUps->items[i].failures;
  return 0;
}
static void dropPendingReads() {
  delete Notifications::g_readUps;
  Notifications::g_readUps = nullptr;
  Notifications::g_readUpsDirty = false;
  Notifications::g_readUpsDueMs = 0;
}
#else
static int pendingReads() { return Notifications::g_reads != nullptr ? Notifications::g_reads->count : 0; }
static int pendingFailures() {
  return Notifications::g_reads != nullptr && Notifications::g_reads->count > 0 ? Notifications::g_reads->items[0].failures : 0;
}
static void dropPendingReads() {
  delete Notifications::g_reads;
  Notifications::g_reads = nullptr;
}
#endif

// ---- volatile state of the notification service (a reboot loses it) -------
static void resetNotifyVolatile() {
#ifdef NOTIFY_HEAD  // 23293c5's notifications.cpp
  delete Notifications::g_sweep;
  Notifications::g_sweep = nullptr;
  Notifications::g_reconcileCursor = 0;
#else
  if (Notifications::g_sweep != nullptr) MessageStore::endConversationWalk(Notifications::g_sweep->walk);
  delete Notifications::g_sweep;
  Notifications::g_sweep = nullptr;
  Notifications::g_recountRetryMs = 0;
  Notifications::g_recountFailures = 0;
  delete Notifications::g_job;
  Notifications::g_job = nullptr;
  dropPendingReads();  // RAM only
  Notifications::g_reconcileCursor = 0;
  Notifications::g_nextStepMs = T::now;
  Notifications::g_saveDueMs = 0;
#endif
  memset(Notifications::g_summaries, 0, sizeof(Notifications::g_summaries));
  Notifications::g_loadState = Notifications::LOAD_OK;
  Notifications::g_saveDirty = false;
  Notifications::g_nextRetryMs = T::now;
  Notifications::g_raceInvitePending = false;
}

static PacketCodec::MessageEnvelope envOf(const char* sender, uint8_t type, uint32_t ts) {
  PacketCodec::MessageEnvelope env{};
  snprintf(env.message_id, sizeof(env.message_id), "MSG%020u", ++g_msgNo);
  env.schema_version = 1;
  env.message_type = type;
  strncpy(env.sender_device_id, sender, sizeof(env.sender_device_id) - 1);
  strncpy(env.sender_name_cache, "Bob", sizeof(env.sender_name_cache) - 1);
  strncpy(env.group_code, kG, sizeof(env.group_code) - 1);
  env.timestamp = ts;
  return env;
}

// A record exactly as an older firmware left it: stored, never counted. Written
// in the on-flash record format directly (legacy_store.h), so a conversation
// can hold up to the earlier 300-record retention.
static MessageRef storeOnly(const char* contact, uint8_t type, bool received, uint32_t ts) {
  PacketCodec::MessageEnvelope env = envOf(received ? contact : "DEADBEEFAAAA", type, ts);
  return writeLegacyRecord(kG, contact, received ? MessageStore::Direction::RECEIVED : MessageStore::Direction::SENT,
                           received ? MessageStore::FLAG_UNREAD : 0, ts, env, "stored message body");
}

// A live arrival: appended through the store (retention applies) and counted
// as the arrival handlers do.
static MessageRef arrive(const char* contact, uint8_t type, uint32_t ts) {
  PacketCodec::MessageEnvelope env = envOf(contact, type, ts);
  const char* body = "stored message body";
  uint8_t wire[256];
  size_t n = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>(body), static_cast<uint16_t>(strlen(body)),
                                              wire, sizeof(wire));
  MessageRef ref{};
  MessageStore::appendStoredMessage(kG, contact, MessageStore::Direction::RECEIVED, MessageStore::FLAG_UNREAD, ts, wire,
                                    static_cast<uint16_t>(n), nullptr, 0, &ref);
  uint8_t mask = type == PacketCodec::MSG_TYPE_ENIGMA ? Notifications::BADGE_ENIGMA
               : type == PacketCodec::MSG_TYPE_GAME   ? (Notifications::BADGE_TEXT | Notifications::BADGE_ENIGMA)
                                                      : Notifications::BADGE_TEXT;
  Notifications::onMessageArrived(kG, contact, ref, mask, false);
  return ref;
}

// ---- flash truth, through the real store ----------------------------------
struct TypeCount { int text = 0, enigma = 0, game = 0, files = 0; };
static TypeCount truth(const char* contact) {
  TypeCount t;
  bool reliable = false;
  MessageStore::findMessagesByPredicate(
      kG, contact,
      [](const MessageStore::StoredHeader& h, const PacketCodec::MessageEnvelope& e, void* p) {
        TypeCount* c = static_cast<TypeCount*>(p);
        c->files++;
        if (h.direction == MessageStore::Direction::RECEIVED && (h.flags & MessageStore::FLAG_UNREAD)) {
          if (e.message_type == PacketCodec::MSG_TYPE_TEXT) c->text++;
          if (e.message_type == PacketCodec::MSG_TYPE_ENIGMA) c->enigma++;
          if (e.message_type == PacketCodec::MSG_TYPE_GAME) c->game++;
        }
        return false;
      },
      &t, nullptr, 0, &reliable);
  return t;
}
static bool isUnread(const MessageRef& ref) {
  StoredMessageView v;
  return MessageStore::loadMessage(ref, &v) && (v.header.flags & MessageStore::FLAG_UNREAD);
}
// A TEXT record as an older firmware left it, with its text (shown in the chat).
static MessageRef storeText(const char* contact, const char* sender, bool received, bool unread, uint32_t ts, const char* text) {
  PacketCodec::MessageEnvelope env = envOf(sender, PacketCodec::MSG_TYPE_TEXT, ts);
  uint8_t payload[96];
  size_t pn = PacketCodec::encodeTextPayload(text, payload, sizeof(payload));
  return writeLegacyRecordBytes(kG, contact, received ? MessageStore::Direction::RECEIVED : MessageStore::Direction::SENT,
                                unread ? MessageStore::FLAG_UNREAD : 0, ts, env, payload, static_cast<uint16_t>(pn));
}
// A pending-outbox record (sent, not yet published): never evicted.
static MessageRef storePending(const char* contact, uint32_t ts) {
  PacketCodec::MessageEnvelope env = envOf("DEADBEEFAAAA", PacketCodec::MSG_TYPE_TEXT, ts);
  uint8_t wire[256];
  size_t n = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>("OUT"), 3, wire, sizeof(wire));
  MessageRef ref{};
  MessageStore::appendStoredMessage(kG, contact, MessageStore::Direction::SENT, MessageStore::FLAG_PENDING_OUTBOX, ts, wire,
                                    static_cast<uint16_t>(n), nullptr, 0, &ref);
  return ref;
}
// The records on "flash" (host directory), read directly: sequence -> bytes.
static std::map<uint32_t, std::string> onFlash(const char* group, const char* contact) {
  std::map<uint32_t, std::string> out;
  std::error_code ec;
  std::string dir = LittleFS.root + "/messages/" + group + "/" + contact;
  for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
    std::string name = e.path().filename().string();
    if (name.size() < 5 || name.compare(name.size() - 4, 4, ".msg") != 0) continue;
    FILE* f = fopen(e.path().c_str(), "rb");
    std::string bytes;
    int ch;
    while (f && (ch = fgetc(f)) != EOF) bytes.push_back(static_cast<char>(ch));
    if (f) fclose(f);
    out[static_cast<uint32_t>(strtoul(name.c_str(), nullptr, 10))] = bytes;
  }
  return out;
}
static bool textBadge() { return Notifications::hasAnyUnreadText(); }
static bool enigmaBadge() { return Notifications::hasAnyUnreadEnigma(); }

// ---- the main loop, with per-pass work recorded ---------------------------
struct PassMax {
  uint64_t opens = 0, dirEntries = 0, writes = 0, renames = 0, removes = 0;
  int nvs = 0;
  void add(const FsStats& a, const FsStats& b, int nvsA, int nvsB) {
    removes = std::max<uint64_t>(removes, b.removes - a.removes);
    opens = std::max<uint64_t>(opens, b.fileOpens - a.fileOpens);
    dirEntries = std::max<uint64_t>(dirEntries, b.dirEntries - a.dirEntries);
    writes = std::max<uint64_t>(writes, b.writes - a.writes);
    renames = std::max<uint64_t>(renames, b.renames - a.renames);
    nvs = std::max(nvs, nvsB - nvsA);
  }
};
static PassMax g_svcMax, g_uiMax;
static void loopPass(int n = 1) {
  for (int i = 0; i < n; i++) {
    T::now += 20;
    FsStats a = g_fs; int na = g_notifyPrefs.writes;
    T_tickServices();
    FsStats b = g_fs; int nb = g_notifyPrefs.writes;
    Menu::tick();
    FsStats c = g_fs; int nc = g_notifyPrefs.writes;
    g_svcMax.add(a, b, na, nb);
    g_uiMax.add(b, c, nb, nc);
  }
}
static void runFor(int seconds) { loopPass(seconds * 1000 / 20); }
// Chat entry, pass by pass: the largest number of file opens one main-loop
// pass made (services and UI together), the opens in total, and the passes
// until `expect` was on screen (-1: never, within `limit` passes).
struct EntryProfile {
  uint64_t maxPassOpens = 0, totalOpens = 0;
  int passesToContent = -1;
};
// The chat's index as the boot sweep has not reached the conversation yet:
// nothing cached (an external mutation drops every cache).
static void coldIndex() { MessageStore::notifyExternalStorageMutationAttempted(); }
static EntryProfile enterChat(bool enigma, const char* contact, const char* expect, int limit = 600) {
  EntryProfile p;
  FsStats start = g_fs;
  if (enigma) Enigma::navigateToChatDirect(kG, contact);
  else TextMessage::navigateToChatDirect(kG, contact);
  for (int i = 1; i <= limit; i++) {
    FsStats a = g_fs;
    loopPass();
    p.maxPassOpens = std::max<uint64_t>(p.maxPassOpens, g_fs.fileOpens - a.fileOpens);
    std::string vis = T::visibleJoined();
    if (vis.find("Loading messages") == std::string::npos && vis.find(expect) != std::string::npos) {
      p.passesToContent = i;
      break;
    }
  }
  p.totalOpens = g_fs.fileOpens - start.fileOpens;
  return p;
}
static void rot(int8_t v) {
  T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, v, 0, 0));
  loopPass();
}
static void back() {
  T::now += 500;
  T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
  loopPass(3);
}
// Opens a chat and waits until it is drawn (its index may load over several
// passes; input given while "Loading messages..." shows is held).
static void waitChatDrawn() {
  loopPass(3);
  for (int i = 0; i < 3000 && T::visibleJoined().find("Loading messages") != std::string::npos; i++) loopPass();
  loopPass();
}
static void openText(const char* contact) { TextMessage::navigateToChatDirect(kG, contact); waitChatDrawn(); }
static void openEnigma(const char* contact) { Enigma::navigateToChatDirect(kG, contact); waitChatDrawn(); }

static void menuStub() {}
// The Outbox's RAM state (a reboot loses it).
static void resetOutboxVolatile() {
  delete Outbox::g_queue;
  Outbox::g_queue = nullptr;
  Outbox::g_queueOverflowed = false;
  Outbox::g_publishRetryAtMs = 0;
  if (Outbox::g_pass != nullptr) MessageStore::endConversationWalk(Outbox::g_pass->walk);
  delete Outbox::g_pass;
  Outbox::g_pass = nullptr;
  Outbox::g_passDone = false;
  Outbox::g_passAgain = false;
  Outbox::g_passAgainAtMs = 0;
  Outbox::g_nextStepMs = 0;
#ifndef OUTBOX_2FDE185  // the reviewed candidate's outbox.cpp lacks these
  Outbox::g_passRetryMs = Outbox::kRetryMs;
  Outbox::g_externalAtPassStart = 0;
#else
  Outbox::g_externalSeen = 0;
#endif
}
static void powerOn() {  // first boot of a test: empty NVS unless the test wrote some
  resetNotifyVolatile();
  resetOutboxVolatile();
  MessageStore::notifyExternalStorageMutationAttempted();
  Menu::init(); g_stackSize = 1; Menu::consumeJustEntered();
  Notifications::serviceInit();
  Menu::pushScreen(menuStub);
  g_svcMax = PassMax(); g_uiMax = PassMax();
}
static void reboot() { powerOn(); }
static void freshDevice() {
  LittleFS.failOpen = nullptr;
  LittleFS.failRemove = nullptr;
  LittleFS.failRename = nullptr;
  LittleFS.onFileOpen = nullptr;
  LittleFS.reverseListing = false;
  LittleFS.wipe();
  MessageStore::notifyExternalStorageMutationAttempted();  // a wiped device: nothing cached survives (as after the reboot that follows)
  T::reset();
  g_notifyPrefs.kv.clear(); g_notifyPrefs.failRead = nullptr; g_notifyPrefs.failWrite = nullptr; g_notifyPrefs.failRemove = nullptr;
  MessageStore::init();
  g_msgNo = 0;
}

int main(int argc, char** argv) {
  LittleFS.root = argc > 1 ? argv[1] : "/tmp/storage_test_fs";
  T::reset();
  T_initServices();  // registers the services' hooks once (as setup() does)

  g_test = "ST1 real store: an arrival is flagged on flash, read by selection, stays read after reboot";
  freshDevice(); powerOn();
  MessageRef t1 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000100u);
  CHECK(isUnread(t1) && textBadge(), "arrival: flash unread %d badge %d", isUnread(t1), textBadge());
  openText(kBob);
  CHECK(isUnread(t1), "entering the chat alone read it");
  rot(-1);
  runFor(5);
  CHECK(!isUnread(t1) && !textBadge(), "after selection: flash unread %d badge %d", isUnread(t1), textBadge());
  back(); reboot(); runFor(30);
  CHECK(!isUnread(t1) && !textBadge(), "after reboot: flash unread %d badge %d", isUnread(t1), textBadge());

  g_test = "ST2 legacy flags: selecting the newest received message reads that message only (round 5; was: every older one of its type)";
  freshDevice();
  for (int i = 0; i < 60; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, i % 5 != 4, 1700000000u + i * 60);
  powerOn(); runFor(60);
  CHECK(textBadge() && truth(kBob).text == 48, "legacy: badge %d unread %d", textBadge(), truth(kBob).text);
  openText(kBob);
  rot(-1);  // newest entry is a sent message: nothing to read
  CHECK(truth(kBob).text == 48, "a sent message selected read something");
  rot(-1);  // newest received message
  runFor(30);
  CHECK(truth(kBob).text == 47 && truth(kBob).files == 60, "after selecting the newest: unread %d files %d", truth(kBob).text,
        truth(kBob).files);
  CHECK(pendingReads() == 0, "a fault-free read is still pending");
  back();
  CHECK(textBadge() && Notifications::getUnreadCount(kG, kBob) == 47, "badge %d count %u", textBadge(), Notifications::getUnreadCount(kG, kBob));
  reboot(); runFor(60);
  CHECK(textBadge() && truth(kBob).text == 47 && Notifications::getUnreadCount(kG, kBob) == 47, "after reboot: badge %d unread %d count %u",
        textBadge(), truth(kBob).text, Notifications::getUnreadCount(kG, kBob));

  g_test = "ST3 the messages moved onto are read, nothing older and no other type; newer messages stay unread (round 5; was: read up to the selection)";
  freshDevice(); powerOn();
  MessageRef a1 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000100u);
  MessageRef e1 = arrive(kBob, PacketCodec::MSG_TYPE_ENIGMA, 1700000200u);
  MessageRef g1 = arrive(kBob, PacketCodec::MSG_TYPE_GAME, 1700000300u);
  MessageRef a2 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000400u);
  MessageRef a3 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000500u);
  openText(kBob);
  rot(-1); rot(-1);  // a3, then a2 selected
  runFor(10);
  CHECK(isUnread(a1) && !isUnread(a2), "a1 (older, not moved onto) %d, a2 (moved onto) %d", isUnread(a1), isUnread(a2));
  CHECK(isUnread(e1) && isUnread(g1), "other types read: enigma %d game %d", isUnread(e1), isUnread(g1));
  back();
  CHECK(textBadge() && enigmaBadge(), "badges: text %d enigma %d", textBadge(), enigmaBadge());
  CHECK(!isUnread(a3), "a3 was itself selected (rotated onto) and should be read");
  // a newer Text arrives later: it must stay unread
  MessageRef a4 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000600u);
  // a later arrival with an OLDER timestamp (shown above a2) must stay unread too
  MessageRef a0 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000050u);
  runFor(20);
  CHECK(isUnread(a4) && isUnread(a0), "later arrivals read: newer %d older-timestamp %d", isUnread(a4), isUnread(a0));
  openEnigma(kBob);
  rot(-1);  // the newest entry (a4, Text): selecting it in Enigma reads Text, not Enigma
  runFor(10);
  CHECK(isUnread(e1) && isUnread(g1), "selecting a Text message read enigma %d game %d", isUnread(e1), isUnread(g1));
  back();

  g_test = "ST3b two moves in one batch read both messages moved onto, the one passed over included (round 5; was: it stayed unread)";
  freshDevice(); powerOn();
  MessageRef below = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000900u);  // stored first, newest timestamp
  MessageRef sel = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000500u);    // stored second, shown above it
  openText(kBob);
  T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
  rot(-1);  // two detents in one batch: passes over `below`, selects `sel`
  runFor(10);
  CHECK(!isUnread(sel) && !isUnread(below), "selected %d passed over %d", isUnread(sel), isUnread(below));
  back();

  g_test = "ST3c stored after the selection but shown above it, while the selection's read is still pending: stays unread";
  freshDevice(); powerOn();
  MessageRef s1 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000500u);
  openText(kBob);
  rot(-1);  // selects s1; its read is queued, not yet applied
  MessageRef late = storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000100u);  // older timestamp, stored later
  runFor(10);
  CHECK(!isUnread(s1) && isUnread(late), "selected %d later-stored %d", isUnread(s1), isUnread(late));
  back();

  g_test = "ST4 entering a chat and leaving clears nothing";
  freshDevice();
  for (int i = 0; i < 20; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_ENIGMA, true, 1700000000u + i * 60);
  powerOn(); runFor(30);
  openEnigma(kCarol); runFor(10); back(); runFor(10);
  openText(kCarol); runFor(10); back(); runFor(10);
  CHECK(truth(kCarol).enigma == 20 && enigmaBadge(), "entering cleared: unread %d badge %d", truth(kCarol).enigma, enigmaBadge());

  g_test = "ST5 reboot after a move: the one message read stays read; nothing else is read, nothing resumed (round 5; was: a read-up resumed)";
  freshDevice();
  for (int i = 0; i < 90; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn(); runFor(5);
  openText(kBob);
  rot(-1);
  loopPass(150);  // 3 s: applied
  CHECK(truth(kBob).text == 89, "precondition: %d unread (expected 149)", truth(kBob).text);
  reboot(); runFor(120);
  CHECK(truth(kBob).text == 89 && textBadge() && Notifications::getUnreadCount(kG, kBob) == 89, "after reboot: unread %d badge %d count %u",
        truth(kBob).text, textBadge(), Notifications::getUnreadCount(kG, kBob));

  g_test = "ST6 UI work per rotation is bounded: no file rewrite or NVS write during the encoder pass";
  freshDevice();
  for (int i = 0; i < 300; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn();  // right after boot: every listed conversation still awaits its recount
  openText(kBob);
  g_uiMax = PassMax();
  for (int i = 0; i < 20; i++) rot(-1);
  CHECK(g_uiMax.writes == 0 && g_uiMax.renames == 0 && g_uiMax.nvs == 0 && g_uiMax.opens <= 24,
        "per rotation pass: opens %llu writes %llu renames %llu nvs %d", (unsigned long long)g_uiMax.opens,
        (unsigned long long)g_uiMax.writes, (unsigned long long)g_uiMax.renames, g_uiMax.nvs);

  g_test = "ST7 background work per main-loop pass is bounded, large history (8 x 300); counts converge to flash truth";
  freshDevice();
  char contact[16];
  for (int c = 0; c < 8; c++) {
    snprintf(contact, sizeof(contact), "C%011d", c);
    for (int i = 0; i < 300; i++) storeOnly(contact, c % 2 ? PacketCodec::MSG_TYPE_ENIGMA : PacketCodec::MSG_TYPE_TEXT, i % 3 != 0,
                                            1700000000u + i * 60);
  }
  powerOn();
  g_svcMax = PassMax();
  runFor(600);
  CHECK(g_svcMax.opens <= 8 && g_svcMax.renames <= 1 && g_svcMax.nvs <= 1 && g_svcMax.dirEntries <= 300,
        "per service pass: opens %llu dirEntries %llu writes %llu renames %llu nvs %d", (unsigned long long)g_svcMax.opens,
        (unsigned long long)g_svcMax.dirEntries, (unsigned long long)g_svcMax.writes, (unsigned long long)g_svcMax.renames,
        g_svcMax.nvs);
  bool exact = true;
  for (int c = 0; c < 8; c++) {
    snprintf(contact, sizeof(contact), "C%011d", c);
    TypeCount t = truth(contact);
    exact = exact && Notifications::getUnreadCount(kG, contact) == t.text + t.enigma + t.game;
  }
  CHECK(exact && textBadge() && enigmaBadge(), "counts not equal to flash truth after 10 min");

  g_test = "ST8 NVS writes failing: the read is still applied to flash, and stays applied after reboot";
  freshDevice();
  for (int i = 0; i < 40; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn(); runFor(10);
  g_notifyPrefs.failWrite = [](const char*) { return true; };
  openText(kBob); rot(-1); runFor(60);
  CHECK(truth(kBob).text == 39, "with NVS failing: unread %d (expected 39)", truth(kBob).text);
  back();
  reboot(); runFor(60);
  g_notifyPrefs.failWrite = nullptr;
  runFor(30);
  CHECK(truth(kBob).text == 39 && textBadge() && Notifications::getUnreadCount(kG, kBob) == 39, "after reboot: unread %d badge %d count %u",
        truth(kBob).text, textBadge(), Notifications::getUnreadCount(kG, kBob));

  g_test = "ST9 unreliable directory: background recount retries, then converges";
  freshDevice();
  for (int i = 0; i < 30; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  LittleFS.failOpen = [](const std::string& p) { return p.find(kCarol) != std::string::npos; };
  powerOn(); runFor(60);
  LittleFS.failOpen = nullptr;
  runFor(120);
  CHECK(Notifications::getUnreadCount(kG, kCarol) == 30 && textBadge(), "after the directory recovered: count %u badge %d",
        Notifications::getUnreadCount(kG, kCarol), textBadge());

  g_test = "ST10 an arrival during the conversation's background recount is counted exactly";
  freshDevice();
  for (int i = 0; i < 90; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);  // below the 100 retained: no eviction
  powerOn();
  loopPass(10);
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
  runFor(120);
  CHECK(Notifications::getUnreadCount(kG, kBob) == truth(kBob).text && truth(kBob).text == 91,
        "count %u flash %d", Notifications::getUnreadCount(kG, kBob), truth(kBob).text);

  g_test = "ST11 a read's record rewrite fails five times: retried with backoff until written; only that message read (round 5; was: a read-up's passes)";
  freshDevice();
  for (int i = 0; i < 20; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn(); runFor(30);
  {
    static int tmpFailures = 0;
    tmpFailures = 0;
    LittleFS.failOpen = [](const std::string& p) {  // the first 5 temp-file writes fail
      return p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0 && tmpFailures++ < 5;
    };
    openText(kBob); rot(-1); runFor(90);  // retries after 2, 4, 8, 16 and 32 s
    LittleFS.failOpen = nullptr;
    CHECK(tmpFailures >= 5 && truth(kBob).text == 19 && Notifications::getUnreadCount(kG, kBob) == 19,
          "left unread %d, count %u (write failures %d)", truth(kBob).text, Notifications::getUnreadCount(kG, kBob), tmpFailures);
    back();
  }

  g_test = "ST12 a recount already under way when the last unread message is read does not light the badge again";
  freshDevice();
  // The unread one is stored first (listed, and recounted, first) but shown
  // newest: the recount under way has counted it before it is read.
  // (100 records: none trimmed away by the sweep)
  storeText(kBob, kBob, true, true, 1700000000u + 400 * 60, "E");
  for (int i = 0; i < 99; i++) storeText(kBob, kBob, true, false, 1700000000u + i * 60, "E");
  powerOn();
  {
    Notifications::Summary* b = nullptr;
    runFor(60);  // counted: 1 (sweep done)
    // the stored summary is now exact; a reboot makes it UNRESOLVED and queues its recount
    reboot();
    loopPass(10);  // that recount has started, and counted the unread one
#ifndef NOTIFY_HEAD
    CHECK(Notifications::g_job != nullptr && Notifications::g_job->kind == Notifications::JOB_RECOUNT, "precondition: recount running");
#endif
    openText(kBob); rot(-1);
    for (int i = 0; i < 4000 && truth(kBob).text > 0; i++) loopPass();
    // Lit while the summary is UNRESOLVED (the read was applied while it was
    // being recounted); once dark it must never light again.
    // Whenever no recount of it is under way, a COUNTED summary is exact (a
    // recount that had counted the message before it was read starts over).
    bool dark = false, relit = false, stale = false;
    for (int i = 0; i < 4000; i++) {
      loopPass();
      if (!textBadge()) dark = true;
      else if (dark) relit = true;
      Notifications::Summary* cur = Notifications::findSummary(kG, kBob);
      bool bobJob = Notifications::g_job != nullptr && strcmp(Notifications::g_job->contact_key, kBob) == 0;
      if (cur != nullptr && cur->state == Notifications::SUMMARY_COUNTED && !bobJob && Notifications::getUnreadCount(kG, kBob) != truth(kBob).text)
        stale = true;
    }
    CHECK(!stale, "a recount finished with a stale count (counted the message before it was read)");
    b = Notifications::findSummary(kG, kBob);
    CHECK(!relit && !textBadge() && truth(kBob).text == 0, "badge relit %d now %d unread %d state %d", relit, textBadge(),
          truth(kBob).text, b ? b->state : -1);
    back();
  }

  g_test = "ST13 a leftover <seq>.msg.tmp from an interrupted rewrite never counts its message twice";
  freshDevice();
  {
    MessageRef r = storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, true, 1700000100u);
    char path[512], tmp[520];
    snprintf(path, sizeof(path), "%s/messages/%s/%s/%08lu.msg", LittleFS.root.c_str(), kG, kCarol,
             static_cast<unsigned long>(r.sequence));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* in = fopen(path, "rb"); FILE* out = fopen(tmp, "wb");
    int ch, copied = 0;
    while (in && out && (ch = fgetc(in)) != EOF) { fputc(ch, out); copied++; }
    if (in) fclose(in);
    if (out) fclose(out);
    CHECK(copied > 0, "precondition: leftover .tmp not created (%s)", tmp);
    powerOn(); runFor(30);
    CHECK(Notifications::getUnreadCount(kG, kCarol) == 1, "count %u (the .tmp copy counted)", Notifications::getUnreadCount(kG, kCarol));
  }

  g_test = "ST14 one record unreadable during a recount: the count is not applied until it reads, then exact";
  freshDevice();
  {
    MessageRef bad{};
    for (int i = 0; i < 10; i++) {
      MessageRef r = storeOnly(kCarol, PacketCodec::MSG_TYPE_ENIGMA, true, 1700000000u + i * 60);
      if (i == 4) bad = r;
    }
    static char badName[32];
    snprintf(badName, sizeof(badName), "/%08lu.msg", static_cast<unsigned long>(bad.sequence));
    LittleFS.failOpen = [](const std::string& p) {
      return p.size() > strlen(badName) && p.compare(p.size() - strlen(badName), strlen(badName), badName) == 0;
    };
    powerOn(); runFor(60);
    CHECK(Notifications::getUnreadCount(kG, kCarol) != 9, "a recount missing one record was applied: %u",
          Notifications::getUnreadCount(kG, kCarol));
    LittleFS.failOpen = nullptr;
    runFor(60);
    CHECK(Notifications::getUnreadCount(kG, kCarol) == 10 && enigmaBadge(), "after the record reads again: %u",
          Notifications::getUnreadCount(kG, kCarol));
  }

  // ---- retention: the latest 100 per conversation (approved reduction from 300) ----
  g_test = "ST15 legacy 300-record conversation trimmed in the background to the latest 100; pending outbox kept; contents kept";
  freshDevice();
  T::connected = false;  // offline: the Outbox would publish the pending records (and rewrite their flags)
  {
    std::vector<MessageRef> pending;
    for (int i = 0; i < 300; i++) {
      if (i == 5 || i == 10) pending.push_back(storePending(kCarol, 1700000000u + i * 60));
      else storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, i % 3 != 0, 1700000000u + i * 60);
    }
    std::map<uint32_t, std::string> before = onFlash(kG, kCarol);
    powerOn();
    g_svcMax = PassMax(); g_uiMax = PassMax();
    runFor(600);
    std::map<uint32_t, std::string> after = onFlash(kG, kCarol);
    // expected: both pending records, plus the 98 highest-sequence others
    std::vector<uint32_t> others;
    for (auto& kv : before) if (kv.first != pending[0].sequence && kv.first != pending[1].sequence) others.push_back(kv.first);
    std::sort(others.begin(), others.end());
    std::map<uint32_t, std::string> expect;
    expect[pending[0].sequence] = before[pending[0].sequence];
    expect[pending[1].sequence] = before[pending[1].sequence];
    for (size_t i = others.size() - 98; i < others.size(); i++) expect[others[i]] = before[others[i]];
    CHECK(after.size() == 100 && after == expect, "kept %zu records; the expected set (pending + latest 98, bytes unchanged): %d",
          after.size(), after == expect);
    CHECK(Notifications::getUnreadCount(kG, kCarol) == truth(kCarol).text && textBadge() == (truth(kCarol).text > 0),
          "count %u flash %d", Notifications::getUnreadCount(kG, kCarol), truth(kCarol).text);
    CHECK(g_svcMax.opens <= 8 && g_svcMax.removes <= 1, "per pass: opens %llu removes %llu", (unsigned long long)g_svcMax.opens,
          (unsigned long long)g_svcMax.removes);
  }
  T::connected = true;

  g_test = "ST16 trimming interrupted by a reboot resumes and ends with exactly the latest 100";
  freshDevice();
  for (int i = 0; i < 300; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  {
    std::map<uint32_t, std::string> before = onFlash(kG, kBob);
    powerOn();
    int guard = 0;
    while (onFlash(kG, kBob).size() > 200 && guard++ < 30000) loopPass();
    size_t mid = onFlash(kG, kBob).size();
    reboot();
    runFor(600);
    std::map<uint32_t, std::string> after = onFlash(kG, kBob);
    std::map<uint32_t, std::string> expect;
    size_t k = 0;
    for (auto& kv : before) if (k++ >= before.size() - 100) expect[kv.first] = kv.second;
    CHECK(mid > 100 && mid < 300 && after == expect, "mid %zu, after %zu, latest 100 kept: %d", mid, after.size(), after == expect);
    CHECK(Notifications::getUnreadCount(kG, kBob) == 100 && truth(kBob).text == 100, "count %u flash %d",
          Notifications::getUnreadCount(kG, kBob), truth(kBob).text);
  }

  g_test = "ST17 failed deletions are retried later, never skipped: the kept set is still exactly the latest 100";
  freshDevice();
  for (int i = 0; i < 160; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  {
    std::map<uint32_t, std::string> before = onFlash(kG, kBob);
    static bool failing = true;
    failing = true;
    LittleFS.failRemove = [](const std::string& p) { return failing && p.size() > 4 && p.compare(p.size() - 4, 4, ".msg") == 0; };
    powerOn();
    runFor(120);
    CHECK(onFlash(kG, kBob).size() == 160, "removed while removals fail: %zu", onFlash(kG, kBob).size());
    CHECK(Notifications::getUnreadCount(kG, kBob) == 160, "count changed without a deletion: %u", Notifications::getUnreadCount(kG, kBob));
    failing = false;
    runFor(900);
    std::map<uint32_t, std::string> after = onFlash(kG, kBob);
    std::map<uint32_t, std::string> expect;
    size_t k = 0;
    for (auto& kv : before) if (k++ >= before.size() - 100) expect[kv.first] = kv.second;
    CHECK(after == expect && Notifications::getUnreadCount(kG, kBob) == 100, "after recovery: %zu kept, expected set %d, count %u",
          after.size(), after == expect, Notifications::getUnreadCount(kG, kBob));
    LittleFS.failRemove = nullptr;
  }

  g_test = "ST18 trimmed unread records leave the counts and badges consistent with flash (a category fully trimmed goes dark)";
  freshDevice();
  {
    const char* kDave = "DA7E00000001";
    for (int i = 0; i < 200; i++) storeOnly(kDave, PacketCodec::MSG_TYPE_ENIGMA, true, 1700000000u + i * 60);  // oldest: unread Enigma
    for (int i = 0; i < 100; i++) storeOnly(kDave, PacketCodec::MSG_TYPE_TEXT, false, 1800000000u + i * 60);   // newest: sent Text
    powerOn();
    runFor(900);
    CHECK(onFlash(kG, kDave).size() == 100 && truth(kDave).enigma == 0 && !enigmaBadge() &&
              Notifications::getUnreadCount(kG, kDave) == 0,
          "kept %zu, flash enigma %d, badge %d, count %u", onFlash(kG, kDave).size(), truth(kDave).enigma, enigmaBadge(),
          Notifications::getUnreadCount(kG, kDave));
  }

  g_test = "ST19 a read whose rewrite keeps failing stays pending (retried with backoff, not dropped) without stalling other work; a reboot loses it and the message stays unread (round 5; was: a persisted read-up resumed)";
  freshDevice();
  for (int i = 0; i < 30; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  for (int i = 0; i < 5; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_ENIGMA, true, 1700000000u + i * 60);
  {
    static bool failing = false;
    powerOn(); runFor(30);
    failing = true;
    LittleFS.failOpen = [](const std::string& p) { return failing && p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0; };
    openText(kBob); rot(-1); back();
    storeOnly(kCarol, PacketCodec::MSG_TYPE_ENIGMA, true, 1800000000u);  // found by a recount only
    coldIndex();                                                         // (an outside change: everything recounted)
    reboot();                                                            // ... by the boot sweep; the read is lost with it
    openText(kBob); rot(-1); back();                                     // so it is made again, writes still failing
    runFor(300);
    CHECK(truth(kBob).text == 30 && Notifications::getUnreadCount(kG, kBob) == 30, "precondition: writes failing, nothing cleared (%d left)",
          truth(kBob).text);
    CHECK(pendingReads() == 1 && pendingFailures() >= 4 && pendingFailures() <= 12,
          "the failing read was dropped, never retried, or retried without backoff (%d pending, %d failures in 300 s)", pendingReads(),
          pendingFailures());
    CHECK(Notifications::getUnreadCount(kG, kCarol) == 6, "other work stalled behind the failing read: Carol %u",
          Notifications::getUnreadCount(kG, kCarol));
    reboot();
    failing = false;
    runFor(300);
    CHECK(truth(kBob).text == 30 && Notifications::getUnreadCount(kG, kBob) == 30, "after reboot: Bob %d unread (count %u)", truth(kBob).text,
          Notifications::getUnreadCount(kG, kBob));
    LittleFS.failOpen = nullptr;
  }

  g_test = "ST20 power lost right after a move, before its rewrite: the read is lost, nothing is wrongly read, counts follow flash";
  freshDevice();
  for (int i = 0; i < 40; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn(); runFor(30);
  openText(kBob); rot(-1);
  CHECK(pendingReads() == 1, "precondition: the read is not pending");
  reboot(); TextMessage::clearOpenConversation();
  runFor(120);
  CHECK(truth(kBob).text == 40 && textBadge() && Notifications::getUnreadCount(kG, kBob) == 40, "flash %d badge %d count %u",
        truth(kBob).text, textBadge(), Notifications::getUnreadCount(kG, kBob));

  g_test = "ST21 400 conversations in 4 groups (and 200 empty group directories): the walk reads a bounded number of directory entries per pass and skips none";
  freshDevice();
  {
    char group[16], contact[16];
    std::vector<std::pair<std::string, std::string>> unreadOnes;
    for (int g = 0; g < 4; g++) {
      snprintf(group, sizeof(group), "GRP%d", g);
      for (int c = 0; c < 100; c++) {
        snprintf(contact, sizeof(contact), "K%011d", c);
        bool unread = (c % 41 == 40) || (g == 3 && c == 99);  // a few, including the last in every order
        PacketCodec::MessageEnvelope env = envOf(contact, PacketCodec::MSG_TYPE_TEXT, 1700000000u + c);
        strncpy(env.group_code, group, sizeof(env.group_code) - 1);
        uint8_t wire[256];
        size_t n = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>("x"), 1, wire, sizeof(wire));
        MessageRef ref{};
        MessageStore::appendStoredMessage(group, contact, MessageStore::Direction::RECEIVED, unread ? MessageStore::FLAG_UNREAD : 0,
                                          env.timestamp, wire, static_cast<uint16_t>(n), nullptr, 0, &ref);
        if (unread) unreadOnes.push_back({group, contact});
      }
    }
    // Directories older firmware may leave without conversations: walked
    // through a few entries per step like the rest.
    for (int e = 0; e < 200; e++) {
      std::error_code ec;
      std::filesystem::create_directories(LittleFS.root + "/messages/EMPTY" + std::to_string(e), ec);
    }
    powerOn();
    g_svcMax = PassMax();
    runFor(900);
    bool all = true;
    for (auto& u : unreadOnes) all = all && Notifications::getUnreadCount(u.first.c_str(), u.second.c_str()) == 1;
    CHECK(all && textBadge(), "an unread conversation was skipped (%zu expected)", unreadOnes.size());
    // One step per pass: 16 walk entries, or one listing of a 1-record conversation.
    CHECK(g_svcMax.dirEntries <= 20, "per pass: %llu directory entries", (unsigned long long)g_svcMax.dirEntries);
  }

  g_test = "ST22 the open conversation is not trimmed while open (its draft untouched); it is trimmed after leaving";
  freshDevice();
  for (int i = 0; i < 300; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
  powerOn();
  openText(kBob);
  strcpy(g_composeText, "DRAFT"); g_composeLen = 5;
  runFor(300);
  CHECK(onFlash(kG, kBob).size() == 300 && strcmp(g_composeText, "DRAFT") == 0, "while open: %zu kept, draft '%s'",
        onFlash(kG, kBob).size(), g_composeText);
  back();
  runFor(900);
  CHECK(onFlash(kG, kBob).size() == 100, "after leaving: %zu kept", onFlash(kG, kBob).size());

  for (int order = 0; order < 2; order++) {
    g_test = order == 0 ? "ST23 a conversation beyond the 300-entry listing capacity (copied in by hand): trimmed oldest-first to 100, counts exact"
                        : "ST23b the same with directories listed in reverse name order (LittleFS promises no order)";
    freshDevice();
    LittleFS.reverseListing = order == 1;
    for (int i = 0; i < 340; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, i >= 330, 1700000000u + i * 60);  // newest 10 unread
    powerOn();
    runFor(900);
    auto f = onFlash(kG, kBob);
    bool newest = f.size() == 100 && f.begin()->first == 241 && f.rbegin()->first == 340;
    CHECK(newest, "kept %zu records, sequences %u..%u", f.size(), f.empty() ? 0u : f.begin()->first,
          f.empty() ? 0u : f.rbegin()->first);
    CHECK(Notifications::getUnreadCount(kG, kBob) == 10 && truth(kBob).text == 10 && textBadge(), "count %u, flash %d",
          Notifications::getUnreadCount(kG, kBob), truth(kBob).text);
  }

  g_test = "ST24 store contract: a listing beyond capacity is reported incomplete and holds the oldest sequences (either directory order)";
  for (int order = 0; order < 2; order++) {
    freshDevice();
    LittleFS.reverseListing = order == 1;
    for (int i = 0; i < 340; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, i >= 330, 1700000000u + i * 60);
    static uint32_t seqs[MessageStore::kMaxMessagesPerThread];
    bool reliable = true, truncated = false;
    uint16_t n = MessageStore::listMessageSequences(kG, kBob, seqs, MessageStore::kMaxMessagesPerThread, &reliable, &truncated);
    uint32_t mx = 0;
    for (uint16_t i = 0; i < n; i++) mx = std::max(mx, seqs[i]);
    CHECK(n == 300 && !reliable && truncated && mx == 300, "order %d: n %u reliable %d truncated %d highest kept %u", order, n,
          reliable, truncated, mx);
    bool scanReliable = true;
    MessageStore::findMessagesByPredicate(
        kG, kBob, [](const MessageStore::StoredHeader&, const PacketCodec::MessageEnvelope&, void*) { return false; }, nullptr,
        nullptr, 0, &scanReliable);
    CHECK(!scanReliable, "order %d: a scan that saw 300 of 340 records was reported complete", order);
  }

  g_test = "ST25 an append to a conversation at the retained 100 evicts the oldest at once (not left to the trim)";
  freshDevice();
  for (int i = 0; i < 100; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
  powerOn();
  arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
  {
    auto f = onFlash(kG, kBob);
    CHECK(f.size() == 100 && f.begin()->first == 2 && f.rbegin()->first == 101, "after one append: %zu records, %u..%u", f.size(),
          f.empty() ? 0u : f.begin()->first, f.empty() ? 0u : f.rbegin()->first);
  }

  g_test = "ST26 counts match flash at EVERY pass while deletions fail and while trimming proceeds (and records go oldest-first)";
  freshDevice();
  LittleFS.reverseListing = true;  // no help from the directory order
  for (int i = 0; i < 300; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  {
    static bool failing = true;
    failing = true;
    LittleFS.failRemove = [](const std::string& p) { return failing && p.size() > 4 && p.compare(p.size() - 4, 4, ".msg") == 0; };
    powerOn();
    runFor(20);  // first trim attempt fails, the recount counts what is there
    CHECK(Notifications::findSummary(kG, kBob) != nullptr &&
              Notifications::findSummary(kG, kBob)->state == Notifications::SUMMARY_COUNTED &&
              Notifications::getUnreadCount(kG, kBob) == 300,
          "precondition: not counted before the sampling (count %u)", Notifications::getUnreadCount(kG, kBob));
    int bad = 0, gaps = 0, passes = 0;
    auto sample = [&]() {
      auto f = onFlash(kG, kBob);
      uint16_t c = Notifications::getUnreadCount(kG, kBob);
      bool counted = Notifications::findSummary(kG, kBob) != nullptr &&
                     Notifications::findSummary(kG, kBob)->state == Notifications::SUMMARY_COUNTED;
      if (counted && c != f.size()) bad++;
      if (!f.empty() && f.rbegin()->first - f.begin()->first + 1 != f.size()) gaps++;  // kept set: a contiguous newest run
      passes++;
    };
    for (int i = 0; i < 500; i++) { loopPass(); sample(); }  // 10 s of failing deletions
    CHECK(bad == 0 && onFlash(kG, kBob).size() == 300, "while deletions fail: %d of %d passes counted differently from flash", bad,
          passes);
    failing = false;
    bad = 0; passes = 0;
    for (int i = 0; i < 45000 && onFlash(kG, kBob).size() > 100; i++) { loopPass(); sample(); }
    runFor(30);
    sample();
    CHECK(bad == 0 && gaps == 0, "while trimming: %d of %d passes counted differently from flash, %d with a gap", bad, passes, gaps);
    auto f = onFlash(kG, kBob);
    CHECK(f.size() == 100 && f.begin()->first == 201 && Notifications::getUnreadCount(kG, kBob) == 100, "end: %zu kept from %u, count %u",
          f.size(), f.empty() ? 0u : f.begin()->first, Notifications::getUnreadCount(kG, kBob));
  }

  g_test = "ST27 an unreadable old record stops the trim (reported, retried); nothing newer is deleted in its place";
  freshDevice();
  for (int i = 0; i < 150; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
  {
    char path[256];
    snprintf(path, sizeof(path), "%s/messages/%s/%s/%08lu.msg", LittleFS.root.c_str(), kG, kBob, 3ul);
    FILE* f = fopen(path, "wb");
    fputs("x", f);  // truncated: not a record
    fclose(f);
    MessageStore::notifyExternalStorageMutationAttempted();
    powerOn();
    runFor(300);
    auto k = onFlash(kG, kBob);
    CHECK(k.count(3) == 1 && k.count(4) == 1 && k.size() == 148, "kept %zu; seq 3 %zu, seq 4 %zu (only 1 and 2 may go)", k.size(),
          k.count(3), k.count(4));
  }

  // =========================================================================
  // Chat entry, round 2 (device A on 2ed04b6). Host operation counts; the
  // device-cost model (LittleFS.onFileOpen: 63 ms per file open, device A's
  // measured cost) turns them into simulated time. Not device timing.
  // =========================================================================
  // The chat shows Morse by default: "msg 089" is drawn as "-- ... --. / ----- ---.. ----.".
  const char* kNewest = "----- ---.. ----.";  // 089
  auto everyoneHistory = [](int n) {
    char text[40];
    for (int i = 0; i < n; i++) {
      snprintf(text, sizeof(text), "msg %03d", i);
      bool received = i % 3 != 0;
      storeText(MessageStore::kEveryone, received ? kBob : "DEADBEEFAAAA", received, received && i >= n - 5,
                1700000000u + i * 60, text);
    }
  };
  auto snapshotIndex = [](uint16_t n) {
    std::vector<std::tuple<uint32_t, uint32_t, std::string, uint16_t>> v;
    for (uint16_t i = 0; i < n; i++) {
      const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
      if (e != nullptr) v.emplace_back(e->sequence, e->effective_sort_timestamp, e->message_id, e->flags);
    }
    return v;
  };
  auto freshIndex = [&](const char* contact) {
    coldIndex();
    uint16_t n = MessageStore::loadConversationIndex(kG, contact);
    return snapshotIndex(n);
  };

  g_test = "CE1 Text Everyone chat, 90 messages, after the boot sweep: the index came from the sweep's reads; entry reads only the rows drawn";
  freshDevice();
  everyoneHistory(90);
  for (int i = 0; i < 100; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
  powerOn(); runFor(120);  // boot sweep done
  {
    EntryProfile p = enterChat(false, MessageStore::kEveryone, kNewest);
    printf("   CE1 measured: max %llu opens/pass, %llu opens total, content after %d passes\n",
           (unsigned long long)p.maxPassOpens, (unsigned long long)p.totalOpens, p.passesToContent);
    CHECK(p.passesToContent > 0 && p.passesToContent <= 2, "the newest message appeared after %d passes", p.passesToContent);
    CHECK(p.totalOpens <= 8, "entry opened %llu files (the drawn rows are at most 8)", (unsigned long long)p.totalOpens);
  }

  g_test = "CE1b the same chat entered before the sweep reached it (cold): spread over passes, bounded opens per pass";
  {
    back();
    coldIndex();
    EntryProfile p = enterChat(false, MessageStore::kEveryone, kNewest);
    printf("   CE1b measured: max %llu opens/pass, %llu opens total, content after %d passes\n",
           (unsigned long long)p.maxPassOpens, (unsigned long long)p.totalOpens, p.passesToContent);
    CHECK(p.passesToContent > 1 && p.maxPassOpens <= 12, "max %llu opens in a pass, content after %d passes",
          (unsigned long long)p.maxPassOpens, p.passesToContent);
    CHECK(snapshotIndex(g_indexTotal) == freshIndex(MessageStore::kEveryone) && g_indexTotal == 90,
          "the stepped load differs from a one-call load (%u entries)", g_indexTotal);
  }

  g_test = "CE2 re-entry after work in OTHER conversations (arrivals with eviction, a flag rewrite) reuses the index: no reload";
  {
    back();
    openText(MessageStore::kEveryone);
    back();
    for (int k = 0; k < 3; k++) arrive(kCarol, PacketCodec::MSG_TYPE_TEXT, 1800000000u + k);  // Carol holds 100: each evicts one
    MessageRef c = arrive(kCarol, PacketCodec::MSG_TYPE_TEXT, 1800000010u);
    Notifications::clearUnread(kG, kCarol, c);  // a flag rewrite there
    runFor(30);  // background work settled (CE1b's outside change started an Outbox pass): only the chat is measured
    EntryProfile p = enterChat(false, MessageStore::kEveryone, kNewest);
    printf("   CE2 measured: max %llu opens/pass, %llu opens total, content after %d passes\n",
           (unsigned long long)p.maxPassOpens, (unsigned long long)p.totalOpens, p.passesToContent);
    CHECK(p.passesToContent == 1 && p.totalOpens == 0, "re-entry: %llu opens, content after %d passes",
          (unsigned long long)p.totalOpens, p.passesToContent);
  }

  g_test = "CE2b after visiting another chat: the index was parked and is swapped back without reading";
  {
    back();
    openText(kCarol);
    back();
    runFor(30);
    EntryProfile p = enterChat(false, MessageStore::kEveryone, kNewest);
    printf("   CE2b measured: max %llu opens/pass, %llu opens total, content after %d passes\n",
           (unsigned long long)p.maxPassOpens, (unsigned long long)p.totalOpens, p.passesToContent);
    CHECK(p.passesToContent == 1 && p.totalOpens == 0, "return: %llu opens, content after %d passes",
          (unsigned long long)p.totalOpens, p.passesToContent);
    back();
  }

  g_test = "CE3 the open chat's own changes (an arrival, a read-up rewriting flags, an eviction) are patched in: the index equals a fresh load";
  freshDevice();
  everyoneHistory(100);
  powerOn(); runFor(120);
  {
    openText(MessageStore::kEveryone);
    FsStats a = g_fs;
    MessageRef r = arrive(MessageStore::kEveryone, PacketCodec::MSG_TYPE_TEXT, 1700003000u);  // at 100: evicts the oldest
    runFor(2);
    uint64_t opens = g_fs.fileOpens - a.fileOpens;
    printf("   CE3 measured: %llu opens to store an arrival (with its eviction) and show it in the open chat\n", (unsigned long long)opens);
    CHECK(opens <= 12, "%llu opens: the open chat reloaded its index", (unsigned long long)opens);
    rot(-1); rot(-1);  // selects; the read-up rewrites flags
    runFor(10);
    auto kept = snapshotIndex(g_indexTotal);
    uint16_t total = g_indexTotal;
    back();
    auto fresh = freshIndex(MessageStore::kEveryone);
    (void)opens;
    CHECK(kept == fresh && total == 100 && onFlash(kG, MessageStore::kEveryone).count(r.sequence) == 1,
          "patched index equal to a fresh load %d (%u entries)", kept == fresh, total);
  }

  g_test = "CE13 caches follow flash exactly: after display loads, read-ups, arrivals with eviction, a payload rewrite, a failed write and a deletion, every record served equals its file";
  freshDevice();
  everyoneHistory(100);
  powerOn(); runFor(120);
  {
    openText(MessageStore::kEveryone);
    for (int i = 0; i < 6; i++) rot(-1);
    runFor(5);
    arrive(MessageStore::kEveryone, PacketCodec::MSG_TYPE_TEXT, 1700009000u);
    MessageRef any{};
    strcpy(any.group_code, kG);
    strcpy(any.contact_key, MessageStore::kEveryone);
    any.sequence = 99;
    const uint8_t local[3] = {1, 2, 3};
    MessageStore::updateTypeLocalPayload(any, local, sizeof(local));
    LittleFS.failRename = [](const std::string& p) { return p.find("/00000098.msg") != std::string::npos; };
    any.sequence = 98;
    MessageStore::updateLocalFlags(any, MessageStore::FLAG_UNREAD, 0);  // fails: the record is lost (removed before the rename)
    LittleFS.failRename = nullptr;
    any.sequence = 97;
    MessageStore::deleteMessage(any);
    runFor(5);
    // Every record: what the store serves equals the file on flash.
    auto files = onFlash(kG, MessageStore::kEveryone);
    int differ = 0, served = 0;
    for (uint32_t seq = 1; seq <= 110; seq++) {
      MessageRef ref = any;
      ref.sequence = seq;
      StoredMessageView v;
      bool ok = MessageStore::loadMessage(ref, &v);
      bool onDisk = files.count(seq) == 1;
      if (ok != onDisk) differ++;
      if (ok && onDisk) {
        served++;
        const std::string& f = files[seq];
        uint16_t wl = static_cast<uint8_t>(f[11]) | (static_cast<uint8_t>(f[12]) << 8);
        bool same = v.header.flags == (static_cast<uint8_t>(f[1]) | (static_cast<uint8_t>(f[2]) << 8)) && v.wirePacketLen == wl &&
                    memcmp(v.wirePacket, f.data() + 13, wl) == 0 && v.localPayloadLen == f.size() - 15 - wl;
        if (!same) differ++;
      }
    }
    auto kept = snapshotIndex(g_indexTotal);
    back();
    auto fresh = freshIndex(MessageStore::kEveryone);
    CHECK(differ == 0 && served == static_cast<int>(files.size()), "%d records differ from flash (%d served, %zu on flash)", differ,
          served, files.size());
    CHECK(kept == fresh, "the chat's index differs from a fresh load (%zu vs %zu entries)", kept.size(), fresh.size());
  }

  g_test = "CE15 the record cache keeps conversations apart: two chats with the same sequence numbers each show their own messages";
  freshDevice();
  storeText(kBob, kBob, true, false, 1700000000u, "BBB");
  storeText(kCarol, kCarol, true, false, 1700000000u, "CCC");
  powerOn(); runFor(30);
  {
    openText(kBob);
    bool bobOk = T::visibleJoined().find("-... -... -...") != std::string::npos;
    back();
    openText(kCarol);
    bool carolOk = T::visibleJoined().find("-.-. -.-. -.-.") != std::string::npos && T::visibleJoined().find("-... -... -...") == std::string::npos;
    back();
    CHECK(bobOk && carolOk, "Bob's chat showed his message %d, Carol's showed hers only %d", bobOk, carolOk);
  }

  g_test = "CE16 a recount of Everyone while another chat is open does not replace that chat's index";
  freshDevice();
  everyoneHistory(40);
  for (int i = 0; i < 20; i++) storeText(kBob, kBob, true, false, 1700000000u + i * 60, "BOB");
  powerOn();
  {
    openText(kBob);
    int taken = 0;
    for (int i = 0; i < 2000; i++) {  // the sweep recounts (and feeds) Everyone meanwhile
      loopPass();
      if (!MessageStore::isIndexCurrent(kG, kBob)) taken++;
    }
    CHECK(taken == 0, "the open chat's index was not current in %d passes (taken by a feed)", taken);
    bool ok = g_indexTotal == 20;
    for (uint16_t i = 0; i < g_indexTotal && ok; i++) {
      const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
      ok = e != nullptr && onFlash(kG, kBob).count(e->sequence) == 1;
    }
    rot(-1);
    bool shown = T::visibleJoined().find("-... --- -...") != std::string::npos;
    back();
    CHECK(ok && shown, "Bob's open chat: index still Bob's %d (%u entries), his messages shown %d", ok, g_indexTotal, shown);
  }

  g_test = "CE17 the boot sweep reads each record once (Everyone, visited first, is not visited again by the walk)";
  freshDevice();
  T::connected = false;  // offline: the Outbox's own header pass (once per boot, after connecting) is OB1's
  everyoneHistory(50);
  for (int i = 0; i < 30; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  {
    FsStats a = g_fs;
    powerOn(); runFor(120);
    uint64_t opens = g_fs.fileOpens - a.fileOpens;
    printf("   CE17 measured: %llu file opens for the boot sweep of 80 records\n", (unsigned long long)opens);
    CHECK(opens <= 80 + 4, "%llu opens for 80 records", (unsigned long long)opens);
  }
  T::connected = true;

  g_test = "CE18 a displayed (cached) record that is deleted is no longer served";
  {
    openText(MessageStore::kEveryone);
    const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(g_indexTotal - 1);
    MessageRef r{};
    strcpy(r.group_code, kG);
    strcpy(r.contact_key, MessageStore::kEveryone);
    r.sequence = e->sequence;
    StoredMessageView v;
    bool before = MessageStore::loadMessage(r, &v);
    MessageStore::deleteMessage(r);
    FsStats a = g_fs;
    bool after = MessageStore::loadMessage(r, &v);
    CHECK(before && !after && g_fs.openFails > a.openFails, "served before %d, after deletion %d", before, after);
    back();
  }

  g_test = "CE19 a parked index follows its conversation (arrivals with eviction, a flag rewrite) and is swapped back equal to a fresh load";
  freshDevice();
  everyoneHistory(100);
  for (int i = 0; i < 30; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn(); runFor(120);
  {
    openText(MessageStore::kEveryone);
    back();
    openText(kBob);  // Everyone parked
    MessageRef last{};
    for (int k = 0; k < 3; k++) {
      PacketCodec::MessageEnvelope env = envOf(kCarol, PacketCodec::MSG_TYPE_TEXT, 1800000000u + k);
      uint8_t payload[64], wire[256];
      size_t pn = PacketCodec::encodeTextPayload("parked", payload, sizeof(payload));
      size_t wn = PacketCodec::encodeMessagePacket(env, payload, static_cast<uint16_t>(pn), wire, sizeof(wire));
      MessageStore::appendStoredMessage(kG, MessageStore::kEveryone, MessageStore::Direction::RECEIVED, MessageStore::FLAG_UNREAD,
                                        env.timestamp, wire, static_cast<uint16_t>(wn), nullptr, 0, &last);  // evicts the oldest
      loopPass(2);
    }
    Notifications::clearUnread(kG, MessageStore::kEveryone, last);  // a flag rewrite of a parked record
    arrive(kCarol, PacketCodec::MSG_TYPE_TEXT, 1800000500u);  // a mutation elsewhere
    runFor(5);
    back();
    FsStats a = g_fs;
    uint16_t c = 0;
    bool swapped = MessageStore::stepConversationIndex(kG, MessageStore::kEveryone, 0, &c, nullptr, nullptr);
    uint64_t reads = g_fs.fileOpens - a.fileOpens;
    auto kept = snapshotIndex(c);
    auto fresh = freshIndex(MessageStore::kEveryone);
    CHECK(swapped && reads == 0 && kept == fresh && c == 100, "swapped back %d with %llu reads; equal to a fresh load %d (%u entries)",
          swapped, (unsigned long long)reads, kept == fresh, c);
  }

  g_test = "CE20 a rewrite whose rename fails loses the record (the old file was removed first): neither the shared nor the parked index keeps listing it";
  for (int parked = 0; parked < 2; parked++) {
    freshDevice();
    everyoneHistory(40);
    for (int i = 0; i < 10; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
    powerOn(); runFor(60);
    openText(MessageStore::kEveryone);
    if (parked) { back(); openText(kBob); }
    MessageRef r{};
    strcpy(r.group_code, kG);
    strcpy(r.contact_key, MessageStore::kEveryone);
    r.sequence = 40;
    LittleFS.failRename = [](const std::string&) { return true; };
    MessageStore::updateLocalFlags(r, 0, MessageStore::FLAG_UNREAD);
    LittleFS.failRename = nullptr;
    bool gone = onFlash(kG, MessageStore::kEveryone).count(40) == 0;
    back();
    if (!parked) TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone), waitChatDrawn();
    uint16_t c = 0;
    if (parked) {
      while (!MessageStore::stepConversationIndex(kG, MessageStore::kEveryone, 8, &c, nullptr, nullptr)) {}
    } else {
      for (int i = 0; i < 2000 && g_indexDirty; i++) loopPass();
      c = g_indexTotal;
    }
    bool listed = false;
    for (uint16_t i = 0; i < c; i++) listed = listed || MessageStore::getIndexEntry(i)->sequence == 40;
    CHECK(gone && !listed && c == 39, "%s: record gone %d, still listed %d (%u entries)", parked ? "parked" : "shared", gone, listed, c);
    if (!parked) back();
  }

  g_test = "CE21 a 1:1 chat opened right after boot holds the shared index: the sweep's recount builds Everyone's parked index, entered without reloading and equal to a fresh load";
  for (int variant = 0; variant < 3; variant++) {  // 0: quiet, 1: an arrival elsewhere meanwhile, 2: an arrival in Everyone meanwhile
    freshDevice();
    everyoneHistory(90);
    for (int i = 0; i < 30; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
    for (int i = 0; i < 20; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
    powerOn();
    openText(kBob);  // before the sweep reached anything
    for (int i = 0; i < 2000; i++) {
      loopPass();
      if (variant > 0 && i == 40) arrive(variant == 1 ? kCarol : MessageStore::kEveryone, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
    }
    back();
    FsStats a = g_fs;
    EntryProfile p = enterChat(false, MessageStore::kEveryone, variant == 2 ? "" : kNewest);
    for (int i = 0; i < 2000 && g_indexDirty; i++) loopPass();
    uint64_t opens = g_fs.fileOpens - a.fileOpens;
    auto kept = snapshotIndex(g_indexTotal);
    back();
    auto fresh = freshIndex(MessageStore::kEveryone);
    printf("   CE21 variant %d measured: %llu opens on entry, content after %d passes\n", variant, (unsigned long long)opens,
           p.passesToContent);
    CHECK(kept == fresh && fresh.size() == (variant == 2 ? 91u : 90u), "variant %d: index equal to a fresh load %d (%zu entries)",
          variant, kept == fresh, fresh.size());
    CHECK(p.passesToContent > 0 && p.passesToContent <= 2 && opens <= 8, "variant %d: content after %d passes, %llu opens (no reload)",
          variant, p.passesToContent, (unsigned long long)opens);
  }

  g_test = "CE22 the parked feed, step by step: a mutation of its conversation, an unreadable record or an out-of-step record drops it; a mutation elsewhere does not";
  for (int variant = 0; variant < 5; variant++) {  // 0 clean, 1 arrival elsewhere, 2 arrival in Everyone, 3 unreadable, 4 out of step
    freshDevice();
    everyoneHistory(30);
    for (int i = 0; i < 10; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
    MessageStore::loadConversationIndex(kG, kBob);  // the shared index is current for Bob
    uint32_t seqs[MessageStore::kMaxMessagesPerThread];
    bool reliable = false;
    uint16_t n = MessageStore::listMessageSequences(kG, MessageStore::kEveryone, seqs, MessageStore::kMaxMessagesPerThread, &reliable);
    bool began = MessageStore::beginIndexFeed(kG, MessageStore::kEveryone, seqs, n, reliable, true);
    for (uint16_t i = 0; i < n; i++) {
      if (i == 15 && variant == 1) arrive(kCarol, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
      if (i == 15 && variant == 2) arrive(MessageStore::kEveryone, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
      MessageRef ref{};
      strcpy(ref.group_code, kG);
      strcpy(ref.contact_key, MessageStore::kEveryone);
      ref.sequence = seqs[variant == 4 && i == 15 ? 16 : i];
      StoredMessageView view;
      bool ok = MessageStore::loadMessage(ref, &view) && !(variant == 3 && i == 15);
      MessageStore::feedIndexRecord(kG, MessageStore::kEveryone, ref.sequence, ok ? &view.header : nullptr,
                                    ok ? view.envelope.message_id : nullptr);
    }
    uint16_t c = 0;
    FsStats a = g_fs;
    bool swapped = MessageStore::stepConversationIndex(kG, MessageStore::kEveryone, 0, &c, nullptr, nullptr);
    uint64_t reads = g_fs.fileOpens - a.fileOpens;
    bool expectUsable = variant <= 1;
    auto kept = swapped ? snapshotIndex(c) : decltype(snapshotIndex(0))();
    auto fresh = freshIndex(MessageStore::kEveryone);
    CHECK(began && swapped == expectUsable && reads == 0 && (!swapped || kept == fresh),
          "variant %d: feed began %d, used %d (expected %d) with %llu reads, equal to a fresh load %d", variant, began, swapped,
          expectUsable, (unsigned long long)reads, kept == fresh);
  }

  g_test = "CE23 the selection stays on its message when the index changes under it (an arrival patched in above it; a reload after an outside change): nothing else is read";
  for (int variant = 0; variant < 2; variant++) {
    freshDevice();
    for (int i = 0; i < 10; i++) storeText(kBob, "DEADBEEFAAAA", false, false, 1700000000u + i * 600, "OUT");
    powerOn(); runFor(30);
    openText(kBob);
    rot(-1); rot(-1); rot(-1);
    uint32_t selected = MessageStore::getIndexEntry(g_historyCursor)->sequence;
    MessageRef older = variant == 0 ? arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000100u)                   // shown near the top
                                    : storeText(kBob, kBob, true, true, 1700000100u, "LATE");                  // written outside the store
    runFor(5);
    bool same = g_historyCursor != kNoHistoryCursor && MessageStore::getIndexEntry(g_historyCursor) != nullptr &&
                MessageStore::getIndexEntry(g_historyCursor)->sequence == selected;
    bool listed = false;
    for (uint16_t i = 0; i < g_indexTotal; i++) listed = listed || MessageStore::getIndexEntry(i)->sequence == older.sequence;
    CHECK(same && listed && g_indexTotal == 11 && isUnread(older), "variant %d: still on its message %d, the new one listed %d (%u), still unread %d",
          variant, same, listed, g_indexTotal, isUnread(older));
    auto kept = snapshotIndex(g_indexTotal);
    back();
    CHECK(kept == freshIndex(kBob), "variant %d: the chat's index differs from a fresh load", variant);
  }

  g_test = "CE11 an arrival in Everyone during its stepped load is followed (no restart): shown, every record read once";
  freshDevice();
  everyoneHistory(90);
  powerOn(); runFor(120);
  {
    coldIndex();
    FsStats a = g_fs;
    TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    loopPass(5);
    MessageRef r = arrive(MessageStore::kEveryone, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
    for (int i = 0; i < 3000 && g_indexDirty; i++) loopPass();
    loopPass();
    uint64_t opens = g_fs.fileOpens - a.fileOpens;
    bool listed = false;
    for (uint16_t i = 0; i < g_indexTotal; i++) listed = listed || MessageStore::getIndexEntry(i)->sequence == r.sequence;
    printf("   CE11 measured: %llu opens to load 90 records with an arrival during the load\n", (unsigned long long)opens);
    CHECK(listed && g_indexTotal == 91 && opens <= 90 + 12, "listed %d (%u entries), %llu opens", listed, g_indexTotal,
          (unsigned long long)opens);
    back();
  }

  g_test = "CE22b the shared feed (no chat open), step by step: an unreadable or out-of-step record, or a mutation of its conversation, leaves it unused; a mutation elsewhere does not";
  for (int variant = 0; variant < 5; variant++) {
    freshDevice();
    everyoneHistory(30);
    for (int i = 0; i < 10; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
    coldIndex();
    uint32_t seqs[MessageStore::kMaxMessagesPerThread];
    bool reliable = false;
    uint16_t n = MessageStore::listMessageSequences(kG, MessageStore::kEveryone, seqs, MessageStore::kMaxMessagesPerThread, &reliable);
    bool began = MessageStore::beginIndexFeed(kG, MessageStore::kEveryone, seqs, n, reliable, false);
    for (uint16_t i = 0; i < n; i++) {
      if (i == 15 && variant == 1) arrive(kCarol, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
      if (i == 15 && variant == 2) arrive(MessageStore::kEveryone, PacketCodec::MSG_TYPE_TEXT, 1800000000u);
      MessageRef ref{};
      strcpy(ref.group_code, kG);
      strcpy(ref.contact_key, MessageStore::kEveryone);
      ref.sequence = seqs[variant == 4 && i == 15 ? 16 : i];
      StoredMessageView view;
      bool ok = MessageStore::loadMessage(ref, &view) && !(variant == 3 && i == 15);
      MessageStore::feedIndexRecord(kG, MessageStore::kEveryone, ref.sequence, ok ? &view.header : nullptr,
                                    ok ? view.envelope.message_id : nullptr);
    }
    uint16_t c = 0;
    FsStats a = g_fs;
    bool ready = MessageStore::stepConversationIndex(kG, MessageStore::kEveryone, 0, &c, nullptr, nullptr);
    uint64_t reads = g_fs.fileOpens - a.fileOpens;
    auto kept = ready ? snapshotIndex(c) : decltype(snapshotIndex(0))();
    auto fresh = freshIndex(MessageStore::kEveryone);
    bool expectUsable = variant <= 1;
    CHECK(began && ready == expectUsable && (!ready || (kept == fresh && reads == 0)),
          "variant %d: feed began %d, used %d (expected %d) with %llu reads, equal to a fresh load %d", variant, began, ready,
          expectUsable, (unsigned long long)reads, kept == fresh);
  }

  g_test = "CE24 a load that met an unreadable record is not cached: once the record reads again, the next entry lists it";
  freshDevice();
  everyoneHistory(30);
  powerOn(); runFor(60);
  {
    coldIndex();
    LittleFS.failOpen = [](const std::string& p) { return p.find("/EVERYONE/00000007.msg") != std::string::npos; };
    openText(MessageStore::kEveryone);
    uint16_t during = g_indexTotal;
    back();
    LittleFS.failOpen = nullptr;
    openText(MessageStore::kEveryone);
    auto kept = snapshotIndex(g_indexTotal);
    back();
    CHECK(during == 29 && kept == freshIndex(MessageStore::kEveryone) && kept.size() == 30, "while unreadable %u entries; after %zu",
          during, kept.size());
  }

  // ---- device-cost model: 63 ms per file open (device A's measured cost) ----
  auto deviceCost = []() { LittleFS.onFileOpen = [](const std::string&) { T::now += 63; }; };
  auto modelEntry = [&](const char* label, int afterBootMs, int maxPasses) {
    T::now += 0;
    loopPass(afterBootMs / 20);
    PassMax bootMax = g_svcMax;
    uint32_t start = T::now;
    EntryProfile p = enterChat(false, MessageStore::kEveryone, kNewest, maxPasses);
    uint32_t simulated = T::now - start;
    printf("   %s: chat drawn after %d passes, %u ms simulated, max %llu opens per pass (boot passes before: max %llu)\n", label,
           p.passesToContent, simulated, (unsigned long long)p.maxPassOpens, (unsigned long long)bootMax.opens);
    return std::make_tuple(p, simulated, bootMax);
  };

  g_test = "DM1b the broadcast chat opened right after boot: the sweep and the outbox pass read nothing while it loads";
  freshDevice();
  everyoneHistory(90);
  for (int i = 0; i < 100; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
  powerOn();
  {
    loopPass(5);
    TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    loopPass();  // the chat's first step (the services ran before it)
    uint64_t svcOpens = 0;
    int passes = 0;
    for (; passes < 3000 && g_indexDirty; passes++) {
      FsStats a = g_fs;
      T::now += 20;
      T_tickServices();
      svcOpens += g_fs.fileOpens - a.fileOpens;
      Menu::tick();
    }
    CHECK(passes > 5 && svcOpens == 0, "%llu background opens during %d loading passes", (unsigned long long)svcOpens, passes);
    back();
  }

  g_test = "DM1 device-cost model: the broadcast chat opened 1 s after boot (sweep and outbox pass still running) -- bounded opens per pass until drawn";
  freshDevice();
  everyoneHistory(90);
  for (int i = 0; i < 100; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
  deviceCost();
  powerOn();
  {
    auto r = modelEntry("DM1 1 s after boot", 1000, 3000);
    EntryProfile p = std::get<0>(r);
    CHECK(p.passesToContent > 0 && p.maxPassOpens <= 6, "drawn after %d passes, max %llu opens in a pass", p.passesToContent,
          (unsigned long long)p.maxPassOpens);
    CHECK(std::get<2>(r).opens <= 2, "boot passes before the chat: max %llu opens", (unsigned long long)std::get<2>(r).opens);
  }
  back();
  LittleFS.onFileOpen = nullptr;

  g_test = "DM2 device-cost model: the broadcast chat opened once boot work settled -- drawn in the first pass from the sweep-built index";
  freshDevice();
  everyoneHistory(90);
  for (int i = 0; i < 100; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
  deviceCost();
  powerOn();
  {
    auto r = modelEntry("DM2 after boot work settled", 60000, 100);
    EntryProfile p = std::get<0>(r);
    CHECK(p.passesToContent == 1 && std::get<1>(r) < 1000, "drawn after %d passes, %u ms simulated", p.passesToContent, std::get<1>(r));
  }
  back();
  LittleFS.onFileOpen = nullptr;

  g_test = "DM3 device-cost model: 3 large conversations met before Everyone; the chat opened 14 s after boot is drawn in one pass (Everyone visited first)";
  freshDevice();
  char c3[16];
  for (int c = 0; c < 3; c++) {
    snprintf(c3, sizeof(c3), "A%011d", c);  // listed before EVERYONE
    for (int i = 0; i < 99; i++) storeOnly(c3, PacketCodec::MSG_TYPE_TEXT, false, 1700000000u + i * 60);
  }
  everyoneHistory(90);
  deviceCost();
  powerOn();
  {
    auto r = modelEntry("DM3 14 s after boot, 3 x 99 others", 14000, 100);
    EntryProfile p = std::get<0>(r);
    CHECK(p.passesToContent == 1, "drawn after %d passes", p.passesToContent);
  }
  back();
  LittleFS.onFileOpen = nullptr;

  // ---- input while the index loads -------------------------------------------
  // A cold Everyone chat of 90 messages takes about 24 passes to load (4 reads a pass).
  auto coldEveryone = [&]() {
    freshDevice();
    everyoneHistory(90);
    powerOn(); runFor(120);
    coldIndex();
  };

  g_test = "IN1 rotations during loading are held and replayed in order: the same selection as rotating after the load";
  coldEveryone();
  {
    TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    loopPass();
    bool loading = T::visibleJoined().find("Loading messages") != std::string::npos;
    for (int i = 0; i < 5; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    loopPass();
    for (int i = 0; i < 3000 && g_indexDirty; i++) loopPass();
    loopPass();
    uint16_t cursor = g_historyCursor;
    CHECK(loading && cursor == g_indexTotal - 5, "loading shown %d; cursor %u, expected %u", loading, cursor, g_indexTotal - 5);
    back();
  }

  // ---- held input that cannot all be held (review round) ----------------------
  // Every pass of a loading chat is measured in the device-cost model (63 ms
  // per open): the bound is what one stepped load pass costs (4 index reads,
  // plus a little), never a whole conversation.
  constexpr uint64_t kLoadingPassOpens = 6;
  struct LoadRun {
    uint64_t maxPassOpens = 0;
    int passes = 0;
    bool left = false;
  };
  auto chatLoading = []() { return T::visibleJoined().find("Loading messages") != std::string::npos; };
  // Runs passes (feeding the given events, `perPass` at a time) until the chat
  // is drawn or left; records the largest number of opens in one pass.
  // perPass 0: each event arrives when its time comes (keying in real time).
  auto runLoading = [&](std::vector<InputEvent> evs, size_t perPass, int limit = 3000) {
    LoadRun r;
    size_t next = 0;
    for (int i = 0; i < limit; i++) {
      if (perPass == 0) {
        while (next < evs.size() && static_cast<int32_t>(T::now - evs[next].eventMs) >= 0) T::raw.push_back(evs[next++]);
      }
      for (size_t k = 0; k < perPass && next < evs.size(); k++) T::raw.push_back(evs[next++]);
      FsStats a = g_fs;
      loopPass();
      r.maxPassOpens = std::max<uint64_t>(r.maxPassOpens, g_fs.fileOpens - a.fileOpens);
      r.passes++;
      if (!TextMessage::isAnyConversationOpen()) { r.left = true; break; }
      if (next >= evs.size() && T::raw.empty() && !chatLoading() && i > 0) break;
    }
    return r;
  };
  // Morse key events for `text` (A-Z) at 15 wpm, all in the past: dots 60 ms,
  // dashes 240 ms, 80 ms between symbols, 400 ms between letters.
  // (future: keyed from now on, for perPass 0)
  auto morseEvents = [](const char* text, std::vector<InputEvent>& out, bool future = false) {
    uint32_t span = 0;
    for (const char* c = text; *c; c++) {
      char pat[Morse::kMaxPatternLength + 1];
      Morse::encodeChar(*c, pat, sizeof(pat));
      for (char* q = pat; *q; q++) span += (*q == '.' ? 60 : 240) + 80;
      span += 320;
    }
    uint32_t t = future ? T::now + 20 : T::now - span - 600;
    for (const char* c = text; *c; c++) {
      char pat[Morse::kMaxPatternLength + 1];
      Morse::encodeChar(*c, pat, sizeof(pat));
      for (char* q = pat; *q; q++) {
        uint32_t dur = *q == '.' ? 60 : 240;
        out.push_back(InputEvent(InputEventType::DOT_PRESS_START, 0, 0, t));
        out.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, dur, t + dur));
        t += dur + 80;
      }
      t += 320;
    }
  };
  auto openCold = [&](bool enigma) {
    coldEveryone();
    deviceCost();
    T::inputCap = 32;  // input.cpp's queue
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    loopPass();
  };
  auto closeChat = [&]() {
    LittleFS.onFileOpen = nullptr;
    g_failNothrowArrayNew = false;
    T::inputCap = 0;
    if (TextMessage::isAnyConversationOpen()) back();
    loopPass(3);
  };
  const char* kChat[2] = {"Text", "Enigma"};
  std::string twentyE(20, 'E');

  g_test = "IN2 more input while loading than the held buffer takes (40 key events over 4 passes): nothing lost, every pass bounded (no synchronous load)";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    std::vector<InputEvent> evs;
    morseEvents(twentyE.c_str(), evs);  // 40 events
    bool loadingAtStart = chatLoading();
    LoadRun r = runLoading(evs, 10);
    loopPass(30);  // the last letter is finalized after its gap
    bool drafted = T::visibleJoined().find(twentyE) != std::string::npos;
    CHECK(loadingAtStart && r.passes > 8 && r.maxPassOpens <= kLoadingPassOpens && drafted && T::inputDropped == 0,
          "%s: loading %d, %d passes, max %llu opens in a pass, draft of 20 E shown %d, %u dropped", kChat[enigma], loadingAtStart,
          r.passes, (unsigned long long)r.maxPassOpens, drafted, T::inputDropped);
    closeChat();
  }

  g_test = "IN2b a Back queued after the event that overflowed the held buffer leaves at once (bounded pass)";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    std::vector<InputEvent> evs;
    morseEvents(twentyE.c_str(), evs);
    runLoading(std::vector<InputEvent>(evs.begin(), evs.begin() + 34), 34, 1);  // 32 held, the 33rd overflows
    bool stillLoading = chatLoading();
    std::vector<InputEvent> rest(evs.begin() + 34, evs.end());
    rest.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
    LoadRun r = runLoading(rest, rest.size(), 3);
    CHECK(stillLoading && r.left && r.passes <= 1 && r.maxPassOpens <= kLoadingPassOpens, "%s: loading %d, left %d after %d passes, %llu opens",
          kChat[enigma], stillLoading, r.left, r.passes, (unsigned long long)r.maxPassOpens);
    closeChat();
  }

  g_test = "IN2c more input than the device can buffer at all (held buffer and input queue full): the loss is shown, the oldest input is kept, in order";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    std::vector<InputEvent> evs;
    morseEvents(std::string(50, 'E').c_str(), evs);  // 100 events in one burst: more than 32 + 32 can hold
    LoadRun r = runLoading(evs, evs.size());
    bool noticeShown = T::visibleJoined().find("Input lost while loading") != std::string::npos;
    loopPass(30);
    std::string vis = T::visibleJoined();
    size_t es = 0;
    for (size_t k = vis.find("EEEE"); k != std::string::npos && k < vis.size() && vis[k] == 'E'; k++) es++;
    // One input update takes in 32 of the 100 (the queue's capacity) and drops
    // the newest 68: kept are the oldest 32 events = 16 E, in order.
    CHECK(T::inputDropped == 68 && noticeShown && es == 16 && r.maxPassOpens <= kLoadingPassOpens,
          "%s: %u dropped by the input queue, notice shown %d, %zu E kept, max %llu opens in a pass", kChat[enigma], T::inputDropped,
          noticeShown, es, (unsigned long long)r.maxPassOpens);
    closeChat();
  }

  g_test = "IN2d with the held buffer and the input queue full, a Back pressed afterwards still leaves at once";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    std::vector<InputEvent> evs;
    morseEvents(std::string(50, 'E').c_str(), evs);
    runLoading(std::vector<InputEvent>(evs.begin(), evs.begin() + 32), 32, 1);  // 32 held
    runLoading(std::vector<InputEvent>(evs.begin() + 32, evs.end()), 68, 1);    // 32 queued (full), the rest dropped
    bool stillLoading = chatLoading();
    LoadRun r = runLoading({InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now)}, 1, 3);
    CHECK(stillLoading && r.left && r.passes <= 1, "%s: loading %d, left %d after %d passes", kChat[enigma], stillLoading, r.left,
          r.passes);
    closeChat();
  }

  g_test = "IN8 a Send whose store write fails makes the open chat's index stale mid-batch: the reload is stepped (bounded passes), the keys after it kept in order";
  for (int enigma = 0; enigma < 2; enigma++) {
    for (int after = 0; after < 3; after++) {  // keys after the Send in the same batch (1), none (0), a rotation (2)
      coldEveryone();
      if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
      else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
      waitChatDrawn();
      deviceCost();
      T::inputCap = 32;
      T::connected = false;
      // The Send's store write fails before touching the record (its temp
      // file cannot be written): the draft is kept, the index goes stale.
      LittleFS.failOpen = [](const std::string& p) { return p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0; };
      std::vector<InputEvent> evs;
      morseEvents(after == 1 ? "ET" : "E", evs);  // E: events 0-1; T (if any) 400 ms after E's release
      evs.insert(evs.begin() + 2, InputEvent(InputEventType::ENCODER_SHORT, 0, 120, evs[1].eventMs + 200));
      if (after == 2) evs.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));  // selects the newest message
      int composeCursorY0 = -1;
      for (auto& d : T::screen) if (d.text == "#CURSOR") composeCursorY0 = std::max(composeCursorY0, d.y);
      for (auto& ev : evs) T::raw.push_back(ev);
      FsStats a0 = g_fs;
      loopPass();  // the batch: E, the failing Send, (T)
      uint64_t firstPass = g_fs.fileOpens - a0.fileOpens;
      LittleFS.failOpen = nullptr;
      // A reload during a visit keeps the history on screen (no placeholder):
      // a fixed 300 passes, each measured.
      LoadRun r;
      r.maxPassOpens = firstPass;
      for (int q = 0; q < 300; q++) {
        FsStats a = g_fs;
        loopPass();
        r.maxPassOpens = std::max<uint64_t>(r.maxPassOpens, g_fs.fileOpens - a.fileOpens);
      }
      loopPass(30);
      T::connected = true;
      std::string want = after == 1 ? "ET" : "E";
      bool drafted;
      if (after == 2) {
        // The rotation was kept: the selection cursor is in the history, above the compose row.
        int cursorY = -1;
        for (auto& d : T::screen) if (d.text == "#CURSOR") cursorY = d.y;
        drafted = cursorY >= 0 && cursorY < composeCursorY0 && T::visibleJoined().find("#CURSOR|E|") == std::string::npos;
        want = "(the rotation)";
      } else {
        drafted = T::visibleJoined().find("#CURSOR|" + want + "|") != std::string::npos;
      }
      CHECK(r.maxPassOpens <= kLoadingPassOpens + 2 && drafted, "%s, keys after %d: max %llu opens in a pass, draft %s shown %d",
            kChat[enigma], after, (unsigned long long)r.maxPassOpens, want.c_str(), drafted);
      closeChat();
    }
  }

  g_test = "IN2e held buffer full and the queue full across two passes: the oldest 64 events are kept in order (the newest dropped, counted, warned)";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    std::vector<InputEvent> evs;
    morseEvents(std::string(50, 'E').c_str(), evs);  // 100 events
    runLoading(std::vector<InputEvent>(evs.begin(), evs.begin() + 32), 32, 1);  // 32 held
    LoadRun r = runLoading(std::vector<InputEvent>(evs.begin() + 32, evs.end()), 68);  // 32 queued, 36 dropped
    loopPass(30);
    std::string vis = T::visibleJoined();
    size_t es = 0;
    for (size_t k = vis.find("EEEE"); k != std::string::npos && k < vis.size() && vis[k] == 'E'; k++) es++;
    bool warned = vis.find("Input lost while loading") != std::string::npos;
    CHECK(T::inputDropped == 36 && es == 32 && warned && r.maxPassOpens <= kLoadingPassOpens,
          "%s: %u dropped (expected 36), %zu E kept (expected 32), warned %d, max %llu opens", kChat[enigma], T::inputDropped, es, warned,
          (unsigned long long)r.maxPassOpens);
    closeChat();
  }

  g_test = "IN3 no memory for the held buffer while loading: the input waits (bounded passes), nothing lost";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    g_failNothrowArrayNew = true;  // the held-input buffer cannot be allocated
    std::vector<InputEvent> evs;
    morseEvents("SOS", evs);
    LoadRun r = runLoading(evs, evs.size());
    g_failNothrowArrayNew = false;
    loopPass(30);
    bool drafted = T::visibleJoined().find("SOS") != std::string::npos;
    CHECK(r.passes > 8 && r.maxPassOpens <= kLoadingPassOpens && drafted && T::inputDropped == 0,
          "%s: %d passes, max %llu opens in a pass, draft SOS shown %d", kChat[enigma], r.passes, (unsigned long long)r.maxPassOpens,
          drafted);
    closeChat();
  }

  g_test = "IN3b no memory for the held buffer, then Back: leaves at once";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    g_failNothrowArrayNew = true;
    std::vector<InputEvent> evs;
    morseEvents("EE", evs);
    evs.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
    LoadRun r = runLoading(evs, evs.size(), 3);
    g_failNothrowArrayNew = false;
    CHECK(r.left && r.passes <= 1 && r.maxPassOpens <= kLoadingPassOpens, "%s: left %d after %d passes, %llu opens", kChat[enigma],
          r.left, r.passes, (unsigned long long)r.maxPassOpens);
    closeChat();
  }

  g_test = "IN4d a Send and its replayed Short arriving after the held buffer overflowed: one send attempt";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    T::connected = false;
    LittleFS.failOpen = [](const std::string& p) { return p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0; };  // draft kept
    std::vector<InputEvent> evs;
    morseEvents(twentyE.c_str(), evs);
    evs.push_back(InputEvent(InputEventType::ENCODER_SHORT, 0, 120, T::now - 10));
    evs.push_back(InputEvent(InputEventType::ENCODER_SHORT, 0, 120, T::now - 10));  // replayed
    size_t before = std::count(T::seq.begin(), T::seq.end(), std::string("nextId"));
    LoadRun r = runLoading(evs, 12);
    loopPass(30);
    size_t attempts = std::count(T::seq.begin(), T::seq.end(), std::string("nextId")) - before;
    LittleFS.failOpen = nullptr;
    T::connected = true;
    CHECK(attempts == 1 && r.maxPassOpens <= kLoadingPassOpens, "%s: %zu send attempts, max %llu opens in a pass", kChat[enigma], attempts,
          (unsigned long long)r.maxPassOpens);
    closeChat();
  }

  g_test = "IN5b representative Morse keyed while loading (buffer overflowing, or no memory for it) gives the same draft as keyed live";
  for (int enigma = 0; enigma < 2; enigma++) {
    for (int mode = 0; mode < 3; mode++) {  // 0 live (loaded), 1 loading with overflow, 2 loading without memory
      // Without a held buffer the device keeps 1 carried event + its 32-event
      // input queue: PARIS (28 events) fits, PARISTEST (48) does not (IN5c).
      const char* word = mode == 2 ? "PARIS" : "PARISTEST";
      if (mode == 0) {
        coldEveryone();
        T::inputCap = 32;
        if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
        else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
        waitChatDrawn();
        deviceCost();
      } else {
        openCold(enigma);
        if (mode == 2) g_failNothrowArrayNew = true;
      }
      std::vector<InputEvent> evs;
      morseEvents(word, evs, true);
      LoadRun r = runLoading(evs, 0);
      g_failNothrowArrayNew = false;
      loopPass(30);
      bool drafted = T::visibleJoined().find(word) != std::string::npos;
      CHECK(drafted && T::inputDropped == 0 && r.maxPassOpens <= kLoadingPassOpens, "%s mode %d: draft %s shown %d, %u dropped, max %llu opens",
            kChat[enigma], mode, word, drafted, T::inputDropped, (unsigned long long)r.maxPassOpens);
      closeChat();
    }
  }

  g_test = "IN5c more Morse than the device can keep without a held buffer (48 events): the loss is shown, never a silently different draft";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    g_failNothrowArrayNew = true;
    std::vector<InputEvent> evs;
    morseEvents("PARISTEST", evs, true);
    bool noticeWhileLoading = false;
    {
      size_t next = 0;
      for (int i = 0; i < 3000; i++) {
        while (next < evs.size() && static_cast<int32_t>(T::now - evs[next].eventMs) >= 0) T::raw.push_back(evs[next++]);
        loopPass();
        noticeWhileLoading = noticeWhileLoading || T::visibleJoined().find("Input lost while loading") != std::string::npos;
        if (next >= evs.size() && !chatLoading()) break;
      }
    }
    g_failNothrowArrayNew = false;
    loopPass(2);
    bool noticeAfter = T::visibleJoined().find("Input lost while loading") != std::string::npos;
    bool full = T::visibleJoined().find("PARISTEST") != std::string::npos;
    CHECK((noticeWhileLoading || noticeAfter) && !full, "%s: notice while loading %d, after %d; full draft shown %d", kChat[enigma],
          noticeWhileLoading, noticeAfter, full);
    closeChat();
  }

  // ---- review round 4: Back into a full queue; loss on the completing pass ----
  g_test = "IN9u the production input queue (core/input_queue.h): FIFO order, drops counted; a Back reaching a full queue is kept, displacing the newest other event (counted)";
  {
    InputQueue q;
    for (int i = 0; i < 33; i++) q.push(InputEvent(InputEventType::ENCODER_ROTATE, 1, 0, static_cast<uint32_t>(100 + i)));
    bool fullAfter32 = q.full() && q.size() == 32 && q.dropped() == 1;  // the 33rd ordinary event: dropped, as before
    q.push(InputEvent(InputEventType::ENCODER_LONG, 0, 800, 999));
    std::vector<uint32_t> order;
    bool backLast = false;
    InputEvent e;
    while (q.pop(e)) { order.push_back(e.eventMs); backLast = e.type == InputEventType::ENCODER_LONG; }
    bool ordered = order.size() == 32 && order.back() == 999;
    for (size_t i = 0; i + 1 < order.size() && ordered; i++) ordered = order[i] == 100 + i;  // 100..130, then the Back
    CHECK(fullAfter32 && ordered && backLast && q.dropped() == 2, "full after 32 %d; order kept with the Back last %d (%zu events); dropped %u",
          fullAfter32, ordered && backLast, order.size(), q.dropped());
    InputQueue b;
    for (int i = 0; i < 32; i++) b.push(InputEvent(InputEventType::ENCODER_LONG, 0, 800, static_cast<uint32_t>(i)));
    b.push(InputEvent(InputEventType::ENCODER_LONG, 0, 800, 77));
    CHECK(b.size() == 32 && b.dropped() == 1, "a queue of Backs: a further Back dropped (%u kept, %u dropped)", b.size(), b.dropped());
  }

  g_test = "IN9 a Back generated while the input queue is already full (held buffer full, chat still loading) is recognized: the chat leaves in that pass, no synchronous load";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    std::vector<InputEvent> evs;
    morseEvents(std::string(32, 'E').c_str(), evs);  // 64 events
    runLoading(std::vector<InputEvent>(evs.begin(), evs.begin() + 32), 32, 1);  // pass 1: 32 held
    bool loading = chatLoading();
    // Pass 2: one input update brings 32 more (the queue is then full) and,
    // behind them, the Back -- before the chat can look at the queue.
    std::vector<InputEvent> burst(evs.begin() + 32, evs.end());
    burst.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
    LoadRun r = runLoading(burst, burst.size(), 6);
    CHECK(loading && r.left && r.passes <= 1 && r.maxPassOpens <= kLoadingPassOpens && T::inputDropped >= 1,
          "%s: loading %d; left %d after %d passes, max %llu opens; %u displaced/dropped", kChat[enigma], loading, r.left, r.passes,
          (unsigned long long)r.maxPassOpens, T::inputDropped);
    closeChat();
  }

  g_test = "IN10 input lost by the very input update of the pass in which loading completes is still reported (\"Input lost while loading\")";
  for (int enigma = 0; enigma < 2; enigma++) {
    openCold(enigma);
    std::vector<InputEvent> evs;
    morseEvents(std::string(34, 'E').c_str(), evs);  // 68 events
    runLoading(std::vector<InputEvent>(evs.begin(), evs.begin() + 32), 32, 1);       // 32 held
    runLoading(std::vector<InputEvent>(evs.begin() + 32, evs.begin() + 63), 31, 1);  // 31 queued: not full, nothing lost yet
    // Until one record is left: the next pass's load step completes the index.
    uint16_t c = 0, rd = 0, tot = 0;
    for (int i = 0; i < 3000; i++) {
      MessageStore::stepConversationIndex(kG, MessageStore::kEveryone, 0, &c, &rd, &tot);  // (reads nothing)
      if (tot > 0 && rd + 1 >= tot) break;
      loopPass();
    }
    bool oneLeft = tot > 0 && rd + 1 == tot && chatLoading();
    uint32_t droppedBefore = T::inputDropped;
    // That pass: the load completes, then its input update overflows the queue.
    runLoading(std::vector<InputEvent>(evs.begin() + 63, evs.end()), 5, 1);
    bool completed = !chatLoading();
    bool dropped = T::inputDropped > droppedBefore;
    loopPass(2);
    bool warned = T::visibleJoined().find("Input lost while loading") != std::string::npos;
    CHECK(oneLeft && completed && dropped && warned, "%s: one record left %d, completed in that pass %d, input dropped %d, warning shown %d",
          kChat[enigma], oneLeft, completed, dropped, warned);
    closeChat();
  }

  g_test = "IN11 no false warning: input dropped before the visit (another screen), or lost in a previous visit, is not reported in a new one";
  for (int enigma = 0; enigma < 2; enigma++) {
    coldEveryone();
    deviceCost();
    T::inputCap = 32;
    // On another screen: a burst the queue cannot take (8 dropped), then drained there.
    std::vector<InputEvent> junk;
    morseEvents(std::string(20, 'E').c_str(), junk);
    for (auto& j : junk) T::raw.push_back(j);
    Input::update();
    InputEvent x;
    while (Input::popEvent(x)) {}
    bool droppedElsewhere = T::inputDropped > 0;
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    std::vector<InputEvent> evs;
    morseEvents("SOS", evs, true);
    runLoading(evs, 0);
    loopPass(30);
    bool warned1 = T::visibleJoined().find("Input lost while loading") != std::string::npos;
    bool drafted = T::visibleJoined().find("SOS") != std::string::npos;
    // A visit that did lose input, then a new visit.
    back();
    coldIndex();
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    loopPass();
    std::vector<InputEvent> lots;
    morseEvents(std::string(50, 'E').c_str(), lots);
    runLoading(lots, lots.size());
    loopPass(5);
    bool warnedLossVisit = T::visibleJoined().find("Input lost while loading") != std::string::npos;
    back();
    coldIndex();
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    runLoading({}, 0);
    loopPass(5);
    bool warned3 = T::visibleJoined().find("Input lost while loading") != std::string::npos;
    CHECK(droppedElsewhere && !warned1 && drafted && warnedLossVisit && !warned3,
          "%s: dropped elsewhere %d, warned on the next visit %d (draft SOS %d); lossy visit warned %d; the visit after it warned %d",
          kChat[enigma], droppedElsewhere, warned1, drafted, warnedLossVisit, warned3);
    closeChat();
  }

  g_test = "IN11b input dropped while a chat is already loaded (not loading) does not raise the \"while loading\" warning";
  for (int enigma = 0; enigma < 2; enigma++) {
    coldEveryone();
    T::inputCap = 32;
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    waitChatDrawn();
    std::vector<InputEvent> evs;
    morseEvents(std::string(20, 'E').c_str(), evs);  // 40 in one update: 8 dropped by the queue
    for (auto& e2 : evs) T::raw.push_back(e2);
    loopPass(5);
    bool dropped = T::inputDropped > 0;
    bool warned = T::visibleJoined().find("Input lost while loading") != std::string::npos;
    CHECK(dropped && !warned, "%s: dropped %d, warned %d", kChat[enigma], dropped, warned);
    closeChat();
  }

  // A Send whose draft is kept (offline and the store write failing) and the
  // same Short replayed by the encoder (same release time): one attempt.
  auto sendWithReplay = [&](bool enigma, bool reloadFirst) {
    coldEveryone();
    T::connected = false;
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    if (reloadFirst) {
      waitChatDrawn();
      coldIndex();
      if (enigma) Enigma::markConversationChanged(kG, MessageStore::kEveryone);
      else TextMessage::markConversationChanged(kG, MessageStore::kEveryone);
    }
    loopPass();
    LittleFS.failOpen = [](const std::string& p) { return p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0; };
    uint32_t t = T::now;
    T::raw.push_back(InputEvent(InputEventType::DOT_PRESS_START, 0, 0, t - 300));
    T::raw.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, 60, t - 240));
    T::raw.push_back(InputEvent(InputEventType::ENCODER_SHORT, 0, 120, t - 10));
    T::raw.push_back(InputEvent(InputEventType::ENCODER_SHORT, 0, 120, t - 10));  // replayed
    size_t before = std::count(T::seq.begin(), T::seq.end(), std::string("nextId"));
    // Until everything held was replayed: the failed store write makes the
    // index stale, so a reload (with "Loading messages...") can follow the Send.
    for (int settled = 0, i = 0; i < 6000 && settled < 40; i++) {
      loopPass();
      bool busy = g_indexDirty || !T::raw.empty() || T::visibleJoined().find("Loading messages") != std::string::npos ||
                  T::visibleJoined().find("Sending...") != std::string::npos;
      settled = busy ? 0 : settled + 1;
    }
    size_t attempts = std::count(T::seq.begin(), T::seq.end(), std::string("nextId")) - before;
    LittleFS.failOpen = nullptr;
    T::connected = true;
    back();
    return attempts;
  };

  g_test = "IN4 a Send and its replayed Short, both received while loading: one send attempt";
  {
    size_t n = sendWithReplay(false, false);
    CHECK(n == 1, "%zu send attempts", n);
  }
  g_test = "IN4b the same while a reload during the visit holds the input: one send attempt";
  {
    size_t n = sendWithReplay(false, true);
    CHECK(n == 1, "%zu send attempts", n);
  }
  g_test = "IN4c Enigma: a Send and its replayed Short, both received while loading: one send attempt";
  {
    size_t n = sendWithReplay(true, false);
    CHECK(n == 1, "%zu send attempts", n);
  }

  g_test = "IN5 one Morse sequence (SOS) keyed while the chat loads gives the same draft as keyed live (device-cost model, 63 ms per open)";
  {
    auto keySos = [](uint32_t t0) {
      // S O S at 15 wpm (dit 80 ms): dots 60 ms, dashes 240 ms, 80 ms symbol gaps, 400 ms letter gaps
      uint32_t t = t0;
      const char* pattern[3] = {"...", "---", "..."};
      for (int l = 0; l < 3; l++) {
        for (const char* c = pattern[l]; *c; c++) {
          uint32_t dur = *c == '.' ? 60 : 240;
          T::raw.push_back(InputEvent(InputEventType::DOT_PRESS_START, 0, 0, t));
          T::raw.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, dur, t + dur));
          t += dur + 80;
        }
        t += 320;
      }
      return t;
    };
    std::string live, held;
    for (int mode = 0; mode < 2; mode++) {
      coldEveryone();
      deviceCost();
      TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
      if (mode == 0) waitChatDrawn();
      else loopPass();
      bool loading = mode == 1 && T::visibleJoined().find("Loading messages") != std::string::npos;
      // Keyed over the next ~3 s; each event becomes visible to the chat once its time has passed.
      uint32_t t0 = T::now + 20;
      std::deque<InputEvent> evs;
      std::swap(evs, T::raw);
      keySos(t0);
      std::swap(evs, T::raw);
      while (!evs.empty() || T::now < t0 + 4000) {
        while (!evs.empty() && evs.front().eventMs <= T::now) { T::raw.push_back(evs.front()); evs.pop_front(); }
        loopPass();
      }
      for (int i = 0; i < 3000 && g_indexDirty; i++) loopPass();
      loopPass(40);
      (mode == 0 ? live : held) = std::string(g_composeText, g_composeLen);
      if (mode == 1) CHECK(loading, "precondition: the chat was not loading while keyed");
      LittleFS.onFileOpen = nullptr;
      back();
    }
    CHECK(live == "SOS" && held == live, "live draft '%s', keyed while loading '%s'", live.c_str(), held.c_str());
  }

  g_test = "IN6 a loaded chat: Back queued after 20 rotations leaves in the same pass without reading rows for them (device-cost model)";
  for (int enigma = 0; enigma < 2; enigma++) {
    freshDevice();
    everyoneHistory(90);
    powerOn(); runFor(120);
    if (enigma) openEnigma(MessageStore::kEveryone);
    else openText(MessageStore::kEveryone);
    deviceCost();
    for (int i = 0; i < 20; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
    FsStats a = g_fs;
    uint32_t start = T::now;
    loopPass();
    uint32_t ms = T::now - start;
    bool left = !TextMessage::isAnyConversationOpen();
    printf("   IN6%s measured: left after one pass of %u ms simulated, %llu opens (20 rotations, then Back)\n", enigma ? "c" : "", ms,
           (unsigned long long)(g_fs.fileOpens - a.fileOpens));
    CHECK(left && g_fs.fileOpens - a.fileOpens <= 1, "%s: left %d, %llu opens", enigma ? "Enigma" : "Text", left,
          (unsigned long long)(g_fs.fileOpens - a.fileOpens));
    LittleFS.onFileOpen = nullptr;
    loopPass(3);
  }

  g_test = "IN6b a Send queued before a rotation and a Back is still sent; nothing after the rotation is handled";
  freshDevice();
  everyoneHistory(20);
  powerOn(); runFor(60);
  {
    openText(MessageStore::kEveryone);
    uint32_t t = T::now;
    T::raw.push_back(InputEvent(InputEventType::DOT_PRESS_START, 0, 0, t - 300));
    T::raw.push_back(InputEvent(InputEventType::DOT_RELEASE, 0, 60, t - 240));
    T::raw.push_back(InputEvent(InputEventType::ENCODER_SHORT, 0, 120, t - 100));
    T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    T::raw.push_back(InputEvent(InputEventType::ENCODER_SHORT, 0, 120, t - 5));  // would select/act in history
    T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, t));
    size_t before = onFlash(kG, MessageStore::kEveryone).size();
    loopPass(3);
    CHECK(!TextMessage::isConversationOpen(kG, MessageStore::kEveryone) && onFlash(kG, MessageStore::kEveryone).size() == before + 1,
          "left %d, %zu messages stored by the Send (expected 1)", !TextMessage::isConversationOpen(kG, MessageStore::kEveryone),
          onFlash(kG, MessageStore::kEveryone).size() - before);
  }

  g_test = "IN7 Back while the chat loads leaves at once (held input dropped with it)";
  for (int enigma = 0; enigma < 2; enigma++) {
    coldEveryone();
    if (enigma) Enigma::navigateToChatDirect(kG, MessageStore::kEveryone);
    else TextMessage::navigateToChatDirect(kG, MessageStore::kEveryone);
    loopPass();
    for (int i = 0; i < 10; i++) T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    loopPass();
    T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
    FsStats a = g_fs;
    loopPass();
    CHECK(!TextMessage::isAnyConversationOpen() && g_heldInput.empty() && g_fs.fileOpens - a.fileOpens <= 4,
          "%s: left %d, held left %d, %llu opens", enigma ? "Enigma" : "Text", !TextMessage::isAnyConversationOpen(),
          !g_heldInput.empty(), (unsigned long long)(g_fs.fileOpens - a.fileOpens));
    loopPass(3);
  }

  // ---- Outbox: incremental, header-only, durable pending records ----------------
  auto pendingCount = [](const char* contact) {
    int n = 0;
    for (auto& kv : onFlash(kG, contact)) n += (static_cast<uint8_t>(kv.second[1]) & MessageStore::FLAG_PENDING_OUTBOX) ? 1 : 0;
    return n;
  };
  auto pubsFor = [](const std::string& topicPart) {
    int n = 0;
    for (auto& p : T::pubs) n += (p.result && p.topic.find(topicPart) != std::string::npos) ? 1 : 0;
    return n;
  };

  g_test = "OB1 pending records left on flash are found by an incremental pass and published once each; no step reads more than 4 records";
  freshDevice();
  for (int i = 0; i < 60; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  for (int i = 0; i < 5; i++) storePending(kBob, 1700005000u + i);
  MessageStore::notifyExternalStorageMutationAttempted();
  {
    powerOn();
    g_svcMax = PassMax();
    runFor(120);
    CHECK(pendingCount(kBob) == 0 && pubsFor("msg/" + std::string(kBob)) == 5, "pending left %d, published %d", pendingCount(kBob),
          pubsFor("msg/" + std::string(kBob)));
    CHECK(g_svcMax.opens <= 8, "max %llu opens in a service pass", (unsigned long long)g_svcMax.opens);
    FsStats a = g_fs;
    runFor(60);
    CHECK(g_fs.fileOpens == a.fileOpens, "%llu opens while idle after the pass (the pass ran again)",
          (unsigned long long)(g_fs.fileOpens - a.fileOpens));
  }

  g_test = "OB2 a send while offline is queued directly: published on connection with no storage pass";
  freshDevice();
  everyoneHistory(30);
  powerOn(); runFor(60);
  {
    T::connected = false;
    MessageRef r = storePending(kBob, 1700009000u);
    runFor(10);
    unsigned listingsBefore = 0;
    FsStats a = g_fs;
    T::connected = true;
    runFor(10);
    (void)listingsBefore;
    CHECK(pendingCount(kBob) == 0 && pubsFor("msg/" + std::string(kBob)) == 1 && g_fs.dirOpens - a.dirOpens <= 1,
          "pending %d, published %d, %llu directory opens", pendingCount(kBob), pubsFor("msg/" + std::string(kBob)),
          (unsigned long long)(g_fs.dirOpens - a.dirOpens));
    (void)r;
  }

  g_test = "OB3 a pending record survives reboots while offline and is published once after connecting";
  freshDevice();
  {
    T::connected = false;
    powerOn(); runFor(10);
    storePending(kBob, 1700009000u);
    runFor(10);
    reboot(); runFor(30);
    reboot(); runFor(30);
    int offlinePubs = static_cast<int>(T::pubs.size());
    CHECK(pendingCount(kBob) == 1, "pending %d after reboots offline", pendingCount(kBob));
    T::connected = true;
    runFor(30);
    CHECK(pendingCount(kBob) == 0 && pubsFor("msg/" + std::string(kBob)) == 1, "after connecting: pending %d, published %d (offline attempts %d)",
          pendingCount(kBob), pubsFor("msg/" + std::string(kBob)), offlinePubs);
  }

  g_test = "OB4 a failed publish is retried later; the record stays pending until a publish succeeds; never published twice";
  freshDevice();
  powerOn(); runFor(10);
  {
    T::publishScript = {false};
    T::onPublished = nullptr;
    storePending(kBob, 1700009000u);
    loopPass(5);
    int pendingAfterFail = pendingCount(kBob);
    T::connected = true;  // the failed publish closed the connection; reconnected
    runFor(30);
    CHECK(pendingAfterFail == 1 && pendingCount(kBob) == 0 && pubsFor("msg/" + std::string(kBob)) == 1,
          "after the failure: pending %d; then pending %d, published %d", pendingAfterFail, pendingCount(kBob),
          pubsFor("msg/" + std::string(kBob)));
  }

  g_test = "OB5 more pending records than the queue holds (40, after the boot pass): every one published (a further pass finds the rest), each once";
  freshDevice();
  {
    powerOn(); runFor(30);  // the boot pass is done
    T::connected = false;
    for (int i = 0; i < 40; i++) storePending(kBob, 1700009000u + i);
    T::connected = true;
    runFor(180);
    std::map<std::string, int> seen;
    for (auto& p : T::pubs) if (p.result) seen[std::string(p.data.begin(), p.data.end())]++;
    bool once = true;
    for (auto& kv : seen) once = once && kv.second == 1;
    CHECK(pendingCount(kBob) == 0 && seen.size() == 40 && once, "pending %d, %zu distinct published, each once %d", pendingCount(kBob),
          seen.size(), once);
  }

  g_test = "OB6 a change made outside the store starts a new pass; a pending record it left is found";
  freshDevice();
  powerOn(); runFor(30);
  {
    T::connected = false;
    storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u);
    PacketCodec::MessageEnvelope env = envOf("DEADBEEFAAAA", PacketCodec::MSG_TYPE_TEXT, 1700009100u);
    writeLegacyRecord(kG, kBob, MessageStore::Direction::SENT, MessageStore::FLAG_PENDING_OUTBOX, 1700009100u, env, "OUT");  // notifies
    T::connected = true;
    runFor(30);
    CHECK(pendingCount(kBob) == 0 && pubsFor("msg/" + std::string(kBob)) == 1, "pending %d, published %d", pendingCount(kBob),
          pubsFor("msg/" + std::string(kBob)));
  }

  g_test = "OB7 a pending record that cannot be read during the pass is not skipped: a later pass publishes it";
  freshDevice();
  {
    PacketCodec::MessageEnvelope env = envOf("DEADBEEFAAAA", PacketCodec::MSG_TYPE_TEXT, 1700009100u);
    MessageRef r = writeLegacyRecord(kG, kBob, MessageStore::Direction::SENT, MessageStore::FLAG_PENDING_OUTBOX, 1700009100u, env, "OUT");
    static char name[32];
    snprintf(name, sizeof(name), "/%08lu.msg", static_cast<unsigned long>(r.sequence));
    LittleFS.failOpen = [](const std::string& p) { return p.find(kBob) != std::string::npos && p.find(name) != std::string::npos; };
    powerOn(); runFor(20);
    int during = pubsFor("msg/" + std::string(kBob));
    LittleFS.failOpen = nullptr;
    runFor(30);
    CHECK(during == 0 && pendingCount(kBob) == 0 && pubsFor("msg/" + std::string(kBob)) == 1, "during %d; then pending %d, published %d",
          during, pendingCount(kBob), pubsFor("msg/" + std::string(kBob)));
  }

  // ---- Outbox, review round: a queued record that cannot be read at publication ----
  // Counts (and optionally fails) the opens of one record file.
  static std::string s_watchName;
  static bool s_watchFail = false;
  static int s_watchOpens = 0;
  auto watchRecord = [](const char* contact, uint32_t seq, bool fail) {
    char name[64];
    snprintf(name, sizeof(name), "/%s/%08lu.msg", contact, static_cast<unsigned long>(seq));
    s_watchName = name;
    s_watchFail = fail;
    s_watchOpens = 0;
    LittleFS.failOpen = [](const std::string& p) {
      if (p.size() < s_watchName.size() || p.compare(p.size() - s_watchName.size(), s_watchName.size(), s_watchName) != 0) return false;
      s_watchOpens++;
      return s_watchFail;
    };
  };

  g_test = "OB8 a queued record whose read fails at publication (after the boot pass) is retried, not dropped: published once its reads work, flag cleared";
  for (int dirToo = 0; dirToo < 2; dirToo++) {  // 1: its conversation cannot be listed either (absence unconfirmed)
  freshDevice();
  powerOn(); runFor(30);  // the boot pass is done; nothing else will look at storage
  {
    watchRecord(kCarol, 1, true);  // Carol's first record: unreadable from now on
    static bool s_dirFail = false;
    s_dirFail = dirToo;
    if (dirToo) {
      LittleFS.failOpen = [](const std::string& p) {
        bool rec = p.size() >= s_watchName.size() && p.compare(p.size() - s_watchName.size(), s_watchName.size(), s_watchName) == 0;
        bool dir = s_dirFail && p.size() >= 15 && p.compare(p.size() - 15, 15, "/CA201000000001") == 0;
        if (rec) s_watchOpens++;
        return (rec && s_watchFail) || (dir && s_watchFail);
      };
    }
    storePending(kCarol, 1700009000u);  // the normal store path: queued directly
    loopPass(50);
    int failedReads = s_watchOpens;
    int pubsDuring = pubsFor("msg/" + std::string(kCarol));
    s_watchFail = false;  // reads work again: no reboot, no new message, no outside change
    runFor(120);
    LittleFS.failOpen = nullptr;
    CHECK(failedReads >= 1 && pubsDuring == 0, "precondition: %d failed reads at publication, %d published meanwhile", failedReads, pubsDuring);
    CHECK(pubsFor("msg/" + std::string(kCarol)) == 1 && pendingCount(kCarol) == 0,
          "listing fails too %d: after reads recovered: published %d (expected 1), still pending %d", dirToo,
          pubsFor("msg/" + std::string(kCarol)), pendingCount(kCarol));
  }
  }

  g_test = "OB7b a record that stays unreadable during discovery: the repeated passes back off (no whole-store pass every few seconds)";
  freshDevice();
  for (int i = 0; i < 20; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  {
    PacketCodec::MessageEnvelope env = envOf("DEADBEEFAAAA", PacketCodec::MSG_TYPE_TEXT, 1700009100u);
    MessageRef r = writeLegacyRecord(kG, kBob, MessageStore::Direction::SENT, MessageStore::FLAG_PENDING_OUTBOX, 1700009100u, env, "OUT");
    watchRecord(kBob, r.sequence, true);
    // The Notifications sweep retries the same record too (its own bounded
    // backoff): measured offline first, where no Outbox pass runs.
    T::connected = false;
    powerOn(); runFor(300);  // both backoffs at their steady pace
    int a = s_watchOpens;
    runFor(300);
    int sweepOnly = s_watchOpens - a;
    T::connected = true;
    runFor(300);
    int b = s_watchOpens;
    runFor(300);
    int both = s_watchOpens - b;
    LittleFS.failOpen = nullptr;
    int passes = both - sweepOnly;  // each Outbox pass tries the unreadable record once
    printf("   OB7b measured: in 300 s, %d reads of the unreadable record offline (sweep), %d online: %d Outbox passes\n", sweepOnly,
           both, passes);
    CHECK(passes >= 1 && passes <= 4, "%d Outbox passes in 300 s (sweep %d, both %d)", passes, sweepOnly, both);
  }

  g_test = "OB9 a persistently unreadable queued record does not stop another from being published; it stays pending and queued, retried with backoff";
  freshDevice();
  powerOn(); runFor(30);
  {
    watchRecord(kBob, 1, true);
    storePending(kBob, 1700009000u);   // never readable
    storePending(kCarol, 1700009100u); // readable
    runFor(120);
    int attempts = s_watchOpens;
    CHECK(pubsFor("msg/" + std::string(kCarol)) == 1 && pendingCount(kCarol) == 0, "the readable record: published %d, pending %d",
          pubsFor("msg/" + std::string(kCarol)), pendingCount(kCarol));
    CHECK(pendingCount(kBob) == 1 && attempts >= 2 && attempts <= 12, "the unreadable one: pending %d, %d read attempts in 120 s",
          pendingCount(kBob), attempts);
    s_watchFail = false;
    runFor(120);
    LittleFS.failOpen = nullptr;
    CHECK(pubsFor("msg/" + std::string(kBob)) == 1 && pendingCount(kBob) == 0, "once readable: published %d, pending %d",
          pubsFor("msg/" + std::string(kBob)), pendingCount(kBob));
  }

  g_test = "OB10 a queued record confirmed gone (deleted before publication) is dropped after one read: no publish, no retries";
  freshDevice();
  powerOn(); runFor(30);
  {
    T::connected = false;
    MessageRef r = storePending(kCarol, 1700009000u);
    MessageStore::deleteMessage(r);
    watchRecord(kCarol, r.sequence, false);  // counts reads only
    T::connected = true;
    runFor(120);
    LittleFS.failOpen = nullptr;
    CHECK(pubsFor("msg/" + std::string(kCarol)) == 0 && s_watchOpens <= 1, "published %d, %d read attempts",
          pubsFor("msg/" + std::string(kCarol)), s_watchOpens);
  }

#include "pm_tests.inc"

  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
