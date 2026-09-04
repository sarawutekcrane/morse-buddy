#include "core/text_message.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/morse.h"
#include "core/mqtt_manager.h"
#include "core/notifications.h"
#include "core/packet_codec.h"
#include "core/presence.h"
#include "core/settings.h"
#include "core/sleep.h"
#include "core/storage_messages.h"
#include "core/wifi_manager.h"

namespace {

// ---- forward declarations (mutual navigation) ------------------------------
void screenNoFamilyGroups();
void screenGroupSelect();
void screenRecipient();
void screenChat();

// ---- shared selection state -------------------------------------------------
char g_selectedGroupCode[33];
char g_selectedContactKey[MessageStore::kContactKeyLen];

// "Exact conversation currently open" — read by the incoming-message
// handler so a message that arrives while its own Chat is open is saved
// as already-read, no tone/badge (Phase 2 section 13).
bool g_chatIsOpen = false;
char g_chatOpenGroup[33];
char g_chatOpenContact[MessageStore::kContactKeyLen];

bool isChatOpenFor(const char* group_code, const char* contact_key) {
  return g_chatIsOpen && strcmp(g_chatOpenGroup, group_code) == 0 && strcmp(g_chatOpenContact, contact_key) == 0;
}

// ---- canonical raw-Morse builder (shared by compose + TEXT RenderFn) -------
void buildCanonicalRawMorse(const char* text, char* out, size_t outSize) {
  size_t pos = 0;
  out[0] = '\0';
  size_t len = strlen(text);
  for (size_t i = 0; i < len && pos + 1 < outSize; i++) {
    char c = text[i];
    if (c == ' ') {
      out[pos++] = '/';
    } else {
      char pat[Morse::kMaxPatternLength + 1];
      if (Morse::encodeChar(c, pat, sizeof(pat))) {
        size_t patLen = strlen(pat);
        if (pos + patLen < outSize) {
          memcpy(out + pos, pat, patLen);
          pos += patLen;
        }
      }
    }
    if (pos + 1 < outSize) out[pos++] = ' ';
  }
  out[pos] = '\0';
}

// ---- TEXT message type: render + hold-preview event handler ----------------
bool g_holdActive = false;
MessageRef g_heldRef;

bool refsEqual(const MessageRef& a, const MessageRef& b) {
  return strcmp(a.group_code, b.group_code) == 0 && strcmp(a.contact_key, b.contact_key) == 0 &&
         a.sequence == b.sequence;
}

void renderTextMessage(const StoredMessageView& msgIncomplete, char* outBuffer, size_t outBufferSize);
void onTextMessageEvent(const MessageRef& ref, MessageEventType eventType);

// ---- conversation index cache (avoid rescanning LittleFS every frame) -----
bool g_indexDirty = true;
uint16_t g_indexTotal = 0;

void markIndexDirty() { g_indexDirty = true; }

void refreshIndexIfNeeded() {
  if (!g_indexDirty) return;
  g_indexTotal = MessageStore::loadConversationIndex(g_selectedGroupCode, g_selectedContactKey);
  g_indexDirty = false;
}

// =============================================================================
// No Family Groups
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

// =============================================================================
// Group Select (only shown when >1 group configured)
// =============================================================================
SettingItem g_groupSelectItems[Settings::kMaxGroups];
char g_groupSelectLabelBuf[Settings::kMaxGroups][21];
ListMenu g_groupSelectListMenu;

void groupSelectTrampoline() {
  uint8_t idx = g_groupSelectListMenu.selectedIndex();
  Settings::FamilyGroup g = Settings::getGroup(idx);
  strncpy(g_selectedGroupCode, g.code, sizeof(g_selectedGroupCode) - 1);
  g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
  Menu::goBack();
  Menu::pushScreen(screenRecipient);
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

// =============================================================================
// Recipient (Online / Recent Offline / Everyone) — Addendum section 5
// =============================================================================
constexpr uint8_t kMaxRecipientItems = 41;  // Everyone + up to 20 online + up to 20 recent-offline
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
  Menu::pushScreen(screenChat);
}

void screenRecipient() {
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
      g_recipientItems[g_recipientItemCount] = SettingItem{g_recipientLabelBuf[g_recipientItemCount],
                                                            recipientTrampoline};
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
      g_recipientItems[g_recipientItemCount] = SettingItem{g_recipientLabelBuf[g_recipientItemCount],
                                                            recipientTrampoline};
      g_recipientItemCount++;
    }

