#include "core/text_message.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/identity_color.h"
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
#include "core/ui_scratch.h"
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
// Which of the selected logical message's own wrapped visual rows is
// focused (Hardware Fix #4.4 issues B/C, reusing the Hardware Fix #4.3a
// design) -- 0 is its first row. Lets a message taller than the history
// viewport be scrolled through row by row while g_historyCursor keeps
// pointing at the same logical message the whole time.
uint16_t g_historyRowOffset = 0;

char g_composeText[PacketCodec::kMaxDecodedTextLen + 1];
uint8_t g_composeLen = 0;

// Hardware Fix #4.4 issue B: buildComposePrefixSuffix()'s RAW-mode output
// (canonical dots/dashes/slashes/spaces, up to Morse::kMaxPatternLength
// symbol chars plus a trailing space per confirmed character) can be far
// longer than g_composeText itself, so the display-side buffer that holds
// it is sized from that same worst case instead of an arbitrary guess --
// see the identical reasoning in enigma.cpp.
//
// Hardware Fix #4.4b: at 1801 bytes, this is no longer a permanent global
// array -- this screen needs it AND its own history scratch (below) live
// at once during one render pass (the compose prefix is read again to
// draw the compose row AFTER history has already been drawn using its own
// scratch), so it comes from UiScratch::Slot::A while history uses
// Slot::B -- two distinct slots, never aliased against each other. See
// ui_scratch.h.
constexpr size_t kComposePrefixCap = PacketCodec::kMaxDecodedTextLen * (Morse::kMaxPatternLength + 1) + 1;
char g_composePattern[Morse::kMaxPatternLength + 1];
uint8_t g_composePatternLen = 0;
uint32_t g_lastMorseReleaseMs = 0;
// Hardware Fix #4.7e: true from DOT_PRESS_START until the matching
// DOT_RELEASE -- i.e. the physical DOT/DASH key is currently held down for
// a symbol that hasn't finished yet. Real-hardware testing found "A" (.-)
// usually produced "ET" instead: the per-tick IDLE finalizer only measured
// silence since the last release, with no idea a NEW key press was already
// in progress, so it could finalize the pending "." into "E" WHILE the
// following DASH was still being held (its own release hadn't happened
// yet, so nothing had told the idle check to stop). Gating the idle
// finalizer on !g_composeKeyHeld makes finalization depend only on the
// deliberate rules in Fix #4.7d (the physical gap BEFORE a press, or an
// idle gap with no press in progress) -- never on a coincidence of timing
// while a key is already down. This is state driven purely by semantic
// InputEvents (DOT_PRESS_START/DOT_RELEASE) -- it never reads a GPIO pin
// or any input.cpp/.h internal directly.
bool g_composeKeyHeld = false;
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
  // Hardware Fix #4.7e: clearDraft() is the special-command (>=2000ms
  // hold) full-draft-clear path, entered from DOT_RELEASE -- reset here
  // too so no stale "held" state could ever survive a screen re-entry
  // that calls this on setup (see screenChat()'s Menu::consumeJustEntered()
  // branch).
  g_composeKeyHeld = false;
}

