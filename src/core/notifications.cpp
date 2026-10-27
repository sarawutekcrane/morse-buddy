#include "core/notifications.h"

#include <Arduino.h>
#include <string.h>

#include <new>

#include "core/hooks.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/sleep.h"
#include "core/sound_facade.h"
#include "core/storage_init.h"

namespace Notifications {

namespace {

constexpr uint8_t kMaxSummaries = 24;

// Unread received messages per conversation, counted separately for each
// stored message type (hardware finding 3 on 30ed514: one shared count and a
// merged category mask kept every badge the conversation had ever lit while
// ANY of its messages stayed unread). The Main Menu badges derive from these:
// Text = unread TEXT or GAME, Enigma = unread ENIGMA or GAME -- a Friend game
// record appears in both chats' Unified Thread, as its arrival already
// announced (BADGE_TEXT | BADGE_ENIGMA).
enum TypeSlot : uint8_t { SLOT_TEXT = 0, SLOT_ENIGMA = 1, SLOT_GAME = 2, kTypeSlots = 3 };

// COUNTED: unread[] are exact per-type counts. UNRESOLVED: the counts are not
// known (a store scan or record read failed); unread[] then only flag, 1 or
// 0, which categories may have unread messages, so their badges stay lit.
// An UNRESOLVED conversation is recounted from the stored messages (the
// source of truth) once a scan is reliable again; its flags are never
// treated as counts.
enum SummaryState : uint8_t { SUMMARY_FREE = 0, SUMMARY_COUNTED = 1, SUMMARY_UNRESOLVED = 2 };

struct Summary {
  uint8_t state;
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  uint16_t unread[kTypeSlots];
};

// The record earlier firmware stored under "summary" (one count and a merged
// mask). Read to migrate; never written.
struct LegacySummary {
  bool active;
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  uint16_t unread_count;
  uint8_t badge_mask;
};

constexpr const char* kSummaryKey = "summary2";
constexpr const char* kLegacyKey = "summary";
// After every successful load (at boot, or when an unreadable record becomes
// readable) every stored conversation is recounted once from its messages,
// one per kRetryMs: the stored summary can miss an unread message that
// arrived while it could not be read or written, or whose arrival was cut off
// by a power loss after the message was stored (unread) but before it was
// counted -- and RAM, which may have known, does not survive a reboot. The
// stored messages are the only record that always does. The sweep's position
// is the last conversation checked, in (group, contact) order, so
// conversations added or removed meanwhile don't shift it. Built on the heap
// only while a sweep runs.
struct Sweep {
  bool started;
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
};

// LOAD_OK: g_summaries is authoritative. Otherwise a stored record exists but
// could not be read: nothing is written (so it is never overwritten, and an
// existing summary2 is never replaced from older legacy data) and the read is
// retried; arrivals and reads meanwhile are tracked in RAM and merged in as
// UNRESOLVED once the record is read, and the sweep after the load then
// recounts every conversation (which also covers what RAM lost to a reboot).
enum LoadState : uint8_t { LOAD_OK = 0, LOAD_RETRY_CURRENT, LOAD_RETRY_LEGACY };

constexpr uint32_t kRetryMs = 10000;

Summary g_summaries[kMaxSummaries];
LoadState g_loadState = LOAD_OK;
bool g_saveDirty = false;     // RAM is ahead of the stored record (a save failed or was deferred)
uint32_t g_nextRetryMs = 0;
uint8_t g_reconcileCursor = 0;
Sweep* g_sweep = nullptr;           // non-null while a sweep runs

int slotForType(uint8_t messageType) {
  switch (messageType) {
    case PacketCodec::MSG_TYPE_TEXT: return SLOT_TEXT;
    case PacketCodec::MSG_TYPE_ENIGMA: return SLOT_ENIGMA;
    case PacketCodec::MSG_TYPE_GAME: return SLOT_GAME;
    default: return -1;
  }
}

uint16_t totalUnread(const Summary& s) {
  return static_cast<uint16_t>(s.unread[SLOT_TEXT] + s.unread[SLOT_ENIGMA] + s.unread[SLOT_GAME]);
}

void setKeys(Summary* s, const char* group_code, const char* contact_key) {
  strncpy(s->group_code, group_code, sizeof(s->group_code) - 1);
  s->group_code[sizeof(s->group_code) - 1] = '\0';
  strncpy(s->contact_key, contact_key, sizeof(s->contact_key) - 1);
  s->contact_key[sizeof(s->contact_key) - 1] = '\0';
}

// Marks *s UNRESOLVED, keeping a flag for every category it may have unread
// (its own non-zero slots, plus the BADGE_* categories in `badgeMask`).
void markUnresolved(Summary* s, uint8_t badgeMask) {
  for (uint8_t i = 0; i < kTypeSlots; i++) s->unread[i] = s->unread[i] != 0 ? 1 : 0;
  if (badgeMask & BADGE_TEXT) s->unread[SLOT_TEXT] = 1;
  if (badgeMask & BADGE_ENIGMA) s->unread[SLOT_ENIGMA] = 1;
  s->state = SUMMARY_UNRESOLVED;
}

bool saveSummaries() {
  if (g_loadState != LOAD_OK) {  // the stored record could not be read: never overwrite it
    g_saveDirty = true;
    return false;
  }
  if (Storage::notify().putBytes(kSummaryKey, g_summaries, sizeof(g_summaries)) == sizeof(g_summaries)) {
    g_saveDirty = false;
    return true;
  }
  if (!g_saveDirty) Serial.println("[NOTIFY] unread summary not saved; retrying");
  g_saveDirty = true;
  return false;
}

struct UnreadTally {
  uint16_t n[kTypeSlots];
};

bool tallyUnread(const MessageStore::StoredHeader& header, const PacketCodec::MessageEnvelope& envelope, void* ctx) {
  if (header.flags & MessageStore::FLAG_UNREAD) {
    int slot = slotForType(envelope.message_type);
    if (slot >= 0) static_cast<UnreadTally*>(ctx)->n[slot]++;
  }
  return false;  // counted here; no refs collected
}

// Recounts one conversation's unread messages from the stored records. On a
// reliable scan *s becomes COUNTED with the exact counts; otherwise it is
// left unchanged and false is returned.
bool recountFromStore(Summary* s) {
  UnreadTally t;
  memset(&t, 0, sizeof(t));
  bool reliable = false;
  MessageStore::findMessagesByPredicate(s->group_code, s->contact_key, tallyUnread, &t, nullptr, 0, &reliable);
  if (!reliable) return false;
  for (uint8_t i = 0; i < kTypeSlots; i++) s->unread[i] = t.n[i];
  s->state = SUMMARY_COUNTED;
  return true;
}

// Migrates the earlier "summary" record into dst: each conversation it lists
// with unread messages is recounted per type from the stored messages; one
// whose scan is not reliable becomes UNRESOLVED with the categories its old
// mask showed (both if none). False -- dst untouched -- if the record could
// not be read in full (a read error or an unexpected length).
bool migrateLegacyInto(Summary* dst) {
  LegacySummary* legacy = new (std::nothrow) LegacySummary[kMaxSummaries];
  if (legacy == nullptr) return false;
  bool ok = Storage::notify().getBytes(kLegacyKey, legacy, sizeof(LegacySummary) * kMaxSummaries) ==
            sizeof(LegacySummary) * kMaxSummaries;
  if (ok) {
    uint8_t out = 0;
    for (uint8_t i = 0; i < kMaxSummaries; i++) {
      const LegacySummary& l = legacy[i];
      if (!l.active || l.unread_count == 0) continue;
      Summary& s = dst[out++];
      memset(&s, 0, sizeof(s));
      memcpy(s.group_code, l.group_code, sizeof(s.group_code));
      s.group_code[sizeof(s.group_code) - 1] = '\0';
      memcpy(s.contact_key, l.contact_key, sizeof(s.contact_key));
      s.contact_key[sizeof(s.contact_key) - 1] = '\0';
      if (!recountFromStore(&s)) {
        uint8_t mask = l.badge_mask & (BADGE_TEXT | BADGE_ENIGMA);
        markUnresolved(&s, mask != 0 ? mask : static_cast<uint8_t>(BADGE_TEXT | BADGE_ENIGMA));
      }
    }
  }
  delete[] legacy;
  return ok;
}

// Reads the stored summaries into dst (cleared first). LOAD_OK: dst is
// authoritative -- read from summary2, migrated from the legacy record
// (*migrated, not yet saved), or empty because nothing is stored.
LoadState loadInto(Summary* dst, bool* migrated) {
  memset(dst, 0, sizeof(Summary) * kMaxSummaries);
  *migrated = false;
  Preferences& p = Storage::notify();
  if (p.isKey(kSummaryKey)) {
    if (p.getBytes(kSummaryKey, dst, sizeof(Summary) * kMaxSummaries) == sizeof(Summary) * kMaxSummaries) return LOAD_OK;
    memset(dst, 0, sizeof(Summary) * kMaxSummaries);
    return LOAD_RETRY_CURRENT;
  }
  if (!p.isKey(kLegacyKey)) return LOAD_OK;
  if (!migrateLegacyInto(dst)) {
    memset(dst, 0, sizeof(Summary) * kMaxSummaries);
    return LOAD_RETRY_LEGACY;
  }
  *migrated = true;
  return LOAD_OK;
}

// A stored count can be stale: a read or eviction while the record could not
// be read (or written) changed the messages but not the record, and that
// change may have been lost with RAM at a reboot. So every count read from
// summary2 is checked once: each conversation listed with unread messages
// becomes UNRESOLVED (its badge categories stay lit) and reconcileOne()
// recounts it from the stored messages, one per kRetryMs. Bounded: at most
// kMaxSummaries recounts per load, and none for conversations with nothing
// unread (a missing count can't come from a read or an eviction).
void verifyLoaded(Summary* set) {
  for (uint8_t i = 0; i < kMaxSummaries; i++)
    if (set[i].state == SUMMARY_COUNTED && totalUnread(set[i]) > 0) markUnresolved(&set[i], 0);
}

Summary* findIn(Summary* set, const char* group_code, const char* contact_key) {
  for (uint8_t i = 0; i < kMaxSummaries; i++) {
    Summary& s = set[i];
    if (s.state != SUMMARY_FREE && strcmp(s.group_code, group_code) == 0 && strcmp(s.contact_key, contact_key) == 0) return &s;
  }
  return nullptr;
}

// The entry for (group, contact) in set: the existing one, else a free one or
// one with nothing unread (cleared for it); nullptr if all hold unread ones.
Summary* claimIn(Summary* set, const char* group_code, const char* contact_key) {
  Summary* t = findIn(set, group_code, contact_key);
  for (uint8_t i = 0; i < kMaxSummaries && t == nullptr; i++) {
    if (set[i].state == SUMMARY_FREE || (set[i].state == SUMMARY_COUNTED && totalUnread(set[i]) == 0)) {
      t = &set[i];
      memset(t, 0, sizeof(*t));
      setKeys(t, group_code, contact_key);
    }
  }
  return t;
}

void startSweep() {
  if (g_sweep == nullptr) g_sweep = new (std::nothrow) Sweep;
  if (g_sweep != nullptr) memset(g_sweep, 0, sizeof(*g_sweep));
  else Serial.println("[NOTIFY] no memory for the unread sweep; next boot");
}

void loadSummaries() {
  bool migrated = false;
  g_loadState = loadInto(g_summaries, &migrated);
  if (g_loadState != LOAD_OK) {
    Serial.println("[NOTIFY] unread summary unreadable; retrying");
    return;
  }
  if (migrated) {
    saveSummaries();  // migration recounted every conversation already
  } else {
    verifyLoaded(g_summaries);
  }
  startSweep();
}

// The stored record became readable: every conversation touched in RAM
// meanwhile is merged in as UNRESOLVED (its RAM counts started from nothing,
// so neither copy is complete) and is recounted by the reconcile step.
void retryLoad() {
  Summary* loaded = new (std::nothrow) Summary[kMaxSummaries];
  if (loaded == nullptr) return;
  bool migrated = false;
  LoadState st = loadInto(loaded, &migrated);
  if (st == LOAD_OK) {
    if (!migrated) verifyLoaded(loaded);
    for (auto& ram : g_summaries) {
      if (ram.state == SUMMARY_FREE) continue;
      uint8_t mask = static_cast<uint8_t>(((ram.unread[SLOT_TEXT] || ram.unread[SLOT_GAME]) ? BADGE_TEXT : 0) |
                                          ((ram.unread[SLOT_ENIGMA] || ram.unread[SLOT_GAME]) ? BADGE_ENIGMA : 0));
      Summary* t = claimIn(loaded, ram.group_code, ram.contact_key);
      if (t != nullptr) markUnresolved(t, mask);  // (all 24 holding unread conversations: the stored ones are kept)
    }
    memcpy(g_summaries, loaded, sizeof(g_summaries));
    g_loadState = LOAD_OK;
    startSweep();
    saveSummaries();
  } else {
    g_loadState = st;
  }
  delete[] loaded;
}

// One UNRESOLVED conversation per call is recounted (bounded storage work).
// False if there was none.
bool reconcileOne() {
  for (uint8_t k = 0; k < kMaxSummaries; k++) {
    Summary& s = g_summaries[(g_reconcileCursor + k) % kMaxSummaries];
    if (s.state != SUMMARY_UNRESOLVED) continue;
    g_reconcileCursor = static_cast<uint8_t>((g_reconcileCursor + k + 1) % kMaxSummaries);
    if (recountFromStore(&s)) saveSummaries();
    return true;
  }
  return false;
}

Summary* findSummary(const char* group_code, const char* contact_key);
Summary* findOrCreateSummary(const char* group_code, const char* contact_key);

int keyCompare(const char* g1, const char* c1, const char* g2, const char* c2) {
  int r = strcmp(g1, g2);
  return r != 0 ? r : strcmp(c1, c2);
}

struct SweepPick {
  const Sweep* after;
  bool found;
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
};

// listConversations callback: the first conversation after the sweep's position.
void pickNextConversation(const char* group_code, const char* contact_key, void* ctx) {
  SweepPick* p = static_cast<SweepPick*>(ctx);
  if (p->after->started && keyCompare(group_code, contact_key, p->after->group_code, p->after->contact_key) <= 0) return;
  if (p->found && keyCompare(group_code, contact_key, p->group_code, p->contact_key) >= 0) return;
  strncpy(p->group_code, group_code, sizeof(p->group_code) - 1);
  strncpy(p->contact_key, contact_key, sizeof(p->contact_key) - 1);
  p->found = true;
}

// One stored conversation per call is recounted -- listed or not, and also
// when listed with nothing unread (an UNRESOLVED one is left to
// reconcileOne). It is added if it has unread messages and isn't listed. An
// unreliable listing or recount is retried at the next call.
void sweepOne() {
  SweepPick pick;
  memset(&pick, 0, sizeof(pick));
  pick.after = g_sweep;
  if (!MessageStore::listConversations(pickNextConversation, &pick)) return;
  if (!pick.found) {
    delete g_sweep;
    g_sweep = nullptr;
    return;
  }
  Summary* listed = findSummary(pick.group_code, pick.contact_key);
  if (listed == nullptr || listed->state == SUMMARY_COUNTED) {
    Summary t;
    memset(&t, 0, sizeof(t));
    setKeys(&t, pick.group_code, pick.contact_key);
    if (!recountFromStore(&t)) return;
    Summary* s = listed;
    if (s == nullptr && totalUnread(t) > 0) s = findOrCreateSummary(pick.group_code, pick.contact_key);
    if (s != nullptr && memcmp(s->unread, t.unread, sizeof(s->unread)) != 0) {
      memcpy(s->unread, t.unread, sizeof(s->unread));
      s->state = SUMMARY_COUNTED;
      saveSummaries();
    }
  }
  g_sweep->started = true;
  memcpy(g_sweep->group_code, pick.group_code, sizeof(g_sweep->group_code));
  memcpy(g_sweep->contact_key, pick.contact_key, sizeof(g_sweep->contact_key));
}

// Every kRetryMs: retry an unreadable stored record; otherwise retry a failed
// save (if any) and then reconcile one UNRESOLVED conversation, else take one
// step of a sweep -- a save that keeps failing doesn't stop the recount, so
// the badges in RAM stay right meanwhile.
void serviceTick() {
  uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextRetryMs) < 0) return;
  g_nextRetryMs = now + kRetryMs;
  if (g_loadState != LOAD_OK) {
    retryLoad();
  } else {
    if (g_saveDirty) saveSummaries();
    if (!reconcileOne() && g_sweep != nullptr) sweepOne();
  }
}

