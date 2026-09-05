#include "core/number_guessing_friend.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/enigma.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/mqtt_manager.h"
#include "core/notifications.h"
#include "core/number_guessing.h"
#include "core/packet_codec.h"
#include "core/presence.h"
#include "core/settings.h"
#include "core/storage_messages.h"
#include "core/text_message.h"
#include "core/wifi_manager.h"

namespace {

using NumberGuessing::DigitEntryState;

// ---- forward declarations (mutual navigation) ------------------------------
void screenNoFamilyGroups();
void screenGroupSelect();
void screenFriendRecipient();
void screenFriendChat();
void screenAnswer();
void screenFriendResult();
void screenPlayOrCancel();
void screenSetManuallyOrRandom();
void screenManualEntry();
void screenRandomConfirm();
void screenQuickSwitch();

// ---- shared selection state (Recipient -> Chat) ----------------------------
char g_selectedGroupCode[33];
char g_selectedContactKey[MessageStore::kContactKeyLen];

// ---- challenge-creation state (persists across the whole creation flow) ---
char g_challengeGroupCode[33];
char g_challengeContactKey[MessageStore::kContactKeyLen];
uint8_t g_pendingRandomSecret[4];

// =============================================================================
// Local payload: game state + FIFO(50) guess history (Addendum section 12;
// Phase 3 sections 10-13). kMaxLocalPayloadLen is a shared 400-byte cap
// across every message type (Phase 1/2 constant), so a full 255-attempt
// history (1020 bytes) cannot fit; per user confirmation this keeps the
// most-recent attempts before a correct guess (FIFO ring, not the first N)
// while totalAttempts always counts every real attempt made. Trimmed from
// an initial 90 to 50 to fix a DRAM link overflow (each of the 6 static
// FriendGameState/Reassembly instances embeds one of these arrays).
constexpr uint8_t kMaxFriendHistory = 50;
constexpr uint8_t STATE_LOCKED = 0;
constexpr uint8_t STATE_UNLOCKED = 1;

struct FriendEntry {
  uint16_t guessValue;
  uint8_t aCount;
  uint8_t bCount;
};

struct FriendGameState {
  uint8_t state;
  uint8_t secret[4];
  uint16_t totalAttempts;
  uint8_t historyCount;
  uint8_t writeIndex;
  FriendEntry history[kMaxFriendHistory];
};

void writeU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

size_t encodeFriendLocal(const FriendGameState& s, uint8_t* out, size_t outCap) {
  size_t need = 9 + static_cast<size_t>(s.historyCount) * 4;
  if (outCap < need) return 0;
  size_t pos = 0;
  out[pos++] = s.state;
  memcpy(out + pos, s.secret, 4);
  pos += 4;
  writeU16(out + pos, s.totalAttempts);
  pos += 2;
  out[pos++] = s.historyCount;
  out[pos++] = s.writeIndex;
  for (uint8_t i = 0; i < s.historyCount; i++) {
    writeU16(out + pos, s.history[i].guessValue);
    pos += 2;
    out[pos++] = s.history[i].aCount;
    out[pos++] = s.history[i].bCount;
  }
  return pos;
}

bool decodeFriendLocal(const uint8_t* data, uint16_t len, FriendGameState* out) {
  memset(out, 0, sizeof(*out));
  if (data == nullptr || len < 9) return false;
  size_t pos = 0;
  out->state = data[pos++];
  memcpy(out->secret, data + pos, 4);
  pos += 4;
  out->totalAttempts = readU16(data + pos);
  pos += 2;
  out->historyCount = data[pos++];
  out->writeIndex = data[pos++];
  if (out->historyCount > kMaxFriendHistory) out->historyCount = kMaxFriendHistory;
  for (uint8_t i = 0; i < out->historyCount; i++) {
    if (pos + 4 > len) {
      out->historyCount = i;
      break;
    }
    out->history[i].guessValue = readU16(data + pos);
    pos += 2;
    out->history[i].aCount = data[pos++];
    out->history[i].bCount = data[pos++];
  }
  return true;
}

// Maps a chronological display index (0 = oldest kept, historyCount-1 =
// newest) to its physical slot in the ring buffer.
uint8_t chronologicalIndex(const FriendGameState& s, uint8_t i) {
  if (s.historyCount < kMaxFriendHistory) return i;
  return static_cast<uint8_t>((s.writeIndex + i) % kMaxFriendHistory);
}

void appendFriendEntry(FriendGameState* s, uint16_t guessValue, uint8_t a, uint8_t b) {
  s->history[s->writeIndex] = FriendEntry{guessValue, a, b};
  s->writeIndex = static_cast<uint8_t>((s->writeIndex + 1) % kMaxFriendHistory);
  if (s->historyCount < kMaxFriendHistory) s->historyCount++;
  s->totalAttempts++;
}

void generateRandomFriendSecret(uint8_t out[4]) {
  bool used[10] = {false};
  for (int i = 0; i < 4; i++) {
    int d;
    do {
      d = static_cast<int>(random(10));
    } while (used[d]);
    used[d] = true;
    out[i] = static_cast<uint8_t>(d);
  }
}

uint16_t digitsToValue(const uint8_t digits[4]) {
  return static_cast<uint16_t>(digits[0] * 1000 + digits[1] * 100 + digits[2] * 10 + digits[3]);
}

// ---- MSG_TYPE_GAME wire payload: just the 4-digit secret -------------------
size_t encodeChallengePayload(const uint8_t secret[4], uint8_t* out, size_t cap) {
  if (cap < 4) return 0;
  memcpy(out, secret, 4);
  return 4;
}

bool decodeChallengePayload(const uint8_t* data, uint16_t len, uint8_t secret[4]) {
  if (data == nullptr || len < 4) return false;
  memcpy(secret, data, 4);
  return true;
}

// =============================================================================
// PK_GAME_RESULT_CHUNK wire format (Phase 3 section 13). Own small codec,
// same precedent as enigma_keys.cpp, since this isn't a PK_MESSAGE envelope.
// Chunked (not one packet) because the MQTT client's fixed buffer (see
// mqtt_manager.cpp) is sized for ~1KB frames; splitting into small fixed
// chunks of entries keeps every publish comfortably under that.
// =============================================================================
constexpr uint8_t kEntriesPerChunk = 20;
constexpr size_t kChunkFixedSize =
    PacketCodec::kMessageIdLen + PacketCodec::kSenderDeviceIdLen + 1 + 1 + 2 + 1;  // 24+16+1+1+2+1 = 45
constexpr size_t kChunkMaxPayload = kChunkFixedSize + kEntriesPerChunk * 4;

size_t buildChunkPayload(const char* challengeMessageId, const char* answererDeviceId, uint8_t chunkIndex,
                         uint8_t chunkCount, uint16_t totalAttempts, const FriendEntry* entries, uint8_t entryCount,
                         uint8_t* out, size_t outCap) {
  size_t need = kChunkFixedSize + static_cast<size_t>(entryCount) * 4;
  if (outCap < need) return 0;
  size_t pos = 0;
  memset(out + pos, 0, PacketCodec::kMessageIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), challengeMessageId, PacketCodec::kMessageIdLen - 1);
  pos += PacketCodec::kMessageIdLen;
  memset(out + pos, 0, PacketCodec::kSenderDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), answererDeviceId, PacketCodec::kSenderDeviceIdLen - 1);
  pos += PacketCodec::kSenderDeviceIdLen;
  out[pos++] = chunkIndex;
  out[pos++] = chunkCount;
  writeU16(out + pos, totalAttempts);
  pos += 2;
  out[pos++] = entryCount;
  for (uint8_t i = 0; i < entryCount; i++) {
    writeU16(out + pos, entries[i].guessValue);
    pos += 2;
    out[pos++] = entries[i].aCount;
    out[pos++] = entries[i].bCount;
  }
  return pos;
}

