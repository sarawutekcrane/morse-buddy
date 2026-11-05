// HOST MEASUREMENT (not production, not a pass/fail test). Runs the ACTUAL
// storage_messages.cpp on a real host directory (fake_fs/), with the ACTUAL
// menu.cpp, notifications.cpp, text_message.cpp and enigma.cpp, and counts
// the storage work done per main-loop pass:
//   B1  boot, then idle on the Main Menu (service ticks + menu ticks);
//   B2  open the largest conversation's Text chat and rotate through it;
//   B3  rotate the Main Menu while background work runs.
// B0b opens the LAST conversation's chat right after boot, before background
// work reaches it (the legacy-size history), and B1 reports when every
// conversation was trimmed to the retained history (if the build trims).
// Uses public APIs only, so the same file builds against 30ed514, 23293c5 and
// the fix candidate; the history is written in the record format directly
// (legacy_store.h), as older firmware left it. Host time is reported but is NOT device time; the
// operation counts are what carries over.
//
// Conversation 0 is the group's broadcast conversation (EVERYONE). With the
// optional msPerOpen, every file open advances the mock clock by that much
// (device A on 2ed04b6: about 63 ms), and B0b/B2 report the simulated time
// until the chat is drawn -- a cost model, not device timing.
//
// Usage: storage_bench <fsroot> <conversations> <messagesPerConversation> <seconds> [msPerOpen]
#include "stubs.h"
#include "core/menu.cpp"
#include "core/notifications.cpp"
#include "core/text_message.cpp"
#include "core/storage_messages.h"
#include "LittleFS.h"
#include <chrono>
#include <cstdio>
#include <string>
#include <filesystem>
#include "legacy_store.h"

namespace Sleep { void notifyActivity() {} }
void playTone(uint16_t, uint16_t, SoundClass) {}
static Preferences g_notifyPrefs;
namespace Storage { Preferences& notify() { return g_notifyPrefs; } }
void T_initServices();
void T_tickServices();

static const char* kG = "GROUP1";
static uint32_t g_msgNo = 0;

// Written in the on-flash record format directly (legacy_store.h): older
// firmware kept up to 300 records per conversation, which the current append
// path (retention 100) cannot build.
static void storeLegacy(const char* contact, uint8_t type, bool received, bool unread, uint32_t ts) {
  PacketCodec::MessageEnvelope env{};
  snprintf(env.message_id, sizeof(env.message_id), "MSG%020u", ++g_msgNo);
  env.schema_version = 1;
  env.message_type = type;
  strncpy(env.sender_device_id, received ? contact : "DEADBEEFAAAA", sizeof(env.sender_device_id) - 1);
  strncpy(env.sender_name_cache, "Bob", sizeof(env.sender_name_cache) - 1);
  strncpy(env.group_code, kG, sizeof(env.group_code) - 1);
  env.timestamp = ts;
  writeLegacyRecord(kG, contact, received ? MessageStore::Direction::RECEIVED : MessageStore::Direction::SENT,
                    unread ? MessageStore::FLAG_UNREAD : 0, ts, env, "hello there, this is a stored message");
}

// Records on "flash" (host directory, read directly: no store counters move).
static size_t recordsOnFlash(size_t* largest) {
  size_t total = 0;
  *largest = 0;
  std::error_code ec;
  for (auto& c : std::filesystem::directory_iterator(LittleFS.root + "/messages/" + kG, ec)) {
    size_t n = 0;
    for (auto& f : std::filesystem::directory_iterator(c.path(), ec)) n += f.path().extension() == ".msg";
    total += n;
    if (n > *largest) *largest = n;
  }
  return total;
}

struct Snap {
  FsStats fs;
  int nvs;
};
static Snap snap() { return Snap{g_fs, g_notifyPrefs.writes}; }

