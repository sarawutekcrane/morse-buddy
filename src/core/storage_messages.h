#pragma once
// Unified Conversation Thread storage engine (Addendum sections 7.8-7.11,
// 8; Phase 2 section 7). Gives StoredMessageView/MessageRef (forward-
// declared as incomplete types in core/hooks.h) their real definition.
//
// LittleFS path: /messages/<group_code>/<contact_key>/<sequence>.msg
// contact_key is a device_id or the literal "EVERYONE".
//
// Type-specific local payload is treated as opaque bytes here; Phase 3
// stores Enigma/Game local state through updateTypeLocalPayload() without
// this file changing.

#include <stddef.h>
#include <stdint.h>

#include "core/packet_codec.h"

namespace MessageStore {

// Messages kept per conversation: an append evicts the oldest non-pending
// record (lowest sequence) once this many are stored, and Notifications'
// background trim brings older conversations down to it.
constexpr uint16_t kRetainedMessagesPerThread = 100;
// The most records a conversation can hold: the cap of every earlier
// firmware, so stored history is never larger. Listings are sized for it,
// and a listing that finds more reports itself incomplete.
constexpr uint16_t kMaxMessagesPerThread = 300;
constexpr size_t kContactKeyLen = 17;
constexpr size_t kMaxLocalPayloadLen = 400;
constexpr const char* kEveryone = "EVERYONE";

enum class Direction : uint8_t { SENT = 0, RECEIVED = 1 };

enum StoredFlag : uint16_t {
  FLAG_PENDING_OUTBOX = 1u << 0,
  FLAG_UNREAD = 1u << 1,
};

struct StoredHeader {
  Direction direction;
  uint16_t flags;
  uint32_t local_write_sequence;
  uint32_t effective_sort_timestamp;
};

}  // namespace MessageStore

// MessageRef/StoredMessageView are declared as incomplete types at GLOBAL
// scope in core/hooks.h (RenderFn/MessageEventFn use them there) — they
// must be defined here at global scope too, matching exactly, not nested
// in namespace MessageStore, or registerMessageType()'s function-pointer
// types would silently mismatch.

// Enough to re-locate/re-load one specific stored message.
struct MessageRef {
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  uint32_t sequence;
};

// Lazily-loaded, read-only view of one stored message for rendering. All
// pointer fields point into an internal shared buffer valid only until the
// next loadMessage() call — render immediately, don't retain.
struct StoredMessageView {
  MessageRef ref;
  MessageStore::StoredHeader header;
  PacketCodec::MessageEnvelope envelope;
  const uint8_t* wirePacket;
  uint16_t wirePacketLen;
  const uint8_t* typePayload;
  uint16_t typePayloadLen;
  const uint8_t* localPayload;
  uint16_t localPayloadLen;
};