bool parseChunkPayload(const uint8_t* data, uint16_t len, char* outMsgId, char* outDeviceId, uint8_t* outIndex,
                       uint8_t* outCount, uint16_t* outTotalAttempts, FriendEntry* outEntries,
                       uint8_t* outEntryCount, uint8_t outEntriesCapacity) {
  if (data == nullptr || len < kChunkFixedSize) return false;
  size_t pos = 0;
  memcpy(outMsgId, data + pos, PacketCodec::kMessageIdLen);
  outMsgId[PacketCodec::kMessageIdLen - 1] = '\0';
  pos += PacketCodec::kMessageIdLen;
  memcpy(outDeviceId, data + pos, PacketCodec::kSenderDeviceIdLen);
  outDeviceId[PacketCodec::kSenderDeviceIdLen - 1] = '\0';
  pos += PacketCodec::kSenderDeviceIdLen;
  *outIndex = data[pos++];
  *outCount = data[pos++];
  *outTotalAttempts = readU16(data + pos);
  pos += 2;
  *outEntryCount = data[pos++];
  if (*outEntryCount > outEntriesCapacity) return false;  // malformed/corrupt: would overflow the caller's buffer
  if (pos + static_cast<size_t>(*outEntryCount) * 4 > len) return false;
  for (uint8_t i = 0; i < *outEntryCount; i++) {
    outEntries[i].guessValue = readU16(data + pos);
    pos += 2;
    outEntries[i].aCount = data[pos++];
    outEntries[i].bCount = data[pos++];
  }
  return true;
}

void sendResultChunks(const char* group_code, const char* challenger_contact_key, const char* challenge_message_id,
                      const FriendGameState& s) {
  if (!MqttManager::isGroupConnected(group_code)) return;  // best-effort only; no retry for the chunk transfer

  uint8_t chunkCount = static_cast<uint8_t>((s.historyCount + kEntriesPerChunk - 1) / kEntriesPerChunk);
  if (chunkCount == 0) chunkCount = 1;

  char topicSuffix[24];
  snprintf(topicSuffix, sizeof(topicSuffix), "msg/%s", challenger_contact_key);

  for (uint8_t c = 0; c < chunkCount; c++) {
    uint8_t startIdx = static_cast<uint8_t>(c * kEntriesPerChunk);
    uint8_t remaining = static_cast<uint8_t>(s.historyCount - startIdx);
    uint8_t cnt = remaining < kEntriesPerChunk ? remaining : kEntriesPerChunk;

    FriendEntry ordered[kEntriesPerChunk];
    for (uint8_t j = 0; j < cnt; j++) {
      ordered[j] = s.history[chronologicalIndex(s, static_cast<uint8_t>(startIdx + j))];
    }

    uint8_t payload[kChunkMaxPayload];
    size_t payloadLen = buildChunkPayload(challenge_message_id, Identity::deviceId(), c, chunkCount, s.totalAttempts,
                                          ordered, cnt, payload, sizeof(payload));
    if (payloadLen == 0) continue;

    uint8_t wireBuf[PacketCodec::kHeaderSize + kChunkMaxPayload];
    if (!PacketCodec::encodeHeader(PacketCodec::PK_GAME_RESULT_CHUNK, static_cast<uint16_t>(payloadLen), wireBuf,
                                   sizeof(wireBuf))) {
      continue;
    }
    memcpy(wireBuf + PacketCodec::kHeaderSize, payload, payloadLen);
    MqttManager::publishBinary(group_code, topicSuffix, wireBuf,
                               static_cast<uint16_t>(PacketCodec::kHeaderSize + payloadLen), false, 1);
  }
}

// ---- Reassembly (receive side): dedup by chunk_index bitmask ---------------
constexpr uint8_t kMaxReassembly = 4;

