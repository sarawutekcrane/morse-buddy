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
constexpr const char* kReadUpKey = "readup";

// ---- reading up to a selection (hardware findings 1 and 3 on 23293c5) -----
//
// Selecting a received message in a chat reads it AND every older received
// message of the same type in that conversation: older means at or above it
// in the history (sort key (effective_sort_timestamp, message_id) <= the
// selected one's) and stored no later than it (sequence <= its sequence), so
// a message shown below it, or one that arrived after it (even with an older
// timestamp), stays unread. Text, Enigma and Friend-game records are separate
// types: reading one never reads another. Older firmware never cleared a
// stored record's unread flag on viewing, so long-used devices hold many such
// flags; this is how they get cleared without selecting each one.
//
// The selection itself only records the request: in RAM at once, and in the
// "readup" NVS record about kReadUpSaveDelayMs later (coalesced). A power
// loss before that write loses the selection: its messages stay unread
// (never wrongly read) and are counted as such after the reboot; flags
// already cleared on flash stay cleared. Once written, a reboot resumes it.
// Clearing the flags on flash is background work done a few records per
// step (below); until then counts and recounts treat a covered message as
// read. A read-up is marked done only after a pass finds nothing left to
// clear and nothing it could not read; until then it is retried, with a
// growing pause between attempts so the other background work goes on.
constexpr uint8_t kMaxReadUps = 8;
struct ReadUp {
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  uint8_t type;
  uint8_t done;  // applied and verified on flash; kept only so a covered selection adds nothing
  uint32_t sortTs;
  char messageId[PacketCodec::kMessageIdLen];
  uint32_t sequence;
  uint8_t failures;    // attempts that ended with something left (RAM; reset at load)
  uint32_t retryAtMs;  // no attempt before this (RAM; reset at load)
};
struct ReadUpRecord {
  uint8_t count;
  ReadUp items[kMaxReadUps];
};

// ---- background work: bounded per main-loop pass -------------------------
//
// Recounts (UNRESOLVED conversations, the sweep below) and read-ups are jobs
// over one conversation, run one step at a time from serviceTick(), at most
// one step per kStepMs. A step is ONE of: one conversation directory listing
// (names only, at most 300 entries), up to kLoadsPerStep record reads, one
// record rewrite (plus the reads before it in that step), one NVS write, or
// one retry of an unreadable summary record. Nothing on the arrival, read or
// eviction paths scans a whole conversation.
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
constexpr uint8_t kCountLoads = 4;       // record reads a selection may spend to count what stays unread
constexpr uint32_t kSaveDelayMs = 2000;  // summary writes are coalesced
constexpr uint32_t kReadUpSaveDelayMs = 1000;
constexpr uint8_t kReadUpPasses = 3;     // passes per attempt; a pass that rewrote anything is checked by the next
constexpr uint8_t kWalkEntriesPerStep = 16;
constexpr uint8_t kJobAttempts = 5;      // unreliable listings or passes before a job gives way (and backs off)
constexpr uint32_t kBackoffMinMs = 2000;
constexpr uint32_t kBackoffMaxMs = 60000;

struct Sweep {
  MessageStore::ConversationWalk* walk;  // nullptr between passes
  bool incomplete;                       // this pass left something for the next one
  uint32_t nextPassMs;                   // a pass after an incomplete one starts no earlier
  uint32_t backoffMs;
};

