#include "core/notifications.h"

#include <Arduino.h>
#include <string.h>

#include <new>

#include "core/hooks.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/settings.h"
#include "core/sleep.h"
#include "core/sound_facade.h"
#include "core/storage_init.h"
#include "core/text_message.h"

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
// The earlier firmware's "read up to a selection" record (511d13e and
// before). Never read or applied by this firmware: erased once at boot
// (discardOldReadUpRecord()).
constexpr const char* kOldReadUpKey = "readup";

// ---- per-message read (Text and Enigma chats) --------------------------
//
// Moving a chat's cursor onto a received message reads exactly that message
// (readMovedOnto()): nothing older, nothing else on screen, no other type,
// conversation or category. Entering a chat, drawing it, its initial or a
// restored selection, a load completing and an arrival read nothing.
//
// An accepted request is kept in RAM only, by exact identity (group,
// contact, sequence and message id), and applied by the background work:
// one record rewrite per step (serviceTick()), before any other job. Nothing
// is written or listed on the cursor-movement path. The count is decremented
// only once the rewrite clearing FLAG_UNREAD succeeded, so counts and badges
// describe what is on flash, except after a rewrite whose outcome is unknown
// (see applyRead(): the conversation is then recounted). A rewrite (or
// read) that fails is retried (kBackoffMinMs doubling, at most
// kBackoffMaxMs) and moved behind the others.
//
// Trustworthy completion only. A request ends when its rewrite succeeded;
// when its record, loaded, is already read or holds another message (its
// sequence reused); when the store reports that record's removal as done
// (eviction, trimming: forgetPendingReads()); when the same message was
// read by its explicit DOT action; or when its group is no longer
// configured (deleted). A record that cannot be loaded is never taken as
// gone: through the Arduino API a failed open, or a directory listing cut
// short by a read error, looks exactly like absence. Such a request keeps
// its slot and is retried with backoff -- until the record loads again, or
// until reboot if it really disappeared without a removal the store could
// report (a rewrite whose rename failed, a file removed outside the store).
// One retry is one failed open, about once a minute at most; it never
// blocks the other requests or the other background work.
//
// Finite capacity: at most kMaxPendingReads accepted requests. A request
// that finds the list full (a burst of moves, or earlier requests waiting
// for storage to recover), or no memory for the list, is refused:
// readMovedOnto() returns false, nothing is recorded, the message stays
// unread (its marker stays) and the chat says so ("Read not saved (busy)").
// Moving onto it again later, once there is room, reads it.
//
// Nothing is persisted: a power loss between the move and the rewrite's
// rename loses the read -- the message stays unread on flash (marker shown,
// counted after the reboot); never wrongly read.
constexpr uint8_t kMaxPendingReads = 32;
struct PendingRead {
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  char messageId[PacketCodec::kMessageIdLen];
  uint32_t sequence;
  uint32_t retryAtMs;  // 0: due now
  uint8_t failures;    // rewrites (or reads) that failed so far
};
struct PendingReads {
  uint8_t count;
  PendingRead items[kMaxPendingReads];  // oldest first
};

// ---- background work: bounded per main-loop pass -------------------------
//
// Recounts (UNRESOLVED conversations, the sweep below) and trims are jobs
// over one conversation, run one step at a time from serviceTick(), at most
// one step per kStepMs. A step is ONE of: one conversation directory listing
// (names only, at most 300 entries), up to kLoadsPerStep record reads, one
// record deletion, one per-message read (a record rewrite), one NVS write,
// or one retry of an unreadable summary record. Nothing on the arrival, read
// or eviction paths scans a whole conversation.
//
// After every successful load (at boot, or when an unreadable record becomes
// readable) every stored conversation is visited once (the sweep): first
// trimmed to the retained history, then recounted. The recount is needed
// because the stored summary can miss an unread message that arrived while
// it could not be read or written, or whose arrival was cut off by a power
// loss after the message was stored (unread) but before it was counted. The
// stored messages are the only record that always survives. The sweep walks
// the directories incrementally (MessageStore::stepConversationWalk(), at
// most kWalkEntriesPerStep entries per step). A pass that leaves anything
// undone (a failed deletion or listing, a conversation open on screen, a
// walk that failed) is followed by another pass after a growing pause.
//
// Trimming (approved reduction from 300 to MessageStore::
// kRetainedMessagesPerThread): the oldest records by sequence that are not
// pending in the outbox are deleted, one per step, until the conversation
// holds kRetainedMessagesPerThread -- the same order appendStoredMessage()
// evicts in. A deleted unread record is uncounted only once its deletion
// succeeded. A record that cannot be read is never skipped: the trim stops
// and is retried in a later pass. The conversation open on screen is not
// trimmed until it is left.
constexpr uint32_t kStepMs = 25;
constexpr uint8_t kLoadsPerStep = 4;
// A step also ends once this much time went into it (device A on 2ed04b6:
// about 63 ms per record open, so 4 reads were 250-480 ms passes).
constexpr uint32_t kStepBudgetMs = 20;
constexpr uint32_t kSaveDelayMs = 2000;  // summary writes are coalesced
constexpr uint8_t kWalkEntriesPerStep = 16;
constexpr uint8_t kJobAttempts = 5;      // unreliable listings or passes before a job gives way (and backs off)
constexpr uint32_t kBackoffMinMs = 2000;
constexpr uint32_t kBackoffMaxMs = 60000;