namespace MessageStore {

struct ConversationIndexEntry {
  uint32_t sequence;
  uint32_t effective_sort_timestamp;
  char message_id[PacketCodec::kMessageIdLen];
  uint16_t flags;
};

// Called whenever an UNREAD record is evicted (FIFO or global low-space),
// so Notifications can decrement its per-conversation summary. Optional.
using EvictedCallback = void (*)(const MessageRef& ref, uint16_t flags);
void setOnEvictedCallback(EvictedCallback cb);

void init();

// Hardware Diagnostic #4.9h: a RAM-only, monotonically increasing counter
// bumped on every storage mutation that could change what a
// findMessagesByPredicate() scan would find -- append, any flag change
// (including Outbox's own pending-flag clear after a successful publish),
// a local-payload rewrite, and eviction. NOT persisted to NVS/disk and
// carries no meaning across a reboot (starts at 0 every boot); callers
// only ever compare two readings taken within the same boot session to
// detect "did storage possibly change since I last scanned," never store
// it as a message count or version number in its own right. See
// atomicRewrite()'s and the eviction path's own comments in
// storage_messages.cpp for exactly where and why this is bumped.
uint32_t getStorageChangeGeneration();

// Hardware Diagnostic #4.9u: a SEPARATE RAM-only generation counter,
// specifically for Outbox's empty-scan cache (outbox.cpp) -- bumped
// alongside getStorageChangeGeneration() above at every mutation that
// could affect whether ANY pending-outbox record exists anywhere, but
// deliberately NOT bumped by a successful write whose resulting header
// has no FLAG_PENDING_OUTBOX bit (see atomicRewrite()'s own comment in
// storage_messages.cpp), since such a write cannot have created a new
// pending record and therefore cannot invalidate a previously-established
// "no pending messages anywhere" answer. Every other mutation (a pending
// append or flag-set, ANY failed write, every deletion, every external
// mutation notification) still bumps this like the general generation.
// Same usage contract as getStorageChangeGeneration(): RAM-only, resets
// to 0 every boot, compared only between two readings taken within the
// same boot session, never stored as a count/version in its own right.
uint32_t getOutboxChangeGeneration();

// Hardware Diagnostic #4.9k: narrowly-scoped notification for the one
// piece of code outside this file that mutates /messages directly without
// going through atomicRewrite()/removeRecordFile() -- currently only
// Storage::removeGroupDirectoryIfPresent(). Advances the same generation
// getStorageChangeGeneration() reports, so anything this file has cached
// (e.g. the shared conversation index) is correctly invalidated. Call
// exactly once, before that external code's own destructive filesystem
// work begins. Not for use by ordinary MessageStore-mediated mutations,
// which already bump the generation themselves.
void notifyExternalStorageMutationAttempted();

// Appends a new record. wirePacket/wirePacketLen is the exact bytes
// sent/received; localPayload may be nullptr/0 (TEXT needs none).
// Enforces the per-thread retention (kRetainedMessagesPerThread, one
// eviction per append) + global low-space eviction first; returns
// false only when no evictable (non-pending) record exists anywhere
// needed to make room — caller shows "Storage Full".
bool appendStoredMessage(const char* group_code, const char* contact_key, Direction direction, uint16_t flags,
                         uint32_t effective_sort_timestamp, const uint8_t* wirePacket, uint16_t wirePacketLen,
                         const uint8_t* localPayload, uint16_t localPayloadLen, MessageRef* outRef);

// Loads one record into the shared internal buffer. False if missing/corrupt.
bool loadMessage(const MessageRef& ref, StoredMessageView* outView);

// Refreshes the internal index for one conversation, sorted ascending by
// (effective_sort_timestamp, message_id) per Addendum 7.11. Returns count.
uint16_t loadConversationIndex(const char* group_code, const char* contact_key);
const ConversationIndexEntry* getIndexEntry(uint16_t i);

void updateLocalStatus(const MessageRef& ref, uint16_t newFlags);
void updateLocalFlags(const MessageRef& ref, uint16_t setMask, uint16_t clearMask);
bool updateTypeLocalPayload(const MessageRef& ref, const uint8_t* localPayload, uint16_t localPayloadLen);

bool findMessageById(const char* group_code, const char* contact_key, const char* message_id, MessageRef* outRef);

// An incremental walk over every stored conversation directory, a bounded
// number of directory entries per call (the number of groups and
// conversations is not bounded). Holds the open directories between calls,
// so a caller can spread a walk over many main-loop passes. Conversations
// created or removed during a walk may or may not be reported; every one
// that exists throughout the walk is reported exactly once.
struct ConversationWalk;
enum class WalkResult : uint8_t { FOUND, MORE, END, FAILED };
// nullptr if out of memory. The walk starts at the first group.
ConversationWalk* beginConversationWalk();
// Reads up to maxEntries directory entries. FOUND: outGroup/outContact name
// the next conversation. MORE: none found yet, call again. END: every
// conversation has been reported. FAILED: a directory could not be opened,
// so the walk is incomplete (end it and start again later).
WalkResult stepConversationWalk(ConversationWalk* walk, uint8_t maxEntries, char* outGroup, size_t outGroupSize,
                                char* outContact, size_t outContactSize);
void endConversationWalk(ConversationWalk* walk);

// The sequence numbers of one conversation's stored messages (directory
// order, at most capacity of them), from directory entries only -- no record
// is opened. outReliable is set false if the directory could not be opened
// or held more than capacity records (the listing is then incomplete). In
// that second case outTruncated (optional) is set, and the listing holds the
// capacity LOWEST (oldest) sequences, in no particular order -- enough to
// trim from the oldest end, never enough to count the conversation.
// Used by Notifications to read a conversation a few records at a time.
uint16_t listMessageSequences(const char* group_code, const char* contact_key, uint32_t* outSeqs, uint16_t capacity,
                              bool* outReliable, bool* outTruncated = nullptr);

// Deletes one stored record. No eviction callback: the caller accounts for
// it. False if it could not be removed.
bool deleteMessage(const MessageRef& ref);

using MessagePredicate = bool (*)(const StoredHeader& header, const PacketCodec::MessageEnvelope& envelope,
                                  void* ctx);

// group_code == nullptr scans every group; contact_key == nullptr (with a
// group_code given) scans every conversation in that group. Matches are
// appended to outRefs up to outRefsCapacity; returns the match count
// found (which may exceed outRefsCapacity if it was too small).
//
// Hardware Diagnostic #4.9h follow-up: optional trailing output, defaulted
// to nullptr so every existing call site (outbox.cpp, enigma.cpp) compiles
// and behaves exactly as before, unaware this parameter exists. When
// non-null, set true only if the scan completed without any detected
// directory-open failure and without any on-disk record it found but
// could not load/decode -- i.e. only when the returned count (including a
// zero count) can be trusted as the true, complete answer. A caller MUST
// NOT treat a zero return as "definitely nothing found" for caching/
// skip-future-work purposes unless this is also true; matched/loaded
// records themselves are always fully valid and counted regardless of
// this flag. Residual limitation: the underlying Arduino FS API provides
// no way to distinguish a directory listing that reached a genuine clean
// end from one cut short by a mid-iteration I/O error, so that specific
// failure mode is not detected -- see listSequences()'s own comment in
// storage_messages.cpp for the exact boundary of what is and isn't
// caught.
uint16_t findMessagesByPredicate(const char* group_code, const char* contact_key, MessagePredicate pred, void* ctx,
                                 MessageRef* outRefs, uint16_t outRefsCapacity, bool* outScanReliable = nullptr);

// Low-level primitive used by every mutator above (temp-write, flush,
// rename); exposed for a future phase's type-specific full-record replace.
bool atomicRewrite(const MessageRef& ref, const StoredHeader& header, const uint8_t* wirePacket,
                   uint16_t wirePacketLen, const uint8_t* localPayload, uint16_t localPayloadLen);

// Evicts the oldest non-pending record in one conversation. False if none
// is evictable (all PENDING_OUTBOX).
bool evictOldest(const char* group_code, const char* contact_key);

// True (and records it) if message_id was already seen for this
// conversation's 32-entry recent ring; QoS1 duplicates are dropped by the
// caller when this returns true.
bool isDuplicateAndRecord(const char* group_code, const char* contact_key, const char* message_id);

// Builds the bare sender name (no trailing punctuation/space) from an
// envelope's sender_name_cache, falling back to sender_device_id when the
// cache is empty (Hardware Fix #4 issue 2). One shared helper so every
// unified-thread renderer (Text/Enigma/Game) shows sender identity the
// same way regardless of which message type a row belongs to; outSize
// must be large enough for a reasonable name, longer names are safely
// truncated (never overflowed) same as any other Display::printLine()
// call. Hardware Fix #4.8b Part C14/C16: this used to append ": " as a
// visual separator, but that read with no visible start boundary against
// Morse dots/dashes on the real 240x135 TFT -- callers now draw a small
// TFT-primitive divider bar (Display::drawSenderDivider()) immediately
// after this bare name instead, so this string must never contain any
// trailing separator text.
void buildSenderPrefix(const PacketCodec::MessageEnvelope& env, char* out, size_t outSize);

}  // namespace MessageStore