Summary* findSummary(const char* group_code, const char* contact_key) {
  return findIn(g_summaries, group_code, contact_key);
}

Summary* findOrCreateSummary(const char* group_code, const char* contact_key) {
  Summary* s = findSummary(group_code, contact_key);
  if (s != nullptr) return s;
  // A free entry, else (24 conversations listed: very unlikely) one with
  // nothing unread; an UNRESOLVED entry is never reused.
  for (int pass = 0; pass < 2; pass++) {
    for (auto& c : g_summaries) {
      bool usable = pass == 0 ? c.state == SUMMARY_FREE : (c.state == SUMMARY_COUNTED && totalUnread(c) == 0);
      if (!usable) continue;
      memset(&c, 0, sizeof(c));
      c.state = SUMMARY_COUNTED;
      setKeys(&c, group_code, contact_key);
      return &c;
    }
  }
  return nullptr;
}

bool hasBadge(uint8_t mask) {
  for (auto& s : g_summaries) {
    if (s.state == SUMMARY_FREE) continue;  // COUNTED: counts; UNRESOLVED: category flags -- same test
    if ((mask & BADGE_TEXT) && (s.unread[SLOT_TEXT] > 0 || s.unread[SLOT_GAME] > 0)) return true;
    if ((mask & BADGE_ENIGMA) && (s.unread[SLOT_ENIGMA] > 0 || s.unread[SLOT_GAME] > 0)) return true;
  }
  return false;  // no stored message type maps to BADGE_TRAINING_GAME (Race ORs in its own flag)
}