// Each pass first visits every configured group's broadcast conversation
// (Everyone: the chat most likely opened first after boot), whose recount
// then feeds that chat's index (MessageStore::beginIndexFeed()); the walk
// skips those afterwards.
struct Sweep {
  uint8_t everyoneNext;                  // the next configured group whose Everyone is visited
  bool everyoneDone;                     // every group's Everyone visited this pass
  MessageStore::ConversationWalk* walk;  // nullptr between passes
  bool incomplete;                       // this pass left something for the next one
  uint32_t nextPassMs;                   // a pass after an incomplete one starts no earlier
  uint32_t backoffMs;
};

enum JobKind : uint8_t { JOB_RECOUNT = 1, JOB_TRIM = 3 };
struct Job {
  JobKind kind;
  bool fromSweep;
  bool listed;      // seqs[] holds the conversation's records
  bool restart;     // the conversation changed under a recount: list and count again
  bool unreliable;  // a record could not be read in this pass
  uint8_t attempts; // unreliable listings or passes so far
  uint16_t excess;  // trim: records still to delete
  uint16_t n, pos;
  uint16_t tally[kTypeSlots];
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  uint32_t seqs[MessageStore::kMaxMessagesPerThread];
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
bool g_saveDirty = false;  // RAM is ahead of the stored record (save pending, deferred or failed)
uint32_t g_saveDueMs = 0;
uint32_t g_nextRetryMs = 0;
uint32_t g_nextStepMs = 0;
uint8_t g_reconcileCursor = 0;
Sweep* g_sweep = nullptr;          // non-null while a sweep runs (or waits for its next pass)
uint32_t g_recountRetryMs = 0;     // a recount that gave way: UNRESOLVED ones wait until then
Job* g_job = nullptr;              // non-null while a job runs
PendingReads* g_reads = nullptr;    // non-null while a per-message read is pending

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

bool sameConversation(const char* g1, const char* c1, const char* g2, const char* c2) {
  return strcmp(g1, g2) == 0 && strcmp(c1, c2) == 0;
}

// Marks *s UNRESOLVED, keeping a flag for every category it may have unread
// (its own non-zero slots, plus the BADGE_* categories in `badgeMask`).
void markUnresolved(Summary* s, uint8_t badgeMask) {
  for (uint8_t i = 0; i < kTypeSlots; i++) s->unread[i] = s->unread[i] != 0 ? 1 : 0;
  if (badgeMask & BADGE_TEXT) s->unread[SLOT_TEXT] = 1;
  if (badgeMask & BADGE_ENIGMA) s->unread[SLOT_ENIGMA] = 1;
  s->state = SUMMARY_UNRESOLVED;
}

// The summary changed: written by serviceTick() kSaveDelayMs after the first
// unsaved change (coalesced). Losing an unsaved change to a power loss is
// safe: the stored messages' flags are the truth and the sweep after the next
// load recounts every conversation from them.
void saveSummaries() {
  if (!g_saveDirty) g_saveDueMs = millis() + kSaveDelayMs;
  g_saveDirty = true;
}

// Called only when the stored record has loaded (serviceTick()).
void writeSummaries() {
  if (Storage::notify().putBytes(kSummaryKey, g_summaries, sizeof(g_summaries)) == sizeof(g_summaries)) {
    g_saveDirty = false;
    return;
  }
  Serial.println("[NOTIFY] unread summary not saved; retrying");
  g_saveDueMs = millis() + kRetryMs;
}

uint32_t backoffMs(uint8_t failures) {
  uint32_t ms = kBackoffMinMs;
  for (uint8_t i = 1; i < failures && ms < kBackoffMaxMs; i++) ms *= 2;
  return ms < kBackoffMaxMs ? ms : kBackoffMaxMs;
}

// ---- loading ----------------------------------------------------------------

// Migrates the earlier "summary" record into dst: each conversation it lists
// with unread messages becomes UNRESOLVED with the categories its old mask
// showed (both if none), and is recounted by the background work. False --
// dst untouched -- if the record could not be read in full (a read error or
// an unexpected length).
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
      uint8_t mask = l.badge_mask & (BADGE_TEXT | BADGE_ENIGMA);
      markUnresolved(&s, mask != 0 ? mask : static_cast<uint8_t>(BADGE_TEXT | BADGE_ENIGMA));
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
// becomes UNRESOLVED (its badge categories stay lit) and is recounted in the
// background. None for conversations with nothing unread (the sweep covers
// those).
void verifyLoaded(Summary* set) {
  for (uint8_t i = 0; i < kMaxSummaries; i++)
    if (set[i].state == SUMMARY_COUNTED && totalUnread(set[i]) > 0) markUnresolved(&set[i], 0);
}

Summary* findIn(Summary* set, const char* group_code, const char* contact_key) {
  for (uint8_t i = 0; i < kMaxSummaries; i++) {
    Summary& s = set[i];
    if (s.state != SUMMARY_FREE && sameConversation(s.group_code, s.contact_key, group_code, contact_key)) return &s;
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

// A sweep pass starts now (replacing a pass under way or waiting).
void startSweep() {
  if (g_sweep == nullptr) {
    g_sweep = new (std::nothrow) Sweep;
    if (g_sweep == nullptr) {
      Serial.println("[NOTIFY] no memory for the sweep; next boot");
      return;
    }
    memset(g_sweep, 0, sizeof(*g_sweep));
  } else {
    MessageStore::endConversationWalk(g_sweep->walk);
    g_sweep->walk = nullptr;
    g_sweep->incomplete = false;
  }
  g_sweep->everyoneNext = 0;
  g_sweep->everyoneDone = false;
  g_sweep->nextPassMs = millis();
  g_sweep->backoffMs = kBackoffMinMs;
}

void loadSummaries() {
  bool migrated = false;
  g_loadState = loadInto(g_summaries, &migrated);
  if (g_loadState != LOAD_OK) {
    Serial.println("[NOTIFY] unread summary unreadable; retrying");
    return;
  }
  if (migrated) {
    saveSummaries();
  } else {
    verifyLoaded(g_summaries);
  }
  startSweep();
}

// The stored record became readable: every conversation touched in RAM
// meanwhile is merged in as UNRESOLVED (its RAM counts started from nothing,
// so neither copy is complete) and is recounted in the background.
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

// ---- jobs -----------------------------------------------------------------

// Something changed in this conversation's records: a recount or trim of it
// that is under way lists it again (its listing and tally may no longer
// match).
void conversationChanged(const char* group_code, const char* contact_key) {
  if (g_job != nullptr && (g_job->kind == JOB_RECOUNT || g_job->kind == JOB_TRIM) &&
      sameConversation(g_job->group_code, g_job->contact_key, group_code, contact_key))
    g_job->restart = true;
}

Job* newJob(JobKind kind, const char* group_code, const char* contact_key) {
  Job* j = new (std::nothrow) Job;
  if (j == nullptr) return nullptr;  // retried at a later step
  memset(j, 0, sizeof(*j));
  j->kind = kind;
  strncpy(j->group_code, group_code, sizeof(j->group_code) - 1);
  strncpy(j->contact_key, contact_key, sizeof(j->contact_key) - 1);
  return j;
}

void endJob() {
  delete g_job;
  g_job = nullptr;
}

uint8_t g_recountFailures = 0;  // recounts of UNRESOLVED conversations that gave way in a row

// The job could not complete now (records or listings unreliable, a deletion
// failed, or its conversation is open on screen): it gives way to the other
// work and is tried again later -- by the sweep's next pass, or (an
// UNRESOLVED recount) after a growing pause. Nothing is skipped silently.
void giveWay(Job& j) {
  if (j.fromSweep && g_sweep != nullptr) g_sweep->incomplete = true;
  if (j.kind == JOB_RECOUNT && !j.fromSweep) {
    if (g_recountFailures < 255) g_recountFailures++;
    g_recountRetryMs = millis() + backoffMs(g_recountFailures);
  }
}

// From trimming to recounting the same conversation (the sweep's order).
void continueWithRecount(Job& j) {
  j.kind = JOB_RECOUNT;
  j.listed = false;
  j.restart = false;
  j.unreliable = false;
  j.attempts = 0;
  j.n = 0;
  j.pos = 0;
  memset(j.tally, 0, sizeof(j.tally));
}

// The sweep's next step: open the walk (once per pass), or advance it by at
// most kWalkEntriesPerStep directory entries; a conversation found starts a
// trim (then a recount) of it. A pass that left something undone is
// followed by another after a growing pause.
bool isConfiguredGroup(const char* group_code) {
  for (uint8_t i = 0; i < Settings::getGroupCount(); i++) {
    Settings::FamilyGroup g = Settings::getGroup(i);
    if (g.configured && strcmp(g.code, group_code) == 0) return true;
  }
  return false;
}

void sweepStep() {
  uint32_t now = millis();
  if (g_sweep->walk == nullptr) {
    if (static_cast<int32_t>(now - g_sweep->nextPassMs) < 0) return;
    if (!g_sweep->everyoneDone) {
      // One configured group's Everyone per step (one existence check).
      while (g_sweep->everyoneNext < Settings::getGroupCount()) {
        Settings::FamilyGroup g = Settings::getGroup(g_sweep->everyoneNext++);
        if (!g.configured) continue;
        if (!MessageStore::conversationExists(g.code, MessageStore::kEveryone)) return;
        g_job = newJob(JOB_TRIM, g.code, MessageStore::kEveryone);
        if (g_job != nullptr) g_job->fromSweep = true;
        else g_sweep->incomplete = true;
        return;
      }
      g_sweep->everyoneDone = true;
    }
    g_sweep->walk = MessageStore::beginConversationWalk();  // nullptr (no memory): tried again at the next step
    return;
  }
  char group[33], contact[MessageStore::kContactKeyLen];
  MessageStore::WalkResult r =
      MessageStore::stepConversationWalk(g_sweep->walk, kWalkEntriesPerStep, group, sizeof(group), contact, sizeof(contact));
  if (r == MessageStore::WalkResult::MORE) return;
  if (r == MessageStore::WalkResult::FOUND) {
    if (strcmp(contact, MessageStore::kEveryone) == 0 && isConfiguredGroup(group)) return;  // visited first
    g_job = newJob(JOB_TRIM, group, contact);
    if (g_job != nullptr) g_job->fromSweep = true;
    else g_sweep->incomplete = true;
    return;
  }
  MessageStore::endConversationWalk(g_sweep->walk);
  g_sweep->walk = nullptr;
  if (r == MessageStore::WalkResult::FAILED) g_sweep->incomplete = true;
  if (!g_sweep->incomplete) {
    delete g_sweep;
    g_sweep = nullptr;
    return;
  }
  Serial.println("[NOTIFY] sweep pass incomplete; another follows");
  g_sweep->incomplete = false;
  g_sweep->everyoneNext = 0;
  g_sweep->everyoneDone = false;
  g_sweep->nextPassMs = now + g_sweep->backoffMs;
  g_sweep->backoffMs = g_sweep->backoffMs * 2 < kBackoffMaxMs ? g_sweep->backoffMs * 2 : kBackoffMaxMs;
}

// Starts the next job, or advances the sweep (one step's work at most).
// Order: an UNRESOLVED conversation, then the sweep. (Per-message reads go
// before any job: serviceTick().)
void startNextJob() {
  uint32_t now = millis();
  if (g_loadState != LOAD_OK) return;  // recounts need the stored summary
  if (g_recountFailures == 0 || static_cast<int32_t>(now - g_recountRetryMs) >= 0) {
    for (uint8_t k = 0; k < kMaxSummaries; k++) {
      Summary& s = g_summaries[(g_reconcileCursor + k) % kMaxSummaries];
      if (s.state != SUMMARY_UNRESOLVED) continue;
      g_reconcileCursor = static_cast<uint8_t>((g_reconcileCursor + k + 1) % kMaxSummaries);
      g_job = newJob(JOB_RECOUNT, s.group_code, s.contact_key);
      return;
    }
  }
  if (g_sweep != nullptr) sweepStep();
}

// A recount is complete: the counts become exact -- unless something could
// not be read (it is listed and counted again, and after kJobAttempts gives
// way).
void finishRecount() {
  Job& j = *g_job;
  if (j.unreliable) {
    if (++j.attempts >= kJobAttempts) {
      giveWay(j);
      endJob();
      return;
    }
    j.listed = false;  // listed and counted again
    return;
  }
  Summary* s = findSummary(j.group_code, j.contact_key);
  uint16_t total = static_cast<uint16_t>(j.tally[SLOT_TEXT] + j.tally[SLOT_ENIGMA] + j.tally[SLOT_GAME]);
  if (s == nullptr && total > 0) s = findOrCreateSummary(j.group_code, j.contact_key);
  if (s != nullptr && (s->state != SUMMARY_COUNTED || memcmp(s->unread, j.tally, sizeof(s->unread)) != 0)) {
    memcpy(s->unread, j.tally, sizeof(s->unread));
    s->state = SUMMARY_COUNTED;
    saveSummaries();
  }
  if (!j.fromSweep) g_recountFailures = 0;
  endJob();
}

// ---- per-message read (see PendingRead above) -----------------------------

void forgetPendingReads(const char* group_code, const char* contact_key, const uint32_t* sequence,
                        const char* messageId = nullptr);

// The conversation's count may no longer describe flash (a rewrite whose
// outcome is unknown): recounted from the flags.
void invalidateCount(const char* group_code, const char* contact_key) {
  Summary* s = findSummary(group_code, contact_key);
  if (s == nullptr || s->state != SUMMARY_COUNTED) return;
  markUnresolved(s, 0);
  saveSummaries();
}

enum class ReadOutcome : uint8_t { DONE, RETRY };

// After a failed attempt, the record may have changed without its count
// following. Recount rather than guess.
void recountAfterFailedAttempt(const PendingRead& p) {
  if (p.failures == 0) return;
  invalidateCount(p.group_code, p.contact_key);
}

// Clears this one message's FLAG_UNREAD on flash and, only once that
// succeeded, its count. DONE also when there is nothing (left) to do.
ReadOutcome applyRead(PendingRead& p) {
  MessageRef ref;
  memset(&ref, 0, sizeof(ref));
  strncpy(ref.group_code, p.group_code, sizeof(ref.group_code) - 1);
  strncpy(ref.contact_key, p.contact_key, sizeof(ref.contact_key) - 1);
  ref.sequence = p.sequence;
  if (!isConfiguredGroup(p.group_code)) return ReadOutcome::DONE;  // the group was deleted: nothing of it is shown
  StoredMessageView view;
  // Not loadable: unreadable for now, or gone -- which cannot be told apart
  // (see above). Kept, retried.
  if (!MessageStore::loadMessage(ref, &view)) return ReadOutcome::RETRY;
  if (strncmp(view.envelope.message_id, p.messageId, PacketCodec::kMessageIdLen) != 0) {
    recountAfterFailedAttempt(p);  // another message holds the sequence now: it is not read
    return ReadOutcome::DONE;
  }
  if (view.header.direction != MessageStore::Direction::RECEIVED || !(view.header.flags & MessageStore::FLAG_UNREAD)) {
    recountAfterFailedAttempt(p);  // already read
    return ReadOutcome::DONE;
  }
  Summary* s = findSummary(p.group_code, p.contact_key);
  int slot = slotForType(view.envelope.message_type);
  if (!MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD)) {
    // Retried. The record, read back: still there (a plain failure; should
    // the flag be cleared after all, the next attempt finds it read and
    // recounts), or not loadable -- the rewrite's outcome unknown (a failed
    // rename may have lost it, see atomicRewrite()): its count is recounted.
    StoredMessageView after;
    bool same = MessageStore::loadMessage(ref, &after) &&
                strncmp(after.envelope.message_id, p.messageId, PacketCodec::kMessageIdLen) == 0;
    if (!same) invalidateCount(p.group_code, p.contact_key);
    return ReadOutcome::RETRY;
  }
  conversationChanged(p.group_code, p.contact_key);  // a recount under way may have counted it unread
  if (s != nullptr && s->state == SUMMARY_COUNTED && slot >= 0 && s->unread[slot] > 0) {
    s->unread[slot]--;
    saveSummaries();
  }
  return ReadOutcome::DONE;  // (an UNRESOLVED summary is recounted from the flags)
}

void removePendingRead(uint8_t i) {
  for (uint8_t k = i; k + 1 < g_reads->count; k++) g_reads->items[k] = g_reads->items[k + 1];
  g_reads->count--;
  if (g_reads->count == 0) {
    delete g_reads;
    g_reads = nullptr;
  }
}

// Applies the oldest due per-message read (one record rewrite). False if
// none is due.
bool stepPendingRead() {
  if (g_reads == nullptr) return false;
  uint32_t now = millis();
  for (uint8_t i = 0; i < g_reads->count; i++) {
    PendingRead& p = g_reads->items[i];
    if (p.retryAtMs != 0 && static_cast<int32_t>(now - p.retryAtMs) < 0) continue;
    if (applyRead(p) == ReadOutcome::DONE) {
      removePendingRead(i);
      return true;
    }
    // Retried later, behind the others (it never blocks them).
    PendingRead again = p;
    if (again.failures < 255) again.failures++;
    again.retryAtMs = now + backoffMs(again.failures);
    if (again.retryAtMs == 0) again.retryAtMs = 1;
    Serial.printf("[NOTIFY] read of %s/%s #%lu not written; retried in %lu ms\n", again.group_code, again.contact_key,
                  static_cast<unsigned long>(again.sequence), static_cast<unsigned long>(again.retryAtMs - now));
    for (uint8_t k = i; k + 1 < g_reads->count; k++) g_reads->items[k] = g_reads->items[k + 1];
    g_reads->items[g_reads->count - 1] = again;
    return true;
  }
  return false;
}

// Ends the pending reads of one record whose removal the store reported
// done (group, contact and sequence), or -- message id given too -- of that
// exact message once read another way. Nothing of any other record,
// conversation or group.
void forgetPendingReads(const char* group_code, const char* contact_key, const uint32_t* sequence, const char* messageId) {
  if (g_reads == nullptr || contact_key == nullptr || sequence == nullptr) return;
  for (uint8_t i = g_reads->count; i-- > 0;) {
    const PendingRead& p = g_reads->items[i];
    if (strcmp(p.group_code, group_code) != 0 || strcmp(p.contact_key, contact_key) != 0 || p.sequence != *sequence) continue;
    if (messageId != nullptr && strncmp(p.messageId, messageId, PacketCodec::kMessageIdLen) != 0) continue;
    removePendingRead(i);
    if (g_reads == nullptr) return;
  }
}

// The earlier firmware kept a "read up to this selection" request in NVS
// until it was applied. Its ranges are never applied or turned into
// per-message reads: the record is erased (once; if erasing fails it is
// ignored again at the next boot). Messages it had already cleared on flash
// stay read; those it had not reached keep their (truthful) unread flag, and
// the boot sweep recounts every conversation from the flags.
void discardOldReadUpRecord() {
  Preferences& p = Storage::notify();
  if (!p.isKey(kOldReadUpKey)) return;
  if (p.remove(kOldReadUpKey)) Serial.println("[NOTIFY] earlier read-up record discarded, not applied");
  else Serial.println("[NOTIFY] earlier read-up record not erased; ignored (erased at a later boot)");
}

// One trim step: at most kLoadsPerStep record reads (pending-outbox records
// are kept) and at most one deletion. The records are in ascending sequence.
void stepTrim(Job& j) {
  if (j.excess == 0 || j.pos >= j.n) {
    continueWithRecount(j);
    return;
  }
  if (TextMessage::isConversationOpen(j.group_code, j.contact_key)) {
    giveWay(j);  // not trimmed while on screen: a later pass does it
    continueWithRecount(j);
    return;
  }
  MessageRef ref;
  strncpy(ref.group_code, j.group_code, sizeof(ref.group_code) - 1);
  ref.group_code[sizeof(ref.group_code) - 1] = '\0';
  strncpy(ref.contact_key, j.contact_key, sizeof(ref.contact_key) - 1);
  ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
  uint32_t stepStart = millis();
  for (uint8_t loads = 0; loads < kLoadsPerStep && j.pos < j.n; loads++) {
    if (loads > 0 && millis() - stepStart >= kStepBudgetMs) return;
    ref.sequence = j.seqs[j.pos];
    StoredMessageView view;
    if (!MessageStore::loadMessage(ref, &view)) {
      // Not readable (or gone since the listing): never skipped -- listed
      // again, and after kJobAttempts left to a later pass.
      if (++j.attempts >= kJobAttempts) {
        giveWay(j);
        continueWithRecount(j);
      } else {
        j.restart = true;
      }
      return;
    }
    if (view.header.flags & MessageStore::FLAG_PENDING_OUTBOX) {
      j.pos++;  // kept: never evicted
      continue;
    }
    int slot = slotForType(view.envelope.message_type);
    bool counted = view.header.direction == MessageStore::Direction::RECEIVED &&
                   (view.header.flags & MessageStore::FLAG_UNREAD) && slot >= 0;
    if (!MessageStore::deleteMessage(ref)) {
      Serial.println("[NOTIFY] trim: a record could not be deleted; retried later");
      giveWay(j);
      continueWithRecount(j);
      return;
    }
    j.pos++;
    j.excess--;
    forgetPendingReads(ref.group_code, ref.contact_key, &ref.sequence);
    if (counted) {  // uncounted only now that it is gone
      Summary* s = findSummary(j.group_code, j.contact_key);
      if (s != nullptr && s->state == SUMMARY_COUNTED && s->unread[slot] > 0) {
        s->unread[slot]--;
        saveSummaries();
      }
    }
    return;  // at most one deletion per step
  }
}

void sortAscending(uint32_t* a, uint16_t n) {
  for (uint16_t i = 1; i < n; i++) {
    uint32_t key = a[i];
    int32_t k = static_cast<int32_t>(i) - 1;
    while (k >= 0 && a[k] > key) {
      a[k + 1] = a[k];
      k--;
    }
    a[k + 1] = key;
  }
}

// One step of the current job.
void stepJob() {
  Job& j = *g_job;
  if (!j.listed || j.restart) {
    bool reliable = false, truncated = false;
    j.n = MessageStore::listMessageSequences(j.group_code, j.contact_key, j.seqs, MessageStore::kMaxMessagesPerThread,
                                             &reliable, &truncated);
    j.pos = 0;
    j.restart = false;
    j.unreliable = false;
    memset(j.tally, 0, sizeof(j.tally));
    // A trim can start from a truncated listing (it holds the oldest
    // records; the rest are trimmed by the next pass). Counting cannot.
    if (j.kind == JOB_TRIM && truncated) reliable = true;
    j.listed = reliable;
    if (!reliable) {
      if (++j.attempts >= kJobAttempts) {  // the listing keeps failing: give way, retried later
        giveWay(j);
        if (j.kind == JOB_TRIM) continueWithRecount(j);
        else endJob();
      }
      return;
    }
    // A recount reads every record in listing order: the conversation's
    // chat index is built from those same reads.
    if (j.kind == JOB_RECOUNT)
      MessageStore::beginIndexFeed(j.group_code, j.contact_key, j.seqs, j.n, true, TextMessage::isAnyConversationOpen());
    if (j.kind == JOB_TRIM) {
      sortAscending(j.seqs, j.n);  // oldest first: the order appends evict in
      j.excess = j.n > MessageStore::kRetainedMessagesPerThread
                     ? static_cast<uint16_t>(j.n - MessageStore::kRetainedMessagesPerThread)
                     : 0;
      if (truncated) giveWay(j);  // records beyond the listing: the next pass trims them
    }
    return;
  }
  if (j.kind == JOB_TRIM) {
    stepTrim(j);
    return;
  }
  uint32_t stepStart = millis();
  for (uint8_t loads = 0; loads < kLoadsPerStep && j.pos < j.n; loads++) {
    if (loads > 0 && millis() - stepStart >= kStepBudgetMs) return;
    MessageRef ref;
    strncpy(ref.group_code, j.group_code, sizeof(ref.group_code) - 1);
    ref.group_code[sizeof(ref.group_code) - 1] = '\0';
    strncpy(ref.contact_key, j.contact_key, sizeof(ref.contact_key) - 1);
    ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
    ref.sequence = j.seqs[j.pos++];
    StoredMessageView view;
    if (!MessageStore::loadMessage(ref, &view)) {
      // Evicted since the listing (eviction restarts a recount) or unreadable.
      j.unreliable = true;
      if (j.kind == JOB_RECOUNT) MessageStore::feedIndexRecord(ref.group_code, ref.contact_key, ref.sequence, nullptr, nullptr);
      continue;
    }
    if (j.kind == JOB_RECOUNT)
      MessageStore::feedIndexRecord(ref.group_code, ref.contact_key, ref.sequence, &view.header, view.envelope.message_id);
    if (view.header.direction != MessageStore::Direction::RECEIVED || !(view.header.flags & MessageStore::FLAG_UNREAD)) continue;
    int slot = slotForType(view.envelope.message_type);
    if (slot >= 0) j.tally[slot]++;
  }
  if (j.pos < j.n) return;
  if (j.restart) return;  // relisted at the next step
  finishRecount();
}

// One bounded step of background work per kStepMs, in priority order: retry
// an unreadable stored summary (every kRetryMs), write a due summary, apply
// a due per-message read, then advance (or start) a job. A summary write that keeps
// failing is retried every kRetryMs without stopping the other work.
void serviceTick() {
  uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextStepMs) < 0) return;
  g_nextStepMs = now + kStepMs;
  // A chat loading its index goes first: its reads are what the user waits for.
  if (MessageStore::chatIndexLoading()) return;
  if (g_loadState != LOAD_OK && static_cast<int32_t>(now - g_nextRetryMs) >= 0) {
    g_nextRetryMs = now + kRetryMs;
    retryLoad();
    return;
  }
  // (an unreadable stored record is never overwritten: no write until it loads)
  if (g_saveDirty && g_loadState == LOAD_OK && static_cast<int32_t>(now - g_saveDueMs) >= 0) {
    writeSummaries();
    return;
  }
  // One background storage step per main-loop pass, shared with the Outbox.
  if (!claimBackgroundStorageStep(BG_NOTIFICATIONS)) {
    g_nextStepMs = now;  // not this pass: the next one
    return;
  }
  if (stepPendingRead()) return;  // the user is looking at it: before any job
  if (g_job != nullptr) {
    stepJob();
    return;
  }
  startNextJob();
}

