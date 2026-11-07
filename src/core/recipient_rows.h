#pragma once
// Recipient rows of the Text and Enigma chats (shared by both): online
// contacts, then recent offline contacts, then every other conversation of
// the group that may hold unread messages of the list's category, then
// Everyone. Rows are keyed by contact identity within the selected group.
//
// The unread conversations come from Notifications::nextUnreadContactIn(),
// the in-memory summaries behind the Main Menu and list badges (tracked
// summaries only, at most 24): building, refreshing and drawing these rows
// opens no file, lists no directory and touches no NVS. Names come from
// Presence's RAM tables (Presence::resolveDisplayName()); with no name known
// the row shows the contact id.
//
// Capacity (round 10): no row gives way to another. The rows hold every
// recipient that can exist at once -- 20 online + 20 recent offline + one per
// tracked conversation + Everyone -- plus one spare (kMaxRows = 66). ListMenu
// scrolls them. The two chats share this one row store: a chat's list is only
// ever the top screen (choosing a row replaces it with the chat or contact
// menu, and every return to it rebuilds it), so the Text and Enigma lists are
// never live together. Sharing keeps static RAM below the two former 41-row
// stores.
//
// During one visit rows are never removed, with one bounded exception: if new
// unread conversations keep appearing while the list stays open (a summary
// whose last read completed is reused by a new sender) until all kMaxRows
// rows are used, a new unread row reuses an added row whose conversation is
// no longer unread and is not selected. Ordinary (online/recent) rows and
// Everyone are never reused. Why one always exists: when all rows are used
// there are 25 added rows, while at most 24 conversations are tracked and the
// new one is one of them, so at most 23 added rows are unread -- at least two
// are not, and at most one of those is selected.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/menu.h"
#include "core/notifications.h"
#include "core/presence.h"
#include "core/storage_messages.h"

