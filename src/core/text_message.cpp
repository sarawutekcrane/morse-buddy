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

void setOpenConversationImpl(const char* group_code, const char* contact_key) {
  strncpy(g_chatOpenGroup, group_code, sizeof(g_chatOpenGroup) - 1);
  g_chatOpenGroup[sizeof(g_chatOpenGroup) - 1] = '\0';
  strncpy(g_chatOpenContact, contact_key, sizeof(g_chatOpenContact) - 1);
  g_chatOpenContact[sizeof(g_chatOpenContact) - 1] = '\0';
  g_chatIsOpen = true;
}

void clearOpenConversationImpl() { g_chatIsOpen = false; }

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
  // Captured before Input::isBack() can call Menu::goBack() and reassign
  // the flag to whichever screen becomes newly on top (established pattern
  // -- see comingSoonScreen in menu.cpp).
  bool justEntered = Menu::consumeJustEntered();

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) Menu::goBack();
  }

  Display::drawStatusBar();
  if (!justEntered) return;

  Display::setFont(Display::Font::PRIMARY);
  Display::clearContentArea();
  int16_t lh = Display::lineHeight();
  Display::printLine(6, 60, "No Family Groups");
  Display::printLine(6, 60 + lh, "Add one in Settings");
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
Morse::WordGapState g_composeWordGap;
// True while the pattern currently being keyed (g_composePattern) is known
// to start a new word -- captured once, at that pattern's FIRST symbol
// (see captureWordBoundaryOnSymbolStart()), and left untouched by symbol
// 2/3/... of the same pattern. Only consumed -- as an actual ASCII space,
// committed immediately before the decoded character -- by
// finalizeComposeChar()'s NORMAL letter finalization; a delete prosign or
// a special-command clear discard it without ever writing a space
// (Hardware Fix #4.2).
bool g_currentPatternStartsNewWord = false;

void resetComposePattern() {
  g_composePatternLen = 0;
  g_composePattern[0] = '\0';
}

void clearDraft() {
  g_composeLen = 0;
  g_composeText[0] = '\0';
  resetComposePattern();
  g_currentPatternStartsNewWord = false;
  Morse::cancelWordGap(&g_composeWordGap);
}