// Whether one summary lights a badge category. COUNTED: counts; UNRESOLVED:
// category flags (counts unknown, so possibly unread) -- the same test.
bool summaryShows(const Summary& s, uint8_t mask) {
  if (s.state == SUMMARY_FREE) return false;
  if ((mask & BADGE_TEXT) && (s.unread[SLOT_TEXT] > 0 || s.unread[SLOT_GAME] > 0)) return true;
  if ((mask & BADGE_ENIGMA) && (s.unread[SLOT_ENIGMA] > 0 || s.unread[SLOT_GAME] > 0)) return true;
  return false;  // no stored message type maps to BADGE_TRAINING_GAME (Race ORs in its own flag)
}

bool hasBadge(uint8_t mask) {
  for (auto& s : g_summaries)
    if (summaryShows(s, mask)) return true;
  return false;
}

// An UNREAD record (or one whose header could not be read) was evicted --
// its removal succeeded (the append path's retention cap or low-space
// cleanup; background trimming deletes through deleteMessage(), uncounting
// and forgetting as it goes, and never comes here).
// Its type can no longer be read, so the conversation becomes UNRESOLVED (its
// badges stay lit) and is recounted in the background -- never here: this
// runs inside an append, on the arrival path.
void onMessageEvicted(const MessageRef& ref, uint16_t flags) {
  conversationChanged(ref.group_code, ref.contact_key);
  forgetPendingReads(ref.group_code, ref.contact_key, &ref.sequence);
  if (!(flags & MessageStore::FLAG_UNREAD)) return;
  Summary* s = findSummary(ref.group_code, ref.contact_key);
  if (s == nullptr) return;  // not listed: nothing to correct (the sweep covers the rest)
  markUnresolved(s, 0);
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
  // (Its pending reads end at their next attempt, once the group is no
  // longer configured: applyRead().)
}

