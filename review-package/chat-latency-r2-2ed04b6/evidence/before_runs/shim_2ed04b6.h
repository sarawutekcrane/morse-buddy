// BEFORE-RUN SHIM declarations (not production): what the final storage
// tests call that 2ed04b6 lacks.
#pragma once
#include "core/storage_messages.h"
namespace MessageStore {
bool stepConversationIndex(const char* g, const char* c, uint8_t maxReads, uint16_t* count, uint16_t* read, uint16_t* total);
bool beginIndexFeed(const char* g, const char* c, const uint32_t* seqs, uint16_t n, bool reliable, bool sharedInUse);
void feedIndexRecord(const char* g, const char* c, uint32_t seq, const StoredHeader* hdr, const char* message_id);
}
namespace TextMessage { bool isAnyConversationOpen(); }
// 2ed04b6 holds no input during loading (it loads in one pass).
struct BeforeRunHeldInput {
  bool empty() const { return true; }
};
static BeforeRunHeldInput g_heldInput;