// MessageStore::setOnEvictedCallback's own doc comment says this exists "so
// Notifications can decrement its per-conversation summary" -- without
// wiring it up here, an UNREAD record evicted by the 300-message FIFO cap or
// global low-space cleanup left its summary's unread_count permanently
// stuck (the evicted message can never again be reached by clearUnread(),
// since it no longer exists), which could pin the Main Menu badge on
// indefinitely. `flags` is the evicted record's own header flags, captured
// before removal, so no re-read of the now-deleted file is needed.
// The evicted record's type is no longer readable, so its conversation is
// recounted from what remains; if that scan is not reliable the conversation
// becomes UNRESOLVED (its badges stay lit until a later recount).
void onMessageEvicted(const MessageRef& ref, uint16_t flags) {
  if (!(flags & MessageStore::FLAG_UNREAD)) return;
  Summary* s = findSummary(ref.group_code, ref.contact_key);
  if (s == nullptr) return;  // not listed: nothing stored or in RAM to correct (a loaded record is verified)
  if (!recountFromStore(s)) markUnresolved(s, 0);
  saveSummaries();
}

bool g_raceInvitePending = false;

void onSettingsChanged(const SettingsChangeInfo& info) {
  if (info.event != SET_GROUP_DELETED) return;
  bool changed = false;
  for (auto& s : g_summaries) {
    if (s.state != SUMMARY_FREE && strcmp(s.group_code, info.group_code) == 0) {
      s.state = SUMMARY_FREE;
      changed = true;
    }
  }
  if (changed) saveSummaries();
}