void serviceInit() {
  discardOldReadUpRecord();
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
                      uint8_t badgeMask, bool conversationCurrentlyOpen, bool readIfOpen) {
  conversationChanged(group_code, contact_key);
  StoredMessageView view;
  bool loaded = MessageStore::loadMessage(ref, &view);
  if (conversationCurrentlyOpen && readIfOpen) {
    if (!loaded || (view.header.flags & MessageStore::FLAG_UNREAD))
      MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD);
    return;
  }
  // The arrival handlers store it unread already; rewrite only if not.
  if (loaded && !(view.header.flags & MessageStore::FLAG_UNREAD)) {
    MessageStore::updateLocalFlags(ref, MessageStore::FLAG_UNREAD, 0);
  } else if (!loaded) {
    MessageStore::updateLocalFlags(ref, MessageStore::FLAG_UNREAD, 0);
  }
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

  if (conversationCurrentlyOpen) return;  // on screen already (and still unread: readIfOpen false)
  playTone(1000, 150, SOUND_NOTIFICATION);
  Sleep::notifyActivity();
}

void clearUnread(const char* group_code, const char* contact_key, const MessageRef& ref) {
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  if (!(view.header.flags & MessageStore::FLAG_UNREAD)) return;

  if (!MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD)) return;  // still unread: still counted
  conversationChanged(group_code, contact_key);
  forgetPendingReads(group_code, contact_key, &ref.sequence, view.envelope.message_id);  // read: nothing left to do
  Summary* s = findSummary(group_code, contact_key);
  if (s == nullptr || s->state == SUMMARY_UNRESOLVED) return;  // an UNRESOLVED one is recounted in the background
  int slot = slotForType(view.envelope.message_type);
  if (slot >= 0 && s->unread[slot] > 0) {
    s->unread[slot]--;
    saveSummaries();
  }
}

