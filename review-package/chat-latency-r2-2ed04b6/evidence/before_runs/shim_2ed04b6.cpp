// BEFORE-RUN SHIM (not production): built from 2ed04b6's existing API.
#include "core/storage_messages.h"
#include "core/text_message.h"
namespace MessageStore {
bool stepConversationIndex(const char* g, const char* c, uint8_t, uint16_t* count, uint16_t* read, uint16_t* total) {
  uint16_t n = loadConversationIndex(g, c);  // 2ed04b6: always one call
  if (count) *count = n;
  if (read) *read = n;
  if (total) *total = n;
  return true;
}
// No sweep-fed index on 2ed04b6: a feed is refused.
bool beginIndexFeed(const char*, const char*, const uint32_t*, uint16_t, bool, bool) { return false; }
void feedIndexRecord(const char*, const char*, uint32_t, const StoredHeader*, const char*) {}
}  // namespace MessageStore
namespace TextMessage {
// The conversations the tests open.
bool isAnyConversationOpen() {
  return isConversationOpen("GROUP1", "EVERYONE") || isConversationOpen("GROUP1", "B0B000000001") ||
         isConversationOpen("GROUP1", "CA201000000001");
}
}  // namespace TextMessage