void serviceInit() {
  loadSummaries();
  registerSettingsChangeHook(onSettingsChanged);
  MessageStore::setOnEvictedCallback(onMessageEvicted);
  Menu::registerMainMenuBadge(MainMenuIndex::TEXT, hasAnyUnreadText);
  Menu::registerMainMenuBadge(MainMenuIndex::ENIGMA, hasAnyUnreadEnigma);
  Menu::registerMainMenuBadge(MainMenuIndex::TRAINING_GAME, hasAnyUnreadTrainingGame);
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = serviceTick;
    registerAppService(svc);
  }
};
Registrar g_registrar;

}  // namespace

void onMessageArrived(const char* group_code, const char* contact_key, const MessageRef& ref,
                      uint8_t badgeMask, bool conversationCurrentlyOpen) {
  if (conversationCurrentlyOpen) {
    MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD);
    return;
  }

  MessageStore::updateLocalFlags(ref, MessageStore::FLAG_UNREAD, 0);
  StoredMessageView view;
  bool loaded = MessageStore::loadMessage(ref, &view);
  Summary* s = findOrCreateSummary(group_code, contact_key);
  if (s != nullptr) {
    int slot = loaded ? slotForType(view.envelope.message_type) : -1;
    if (!loaded) {
      markUnresolved(s, badgeMask);  // type unknown: the announced categories stay lit until a recount
    } else if (slot >= 0 && s->state == SUMMARY_COUNTED) {
      s->unread[slot]++;
    } else if (slot >= 0) {
      s->unread[slot] = 1;  // UNRESOLVED: flag the category; the recount will count it
    }
    saveSummaries();
  }

  playTone(1000, 150, SOUND_NOTIFICATION);
  Sleep::notifyActivity();
}