struct Worst {
  uint64_t opens = 0, dirEntries = 0, writes = 0, renames = 0, removes = 0;
  int nvs = 0;
  double us = 0;
  uint64_t calls = 0, callsOver8Opens = 0, totalOpens = 0, totalDir = 0, totalWrites = 0, totalRemoves = 0;
  int totalNvs = 0;
  void add(const Snap& a, const Snap& b, double us_) {
    uint64_t o = b.fs.fileOpens - a.fs.fileOpens, d = b.fs.dirEntries - a.fs.dirEntries, w = b.fs.writes - a.fs.writes,
             r = b.fs.renames - a.fs.renames, x = b.fs.removes - a.fs.removes;
    int n = b.nvs - a.nvs;
    calls++;
    totalOpens += o; totalDir += d; totalWrites += w; totalNvs += n; totalRemoves += x;
    if (o > 8) callsOver8Opens++;
    if (o > opens) opens = o;
    if (d > dirEntries) dirEntries = d;
    if (w > writes) writes = w;
    if (r > renames) renames = r;
    if (x > removes) removes = x;
    if (n > nvs) nvs = n;
    if (us_ > this->us) this->us = us_;
  }
  void print(const char* name) const {
    printf("%-34s calls=%llu  per-call max: opens=%llu dirEntries=%llu writes=%llu renames=%llu removes=%llu nvs=%d hostUs=%.0f  | "
           "calls>8opens=%llu  totals: opens=%llu dirEntries=%llu writes=%llu removes=%llu nvs=%d\n",
           name, (unsigned long long)calls, (unsigned long long)opens, (unsigned long long)dirEntries,
           (unsigned long long)writes, (unsigned long long)renames, (unsigned long long)removes, nvs, us,
           (unsigned long long)callsOver8Opens, (unsigned long long)totalOpens, (unsigned long long)totalDir,
           (unsigned long long)totalWrites, (unsigned long long)totalRemoves, totalNvs);
  }
};