bool readMovedOnto(const char* group_code, const char* contact_key, uint32_t sequence, const char* message_id) {
  if (group_code == nullptr || contact_key == nullptr || message_id == nullptr) return false;
  PendingRead r;
  memset(&r, 0, sizeof(r));
  strncpy(r.group_code, group_code, sizeof(r.group_code) - 1);
  strncpy(r.contact_key, contact_key, sizeof(r.contact_key) - 1);
  memcpy(r.messageId, message_id, sizeof(r.messageId));
  r.sequence = sequence;
  if (g_reads != nullptr) {
    for (uint8_t i = 0; i < g_reads->count; i++) {
      const PendingRead& p = g_reads->items[i];
      if (p.sequence == sequence && sameConversation(p.group_code, p.contact_key, group_code, contact_key) &&
          strncmp(p.messageId, r.messageId, sizeof(r.messageId)) == 0)
        return true;  // already accepted
    }
  } else {
    g_reads = new (std::nothrow) PendingReads;
    if (g_reads == nullptr) {
      Serial.printf("[NOTIFY] read of %s/%s #%lu refused: no memory; left unread\n", group_code, contact_key,
                    static_cast<unsigned long>(sequence));
      return false;
    }
    g_reads->count = 0;
  }
  if (g_reads->count >= kMaxPendingReads) {
    Serial.printf("[NOTIFY] read of %s/%s #%lu refused: %u reads pending; left unread\n", group_code, contact_key,
                  static_cast<unsigned long>(sequence), static_cast<unsigned>(g_reads->count));
    return false;
  }
  g_reads->items[g_reads->count++] = r;
  return true;
}