void clearUnread(const char* group_code, const char* contact_key, const MessageRef& ref) {
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  if (!(view.header.flags & MessageStore::FLAG_UNREAD)) return;

  MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD);
  Summary* s = findSummary(group_code, contact_key);
  if (s == nullptr) return;  // not listed: nothing stored or in RAM to correct (a loaded record is verified)
  if (s->state == SUMMARY_UNRESOLVED) {
    if (recountFromStore(s)) saveSummaries();  // else it stays lit until a reliable recount
    return;
  }
  int slot = slotForType(view.envelope.message_type);
  if (slot >= 0 && s->unread[slot] > 0) {
    s->unread[slot]--;
    saveSummaries();
  }
}

uint16_t getUnreadCount(const char* group_code, const char* contact_key) {
  Summary* s = findSummary(group_code, contact_key);
  return s != nullptr ? totalUnread(*s) : 0;
}

bool hasAnyUnreadText() { return hasBadge(BADGE_TEXT); }
bool hasAnyUnreadEnigma() { return hasBadge(BADGE_ENIGMA); }
bool hasAnyUnreadTrainingGame() { return hasBadge(BADGE_TRAINING_GAME) || g_raceInvitePending; }

void setRaceInvitePending(bool pending) { g_raceInvitePending = pending; }

}  // namespace Notifications
