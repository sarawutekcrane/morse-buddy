#include "core/storage_messages.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <stdlib.h>
#include <string.h>

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
uint16_t listSequences(const char* group_code, const char* contact_key, uint32_t* outSeqs, uint16_t capacity) {
  char dirPath[80];
  buildConversationDir(group_code, contact_key, dirPath, sizeof(dirPath));
  uint16_t count = 0;
  File dir = LittleFS.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
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
template <typename Fn>
void forEachConversation(const char* onlyGroup, Fn&& fn) {
  char groupPath[40];
  if (onlyGroup != nullptr) {
    buildGroupDir(onlyGroup, groupPath, sizeof(groupPath));
    File gdir = LittleFS.open(groupPath);
    if (!gdir || !gdir.isDirectory()) {
      if (gdir) gdir.close();
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
    return;
  }
  File groupDir = root.openNextFile();
  while (groupDir) {
    if (groupDir.isDirectory()) {
      char gnameBuf[33];
      strncpy(gnameBuf, baseName(groupDir.path()), sizeof(gnameBuf) - 1);
      gnameBuf[sizeof(gnameBuf) - 1] = '\0';
      groupDir.close();
      forEachConversation(gnameBuf, fn);
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

void init() {
  ensureDirExists("/messages");
  // Dedup rings are rebuilt lazily per-conversation on first touch
  // (getOrCreateRing), which already satisfies "rebuild from newest stored
  // messages" without an eager full-tree scan at boot.
}

bool atomicRewrite(const MessageRef& ref, const StoredHeader& header, const uint8_t* wirePacket,
                   uint16_t wirePacketLen, const uint8_t* localPayload, uint16_t localPayloadLen) {
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

bool appendStoredMessage(const char* group_code, const char* contact_key, Direction direction, uint16_t flags,
                         uint32_t effective_sort_timestamp, const uint8_t* wirePacket, uint16_t wirePacketLen,
                         const uint8_t* localPayload, uint16_t localPayloadLen, MessageRef* outRef) {
  ensureFreeSpaceForWrite();

  if (countMessages(group_code, contact_key) >= kMaxMessagesPerThread) {
    if (!evictOldest(group_code, contact_key)) return false;  // Storage Full
  }

  ensureDirExists("/messages");
  char groupDir[40];
  buildGroupDir(group_code, groupDir, sizeof(groupDir));
  ensureDirExists(groupDir);
  char convDir[80];
  buildConversationDir(group_code, contact_key, convDir, sizeof(convDir));
  ensureDirExists(convDir);

  uint32_t nextSeq = findMaxSequence(group_code, contact_key) + 1;

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

  if (!atomicRewrite(ref, hdr, wirePacket, wirePacketLen, localPayload, localPayloadLen)) return false;
  if (outRef != nullptr) *outRef = ref;
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

bool indexEntryAfter(const ConversationIndexEntry& a, const ConversationIndexEntry& b) {
  if (a.effective_sort_timestamp != b.effective_sort_timestamp) {
    return a.effective_sort_timestamp > b.effective_sort_timestamp;
  }
  return strcmp(a.message_id, b.message_id) > 0;
}
}  // namespace

uint16_t loadConversationIndex(const char* group_code, const char* contact_key) {
  static uint32_t seqs[kMaxMessagesPerThread];
  uint16_t n = listSequences(group_code, contact_key, seqs, kMaxMessagesPerThread);

  g_indexCount = 0;
  for (uint16_t i = 0; i < n && g_indexCount < kMaxMessagesPerThread; i++) {
    StoredHeader hdr;
    char mid[PacketCodec::kMessageIdLen];
    if (readHeaderOnly(group_code, contact_key, seqs[i], &hdr) &&
        readMessageIdFromFile(group_code, contact_key, seqs[i], mid)) {
      ConversationIndexEntry& e = g_index[g_indexCount++];
      e.sequence = seqs[i];
      e.effective_sort_timestamp = hdr.effective_sort_timestamp;
      strncpy(e.message_id, mid, PacketCodec::kMessageIdLen - 1);
      e.message_id[PacketCodec::kMessageIdLen - 1] = '\0';
      e.flags = hdr.flags;
    }
  }

  for (uint16_t i = 1; i < g_indexCount; i++) {
    ConversationIndexEntry key = g_index[i];
    int32_t j = static_cast<int32_t>(i) - 1;
    while (j >= 0 && indexEntryAfter(g_index[j], key)) {
      g_index[j + 1] = g_index[j];
      j--;
    }
    g_index[j + 1] = key;
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
                                 MessageRef* outRefs, uint16_t outRefsCapacity) {
  uint16_t matchCount = 0;

  auto scanConversation = [&](const char* g, const char* c) {
    static uint32_t seqs[kMaxMessagesPerThread];
    uint16_t n = listSequences(g, c, seqs, kMaxMessagesPerThread);
    for (uint16_t i = 0; i < n; i++) {
      MessageRef ref;
      strncpy(ref.group_code, g, sizeof(ref.group_code) - 1);
      ref.group_code[sizeof(ref.group_code) - 1] = '\0';
      strncpy(ref.contact_key, c, sizeof(ref.contact_key) - 1);
      ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
      ref.sequence = seqs[i];

      StoredMessageView view;
      if (loadMessage(ref, &view) && pred(view.header, view.envelope, ctx)) {
        if (matchCount < outRefsCapacity) outRefs[matchCount] = ref;
        matchCount++;
      }
    }
  };

  if (group_code == nullptr) {
    forEachConversation(nullptr, scanConversation);
  } else if (contact_key == nullptr) {
    forEachConversation(group_code, scanConversation);
  } else {
    scanConversation(group_code, contact_key);
  }

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

}  // namespace MessageStore