enum JobKind : uint8_t { JOB_RECOUNT = 1, JOB_READUP = 2, JOB_TRIM = 3 };
struct Job {
  JobKind kind;
  bool fromSweep;
  bool listed;      // seqs[] holds the conversation's records
  bool restart;     // the conversation changed under a recount: list and count again
  bool unreliable;  // a record could not be read in this pass
  uint8_t pass;     // read-up: passes made
  uint8_t attempts; // unreliable listings or passes so far
  uint16_t rewrites;
  uint16_t excess;  // trim: records still to delete
  uint16_t n, pos;
  uint16_t tally[kTypeSlots];
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  ReadUp readUp;
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
ReadUpRecord* g_readUps = nullptr;  // non-null while any read-up is kept
bool g_readUpsDirty = false;
uint32_t g_readUpsDueMs = 0;

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

// ---- read-up records ------------------------------------------------------

// a covers b: same conversation and type, and b is at or above a in the
// history and stored no later than it.
bool readUpCovers(const ReadUp& a, const char* group_code, const char* contact_key, uint8_t type, uint32_t sortTs,
                  const char* messageId, uint32_t sequence) {
  if (a.type != type || !sameConversation(a.group_code, a.contact_key, group_code, contact_key)) return false;
  if (sequence > a.sequence) return false;
  if (sortTs != a.sortTs) return sortTs < a.sortTs;
  return strncmp(messageId, a.messageId, PacketCodec::kMessageIdLen) <= 0;
}

// True if a pending (not yet applied) read-up covers this received record.
bool coveredByPendingReadUp(const char* group_code, const char* contact_key, const MessageStore::StoredHeader& header,
                            const PacketCodec::MessageEnvelope& envelope, uint32_t sequence) {
  if (g_readUps == nullptr || header.direction != MessageStore::Direction::RECEIVED) return false;
  for (uint8_t i = 0; i < g_readUps->count; i++) {
    const ReadUp& r = g_readUps->items[i];
    if (!r.done && readUpCovers(r, group_code, contact_key, envelope.message_type, header.effective_sort_timestamp,
                                envelope.message_id, sequence))
      return true;
  }
  return false;
}

void readUpsChanged() {
  if (!g_readUpsDirty) g_readUpsDueMs = millis() + kReadUpSaveDelayMs;
  g_readUpsDirty = true;
}

void writeReadUps() {
  ReadUpRecord empty;
  memset(&empty, 0, sizeof(empty));
  const ReadUpRecord* r = g_readUps != nullptr ? g_readUps : &empty;
  if (Storage::notify().putBytes(kReadUpKey, r, sizeof(*r)) == sizeof(*r)) {
    g_readUpsDirty = false;
    return;
  }
  Serial.println("[NOTIFY] read-up record not saved; retrying (the flags are still cleared meanwhile)");
  g_readUpsDueMs = millis() + kRetryMs;
}

void loadReadUps() {
  Preferences& p = Storage::notify();
  if (!p.isKey(kReadUpKey)) return;
  ReadUpRecord* r = new (std::nothrow) ReadUpRecord;
  if (r == nullptr) return;
  if (p.getBytes(kReadUpKey, r, sizeof(*r)) != sizeof(*r) || r->count > kMaxReadUps) {
    // Unreadable: its selections are lost and those messages stay unread
    // (never wrongly read); the record is replaced at the next selection.
    Serial.println("[NOTIFY] read-up record unreadable; ignored");
    delete r;
    return;
  }
  for (uint8_t i = 0; i < r->count; i++) {
    r->items[i].group_code[sizeof(r->items[i].group_code) - 1] = '\0';
    r->items[i].contact_key[sizeof(r->items[i].contact_key) - 1] = '\0';
    r->items[i].failures = 0;  // a new boot retries at once
    r->items[i].retryAtMs = 0;
  }
  if (r->count == 0) {
    delete r;
    return;
  }
  g_readUps = r;
}

// Adds a read-up for the selected record. False if an existing one already
// covers it (nothing new to read).
bool addReadUp(const ReadUp& add) {
  if (g_readUps == nullptr) {
    g_readUps = new (std::nothrow) ReadUpRecord;
    if (g_readUps == nullptr) return false;
    memset(g_readUps, 0, sizeof(*g_readUps));
  }
  ReadUpRecord& r = *g_readUps;
  for (uint8_t i = 0; i < r.count; i++) {
    if (readUpCovers(r.items[i], add.group_code, add.contact_key, add.type, add.sortTs, add.messageId, add.sequence))
      return false;
  }
  // Drop what the new one covers, then make room: a finished one first, else
  // the oldest pending one (its messages stay unread -- never wrongly read).
  uint8_t out = 0;
  for (uint8_t i = 0; i < r.count; i++) {
    const ReadUp& x = r.items[i];
    if (!readUpCovers(add, x.group_code, x.contact_key, x.type, x.sortTs, x.messageId, x.sequence)) r.items[out++] = x;
  }
  r.count = out;
  if (r.count == kMaxReadUps) {
    uint8_t victim = 0;
    for (uint8_t i = 0; i < r.count; i++) {
      if (r.items[i].done) {
        victim = i;
        break;
      }
    }
    if (!r.items[victim].done) Serial.println("[NOTIFY] read-up list full; oldest selection dropped");
    for (uint8_t i = victim; i + 1 < r.count; i++) r.items[i] = r.items[i + 1];
    r.count--;
  }
  r.items[r.count++] = add;
  readUpsChanged();
  return true;
}

// The same selection (the RAM-only retry fields aside).
bool sameReadUp(const ReadUp& a, const ReadUp& b) {
  return a.type == b.type && a.sortTs == b.sortTs && a.sequence == b.sequence &&
         sameConversation(a.group_code, a.contact_key, b.group_code, b.contact_key) &&
         strncmp(a.messageId, b.messageId, PacketCodec::kMessageIdLen) == 0;
}

ReadUp* findReadUp(const ReadUp& r) {
  if (g_readUps == nullptr) return nullptr;
  for (uint8_t i = 0; i < g_readUps->count; i++)
    if (sameReadUp(g_readUps->items[i], r)) return &g_readUps->items[i];
  return nullptr;
}

void markReadUpDone(const ReadUp& finished) {
  ReadUp* x = findReadUp(finished);
  if (x == nullptr) return;
  x->done = 1;
  readUpsChanged();
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
void sweepStep() {
  uint32_t now = millis();
  if (g_sweep->walk == nullptr) {
    if (static_cast<int32_t>(now - g_sweep->nextPassMs) < 0) return;
    g_sweep->walk = MessageStore::beginConversationWalk();  // nullptr (no memory): tried again at the next step
    return;
  }
  char group[33], contact[MessageStore::kContactKeyLen];
  MessageStore::WalkResult r =
      MessageStore::stepConversationWalk(g_sweep->walk, kWalkEntriesPerStep, group, sizeof(group), contact, sizeof(contact));
  if (r == MessageStore::WalkResult::MORE) return;
  if (r == MessageStore::WalkResult::FOUND) {
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
  g_sweep->nextPassMs = now + g_sweep->backoffMs;
  g_sweep->backoffMs = g_sweep->backoffMs * 2 < kBackoffMaxMs ? g_sweep->backoffMs * 2 : kBackoffMaxMs;
}

// Starts the next job, or advances the sweep (one step's work at most).
// Order: a pending read-up that is due (the user is waiting to see it), then
// an UNRESOLVED conversation, then the sweep.
void startNextJob() {
  uint32_t now = millis();
  if (g_readUps != nullptr) {
    for (uint8_t i = 0; i < g_readUps->count; i++) {
      const ReadUp& r = g_readUps->items[i];
      if (r.done || (r.retryAtMs != 0 && static_cast<int32_t>(now - r.retryAtMs) < 0)) continue;
      g_job = newJob(JOB_READUP, r.group_code, r.contact_key);
      if (g_job != nullptr) g_job->readUp = r;
      return;
    }
  }
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

// A read-up pass is complete. A pass that rewrote anything is checked by
// another (a rewrite can fail and leave the flag set); one that rewrote
// nothing and read everything finishes it, and the conversation is
// recounted. An attempt that still leaves something after kReadUpPasses
// passes keeps the read-up pending (never marked done) and retries it after
// a growing pause, while the other work goes on.
void finishReadUp() {
  Job& j = *g_job;
  bool left = j.rewrites > 0 || j.unreliable;
  if (left && j.pass + 1 < kReadUpPasses) {
    j.pass++;
    j.rewrites = 0;
    j.unreliable = false;
    j.listed = false;
    return;
  }
  if (left) {
    ReadUp* x = findReadUp(j.readUp);
    if (x != nullptr) {
      if (x->failures < 255) x->failures++;
      x->retryAtMs = millis() + backoffMs(x->failures);
      if (x->retryAtMs == 0) x->retryAtMs = 1;
    }
    Serial.println("[NOTIFY] read-up not complete; retried later");
    endJob();
    return;
  }
  markReadUpDone(j.readUp);
  Summary* s = findSummary(j.group_code, j.contact_key);
  if (s != nullptr && s->state == SUMMARY_COUNTED) {
    markUnresolved(s, 0);  // recounted next, from the flags just written
    saveSummaries();
  }
  endJob();
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
  for (uint8_t loads = 0; loads < kLoadsPerStep && j.pos < j.n; loads++) {
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
                   (view.header.flags & MessageStore::FLAG_UNREAD) && slot >= 0 &&
                   !coveredByPendingReadUp(j.group_code, j.contact_key, view.header, view.envelope, ref.sequence);
    if (!MessageStore::deleteMessage(ref)) {
      Serial.println("[NOTIFY] trim: a record could not be deleted; retried later");
      giveWay(j);
      continueWithRecount(j);
      return;
    }
    j.pos++;
    j.excess--;
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
        if (j.kind == JOB_READUP) {
          j.unreliable = true;
          j.pass = kReadUpPasses;  // this attempt ends with something left
          finishReadUp();
          return;
        }
        giveWay(j);
        if (j.kind == JOB_TRIM) continueWithRecount(j);
        else endJob();
      }
      return;
    }
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
  for (uint8_t loads = 0; loads < kLoadsPerStep && j.pos < j.n; loads++) {
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
      continue;
    }
    if (view.header.direction != MessageStore::Direction::RECEIVED || !(view.header.flags & MessageStore::FLAG_UNREAD)) continue;
    if (j.kind == JOB_RECOUNT) {
      int slot = slotForType(view.envelope.message_type);
      if (slot >= 0 && !coveredByPendingReadUp(j.group_code, j.contact_key, view.header, view.envelope, ref.sequence))
        j.tally[slot]++;
      continue;
    }
    if (readUpCovers(j.readUp, j.group_code, j.contact_key, view.envelope.message_type, view.header.effective_sort_timestamp,
                     view.envelope.message_id, ref.sequence)) {
      MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD);
      j.rewrites++;
      return;  // at most one rewrite per step
    }
  }
  if (j.pos < j.n) return;
  if (j.kind == JOB_RECOUNT) {
    if (j.restart) return;  // relisted at the next step
    finishRecount();
  } else {
    finishReadUp();
  }
}

// One bounded step of background work per kStepMs, in priority order: retry
// an unreadable stored summary (every kRetryMs), write a due summary or
// read-up record, then advance (or start) a job. A summary write that keeps
// failing is retried every kRetryMs without stopping the other work.
void serviceTick() {
  uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextStepMs) < 0) return;
  g_nextStepMs = now + kStepMs;
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
  if (g_readUpsDirty && static_cast<int32_t>(now - g_readUpsDueMs) >= 0) {
    writeReadUps();
    return;
  }
  if (g_job != nullptr) {
    stepJob();
    return;
  }
  startNextJob();
}

