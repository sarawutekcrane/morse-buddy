// HOST TEST (not production). Unread marking against the REAL message store:
// the ACTUAL storage_messages.cpp on a real host directory (fake_fs/, every
// operation counted), with the ACTUAL menu.cpp, notifications.cpp,
// text_message.cpp and enigma.cpp. Flash truth (each record's FLAG_UNREAD) is
// read back through the real store, never through a fake.
//
// Covers hardware findings 1 and 3 on 23293c5: read up to the selected
// message (scoped to group, contact and message type), legacy unread flags,
// reboot and failure recovery, and bounded work per main-loop pass.
#include "stubs.h"
#include "core/menu.cpp"
#include "core/notifications.cpp"
#include "core/text_message.cpp"
#include "core/enigma.h"
#include "core/storage_messages.h"
#include "LittleFS.h"
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include "legacy_store.h"
#include <filesystem>

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
  delete Notifications::g_readUps;
  Notifications::g_readUps = nullptr;
  Notifications::g_readUpsDirty = false;
  Notifications::g_reconcileCursor = 0;
  Notifications::g_nextStepMs = T::now;
  Notifications::g_saveDueMs = 0;
  Notifications::g_readUpsDueMs = 0;
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
static void rot(int8_t v) {
  T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, v, 0, 0));
  loopPass();
}
static void back() {
  T::now += 500;
  T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
  loopPass(3);
}
static void openText(const char* contact) { TextMessage::navigateToChatDirect(kG, contact); loopPass(3); }
static void openEnigma(const char* contact) { Enigma::navigateToChatDirect(kG, contact); loopPass(3); }

