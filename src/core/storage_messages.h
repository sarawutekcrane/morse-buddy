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

// Appends a new record. wirePacket/wirePacketLen is the exact bytes
// sent/received; localPayload may be nullptr/0 (TEXT needs none).
// Enforces per-thread 300 FIFO + global low-space eviction first; returns
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

using MessagePredicate = bool (*)(const StoredHeader& header, const PacketCodec::MessageEnvelope& envelope,
                                  void* ctx);

// group_code == nullptr scans every group; contact_key == nullptr (with a
// group_code given) scans every conversation in that group. Matches are
// appended to outRefs up to outRefsCapacity; returns the match count
// found (which may exceed outRefsCapacity if it was too small).
uint16_t findMessagesByPredicate(const char* group_code, const char* contact_key, MessagePredicate pred, void* ctx,
                                 MessageRef* outRefs, uint16_t outRefsCapacity);

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

}  // namespace MessageStore