// Called on every DOT_PRESS_START, before the new symbol is accepted into
// the pattern buffer (Hardware Fix #4.2). Only captures whether the
// pattern now starting is a new word -- and only at that pattern's FIRST
// symbol (g_composePatternLen == 0); symbol 2/3/... of a multi-symbol
// character (e.g. W = .--) must never re-resolve or overwrite this flag,
// or the boundary decision would be lost partway through composing the
// letter. Never mutates compose text itself -- see finalizeComposeChar().
//
// Hardware Fix #4.7d: takes the physical DOT_PRESS_START event timestamp
// (pressMs) instead of reading millis() itself, so the word-boundary
// decision reflects the user's real key rhythm rather than whenever the
// main loop got around to processing this event.
void captureWordBoundaryOnSymbolStart(uint32_t pressMs) {
  if (g_composePatternLen != 0) return;
  g_currentPatternStartsNewWord =
      Morse::consumeWordBoundaryOnSymbolStart(&g_composeWordGap, Settings::getWpm(), pressMs);
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

// =============================================================================
// Multi-line history + compose layout (Hardware Fix #4.4 issues B/C,
// reusing the Hardware Fix #4.3a/4.3 design already proven in enigma.cpp).
// A single logical message/compose line can span more than one visual row,
// word-wrapped to the real pixel width of the currently-active font
// (Display::wrapLineAt()) instead of being clipped at a fixed character
// count. This never changes message storage, wire format, or the
// canonical-Morse post-Send history rule (renderTextMessage() below is
// untouched) -- it only changes how that already-decoded text is laid out
// on screen.
// =============================================================================

// Hardware Fix #4.4 issue B: unlike Enigma (which shows either a bounded
// ciphertext or a bounded decrypted-plaintext cache), renderTextMessage()
// below ALWAYS renders the canonical RAW-Morse expansion of the decoded
// text in history (by design -- Text history is canonical Morse, never
// Letters Only, per Hardware Fix #4.2's own requirement), which can be far
// longer than the original text: up to kMaxDecodedTextLen chars, each
// expanding to up to Morse::kMaxPatternLength symbol chars plus a trailing
// space. Sized from that exact worst case so history text is never
// silently capped by an unrelated guessed buffer size, matching
// the compose-prefix buffer's identical reasoning above (Hardware Fix
// #4.4b: both now come from the shared UiScratch pool, not a static array).
constexpr size_t kHistoryLineBufCap = PacketCodec::kMaxDecodedTextLen * (Morse::kMaxPatternLength + 1) + 1;

// Hardware Fix #4.4b: kHistoryLineBufCap (1801 bytes) is no longer a
// permanent global array either -- it now comes from the shared
// UiScratch::Slot::B pool (ui_scratch.h), heap-backed and allocated once.
// loadHistoryRow() below is called from many places, including during
// input handling (ENCODER_ROTATE row-stepping) well before the render
// body's own up-front availability check runs, so it defensively falls
// back to a small static "?" placeholder -- the same one already used for
// an unloadable message -- if the pool has no memory to give it, rather
// than ever handing any caller a null or dangling lineBuf pointer.
// Hardware Fix #4.7: two width regimes per message instead of one. TRUE
// ROW 0 (the compact cursor cell + optional icon cell + CYAN sender
// prefix + WHITE body) is narrower than every row after it, which drops
// the icon/sender entirely. Hardware Fix #4.7b Part C: continuation rows
// start at the same X as the sender name on row 0 (continuationX ==
// senderX, i.e. labelX plus whatever icon width that message actually
// reserved), not merely past the cursor cell -- this keeps the history
// cursor's own cell clear and gives row 0 and its continuations a
// consistent visual left edge. Every consumer of a message's row layout
// (row counting, viewport placement, focused-row stepping, historyRowY(),
// and actual drawing) reads these same four fields off one HistoryRowInfo,
// so they can never disagree.
struct HistoryRowInfo {
  const char* lineBuf;  // points into the Slot::B scratch buffer (or a static fallback); valid until the next loadHistoryRow() call
  char senderPrefix[24];
  MessageIconKind icon;
  int16_t firstBodyX;
  int16_t firstBodyWidth;
  int16_t continuationX;
  int16_t continuationWidth;
  // Feature Fix #4.8: the sender's personal color, resolved once here (via
  // Presence::resolveColorIndex()) rather than the fixed ST77XX_CYAN every
  // sender name used before -- already falls back to CYAN for a legacy/
  // unknown sender so old peers stay readable. Message BODY color is
  // untouched (always WHITE, drawn via plain printLine()).
  uint16_t senderColor565;
};

// Loads history entry `index` and computes where its body text wraps.
// `labelX` is the fixed compact-cursor-cell-relative icon-cell X already
// used by every history row's TRUE first row (2 + Display::kCursorCellWidth).
void loadHistoryRow(uint16_t index, int16_t labelX, HistoryRowInfo* out) {
  out->senderPrefix[0] = '\0';
  out->icon = MessageIconKind::NONE;
  out->senderColor565 = ST77XX_CYAN;  // safe default; overwritten below once a sender is actually known
  char* scratch = UiScratch::ensure(UiScratch::Slot::B, kHistoryLineBufCap);
  if (scratch == nullptr) {
    // Never dereference a failed allocation (Hardware Fix #4.4b) -- degrade
    // to the existing "message failed to load" placeholder instead. icon
    // is NONE here, so the icon cell is correctly not reserved either.
    out->lineBuf = "?";
    out->firstBodyX = labelX;
    out->firstBodyWidth = static_cast<int16_t>(Display::kScreenWidth - out->firstBodyX);
    out->continuationX = out->firstBodyX;
    out->continuationWidth = out->firstBodyWidth;
    return;
  }
  out->lineBuf = scratch;
  scratch[0] = '?';
  scratch[1] = '\0';
  const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(index);
  if (entry != nullptr) {
    MessageRef ref = refForIndexEntry(*entry);
    StoredMessageView view;
    if (MessageStore::loadMessage(ref, &view)) {
      RenderFn renderFn = getMessageRenderFn(view.envelope.message_type);
      if (renderFn != nullptr) renderFn(view, scratch, kHistoryLineBufCap);
      MessageStore::buildSenderPrefix(view.envelope, out->senderPrefix, sizeof(out->senderPrefix));
      MessageIconFn iconFn = getMessageIconFn(view.envelope.message_type);
      if (iconFn != nullptr) out->icon = iconFn(view);
      // Feature Fix #4.8 section 2M/2N: presentation metadata resolved at
      // render time from the CURRENT known/personal color, never stored
      // in the message itself -- an old message's sender color can change
      // (e.g. after its author picks a new color) without rewriting any
      // stored record. Falls back to the existing CYAN for a legacy/
      // unknown sender so old peers stay readable.
      uint8_t colorIdx = Presence::resolveColorIndex(view.envelope.group_code, view.envelope.sender_device_id);
      out->senderColor565 = IdentityColor::isValid(colorIdx) ? IdentityColor::color565(colorIdx) : ST77XX_CYAN;
    }
  }
  // Hardware Fix #4.7b: only reserve the lock-icon cell when a message
  // actually has one -- MessageIconKind::NONE (the overwhelming majority
  // of Text history) no longer wastes kLockIconCellWidth of otherwise-
  // usable space between the cursor and the sender name.
  int16_t iconWidth = (out->icon == MessageIconKind::NONE) ? 0 : Display::kLockIconCellWidth;
  int16_t senderX = static_cast<int16_t>(labelX + iconWidth);
  out->firstBodyX = static_cast<int16_t>(senderX + Display::textWidth(out->senderPrefix));
  out->firstBodyWidth = static_cast<int16_t>(Display::kScreenWidth - out->firstBodyX);
  // Continuation rows start at the SAME X as the sender name on row 0
  // (Hardware Fix #4.7b Part C), not merely past the cursor cell -- this
  // keeps the history cursor's own cell clear and gives row 0 and its
  // continuations a consistent visual left edge.
  out->continuationX = senderX;
  out->continuationWidth = static_cast<int16_t>(Display::kScreenWidth - out->continuationX);
}

// Streams through an already-loaded message's wrapped rows via
// Display::wrapLineAt(), never materializing them all at once. Returns the
// total row count (always >= 1: an empty body still occupies one row
// rather than vanishing).
uint16_t countHistoryRows(const HistoryRowInfo& info) {
  size_t len = strlen(info.lineBuf);
  if (len == 0) return 1;
  uint16_t rows = 0;
  size_t pos = 0;
  int16_t width = info.firstBodyWidth;
  while (pos < len) {
    uint16_t s, l;
    if (!Display::wrapLineAt(info.lineBuf, pos, width, &s, &l)) break;
    pos = static_cast<size_t>(s) + l;
    rows++;
    width = info.continuationWidth;  // row 0 wraps at firstBodyWidth; every row after at continuationWidth
  }
  return (rows > 0) ? rows : 1;
}

uint16_t countHistoryRowsFor(uint16_t index, int16_t labelX) {
  HistoryRowInfo info;
  loadHistoryRow(index, labelX, &info);
  return countHistoryRows(info);
}

// Picks the history viewport's starting position as a (message index,
// rows-already-scrolled-past-within-that-message) pair instead of a plain
// message index, so a single logical message taller than the viewport can
// itself be windowed mid-message. Default placement is bottom-anchored
// (fills the viewport with the most recent rows, splitting the oldest
// included message mid-way if it alone doesn't fit the remaining budget).
// When a message is focused, that default window is kept AS-IS whenever
// the focused row already falls inside it (preserving the marker-only
// partial redraw), and is shifted the minimum amount otherwise so the
// focused row becomes visible. Identical algorithm to enigma.cpp's
// computeHistoryViewport() (Hardware Fix #4.3a issue 1).
void computeHistoryViewport(uint16_t viewportLines, int16_t labelX, uint16_t* outStartIdx,
                            uint16_t* outStartRowSkip) {
  *outStartIdx = 0;
  *outStartRowSkip = 0;
  if (g_indexTotal == 0) return;

  int32_t budget = viewportLines;
  uint16_t defIdx = 0;
  uint16_t defSkip = 0;
  uint16_t idx = g_indexTotal;
  bool placedAny = false;
  while (idx > 0) {
    idx--;
    uint16_t rows = countHistoryRowsFor(idx, labelX);
    if (static_cast<int32_t>(rows) <= budget) {
      budget -= rows;
      defIdx = idx;
      defSkip = 0;
      placedAny = true;
      if (budget == 0) break;
    } else {
      defIdx = idx;
      defSkip = static_cast<uint16_t>(rows - static_cast<uint16_t>(budget));
      placedAny = true;
      break;
    }
  }
  if (!placedAny) {
    defIdx = 0;
    defSkip = 0;
  }

  if (g_historyCursor == kNoHistoryCursor) {
    *outStartIdx = defIdx;
    *outStartRowSkip = defSkip;
    return;
  }

  if (g_historyCursor < defIdx || (g_historyCursor == defIdx && g_historyRowOffset < defSkip)) {
    *outStartIdx = g_historyCursor;
    *outStartRowSkip = g_historyRowOffset;
    return;
  }

  int32_t rowsBefore;
  {
    HistoryRowInfo first;
    loadHistoryRow(defIdx, labelX, &first);
    if (defIdx == g_historyCursor) {
      rowsBefore = static_cast<int32_t>(g_historyRowOffset) - static_cast<int32_t>(defSkip);
    } else {
      rowsBefore = static_cast<int32_t>(countHistoryRows(first)) - static_cast<int32_t>(defSkip);
      for (uint16_t i = static_cast<uint16_t>(defIdx + 1); i < g_historyCursor; i++) {
        rowsBefore += countHistoryRowsFor(i, labelX);
      }
      rowsBefore += g_historyRowOffset;
    }
  }

  if (rowsBefore < static_cast<int32_t>(viewportLines)) {
    *outStartIdx = defIdx;
    *outStartRowSkip = defSkip;
    return;
  }

  uint16_t curIdx = defIdx;
  uint16_t curSkip = defSkip;
  while (rowsBefore >= static_cast<int32_t>(viewportLines)) {
    uint16_t rows = countHistoryRowsFor(curIdx, labelX);
    if (static_cast<uint16_t>(curSkip + 1) < rows) {
      curSkip++;
    } else {
      curIdx++;
      curSkip = 0;
    }
    rowsBefore--;
  }
  *outStartIdx = curIdx;
  *outStartRowSkip = curSkip;
}

// Y of visual row `targetRowIdx` of history index `targetIdx`, given the
// viewport currently starts at (startIdx, startRowSkip).
int16_t historyRowY(uint16_t startIdx, uint16_t startRowSkip, uint16_t targetIdx, uint16_t targetRowIdx,
                    int16_t labelX, int16_t contentTop, int16_t lh) {
  int32_t rows = 0;
  for (uint16_t i = startIdx; i <= targetIdx; i++) {
    HistoryRowInfo info;
    loadHistoryRow(i, labelX, &info);
    uint16_t skip = (i == startIdx) ? startRowSkip : 0;
    uint16_t limit = (i == targetIdx) ? targetRowIdx : countHistoryRows(info);
    if (limit > skip) rows += static_cast<int32_t>(limit - skip);
  }
  return static_cast<int16_t>(contentTop + rows * lh);
}

constexpr uint8_t kMaxComposeWrapLines = 128;
constexpr size_t kComposeTailSrcCap = 96;

struct ComposeLayout {
  uint16_t prefixStarts[kMaxComposeWrapLines];
  uint16_t prefixLens[kMaxComposeWrapLines];
  uint8_t prefixCount;    // rows wrapping the confirmed compose text alone
  char tailSrc[kComposeTailSrcCap];
  uint16_t tailStarts[8];
  uint16_t tailLens[8];
  uint8_t tailCount;      // rows wrapping (last confirmed line + in-progress suffix)
  uint8_t confirmedRows;  // prefixCount>0 ? prefixCount-1 : 0 -- stable across suffix changes
  uint8_t totalRows;      // confirmedRows + tailCount, always >= 1
};

// Splits the compose line into rows that are provably stable while a
// Morse pattern is being keyed in (every row except the very last) and
// rows that depend on the in-progress suffix (the last confirmed line
// re-wrapped together with the suffix) -- identical algorithm to
// enigma.cpp's buildComposeLayout().
void buildComposeLayout(const char* prefix, const char* suffix, int16_t widthPx, ComposeLayout* out) {
  out->prefixCount = Display::wrapText(prefix, widthPx, out->prefixStarts, out->prefixLens, kMaxComposeWrapLines);
  out->confirmedRows = (out->prefixCount > 0) ? static_cast<uint8_t>(out->prefixCount - 1) : 0;

  size_t tp = 0;
  if (out->prefixCount > 0) {
    size_t lastStart = out->prefixStarts[out->prefixCount - 1];
    size_t lastLen = out->prefixLens[out->prefixCount - 1];
    if (lastLen >= kComposeTailSrcCap) lastLen = kComposeTailSrcCap - 1;
    memcpy(out->tailSrc, prefix + lastStart, lastLen);
    tp = lastLen;
  }
  size_t suffixLen = strlen(suffix);
  if (tp + suffixLen >= kComposeTailSrcCap) suffixLen = kComposeTailSrcCap - 1 - tp;
  memcpy(out->tailSrc + tp, suffix, suffixLen);
  tp += suffixLen;
  out->tailSrc[tp] = '\0';

  out->tailCount = Display::wrapText(out->tailSrc, widthPx, out->tailStarts, out->tailLens, 8);
  if (out->tailCount == 0) {
    out->tailStarts[0] = 0;
    out->tailLens[0] = 0;
    out->tailCount = 1;
  }
  out->totalRows = static_cast<uint8_t>(out->confirmedRows + out->tailCount);
}

// Slices out the text of logical compose row `rowIdx` (0-based over the
// full, unwindowed row list -- confirmed rows first, then tail rows).
void composeRowText(const ComposeLayout& layout, const char* prefix, uint8_t rowIdx, char* out, size_t outCap) {
  size_t s, l;
  if (rowIdx < layout.confirmedRows) {
    s = layout.prefixStarts[rowIdx];
    l = layout.prefixLens[rowIdx];
    if (l >= outCap) l = outCap - 1;
    memcpy(out, prefix + s, l);
  } else {
    uint8_t tIdx = static_cast<uint8_t>(rowIdx - layout.confirmedRows);
    s = layout.tailStarts[tIdx];
    l = layout.tailLens[tIdx];
    if (l >= outCap) l = outCap - 1;
    memcpy(out, layout.tailSrc + s, l);
  }
  out[l] = '\0';
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
    // Hardware Fix #4.7d Parts A-C: use the physical accepted press time,
    // not whenever this event happens to be processed.
    uint32_t pressMs = e.eventMs != 0 ? e.eventMs : millis();
    // Catch-up finalization: Fix #4.7b's reliable edge capture means a
    // busy main loop can now drain "release old letter / >=3dit physical
    // gap / press new letter" all in one tick -- the old idle check only
    // ran AFTER the whole event-drain loop, so the previous letter would
    // still be sitting in g_composePattern when this new symbol arrived,
    // merging two genuinely separate physical letters into one. Finalize
    // the pending letter here, using the real physical gap, BEFORE this
    // new symbol is accepted -- and BEFORE capturing the word boundary
    // below, since finalizeComposeChar() arms the word-gap timer from the
    // previous letter's real release time, which this new pressMs must be
    // compared against.
    if (g_composePatternLen > 0 && (pressMs - g_lastMorseReleaseMs) >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeComposeChar();
    }
    // Only captures whether this new pattern starts a new word (Hardware
    // Fix #4.2) -- never mutates compose text itself. The space (if any)
    // is committed later, only at NORMAL letter finalization; a delete
    // prosign discards the captured flag below instead of consuming it as
    // a space.
    captureWordBoundaryOnSymbolStart(pressMs);
    // Hardware Fix #4.7e: mark the key held only after the catch-up
    // finalize/word-boundary-capture above have used the pre-press state --
    // the physical gap BEFORE this press is what decides whether the
    // previous character ended, not whether a key happens to be held.
    g_composeKeyHeld = true;
  } else if (e.type == InputEventType::DOT_RELEASE) {
    // Hardware Fix #4.7e: clear held state before any early-return path
    // below, so it can never remain stuck true after a real release --
    // special-command, normal DOT/DASH, and delete all go through here.
    g_composeKeyHeld = false;
    if (e.durationMs >= Morse::kSpecialCommandMs) {
      clearDraft();  // DOT/DASH >=2000ms on compose: clear full draft
      return;
    }
    Morse::SymbolClass sc = Morse::classifyPress(e.durationMs, Settings::getWpm());
    if (g_composePatternLen < Morse::kMaxPatternLength) {
      g_composePattern[g_composePatternLen++] = (sc == Morse::SymbolClass::DOT) ? '.' : '-';
      g_composePattern[g_composePatternLen] = '\0';
    }
    // Hardware Fix #4.7d: store the physical accepted release time, not
    // processing-time millis(), so the next DOT_PRESS_START's catch-up
    // check and the idle finalizer below both measure the real gap.
    g_lastMorseReleaseMs = e.eventMs != 0 ? e.eventMs : millis();

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
//
// Hardware Fix #4.4 issues B/C extend this the same way Hardware Fix
// #4.3/4.3a extended Enigma's: a history message and the compose line can
// each now span multiple visual rows, so "the viewport itself moved" is
// tracked via g_chatLastViewportLines/g_chatLastStartRowSkip (message
// index alone is no longer enough once a single message can occupy more
// rows than the whole viewport), g_chatLastComposeSkipped catches an
// already-overflowing compose area's visible window sliding without its
// row count changing, and g_chatLastComposeRowText snapshots each
// currently-visible compose row's own text so a single dot/dash only
// repaints the one row it actually changed.
bool g_chatNeedsFullRedraw = true;
int16_t g_chatLastStartIdx = -1;
int16_t g_chatLastStartRowSkip = -1;
uint16_t g_chatLastCursor = kNoHistoryCursor;
uint16_t g_chatLastRowOffset = 0;
bool g_chatLastComposeFocused = true;
constexpr uint16_t kNoViewportLines = 0xFFFF;
uint16_t g_chatLastViewportLines = kNoViewportLines;
constexpr uint8_t kMaxShownComposeRows = 10;
uint8_t g_chatLastComposeSkipped = 0xFF;
char g_chatLastComposeRowText[kMaxShownComposeRows][64] = {{0}};

void screenChat() {
  if (Menu::consumeJustEntered()) {
    clearDraft();
    g_historyCursor = kNoHistoryCursor;
    g_historyRowOffset = 0;
    strncpy(g_chatOpenGroup, g_selectedGroupCode, sizeof(g_chatOpenGroup) - 1);
    g_chatOpenGroup[sizeof(g_chatOpenGroup) - 1] = '\0';
    strncpy(g_chatOpenContact, g_selectedContactKey, sizeof(g_chatOpenContact) - 1);
    g_chatOpenContact[sizeof(g_chatOpenContact) - 1] = '\0';
    g_chatIsOpen = true;
    markIndexDirty();
    g_chatRenderDirty = true;
    g_chatNeedsFullRedraw = true;
  }

  // Needed before input handling below: deciding how far ENCODER_ROTATE can
  // step within a multi-row message requires measuring text width, which
  // depends on the active font (Hardware Fix #4.4 issue C).
  Display::setFont(Display::Font::PRIMARY);

  Input::update();
  InputEvent e;
  bool hadEvent = false;
  while (Input::popEvent(e)) {
    hadEvent = true;
    if (e.type == InputEventType::ENCODER_ROTATE) {
      refreshIndexIfNeeded();
      // Hardware Fix #4.4 issue C: a logical message can span more visual
      // rows than the viewport, so rotating steps through THAT message's
      // own rows (g_historyRowOffset) before moving to the next/previous
      // logical message -- identical to enigma.cpp's ENCODER_ROTATE
      // handler (Hardware Fix #4.3a issue 1).
      int16_t rotLabelX = static_cast<int16_t>(2 + Display::kCursorCellWidth);
      if (g_historyCursor == kNoHistoryCursor) {
        if (e.value < 0 && g_indexTotal > 0) {
          g_historyCursor = static_cast<uint16_t>(g_indexTotal - 1);
          uint16_t rows = countHistoryRowsFor(g_historyCursor, rotLabelX);
          g_historyRowOffset = static_cast<uint16_t>((rows > 0) ? rows - 1 : 0);
        }
      } else if (e.value < 0) {
        if (g_historyRowOffset > 0) {
          g_historyRowOffset--;
        } else if (g_historyCursor > 0) {
          g_historyCursor--;
          uint16_t rows = countHistoryRowsFor(g_historyCursor, rotLabelX);
          g_historyRowOffset = static_cast<uint16_t>((rows > 0) ? rows - 1 : 0);
        }
      } else {
        uint16_t rows = countHistoryRowsFor(g_historyCursor, rotLabelX);
        if (static_cast<uint16_t>(g_historyRowOffset + 1) < rows) {
          g_historyRowOffset++;
        } else if (g_historyCursor + 1 >= g_indexTotal) {
          g_historyCursor = kNoHistoryCursor;
          g_historyRowOffset = 0;
        } else {
          g_historyCursor++;
          g_historyRowOffset = 0;
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
  //
  // Hardware Fix #4.7e: also gated on !g_composeKeyHeld -- without it, an
  // intentional DASH held for ~400-500ms at low WPM could itself age past
  // letterGapMs() measured from the PREVIOUS release, finalizing "." into
  // "E" while the DASH was still being held, before its own release ever
  // arrived (observed on hardware as "A" == ".-" producing "ET"). Letter
  // finalization now only ever happens (a) at DOT_PRESS_START, from the
  // real physical gap BEFORE that press (Fix #4.7d), or (b) here, while no
  // DOT/DASH key is currently down -- never merely because silence since
  // the last release happened to cross the threshold while a new press was
  // already in progress.
  if (g_historyCursor == kNoHistoryCursor && !g_composeKeyHeld && g_composePatternLen > 0) {
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

  // Font was already set to PRIMARY above (before input handling); still
  // current here since nothing else runs in between.
  int16_t lh = Display::lineHeight();

  int16_t contentTop = Display::kStatusBarHeight + 2;
  int16_t contentHeight = Display::kScreenHeight - contentTop;
  uint16_t totalLines = (contentHeight > 0) ? static_cast<uint16_t>(contentHeight / lh) : 0;
  if (totalLines < 2) totalLines = 2;  // at least 1 history row + compose row

  // Hardware Fix #4.4b: this screen needs two large RAW-Morse expansion
  // scratch buffers live at once -- the confirmed compose prefix (read
  // again below to draw the compose row, AFTER history has already used
  // its own scratch to draw) and the history renderer's own per-message
  // scratch -- so it claims BOTH shared pool slots up front, before any
  // drawing happens this pass, and bails out cleanly if either is
  // unavailable rather than leaving a half-drawn screen or touching
  // g_composeText/the draft.
  char* composePrefixBuf = UiScratch::ensure(UiScratch::Slot::A, kComposePrefixCap);
  char* historyScratchCheck = UiScratch::ensure(UiScratch::Slot::B, kHistoryLineBufCap);
  if (composePrefixBuf == nullptr || historyScratchCheck == nullptr) {
    Display::clearContentArea();
    Display::printLine(2, contentTop, "Memory Low");
    g_chatNeedsFullRedraw = true;  // force a full redraw once a later pass succeeds
    return;
  }

  // Hardware Fix #4.7: compact cursor cell (Display::kCursorCellWidth)
  // instead of the old PRIMARY-font ">" glyph + padding, on both history
  // and compose -- compose has no sender prefix, so its text starts right
  // after this same cell (Part G).
  int16_t labelX = static_cast<int16_t>(2 + Display::kCursorCellWidth);  // history icon-cell X
  int16_t composePrefixX = labelX;                                       // compose shares the same left margin
  int16_t composeWidth = static_cast<int16_t>(Display::kScreenWidth - composePrefixX);

  // Compose text (Hardware Fix #4.4 issue B): word-wrapped by real pixel
  // width instead of clipped at a fixed character count. Confirmed rows
  // (everything except the last) are provably stable while a pattern is
  // being keyed in -- only the last confirmed line + in-progress suffix is
  // ever re-wrapped on a dot/dash. Canonical behavior is unchanged: Typing
  // Display still controls COMPOSE only (buildComposePrefixSuffix() itself
  // is untouched).
  char composeSuffix[Morse::kMaxPatternLength + 2];
  buildComposePrefixSuffix(composePrefixBuf, kComposePrefixCap, composeSuffix, sizeof(composeSuffix));
  bool composeFocused = (g_historyCursor == kNoHistoryCursor);

  ComposeLayout layout;
  buildComposeLayout(composePrefixBuf, composeSuffix, composeWidth, &layout);

  // No separate error row in this screen (unlike Enigma) -- the whole
  // budget below the history viewport is compose's own.
  uint16_t maxComposeRows = totalLines;
  if (maxComposeRows > kMaxShownComposeRows) maxComposeRows = kMaxShownComposeRows;
  uint16_t shownComposeRows = (layout.totalRows < maxComposeRows) ? layout.totalRows : maxComposeRows;
  if (shownComposeRows == 0) shownComposeRows = 1;
  uint8_t skippedComposeRows = static_cast<uint8_t>(layout.totalRows - shownComposeRows);

  // History gets whatever vertical space compose doesn't need this frame
  // (Hardware Fix #4.4 issue B: "history viewport above it shrinks as
  // needed"); can reach 0 when compose alone fills the screen.
  uint16_t viewportLines = static_cast<uint16_t>(totalLines - shownComposeRows);

  uint16_t startIdx = 0;
  uint16_t startRowSkip = 0;
  computeHistoryViewport(viewportLines, labelX, &startIdx, &startRowSkip);
  int16_t composeY = static_cast<int16_t>(contentTop + viewportLines * lh);

  bool firstDraw = g_chatNeedsFullRedraw;
  // Any change to how many rows the history viewport has means every Y
  // coordinate below the status bar shifted, so history and compose both
  // need a full repaint together (Hardware Fix #4.4 issues B/C).
  bool historyLayoutChanged = firstDraw || (viewportLines != g_chatLastViewportLines);
  bool contentChanged = !historyLayoutChanged && wasIndexDirty;
  bool scrolled = !historyLayoutChanged && !contentChanged &&
                  (static_cast<int16_t>(startIdx) != g_chatLastStartIdx ||
                   static_cast<int16_t>(startRowSkip) != g_chatLastStartRowSkip);
  bool selectionOnlyChanged = !historyLayoutChanged && !contentChanged && !scrolled &&
                              (g_historyCursor != g_chatLastCursor || g_historyRowOffset != g_chatLastRowOffset);

  if (historyLayoutChanged || contentChanged || scrolled) {
    if (historyLayoutChanged) {
      Display::clearContentArea();
    } else {
      int16_t regionH = static_cast<int16_t>(composeY - contentTop);
      if (regionH < 0) regionH = 0;
      Display::tft().fillRect(0, contentTop, Display::kScreenWidth, regionH, ST77XX_BLACK);
    }
    // Hardware Fix #4.4 issue C: streams each message's rows on demand via
    // Display::wrapLineAt() instead of a fixed-size lineBuf[48], and the
    // FIRST message drawn can start mid-message at startRowSkip -- so a
    // single logical message taller than the whole viewport still has
    // every one of its rows reachable as the viewport scrolls.
    int16_t y = contentTop;
    uint16_t rowsDrawn = 0;
    for (uint16_t i = startIdx; i < g_indexTotal && rowsDrawn < viewportLines; i++) {
      HistoryRowInfo info;
      loadHistoryRow(i, labelX, &info);
      uint16_t rowSkip = (i == startIdx) ? startRowSkip : 0;
      size_t len = strlen(info.lineBuf);
      size_t pos = 0;
      uint16_t rowIdx = 0;
      while (rowsDrawn < viewportLines) {
        int16_t rowWidth = (rowIdx == 0) ? info.firstBodyWidth : info.continuationWidth;
        uint16_t s = 0, l = 0;
        bool has = (len == 0) ? (rowIdx == 0) : Display::wrapLineAt(info.lineBuf, pos, rowWidth, &s, &l);
        if (!has) break;
        if (rowIdx >= rowSkip) {
          int16_t rowBodyX = (rowIdx == 0) ? info.firstBodyX : info.continuationX;
          if (rowIdx == 0) {
            // Icon cell (only reserved when an icon is actually present,
            // Hardware Fix #4.7b Part B) + sender name (Feature Fix #4.8:
            // the sender's personal color, falling back to the original
            // CYAN for a legacy/unknown sender) appear on the message's
            // true FIRST row only; continuation rows start at that same
            // sender X, with no icon/sender repeated (Hardware Fix #4.7
            // Part E, #4.7b Part C).
            drawRowIcon(labelX, y, info.icon);
            int16_t iconWidth = (info.icon == MessageIconKind::NONE) ? 0 : Display::kLockIconCellWidth;
            int16_t textX = static_cast<int16_t>(labelX + iconWidth);
            Display::printLineColored(textX, y, info.senderPrefix, info.senderColor565);
          }
          // The marker sits on the exact focused row (g_historyRowOffset),
          // which computeHistoryViewport() always keeps inside the drawn
          // range for the selected message. May land on a continuation
          // row while scrolling through a stored message -- intentional.
          if (i == g_historyCursor && rowIdx == g_historyRowOffset) Display::drawSelectionCursor(2, y);
          char lineChunk[Display::kPrintLineBufferSize];
          size_t clen = l;
          if (clen > Display::kPrintLineMaxChars) clen = Display::kPrintLineMaxChars;
          memcpy(lineChunk, info.lineBuf + s, clen);
          lineChunk[clen] = '\0';
          Display::printLine(rowBodyX, y, lineChunk);
          y = static_cast<int16_t>(y + lh);
          rowsDrawn++;
        }
        if (len == 0) break;
        pos = static_cast<size_t>(s) + l;
        rowIdx++;
      }
    }
    g_chatNeedsFullRedraw = false;
  } else if (selectionOnlyChanged && viewportLines > 0) {
    // Guarded on viewportLines > 0 so a focused message never gets its
    // cursor marker drawn into the compose region below just because the
    // history viewport currently has zero rows.
    if (g_chatLastCursor != kNoHistoryCursor) {
      int16_t oldY =
          historyRowY(startIdx, startRowSkip, g_chatLastCursor, g_chatLastRowOffset, labelX, contentTop, lh);
      Display::tft().fillRect(2, oldY, Display::kCursorCellWidth, lh, ST77XX_BLACK);
    }
    if (g_historyCursor != kNoHistoryCursor) {
      int16_t newY = historyRowY(startIdx, startRowSkip, g_historyCursor, g_historyRowOffset, labelX, contentTop, lh);
      Display::tft().fillRect(2, newY, Display::kCursorCellWidth, lh, ST77XX_BLACK);
      Display::drawSelectionCursor(2, newY);
    }
  }

  // Compose row(s) are diffed independently of the history rows above them
  // (Hardware Fix #3, Section F; extended for multi-row in Hardware Fix
  // #4.4 issue B); historyLayoutChanged forces a full redraw since
  // clearContentArea() already wiped the compose region too.
  //
  // Hardware Fix #4.7c: composeFocusChanged is now also folded into
  // composeBlockChanged. Real hardware testing found stale text surviving
  // in the compose area after HISTORY -> COMPOSE (rotate up into history,
  // then back down to the bottom). Root cause: a pure focus transition,
  // with the compose text itself unchanged, previously left
  // composeBlockChanged false, so it fell into the small per-row diff
  // below -- which only compares rowText against the cached snapshot
  // (equal, since the text didn't change) and, at most, patches the
  // cursor's own kCursorCellWidth-wide cell. That patch never touches the
  // rest of the row, so any pixels drawn there while focus was elsewhere
  // (or left over from a still-uncorrected earlier partial draw) could
  // survive until something else finally changed the row's text. Treating
  // every focus transition as a full compose-block clear+redraw removes
  // that gap entirely, at the cost of one extra full redraw exactly on the
  // tick focus flips -- not on every tick while focus is held.
  bool composeFocusChanged = (composeFocused != g_chatLastComposeFocused);
  bool composeWindowChanged = (skippedComposeRows != g_chatLastComposeSkipped);
  // Row-count shrink safety (reviewed, not patched): shownComposeRows can
  // only change when layout.totalRows crosses the maxComposeRows cap,
  // since totalLines and maxComposeRows are both per-tick constants
  // derived only from screen height -- and viewportLines is defined as
  // exactly (totalLines - shownComposeRows) above, so ANY shownComposeRows
  // change necessarily changes viewportLines too, which already forces
  // historyLayoutChanged (and therefore composeBlockChanged) true and
  // clearContentArea() wipes the WHOLE content area, including any taller
  // previous compose block. The remaining case -- totalRows shrinking
  // while still capped at maxComposeRows -- changes skippedComposeRows
  // (composeWindowChanged) but never shownComposeRows itself, so the
  // block's own height never shrinks in that case. No obsolete row can
  // therefore survive a shrink without already going through a full clear.
  bool composeBlockChanged = historyLayoutChanged || composeWindowChanged || composeFocusChanged;

  if (composeBlockChanged) {
    if (!historyLayoutChanged) {
      int16_t blockH = static_cast<int16_t>(shownComposeRows * lh);
      Display::tft().fillRect(0, composeY, Display::kScreenWidth, blockH, ST77XX_BLACK);
    }
    for (uint16_t shownRow = 0; shownRow < shownComposeRows; shownRow++) {
      uint8_t rowIdx = static_cast<uint8_t>(skippedComposeRows + shownRow);
      char rowText[64];
      composeRowText(layout, composePrefixBuf, rowIdx, rowText, sizeof(rowText));
      int16_t y = static_cast<int16_t>(composeY + shownRow * lh);
      // Hardware Fix #4.7 Part F: the cursor belongs to LOGICAL compose row
      // 0 (rowIdx == 0), never merely the first VISIBLE row (shownRow == 0)
      // -- when skippedComposeRows > 0, logical row 0 has scrolled off and
      // no shown row gets a cursor at all.
      if (rowIdx == 0 && composeFocused) Display::drawSelectionCursor(2, y);
      Display::printLine(composePrefixX, y, rowText);
      strncpy(g_chatLastComposeRowText[shownRow], rowText, sizeof(g_chatLastComposeRowText[shownRow]) - 1);
      g_chatLastComposeRowText[shownRow][sizeof(g_chatLastComposeRowText[shownRow]) - 1] = '\0';
    }
  } else {
    for (uint16_t shownRow = 0; shownRow < shownComposeRows; shownRow++) {
      uint8_t rowIdx = static_cast<uint8_t>(skippedComposeRows + shownRow);
      char rowText[64];
      composeRowText(layout, composePrefixBuf, rowIdx, rowText, sizeof(rowText));
      int16_t y = static_cast<int16_t>(composeY + shownRow * lh);
      bool textChanged = strcmp(rowText, g_chatLastComposeRowText[shownRow]) != 0;
      bool markerNeedsRedraw = (rowIdx == 0) && composeFocusChanged;
      if (textChanged) {
        Display::tft().fillRect(0, y, Display::kScreenWidth, lh, ST77XX_BLACK);
        if (rowIdx == 0 && composeFocused) Display::drawSelectionCursor(2, y);
        Display::printLine(composePrefixX, y, rowText);
        strncpy(g_chatLastComposeRowText[shownRow], rowText, sizeof(g_chatLastComposeRowText[shownRow]) - 1);
        g_chatLastComposeRowText[shownRow][sizeof(g_chatLastComposeRowText[shownRow]) - 1] = '\0';
      } else if (markerNeedsRedraw) {
        Display::tft().fillRect(2, y, Display::kCursorCellWidth, lh, ST77XX_BLACK);
        if (composeFocused) Display::drawSelectionCursor(2, y);
      }
    }
  }

  g_chatLastComposeFocused = composeFocused;
  g_chatLastComposeSkipped = skippedComposeRows;
  g_chatLastViewportLines = viewportLines;
  g_chatLastStartIdx = static_cast<int16_t>(startIdx);
  g_chatLastStartRowSkip = static_cast<int16_t>(startRowSkip);
  g_chatLastCursor = g_historyCursor;
  g_chatLastRowOffset = g_historyRowOffset;
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