struct Reassembly {
  bool active;
  char group_code[33];
  char challenge_message_id[PacketCodec::kMessageIdLen];
  char sender_device_id[PacketCodec::kSenderDeviceIdLen];
  uint8_t chunk_count;
  uint32_t receivedMask;
  uint16_t total_attempts;
  uint8_t historyCount;
  FriendEntry history[kMaxFriendHistory];
};

Reassembly g_reassembly[kMaxReassembly];

void resetReassemblySlot(Reassembly* r, const char* group_code, const char* msgId, const char* deviceId,
                         uint8_t chunkCount) {
  r->active = true;
  strncpy(r->group_code, group_code, sizeof(r->group_code) - 1);
  r->group_code[sizeof(r->group_code) - 1] = '\0';
  strncpy(r->challenge_message_id, msgId, sizeof(r->challenge_message_id) - 1);
  r->challenge_message_id[sizeof(r->challenge_message_id) - 1] = '\0';
  strncpy(r->sender_device_id, deviceId, sizeof(r->sender_device_id) - 1);
  r->sender_device_id[sizeof(r->sender_device_id) - 1] = '\0';
  r->chunk_count = chunkCount;
  r->receivedMask = 0;
  r->total_attempts = 0;
  r->historyCount = 0;
}

Reassembly* findOrCreateReassembly(const char* group_code, const char* msgId, const char* deviceId,
                                   uint8_t chunkCount) {
  for (auto& r : g_reassembly) {
    if (r.active && strcmp(r.group_code, group_code) == 0 && strcmp(r.challenge_message_id, msgId) == 0 &&
        strcmp(r.sender_device_id, deviceId) == 0) {
      return &r;
    }
  }
  for (auto& r : g_reassembly) {
    if (!r.active) {
      resetReassemblySlot(&r, group_code, msgId, deviceId, chunkCount);
      return &r;
    }
  }
  // Pool exhausted (very unlikely: 4 concurrent in-flight Friend results) —
  // evict the first slot rather than drop the packet silently.
  resetReassemblySlot(&g_reassembly[0], group_code, msgId, deviceId, chunkCount);
  return &g_reassembly[0];
}

void handleGameResultChunkPacket(const char* group_code, const char* topic, const uint8_t* payload,
                                 size_t payloadLen) {
  (void)topic;
  char challengeMsgId[PacketCodec::kMessageIdLen];
  char senderDeviceId[PacketCodec::kSenderDeviceIdLen];
  uint8_t chunkIndex, chunkCount, entryCount;
  uint16_t totalAttempts;
  FriendEntry entries[kEntriesPerChunk];
  if (!parseChunkPayload(payload, static_cast<uint16_t>(payloadLen), challengeMsgId, senderDeviceId, &chunkIndex,
                         &chunkCount, &totalAttempts, entries, &entryCount, kEntriesPerChunk)) {
    return;
  }
  if (chunkCount == 0 || chunkCount > 32 || chunkIndex >= chunkCount) return;

  Reassembly* r = findOrCreateReassembly(group_code, challengeMsgId, senderDeviceId, chunkCount);
  if (r->receivedMask & (1UL << chunkIndex)) return;  // duplicate chunk

  uint16_t offset = static_cast<uint16_t>(chunkIndex) * kEntriesPerChunk;
  for (uint8_t i = 0; i < entryCount && (offset + i) < kMaxFriendHistory; i++) {
    r->history[offset + i] = entries[i];
  }
  r->receivedMask |= (1UL << chunkIndex);
  r->total_attempts = totalAttempts;
  uint16_t newCount = static_cast<uint16_t>(offset + entryCount);
  if (newCount > r->historyCount) r->historyCount = static_cast<uint8_t>(newCount > kMaxFriendHistory ? kMaxFriendHistory : newCount);

  uint32_t fullMask = (chunkCount >= 32) ? 0xFFFFFFFFUL : ((1UL << chunkCount) - 1UL);
  if ((r->receivedMask & fullMask) != fullMask) return;  // not complete yet

  MessageRef ref;
  if (MessageStore::findMessageById(group_code, senderDeviceId, challengeMsgId, &ref)) {
    StoredMessageView view;
    FriendGameState existing;
    memset(&existing, 0, sizeof(existing));
    if (MessageStore::loadMessage(ref, &view)) decodeFriendLocal(view.localPayload, view.localPayloadLen, &existing);

    FriendGameState updated;
    memset(&updated, 0, sizeof(updated));
    updated.state = STATE_UNLOCKED;
    memcpy(updated.secret, existing.secret, 4);
    updated.totalAttempts = r->total_attempts;
    updated.historyCount = r->historyCount;
    updated.writeIndex = 0;
    memcpy(updated.history, r->history, static_cast<size_t>(updated.historyCount) * sizeof(FriendEntry));

    uint8_t buf[9 + kMaxFriendHistory * 4];
    size_t len = encodeFriendLocal(updated, buf, sizeof(buf));
    if (len > 0) MessageStore::updateTypeLocalPayload(ref, buf, static_cast<uint16_t>(len));

    bool conversationOpen = TextMessage::isConversationOpen(group_code, senderDeviceId);
    Notifications::onMessageArrived(group_code, senderDeviceId, ref,
                                    Notifications::BADGE_TEXT | Notifications::BADGE_ENIGMA, conversationOpen);
  }

  r->active = false;
}

// =============================================================================
// No Family Groups / Group Select / Recipient (one named friend only, no
// Everyone — Phase 3 section 10) — same shape as Text/Enigma's, duplicated
// per established Phase 3 precedent (each mode's recipient list is private
// to its own file; there is no shared list-building utility to reuse).
// =============================================================================
void screenNoFamilyGroups() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) Menu::goBack();
  }
  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(6, 60);
  tft.print("No Family Groups");
  tft.setCursor(6, 76);
  tft.print("Add one in Settings");
}

SettingItem g_groupSelectItems[Settings::kMaxGroups];
char g_groupSelectLabelBuf[Settings::kMaxGroups][21];
ListMenu g_groupSelectListMenu;