static double nowUs() {
  return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One main-loop pass: services, then the UI (as main.cpp's loop()).
static void loopPass(Worst* svc, Worst* ui) {
  T::now += 20;
  Snap a = snap();
  double t0 = nowUs();
  T_tickServices();
  double t1 = nowUs();
  Snap b = snap();
  Menu::tick();
  double t2 = nowUs();
  Snap c = snap();
  if (svc) svc->add(a, b, t1 - t0);
  if (ui) ui->add(b, c, t2 - t1);
}

static int unreadOnFlash(const char* contact) {
  struct Ctx { int n; } ctx{0};
  bool reliable = false;
  MessageStore::findMessagesByPredicate(
      kG, contact,
      [](const MessageStore::StoredHeader& h, const PacketCodec::MessageEnvelope&, void* p) {
        if (h.flags & MessageStore::FLAG_UNREAD) static_cast<Ctx*>(p)->n++;
        return false;
      },
      &ctx, nullptr, 0, &reliable);
  return ctx.n;
}

static void menuStub() {}

int main(int argc, char** argv) {
  if (argc < 5) { printf("usage: storage_bench <fsroot> <conversations> <msgs> <seconds> [msPerOpen]\n"); return 2; }
  LittleFS.root = argv[1];
  int convs = atoi(argv[2]), msgs = atoi(argv[3]), seconds = atoi(argv[4]);
  static uint32_t msPerOpen = argc > 5 ? static_cast<uint32_t>(atoi(argv[5])) : 0;
  LittleFS.wipe();
  T::reset();
  MessageStore::init();
  char contact[16];
  auto contactName = [&](int c) {
    if (c == 0) snprintf(contact, sizeof(contact), "%s", MessageStore::kEveryone);
    else snprintf(contact, sizeof(contact), "B%011d", c);
    return contact;
  };
  for (int c = 0; c < convs; c++) {
    contactName(c);
    for (int i = 0; i < msgs; i++) storeLegacy(contact, PacketCodec::MSG_TYPE_TEXT, i % 5 != 4, i % 5 != 4, 1700000000u + i * 60);
  }
  printf("history: %d conversations x %d messages (4 of 5 received, all received ones flagged unread as legacy firmware left them)\n",
         convs, msgs);

  if (msPerOpen > 0) LittleFS.onFileOpen = [](const std::string&) { T::now += msPerOpen; };
  // Opens the chat and runs passes until it is drawn (no "Loading messages..."):
  // returns the simulated time it took.
  auto openUntilDrawn = [&](const char* c, Worst* ui, Worst* svc) {
    TextMessage::navigateToChatDirect(kG, c);
    uint32_t start = T::now;
    for (int i = 0; i < 5000; i++) {
      loopPass(svc, ui);
      std::string v = T::visibleJoined();
      if (i > 0 && v.find("Loading messages") == std::string::npos && v.find("Bob") != std::string::npos) break;
    }
    return T::now - start;
  };
  // Boot: services' init, then the Main Menu.
  g_fs.clear();
  Menu::init(); g_stackSize = 1; Menu::consumeJustEntered();
  double t0 = nowUs();
  T_initServices();
  double tInit = nowUs() - t0;
  Snap afterInit = snap();
  printf("%-34s opens=%llu dirEntries=%llu writes=%llu nvs=%d hostUs=%.0f\n", "B0 boot: service init", (unsigned long long)afterInit.fs.fileOpens,
         (unsigned long long)afterInit.fs.dirEntries, (unsigned long long)afterInit.fs.writes, afterInit.nvs, tInit);

  // B0b: the last conversation's chat, opened at once (legacy-size history).
  contactName(convs - 1);
  Worst openLegacy;
  uint32_t b0bMs = openUntilDrawn(contact, &openLegacy, &openLegacy);
  openLegacy.print("B0b open chat at boot (until drawn)");
  if (msPerOpen > 0) printf("   B0b simulated: %u ms until drawn (%u ms per open)\n", b0bMs, msPerOpen);
  T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
  loopPass(nullptr, nullptr); loopPass(nullptr, nullptr);

  Worst idleSvc, idleUi;
  int passes = seconds * 1000 / 20;
  size_t largest = 0, total = recordsOnFlash(&largest);
  printf("   records on flash at boot: %zu (largest conversation %zu)\n", total, largest);
  int trimmedAtPass = -1;
  for (int i = 0; i < passes; i++) {
    loopPass(&idleSvc, &idleUi);
    if (trimmedAtPass < 0 && i % 50 == 49) {
      recordsOnFlash(&largest);
      if (largest <= 100) trimmedAtPass = i + 1;
    }
  }
  total = recordsOnFlash(&largest);
  if (trimmedAtPass >= 0)
    printf("   every conversation at <=100 records after %.1f s (simulated); now %zu records\n", trimmedAtPass * 0.02, total);
  else
    printf("   after %ds: %zu records, largest conversation %zu (not trimmed)\n", seconds, total, largest);
  idleSvc.print("B1 idle: services per loop pass");
  idleUi.print("B1 idle: Main Menu per loop pass");
  printf("   after %ds idle: Text badge=%d\n", seconds, Notifications::hasAnyUnreadText() ? 1 : 0);

  // B2: open the broadcast chat (conversation 0) and rotate up 20 messages.
  Worst openUi;
  uint32_t b2Ms = openUntilDrawn(contactName(0), &openUi, &openUi);
  openUi.print("B2 open chat (until drawn)");
  if (msPerOpen > 0) printf("   B2 simulated: %u ms until drawn\n", b2Ms);
  Worst rotUi, rotSvc;
  for (int i = 0; i < 20; i++) {
    T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, -1, 0, 0));
    loopPass(&rotSvc, &rotUi);
  }
  rotUi.print("B2 rotate x20: UI per pass");
  rotSvc.print("B2 rotate x20: services per pass");
  Worst afterSvc, afterUi;
  for (int i = 0; i < passes; i++) loopPass(&afterSvc, &afterUi);
  afterSvc.print("B2 then idle in chat: services");
  afterUi.print("B2 then idle in chat: UI (redraws)");
  printf("   unread on flash in %s after rotation + %ds: %d (of %d received)\n", MessageStore::kEveryone, seconds,
         unreadOnFlash(MessageStore::kEveryone), msgs - msgs / 5);

  // B3: back to the Main Menu, rotate it 20 times.
  T::raw.push_back(InputEvent(InputEventType::ENCODER_LONG, 0, 800, T::now));
  loopPass(nullptr, nullptr); loopPass(nullptr, nullptr);
  Worst menuUi, menuSvc;
  for (int i = 0; i < 20; i++) {
    T::raw.push_back(InputEvent(InputEventType::ENCODER_ROTATE, 1, 0, 0));
    loopPass(&menuSvc, &menuUi);
  }
  menuUi.print("B3 Main Menu rotate x20: UI");
  menuSvc.print("B3 Main Menu rotate x20: services");
  printf("   Text badge now=%d\n", Notifications::hasAnyUnreadText() ? 1 : 0);
  (void)menuStub;
  return 0;
}