// Called on every DOT_PRESS_START, before the new symbol is accepted into
// the pattern buffer (Hardware Fix #4.2). Only captures whether the
// pattern now starting is a new word -- and only at that pattern's FIRST
// symbol (g_composePatternLen == 0); symbol 2/3/... of a multi-symbol
// character (e.g. W = .--) must never re-resolve or overwrite this flag,
// or the boundary decision would be lost partway through composing the
// letter. Never mutates compose text itself -- see finalizeComposeChar().
void captureWordBoundaryOnSymbolStart() {
  if (g_composePatternLen != 0) return;
  g_currentPatternStartsNewWord =
      Morse::consumeWordBoundaryOnSymbolStart(&g_composeWordGap, Settings::getWpm(), millis());
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

// Draws a history row's optional per-type status icon (currently only
// Enigma's lock state) in its fixed kLockIconCellWidth cell before the
// sender name -- shape AND color both carry the state (Hardware Fix #4
// issue 5); a message viewed from a different type's own Unified Thread
// screen still shows its correct icon since this maps the same
// MessageIconKind the owning type registered.
void drawRowIcon(int16_t x, int16_t y, MessageIconKind icon) {
  switch (icon) {
    case MessageIconKind::LOCK_CLOSED_RED:
      Display::drawLockIcon(x, y, false, ST77XX_RED);
      break;
    case MessageIconKind::LOCK_CLOSED_YELLOW:
      Display::drawLockIcon(x, y, false, ST77XX_YELLOW);
      break;
    case MessageIconKind::LOCK_OPEN_GREEN:
      Display::drawLockIcon(x, y, true, ST77XX_GREEN);
      break;
    case MessageIconKind::NONE:
      break;
  }
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
  // Hardware Fix #4.3 issue F: only embed a real, user-chosen name. env is
  // already zeroed above, so leaving this unset when no name has been
  // configured yet keeps sender_name_cache empty, letting
  // MessageStore::buildSenderPrefix()'s existing fallback show the device
  // id instead of the compiled "Me" placeholder leaking out as if it were
  // this device's actual chosen name.
  if (Settings::hasCustomMyName()) {
    strncpy(env.sender_name_cache, Settings::getMyName(), sizeof(env.sender_name_cache) - 1);
  }
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
  // NORMAL character finalization is the only place a word separator is
  // ever actually committed (Hardware Fix #4.2) -- and only as one atomic
  // write together with the character it separates, so a boundary is
  // never left as a trailing space with no room for the letter that was
  // supposed to follow it.
  bool needsSpace = g_currentPatternStartsNewWord && g_composeLen > 0 && g_composeText[g_composeLen - 1] != ' ';
  size_t needed = needsSpace ? 2 : 1;
  if (g_composeLen + needed <= PacketCodec::kMaxDecodedTextLen) {
    if (needsSpace) g_composeText[g_composeLen++] = ' ';
    g_composeText[g_composeLen++] = c;
    g_composeText[g_composeLen] = '\0';
  } else if (g_composeLen < PacketCodec::kMaxDecodedTextLen) {
    g_composeText[g_composeLen++] = c;
    g_composeText[g_composeLen] = '\0';
  }
  g_currentPatternStartsNewWord = false;
  resetComposePattern();
  // Word-gap timer starts from the release of the symbol that just
  // completed this letter (g_lastMorseReleaseMs), not from now.
  Morse::armWordGap(&g_composeWordGap, g_lastMorseReleaseMs);
}

// Splits the compose line into a "prefix" (derived solely from confirmed
// g_composeText, so it is provably unchanged while a Morse pattern is being
// keyed in) and a "suffix" (the in-progress g_composePattern, the only part
// that changes on every dot/dash). In LETTERS_ONLY mode the pattern isn't
// shown at all, so the suffix is always empty. Callers use this split to
// avoid repainting confirmed compose text on every symbol (Hardware Fix #3
// corrective item 2).
void buildComposePrefixSuffix(char* prefix, size_t prefixSize, char* suffix, size_t suffixSize) {
  Settings::TypingDisplay mode = Settings::getTypingDisplay();
  if (mode == Settings::TypingDisplay::LETTERS_ONLY) {
    strncpy(prefix, g_composeText, prefixSize - 1);
    prefix[prefixSize - 1] = '\0';
    suffix[0] = '\0';
  } else if (mode == Settings::TypingDisplay::MIXED) {
    strncpy(prefix, g_composeText, prefixSize - 1);
    prefix[prefixSize - 1] = '\0';
    snprintf(suffix, suffixSize, "%s%s", (g_composePatternLen > 0 ? " " : ""), g_composePattern);
  } else {
    buildCanonicalRawMorse(g_composeText, prefix, prefixSize);
    strncpy(suffix, g_composePattern, suffixSize - 1);
    suffix[suffixSize - 1] = '\0';
  }
}

void handleComposeEvent(const InputEvent& e) {
  if (e.type == InputEventType::DOT_PRESS_START) {
    // Only captures whether this new pattern starts a new word (Hardware
    // Fix #4.2) -- never mutates compose text itself. The space (if any)
    // is committed later, only at NORMAL letter finalization; a delete
    // prosign discards the captured flag below instead of consuming it as
    // a space.
    captureWordBoundaryOnSymbolStart();
  } else if (e.type == InputEventType::DOT_RELEASE) {
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
      // A delete prosign never commits a word separator -- even if it was
      // keyed right after a >=7 dit pause -- it must remove the previous
      // REAL confirmed character, not an auto-inserted space that was
      // never actually written to compose text (Hardware Fix #4.2).
      g_currentPatternStartsNewWord = false;
      if (g_composeLen > 0) {
        g_composeLen--;
        g_composeText[g_composeLen] = '\0';
      }
      resetComposePattern();
      Morse::cancelWordGap(&g_composeWordGap);
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

// Dispatches one MessageEventType to whichever type owns the currently
// focused history entry (TEXT/ENIGMA/GAME) — the generic mechanism Phase 2
// built EVT_ENCODER_SHORT/EVT_COMBINED_REVEAL for, so a Game challenge or
// Enigma message viewed from Text's own Chat (same Unified Thread) still
// gets its type-specific Encoder/reveal behavior without this file being
// restructured per phase.
void dispatchHistoryMessageEvent(const MessageStore::ConversationIndexEntry& entry, MessageEventType eventType) {
  MessageRef ref = refForIndexEntry(entry);
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  MessageEventFn fn = getMessageEventFn(view.envelope.message_type);
  if (fn != nullptr) fn(ref, eventType);
}

void handleHistoryFocusEvent(const InputEvent& e) {
  const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(g_historyCursor);
  if (e.type == InputEventType::DOT_PRESS_START) {
    if (entry != nullptr) dispatchHistoryMessageEvent(*entry, EVT_DOT_HOLD_START);
  } else if (e.type == InputEventType::DOT_RELEASE) {
    if (entry != nullptr) {
      dispatchHistoryMessageEvent(*entry, EVT_DOT_HOLD_END);
      MessageRef ref = refForIndexEntry(*entry);
      StoredMessageView view;
      if (MessageStore::loadMessage(ref, &view) && (view.header.flags & MessageStore::FLAG_UNREAD)) {
        Notifications::clearUnread(g_selectedGroupCode, g_selectedContactKey, ref);
      }
    }
  } else if (e.type == InputEventType::ENCODER_SHORT) {
    if (entry != nullptr) dispatchHistoryMessageEvent(*entry, EVT_ENCODER_SHORT);
  } else if (e.type == InputEventType::COMBINED_REVEAL) {
    if (entry != nullptr) dispatchHistoryMessageEvent(*entry, EVT_COMBINED_REVEAL);
  } else if (e.type == InputEventType::ENCODER_LONG) {
    g_chatIsOpen = false;
    Menu::goBack();
  }
}

bool g_chatRenderDirty = true;

// Hardware Fix #3: same three-way redraw split as ListMenu, plus a
// separate wasIndexDirty-driven "content changed" trigger (an actual
// new/changed message must redraw the history row region even without a
// scroll), and the compose row diffed completely independently. Stored
// message history is never touched by a plain cursor move or by compose
// activity (Morse pattern growth, typed characters) -- only by a genuine
// content or viewport change.
bool g_chatNeedsFullRedraw = true;
int16_t g_chatLastStartIdx = -1;
uint16_t g_chatLastCursor = kNoHistoryCursor;
char g_chatLastComposePrefix[64] = {0};
char g_chatLastComposeSuffix[Morse::kMaxPatternLength + 2] = {0};
bool g_chatLastComposeFocused = true;

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
    g_chatRenderDirty = true;
    g_chatNeedsFullRedraw = true;
  }

  Input::update();
  InputEvent e;
  bool hadEvent = false;
  while (Input::popEvent(e)) {
    hadEvent = true;
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
  if (hadEvent) g_chatRenderDirty = true;

  // Word-boundary resolution is no longer a per-tick check (Hardware Fix
  // #4.1): it only happens at the next DOT_PRESS_START, in
  // handleComposeEvent() above, so idling past 7 dit before pressing Send
  // never mutates compose text on its own.
  if (g_historyCursor == kNoHistoryCursor && g_composePatternLen > 0) {
    if (millis() - g_lastMorseReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeComposeChar();
      g_chatRenderDirty = true;
    }
  }

  bool wasIndexDirty = g_indexDirty;
  refreshIndexIfNeeded();
  if (wasIndexDirty) g_chatRenderDirty = true;  // index actually reloaded: content may have changed

  Display::drawStatusBar();
  if (!g_chatRenderDirty) return;
  g_chatRenderDirty = false;

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();

  // As many history rows as fit, with the last row always reserved for the
  // compose line -- mirrors the original fixed-4-row layout, just computed
  // from the active font's real line height instead of a hardcoded 10px
  // (Hardware Fix #1: PRIMARY's larger line height means fewer rows fit).
  int16_t contentTop = Display::kStatusBarHeight + 2;
  int16_t contentHeight = Display::kScreenHeight - contentTop;
  uint16_t totalLines = (contentHeight > 0) ? static_cast<uint16_t>(contentHeight / lh) : 0;
  if (totalLines < 2) totalLines = 2;
  uint16_t viewportLines = static_cast<uint16_t>(totalLines - 1);

  uint16_t startIdx = 0;
  if (g_indexTotal > viewportLines) startIdx = static_cast<uint16_t>(g_indexTotal - viewportLines);
  if (g_historyCursor != kNoHistoryCursor) {
    if (g_historyCursor < startIdx) {
      startIdx = g_historyCursor;
    } else if (g_historyCursor >= startIdx + viewportLines) {
      startIdx = static_cast<uint16_t>(g_historyCursor - viewportLines + 1);
    }
  }
  int16_t composeY = static_cast<int16_t>(contentTop + viewportLines * lh);

  bool firstDraw = g_chatNeedsFullRedraw;
  bool contentChanged = !firstDraw && wasIndexDirty;
  bool scrolled = !firstDraw && !contentChanged && (static_cast<int16_t>(startIdx) != g_chatLastStartIdx);
  bool selectionOnlyChanged = !firstDraw && !contentChanged && !scrolled && (g_historyCursor != g_chatLastCursor);

  int16_t markerW = static_cast<int16_t>(Display::textWidth(">") + 4);
  int16_t labelX = static_cast<int16_t>(2 + markerW);

  if (firstDraw || contentChanged || scrolled) {
    if (firstDraw) {
      Display::clearContentArea();
    } else {
      int16_t regionH = static_cast<int16_t>(composeY - contentTop);
      if (regionH < 0) regionH = 0;
      Display::tft().fillRect(0, contentTop, Display::kScreenWidth, regionH, ST77XX_BLACK);
    }
    int16_t y = contentTop;
    for (uint16_t i = startIdx; i < g_indexTotal && i < startIdx + viewportLines; i++) {
      const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(i);
      if (entry == nullptr) continue;
      MessageRef ref = refForIndexEntry(*entry);
      StoredMessageView view;
      char lineBuf[48] = "?";
      char senderPrefix[24] = {0};
      MessageIconKind icon = MessageIconKind::NONE;
      if (MessageStore::loadMessage(ref, &view)) {
        RenderFn renderFn = getMessageRenderFn(view.envelope.message_type);
        if (renderFn != nullptr) renderFn(view, lineBuf, sizeof(lineBuf));
        MessageStore::buildSenderPrefix(view.envelope, senderPrefix, sizeof(senderPrefix));
        MessageIconFn iconFn = getMessageIconFn(view.envelope.message_type);
        if (iconFn != nullptr) icon = iconFn(view);
      }
      if (i == g_historyCursor) Display::printLine(2, y, ">");
      // The icon cell is reserved on every row (whether or not that row's
      // type actually has an icon) so sender names stay X-aligned even in
      // a thread that mixes Enigma rows (icon) with Text/Game rows (none)
      // -- consistent with Enigma's own "same X regardless of state" rule.
      drawRowIcon(labelX, y, icon);
      int16_t textX = static_cast<int16_t>(labelX + Display::kLockIconCellWidth);
      Display::printLine(textX, y, senderPrefix);
      Display::printLine(static_cast<int16_t>(textX + Display::textWidth(senderPrefix)), y, lineBuf);
      y += lh;
    }
    g_chatNeedsFullRedraw = false;
  } else if (selectionOnlyChanged) {
    if (g_chatLastCursor != kNoHistoryCursor) {
      int16_t oldY = static_cast<int16_t>(contentTop + (g_chatLastCursor - startIdx) * lh);
      Display::tft().fillRect(2, oldY, markerW, lh, ST77XX_BLACK);
    }
    if (g_historyCursor != kNoHistoryCursor) {
      int16_t newY = static_cast<int16_t>(contentTop + (g_historyCursor - startIdx) * lh);
      Display::tft().fillRect(2, newY, markerW, lh, ST77XX_BLACK);
      Display::printLine(2, newY, ">");
    }
  }

  // Compose row is diffed completely independently of the history rows
  // above it -- Morse pattern growth, typed characters, and the cursor-
  // focus marker flip never repaint stored message history (Hardware Fix
  // #3, Section F). The row is further split into a fixed-width focus
  // marker, a stable confirmed-text prefix, and a dynamic Morse-pattern
  // suffix, so an in-progress dot/dash never repaints already-confirmed
  // compose text (corrective item 2).
  char composePrefix[64];
  char composeSuffix[Morse::kMaxPatternLength + 2];
  buildComposePrefixSuffix(composePrefix, sizeof(composePrefix), composeSuffix, sizeof(composeSuffix));
  bool composeFocused = (g_historyCursor == kNoHistoryCursor);

  int16_t composeMarkerW = static_cast<int16_t>(Display::textWidth(">") + 4);
  int16_t composePrefixX = static_cast<int16_t>(2 + composeMarkerW);
  int16_t composePrefixW = Display::textWidth(composePrefix);
  int16_t composeSuffixX = static_cast<int16_t>(composePrefixX + composePrefixW);

  bool composeFocusChanged = (composeFocused != g_chatLastComposeFocused);
  bool composePrefixChanged = strcmp(composePrefix, g_chatLastComposePrefix) != 0;
  bool composeSuffixChanged = strcmp(composeSuffix, g_chatLastComposeSuffix) != 0;

  if (firstDraw || composePrefixChanged) {
    // The confirmed prefix changed (or this is the first draw): layout may
    // have shifted, so redraw marker + prefix + suffix together -- still
    // only the compose row, never history.
    if (!firstDraw) Display::tft().fillRect(0, composeY, Display::kScreenWidth, lh, ST77XX_BLACK);
    Display::printLine(2, composeY, composeFocused ? ">" : " ");
    Display::printLine(composePrefixX, composeY, composePrefix);
    if (composeSuffix[0] != '\0') Display::printLine(composeSuffixX, composeY, composeSuffix);
  } else if (composeFocusChanged) {
    // Marker only -- confirmed prefix and suffix are untouched.
    Display::tft().fillRect(2, composeY, composeMarkerW, lh, ST77XX_BLACK);
    Display::printLine(2, composeY, composeFocused ? ">" : " ");
  } else if (composeSuffixChanged) {
    // The hot path this fix targets: only the Morse-pattern suffix changed.
    // The confirmed prefix is provably unchanged and at the same X, so it
    // is never touched -- only the suffix's own cell is erased/redrawn.
    int16_t oldSuffixW = Display::textWidth(g_chatLastComposeSuffix);
    int16_t newSuffixW = Display::textWidth(composeSuffix);
    int16_t eraseW = static_cast<int16_t>((oldSuffixW > newSuffixW ? oldSuffixW : newSuffixW) + 4);
    int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - composeSuffixX);
    if (eraseW > maxW) eraseW = maxW;
    if (eraseW < 0) eraseW = 0;
    Display::tft().fillRect(composeSuffixX, composeY, eraseW, lh, ST77XX_BLACK);
    if (composeSuffix[0] != '\0') Display::printLine(composeSuffixX, composeY, composeSuffix);
  }

  strncpy(g_chatLastComposePrefix, composePrefix, sizeof(g_chatLastComposePrefix) - 1);
  g_chatLastComposePrefix[sizeof(g_chatLastComposePrefix) - 1] = '\0';
  strncpy(g_chatLastComposeSuffix, composeSuffix, sizeof(g_chatLastComposeSuffix) - 1);
  g_chatLastComposeSuffix[sizeof(g_chatLastComposeSuffix) - 1] = '\0';
  g_chatLastComposeFocused = composeFocused;

  g_chatLastStartIdx = static_cast<int16_t>(startIdx);
  g_chatLastCursor = g_historyCursor;
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
// Incoming PK_MESSAGE handler: decodes the envelope once, runs the shared
// dedup + timestamp-fallback logic once, then dispatches by
// envelope.message_type to whichever handler that type registered (see
// text_message.h — the registry only allows one PK_MESSAGE claim, so this
// is the fan-out underneath it).
// =============================================================================
constexpr uint8_t kMaxIncomingHandlers = 4;
struct IncomingHandlerEntry {
  bool used;
  uint8_t messageType;
  TextMessage::IncomingMessageHandlerFn fn;
};
IncomingHandlerEntry g_incomingHandlers[kMaxIncomingHandlers];

void handleTextArrival(const char* group_code, const char* contact_key, const PacketCodec::MessageEnvelope& env,
                       uint32_t effectiveTs, const uint8_t* typePayload, uint16_t typePayloadLen) {
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

void handleIncomingMessagePacket(const char* group_code, const char* topic, const uint8_t* payload,
                                 size_t payloadLen) {
  PacketCodec::MessageEnvelope env;
  const uint8_t* typePayload = nullptr;
  uint16_t typePayloadLen = 0;
  if (!PacketCodec::decodeMessageEnvelope(payload, payloadLen, &env, &typePayload, &typePayloadLen)) return;

  // Hardware Fix #4 issue 4: the MQTT broker can echo our own broadcast
  // back to us; our outgoing copy is already stored locally (Direction::
  // SENT, correct lock state for Enigma), so accepting this envelope too
  // would create a duplicate, wrongly-stateful RECEIVED record for the
  // same message. Rejected centrally here -- before the dedup ring or any
  // per-type handler (Text/Enigma/Game all funnel through this one
  // PK_MESSAGE dispatcher) -- so every message type is protected by one
  // shared check instead of a separate filter per type.
  if (strcmp(env.sender_device_id, Identity::deviceId()) == 0) return;

  bool isBroadcast = (strcmp(topic, "broadcast") == 0);
  const char* contact_key = isBroadcast ? MessageStore::kEveryone : env.sender_device_id;

  if (MessageStore::isDuplicateAndRecord(group_code, contact_key, env.message_id)) return;

  uint32_t effectiveTs = env.timestamp;
  if (effectiveTs == 0 && WifiManager::isNtpSynced()) effectiveTs = WifiManager::getUnixTime();

  for (auto& entry : g_incomingHandlers) {
    if (entry.used && entry.messageType == env.message_type) {
      entry.fn(group_code, contact_key, env, effectiveTs, typePayload, typePayloadLen);
      return;
    }
  }
  // Unregistered message_type: logged and ignored safely, same policy as
  // an unregistered packet_kind.
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

bool registerIncomingMessageHandlerImpl(uint8_t messageType, TextMessage::IncomingMessageHandlerFn fn) {
  for (auto& e : g_incomingHandlers) {
    if (e.used && e.messageType == messageType) return false;
  }
  for (auto& e : g_incomingHandlers) {
    if (!e.used) {
      e = {true, messageType, fn};
      return true;
    }
  }
  return false;
}

// Number Guessing's post-challenge quick-switch (Phase 3 section 11): reset
// the stack and leave Recipient Selection (this group, already known)
// underneath the directly-pushed Chat, so Encoder long from Chat lands on
// "that mode's normal Recipient Selection" per the spec's closing rule,
// instead of whatever deep Game-creation stack was in progress.
void navigateToChatDirectImpl(const char* group_code, const char* contact_key) {
  strncpy(g_selectedGroupCode, group_code, sizeof(g_selectedGroupCode) - 1);
  g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
  strncpy(g_selectedContactKey, contact_key, sizeof(g_selectedContactKey) - 1);
  g_selectedContactKey[sizeof(g_selectedContactKey) - 1] = '\0';
  Menu::init();
  Menu::pushScreen(screenRecipient);
  Menu::pushScreen(screenChat);
}

struct Registrar {
  Registrar() {
    registerModeHandler(Modes::TEXT, screenTextEntry);
    registerMessageType(PacketCodec::MSG_TYPE_TEXT, renderTextMessage, onTextMessageEvent);
    registerNetworkPacketHandler(PacketCodec::PK_MESSAGE, handleIncomingMessagePacket);
    registerIncomingMessageHandlerImpl(PacketCodec::MSG_TYPE_TEXT, handleTextArrival);
  }
};
Registrar g_registrar;

}  // namespace

namespace TextMessage {
bool registerIncomingMessageHandler(uint8_t messageType, IncomingMessageHandlerFn fn) {
  return registerIncomingMessageHandlerImpl(messageType, fn);
}
void setOpenConversation(const char* group_code, const char* contact_key) {
  setOpenConversationImpl(group_code, contact_key);
}
void clearOpenConversation() { clearOpenConversationImpl(); }
bool isConversationOpen(const char* group_code, const char* contact_key) { return isChatOpenFor(group_code, contact_key); }
void navigateToChatDirect(const char* group_code, const char* contact_key) {
  navigateToChatDirectImpl(group_code, contact_key);
}
}  // namespace TextMessage