bool hasBadge(uint8_t mask) {
  for (auto& s : g_summaries) {
    if (s.state == SUMMARY_FREE) continue;  // COUNTED: counts; UNRESOLVED: category flags -- same test
    if ((mask & BADGE_TEXT) && (s.unread[SLOT_TEXT] > 0 || s.unread[SLOT_GAME] > 0)) return true;
    if ((mask & BADGE_ENIGMA) && (s.unread[SLOT_ENIGMA] > 0 || s.unread[SLOT_GAME] > 0)) return true;
  }
  return false;  // no stored message type maps to BADGE_TRAINING_GAME (Race ORs in its own flag)
}

// An UNREAD record was evicted (the append path's retention cap or low-space
// cleanup; background trimming deletes through deleteMessage(), uncounting as
// it goes, and never comes here).
// Its type can no longer be read, so the conversation becomes UNRESOLVED (its
// badges stay lit) and is recounted in the background -- never here: this
// runs inside an append, on the arrival path.
void onMessageEvicted(const MessageRef& ref, uint16_t flags) {
  conversationChanged(ref.group_code, ref.contact_key);
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
}

void serviceInit() {
  loadReadUps();
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
  conversationChanged(group_code, contact_key);
  StoredMessageView view;
  bool loaded = MessageStore::loadMessage(ref, &view);
  if (conversationCurrentlyOpen) {
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

  playTone(1000, 150, SOUND_NOTIFICATION);
  Sleep::notifyActivity();
}

void clearUnread(const char* group_code, const char* contact_key, const MessageRef& ref) {
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  if (!(view.header.flags & MessageStore::FLAG_UNREAD)) return;

  MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD);
  conversationChanged(group_code, contact_key);
  Summary* s = findSummary(group_code, contact_key);
  if (s == nullptr || s->state == SUMMARY_UNRESOLVED) return;  // an UNRESOLVED one is recounted in the background
  if (coveredByPendingReadUp(group_code, contact_key, view.header, view.envelope, ref.sequence)) return;  // counted as read already
  int slot = slotForType(view.envelope.message_type);
  if (slot >= 0 && s->unread[slot] > 0) {
    s->unread[slot]--;
    saveSummaries();
  }
}

