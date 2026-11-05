#include "core/storage_messages.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <stdlib.h>
#include <new>
#include <string.h>

#include "core/identity.h"
#include "core/settings.h"

namespace MessageStore {

namespace {

// ---------------------------------------------------------------------------
// Little-endian helpers (kept local; storage_messages owns its own on-disk
// format, separate from the wire PacketCodec format it embeds verbatim).
// ---------------------------------------------------------------------------
void writeU16LE(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
uint16_t readU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
void writeU32LE(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
uint32_t readU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

constexpr size_t kStoredHeaderDiskSize = 1 + 2 + 4 + 4;  // direction+flags+local_write_seq+effective_sort_ts = 11
constexpr size_t kMaxRecordFileSize = 1024;
constexpr size_t kMessageIdOffsetInFile = kStoredHeaderDiskSize + 2 + PacketCodec::kHeaderSize;  // = 19

EvictedCallback g_onEvicted = nullptr;
PendingWrittenCallback g_onPendingWritten = nullptr;
uint32_t g_externalMutations = 0;

// Chat-entry latency (device A on 2ed04b6: one LittleFS open per record,
// about 63 ms each). Defined further down; the low-level primitives below
// call them around every record write and removal.
void bumpGenerationFor(const char* group_code, const char* contact_key);
void afterRecordWrite(const MessageRef& ref, const StoredHeader& header, bool ok);
void afterRecordRemove(const char* group_code, const char* contact_key, uint32_t seq, bool removed);
void dropCachedRecord(const char* group_code, const char* contact_key, uint32_t seq);
void dropAllCachedRecords();
bool cachedRecordBytes(const char* group_code, const char* contact_key, uint32_t seq, const uint8_t** bytes, uint16_t* len);

// Hardware Diagnostic #4.9h: see getStorageChangeGeneration()'s header
// comment in storage_messages.h. Bumped at the two low-level primitives
// every actual message-record mutation in this file funnels through --
// atomicRewrite() (append, flag changes, local-payload rewrites all call
// it) and removeRecordFile() (both eviction paths call it) -- BEFORE
// their own destructive filesystem work begins. This is deliberately
// unconditional, even on a path that goes on to fail/return false: a
// failed atomicRewrite() can still have already removed the old final
// file before its rename failed (see atomicRewrite() below), so the safe
// rule is "the mutation may have touched disk the moment it was
// attempted," not "only once it's confirmed to have succeeded." Never
// decremented or reset except by reboot (0 at cold start).
uint32_t g_storageChangeGeneration = 0;

// Hardware Diagnostic #4.9u: a SEPARATE RAM-only generation counter,
// bumped alongside (never instead of) g_storageChangeGeneration above at
// every mutation that could affect whether ANY pending-outbox record
// exists anywhere -- but, unlike g_storageChangeGeneration, deliberately
// NOT bumped by a successful non-pending atomicRewrite() (see that
// function's own comment below), so Outbox's empty-scan cache
// (outbox.cpp) can survive exactly that one case: a successful write
// whose resulting on-disk header has no FLAG_PENDING_OUTBOX bit, which by
// construction cannot have created a new pending record and therefore
// cannot invalidate a previously-established "no pending messages
// anywhere" answer. Every other mutation this file makes -- a pending
// append or flag-set, ANY failed write regardless of its intended flags,
// every deletion, and every external mutation notification -- still
// bumps this exactly like the general generation, conservatively. Same
// usage contract as g_storageChangeGeneration: RAM-only, starts at 0
// every boot, never decremented or reset except by reboot, only ever
// compared between two readings taken within the same boot session.
uint32_t g_outboxChangeGeneration = 0;

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
void buildConversationDir(const char* group_code, const char* contact_key, char* outPath, size_t outSize) {
  snprintf(outPath, outSize, "/messages/%s/%s", group_code, contact_key);
}
void buildGroupDir(const char* group_code, char* outPath, size_t outSize) {
  snprintf(outPath, outSize, "/messages/%s", group_code);
}
void buildRecordPath(const char* group_code, const char* contact_key, uint32_t sequence, char* outPath,
                     size_t outSize) {
  snprintf(outPath, outSize, "/messages/%s/%s/%08lu.msg", group_code, contact_key,
           static_cast<unsigned long>(sequence));
}
void ensureDirExists(const char* path) {
  if (!LittleFS.exists(path)) LittleFS.mkdir(path);
}
const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash != nullptr ? slash + 1 : path;
}

// ---------------------------------------------------------------------------
// Low-level record I/O
// ---------------------------------------------------------------------------
// Hardware Diagnostic #4.9h follow-up: optional trailing output, defaulted
// to nullptr in the header declaration so every existing call site (there
// are several -- findMaxSequence(), countMessages(),
// findOldestNonPendingSequence(), rebuildRingFromStorage(),
// findGlobalOldestNonPending(), loadConversationIndex(),
// findMessageById()) compiles and behaves exactly as before, unaware this
// parameter exists. When non-null, set true only if this specific
// directory could not be opened/confirmed as a directory at all (a real
// I/O anomaly on a path already known to exist by the caller -- see
// findMessagesByPredicate()'s scanConversation lambda, its one consumer,
// which only ever calls this for a contact_key directory it just finished
// enumerating as present); left false for a directory that opened fine
// and simply contained zero (or more) message files, which is not a
// failure. Residual limitation: the underlying Arduino FS API's
// openNextFile() returns the same falsy File whether a directory listing
// reached a genuinely clean end or was cut short by an underlying error
// partway through -- there is no distinct signal for that case, so a
// mid-iteration failure here is NOT detected or reported, only a failure
// to open the directory in the first place.
uint16_t listSequences(const char* group_code, const char* contact_key, uint32_t* outSeqs, uint16_t capacity,
                       bool* outOpenFailed = nullptr, bool* outTruncated = nullptr) {
  if (outOpenFailed != nullptr) *outOpenFailed = false;
  if (outTruncated != nullptr) *outTruncated = false;
  char dirPath[80];
  buildConversationDir(group_code, contact_key, dirPath, sizeof(dirPath));
  uint16_t count = 0;
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    if (outOpenFailed != nullptr) *outOpenFailed = true;
    return 0;
  }
  // Names only (readdir): openNextFile() would open every record just to
  // read its name. Only "<sequence>.msg" counts -- atomicRewrite()'s
  // "<sequence>.msg.tmp" would otherwise list the same sequence twice.
  bool isDir = false;
  String entry = dir.getNextFileName(&isDir);
  while (entry.length() > 0) {
    const char* name = baseName(entry.c_str());
    size_t len = strlen(name);
    if (!isDir && len > 4 && strcmp(name + len - 4, ".msg") == 0) {
      uint32_t seq = static_cast<uint32_t>(strtoul(name, nullptr, 10));
      if (count < capacity) {
        outSeqs[count++] = seq;
      } else if (capacity > 0) {
        // Over capacity: keep the lowest sequences (replace the highest
        // kept), so a truncated listing still holds the oldest records.
        if (outTruncated != nullptr) *outTruncated = true;
        uint16_t hi = 0;
        for (uint16_t k = 1; k < count; k++)
          if (outSeqs[k] > outSeqs[hi]) hi = k;
        if (seq < outSeqs[hi]) outSeqs[hi] = seq;
      }
    }
    entry = dir.getNextFileName(&isDir);
  }
  dir.close();
  return count;
}

uint32_t findMaxSequence(const char* group_code, const char* contact_key) {
  static uint32_t seqs[kMaxMessagesPerThread];
  uint16_t n = listSequences(group_code, contact_key, seqs, kMaxMessagesPerThread);
  uint32_t maxSeq = 0;
  for (uint16_t i = 0; i < n; i++) {
    if (seqs[i] > maxSeq) maxSeq = seqs[i];
  }
  return maxSeq;
}

// Hardware Diagnostic #4.9p: optional trailing outputs, both defaulted to
// nullptr so the (currently single) existing call site compiles and
// behaves exactly as before if left unspecified, and the uint16_t count
// return is unchanged. Reuses the SAME single listSequences() call and
// static seqs[] buffer this function already had -- no second 300-entry
// buffer, no change to filename parsing, enumeration limits, ordering, or
// listSequences() semantics. outMaxSeq, when non-null, is computed with
// the identical comparison and initial value (0) findMaxSequence() uses,
// over exactly the n entries listSequences() returned -- not a separate
// scan. outOpenFailed is passed straight through to listSequences()'s own
// existing parameter of the same name/meaning.
uint16_t countMessages(const char* group_code, const char* contact_key, uint32_t* outMaxSeq = nullptr,
                       bool* outOpenFailed = nullptr) {
  static uint32_t seqs[kMaxMessagesPerThread];
  uint16_t n = listSequences(group_code, contact_key, seqs, kMaxMessagesPerThread, outOpenFailed);
  if (outMaxSeq != nullptr) {
    uint32_t maxSeq = 0;
    for (uint16_t i = 0; i < n; i++) {
      if (seqs[i] > maxSeq) maxSeq = seqs[i];
    }
    *outMaxSeq = maxSeq;
  }
  return n;
}

void decodeStoredHeader(const uint8_t* buf, StoredHeader* outHdr) {
  outHdr->direction = static_cast<Direction>(buf[0]);
  outHdr->flags = readU16LE(&buf[1]);
  outHdr->local_write_sequence = readU32LE(&buf[3]);
  outHdr->effective_sort_timestamp = readU32LE(&buf[7]);
}

bool readHeaderOnly(const char* group_code, const char* contact_key, uint32_t seq, StoredHeader* outHdr) {
  const uint8_t* cached = nullptr;
  uint16_t cachedLen = 0;
  if (cachedRecordBytes(group_code, contact_key, seq, &cached, &cachedLen) && cachedLen >= kStoredHeaderDiskSize) {
    decodeStoredHeader(cached, outHdr);
    return true;
  }
  char path[96];
  buildRecordPath(group_code, contact_key, seq, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t buf[kStoredHeaderDiskSize];
  size_t r = f.read(buf, sizeof(buf));
  f.close();
  if (r != sizeof(buf)) return false;
  outHdr->direction = static_cast<Direction>(buf[0]);
  outHdr->flags = readU16LE(&buf[1]);
  outHdr->local_write_sequence = readU32LE(&buf[3]);
  outHdr->effective_sort_timestamp = readU32LE(&buf[7]);
  return true;
}

bool readMessageIdFromFile(const char* group_code, const char* contact_key, uint32_t seq,
                           char outId[PacketCodec::kMessageIdLen]) {
  char path[96];
  buildRecordPath(group_code, contact_key, seq, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  bool ok = f.seek(kMessageIdOffsetInFile);
  size_t r = 0;
  if (ok) r = f.read(reinterpret_cast<uint8_t*>(outId), PacketCodec::kMessageIdLen);
  f.close();
  if (!ok || r != PacketCodec::kMessageIdLen) return false;
  outId[PacketCodec::kMessageIdLen - 1] = '\0';
  return true;
}

bool findOldestNonPendingSequence(const char* group_code, const char* contact_key, uint32_t* outSeq) {
  static uint32_t seqs[kMaxMessagesPerThread];
  uint16_t n = listSequences(group_code, contact_key, seqs, kMaxMessagesPerThread);
  // Insertion sort ascending (n <= 300; this only runs on eviction, not per-frame).
  for (uint16_t i = 1; i < n; i++) {
    uint32_t key = seqs[i];
    int32_t j = static_cast<int32_t>(i) - 1;
    while (j >= 0 && seqs[j] > key) {
      seqs[j + 1] = seqs[j];
      j--;
    }
    seqs[j + 1] = key;
  }
  for (uint16_t i = 0; i < n; i++) {
    StoredHeader hdr;
    if (readHeaderOnly(group_code, contact_key, seqs[i], &hdr) && !(hdr.flags & FLAG_PENDING_OUTBOX)) {
      *outSeq = seqs[i];
      return true;
    }
  }
  return false;
}

bool removeRecordFile(const char* group_code, const char* contact_key, uint32_t seq, bool notifyIfUnread) {
  // Hardware Diagnostic #4.9h: bumped unconditionally, before the
  // LittleFS.remove() below -- both eviction paths (findOldestNonPending-
  // Sequence()-driven per-thread FIFO eviction via evictOldest(), and
  // findGlobalOldestNonPending()-driven low-space eviction via
  // ensureFreeSpaceForWrite()) funnel through here, so this one call site
  // covers deletion for both. See g_storageChangeGeneration's own comment
  // above for why unconditional-before-the-attempt is the safe rule.
  bumpGenerationFor(group_code, contact_key);
  // Hardware Diagnostic #4.9u: conservative -- a deletion could remove a
  // pending OR non-pending record, and determining which would require
  // reading the header first (readHeaderOnly() below can itself fail), so
  // this always bumps alongside the general generation rather than trying
  // to skip it for a record known to be non-pending.
  g_outboxChangeGeneration++;

  StoredHeader hdr;
  bool haveHdr = readHeaderOnly(group_code, contact_key, seq, &hdr);
  char path[96];
  buildRecordPath(group_code, contact_key, seq, path, sizeof(path));
  bool removed = LittleFS.remove(path);
  dropCachedRecord(group_code, contact_key, seq);
  afterRecordRemove(group_code, contact_key, seq, removed);
  if (removed && notifyIfUnread && haveHdr && (hdr.flags & FLAG_UNREAD) && g_onEvicted != nullptr) {
    MessageRef ref;
    strncpy(ref.group_code, group_code, sizeof(ref.group_code) - 1);
    ref.group_code[sizeof(ref.group_code) - 1] = '\0';
    strncpy(ref.contact_key, contact_key, sizeof(ref.contact_key) - 1);
    ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
    ref.sequence = seq;
    g_onEvicted(ref, hdr.flags);
  }
  return removed;
}

// Walks every conversation directory under /messages, invoking `fn(group,
// contact)` for each. Shared by the global low-space scan and by
// findMessagesByPredicate's "scan everything"/"scan one group" modes.
//
// Hardware Diagnostic #4.9h follow-up: optional trailing output, defaulted
// to nullptr so the one existing caller (findGlobalOldestNonPending(), via
// the low-space eviction path) compiles and behaves exactly as before,
// unaware this parameter exists. When non-null, only ever set to false on
// a detected anomaly (sticky -- never reset back to true here, so a
// caller threading the same pointer through a whole scan gets the OR of
// every failure along the way): any failed root/group directory open, or
// a root/group path that opened but was not actually a directory, makes
// the scan unsafe to cache as empty. A genuinely absent path may
// conservatively be retried later rather than trusted as "confirmed
// empty" now. Successfully opened directories -- including ones that
// simply contain zero conversations -- remain cacheable and do not set
// this. Corrected per verified Arduino-ESP32 2.0.17 FS framework evidence
// (libraries/FS/src/vfs_api.cpp): LittleFS.exists() itself opens the path
// in read mode and returns false on that open's failure, so it cannot
// reliably distinguish "this path was never created" from "this path
// exists but couldn't be opened" -- it must NOT be used here to try to
// downgrade a failed open to a non-anomaly; every failed open is treated
// as unreliable, full stop.
template <typename Fn>
void forEachConversation(const char* onlyGroup, Fn&& fn, bool* outReliable = nullptr) {
  char groupPath[40];
  if (onlyGroup != nullptr) {
    buildGroupDir(onlyGroup, groupPath, sizeof(groupPath));
    File gdir = LittleFS.open(groupPath);
    if (!gdir || !gdir.isDirectory()) {
      if (gdir) gdir.close();
      if (outReliable != nullptr) *outReliable = false;
      return;
    }
    // Names only (readdir), as in listSequences(): opening each entry cost
    // an open per conversation, and a failed open ended the listing early
    // while still reporting it complete.
    bool isDir = false;
    String entry = gdir.getNextFileName(&isDir);
    while (entry.length() > 0) {
      if (isDir) {
        char cnameBuf[kContactKeyLen];
        strncpy(cnameBuf, baseName(entry.c_str()), sizeof(cnameBuf) - 1);
        cnameBuf[sizeof(cnameBuf) - 1] = '\0';
        fn(onlyGroup, cnameBuf);
      }
      entry = gdir.getNextFileName(&isDir);
    }
    gdir.close();
    return;
  }

  File root = LittleFS.open("/messages");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    if (outReliable != nullptr) *outReliable = false;
    return;
  }
  bool isDir = false;
  String entry = root.getNextFileName(&isDir);
  while (entry.length() > 0) {
    if (isDir) {
      char gnameBuf[33];
      strncpy(gnameBuf, baseName(entry.c_str()), sizeof(gnameBuf) - 1);
      gnameBuf[sizeof(gnameBuf) - 1] = '\0';
      forEachConversation(gnameBuf, fn, outReliable);
    }
    entry = root.getNextFileName(&isDir);
  }
  root.close();
}

bool findGlobalOldestNonPending(MessageRef* outRef) {
  bool found = false;
  uint32_t bestTs = 0;
  MessageRef bestRef{};

  forEachConversation(nullptr, [&](const char* g, const char* c) {
    static uint32_t seqs[kMaxMessagesPerThread];
    uint16_t n = listSequences(g, c, seqs, kMaxMessagesPerThread);
    for (uint16_t i = 0; i < n; i++) {
      StoredHeader hdr;
      if (readHeaderOnly(g, c, seqs[i], &hdr) && !(hdr.flags & FLAG_PENDING_OUTBOX)) {
        if (!found || hdr.effective_sort_timestamp < bestTs) {
          found = true;
          bestTs = hdr.effective_sort_timestamp;
          strncpy(bestRef.group_code, g, sizeof(bestRef.group_code) - 1);
          bestRef.group_code[sizeof(bestRef.group_code) - 1] = '\0';
          strncpy(bestRef.contact_key, c, sizeof(bestRef.contact_key) - 1);
          bestRef.contact_key[sizeof(bestRef.contact_key) - 1] = '\0';
          bestRef.sequence = seqs[i];
        }
      }
    }
  });

  if (found && outRef != nullptr) *outRef = bestRef;
  return found;
}

void ensureFreeSpaceForWrite() {
  constexpr size_t kLowWatermark = 96 * 1024;
  constexpr size_t kTargetFree = 128 * 1024;

  size_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (free >= kLowWatermark) return;

  for (int iter = 0; iter < 300; iter++) {
    free = LittleFS.totalBytes() - LittleFS.usedBytes();
    if (free >= kTargetFree) return;
    MessageRef ref;
    if (!findGlobalOldestNonPending(&ref)) return;  // nothing left to evict
    removeRecordFile(ref.group_code, ref.contact_key, ref.sequence, /*notifyIfUnread=*/true);
  }
}

// ---------------------------------------------------------------------------
// Dedup ring pool (Addendum section 7.10). A small fixed-size, LRU-managed
// pool rather than one ring per every possible conversation — Phase 2's
// message traffic pattern only needs correct dedup for conversations that
// are actively exchanging messages, and this bounds RAM to a fixed amount
// regardless of how many groups/contacts exist.
// ---------------------------------------------------------------------------
constexpr uint8_t kDedupPoolSize = 8;
constexpr uint8_t kDedupRingLen = 32;

struct DedupRing {
  bool used = false;
  char group_code[33] = {0};
  char contact_key[kContactKeyLen] = {0};
  char ids[kDedupRingLen][PacketCodec::kMessageIdLen];
  uint8_t count = 0;
  uint8_t nextSlot = 0;
  uint32_t lastTouchedMs = 0;
};
DedupRing g_dedupPool[kDedupPoolSize];

void rebuildRingFromStorage(DedupRing* ring) {
  static uint32_t seqs[kMaxMessagesPerThread];
  uint16_t n = listSequences(ring->group_code, ring->contact_key, seqs, kMaxMessagesPerThread);
  // Ascending sort, then keep only the newest kDedupRingLen.
  for (uint16_t i = 1; i < n; i++) {
    uint32_t key = seqs[i];
    int32_t j = static_cast<int32_t>(i) - 1;
    while (j >= 0 && seqs[j] > key) {
      seqs[j + 1] = seqs[j];
      j--;
    }
    seqs[j + 1] = key;
  }
  uint16_t start = (n > kDedupRingLen) ? static_cast<uint16_t>(n - kDedupRingLen) : 0;
  ring->count = 0;
  ring->nextSlot = 0;
  for (uint16_t i = start; i < n; i++) {
    char mid[PacketCodec::kMessageIdLen];
    if (readMessageIdFromFile(ring->group_code, ring->contact_key, seqs[i], mid)) {
      strncpy(ring->ids[ring->nextSlot], mid, PacketCodec::kMessageIdLen - 1);
      ring->ids[ring->nextSlot][PacketCodec::kMessageIdLen - 1] = '\0';
      ring->nextSlot = static_cast<uint8_t>((ring->nextSlot + 1) % kDedupRingLen);
      if (ring->count < kDedupRingLen) ring->count++;
    }
  }
}

DedupRing* getOrCreateRing(const char* group_code, const char* contact_key) {
  DedupRing* free_slot = nullptr;
  DedupRing* lru = nullptr;
  for (uint8_t i = 0; i < kDedupPoolSize; i++) {
    DedupRing& r = g_dedupPool[i];
    if (r.used && strcmp(r.group_code, group_code) == 0 && strcmp(r.contact_key, contact_key) == 0) {
      r.lastTouchedMs = millis();
      return &r;
    }
    if (!r.used && free_slot == nullptr) free_slot = &r;
    if (lru == nullptr || r.lastTouchedMs < lru->lastTouchedMs) lru = &r;
  }

  DedupRing* slot = (free_slot != nullptr) ? free_slot : lru;
  slot->used = true;
  strncpy(slot->group_code, group_code, sizeof(slot->group_code) - 1);
  slot->group_code[sizeof(slot->group_code) - 1] = '\0';
  strncpy(slot->contact_key, contact_key, sizeof(slot->contact_key) - 1);
  slot->contact_key[sizeof(slot->contact_key) - 1] = '\0';
  slot->lastTouchedMs = millis();
  rebuildRingFromStorage(slot);
  return slot;
}

}  // namespace

void setOnEvictedCallback(EvictedCallback cb) { g_onEvicted = cb; }

uint32_t getStorageChangeGeneration() { return g_storageChangeGeneration; }

uint32_t getOutboxChangeGeneration() { return g_outboxChangeGeneration; }

// Hardware Diagnostic #4.9k: narrow escape hatch for the one piece of code
// outside this file that mutates /messages directly on disk without going
// through atomicRewrite()/removeRecordFile() -- currently only
// Storage::removeGroupDirectoryIfPresent() (storage_init.cpp), which
// deletes a whole group's message files/directories itself. This advances
// the same g_storageChangeGeneration those two internal primitives bump,
// so any index or scan this file has cached is correctly invalidated by
// that external mutation attempt. The caller must invoke this exactly
// once, before its own destructive filesystem work begins -- the same
// unconditional-before-the-attempt rule those internal bump sites follow,
// since a deletion that fails partway through may still have removed some
// files, so a cache must not survive on the hope that the attempt either
// fully succeeded or fully no-opped. This is not a general-purpose API:
// do not call it from ordinary MessageStore-mediated code, which already
// bumps the generation itself.
// Hardware Diagnostic #4.9u: conservatively bumps the Outbox generation
// too -- an external deletion (currently only whole-group removal) could
// remove pending records, and this file has no way to know from here
// whether it did.
void notifyExternalStorageMutationAttempted() {
  bumpGenerationFor(nullptr, nullptr);  // which conversations: unknown -- every cached index goes stale
  g_outboxChangeGeneration++;
  dropAllCachedRecords();
  g_externalMutations++;
}

void setOnPendingWrittenCallback(PendingWrittenCallback cb) { g_onPendingWritten = cb; }

uint32_t getExternalMutationCount() { return g_externalMutations; }

void init() {
  ensureDirExists("/messages");
  // Dedup rings are rebuilt lazily per-conversation on first touch
  // (getOrCreateRing), which already satisfies "rebuild from newest stored
  // messages" without an eager full-tree scan at boot.
}

namespace {
// Hardware Diagnostic #4.9k: relocated here (from just above
// loadConversationIndex(), where indexEntryAfter()/readIndexMetadata()
// still remain) so appendStoredMessage() below -- Hardware Diagnostic
// #4.9q -- can read/write these same globals. Nothing about the
// declarations themselves changed, only their position in the file.
ConversationIndexEntry g_index[kMaxMessagesPerThread];
uint16_t g_indexCount = 0;

// Hardware Diagnostic #4.9k: identity + generation of the ONE conversation
// currently held in the shared g_index/g_indexCount above, if any. There
// is exactly one cache slot, matching the single shared index -- switching
// A -> B -> A means A's own entry must be reloaded, not that a second slot
// remembers it. g_indexCacheValid starts false (nothing cached yet at
// boot; this file adds no boot preload). Identity buffers are sized with
// the same existing constants MessageRef/ConversationIndexEntry callers
// already use (PacketCodec::kGroupCodeLen, kContactKeyLen) rather than a
// new literal, so they can hold any group_code/contact_key this codebase
// considers representable.
bool g_indexCacheValid = false;
char g_indexCacheGroup[PacketCodec::kGroupCodeLen] = {0};
char g_indexCacheContact[kContactKeyLen] = {0};
uint32_t g_indexCacheGeneration = 0;

// Hardware Diagnostic #4.9q: pending single-entry incremental-refresh
// candidate for the g_index above. Recorded only by appendStoredMessage()
// (see its own maybeRecordIndexAppendCandidate() call, in its success
// path only) on a fully successful append to a conversation whose cached
// index was valid and unchanged across the ENTIRE append. Consumed only
// by the NEXT loadConversationIndex() call for this EXACT identity --
// see tryApplyIndexAppendCandidate() -- which either applies it (reading
// just the new record and inserting it in place) or discards it and
// falls back to the existing full-load path. appendStoredMessage() never
// writes g_index/g_indexCount/g_indexCacheValid/g_indexCacheGroup/
// g_indexCacheContact/g_indexCacheGeneration itself -- only these scalar
// fields -- so any code currently iterating g_index via getIndexEntry(),
// or holding a count from its last loadConversationIndex() call, is
// completely unaffected by an append until it explicitly reloads. Scalars
// only: no pointer into g_index or any other shared buffer is ever stored
// here, and this is not a persistent cache -- it describes exactly one
// pending append, consumed or discarded by the very next load for this
// identity, never accumulated across more than one.
bool g_pendingCandidateValid = false;
char g_pendingCandidateGroup[PacketCodec::kGroupCodeLen] = {0};
char g_pendingCandidateContact[kContactKeyLen] = {0};
uint32_t g_pendingCandidateSequence = 0;
uint32_t g_pendingCandidatePostWriteGeneration = 0;
}  // namespace

namespace {
// The record bytes atomicRewriteFile() last built (written through to the
// record cache by atomicRewrite()).
uint8_t g_rewriteScratch[kMaxRecordFileSize];
size_t g_rewriteLen = 0;

bool atomicRewriteFile(const MessageRef& ref, const StoredHeader& header, const uint8_t* wirePacket,
                       uint16_t wirePacketLen, const uint8_t* localPayload, uint16_t localPayloadLen) {
  // Hardware Diagnostic #4.9h: the generation is bumped by atomicRewrite(),
  // unconditionally, before any of this function's own filesystem work --
  // see g_storageChangeGeneration's own comment above for why this must
  // happen even on a path that goes on to fail/return false (the
  // remove(finalPath)-then-rename(tempPath, finalPath) sequence below can
  // leave finalPath gone if the rename fails, which is itself a real change
  // to what a later scan would find, even though this function reports
  // failure).
  g_rewriteLen = 0;

  // Hardware Diagnostic #4.9u: if the header being written IS pending,
  // this attempt could be creating (or re-affirming) a pending record, so
  // bump the Outbox generation unconditionally too, before any filesystem
  // work, on every attempt -- exactly like g_storageChangeGeneration
  // above, for the same "may have touched what matters the moment it was
  // attempted" reason. If it is NOT pending, this write cannot be
  // creating a new pending record; the Outbox generation is only bumped
  // below, at whichever exit this call actually fails at -- never on the
  // success path, since a non-pending write that fully succeeds cannot
  // have changed whether any pending record exists.
  bool intendsPending = (header.flags & FLAG_PENDING_OUTBOX) != 0;
  if (intendsPending) g_outboxChangeGeneration++;

  uint8_t* scratch = g_rewriteScratch;
  size_t needed = kStoredHeaderDiskSize + 2 + wirePacketLen + 2 + localPayloadLen;
  if (needed > sizeof(g_rewriteScratch)) {
    if (!intendsPending) g_outboxChangeGeneration++;
    return false;
  }

  size_t pos = 0;
  scratch[pos++] = static_cast<uint8_t>(header.direction);
  writeU16LE(&scratch[pos], header.flags);
  pos += 2;
  writeU32LE(&scratch[pos], header.local_write_sequence);
  pos += 4;
  writeU32LE(&scratch[pos], header.effective_sort_timestamp);
  pos += 4;
  writeU16LE(&scratch[pos], wirePacketLen);
  pos += 2;
  if (wirePacketLen > 0) {
    memcpy(&scratch[pos], wirePacket, wirePacketLen);
    pos += wirePacketLen;
  }
  writeU16LE(&scratch[pos], localPayloadLen);
  pos += 2;
  if (localPayloadLen > 0) {
    memcpy(&scratch[pos], localPayload, localPayloadLen);
    pos += localPayloadLen;
  }
  g_rewriteLen = pos;

  char finalPath[96];
  buildRecordPath(ref.group_code, ref.contact_key, ref.sequence, finalPath, sizeof(finalPath));
  char tempPath[100];
  snprintf(tempPath, sizeof(tempPath), "%s.tmp", finalPath);

  File f = LittleFS.open(tempPath, "w");
  if (!f) {
    if (!intendsPending) g_outboxChangeGeneration++;
    return false;
  }
  size_t written = f.write(scratch, pos);
  f.flush();
  f.close();
  if (written != pos) {
    LittleFS.remove(tempPath);
    if (!intendsPending) g_outboxChangeGeneration++;
    return false;
  }

  LittleFS.remove(finalPath);  // overwrite semantics: clear any prior file at the target first
  // Hardware Diagnostic #4.9u: captured rather than returned directly, so
  // a non-pending write that fails here (the one remaining failure exit)
  // can still bump the Outbox generation before returning -- success
  // (renamed == true) never does. Every other operation/ordering below is
  // unchanged.
  bool renamed = LittleFS.rename(tempPath, finalPath);
  if (!intendsPending && !renamed) g_outboxChangeGeneration++;
  return renamed;
}
}  // namespace

bool atomicRewrite(const MessageRef& ref, const StoredHeader& header, const uint8_t* wirePacket,
                   uint16_t wirePacketLen, const uint8_t* localPayload, uint16_t localPayloadLen) {
  bumpGenerationFor(ref.group_code, ref.contact_key);
  bool ok = atomicRewriteFile(ref, header, wirePacket, wirePacketLen, localPayload, localPayloadLen);
  afterRecordWrite(ref, header, ok);
  // A record written pending (an offline send, a retried one): Outbox
  // queues it directly instead of scanning storage for it.
  if (ok && (header.flags & FLAG_PENDING_OUTBOX) && g_onPendingWritten != nullptr) g_onPendingWritten(ref);
  return ok;
}

namespace {
// Hardware Diagnostic #4.9q: called only from appendStoredMessage()'s
// success path, after atomicRewrite() has already returned true. Never
// mutates g_index, g_indexCount, or any g_indexCache* field -- only ever
// writes the g_pendingCandidate* scalars, so any code currently iterating
// the shared index (getIndexEntry()) or holding a count from its last
// loadConversationIndex() call is completely unaffected until it
// explicitly reloads. Records a candidate only when EVERY one of these
// holds, all evaluated against this exact append:
//   - a reliable, valid cached index for this exact group/contact existed
//     at function entry (entryIndexEligible, computed by the caller from
//     a generation snapshot taken before ensureFreeSpaceForWrite() ran);
//   - no candidate is already outstanding (never overwrite one and make
//     the old index look like it covers more than the one append that
//     created it);
//   - this append's own count phase (msgCount) agreed with the cached
//     index's own count (g_indexCount). Hardware Diagnostic #4.9r: when
//     that count phase used the RAM-resident index itself (countReused)
//     rather than a real directory enumeration, msgCount IS g_indexCount
//     by construction -- this check is then a tautology, not a second
//     independent reading, and it is trivially satisfied precisely
//     because the RAM path itself already required the pre-append index
//     to be valid for this exact identity and generation (see
//     appendStoredMessage()'s own count-phase comment). When the
//     fallback countMessages() path ran instead, msgCount remains a
//     genuinely independent filesystem reading compared against
//     g_indexCount, exactly as before #4.9r;
//   - this append's own count phase did not report a directory-open
//     failure (countDirOpenFailed) -- always false when the RAM path was
//     used, since no open was attempted there to fail;
//   - #4.9p's maximum-sequence reuse path was actually used, which by
//     construction already means no per-thread eviction was attempted;
//   - the cached index has room for one more entry;
//   - the new sequence is not already present in the cached index;
//   - exactly one generation advance happened across the ENTIRE append,
//     from entry to just after atomicRewrite() succeeded -- computed with
//     unsigned (rollover-safe) subtraction, so this single check also
//     catches low-space eviction, per-thread eviction, and any other
//     mutation or callback that could have touched storage or the shared
//     index along the way;
//   - a final recheck, read fresh (not reused from the entry snapshot),
//     that the cache is STILL valid for this exact identity right now --
//     never revives a candidate against an index that had already gone
//     invalid by the time this runs.
// A second append before the first candidate is consumed can never
// overwrite it with a mismatched one: g_indexCacheGeneration is only ever
// advanced by loadConversationIndex() consuming or discarding a
// candidate, so a second append's own entryIndexEligible can only be true
// if the first candidate was already consumed or discarded by an
// intervening load -- never while one is still outstanding for this
// identity. The explicit g_pendingCandidateValid check below is kept
// anyway as the same kind of conservative, defense-in-depth guard this
// diagnostic series has used throughout (e.g. #4.9p's own generation
// check), not because this invariant was found to be otherwise breakable.
void maybeRecordIndexAppendCandidate(const char* group_code, const char* contact_key, bool entryIndexEligible,
                                     uint32_t entryGeneration, bool countDirOpenFailed, uint16_t msgCount,
                                     bool maxSeqReused, uint32_t newSequence) {
  if (!entryIndexEligible || g_pendingCandidateValid) return;
  if (countDirOpenFailed || !maxSeqReused) return;
  if (msgCount != g_indexCount) return;
  if (g_indexCount >= kMaxMessagesPerThread) return;
  for (uint16_t i = 0; i < g_indexCount; i++) {
    if (g_index[i].sequence == newSequence) return;
  }
  uint32_t generationNow = getStorageChangeGeneration();
  if (generationNow - entryGeneration != 1u) return;  // unsigned, rollover-safe
  // Recheck fresh, right before writing the candidate: never revive an
  // index that has since gone invalid, and never trust the entry-time
  // snapshot alone for the final write.
  if (!g_indexCacheValid || strcmp(g_indexCacheGroup, group_code) != 0 ||
      strcmp(g_indexCacheContact, contact_key) != 0) {
    return;
  }
  strncpy(g_pendingCandidateGroup, group_code, sizeof(g_pendingCandidateGroup) - 1);
  g_pendingCandidateGroup[sizeof(g_pendingCandidateGroup) - 1] = '\0';
  strncpy(g_pendingCandidateContact, contact_key, sizeof(g_pendingCandidateContact) - 1);
  g_pendingCandidateContact[sizeof(g_pendingCandidateContact) - 1] = '\0';
  g_pendingCandidateSequence = newSequence;
  g_pendingCandidatePostWriteGeneration = generationNow;
  g_pendingCandidateValid = true;
}
}  // namespace

// Hardware Diagnostic #4.9o: instrumentation only -- every existing call,
// its arguments, its order, and every existing return/failure path below
// are unchanged. countMessages()'s return value is now captured into a
// local (msgCount) instead of being called inline inside the eviction
// `if`, so it can also be timed -- this is the SAME single call the
// original code made, not an additional one. evictOldest()'s and
// atomicRewrite()'s existing return values are likewise captured for this
// diagnostic's own log line, without changing what either failure path
// does (both still return false exactly as before). No nested timing was
// added inside ensureFreeSpaceForWrite(), countMessages(), evictOldest(),
// findMaxSequence(), or atomicRewrite() themselves -- each is timed only
// from this call site, as one disjoint (never overlapping, never re-run)
// phase in this function's own linear sequence. kUnmeasuredMs marks a
// phase never reached this call (dirs/maxSeq/prepare/rewrite on the
// evict-fail exit) or never attempted (evict, whenever eviction wasn't
// needed this call) -- never a real 0 ms measurement.
//
// This function is called from MessageStore::appendStoredMessage()'s own
// caller chain, itself invoked from sendComposedMessage()'s STORE phase
// (text_message.cpp, Hardware Diagnostic #4.9n) during CHAT_SEND, itself
// nested inside CHAT_SCREEN's own input-processing window (text_message.cpp,
// #4.9i/#4.9l). STORE_APPEND's own total is therefore nested inside both
// CHAT_SEND.store and CHAT_SCREEN's total -- never added on top of either.
//
// Hardware Diagnostic #4.9p: avoids the second directory enumeration
// findMaxSequence() used to perform immediately after this function's own
// countMessages() call, by reusing countMessages()'s own maximum-sequence
// output when it is safe to. Reuse requires ALL of:
//   - countMessages()'s own listSequences() call opened this exact
//     conversation directory successfully (countDirOpenFailed == false);
//   - no per-thread eviction was attempted this call (msgCount was below
//     kMaxMessagesPerThread, i.e. the eviction `if` above was never
//     entered -- the eviction path always falls back to a fresh
//     findMaxSequence() scan below, since evictOldest() can change which
//     sequence is now oldest/newest and this function does not attempt to
//     track that);
//   - g_storageChangeGeneration is unchanged between the generation
//     snapshot taken immediately before countMessages()'s enumeration and
//     the reuse decision below.
// Audit (source-only, this commit): countMessages() and findMaxSequence()
// are both file-local (anonymous-namespace) helpers with exactly one call
// site each, both inside this function, so no other caller's behavior is
// affected by extending countMessages()'s signature. Nothing in this
// function or its callees (ensureFreeSpaceForWrite(), evictOldest(),
// ensureDirExists(), the directory-path builders) re-enters
// appendStoredMessage() or invokes any registered callback capable of
// mutating THIS conversation's directory between the generation snapshot
// and the reuse decision -- g_onEvicted, the only callback in this file,
// only fires from removeRecordFile(), which only runs here via the
// per-thread eviction path already excluded above, or earlier during
// ensureFreeSpaceForWrite() (before the generation snapshot is even
// taken). The generation check is kept anyway as the conservative guard
// this task calls for, not because a live reentrant caller was found.
// This establishes no persistent cache -- msgCount, maxSeqFromCount,
// countDirOpenFailed and generationBeforeCount are all locals scoped to
// one call -- and does not claim the underlying FS API can detect a
// directory listing interrupted partway through by an error; see
// listSequences()'s own residual-limitation comment above. A
// missing/unopenable directory instead always falls back to the original
// post-directory-creation findMaxSequence() scan below, unchanged from
// #4.9o.
//
// Hardware Diagnostic #4.9r: the count phase below now tries the
// RAM-resident conversation index (g_index) before calling countMessages()
// at all -- see the count phase's own comment for the exact eligibility
// list and countReused's meaning. countMessages() (and therefore a real
// directory enumeration) only runs when that RAM path is ineligible or
// its own post-scan generation recheck fails. Everything downstream
// (the eviction check, the #4.9p maxSeq-reuse decision, and
// maybeRecordIndexAppendCandidate()'s own checks) consumes msgCount/
// maxSeqFromCount/countDirOpenFailed exactly as before, uniformly,
// whichever source produced them -- unchanged.
bool appendStoredMessage(const char* group_code, const char* contact_key, Direction direction, uint16_t flags,
                         uint32_t effective_sort_timestamp, const uint8_t* wirePacket, uint16_t wirePacketLen,
                         const uint8_t* localPayload, uint16_t localPayloadLen, MessageRef* outRef) {
  constexpr uint32_t kUnmeasuredMs = 0xFFFFFFFFu;
  uint32_t totalStart = millis();
  bool maxSeqReused = false;
  // Hardware Diagnostic #4.9r: initialized false before any exit, same as
  // maxSeqReused above -- set true only when the count phase below
  // actually used the RAM-resident g_index instead of a real directory
  // enumeration (countMessages()).
  bool countReused = false;

  // Hardware Diagnostic #4.9q: snapshot taken before ensureFreeSpaceForWrite()
  // runs, so entryIndexEligible reflects the TRUE pre-append state -- low-
  // space eviction below (if any) must not be mistaken for "nothing
  // happened yet." See maybeRecordIndexAppendCandidate()'s own comment,
  // just above this function, for the full eligibility list this feeds.
  uint32_t entryGeneration = getStorageChangeGeneration();
  bool entryIndexEligible = g_indexCacheValid && strcmp(g_indexCacheGroup, group_code) == 0 &&
                            strcmp(g_indexCacheContact, contact_key) == 0 &&
                            g_indexCacheGeneration == entryGeneration;

  uint32_t freeSpaceStart = millis();
  ensureFreeSpaceForWrite();
  uint32_t freeSpaceElapsed = millis() - freeSpaceStart;

  uint32_t countStart = millis();
  uint32_t generationBeforeCount = getStorageChangeGeneration();
  uint32_t maxSeqFromCount = 0;
  bool countDirOpenFailed = false;
  uint16_t msgCount = 0;
  // Hardware Diagnostic #4.9r: try the RAM-resident conversation index
  // before falling back to a real directory enumeration. Eligible only
  // when ALL of: a valid cached index exists for this EXACT identity; its
  // cached generation still equals the CURRENT generation, read fresh
  // here (not the entry-time snapshot above -- ensureFreeSpaceForWrite()
  // just ran, and a low-space eviction there would already have bumped
  // the generation past what the cache reflects, which must invalidate
  // this just like any other intervening mutation); no #4.9q candidate is
  // currently outstanding for it; and it has room for one more entry.
  // Reliability here comes entirely from g_indexCacheValid having already
  // been established by a prior loadConversationIndex() (a full load or a
  // #4.9q incremental apply) -- this is NOT a fresh directory open or an
  // independent enumeration, so countDirOpenFailed is simply left false
  // (nothing was attempted here that could fail) rather than claimed as a
  // verified open; its existing meaning for the fallback path below is
  // unchanged.
  if (g_indexCacheValid && strcmp(g_indexCacheGroup, group_code) == 0 &&
      strcmp(g_indexCacheContact, contact_key) == 0 && g_indexCacheGeneration == generationBeforeCount &&
      !g_pendingCandidateValid && g_indexCount < kMaxMessagesPerThread) {
    // The index is sorted by (effective_sort_timestamp, message_id), NOT
    // by sequence -- the last entry is not necessarily the highest
    // sequence, so every entry's sequence field must be scanned, with the
    // same comparison and initial value (0) findMaxSequence() uses.
    uint32_t ramMaxSeq = 0;
    for (uint16_t i = 0; i < g_indexCount; i++) {
      if (g_index[i].sequence > ramMaxSeq) ramMaxSeq = g_index[i].sequence;
    }
    // Recheck before accepting the RAM result: this scan only ever reads
    // g_index, so nothing here can itself have mutated storage, but this
    // still guards against a hypothetically reentrant mutation -- the
    // same conservative style this diagnostic series has used throughout.
    if (getStorageChangeGeneration() == generationBeforeCount) {
      msgCount = g_indexCount;
      maxSeqFromCount = ramMaxSeq;
      countReused = true;
    }
  }
  if (!countReused) {
    msgCount = countMessages(group_code, contact_key, &maxSeqFromCount, &countDirOpenFailed);
  }
  uint32_t countElapsed = millis() - countStart;

  uint32_t evictElapsed = kUnmeasuredMs;
  // Approved retention: kRetainedMessagesPerThread. One record per append at
  // most; a longer (older firmware's) history is trimmed in the background.
  if (msgCount >= kRetainedMessagesPerThread) {
    uint32_t evictStart = millis();
    bool evictOk = evictOldest(group_code, contact_key);
    evictElapsed = millis() - evictStart;
    if (!evictOk) {  // Storage Full
      uint32_t totalElapsed = millis() - totalStart;
      Serial.printf(
          "[PERF][STORE_APPEND] total=%lu ms exit=evict-fail freeSpace=%lu ms count=%lu ms evict=%lu ms "
          "dirs=%lu ms maxSeq=%lu ms prepare=%lu ms rewrite=%lu ms maxSeqReused=%d countReused=%d\n",
          static_cast<unsigned long>(totalElapsed), static_cast<unsigned long>(freeSpaceElapsed),
          static_cast<unsigned long>(countElapsed), static_cast<unsigned long>(evictElapsed),
          static_cast<unsigned long>(kUnmeasuredMs), static_cast<unsigned long>(kUnmeasuredMs),
          static_cast<unsigned long>(kUnmeasuredMs), static_cast<unsigned long>(kUnmeasuredMs),
          maxSeqReused ? 1 : 0, countReused ? 1 : 0);
      return false;
    }
  }

  uint32_t dirsStart = millis();
  ensureDirExists("/messages");
  char groupDir[40];
  buildGroupDir(group_code, groupDir, sizeof(groupDir));
  ensureDirExists(groupDir);
  char convDir[80];
  buildConversationDir(group_code, contact_key, convDir, sizeof(convDir));
  ensureDirExists(convDir);
  uint32_t dirsElapsed = millis() - dirsStart;

  uint32_t maxSeqStart = millis();
  uint32_t nextSeq;
  if (!countDirOpenFailed && msgCount < kMaxMessagesPerThread &&
      getStorageChangeGeneration() == generationBeforeCount) {
    nextSeq = maxSeqFromCount + 1;
    maxSeqReused = true;
  } else {
    nextSeq = findMaxSequence(group_code, contact_key) + 1;
  }
  uint32_t maxSeqElapsed = millis() - maxSeqStart;

  uint32_t prepareStart = millis();
  MessageRef ref;
  strncpy(ref.group_code, group_code, sizeof(ref.group_code) - 1);
  ref.group_code[sizeof(ref.group_code) - 1] = '\0';
  strncpy(ref.contact_key, contact_key, sizeof(ref.contact_key) - 1);
  ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
  ref.sequence = nextSeq;

  StoredHeader hdr;
  hdr.direction = direction;
  hdr.flags = flags;
  hdr.local_write_sequence = nextSeq;
  hdr.effective_sort_timestamp = effective_sort_timestamp;
  uint32_t prepareElapsed = millis() - prepareStart;

  uint32_t rewriteStart = millis();
  bool rewriteOk = atomicRewrite(ref, hdr, wirePacket, wirePacketLen, localPayload, localPayloadLen);
  uint32_t rewriteElapsed = millis() - rewriteStart;
  if (!rewriteOk) {
    uint32_t totalElapsed = millis() - totalStart;
    Serial.printf(
        "[PERF][STORE_APPEND] total=%lu ms exit=rewrite-fail freeSpace=%lu ms count=%lu ms evict=%lu ms "
        "dirs=%lu ms maxSeq=%lu ms prepare=%lu ms rewrite=%lu ms maxSeqReused=%d countReused=%d\n",
        static_cast<unsigned long>(totalElapsed), static_cast<unsigned long>(freeSpaceElapsed),
        static_cast<unsigned long>(countElapsed), static_cast<unsigned long>(evictElapsed),
        static_cast<unsigned long>(dirsElapsed), static_cast<unsigned long>(maxSeqElapsed),
        static_cast<unsigned long>(prepareElapsed), static_cast<unsigned long>(rewriteElapsed),
        maxSeqReused ? 1 : 0, countReused ? 1 : 0);
    return false;
  }
  if (outRef != nullptr) *outRef = ref;

  // Hardware Diagnostic #4.9q: only ever reached after atomicRewrite()
  // has already returned true -- an evict-fail or rewrite-fail return
  // above never reaches this call, so a failed append can never create a
  // candidate. See maybeRecordIndexAppendCandidate()'s own comment for
  // the full eligibility list it checks before recording anything.
  maybeRecordIndexAppendCandidate(group_code, contact_key, entryIndexEligible, entryGeneration, countDirOpenFailed,
                                  msgCount, maxSeqReused, nextSeq);

  uint32_t totalElapsed = millis() - totalStart;
  if (totalElapsed >= 20) {
    Serial.printf(
        "[PERF][STORE_APPEND] total=%lu ms exit=completion freeSpace=%lu ms count=%lu ms evict=%lu ms "
        "dirs=%lu ms maxSeq=%lu ms prepare=%lu ms rewrite=%lu ms maxSeqReused=%d countReused=%d\n",
        static_cast<unsigned long>(totalElapsed), static_cast<unsigned long>(freeSpaceElapsed),
        static_cast<unsigned long>(countElapsed), static_cast<unsigned long>(evictElapsed),
        static_cast<unsigned long>(dirsElapsed), static_cast<unsigned long>(maxSeqElapsed),
        static_cast<unsigned long>(prepareElapsed), static_cast<unsigned long>(rewriteElapsed),
        maxSeqReused ? 1 : 0, countReused ? 1 : 0);
  }
  return true;
}

namespace {
uint8_t g_loadBuffer[kMaxRecordFileSize];
}  // namespace

namespace {
// Decodes the record held in g_loadBuffer[0, len) into outView.
bool parseRecord(const MessageRef& ref, size_t len, StoredMessageView* outView) {
  if (len < kStoredHeaderDiskSize + 2 + 2) return false;
  size_t pos = 0;
  StoredHeader hdr;
  hdr.direction = static_cast<Direction>(g_loadBuffer[pos]);
  pos += 1;
  hdr.flags = readU16LE(&g_loadBuffer[pos]);
  pos += 2;
  hdr.local_write_sequence = readU32LE(&g_loadBuffer[pos]);
  pos += 4;
  hdr.effective_sort_timestamp = readU32LE(&g_loadBuffer[pos]);
  pos += 4;

  if (pos + 2 > len) return false;
  uint16_t wireLen = readU16LE(&g_loadBuffer[pos]);
  pos += 2;
  if (pos + wireLen + 2 > len) return false;
  const uint8_t* wirePtr = &g_loadBuffer[pos];
  pos += wireLen;
  uint16_t localLen = readU16LE(&g_loadBuffer[pos]);
  pos += 2;
  if (pos + localLen > len) return false;
  const uint8_t* localPtr = &g_loadBuffer[pos];

  PacketCodec::Header pkHeader;
  const uint8_t* pkPayload = nullptr;
  if (!PacketCodec::decodeHeader(wirePtr, wireLen, &pkHeader, &pkPayload)) return false;

  PacketCodec::MessageEnvelope env;
  const uint8_t* typePayload = nullptr;
  uint16_t typePayloadLen = 0;
  if (!PacketCodec::decodeMessageEnvelope(pkPayload, pkHeader.payload_length, &env, &typePayload, &typePayloadLen)) {
    return false;
  }

  outView->ref = ref;
  outView->header = hdr;
  outView->envelope = env;
  outView->wirePacket = wirePtr;
  outView->wirePacketLen = wireLen;
  outView->typePayload = typePayload;
  outView->typePayloadLen = typePayloadLen;
  outView->localPayload = (localLen > 0) ? localPtr : nullptr;
  outView->localPayloadLen = localLen;
  return true;
}

// Reads the record into g_loadBuffer: from the record cache, else one open.
bool readRecord(const MessageRef& ref, size_t* outLen, bool* outFromCache) {
  *outFromCache = false;
  const uint8_t* cached = nullptr;
  uint16_t cachedLen = 0;
  if (cachedRecordBytes(ref.group_code, ref.contact_key, ref.sequence, &cached, &cachedLen) &&
      cachedLen <= sizeof(g_loadBuffer)) {
    memcpy(g_loadBuffer, cached, cachedLen);  // a copy: the view never points into a cache slot
    *outLen = cachedLen;
    *outFromCache = true;
    return true;
  }
  char path[96];
  buildRecordPath(ref.group_code, ref.contact_key, ref.sequence, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t len = f.size();
  if (len > sizeof(g_loadBuffer) || len < kStoredHeaderDiskSize + 2 + 2) {
    f.close();
    return false;
  }
  size_t readLen = f.read(g_loadBuffer, len);
  f.close();
  if (readLen != len) return false;
  *outLen = len;
  return true;
}

void keepCachedRecord(const MessageRef& ref, const uint8_t* bytes, size_t len);
}  // namespace

bool loadMessage(const MessageRef& ref, StoredMessageView* outView) {
  if (outView == nullptr) return false;
  size_t len = 0;
  bool fromCache = false;
  if (!readRecord(ref, &len, &fromCache)) return false;
  return parseRecord(ref, len, outView);
}

bool loadMessageForDisplay(const MessageRef& ref, StoredMessageView* outView) {
  if (outView == nullptr) return false;
  size_t len = 0;
  bool fromCache = false;
  if (!readRecord(ref, &len, &fromCache)) return false;
  if (!parseRecord(ref, len, outView)) return false;
  if (!fromCache) keepCachedRecord(ref, g_loadBuffer, len);
  return true;
}

bool readMessageHeader(const MessageRef& ref, StoredHeader* outHeader) {
  if (outHeader == nullptr) return false;
  return readHeaderOnly(ref.group_code, ref.contact_key, ref.sequence, outHeader);
}

namespace {
// Hardware Diagnostic #4.9q: g_index/g_indexCount and the g_indexCache*/
// g_pendingCandidate* globals these functions use now live earlier in
// this file (just above atomicRewrite()), so appendStoredMessage() can
// also reach them -- see that relocated block's own comment for why nothing
// about their behavior changed, only their declaration point.
bool indexEntryAfter(const ConversationIndexEntry& a, const ConversationIndexEntry& b) {
  if (a.effective_sort_timestamp != b.effective_sort_timestamp) {
    return a.effective_sort_timestamp > b.effective_sort_timestamp;
  }
  return strcmp(a.message_id, b.message_id) > 0;
}

// Hardware Diagnostic #4.9j: loadConversationIndex() previously called
// readHeaderOnly() and readMessageIdFromFile() back to back for the same
// record, which opens, reads, and closes the same file twice (hardware
// evidence: read=2072 ms across 26 records). This helper reads both
// pieces of metadata through a single LittleFS.open() instead -- same
// path built with the existing buildRecordPath(), same header byte
// layout/offsets and same explicit little-endian decoding as
// readHeaderOnly() (no direct struct read), same absolute
// kMessageIdOffsetInFile seek and same trailing NUL termination as
// readMessageIdFromFile(). It invents no new offsets, disk layout, packet
// decoding, or validation rules -- it only reuses the two existing
// helpers' own logic against one open file handle. readHeaderOnly() and
// readMessageIdFromFile() themselves are untouched and still used exactly
// as before by every other caller (eviction, dedup, findMessageById()).
bool readIndexMetadata(const char* group_code, const char* contact_key, uint32_t seq, StoredHeader* outHdr,
                       char outId[PacketCodec::kMessageIdLen]) {
  const uint8_t* cached = nullptr;
  uint16_t cachedLen = 0;
  if (cachedRecordBytes(group_code, contact_key, seq, &cached, &cachedLen) &&
      cachedLen >= kMessageIdOffsetInFile + PacketCodec::kMessageIdLen) {
    decodeStoredHeader(cached, outHdr);
    memcpy(outId, cached + kMessageIdOffsetInFile, PacketCodec::kMessageIdLen);
    outId[PacketCodec::kMessageIdLen - 1] = '\0';
    return true;
  }
  char path[96];
  buildRecordPath(group_code, contact_key, seq, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return false;

  uint8_t hdrBuf[kStoredHeaderDiskSize];
  size_t hdrRead = f.read(hdrBuf, sizeof(hdrBuf));
  if (hdrRead != sizeof(hdrBuf)) {
    f.close();
    return false;
  }

  bool seekOk = f.seek(kMessageIdOffsetInFile);
  char idBuf[PacketCodec::kMessageIdLen];
  size_t idRead = 0;
  if (seekOk) idRead = f.read(reinterpret_cast<uint8_t*>(idBuf), PacketCodec::kMessageIdLen);
  f.close();
  if (!seekOk || idRead != PacketCodec::kMessageIdLen) return false;
  idBuf[PacketCodec::kMessageIdLen - 1] = '\0';

  // Both reads succeeded -- only now write the caller's output.
  outHdr->direction = static_cast<Direction>(hdrBuf[0]);
  outHdr->flags = readU16LE(&hdrBuf[1]);
  outHdr->local_write_sequence = readU32LE(&hdrBuf[3]);
  outHdr->effective_sort_timestamp = readU32LE(&hdrBuf[7]);
  memcpy(outId, idBuf, PacketCodec::kMessageIdLen);
  return true;
}

// Hardware Diagnostic #4.9q: attempts to apply the single-entry pending
// index-append candidate recorded by appendStoredMessage() (see
// maybeRecordIndexAppendCandidate()'s own comment, just above
// appendStoredMessage() earlier in this file, for exactly when and why a
// candidate is recorded). Called by loadConversationIndex() only after it
// has already established g_pendingCandidateValid is true and the
// candidate's identity matches group_code/contact_key. Returns true only
// if g_index/g_indexCount were actually updated in place; a false return
// always means this function has already cleared g_pendingCandidateValid
// itself, so the caller's only remaining job is to fall back to the
// existing full-load path -- it must never retry the same candidate, and
// must not treat a false return as having left a partially modified
// index (it never mutates g_index/g_indexCount until every prerequisite
// below has already passed).
bool tryApplyIndexAppendCandidate(const char* group_code, const char* contact_key) {
  // Recheck fresh here, rather than trusting the append-time snapshot
  // that originally made this candidate eligible -- never revive a
  // cached index that has since gone invalid or now belongs to a
  // different identity.
  if (!g_indexCacheValid || strcmp(g_indexCacheGroup, group_code) != 0 ||
      strcmp(g_indexCacheContact, contact_key) != 0) {
    g_pendingCandidateValid = false;
    return false;
  }
  if (g_indexCount >= kMaxMessagesPerThread) {
    g_pendingCandidateValid = false;
    return false;
  }
  for (uint16_t i = 0; i < g_indexCount; i++) {
    if (g_index[i].sequence == g_pendingCandidateSequence) {
      g_pendingCandidateValid = false;
      return false;
    }
  }
  // The one generation this candidate is valid for -- appendStoredMessage()
  // only ever recorded it when this equaled the generation immediately
  // after its own atomicRewrite() succeeded, with nothing else advancing
  // the generation in between. Any further mutation since then (a second
  // append, an eviction, an external notifyExternalStorageMutationAttempted())
  // moves the generation past this value, which this check catches.
  if (getStorageChangeGeneration() != g_pendingCandidatePostWriteGeneration) {
    g_pendingCandidateValid = false;
    return false;
  }

  StoredHeader hdr;
  char mid[PacketCodec::kMessageIdLen];
  bool readOk = readIndexMetadata(group_code, contact_key, g_pendingCandidateSequence, &hdr, mid);
  // Stable-generation recheck after the read, mirroring loadConversationIndex()'s
  // own before/after comparison for a full load -- the read above must not
  // have raced a mutation landing after the prerequisite check just above.
  if (!readOk || getStorageChangeGeneration() != g_pendingCandidatePostWriteGeneration) {
    g_pendingCandidateValid = false;
    return false;
  }

  ConversationIndexEntry newEntry;
  newEntry.sequence = g_pendingCandidateSequence;
  newEntry.effective_sort_timestamp = hdr.effective_sort_timestamp;
  strncpy(newEntry.message_id, mid, PacketCodec::kMessageIdLen - 1);
  newEntry.message_id[PacketCodec::kMessageIdLen - 1] = '\0';
  newEntry.flags = hdr.flags;

  // Find the insertion point in the existing ascending-sorted g_index,
  // exactly as the full-load insertion sort would place this entry.
  uint16_t insertPos = g_indexCount;
  for (uint16_t i = 0; i < g_indexCount; i++) {
    bool newAfterExisting = indexEntryAfter(newEntry, g_index[i]);
    bool existingAfterNew = indexEntryAfter(g_index[i], newEntry);
    if (!newAfterExisting && !existingAfterNew) {
      // Equal on both sort keys -- a real full load's relative order for
      // tied entries depends on filesystem enumeration order, which this
      // incremental path never reproduces. Conservatively refuse to
      // invent a tie order rather than risk a wrong one.
      g_pendingCandidateValid = false;
      return false;
    }
    if (existingAfterNew) {
      insertPos = i;
      break;
    }
  }

  for (uint16_t i = g_indexCount; i > insertPos; i--) {
    g_index[i] = g_index[i - 1];
  }
  g_index[insertPos] = newEntry;
  g_indexCount++;

  g_indexCacheGeneration = g_pendingCandidatePostWriteGeneration;
  g_pendingCandidateValid = false;
  return true;
}
}  // namespace

// ---------------------------------------------------------------------------
// Chat-entry latency (device A on 2ed04b6, 70 broadcast messages:
// "[PERF][CHAT_INDEX] list=158 ms read=4446 ms", one LittleFS open per
// record at about 63 ms each; a cached re-entry still spent about 864 ms
// reading the visible rows twice). The work is removed, not only spread:
//  - a record cache: the rows a chat displayed, kept byte-exact and written
//    through on every store write, so the render and later passes read them
//    without opening a file;
//  - the shared index is kept current across mutations: its own
//    conversation's writes and removals are patched in place, and those of
//    other conversations leave it valid (bumpGenerationFor());
//  - a parked second index keeps the last chat, so going back to it reads
//    nothing;
//  - the Notifications boot sweep's recount, which reads every record
//    anyway, feeds the index (beginIndexFeed()/feedIndexRecord());
//  - what is still loaded is loaded in bounded, time-budgeted steps
//    (stepConversationIndex()), so the chat stays responsive meanwhile.
// All of it is RAM only and rebuilt from the records: nothing new on flash.
// ---------------------------------------------------------------------------
namespace {

// ---- the record cache ------------------------------------------------------
constexpr uint8_t kRecordCacheSlots = 10;
struct CachedRecord {
  bool valid;
  char group[PacketCodec::kGroupCodeLen];
  char contact[kContactKeyLen];
  uint32_t seq;
  uint16_t len, cap;
  uint32_t lastUse;
  uint8_t* bytes;  // heap, exactly the record file's bytes
};
// The slot table is on the heap too (static DRAM has no room for it),
// allocated by the first record kept.
CachedRecord* g_recordCache = nullptr;
uint32_t g_recordCacheClock = 0;

CachedRecord* findCachedRecord(const char* group_code, const char* contact_key, uint32_t seq) {
  if (g_recordCache == nullptr) return nullptr;
  for (uint8_t i = 0; i < kRecordCacheSlots; i++) {
    CachedRecord& c = g_recordCache[i];
    if (c.valid && c.seq == seq && strcmp(c.contact, contact_key) == 0 && strcmp(c.group, group_code) == 0) return &c;
  }
  return nullptr;
}

// Stores len bytes in slot c (reallocating if needed). False if no memory
// (the slot is then dropped).
bool fillCachedRecord(CachedRecord& c, const uint8_t* bytes, size_t len) {
  if (len > c.cap || c.bytes == nullptr) {
    delete[] c.bytes;
    c.bytes = new (std::nothrow) uint8_t[len];
    c.cap = c.bytes != nullptr ? static_cast<uint16_t>(len) : 0;
    if (c.bytes == nullptr) {
      c.valid = false;
      return false;
    }
  }
  memcpy(c.bytes, bytes, len);
  c.len = static_cast<uint16_t>(len);
  c.lastUse = ++g_recordCacheClock;
  return true;
}

void keepCachedRecord(const MessageRef& ref, const uint8_t* bytes, size_t len) {
  if (len == 0 || len > kMaxRecordFileSize) return;
  if (strlen(ref.group_code) >= PacketCodec::kGroupCodeLen || strlen(ref.contact_key) >= kContactKeyLen) return;
  if (g_recordCache == nullptr) {
    g_recordCache = new (std::nothrow) CachedRecord[kRecordCacheSlots]();
    if (g_recordCache == nullptr) return;  // no memory: not cached (read from flash again)
  }
  CachedRecord* slot = findCachedRecord(ref.group_code, ref.contact_key, ref.sequence);
  if (slot == nullptr) {
    for (uint8_t i = 0; i < kRecordCacheSlots; i++) {
      CachedRecord& c = g_recordCache[i];
      if (!c.valid) {
        slot = &c;
        break;
      }
      if (slot == nullptr || c.lastUse < slot->lastUse) slot = &c;
    }
    slot->valid = false;
    strncpy(slot->group, ref.group_code, sizeof(slot->group) - 1);
    slot->group[sizeof(slot->group) - 1] = '\0';
    strncpy(slot->contact, ref.contact_key, sizeof(slot->contact) - 1);
    slot->contact[sizeof(slot->contact) - 1] = '\0';
    slot->seq = ref.sequence;
  }
  if (fillCachedRecord(*slot, bytes, len)) slot->valid = true;
}

// A store write: what the cache holds for this record becomes the bytes just
// written (or is dropped if the write failed -- the file may be gone).
void writeThroughCachedRecord(const MessageRef& ref, bool ok) {
  CachedRecord* c = findCachedRecord(ref.group_code, ref.contact_key, ref.sequence);
  if (c == nullptr) return;
  if (!ok || g_rewriteLen == 0 || !fillCachedRecord(*c, g_rewriteScratch, g_rewriteLen)) c->valid = false;
}

}  // namespace

namespace {
void dropCachedRecord(const char* group_code, const char* contact_key, uint32_t seq) {
  CachedRecord* c = findCachedRecord(group_code, contact_key, seq);
  if (c != nullptr) c->valid = false;
}

void dropAllCachedRecords() {
  if (g_recordCache == nullptr) return;
  for (uint8_t i = 0; i < kRecordCacheSlots; i++) g_recordCache[i].valid = false;
}

bool cachedRecordBytes(const char* group_code, const char* contact_key, uint32_t seq, const uint8_t** bytes,
                       uint16_t* len) {
  CachedRecord* c = findCachedRecord(group_code, contact_key, seq);
  if (c == nullptr) return false;
  c->lastUse = ++g_recordCacheClock;
  *bytes = c->bytes;
  *len = c->len;
  return true;
}

// ---- sorted index arrays -----------------------------------------------------
// Inserts e in sorted position, after every entry it does not sort before
// (the stable order the full load's insertion sort gives). Removes an entry
// of the same sequence first. False if the array is full.
bool patchIndexWrite(ConversationIndexEntry* arr, uint16_t* count, uint16_t cap, const ConversationIndexEntry& e) {
  for (uint16_t i = 0; i < *count; i++) {
    if (arr[i].sequence == e.sequence) {
      memmove(&arr[i], &arr[i + 1], sizeof(ConversationIndexEntry) * (*count - i - 1));
      (*count)--;
      break;
    }
  }
  if (*count >= cap) return false;
  uint16_t at = *count;
  for (uint16_t i = 0; i < *count; i++) {
    if (indexEntryAfter(arr[i], e)) {
      at = i;
      break;
    }
  }
  memmove(&arr[at + 1], &arr[at], sizeof(ConversationIndexEntry) * (*count - at));
  arr[at] = e;
  (*count)++;
  return true;
}

void patchIndexRemove(ConversationIndexEntry* arr, uint16_t* count, uint32_t seq) {
  for (uint16_t i = 0; i < *count; i++) {
    if (arr[i].sequence == seq) {
      memmove(&arr[i], &arr[i + 1], sizeof(ConversationIndexEntry) * (*count - i - 1));
      (*count)--;
      return;
    }
  }
}

// ---- the stepped load into the shared index ----------------------------------
// g_indexCacheGroup/g_indexCacheContact name the conversation g_index holds:
// complete and current when g_indexCacheValid, or being loaded (g_loader).
constexpr uint32_t kIndexStepBudgetMs = 20;  // about one open per pass on device A
constexpr uint32_t kChatLoadingMs = 200;     // a chat steps its load every pass; older steps mean nobody is waiting
struct IndexLoader {
  bool active;
  bool listed;
  bool reliable;
  bool fed;  // the records arrive from the Notifications recount (feedIndexRecord()), not this loader's reads
  uint16_t n, pos, steps;
  uint32_t generation;  // the storage generation the entries so far are current for
  uint32_t startMs, listMs, readMs, lastStepMs;
};
IndexLoader g_loader = {};
uint32_t g_loaderSeqs[kMaxMessagesPerThread];

bool isIndexTarget(const char* group_code, const char* contact_key) {
  return (g_indexCacheValid || g_loader.active) && strcmp(g_indexCacheGroup, group_code) == 0 &&
         strcmp(g_indexCacheContact, contact_key) == 0;
}

// ---- the parked index: the last chat's index, kept while another is open ----
constexpr uint16_t kParkedCapacity = kRetainedMessagesPerThread;
// While the shared index is held by a chat, the boot sweep's recount feeds
// the parked one instead (feeding: not usable until complete).
struct ParkedIndex {
  bool valid;
  char group[PacketCodec::kGroupCodeLen];
  char contact[kContactKeyLen];
  uint32_t generation;
  uint16_t count;
  ConversationIndexEntry entries[kParkedCapacity];
  bool feeding, feedReliable;
  uint16_t feedN, feedPos;
  uint32_t feedGeneration;
  uint32_t feedSeqs[kParkedCapacity];
};
ParkedIndex* g_parked = nullptr;

bool parkedHolds(const char* group_code, const char* contact_key) {
  return g_parked != nullptr && strcmp(g_parked->group, group_code) == 0 && strcmp(g_parked->contact, contact_key) == 0;
}

bool isParked(const char* group_code, const char* contact_key) {
  return g_parked != nullptr && g_parked->valid && parkedHolds(group_code, contact_key);
}

// The same-conversation mutation under way (set by bumpGenerationFor(),
// applied by afterRecordWrite()/afterRecordRemove() once its outcome is
// known): which indexes hold that conversation and were current before it.
struct PendingPatch {
  bool active;
  bool working, loader, parked;
  uint32_t generation;  // the generation the mutation bumped to
};
PendingPatch g_patch = {};

ConversationIndexEntry entryFromRecord(uint32_t seq, const StoredHeader& header, const uint8_t* bytes, size_t len,
                                       bool* ok) {
  ConversationIndexEntry e;
  memset(&e, 0, sizeof(e));
  e.sequence = seq;
  e.effective_sort_timestamp = header.effective_sort_timestamp;
  e.flags = header.flags;
  *ok = len >= kMessageIdOffsetInFile + PacketCodec::kMessageIdLen;
  if (*ok) {
    memcpy(e.message_id, bytes + kMessageIdOffsetInFile, PacketCodec::kMessageIdLen);
    e.message_id[PacketCodec::kMessageIdLen - 1] = '\0';
  }
  return e;
}

bool seqInLoaderRemainder(uint32_t seq) {
  for (uint16_t i = g_loader.pos; i < g_loader.n; i++) {
    if (g_loaderSeqs[i] == seq) return true;
  }
  return false;
}

void removeFromLoaderRemainder(uint32_t seq) {
  for (uint16_t i = g_loader.pos; i < g_loader.n; i++) {
    if (g_loaderSeqs[i] == seq) {
      memmove(&g_loaderSeqs[i], &g_loaderSeqs[i + 1], sizeof(uint32_t) * (g_loader.n - i - 1));
      g_loader.n--;
      return;
    }
  }
}
}  // namespace

namespace {
// Bumps the storage generation for a mutation of one conversation (nullptr:
// unknown, e.g. a whole group removed outside the store). Indexes of OTHER
// conversations that were current stay current; those of this conversation
// are patched once the mutation's outcome is known (afterRecordWrite()/
// afterRecordRemove()), or go stale.
void bumpGenerationFor(const char* group_code, const char* contact_key) {
  uint32_t before = g_storageChangeGeneration;
  bool known = group_code != nullptr && contact_key != nullptr;
  bool target = known && isIndexTarget(group_code, contact_key);
  bool keepWorking = known && g_indexCacheValid && g_indexCacheGeneration == before;
  bool keepLoader = known && g_loader.active && g_loader.generation == before;
  bool keepParked = known && g_parked != nullptr && g_parked->valid && g_parked->generation == before &&
                    !isParked(group_code, contact_key);
  bool keepParkedFeed = known && g_parked != nullptr && g_parked->feeding && g_parked->feedGeneration == before &&
                        !parkedHolds(group_code, contact_key);
  g_patch.active = known;
  g_patch.working = keepWorking && target;
  g_patch.loader = keepLoader && target && !g_loader.fed;  // a fed load is dropped instead (the recount relists)
  g_patch.parked = known && isParked(group_code, contact_key) && g_parked->generation == before;
  g_storageChangeGeneration++;
  g_patch.generation = g_storageChangeGeneration;
  if (keepWorking && !target) g_indexCacheGeneration = g_storageChangeGeneration;
  if (keepLoader && !target) g_loader.generation = g_storageChangeGeneration;
  if (keepParked) g_parked->generation = g_storageChangeGeneration;
  if (keepParkedFeed) g_parked->feedGeneration = g_storageChangeGeneration;
}

void afterRecordWrite(const MessageRef& ref, const StoredHeader& header, bool ok) {
  writeThroughCachedRecord(ref, ok);
  PendingPatch p = g_patch;
  g_patch.active = false;
  if (!ok || !p.active || p.generation != g_storageChangeGeneration) return;  // stale: reloaded at the next use
  bool idOk = false;
  ConversationIndexEntry e = entryFromRecord(ref.sequence, header, g_rewriteScratch, g_rewriteLen, &idOk);
  if (!idOk) return;
  if (p.working && patchIndexWrite(g_index, &g_indexCount, kMaxMessagesPerThread, e)) g_indexCacheGeneration = p.generation;
  if (p.loader) {
    // Loaded already: patched. Still to be read: read later, as written now.
    bool fine = seqInLoaderRemainder(ref.sequence) || patchIndexWrite(g_index, &g_indexCount, kMaxMessagesPerThread, e);
    if (fine) g_loader.generation = p.generation;
  }
  if (p.parked && patchIndexWrite(g_parked->entries, &g_parked->count, kParkedCapacity, e)) g_parked->generation = p.generation;
}

void afterRecordRemove(const char* group_code, const char* contact_key, uint32_t seq, bool removed) {
  (void)group_code;
  (void)contact_key;
  PendingPatch p = g_patch;
  g_patch.active = false;
  if (!removed || !p.active || p.generation != g_storageChangeGeneration) return;
  if (p.working) {
    patchIndexRemove(g_index, &g_indexCount, seq);
    g_indexCacheGeneration = p.generation;
  }
  if (p.loader) {
    removeFromLoaderRemainder(seq);
    patchIndexRemove(g_index, &g_indexCount, seq);
    g_loader.generation = p.generation;
  }
  if (p.parked) {
    patchIndexRemove(g_parked->entries, &g_parked->count, seq);
    g_parked->generation = p.generation;
  }
}

// The shared index (complete and current) is kept in the parked slot before
// the shared one is reused for another conversation.
void parkWorkingIndex() {
  if (!g_indexCacheValid || g_indexCacheGeneration != g_storageChangeGeneration) return;
  if (g_indexCount > kParkedCapacity) return;  // an older firmware's longer history: not parked
  if (g_parked == nullptr) {
    g_parked = new (std::nothrow) ParkedIndex;
    if (g_parked == nullptr) return;
    g_parked->valid = false;
    g_parked->feeding = false;
  }
  strncpy(g_parked->group, g_indexCacheGroup, sizeof(g_parked->group) - 1);
  g_parked->group[sizeof(g_parked->group) - 1] = '\0';
  strncpy(g_parked->contact, g_indexCacheContact, sizeof(g_parked->contact) - 1);
  g_parked->contact[sizeof(g_parked->contact) - 1] = '\0';
  memcpy(g_parked->entries, g_index, sizeof(ConversationIndexEntry) * g_indexCount);
  g_parked->feeding = false;
  g_parked->count = g_indexCount;
  g_parked->generation = g_indexCacheGeneration;
  g_parked->valid = true;
}

void setIndexIdentity(const char* group_code, const char* contact_key) {
  strncpy(g_indexCacheGroup, group_code, sizeof(g_indexCacheGroup) - 1);
  g_indexCacheGroup[sizeof(g_indexCacheGroup) - 1] = '\0';
  strncpy(g_indexCacheContact, contact_key, sizeof(g_indexCacheContact) - 1);
  g_indexCacheContact[sizeof(g_indexCacheContact) - 1] = '\0';
}

bool identityFits(const char* group_code, const char* contact_key) {
  return strlen(group_code) < sizeof(g_indexCacheGroup) && strlen(contact_key) < sizeof(g_indexCacheContact);
}

void finishLoad() {
  g_loader.active = false;
  if (g_loader.reliable && g_loader.generation == g_storageChangeGeneration) {
    g_indexCacheGeneration = g_storageChangeGeneration;
    g_indexCacheValid = true;
  }
  uint32_t wallMs = millis() - g_loader.startMs;
  uint32_t busyMs = g_loader.listMs + g_loader.readMs;
  if (busyMs >= 20 || wallMs >= 20) {
    Serial.printf("[PERF][CHAT_INDEX] total=%lu ms list=%lu ms read=%lu ms sort=0 ms enumerated=%u indexed=%u steps=%u wall=%lu ms%s\n",
                  static_cast<unsigned long>(busyMs), static_cast<unsigned long>(g_loader.listMs),
                  static_cast<unsigned long>(g_loader.readMs), static_cast<unsigned>(g_loader.n),
                  static_cast<unsigned>(g_indexCount), static_cast<unsigned>(g_loader.steps),
                  static_cast<unsigned long>(wallMs), g_loader.fed ? " fed=1" : "");
  }
}

void insertLoadedEntry(uint32_t seq, const StoredHeader& hdr, const char* mid) {
  ConversationIndexEntry e;
  e.sequence = seq;
  e.effective_sort_timestamp = hdr.effective_sort_timestamp;
  strncpy(e.message_id, mid, PacketCodec::kMessageIdLen - 1);
  e.message_id[PacketCodec::kMessageIdLen - 1] = '\0';
  e.flags = hdr.flags;
  if (!patchIndexWrite(g_index, &g_indexCount, kMaxMessagesPerThread, e)) g_loader.reliable = false;
}

void startLoad(const char* group_code, const char* contact_key, bool fed) {
  if (!isIndexTarget(group_code, contact_key)) parkWorkingIndex();
  if (g_pendingCandidateValid &&
      (strcmp(g_pendingCandidateGroup, group_code) != 0 || strcmp(g_pendingCandidateContact, contact_key) != 0)) {
    g_pendingCandidateValid = false;
  }
  g_indexCacheValid = false;
  setIndexIdentity(group_code, contact_key);
  g_indexCount = 0;
  memset(&g_loader, 0, sizeof(g_loader));
  g_loader.active = true;
  g_loader.reliable = identityFits(group_code, contact_key);
  g_loader.fed = fed;
  g_loader.generation = g_storageChangeGeneration;
  g_loader.startMs = millis();
}
}  // namespace

bool stepConversationIndex(const char* group_code, const char* contact_key, uint8_t maxReads, uint16_t* outCount,
                           uint16_t* outRead, uint16_t* outTotal) {
  uint32_t now = g_storageChangeGeneration;
  auto report = [&](bool done) {
    if (outCount != nullptr) *outCount = g_indexCount;
    if (outRead != nullptr) *outRead = g_loader.active ? g_loader.pos : g_indexCount;
    if (outTotal != nullptr) *outTotal = g_loader.active ? g_loader.n : g_indexCount;
    return done;
  };
  // Hardware Diagnostic #4.9k: reuse the shared index untouched when it is
  // complete and current for this conversation. Zero filesystem access.
  if (g_indexCacheValid && strcmp(g_indexCacheGroup, group_code) == 0 &&
      strcmp(g_indexCacheContact, contact_key) == 0 && g_indexCacheGeneration == now) {
    Serial.printf("[PERF][CHAT_INDEX] cacheHit=1 indexed=%u\n", static_cast<unsigned>(g_indexCount));
    return report(true);
  }
  // The parked index: swapped in, the shared one parked in its place.
  if (isParked(group_code, contact_key) && g_parked->generation == now && identityFits(group_code, contact_key)) {
    bool parkOut = g_indexCacheValid && g_indexCacheGeneration == now && g_indexCount <= kParkedCapacity;
    uint16_t inCount = g_parked->count;
    uint16_t outCountWorking = g_indexCount;
    char outGroup[PacketCodec::kGroupCodeLen], outContact[kContactKeyLen];
    strncpy(outGroup, g_indexCacheGroup, sizeof(outGroup));
    strncpy(outContact, g_indexCacheContact, sizeof(outContact));
    uint16_t m = inCount > outCountWorking ? inCount : outCountWorking;
    for (uint16_t i = 0; i < m; i++) {
      ConversationIndexEntry t = g_index[i];
      g_index[i] = g_parked->entries[i];
      if (parkOut) g_parked->entries[i] = t;
    }
    g_loader.active = false;
    setIndexIdentity(group_code, contact_key);
    g_indexCount = inCount;
    g_indexCacheGeneration = now;
    g_indexCacheValid = true;
    if (parkOut) {
      strncpy(g_parked->group, outGroup, sizeof(g_parked->group));
      strncpy(g_parked->contact, outContact, sizeof(g_parked->contact));
      g_parked->count = outCountWorking;
      g_parked->generation = now;
    } else {
      g_parked->valid = false;
    }
    g_parked->feeding = false;
    if (g_pendingCandidateValid) g_pendingCandidateValid = false;
    Serial.printf("[PERF][CHAT_INDEX] parkedHit=1 indexed=%u\n", static_cast<unsigned>(g_indexCount));
    return report(true);
  }
  // Hardware Diagnostic #4.9q: a single appended record patched in.
  if (g_pendingCandidateValid && strcmp(g_pendingCandidateGroup, group_code) == 0 &&
      strcmp(g_pendingCandidateContact, contact_key) == 0) {
    uint32_t incTotalStart = millis();
    if (tryApplyIndexAppendCandidate(group_code, contact_key)) {
      uint32_t incTotalElapsed = millis() - incTotalStart;
      Serial.printf("[PERF][CHAT_INDEX] incremental=1 total=%lu ms indexed=%u\n",
                    static_cast<unsigned long>(incTotalElapsed), static_cast<unsigned>(g_indexCount));
      return report(true);
    }
  }
  if (!(g_loader.active && isIndexTarget(group_code, contact_key) && g_loader.generation == now)) {
    startLoad(group_code, contact_key, false);
  } else if (g_loader.fed) {
    g_loader.fed = false;  // a load fed by the recount is continued by this chat's own reads (chatIndexLoading())
  }
  g_loader.steps++;
  g_loader.lastStepMs = millis();
  uint32_t stepStart = millis();
  if (!g_loader.listed) {
    bool openFailed = false, truncated = false;
    uint32_t listStart = millis();
    g_loader.n = listSequences(group_code, contact_key, g_loaderSeqs, kMaxMessagesPerThread, &openFailed, &truncated);
    g_loader.listMs += millis() - listStart;
    if (openFailed || truncated) g_loader.reliable = false;
    g_loader.pos = 0;
    g_loader.listed = true;
  }
  uint32_t readStart = millis();
  for (uint8_t k = 0; k < maxReads && g_loader.pos < g_loader.n; k++) {
    if (k > 0 && millis() - stepStart >= kIndexStepBudgetMs) break;
    uint32_t seq = g_loaderSeqs[g_loader.pos++];
    StoredHeader hdr;
    char mid[PacketCodec::kMessageIdLen];
    if (readIndexMetadata(group_code, contact_key, seq, &hdr, mid)) {
      insertLoadedEntry(seq, hdr, mid);
    } else {
      g_loader.reliable = false;  // left out, as the one-call load did; not cached
    }
  }
  g_loader.readMs += millis() - readStart;
  if (g_loader.pos < g_loader.n) return report(false);
  finishLoad();
  return report(true);
}

uint16_t loadConversationIndex(const char* group_code, const char* contact_key) {
  while (!stepConversationIndex(group_code, contact_key, 255, nullptr, nullptr, nullptr)) {
  }
  return g_indexCount;
}

bool isIndexCurrent(const char* group_code, const char* contact_key) {
  return g_indexCacheValid && g_indexCacheGeneration == g_storageChangeGeneration &&
         strcmp(g_indexCacheGroup, group_code) == 0 && strcmp(g_indexCacheContact, contact_key) == 0;
}

uint16_t getIndexCount() { return g_indexCount; }

bool chatIndexLoading() {
  return g_loader.active && !g_loader.fed && millis() - g_loader.lastStepMs < kChatLoadingMs;
}

namespace {
// The shared index is in use: the feed builds the parked one instead -- not
// over a current parked index of another conversation (the last chat).
bool beginParkedFeed(const char* group_code, const char* contact_key, const uint32_t* seqs, uint16_t n,
                     bool listingReliable) {
  uint32_t now = getStorageChangeGeneration();
  if (!listingReliable || n > kParkedCapacity) return false;
  if (strlen(group_code) >= PacketCodec::kGroupCodeLen || strlen(contact_key) >= kContactKeyLen) return false;
  if (g_parked != nullptr && g_parked->valid && g_parked->generation == now) return false;  // current: kept
  if (g_parked == nullptr) {
    g_parked = new (std::nothrow) ParkedIndex;
    if (g_parked == nullptr) return false;
  }
  g_parked->valid = false;
  strncpy(g_parked->group, group_code, sizeof(g_parked->group) - 1);
  g_parked->group[sizeof(g_parked->group) - 1] = '\0';
  strncpy(g_parked->contact, contact_key, sizeof(g_parked->contact) - 1);
  g_parked->contact[sizeof(g_parked->contact) - 1] = '\0';
  g_parked->count = 0;
  g_parked->feeding = true;
  g_parked->feedReliable = true;
  g_parked->feedN = n;
  g_parked->feedPos = 0;
  g_parked->feedGeneration = now;
  memcpy(g_parked->feedSeqs, seqs, sizeof(uint32_t) * n);
  if (n == 0) {
    g_parked->feeding = false;
    g_parked->valid = true;
    g_parked->generation = now;
  }
  return true;
}

void feedParkedRecord(uint32_t seq, const StoredHeader* hdr, const char* message_id) {
  ParkedIndex& p = *g_parked;
  if (p.feedPos >= p.feedN || p.feedSeqs[p.feedPos] != seq || p.feedGeneration != getStorageChangeGeneration()) {
    p.feeding = false;  // out of step, or the conversation changed: dropped
    return;
  }
  p.feedPos++;
  if (hdr == nullptr) {
    p.feedReliable = false;
  } else {
    ConversationIndexEntry e;
    e.sequence = seq;
    e.effective_sort_timestamp = hdr->effective_sort_timestamp;
    strncpy(e.message_id, message_id, PacketCodec::kMessageIdLen - 1);
    e.message_id[PacketCodec::kMessageIdLen - 1] = '\0';
    e.flags = hdr->flags;
    if (!patchIndexWrite(p.entries, &p.count, kParkedCapacity, e)) p.feedReliable = false;
  }
  if (p.feedPos == p.feedN) {
    p.feeding = false;
    p.valid = p.feedReliable;
    p.generation = p.feedGeneration;
  }
}
}  // namespace

bool beginIndexFeed(const char* group_code, const char* contact_key, const uint32_t* seqs, uint16_t n,
                    bool listingReliable, bool sharedInUse) {
  // Into the shared index only when no chat draws from it, it holds nothing
  // current and no chat is loading it; otherwise into the parked one.
  uint32_t now = getStorageChangeGeneration();
  if (sharedInUse || (g_indexCacheValid && g_indexCacheGeneration == now) ||
      (g_loader.active && !g_loader.fed && g_loader.generation == now)) {
    if (isIndexTarget(group_code, contact_key)) return false;  // that conversation is already the shared index
    return beginParkedFeed(group_code, contact_key, seqs, n, listingReliable);
  }
  if (!listingReliable || n > kMaxMessagesPerThread || !identityFits(group_code, contact_key)) return false;
  startLoad(group_code, contact_key, true);
  g_loader.listed = true;
  g_loader.n = n;
  memcpy(g_loaderSeqs, seqs, sizeof(uint32_t) * n);
  if (n == 0) finishLoad();
  return true;
}

void feedIndexRecord(const char* group_code, const char* contact_key, uint32_t seq, const StoredHeader* hdr,
                     const char* message_id) {
  if (g_parked != nullptr && g_parked->feeding && parkedHolds(group_code, contact_key)) {
    feedParkedRecord(seq, hdr, message_id);
    return;
  }
  if (!g_loader.active || !g_loader.fed || !isIndexTarget(group_code, contact_key)) return;
  if (g_loader.pos >= g_loader.n || g_loaderSeqs[g_loader.pos] != seq ||
      g_loader.generation != getStorageChangeGeneration()) {
    g_loader.active = false;  // out of step, or the conversation changed: a chat loads it itself
    g_indexCount = 0;
    return;
  }
  g_loader.pos++;
  g_loader.steps++;
  if (hdr == nullptr) {
    g_loader.reliable = false;
  } else {
    insertLoadedEntry(seq, *hdr, message_id);
  }
  if (g_loader.pos == g_loader.n) finishLoad();
}

const ConversationIndexEntry* getIndexEntry(uint16_t i) {
  if (i >= g_indexCount) return nullptr;
  return &g_index[i];
}

void updateLocalStatus(const MessageRef& ref, uint16_t newFlags) {
  StoredMessageView view;
  if (!loadMessage(ref, &view)) return;
  StoredHeader hdr = view.header;
  hdr.flags = newFlags;
  atomicRewrite(ref, hdr, view.wirePacket, view.wirePacketLen, view.localPayload, view.localPayloadLen);
}

void updateLocalFlags(const MessageRef& ref, uint16_t setMask, uint16_t clearMask) {
  StoredMessageView view;
  if (!loadMessage(ref, &view)) return;
  StoredHeader hdr = view.header;
  hdr.flags = static_cast<uint16_t>((hdr.flags & ~clearMask) | setMask);
  atomicRewrite(ref, hdr, view.wirePacket, view.wirePacketLen, view.localPayload, view.localPayloadLen);
}

bool updateTypeLocalPayload(const MessageRef& ref, const uint8_t* localPayload, uint16_t localPayloadLen) {
  StoredMessageView view;
  if (!loadMessage(ref, &view)) return false;
  return atomicRewrite(ref, view.header, view.wirePacket, view.wirePacketLen, localPayload, localPayloadLen);
}

bool findMessageById(const char* group_code, const char* contact_key, const char* message_id, MessageRef* outRef) {
  static uint32_t seqs[kMaxMessagesPerThread];
  uint16_t n = listSequences(group_code, contact_key, seqs, kMaxMessagesPerThread);
  for (uint16_t i = 0; i < n; i++) {
    char mid[PacketCodec::kMessageIdLen];
    if (readMessageIdFromFile(group_code, contact_key, seqs[i], mid) && strcmp(mid, message_id) == 0) {
      if (outRef != nullptr) {
        strncpy(outRef->group_code, group_code, sizeof(outRef->group_code) - 1);
        outRef->group_code[sizeof(outRef->group_code) - 1] = '\0';
        strncpy(outRef->contact_key, contact_key, sizeof(outRef->contact_key) - 1);
        outRef->contact_key[sizeof(outRef->contact_key) - 1] = '\0';
        outRef->sequence = seqs[i];
      }
      return true;
    }
  }
  return false;
}

uint16_t listMessageSequences(const char* group_code, const char* contact_key, uint32_t* outSeqs, uint16_t capacity,
                              bool* outReliable, bool* outTruncated) {
  bool openFailed = false, truncated = false;
  uint16_t n = listSequences(group_code, contact_key, outSeqs, capacity, &openFailed, &truncated);
  if (truncated) Serial.printf("[STORE] %s/%s holds more than %u records; listing incomplete (oldest kept)\n", group_code,
                               contact_key, static_cast<unsigned>(capacity));
  if (outReliable != nullptr) *outReliable = !openFailed && !truncated;
  if (outTruncated != nullptr) *outTruncated = truncated;
  return n;
}

bool conversationExists(const char* group_code, const char* contact_key) {
  char dirPath[80];
  buildConversationDir(group_code, contact_key, dirPath, sizeof(dirPath));
  File dir = LittleFS.open(dirPath);
  bool exists = dir && dir.isDirectory();
  if (dir) dir.close();
  return exists;
}

bool deleteMessage(const MessageRef& ref) {
  return removeRecordFile(ref.group_code, ref.contact_key, ref.sequence, /*notifyIfUnread=*/false);
}

struct ConversationWalk {
  File root;
  File group;
  bool inGroup;
  char groupName[33];
};

ConversationWalk* beginConversationWalk() {
  ConversationWalk* w = new (std::nothrow) ConversationWalk;
  if (w == nullptr) return nullptr;
  w->inGroup = false;
  w->groupName[0] = '\0';
  w->root = LittleFS.open("/messages");  // created by init(): failing to open it is a failure, not "empty"
  return w;
}

WalkResult stepConversationWalk(ConversationWalk* w, uint8_t maxEntries, char* outGroup, size_t outGroupSize,
                                char* outContact, size_t outContactSize) {
  if (!w->root || !w->root.isDirectory()) return WalkResult::FAILED;
  for (uint8_t n = 0; n < maxEntries; n++) {
    bool isDir = false;
    if (!w->inGroup) {
      String entry = w->root.getNextFileName(&isDir);
      if (entry.length() == 0) return WalkResult::END;
      if (!isDir) continue;
      strncpy(w->groupName, baseName(entry.c_str()), sizeof(w->groupName) - 1);
      w->groupName[sizeof(w->groupName) - 1] = '\0';
      char groupPath[40];
      buildGroupDir(w->groupName, groupPath, sizeof(groupPath));
      w->group = LittleFS.open(groupPath);
      if (!w->group || !w->group.isDirectory()) return WalkResult::FAILED;
      w->inGroup = true;
      continue;
    }
    String entry = w->group.getNextFileName(&isDir);
    if (entry.length() == 0) {
      w->group.close();
      w->inGroup = false;
      continue;
    }
    if (!isDir) continue;
    strncpy(outGroup, w->groupName, outGroupSize - 1);
    outGroup[outGroupSize - 1] = '\0';
    strncpy(outContact, baseName(entry.c_str()), outContactSize - 1);
    outContact[outContactSize - 1] = '\0';
    return WalkResult::FOUND;
  }
  return WalkResult::MORE;
}

void endConversationWalk(ConversationWalk* w) {
  if (w == nullptr) return;
  if (w->inGroup) w->group.close();
  if (w->root) w->root.close();
  delete w;
}

uint16_t findMessagesByPredicate(const char* group_code, const char* contact_key, MessagePredicate pred, void* ctx,
                                 MessageRef* outRefs, uint16_t outRefsCapacity, bool* outScanReliable) {
  // Hardware Diagnostic #4.9h follow-up: initialized conservatively before
  // any scanning happens, so even a hypothetical future early-return added
  // here later would leave the caller with a safe "unreliable" answer
  // rather than an uninitialized or accidentally-optimistic one. The real
  // computed value is written once, at this function's one true exit
  // below.
  if (outScanReliable != nullptr) *outScanReliable = false;

  uint16_t matchCount = 0;
  // Sticky-false: starts true, and is only ever downgraded by a detected
  // directory-open failure (propagated from listSequences()/
  // forEachConversation()) or a record that listSequences() found but
  // loadMessage() then failed to read/decode. Either kind of failure means
  // this scan's zero/nonzero result can't be trusted as complete, even
  // though every record that WAS successfully loaded is still fully
  // matched/counted below -- a read failure never skips or discards an
  // otherwise-good result elsewhere in the same scan.
  bool reliable = true;

  auto scanConversation = [&](const char* g, const char* c) {
    static uint32_t seqs[kMaxMessagesPerThread];
    bool openFailed = false, truncated = false;
    uint16_t n = listSequences(g, c, seqs, kMaxMessagesPerThread, &openFailed, &truncated);
    if (openFailed || truncated) reliable = false;  // records beyond the listing capacity were not scanned
    for (uint16_t i = 0; i < n; i++) {
      MessageRef ref;
      strncpy(ref.group_code, g, sizeof(ref.group_code) - 1);
      ref.group_code[sizeof(ref.group_code) - 1] = '\0';
      strncpy(ref.contact_key, c, sizeof(ref.contact_key) - 1);
      ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
      ref.sequence = seqs[i];

      StoredMessageView view;
      if (loadMessage(ref, &view)) {
        if (pred(view.header, view.envelope, ctx)) {
          if (matchCount < outRefsCapacity) outRefs[matchCount] = ref;
          matchCount++;
        }
      } else {
        // A record listSequences() just found on disk but that then
        // failed to load/decode is a genuine anomaly -- this scan's
        // result (even a subsequent zero) can no longer be trusted as
        // complete. Every OTHER record this scan does successfully read
        // is still matched/counted normally above; this only poisons
        // reliability, it never stops or unwinds the scan.
        reliable = false;
      }
    }
  };

  if (group_code == nullptr) {
    forEachConversation(nullptr, scanConversation, &reliable);
  } else if (contact_key == nullptr) {
    forEachConversation(group_code, scanConversation, &reliable);
  } else {
    scanConversation(group_code, contact_key);
  }

  if (outScanReliable != nullptr) *outScanReliable = reliable;
  return matchCount;
}

bool evictOldest(const char* group_code, const char* contact_key) {
  uint32_t seq;
  if (!findOldestNonPendingSequence(group_code, contact_key, &seq)) return false;
  return removeRecordFile(group_code, contact_key, seq, /*notifyIfUnread=*/true);
}

bool isDuplicateAndRecord(const char* group_code, const char* contact_key, const char* message_id) {
  DedupRing* ring = getOrCreateRing(group_code, contact_key);
  for (uint8_t i = 0; i < ring->count; i++) {
    if (strcmp(ring->ids[i], message_id) == 0) return true;
  }
  strncpy(ring->ids[ring->nextSlot], message_id, PacketCodec::kMessageIdLen - 1);
  ring->ids[ring->nextSlot][PacketCodec::kMessageIdLen - 1] = '\0';
  ring->nextSlot = static_cast<uint8_t>((ring->nextSlot + 1) % kDedupRingLen);
  if (ring->count < kDedupRingLen) ring->count++;
  return false;
}

void buildSenderPrefix(const PacketCodec::MessageEnvelope& env, char* out, size_t outSize) {
  const char* name;
  if (strcmp(env.sender_device_id, Identity::deviceId()) == 0) {
    // Our own device (Hardware Fix #4.7): never trust a historical
    // sender_name_cache for the local device -- an old stored record may
    // still carry a name we no longer use (including the compiled "Me"
    // placeholder pre-4.7 devices used to cache), while the CURRENT
    // configured name is the only one that should ever be shown for
    // messages we sent, without rewriting the stored record itself.
    name = Settings::hasCustomMyName() ? Settings::getMyName() : Identity::deviceId();
  } else {
    // Other devices: unchanged -- prefer the cached display name, falling
    // back to the raw device id when the cache is empty.
    name = (env.sender_name_cache[0] != '\0') ? env.sender_name_cache : env.sender_device_id;
  }
  // Hardware Fix #4.8b Part C14/C16: no trailing ": " (or any other
  // text-space suffix) here anymore -- callers now draw a small TFT-
  // primitive divider bar (Display::drawSenderDivider()) immediately after
  // this bare name instead, so this string must contain only the resolved
  // sender name for layout (Display::textWidth()) to measure correctly.
  snprintf(out, outSize, "%s", name);
}

}  // namespace MessageStore