void groupSelectTrampoline() {
  uint8_t idx = g_groupSelectListMenu.selectedIndex();
  Settings::FamilyGroup g = Settings::getGroup(idx);
  strncpy(g_selectedGroupCode, g.code, sizeof(g_selectedGroupCode) - 1);
  g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
  Menu::goBack();
  Menu::pushScreen(screenFriendRecipient);
}

void screenGroupSelect() {
  if (Menu::consumeJustEntered()) {
    uint8_t n = Settings::getGroupCount();
    for (uint8_t i = 0; i < n; i++) {
      Settings::FamilyGroup g = Settings::getGroup(i);
      strncpy(g_groupSelectLabelBuf[i], g.name, sizeof(g_groupSelectLabelBuf[i]) - 1);
      g_groupSelectLabelBuf[i][sizeof(g_groupSelectLabelBuf[i]) - 1] = '\0';
      g_groupSelectItems[i] = SettingItem{g_groupSelectLabelBuf[i], groupSelectTrampoline};
    }
    g_groupSelectListMenu.configure(g_groupSelectItems, n);
  }
  Display::drawStatusBar();
  g_groupSelectListMenu.tick("Select Group");
}

constexpr uint8_t kMaxRecipientItems = 40;  // up to 20 online + up to 20 recent-offline (no Everyone)
SettingItem g_recipientItems[kMaxRecipientItems];
char g_recipientLabelBuf[kMaxRecipientItems][40];
char g_recipientContactKeys[kMaxRecipientItems][MessageStore::kContactKeyLen];
uint8_t g_recipientItemCount = 0;
ListMenu g_recipientListMenu;

void appendDeviceSuffix(char* label, size_t labelBufSize, const char* device_id) {
  size_t idLen = strlen(device_id);
  if (idLen < 4) return;
  char suffix[10];
  snprintf(suffix, sizeof(suffix), " [%s]", device_id + idLen - 4);
  size_t len = strlen(label);
  size_t room = (labelBufSize > len) ? (labelBufSize - len - 1) : 0;
  strncat(label, suffix, room);
}

void recipientTrampoline() {
  uint8_t idx = g_recipientListMenu.selectedIndex();
  if (idx < g_recipientItemCount) {
    strncpy(g_selectedContactKey, g_recipientContactKeys[idx], sizeof(g_selectedContactKey) - 1);
    g_selectedContactKey[sizeof(g_selectedContactKey) - 1] = '\0';
  }
  Menu::goBack();
  Menu::pushScreen(screenFriendChat);
}

void screenFriendRecipient() {
  if (Menu::consumeJustEntered()) {
    g_recipientItemCount = 0;

    Presence::OnlineContact online[20];
    uint8_t onlineN = Presence::getOnlineContacts(g_selectedGroupCode, online, 20);
    for (uint8_t i = 0; i < onlineN && g_recipientItemCount < kMaxRecipientItems; i++) {
      strncpy(g_recipientContactKeys[g_recipientItemCount], online[i].device_id,
              sizeof(g_recipientContactKeys[0]) - 1);
      g_recipientContactKeys[g_recipientItemCount][sizeof(g_recipientContactKeys[0]) - 1] = '\0';
      snprintf(g_recipientLabelBuf[g_recipientItemCount], sizeof(g_recipientLabelBuf[0]), "%s",
               online[i].display_name);
      g_recipientItems[g_recipientItemCount] =
          SettingItem{g_recipientLabelBuf[g_recipientItemCount], recipientTrampoline};
      g_recipientItemCount++;
    }

    Presence::RecentContact recent[20];
    uint8_t recentN = Presence::getRecentContacts(g_selectedGroupCode, recent, 20);
    for (uint8_t i = 0; i < recentN && g_recipientItemCount < kMaxRecipientItems; i++) {
      bool alreadyOnline = false;
      for (uint8_t j = 0; j < onlineN; j++) {
        if (strcmp(online[j].device_id, recent[i].device_id) == 0) {
          alreadyOnline = true;
          break;
        }
      }
      if (alreadyOnline) continue;
      strncpy(g_recipientContactKeys[g_recipientItemCount], recent[i].device_id,
              sizeof(g_recipientContactKeys[0]) - 1);
      g_recipientContactKeys[g_recipientItemCount][sizeof(g_recipientContactKeys[0]) - 1] = '\0';
      const char* name = (recent[i].last_known_name[0] != '\0') ? recent[i].last_known_name : recent[i].device_id;
      snprintf(g_recipientLabelBuf[g_recipientItemCount], sizeof(g_recipientLabelBuf[0]), "%s (offline)", name);
      g_recipientItems[g_recipientItemCount] =
          SettingItem{g_recipientLabelBuf[g_recipientItemCount], recipientTrampoline};
      g_recipientItemCount++;
    }

    for (uint8_t i = 0; i < g_recipientItemCount; i++) {
      for (uint8_t j = static_cast<uint8_t>(i + 1); j < g_recipientItemCount; j++) {
        if (strcmp(g_recipientLabelBuf[i], g_recipientLabelBuf[j]) == 0) {
          appendDeviceSuffix(g_recipientLabelBuf[i], sizeof(g_recipientLabelBuf[i]), g_recipientContactKeys[i]);
          appendDeviceSuffix(g_recipientLabelBuf[j], sizeof(g_recipientLabelBuf[j]), g_recipientContactKeys[j]);
        }
      }
    }

    g_recipientListMenu.configure(g_recipientItems, g_recipientItemCount);
  }
  Display::drawStatusBar();
  g_recipientListMenu.tick("Play with Friend");
}

// =============================================================================
// Unified Game Chat (Phase 3 section 10-11): same generic thread as Text/
// Enigma, entered a third way. Its "compose area" is always the empty
// bottom line — Encoder short there starts a new challenge directly (no
// invokeEmptyLineAction round-trip needed; we already own this feature).
// =============================================================================
constexpr uint16_t kNoHistoryCursor = 0xFFFF;
uint16_t g_historyCursor = kNoHistoryCursor;