void readUpTo(const char* group_code, const char* contact_key, uint16_t selectedIndex, uint16_t indexTotal) {
  const MessageStore::ConversationIndexEntry* sel = MessageStore::getIndexEntry(selectedIndex);
  if (sel == nullptr || selectedIndex >= indexTotal) return;
  // Nothing at or above it that may be unread: nothing to read (the index's
  // flags can only over-report: a cleared flag shows as unread until the
  // chat reloads its index).
  bool anyUnread = false;
  for (uint16_t i = 0; i <= selectedIndex && !anyUnread; i++) {
    const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
    anyUnread = e != nullptr && (e->flags & MessageStore::FLAG_UNREAD) && e->sequence <= sel->sequence;
  }
  if (!anyUnread) return;

  MessageRef ref;
  strncpy(ref.group_code, group_code, sizeof(ref.group_code) - 1);
  ref.group_code[sizeof(ref.group_code) - 1] = '\0';
  strncpy(ref.contact_key, contact_key, sizeof(ref.contact_key) - 1);
  ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
  ref.sequence = sel->sequence;
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  if (view.header.direction != MessageStore::Direction::RECEIVED) return;
  int slot = slotForType(view.envelope.message_type);
  if (slot < 0) return;

  ReadUp r;
  memset(&r, 0, sizeof(r));
  strncpy(r.group_code, group_code, sizeof(r.group_code) - 1);
  strncpy(r.contact_key, contact_key, sizeof(r.contact_key) - 1);
  r.type = view.envelope.message_type;
  r.sortTs = view.header.effective_sort_timestamp;
  memcpy(r.messageId, view.envelope.message_id, sizeof(r.messageId));
  r.sequence = sel->sequence;
  if (!addReadUp(r)) return;  // covered by an earlier selection
  conversationChanged(group_code, contact_key);  // a recount under way may have counted what is now read

  // The badge follows at once when what stays unread of this type can be
  // counted with a few reads: records below the selection, or stored after
  // it, whose index flag shows unread. Otherwise the recount after the
  // read-up sets it (the categories stay lit meanwhile).
  Summary* s = findSummary(group_code, contact_key);
  if (s == nullptr) return;
  uint16_t remaining = 0;
  uint8_t loads = 0;
  for (uint16_t i = 0; i < indexTotal; i++) {
    const MessageStore::ConversationIndexEntry* e = MessageStore::getIndexEntry(i);
    if (e == nullptr || !(e->flags & MessageStore::FLAG_UNREAD)) continue;
    if (i <= selectedIndex && e->sequence <= r.sequence) continue;  // at or above it: read now (if this type)
    if (loads++ == kCountLoads) return;
    ref.sequence = e->sequence;
    StoredMessageView v;
    if (!MessageStore::loadMessage(ref, &v)) return;
    if (v.header.direction == MessageStore::Direction::RECEIVED && (v.header.flags & MessageStore::FLAG_UNREAD) &&
        v.envelope.message_type == r.type && !coveredByPendingReadUp(group_code, contact_key, v.header, v.envelope, e->sequence))
      remaining++;
  }
  if (s->state == SUMMARY_COUNTED) {
    if (s->unread[slot] == remaining) return;
    s->unread[slot] = remaining;
  } else {
    s->unread[slot] = remaining > 0 ? 1 : 0;
  }
  saveSummaries();
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