    // Disambiguate duplicate display names with the last 4 hex of device_id.
    for (uint8_t i = 0; i < g_recipientItemCount; i++) {
      for (uint8_t j = static_cast<uint8_t>(i + 1); j < g_recipientItemCount; j++) {
        if (strcmp(g_recipientLabelBuf[i], g_recipientLabelBuf[j]) == 0) {
          appendDeviceSuffix(g_recipientLabelBuf[i], sizeof(g_recipientLabelBuf[i]), g_recipientContactKeys[i]);
          appendDeviceSuffix(g_recipientLabelBuf[j], sizeof(g_recipientLabelBuf[j]), g_recipientContactKeys[j]);
        }
      }
    }

    if (g_recipientItemCount < kMaxRecipientItems) {
      strncpy(g_recipientContactKeys[g_recipientItemCount], MessageStore::kEveryone,
              sizeof(g_recipientContactKeys[0]) - 1);
      g_recipientContactKeys[g_recipientItemCount][sizeof(g_recipientContactKeys[0]) - 1] = '\0';
      snprintf(g_recipientLabelBuf[g_recipientItemCount], sizeof(g_recipientLabelBuf[0]), "Everyone");
      g_recipientItems[g_recipientItemCount] = SettingItem{g_recipientLabelBuf[g_recipientItemCount],
                                                            recipientTrampoline};
      g_recipientItemCount++;
    }

    g_recipientListMenu.configure(g_recipientItems, g_recipientItemCount);
  }
  Display::drawStatusBar();
  g_recipientListMenu.tick("Recipient");
}

// =============================================================================
// Chat: history viewport + compose line
// =============================================================================
constexpr uint16_t kNoHistoryCursor = 0xFFFF;
uint16_t g_historyCursor = kNoHistoryCursor;

char g_composeText[PacketCodec::kMaxDecodedTextLen + 1];
uint8_t g_composeLen = 0;
char g_composePattern[Morse::kMaxPatternLength + 1];
uint8_t g_composePatternLen = 0;
uint32_t g_lastMorseReleaseMs = 0;

void resetComposePattern() {
  g_composePatternLen = 0;
  g_composePattern[0] = '\0';
}