bool g_indexDirty = true;
uint16_t g_indexTotal = 0;
void markIndexDirty() { g_indexDirty = true; }
void refreshIndexIfNeeded() {
  if (!g_indexDirty) return;
  g_indexTotal = MessageStore::loadConversationIndex(g_selectedGroupCode, g_selectedContactKey);
  g_indexDirty = false;
}

MessageRef refForIndexEntry(const MessageStore::ConversationIndexEntry& entry) {
  MessageRef ref;
  strncpy(ref.group_code, g_selectedGroupCode, sizeof(ref.group_code) - 1);
  ref.group_code[sizeof(ref.group_code) - 1] = '\0';
  strncpy(ref.contact_key, g_selectedContactKey, sizeof(ref.contact_key) - 1);
  ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
  ref.sequence = entry.sequence;
  return ref;
}

void dispatchHistoryMessageEvent(const MessageStore::ConversationIndexEntry& entry, MessageEventType eventType) {
  MessageRef ref = refForIndexEntry(entry);
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  MessageEventFn fn = getMessageEventFn(view.envelope.message_type);
  if (fn != nullptr) fn(ref, eventType);
}

void startChallengeFlow(const char* group_code, const char* contact_key) {
  strncpy(g_challengeGroupCode, group_code, sizeof(g_challengeGroupCode) - 1);
  g_challengeGroupCode[sizeof(g_challengeGroupCode) - 1] = '\0';
  strncpy(g_challengeContactKey, contact_key, sizeof(g_challengeContactKey) - 1);
  g_challengeContactKey[sizeof(g_challengeContactKey) - 1] = '\0';
  Menu::pushScreen(screenPlayOrCancel);
}

void handleFriendChatEvent(const InputEvent& e) {
  if (Input::isBack(e)) {
    TextMessage::clearOpenConversation();
    Menu::goBack();
  } else if (g_historyCursor != kNoHistoryCursor) {
    const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(g_historyCursor);
    if (e.type == InputEventType::ENCODER_SHORT) {
      if (entry != nullptr) dispatchHistoryMessageEvent(*entry, EVT_ENCODER_SHORT);
    }
  } else {
    if (e.type == InputEventType::ENCODER_SHORT) {
      startChallengeFlow(g_selectedGroupCode, g_selectedContactKey);
    }
  }
}

void screenFriendChat() {
  if (Menu::consumeJustEntered()) {
    g_historyCursor = kNoHistoryCursor;
    TextMessage::setOpenConversation(g_selectedGroupCode, g_selectedContactKey);
    markIndexDirty();
  }

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      refreshIndexIfNeeded();
      if (g_historyCursor == kNoHistoryCursor) {
        if (e.value < 0 && g_indexTotal > 0) g_historyCursor = static_cast<uint16_t>(g_indexTotal - 1);
      } else if (e.value < 0) {
        if (g_historyCursor > 0) g_historyCursor--;
      } else {
        if (g_historyCursor + 1 >= g_indexTotal) {
          g_historyCursor = kNoHistoryCursor;
        } else {
          g_historyCursor++;
        }
      }
    } else {
      handleFriendChatEvent(e);
    }
  }

  refreshIndexIfNeeded();

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  constexpr uint16_t kViewportLines = 4;
  uint16_t startIdx = 0;
  if (g_indexTotal > kViewportLines) startIdx = static_cast<uint16_t>(g_indexTotal - kViewportLines);
  if (g_historyCursor != kNoHistoryCursor) {
    if (g_historyCursor < startIdx) {
      startIdx = g_historyCursor;
    } else if (g_historyCursor >= startIdx + kViewportLines) {
      startIdx = static_cast<uint16_t>(g_historyCursor - kViewportLines + 1);
    }
  }

  int16_t y = Display::kStatusBarHeight + 2;
  for (uint16_t i = startIdx; i < g_indexTotal && i < startIdx + kViewportLines; i++) {
    const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(i);
    if (entry == nullptr) continue;
    MessageRef ref = refForIndexEntry(*entry);
    StoredMessageView view;
    char lineBuf[48] = "?";
    if (MessageStore::loadMessage(ref, &view)) {
      RenderFn renderFn = getMessageRenderFn(view.envelope.message_type);
      if (renderFn != nullptr) renderFn(view, lineBuf, sizeof(lineBuf));
    }
    tft.setCursor(2, y);
    tft.print(i == g_historyCursor ? "> " : "  ");
    tft.print(lineBuf);
    y += 10;
  }

  y = Display::kStatusBarHeight + 2 + kViewportLines * 10 + 4;
  tft.setCursor(2, y);
  tft.print(g_historyCursor == kNoHistoryCursor ? "> " : "  ");
  tft.print("(Short: new challenge)");
}

// =============================================================================
// MSG_TYPE_GAME message-type registration
// =============================================================================
void renderGameMessage(const StoredMessageView& msg, char* outBuffer, size_t outBufferSize) {
  FriendGameState s;
  decodeFriendLocal(msg.localPayload, msg.localPayloadLen, &s);
  bool outgoing = msg.header.direction == MessageStore::Direction::SENT;
  if (s.state == STATE_UNLOCKED) {
    snprintf(outBuffer, outBufferSize, "Number Guessing: solved (%u)", s.totalAttempts);
  } else {
    snprintf(outBuffer, outBufferSize, outgoing ? "Number Guessing: waiting..." : "Number Guessing: tap to answer");
  }
}

void startAnswerSession(const MessageRef& ref, const StoredMessageView& view, const FriendGameState& s);
void startResultView(const FriendGameState& s);

