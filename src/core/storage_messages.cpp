#include "core/storage_messages.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <stdlib.h>
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
                       bool* outOpenFailed = nullptr) {
  if (outOpenFailed != nullptr) *outOpenFailed = false;
  char dirPath[80];
  buildConversationDir(group_code, contact_key, dirPath, sizeof(dirPath));
  uint16_t count = 0;
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    if (outOpenFailed != nullptr) *outOpenFailed = true;
    return 0;
  }
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      uint32_t seq = static_cast<uint32_t>(strtoul(baseName(f.path()), nullptr, 10));
      if (count < capacity) outSeqs[count++] = seq;
    }
    f.close();
    f = dir.openNextFile();
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

uint16_t countMessages(const char* group_code, const char* contact_key) {
  static uint32_t seqs[kMaxMessagesPerThread];
  return listSequences(group_code, contact_key, seqs, kMaxMessagesPerThread);
}

bool readHeaderOnly(const char* group_code, const char* contact_key, uint32_t seq, StoredHeader* outHdr) {
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
  g_storageChangeGeneration++;

  StoredHeader hdr;
  bool haveHdr = readHeaderOnly(group_code, contact_key, seq, &hdr);
  char path[96];
  buildRecordPath(group_code, contact_key, seq, path, sizeof(path));
  bool removed = LittleFS.remove(path);
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
    File contactDir = gdir.openNextFile();
    while (contactDir) {
      if (contactDir.isDirectory()) {
        char cnameBuf[kContactKeyLen];
        strncpy(cnameBuf, baseName(contactDir.path()), sizeof(cnameBuf) - 1);
        cnameBuf[sizeof(cnameBuf) - 1] = '\0';
        contactDir.close();
        fn(onlyGroup, cnameBuf);
      } else {
        contactDir.close();
      }
      contactDir = gdir.openNextFile();
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
  File groupDir = root.openNextFile();
  while (groupDir) {
    if (groupDir.isDirectory()) {
      char gnameBuf[33];
      strncpy(gnameBuf, baseName(groupDir.path()), sizeof(gnameBuf) - 1);
      gnameBuf[sizeof(gnameBuf) - 1] = '\0';
      groupDir.close();
      forEachConversation(gnameBuf, fn, outReliable);
    } else {
      groupDir.close();
    }
    groupDir = root.openNextFile();
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
void notifyExternalStorageMutationAttempted() { g_storageChangeGeneration++; }

void init() {
  ensureDirExists("/messages");
  // Dedup rings are rebuilt lazily per-conversation on first touch
  // (getOrCreateRing), which already satisfies "rebuild from newest stored
  // messages" without an eager full-tree scan at boot.
}

bool atomicRewrite(const MessageRef& ref, const StoredHeader& header, const uint8_t* wirePacket,
                   uint16_t wirePacketLen, const uint8_t* localPayload, uint16_t localPayloadLen) {
  // Hardware Diagnostic #4.9h: bumped unconditionally, before any of this
  // function's own filesystem work -- see g_storageChangeGeneration's own
  // comment above for why this must happen even on a path that goes on to
  // fail/return false (the remove(finalPath)-then-rename(tempPath,
  // finalPath) sequence below can leave finalPath gone if the rename
  // fails, which is itself a real change to what a later scan would find,
  // even though this function reports failure).
  g_storageChangeGeneration++;

  static uint8_t scratch[kMaxRecordFileSize];
  size_t needed = kStoredHeaderDiskSize + 2 + wirePacketLen + 2 + localPayloadLen;
  if (needed > sizeof(scratch)) return false;

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

  char finalPath[96];
  buildRecordPath(ref.group_code, ref.contact_key, ref.sequence, finalPath, sizeof(finalPath));
  char tempPath[100];
  snprintf(tempPath, sizeof(tempPath), "%s.tmp", finalPath);

  File f = LittleFS.open(tempPath, "w");
  if (!f) return false;
  size_t written = f.write(scratch, pos);
  f.flush();
  f.close();
  if (written != pos) {
    LittleFS.remove(tempPath);
    return false;
  }

  LittleFS.remove(finalPath);  // overwrite semantics: clear any prior file at the target first
  return LittleFS.rename(tempPath, finalPath);
}

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
bool appendStoredMessage(const char* group_code, const char* contact_key, Direction direction, uint16_t flags,
                         uint32_t effective_sort_timestamp, const uint8_t* wirePacket, uint16_t wirePacketLen,
                         const uint8_t* localPayload, uint16_t localPayloadLen, MessageRef* outRef) {
  constexpr uint32_t kUnmeasuredMs = 0xFFFFFFFFu;
  uint32_t totalStart = millis();

  uint32_t freeSpaceStart = millis();
  ensureFreeSpaceForWrite();
  uint32_t freeSpaceElapsed = millis() - freeSpaceStart;

  uint32_t countStart = millis();
  uint16_t msgCount = countMessages(group_code, contact_key);
  uint32_t countElapsed = millis() - countStart;

  uint32_t evictElapsed = kUnmeasuredMs;
  if (msgCount >= kMaxMessagesPerThread) {
    uint32_t evictStart = millis();
    bool evictOk = evictOldest(group_code, contact_key);
    evictElapsed = millis() - evictStart;
    if (!evictOk) {  // Storage Full
      uint32_t totalElapsed = millis() - totalStart;
      Serial.printf(
          "[PERF][STORE_APPEND] total=%lu ms exit=evict-fail freeSpace=%lu ms count=%lu ms evict=%lu ms "
          "dirs=%lu ms maxSeq=%lu ms prepare=%lu ms rewrite=%lu ms\n",
          static_cast<unsigned long>(totalElapsed), static_cast<unsigned long>(freeSpaceElapsed),
          static_cast<unsigned long>(countElapsed), static_cast<unsigned long>(evictElapsed),
          static_cast<unsigned long>(kUnmeasuredMs), static_cast<unsigned long>(kUnmeasuredMs),
          static_cast<unsigned long>(kUnmeasuredMs), static_cast<unsigned long>(kUnmeasuredMs));
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
  uint32_t nextSeq = findMaxSequence(group_code, contact_key) + 1;
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
        "dirs=%lu ms maxSeq=%lu ms prepare=%lu ms rewrite=%lu ms\n",
        static_cast<unsigned long>(totalElapsed), static_cast<unsigned long>(freeSpaceElapsed),
        static_cast<unsigned long>(countElapsed), static_cast<unsigned long>(evictElapsed),
        static_cast<unsigned long>(dirsElapsed), static_cast<unsigned long>(maxSeqElapsed),
        static_cast<unsigned long>(prepareElapsed), static_cast<unsigned long>(rewriteElapsed));
    return false;
  }
  if (outRef != nullptr) *outRef = ref;

  uint32_t totalElapsed = millis() - totalStart;
  if (totalElapsed >= 20) {
    Serial.printf(
        "[PERF][STORE_APPEND] total=%lu ms exit=completion freeSpace=%lu ms count=%lu ms evict=%lu ms "
        "dirs=%lu ms maxSeq=%lu ms prepare=%lu ms rewrite=%lu ms\n",
        static_cast<unsigned long>(totalElapsed), static_cast<unsigned long>(freeSpaceElapsed),
        static_cast<unsigned long>(countElapsed), static_cast<unsigned long>(evictElapsed),
        static_cast<unsigned long>(dirsElapsed), static_cast<unsigned long>(maxSeqElapsed),
        static_cast<unsigned long>(prepareElapsed), static_cast<unsigned long>(rewriteElapsed));
  }
  return true;
}

namespace {
uint8_t g_loadBuffer[kMaxRecordFileSize];
}  // namespace

bool loadMessage(const MessageRef& ref, StoredMessageView* outView) {
  if (outView == nullptr) return false;
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

namespace {
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
}  // namespace

// Hardware Diagnostic #4.9i: instrumentation only -- every existing return
// value, record limit, ordering rule, filtering condition, and error path
// (a record whose metadata can't be fully read is still silently excluded
// from the index exactly as before -- see readIndexMetadata(), added by
// the #4.9j follow-up to read it through one file handle instead of the
// original two-helper pair this comment used to name) is unchanged. Four
// uint32_t durations are accumulated with plain millis()-delta subtraction
// (the same rollover-safe convention already used elsewhere in this
// codebase, e.g. outbox.cpp's scan/publish/flush timing -- unsigned
// wraparound arithmetic gives the correct elapsed value even across a
// millis() rollover) and only printed, as one line, after everything is
// done, so the diagnostic Serial.printf() call itself is never included in
// any of the four measured windows.
//
// Hardware Diagnostic #4.9k: a cache-hit check now runs first (see the
// early return below) -- when it hits, none of the above real-load timing
// runs at all, so the existing [PERF][CHAT_INDEX] total/list/read/sort
// line only ever describes an actual load, exactly as before. A hit
// instead prints its own compact line. No boot preload: g_indexCacheValid
// starts false, so the very first call for any conversation always falls
// through to a real load.
uint16_t loadConversationIndex(const char* group_code, const char* contact_key) {
  // Hardware Diagnostic #4.9k: reuse the shared index untouched when
  // nothing that could invalidate it has happened since the last
  // successful, reliable load of this EXACT conversation -- same group,
  // same contact, same storage generation. Zero filesystem access on a
  // hit. There is only one cache slot (matching the one shared g_index),
  // so switching A -> B -> A always misses on the return to A; only
  // re-entering the SAME conversation with nothing having bumped the
  // generation in between can hit.
  if (g_indexCacheValid && strcmp(g_indexCacheGroup, group_code) == 0 &&
      strcmp(g_indexCacheContact, contact_key) == 0 &&
      getStorageChangeGeneration() == g_indexCacheGeneration) {
    Serial.printf("[PERF][CHAT_INDEX] cacheHit=1 indexed=%u\n", static_cast<unsigned>(g_indexCount));
    return g_indexCount;
  }
  // Falling through to a real load: whatever was cached no longer applies
  // once g_index below starts being overwritten, so invalidate it BEFORE
  // that happens -- if this load itself turns out unreliable, no stale
  // "valid" cache survives it either.
  g_indexCacheValid = false;

  uint32_t totalStart = millis();

  // Hardware Diagnostic #4.9k: generation snapshot taken before directory
  // enumeration starts; compared again after the metadata-read loop
  // finishes (sorting never touches storage, so it can't move this) to
  // confirm nothing mutated this conversation's storage while this load
  // was in progress.
  uint32_t generationBeforeLoad = getStorageChangeGeneration();

  static uint32_t seqs[kMaxMessagesPerThread];
  uint32_t listStart = millis();
  // Hardware Diagnostic #4.9k: same outOpenFailed output #4.9h-follow-up
  // added to listSequences() -- any failed/non-directory open, INCLUDING a
  // conversation directory that has simply never been created, makes this
  // load ineligible for caching below. Unlike forEachConversation()'s
  // multi-directory walk (where a never-created group/root is normal),
  // here group_code/contact_key name one specific, already-selected
  // conversation, so treating any open failure as uncacheable is the
  // simple, correct rule the task calls for -- it does not change what is
  // returned or enumerated, only whether the result may be trusted later.
  bool dirOpenFailed = false;
  uint16_t n = listSequences(group_code, contact_key, seqs, kMaxMessagesPerThread, &dirOpenFailed);
  uint32_t listElapsed = millis() - listStart;

  g_indexCount = 0;
  // Hardware Diagnostic #4.9k: sticky-false across the whole read loop --
  // any single readIndexMetadata() failure poisons cacheability for this
  // load, but (matching the existing behavior this comment already
  // documented) never stops or skips processing of the OTHER records:
  // every one that DOES load successfully is still indexed exactly as
  // before.
  bool metadataReliable = true;
  uint32_t readStart = millis();
  for (uint16_t i = 0; i < n && g_indexCount < kMaxMessagesPerThread; i++) {
    StoredHeader hdr;
    char mid[PacketCodec::kMessageIdLen];
    if (readIndexMetadata(group_code, contact_key, seqs[i], &hdr, mid)) {
      ConversationIndexEntry& e = g_index[g_indexCount++];
      e.sequence = seqs[i];
      e.effective_sort_timestamp = hdr.effective_sort_timestamp;
      strncpy(e.message_id, mid, PacketCodec::kMessageIdLen - 1);
      e.message_id[PacketCodec::kMessageIdLen - 1] = '\0';
      e.flags = hdr.flags;
    } else {
      metadataReliable = false;
    }
  }
  uint32_t readElapsed = millis() - readStart;

  uint32_t sortStart = millis();
  for (uint16_t i = 1; i < g_indexCount; i++) {
    ConversationIndexEntry key = g_index[i];
    int32_t j = static_cast<int32_t>(i) - 1;
    while (j >= 0 && indexEntryAfter(g_index[j], key)) {
      g_index[j + 1] = g_index[j];
      j--;
    }
    g_index[j + 1] = key;
  }
  uint32_t sortElapsed = millis() - sortStart;

  // Hardware Diagnostic #4.9k: establish cache validity only when this
  // load can be trusted to still be correct until something bumps the
  // generation again -- reliable directory enumeration AND reliable
  // per-record metadata reads AND the generation unchanged across the
  // whole load (a successfully opened, genuinely empty directory still
  // qualifies: dirOpenFailed is false and the read loop trivially stays
  // reliable with n == 0). A partial failure still returns every
  // successfully indexed entry above exactly as before; it simply isn't
  // cached for reuse. Overlong group_code/contact_key that the cache
  // buffers can't hold without truncation are also refused -- caching a
  // truncated identity could later false-hit against a different,
  // longer key sharing the same truncated prefix.
  bool loadReliable = !dirOpenFailed && metadataReliable && (getStorageChangeGeneration() == generationBeforeLoad);
  if (loadReliable && strlen(group_code) < sizeof(g_indexCacheGroup) &&
      strlen(contact_key) < sizeof(g_indexCacheContact)) {
    strncpy(g_indexCacheGroup, group_code, sizeof(g_indexCacheGroup) - 1);
    g_indexCacheGroup[sizeof(g_indexCacheGroup) - 1] = '\0';
    strncpy(g_indexCacheContact, contact_key, sizeof(g_indexCacheContact) - 1);
    g_indexCacheContact[sizeof(g_indexCacheContact) - 1] = '\0';
    g_indexCacheGeneration = getStorageChangeGeneration();
    g_indexCacheValid = true;
  }

  uint32_t totalElapsed = millis() - totalStart;
  if (totalElapsed >= 20) {
    Serial.printf("[PERF][CHAT_INDEX] total=%lu ms list=%lu ms read=%lu ms sort=%lu ms enumerated=%u indexed=%u\n",
                  static_cast<unsigned long>(totalElapsed), static_cast<unsigned long>(listElapsed),
                  static_cast<unsigned long>(readElapsed), static_cast<unsigned long>(sortElapsed),
                  static_cast<unsigned>(n), static_cast<unsigned>(g_indexCount));
  }
  return g_indexCount;
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
    bool openFailed = false;
    uint16_t n = listSequences(g, c, seqs, kMaxMessagesPerThread, &openFailed);
    if (openFailed) reliable = false;
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