void clearDraft() {
  g_composeLen = 0;
  g_composeText[0] = '\0';
  resetComposePattern();
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

void sendComposedMessage() {
  char messageId[PacketCodec::kMessageIdLen];
  Identity::nextId(messageId, sizeof(messageId));

  PacketCodec::MessageEnvelope env;
  memset(&env, 0, sizeof(env));
  strncpy(env.message_id, messageId, sizeof(env.message_id) - 1);
  env.schema_version = 1;
  env.message_type = PacketCodec::MSG_TYPE_TEXT;
  strncpy(env.sender_device_id, Identity::deviceId(), sizeof(env.sender_device_id) - 1);
  strncpy(env.sender_name_cache, Settings::getMyName(), sizeof(env.sender_name_cache) - 1);
  strncpy(env.group_code, g_selectedGroupCode, sizeof(env.group_code) - 1);
  env.timestamp = WifiManager::getUnixTime();

  uint8_t textPayload[PacketCodec::kMaxDecodedTextLen + 2];
  size_t textPayloadLen = PacketCodec::encodeTextPayload(g_composeText, textPayload, sizeof(textPayload));
  if (textPayloadLen == 0 && g_composeLen > 0) return;  // encoding failure; leave draft intact

  static uint8_t wireBuf[PacketCodec::kHeaderSize + 400];
  size_t wireLen = PacketCodec::encodeMessagePacket(env, textPayload, static_cast<uint16_t>(textPayloadLen), wireBuf,
                                                    sizeof(wireBuf));
  if (wireLen == 0) return;

  bool isEveryone = strcmp(g_selectedContactKey, MessageStore::kEveryone) == 0;
  char topicSuffix[24];
  if (isEveryone) {
    snprintf(topicSuffix, sizeof(topicSuffix), "broadcast");
  } else {
    snprintf(topicSuffix, sizeof(topicSuffix), "msg/%s", g_selectedContactKey);
  }

  bool online = MqttManager::isGroupConnected(g_selectedGroupCode);
  bool published = false;
  if (online) {
    published = MqttManager::publishBinary(g_selectedGroupCode, topicSuffix, wireBuf, static_cast<uint16_t>(wireLen),
                                           false, 1);
  }
  uint16_t flags = published ? 0 : MessageStore::FLAG_PENDING_OUTBOX;

  MessageRef outRef;
  MessageStore::appendStoredMessage(g_selectedGroupCode, g_selectedContactKey, MessageStore::Direction::SENT, flags,
                                    env.timestamp, wireBuf, static_cast<uint16_t>(wireLen), nullptr, 0, &outRef);

  clearDraft();
  markIndexDirty();
}

void finalizeComposeChar() {
  char c = Morse::decodePattern(g_composePattern);
  if (g_composeLen < PacketCodec::kMaxDecodedTextLen) {
    g_composeText[g_composeLen++] = c;
    g_composeText[g_composeLen] = '\0';
  }
  resetComposePattern();
}

void buildComposeDisplay(char* out, size_t outSize) {
  Settings::TypingDisplay mode = Settings::getTypingDisplay();
  if (mode == Settings::TypingDisplay::LETTERS_ONLY) {
    strncpy(out, g_composeText, outSize - 1);
    out[outSize - 1] = '\0';
  } else if (mode == Settings::TypingDisplay::MIXED) {
    snprintf(out, outSize, "%s%s%s", g_composeText, (g_composePatternLen > 0 ? " " : ""), g_composePattern);
  } else {
    char raw[64];
    buildCanonicalRawMorse(g_composeText, raw, sizeof(raw));
    snprintf(out, outSize, "%s%s", raw, g_composePattern);
  }
}

void handleComposeEvent(const InputEvent& e) {
  if (e.type == InputEventType::DOT_RELEASE) {
    if (e.durationMs >= Morse::kSpecialCommandMs) {
      clearDraft();  // DOT/DASH >=2000ms on compose: clear full draft
      return;
    }
    Morse::SymbolClass sc = Morse::classifyPress(e.durationMs, Settings::getWpm());
    if (g_composePatternLen < Morse::kMaxPatternLength) {
      g_composePattern[g_composePatternLen++] = (sc == Morse::SymbolClass::DOT) ? '.' : '-';
      g_composePattern[g_composePatternLen] = '\0';
    }
    g_lastMorseReleaseMs = millis();

    if (Morse::isDeletePattern(g_composePattern)) {
      if (g_composeLen > 0) {
        g_composeLen--;
        g_composeText[g_composeLen] = '\0';
      }
      resetComposePattern();
    }
  } else if (e.type == InputEventType::ENCODER_SHORT) {
    if (g_composeLen > 0) {
      sendComposedMessage();
    } else {
      invokeEmptyLineAction(Modes::TEXT, g_selectedGroupCode, g_selectedContactKey);
    }
  } else if (e.type == InputEventType::ENCODER_LONG) {
    g_chatIsOpen = false;
    Menu::goBack();
  }
}

void handleHistoryFocusEvent(const InputEvent& e) {
  const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(g_historyCursor);
  if (e.type == InputEventType::DOT_PRESS_START) {
    if (entry != nullptr) {
      MessageRef ref = refForIndexEntry(*entry);
      StoredMessageView view;
      if (MessageStore::loadMessage(ref, &view)) {
        MessageEventFn fn = getMessageEventFn(view.envelope.message_type);
        if (fn != nullptr) fn(ref, EVT_DOT_HOLD_START);
      }
    }
  } else if (e.type == InputEventType::DOT_RELEASE) {
    if (entry != nullptr) {
      MessageRef ref = refForIndexEntry(*entry);
      StoredMessageView view;
      if (MessageStore::loadMessage(ref, &view)) {
        MessageEventFn fn = getMessageEventFn(view.envelope.message_type);
        if (fn != nullptr) fn(ref, EVT_DOT_HOLD_END);
      }
      if (view.header.flags & MessageStore::FLAG_UNREAD) {
        Notifications::clearUnread(g_selectedGroupCode, g_selectedContactKey, ref);
      }
    }
  } else if (e.type == InputEventType::ENCODER_LONG) {
    g_chatIsOpen = false;
    Menu::goBack();
  }
}

void screenChat() {
  if (Menu::consumeJustEntered()) {
    clearDraft();
    g_historyCursor = kNoHistoryCursor;
    strncpy(g_chatOpenGroup, g_selectedGroupCode, sizeof(g_chatOpenGroup) - 1);
    g_chatOpenGroup[sizeof(g_chatOpenGroup) - 1] = '\0';
    strncpy(g_chatOpenContact, g_selectedContactKey, sizeof(g_chatOpenContact) - 1);
    g_chatOpenContact[sizeof(g_chatOpenContact) - 1] = '\0';
    g_chatIsOpen = true;
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
    } else if (g_historyCursor != kNoHistoryCursor) {
      handleHistoryFocusEvent(e);
    } else {
      handleComposeEvent(e);
    }
  }

  if (g_historyCursor == kNoHistoryCursor && g_composePatternLen > 0) {
    if (millis() - g_lastMorseReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeComposeChar();
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
  char composeLine[64];
  buildComposeDisplay(composeLine, sizeof(composeLine));
  tft.setCursor(2, y);
  tft.print(g_historyCursor == kNoHistoryCursor ? "> " : "  ");
  tft.print(composeLine);
}

// =============================================================================
// TEXT message type registration
// =============================================================================
void renderTextMessage(const StoredMessageView& msg, char* outBuffer, size_t outBufferSize) {
  char decoded[PacketCodec::kMaxDecodedTextLen + 1];
  if (!PacketCodec::decodeTextPayload(msg.typePayload, msg.typePayloadLen, decoded, sizeof(decoded))) {
    snprintf(outBuffer, outBufferSize, "?");
    return;
  }
  if (g_holdActive && refsEqual(msg.ref, g_heldRef)) {
    strncpy(outBuffer, decoded, outBufferSize - 1);
    outBuffer[outBufferSize - 1] = '\0';
    return;
  }
  buildCanonicalRawMorse(decoded, outBuffer, outBufferSize);
}

void onTextMessageEvent(const MessageRef& ref, MessageEventType eventType) {
  if (eventType == EVT_DOT_HOLD_START) {
    g_holdActive = true;
    g_heldRef = ref;
  } else if (eventType == EVT_DOT_HOLD_END) {
    g_holdActive = false;
  }
}

// =============================================================================
// Incoming PK_MESSAGE handler
// =============================================================================
void handleIncomingMessagePacket(const char* group_code, const char* topic, const uint8_t* payload,
                                 size_t payloadLen) {
  PacketCodec::MessageEnvelope env;
  const uint8_t* typePayload = nullptr;
  uint16_t typePayloadLen = 0;
  if (!PacketCodec::decodeMessageEnvelope(payload, payloadLen, &env, &typePayload, &typePayloadLen)) return;
  if (env.message_type != PacketCodec::MSG_TYPE_TEXT) return;  // not ours (yet) — ignored safely

  bool isBroadcast = (strcmp(topic, "broadcast") == 0);
  const char* contact_key = isBroadcast ? MessageStore::kEveryone : env.sender_device_id;

  if (MessageStore::isDuplicateAndRecord(group_code, contact_key, env.message_id)) return;

  uint32_t effectiveTs = env.timestamp;
  if (effectiveTs == 0 && WifiManager::isNtpSynced()) effectiveTs = WifiManager::getUnixTime();

  static uint8_t wireBuf[PacketCodec::kHeaderSize + 400];
  size_t wireLen = PacketCodec::encodeMessagePacket(env, typePayload, typePayloadLen, wireBuf, sizeof(wireBuf));
  if (wireLen == 0) return;

  MessageRef ref;
  bool ok = MessageStore::appendStoredMessage(group_code, contact_key, MessageStore::Direction::RECEIVED,
                                              MessageStore::FLAG_UNREAD, effectiveTs, wireBuf,
                                              static_cast<uint16_t>(wireLen), nullptr, 0, &ref);
  if (!ok) return;  // Storage Full; drop silently (nothing else to do here)

  bool conversationOpen = isChatOpenFor(group_code, contact_key);
  if (conversationOpen) markIndexDirty();
  Notifications::onMessageArrived(group_code, contact_key, ref, Notifications::BADGE_TEXT, conversationOpen);
}

// =============================================================================
// Mode handler entry point
// =============================================================================
void screenTextEntry() {
  uint8_t n = Settings::getGroupCount();
  ScreenHandlerFn target;
  if (n == 0) {
    target = screenNoFamilyGroups;
  } else if (n == 1) {
    Settings::FamilyGroup g = Settings::getGroup(0);
    strncpy(g_selectedGroupCode, g.code, sizeof(g_selectedGroupCode) - 1);
    g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
    target = screenRecipient;
  } else {
    target = screenGroupSelect;
  }
  Menu::goBack();
  Menu::pushScreen(target);
}

struct Registrar {
  Registrar() {
    registerModeHandler(Modes::TEXT, screenTextEntry);
    registerMessageType(PacketCodec::MSG_TYPE_TEXT, renderTextMessage, onTextMessageEvent);
    registerNetworkPacketHandler(PacketCodec::PK_MESSAGE, handleIncomingMessagePacket);
  }
};
Registrar g_registrar;

}  // namespace