void onGameMessageEvent(const MessageRef& ref, MessageEventType eventType) {
  if (eventType != EVT_ENCODER_SHORT) return;
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  FriendGameState s;
  decodeFriendLocal(view.localPayload, view.localPayloadLen, &s);
  bool outgoing = view.header.direction == MessageStore::Direction::SENT;

  if (s.state == STATE_LOCKED) {
    if (outgoing) return;  // outgoing challenge: Encoder short no effect
    startAnswerSession(ref, view, s);
    Menu::pushScreen(screenAnswer);
  } else {
    startResultView(s);
    Menu::pushScreen(screenFriendResult);
  }
}

void handleGameArrival(const char* group_code, const char* contact_key, const PacketCodec::MessageEnvelope& env,
                       uint32_t effectiveTs, const uint8_t* typePayload, uint16_t typePayloadLen) {
  uint8_t secret[4];
  if (!decodeChallengePayload(typePayload, typePayloadLen, secret)) return;

  uint8_t wireBuf[PacketCodec::kHeaderSize + PacketCodec::kEnvelopeFixedSize + 2 + 16];
  size_t wireLen = PacketCodec::encodeMessagePacket(env, typePayload, typePayloadLen, wireBuf, sizeof(wireBuf));
  if (wireLen == 0) return;

  FriendGameState s;
  memset(&s, 0, sizeof(s));
  s.state = STATE_LOCKED;
  memcpy(s.secret, secret, 4);
  uint8_t localBuf[16];
  size_t localLen = encodeFriendLocal(s, localBuf, sizeof(localBuf));

  MessageRef ref;
  bool ok = MessageStore::appendStoredMessage(group_code, contact_key, MessageStore::Direction::RECEIVED,
                                              MessageStore::FLAG_UNREAD, effectiveTs, wireBuf,
                                              static_cast<uint16_t>(wireLen), localBuf,
                                              static_cast<uint16_t>(localLen), &ref);
  if (!ok) return;

  bool conversationOpen = TextMessage::isConversationOpen(group_code, contact_key);
  if (conversationOpen) markIndexDirty();
  Notifications::onMessageArrived(group_code, contact_key, ref,
                                  Notifications::BADGE_TEXT | Notifications::BADGE_ENIGMA, conversationOpen);
}

// =============================================================================
// ANSWER screen (Phase 3 section 12)
// =============================================================================
enum class AnswerPhase : uint8_t { ENTERING, SHOWING_RESULT };
AnswerPhase g_answerPhase = AnswerPhase::ENTERING;
DigitEntryState g_digitEntry;
MessageRef g_activeGameRef;
FriendGameState g_activeGame;
char g_activeChallengeMessageId[PacketCodec::kMessageIdLen];
NumberGuessing::GuessResult g_pendingResult;
uint16_t g_pendingGuessValue = 0;

void startAnswerSession(const MessageRef& ref, const StoredMessageView& view, const FriendGameState& s) {
  g_activeGameRef = ref;
  g_activeGame = s;
  strncpy(g_activeChallengeMessageId, view.envelope.message_id, sizeof(g_activeChallengeMessageId) - 1);
  g_activeChallengeMessageId[sizeof(g_activeChallengeMessageId) - 1] = '\0';
  g_answerPhase = AnswerPhase::ENTERING;
  NumberGuessing::resetDigitEntry(&g_digitEntry);
}

void persistActiveGame() {
  uint8_t buf[9 + kMaxFriendHistory * 4];
  size_t len = encodeFriendLocal(g_activeGame, buf, sizeof(buf));
  if (len > 0) MessageStore::updateTypeLocalPayload(g_activeGameRef, buf, static_cast<uint16_t>(len));
}

void exitAnswerConfirmYes() { Menu::goBack(); }  // pops screenAnswer; confirmPromptScreen pops itself too

void submitAnswerGuess() {
  uint16_t guessValue = digitsToValue(g_digitEntry.digits);
  NumberGuessing::GuessResult r = NumberGuessing::evaluate(g_activeGame.secret, g_digitEntry.digits);
  appendFriendEntry(&g_activeGame, guessValue, r.a, r.b);

  if (r.a == 4) {
    g_activeGame.state = STATE_UNLOCKED;
    persistActiveGame();
    sendResultChunks(g_activeGameRef.group_code, g_activeGameRef.contact_key, g_activeChallengeMessageId,
                     g_activeGame);
    startResultView(g_activeGame);
    Menu::goBack();
    Menu::pushScreen(screenFriendResult);
    return;
  }

  persistActiveGame();
  g_pendingResult = r;
  g_pendingGuessValue = guessValue;
  g_answerPhase = AnswerPhase::SHOWING_RESULT;
}

void screenAnswer() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) {
      Menu::goBack();
      continue;
    }
    if (g_answerPhase == AnswerPhase::SHOWING_RESULT) {
      if (e.type == InputEventType::ENCODER_SHORT) {
        g_answerPhase = AnswerPhase::ENTERING;
        NumberGuessing::resetDigitEntry(&g_digitEntry);
      }
      continue;
    }
    if (e.type == InputEventType::ENCODER_SHORT) {
      if (g_digitEntry.count == 4) {
        submitAnswerGuess();
      } else {
        ConfirmPromptConfig cfg{"Exit this game?", nullptr, false, exitAnswerConfirmYes, nullptr};
        Menu::startConfirmPrompt(cfg);
        Menu::pushScreen(Menu::confirmPromptScreen);
      }
    } else {
      NumberGuessing::handleDigitEntryEvent(&g_digitEntry, e);
    }
  }
  if (g_answerPhase == AnswerPhase::ENTERING) NumberGuessing::tickDigitEntry(&g_digitEntry);

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  char line[32];

  if (g_answerPhase == AnswerPhase::SHOWING_RESULT) {
    snprintf(line, sizeof(line), "%04u -> %uA%uB", g_pendingGuessValue, g_pendingResult.a, g_pendingResult.b);
    tft.setCursor(2, Display::kStatusBarHeight + 2);
    tft.print(line);
    tft.setCursor(2, Display::kStatusBarHeight + 20);
    tft.print("Short: continue");
  } else {
    snprintf(line, sizeof(line), "Attempt %d", g_activeGame.totalAttempts + 1);
    tft.setCursor(2, Display::kStatusBarHeight + 2);
    tft.print(line);

    char guessLine[16] = "____";
    for (uint8_t i = 0; i < g_digitEntry.count; i++) guessLine[i] = static_cast<char>('0' + g_digitEntry.digits[i]);
    if (g_digitEntry.count < 4) guessLine[g_digitEntry.count] = static_cast<char>('0' + g_digitEntry.previewDigit);
    tft.setCursor(2, Display::kStatusBarHeight + 20);
    tft.print("ANSWER: ");
    tft.print(guessLine);
  }
}