namespace RecipientRows {

constexpr size_t kLabelLen = 40;
constexpr uint8_t kMaxOnline = 20;
constexpr uint8_t kMaxRecent = 20;
// Ordinary rows, one added row per tracked conversation plus one spare, and
// Everyone (see the reuse rule above for why the spare).
constexpr uint8_t kMaxRows = kMaxOnline + kMaxRecent + Notifications::kMaxTrackedConversations + 1 + 1;
constexpr uint8_t kNoChange = 0xFF;

struct Storage {
  SettingItem items[kMaxRows];
  char labels[kMaxRows][kLabelLen];
  char keys[kMaxRows][MessageStore::kContactKeyLen];
  uint8_t count;
  uint8_t ordinaryEnd;  // rows [0, ordinaryEnd) are the online and recent ones
};

// The one row store (zero-initialized, no constructor: no guard, no heap).
inline Storage& rows() {
  static Storage s;
  return s;
}

// What differs between the chats.
struct Mode {
  const char* group;
  uint8_t badgeMask;  // Notifications::BADGE_TEXT or BADGE_ENIGMA
  ScreenHandlerFn onSelect;
};

inline int findKey(const char* key) {
  Storage& r = rows();
  for (uint8_t i = 0; i < r.count; i++)
    if (strcmp(r.keys[i], key) == 0) return i;
  return -1;
}

inline void setKey(char* dst, const char* key) {
  strncpy(dst, key, MessageStore::kContactKeyLen - 1);
  dst[MessageStore::kContactKeyLen - 1] = '\0';
}

// Last 4 characters of the id, in brackets: tells equal names apart.
inline void appendDeviceSuffix(char* label, size_t labelBufSize, const char* device_id) {
  size_t idLen = strlen(device_id);
  if (idLen < 4) return;
  char suffix[10];
  snprintf(suffix, sizeof(suffix), " [%s]", device_id + idLen - 4);
  size_t len = strlen(label);
  size_t room = (labelBufSize > len) ? (labelBufSize - len - 1) : 0;
  strncat(label, suffix, room);
}

inline void removeRow(uint8_t at) {
  Storage& r = rows();
  for (uint8_t i = at; i + 1 < r.count; i++) {
    memcpy(r.labels[i], r.labels[i + 1], kLabelLen);
    memcpy(r.keys[i], r.keys[i + 1], MessageStore::kContactKeyLen);
  }
  r.count--;
}

inline void insertRow(uint8_t at) {
  Storage& r = rows();
  for (uint8_t i = r.count; i > at; i--) {
    memcpy(r.labels[i], r.labels[i - 1], kLabelLen);
    memcpy(r.keys[i], r.keys[i - 1], MessageStore::kContactKeyLen);
  }
  r.count++;
}

// Rows full: frees an added row (not ordinary, not Everyone) whose
// conversation is no longer unread and that is not `keep`. False if none.
inline bool reuseAddedRow(const Mode& m, int keep) {
  Storage& r = rows();
  for (int i = static_cast<int>(r.count) - 1; i >= r.ordinaryEnd; i--) {
    if (i == keep || strcmp(r.keys[i], MessageStore::kEveryone) == 0) continue;
    if (Notifications::hasUnreadIn(m.group, r.keys[i], m.badgeMask)) continue;
    removeRow(static_cast<uint8_t>(i));
    return true;
  }
  return false;
}

inline void labelUnreadRow(const Mode& m, uint8_t i) {
  Storage& r = rows();
  char name[MessageStore::kContactKeyLen + 24];
  Presence::resolveDisplayName(m.group, r.keys[i], name, sizeof(name));
  snprintf(r.labels[i], kLabelLen, Presence::isContactOnline(m.group, r.keys[i]) ? "%s" : "%s (offline)", name);
}

inline void bindItems(const Mode& m) {
  Storage& r = rows();
  for (uint8_t i = 0; i < r.count; i++) r.items[i] = SettingItem{r.labels[i], m.onSelect};
}

// While the list is shown: adds a row, before Everyone, for each unread
// conversation not yet listed (e.g. a message from a sender absent at entry).
// Returns the selected row's new index (same identity) if rows changed, else
// kNoChange.
inline uint8_t refresh(const Mode& m, uint8_t selected) {
  Storage& r = rows();
  char selectedKey[MessageStore::kContactKeyLen];
  setKey(selectedKey, selected < r.count ? r.keys[selected] : "");
  bool changed = false;
  uint8_t cursor = 0;
  const char* summaryKey;
  while ((summaryKey = Notifications::nextUnreadContactIn(m.group, m.badgeMask, cursor)) != nullptr) {
    if (findKey(summaryKey) >= 0) continue;  // (Everyone included: it always has its row)
    char key[MessageStore::kContactKeyLen];
    setKey(key, summaryKey);
    if (r.count >= kMaxRows && !reuseAddedRow(m, findKey(selectedKey))) break;
    int at = findKey(MessageStore::kEveryone);
    if (at < 0) at = r.count;
    insertRow(static_cast<uint8_t>(at));
    setKey(r.keys[at], key);
    labelUnreadRow(m, static_cast<uint8_t>(at));
    // A name already shown: tell the rows apart by id.
    size_t len = strlen(r.labels[at]);
    bool suffixNew = false;
    for (uint8_t j = 0; j < r.count; j++) {
      if (j == at || strncmp(r.labels[j], r.labels[at], len) != 0) continue;
      if (r.labels[j][len] == '\0') {
        appendDeviceSuffix(r.labels[j], kLabelLen, r.keys[j]);
        suffixNew = true;
      } else if (strncmp(r.labels[j] + len, " [", 2) == 0) {
        suffixNew = true;
      }
    }
    if (suffixNew) appendDeviceSuffix(r.labels[at], kLabelLen, r.keys[at]);
    changed = true;
  }
  if (!changed) return kNoChange;
  bindItems(m);
  int sel = findKey(selectedKey);
  return static_cast<uint8_t>(sel >= 0 ? sel : 0);
}

// Entry: builds every row afresh.
inline void build(const Mode& m) {
  Storage& r = rows();
  r.count = 0;
  Presence::OnlineContact online[kMaxOnline];
  uint8_t onlineN = Presence::getOnlineContacts(m.group, online, kMaxOnline);
  for (uint8_t i = 0; i < onlineN; i++) {
    setKey(r.keys[r.count], online[i].device_id);
    snprintf(r.labels[r.count], kLabelLen, "%s", online[i].display_name);
    r.count++;
  }

  Presence::RecentContact recent[kMaxRecent];
  uint8_t recentN = Presence::getRecentContacts(m.group, recent, kMaxRecent);
  for (uint8_t i = 0; i < recentN; i++) {
    bool alreadyOnline = false;
    for (uint8_t j = 0; j < onlineN; j++) {
      if (strcmp(online[j].device_id, recent[i].device_id) == 0) {
        alreadyOnline = true;
        break;
      }
    }
    if (alreadyOnline) continue;
    setKey(r.keys[r.count], recent[i].device_id);
    const char* name = (recent[i].last_known_name[0] != '\0') ? recent[i].last_known_name : recent[i].device_id;
    snprintf(r.labels[r.count], kLabelLen, "%s (offline)", name);
    r.count++;
  }

  // Disambiguate duplicate display names with the last 4 hex of device_id.
  for (uint8_t i = 0; i < r.count; i++) {
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < r.count; j++) {
      if (strcmp(r.labels[i], r.labels[j]) == 0) {
        appendDeviceSuffix(r.labels[i], kLabelLen, r.keys[i]);
        appendDeviceSuffix(r.labels[j], kLabelLen, r.keys[j]);
      }
    }
  }
  r.ordinaryEnd = r.count;  // at most kMaxOnline + kMaxRecent: Everyone always fits

  setKey(r.keys[r.count], MessageStore::kEveryone);
  snprintf(r.labels[r.count], kLabelLen, "Everyone");
  r.count++;
  bindItems(m);
  refresh(m, 0);  // then the unread conversations not listed above
}

}  // namespace RecipientRows