uint16_t getUnreadCount(const char* group_code, const char* contact_key) {
  Summary* s = findSummary(group_code, contact_key);
  return s != nullptr ? totalUnread(*s) : 0;
}

bool hasUnreadIn(const char* group_code, const char* contact_key, uint8_t badgeMask) {
  if (group_code == nullptr) return false;
  for (auto& s : g_summaries) {
    if (strcmp(s.group_code, group_code) != 0) continue;
    if (contact_key != nullptr && strcmp(s.contact_key, contact_key) != 0) continue;
    if (summaryShows(s, badgeMask)) return true;
  }
  return false;
}

const char* nextUnreadContactIn(const char* group_code, uint8_t badgeMask, uint8_t& cursor) {
  if (group_code == nullptr) return nullptr;
  while (cursor < kMaxSummaries) {
    const Summary& s = g_summaries[cursor++];
    if (strcmp(s.group_code, group_code) == 0 && summaryShows(s, badgeMask)) return s.contact_key;
  }
  return nullptr;
}

bool hasAnyUnreadText() { return hasBadge(BADGE_TEXT); }
bool hasAnyUnreadEnigma() { return hasBadge(BADGE_ENIGMA); }
bool hasAnyUnreadTrainingGame() { return hasBadge(BADGE_TRAINING_GAME) || g_raceInvitePending; }

void setRaceInvitePending(bool pending) { g_raceInvitePending = pending; }

}  // namespace Notifications