// =============================================================================
// Read-only full history (Unlocked messages, and right after solving)
// =============================================================================
FriendGameState g_viewState;
uint16_t g_friendResultScroll = 0;

void startResultView(const FriendGameState& s) { g_viewState = s; }

void screenFriendResult() {
  if (Menu::consumeJustEntered()) {
    g_friendResultScroll = (g_viewState.historyCount > 0) ? static_cast<uint16_t>(g_viewState.historyCount - 1) : 0;
  }
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      int32_t next = static_cast<int32_t>(g_friendResultScroll) + e.value;
      if (next < 0) next = 0;
      if (next >= g_viewState.historyCount) next = g_viewState.historyCount > 0 ? g_viewState.historyCount - 1 : 0;
      g_friendResultScroll = static_cast<uint16_t>(next);
    } else if (Input::isMenuConfirm(e) || Input::isBack(e)) {
      Menu::goBack();
    }
  }

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  char line[32];
  snprintf(line, sizeof(line), "Total attempts: %d", g_viewState.totalAttempts);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print(line);

  constexpr uint16_t kRows = 4;
  uint16_t start = (g_friendResultScroll >= kRows) ? static_cast<uint16_t>(g_friendResultScroll - kRows + 1) : 0;
  int16_t y = Display::kStatusBarHeight + 16;
  for (uint16_t i = start; i < g_viewState.historyCount && i < start + kRows; i++) {
    const FriendEntry& g = g_viewState.history[chronologicalIndex(g_viewState, static_cast<uint8_t>(i))];
    snprintf(line, sizeof(line), "%s%04d %dA%dB", i == g_friendResultScroll ? "> " : "  ", g.guessValue, g.aCount,
             g.bCount);
    tft.setCursor(2, y);
    tft.print(line);
    y += 10;
  }
}

// =============================================================================
// Create new challenge (Phase 3 section 11)
// =============================================================================
void cancelChallengeTrampoline() { Menu::goBack(); }

void sendChallenge(const char* group_code, const char* contact_key, const uint8_t secret[4]) {
  uint8_t typePayload[4];
  size_t typePayloadLen = encodeChallengePayload(secret, typePayload, sizeof(typePayload));

  char messageId[PacketCodec::kMessageIdLen];
  Identity::nextId(messageId, sizeof(messageId));
  PacketCodec::MessageEnvelope env;
  memset(&env, 0, sizeof(env));
  strncpy(env.message_id, messageId, sizeof(env.message_id) - 1);
  env.schema_version = 1;
  env.message_type = PacketCodec::MSG_TYPE_GAME;
  strncpy(env.sender_device_id, Identity::deviceId(), sizeof(env.sender_device_id) - 1);
  strncpy(env.sender_name_cache, Settings::getMyName(), sizeof(env.sender_name_cache) - 1);
  strncpy(env.group_code, group_code, sizeof(env.group_code) - 1);
  env.timestamp = WifiManager::getUnixTime();

  uint8_t wireBuf[PacketCodec::kHeaderSize + PacketCodec::kEnvelopeFixedSize + 2 + 16];
  size_t wireLen = PacketCodec::encodeMessagePacket(env, typePayload, static_cast<uint16_t>(typePayloadLen), wireBuf,
                                                    sizeof(wireBuf));
  if (wireLen == 0) return;

  char topicSuffix[24];
  snprintf(topicSuffix, sizeof(topicSuffix), "msg/%s", contact_key);  // No Everyone for Friend challenges

  bool online = MqttManager::isGroupConnected(group_code);
  bool published =
      online && MqttManager::publishBinary(group_code, topicSuffix, wireBuf, static_cast<uint16_t>(wireLen), false, 1);
  uint16_t flags = published ? 0 : MessageStore::FLAG_PENDING_OUTBOX;

  FriendGameState s;
  memset(&s, 0, sizeof(s));
  s.state = STATE_LOCKED;
  memcpy(s.secret, secret, 4);
  uint8_t localBuf[16];
  size_t localLen = encodeFriendLocal(s, localBuf, sizeof(localBuf));

  MessageRef outRef;
  MessageStore::appendStoredMessage(group_code, contact_key, MessageStore::Direction::SENT, flags, env.timestamp,
                                    wireBuf, static_cast<uint16_t>(wireLen), localBuf,
                                    static_cast<uint16_t>(localLen), &outRef);
}

void playNumberGuessingTrampoline() {
  Menu::goBack();
  Menu::pushScreen(screenSetManuallyOrRandom);
}

const SettingItem kPlayOrCancelItems[] = {
    {"Play Number Guessing", playNumberGuessingTrampoline},
    {"Cancel", cancelChallengeTrampoline},
};
ListMenu g_playOrCancelListMenu;
void screenPlayOrCancel() {
  if (Menu::consumeJustEntered()) g_playOrCancelListMenu.configure(kPlayOrCancelItems, 2);
  Display::drawStatusBar();
  g_playOrCancelListMenu.tick("Number Guessing");
}