static void menuStub() {}
static void powerOn() {  // first boot of a test: empty NVS unless the test wrote some
  resetNotifyVolatile();
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
  LittleFS.reverseListing = false;
  LittleFS.wipe();
  T::reset();
  g_notifyPrefs.kv.clear(); g_notifyPrefs.failRead = nullptr; g_notifyPrefs.failWrite = nullptr;
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

  g_test = "ST2 legacy flags: selecting the newest received message reads every older one of that type";
  freshDevice();
  for (int i = 0; i < 60; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, i % 5 != 4, 1700000000u + i * 60);
  powerOn(); runFor(60);
  CHECK(textBadge() && truth(kBob).text == 48, "legacy: badge %d unread %d", textBadge(), truth(kBob).text);
  openText(kBob);
  rot(-1);  // newest entry is a sent message: nothing to read
  CHECK(truth(kBob).text == 48, "a sent message selected read something");
  rot(-1);  // newest received message
  runFor(30);
  CHECK(truth(kBob).text == 0 && truth(kBob).files == 60, "after selecting the newest: unread %d files %d", truth(kBob).text,
        truth(kBob).files);
  // With no fault, the first attempt verifies its own rewrites (a second
  // pass) and completes: no failure recorded, no retry pause.
  CHECK(Notifications::g_readUps != nullptr && Notifications::g_readUps->count == 1 && Notifications::g_readUps->items[0].done &&
            Notifications::g_readUps->items[0].failures == 0,
        "a fault-free read-up was not completed by its first attempt");
  back();
  CHECK(!textBadge(), "badge still lit");
  reboot(); runFor(60);
  CHECK(!textBadge() && truth(kBob).text == 0, "after reboot: badge %d unread %d", textBadge(), truth(kBob).text);

  g_test = "ST3 read up to selection is scoped to the message type; newer messages stay unread";
  freshDevice(); powerOn();
  MessageRef a1 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000100u);
  MessageRef e1 = arrive(kBob, PacketCodec::MSG_TYPE_ENIGMA, 1700000200u);
  MessageRef g1 = arrive(kBob, PacketCodec::MSG_TYPE_GAME, 1700000300u);
  MessageRef a2 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000400u);
  MessageRef a3 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000500u);
  openText(kBob);
  rot(-1); rot(-1);  // a3, then a2 selected
  runFor(10);
  CHECK(!isUnread(a1) && !isUnread(a2), "older Text not read: a1 %d a2 %d", isUnread(a1), isUnread(a2));
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

  g_test = "ST3b stored earlier but shown below the selection (later timestamp): stays unread";
  freshDevice(); powerOn();
  MessageRef below = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000900u);  // stored first, newest timestamp
  MessageRef sel = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000500u);    // stored second, shown above it
  openText(kBob);
  T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
  rot(-1);  // two detents in one batch: passes over `below`, selects `sel`
  runFor(10);
  CHECK(!isUnread(sel) && isUnread(below), "selected %d below %d", isUnread(sel), isUnread(below));
  back();

  g_test = "ST3c stored after the selection but shown above it, while the read-up is still pending: stays unread";
  freshDevice(); powerOn();
  MessageRef s1 = arrive(kBob, PacketCodec::MSG_TYPE_TEXT, 1700000500u);
  openText(kBob);
  rot(-1);  // selects s1; the read-up is recorded, not yet applied
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

  g_test = "ST5 reboot while the read is still being applied: it resumes after reboot";
  freshDevice();
  for (int i = 0; i < 150; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn(); runFor(5);
  openText(kBob);
  rot(-1);
  loopPass(150);  // 3 s: saved, not finished
  int midway = truth(kBob).text;
  CHECK(midway > 0, "precondition: already finished (%d left)", midway);
  reboot(); runFor(120);
  CHECK(truth(kBob).text == 0 && !textBadge(), "after reboot: unread %d badge %d", truth(kBob).text, textBadge());

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
  CHECK(truth(kBob).text == 0, "with NVS failing: unread %d", truth(kBob).text);
  back();
  reboot(); runFor(60);
  g_notifyPrefs.failWrite = nullptr;
  runFor(30);
  CHECK(truth(kBob).text == 0 && !textBadge(), "after reboot: unread %d badge %d", truth(kBob).text, textBadge());

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

  g_test = "ST11 a record rewrite fails during a read-up: the next pass retries it, nothing is left unread";
  freshDevice();
  for (int i = 0; i < 20; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn(); runFor(30);
  {
    static int tmpFailures = 0;
    tmpFailures = 0;
    LittleFS.failOpen = [](const std::string& p) {  // the first 5 temp-file writes fail
      return p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0 && tmpFailures++ < 5;
    };
    openText(kBob); rot(-1); runFor(60);
    LittleFS.failOpen = nullptr;
    CHECK(tmpFailures >= 5 && truth(kBob).text == 0, "left unread %d (write failures %d)", truth(kBob).text, tmpFailures);
    back();
  }

  g_test = "ST12 a recount already under way when a read-up is selected does not light the badge again";
  freshDevice();
  for (int i = 0; i < 300; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn();
  {
    Notifications::Summary* b = nullptr;
    runFor(60);  // counted: 300
    // the stored summary is now exact; a reboot makes it UNRESOLVED and queues its recount
    reboot();
    loopPass(3);  // that recount has started
#ifndef NOTIFY_HEAD
    CHECK(Notifications::g_job != nullptr && Notifications::g_job->kind == Notifications::JOB_RECOUNT, "precondition: recount running");
#endif
    openText(kBob); rot(-1);
    bool relit = false;
    for (int i = 0; i < 4000 && truth(kBob).text > 0; i++) {
      loopPass();
      if (textBadge()) relit = true;
    }
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

  g_test = "ST19 read-up rewrites failing far beyond three passes (and across a reboot): never marked done, completes once writes work";
  freshDevice();
  for (int i = 0; i < 30; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  for (int i = 0; i < 5; i++) storeOnly(kCarol, PacketCodec::MSG_TYPE_ENIGMA, true, 1700000000u + i * 60);
  {
    static bool failing = false;
    powerOn(); runFor(30);
    failing = true;
    LittleFS.failOpen = [](const std::string& p) { return failing && p.size() > 4 && p.compare(p.size() - 4, 4, ".tmp") == 0; };
    openText(kBob); rot(-1); back();
    runFor(300);
    CHECK(truth(kBob).text == 30, "precondition: writes failing, nothing cleared (%d left)", truth(kBob).text);
    CHECK(Notifications::g_readUps != nullptr && Notifications::g_readUps->count == 1 && !Notifications::g_readUps->items[0].done &&
              Notifications::g_readUps->items[0].failures >= 2,
          "the failing read-up was dropped or marked done (or never retried)");
    storeOnly(kCarol, PacketCodec::MSG_TYPE_ENIGMA, true, 1800000000u);  // power-loss arrival: only the boot sweep finds it
    reboot();
    runFor(300);
    CHECK(truth(kBob).text == 30, "precondition after reboot: still failing (%d left)", truth(kBob).text);
    // The read-up is first in line after boot; the sweep must still get its turns.
    CHECK(Notifications::getUnreadCount(kG, kCarol) == 6, "other work stalled behind the failing read-up: Carol %u",
          Notifications::getUnreadCount(kG, kCarol));
    failing = false;
    runFor(600);
    CHECK(truth(kBob).text == 0 && Notifications::getUnreadCount(kG, kBob) == 0, "after writes work again: %d left, count %u",
          truth(kBob).text, Notifications::getUnreadCount(kG, kBob));
    LittleFS.failOpen = nullptr;
  }

  g_test = "ST20 power lost before the deferred read-up save: the selection is lost, nothing is wrongly read, counts follow flash";
  freshDevice();
  for (int i = 0; i < 40; i++) storeOnly(kBob, PacketCodec::MSG_TYPE_TEXT, true, 1700000000u + i * 60);
  powerOn(); runFor(30);
  openText(kBob); rot(-1);
  loopPass(10);  // 200 ms: before the ~1 s deferred save
  CHECK(!g_notifyPrefs.isKey("readup") || g_notifyPrefs.kv["readup"][0] == 0, "precondition: the read-up was already saved");
  reboot();
  runFor(120);
  CHECK(truth(kBob).text > 0 && textBadge() && Notifications::getUnreadCount(kG, kBob) == static_cast<uint16_t>(truth(kBob).text),
        "flash %d badge %d count %u", truth(kBob).text, textBadge(), Notifications::getUnreadCount(kG, kBob));

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

  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