void trampolineSetManually() {
  Menu::goBack();
  Menu::pushScreen(screenManualEntry);
}
void trampolineRandomize() {
  Menu::goBack();
  Menu::pushScreen(screenRandomConfirm);
}
const SettingItem kSetManuallyOrRandomItems[] = {
    {"Set Manually", trampolineSetManually},
    {"Randomize", trampolineRandomize},
};
ListMenu g_setManuallyOrRandomListMenu;
void screenSetManuallyOrRandom() {
  if (Menu::consumeJustEntered()) g_setManuallyOrRandomListMenu.configure(kSetManuallyOrRandomItems, 2);
  Display::drawStatusBar();
  g_setManuallyOrRandomListMenu.tick("Number Guessing");
}

void finishChallengeCreation(const uint8_t secret[4]) {
  sendChallenge(g_challengeGroupCode, g_challengeContactKey, secret);
  Menu::goBack();
  Menu::pushScreen(screenQuickSwitch);
}

void screenManualEntry() {
  if (Menu::consumeJustEntered()) NumberGuessing::resetDigitEntry(&g_digitEntry);

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) {
      Menu::goBack();
      continue;
    }
    if (e.type == InputEventType::DOT_RELEASE) {
      bool wasFullNoDelete = (g_digitEntry.count == 4) && !g_digitEntry.deleteFiredThisPress;
      NumberGuessing::handleDigitEntryEvent(&g_digitEntry, e);
      if (wasFullNoDelete) {
        finishChallengeCreation(g_digitEntry.digits);
        return;
      }
    } else {
      NumberGuessing::handleDigitEntryEvent(&g_digitEntry, e);
    }
  }
  NumberGuessing::tickDigitEntry(&g_digitEntry);

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  char guessLine[16] = "____";
  for (uint8_t i = 0; i < g_digitEntry.count; i++) guessLine[i] = static_cast<char>('0' + g_digitEntry.digits[i]);
  if (g_digitEntry.count < 4) guessLine[g_digitEntry.count] = static_cast<char>('0' + g_digitEntry.previewDigit);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print("Set secret: ");
  tft.print(guessLine);
  if (g_digitEntry.count == 4) {
    tft.setCursor(2, Display::kStatusBarHeight + 20);
    tft.print("DOT: send");
  }
}

constexpr uint32_t kDotConfirmMs = 500;  // same short/hold boundary as digit entry (Addendum 9.5)

void screenRandomConfirm() {
  if (Menu::consumeJustEntered()) generateRandomFriendSecret(g_pendingRandomSecret);

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) {
      Menu::goBack();
      continue;
    }
    if (e.type == InputEventType::DOT_RELEASE && e.durationMs < kDotConfirmMs) {
      finishChallengeCreation(g_pendingRandomSecret);
      return;
    }
  }

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print("Random secret ready");
  tft.setCursor(2, Display::kStatusBarHeight + 20);
  tft.print("DOT: send");
}

void quickSwitchText() {
  TextMessage::clearOpenConversation();
  TextMessage::navigateToChatDirect(g_challengeGroupCode, g_challengeContactKey);
}
void quickSwitchEnigma() {
  TextMessage::clearOpenConversation();
  Enigma::navigateToChatDirect(g_challengeGroupCode, g_challengeContactKey);
}
void quickSwitchRadio() {
  TextMessage::clearOpenConversation();
  setPendingRadioIntent(g_challengeGroupCode, g_challengeContactKey);
  Menu::init();
  Menu::pushScreen(comingSoonScreen);
}
void quickSwitchMain() {
  TextMessage::clearOpenConversation();
  Menu::init();
}
const SettingItem kQuickSwitchItems[] = {
    {"Text Message", quickSwitchText},
    {"Enigma Cipher", quickSwitchEnigma},
    {"Radio", quickSwitchRadio},
    {"Main Menu", quickSwitchMain},
};
ListMenu g_quickSwitchListMenu;
void screenQuickSwitch() {
  if (Menu::consumeJustEntered()) g_quickSwitchListMenu.configure(kQuickSwitchItems, 4);
  Display::drawStatusBar();
  g_quickSwitchListMenu.tick("Challenge Sent");
}

// =============================================================================
// Empty compose-line hook (Phase 3 section 14) — single global slot, shared
// by Text and Enigma's own empty-line Encoder-short.
// =============================================================================
void handleEmptyLineAction(uint8_t mode, const char* group_code, const char* contact_key) {
  (void)mode;
  if (strcmp(contact_key, MessageStore::kEveryone) == 0) return;  // Everyone: no effect
  startChallengeFlow(group_code, contact_key);
}

// =============================================================================
// Game sub-mode entry point (Training Game > Number Guessing > Play with
// Friend already routes here via settings.cpp's trampolineFriend)
// =============================================================================
void screenFriendEntry() {
  uint8_t n = Settings::getGroupCount();
  ScreenHandlerFn target;
  if (n == 0) {
    target = screenNoFamilyGroups;
  } else if (n == 1) {
    Settings::FamilyGroup g = Settings::getGroup(0);
    strncpy(g_selectedGroupCode, g.code, sizeof(g_selectedGroupCode) - 1);
    g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
    target = screenFriendRecipient;
  } else {
    target = screenGroupSelect;
  }
  Menu::goBack();
  Menu::pushScreen(target);
}

struct Registrar {
  Registrar() {
    registerGameSubModeHandler(GameSubModes::FRIEND, screenFriendEntry);
    registerMessageType(PacketCodec::MSG_TYPE_GAME, renderGameMessage, onGameMessageEvent);
    TextMessage::registerIncomingMessageHandler(PacketCodec::MSG_TYPE_GAME, handleGameArrival);
    registerNetworkPacketHandler(PacketCodec::PK_GAME_RESULT_CHUNK, handleGameResultChunkPacket);
    registerEmptyLineAction(handleEmptyLineAction);
  }
};
Registrar g_registrar;

}  // namespace
