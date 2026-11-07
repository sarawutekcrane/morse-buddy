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
// Capacity: the row arrays hold `cap` rows (41: Everyone + 20 + 20). When the
// ordinary (online/recent) rows and the unread ones do not all fit, each
// unread row added replaces the last row that is neither unread, selected nor
// Everyone, so every unread conversation and Everyone stay reachable. At most
// 24 conversations are tracked, so the unread rows always fit.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/menu.h"
#include "core/notifications.h"
#include "core/presence.h"
#include "core/storage_messages.h"

namespace RecipientRows {

constexpr size_t kLabelLen = 40;
constexpr uint8_t kNoChange = 0xFF;

struct Table {
  SettingItem* items;
  char (*labels)[kLabelLen];
  char (*keys)[MessageStore::kContactKeyLen];
  uint8_t& count;
  uint8_t cap;
  ScreenHandlerFn onSelect;
  const char* group;
  uint8_t badgeMask;  // Notifications::BADGE_TEXT or BADGE_ENIGMA
};

inline int findKey(const Table& t, const char* key) {
  for (uint8_t i = 0; i < t.count; i++)
    if (strcmp(t.keys[i], key) == 0) return i;
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

inline void removeRow(Table& t, uint8_t at) {
  for (uint8_t i = at; i + 1 < t.count; i++) {
    memcpy(t.labels[i], t.labels[i + 1], kLabelLen);
    memcpy(t.keys[i], t.keys[i + 1], MessageStore::kContactKeyLen);
  }
  t.count--;
}

inline void insertRow(Table& t, uint8_t at) {
  for (uint8_t i = t.count; i > at; i--) {
    memcpy(t.labels[i], t.labels[i - 1], kLabelLen);
    memcpy(t.keys[i], t.keys[i - 1], MessageStore::kContactKeyLen);
  }
  t.count++;
}

// Frees one row: the last one that is not Everyone, not `keep` and not
// unread. False if there is none.
inline bool dropReadRow(Table& t, int keep) {
  for (int i = static_cast<int>(t.count) - 1; i >= 0; i--) {
    if (i == keep || strcmp(t.keys[i], MessageStore::kEveryone) == 0) continue;
    if (Notifications::hasUnreadIn(t.group, t.keys[i], t.badgeMask)) continue;
    removeRow(t, static_cast<uint8_t>(i));
    return true;
  }
  return false;
}

inline void labelUnreadRow(const Table& t, uint8_t i) {
  char name[MessageStore::kContactKeyLen + 24];
  Presence::resolveDisplayName(t.group, t.keys[i], name, sizeof(name));
  snprintf(t.labels[i], kLabelLen, Presence::isContactOnline(t.group, t.keys[i]) ? "%s" : "%s (offline)", name);
}

inline void bindItems(Table& t) {
  for (uint8_t i = 0; i < t.count; i++) t.items[i] = SettingItem{t.labels[i], t.onSelect};
}

// While the list is shown: adds a row, before Everyone, for each unread
// conversation not yet listed (e.g. a message from a sender absent at entry).
// Rows are never removed for being read during the visit; only when the rows
// are full does a new unread row replace the last row that is neither
// unread, selected nor Everyone. Returns the selected row's new index (same
// identity) if rows changed, else kNoChange.
inline uint8_t refresh(Table& t, uint8_t selected) {
  char selectedKey[MessageStore::kContactKeyLen];
  setKey(selectedKey, selected < t.count ? t.keys[selected] : "");
  bool changed = false;
  uint8_t cursor = 0;
  const char* summaryKey;
  while ((summaryKey = Notifications::nextUnreadContactIn(t.group, t.badgeMask, cursor)) != nullptr) {
    if (findKey(t, summaryKey) >= 0) continue;  // (Everyone included: it always has its row)
    char key[MessageStore::kContactKeyLen];
    setKey(key, summaryKey);
    if (t.count >= t.cap && !dropReadRow(t, findKey(t, selectedKey))) break;
    int at = findKey(t, MessageStore::kEveryone);
    if (at < 0) at = t.count;
    insertRow(t, static_cast<uint8_t>(at));
    setKey(t.keys[at], key);
    labelUnreadRow(t, static_cast<uint8_t>(at));
    // A name already shown: tell the rows apart by id.
    size_t len = strlen(t.labels[at]);
    bool suffixNew = false;
    for (uint8_t j = 0; j < t.count; j++) {
      if (j == at || strncmp(t.labels[j], t.labels[at], len) != 0) continue;
      if (t.labels[j][len] == '\0') {
        appendDeviceSuffix(t.labels[j], kLabelLen, t.keys[j]);
        suffixNew = true;
      } else if (strncmp(t.labels[j] + len, " [", 2) == 0) {
        suffixNew = true;
      }
    }
    if (suffixNew) appendDeviceSuffix(t.labels[at], kLabelLen, t.keys[at]);
    changed = true;
  }
  if (!changed) return kNoChange;
  bindItems(t);
  int sel = findKey(t, selectedKey);
  return static_cast<uint8_t>(sel >= 0 ? sel : 0);
}

// Entry: builds every row afresh.
inline void build(Table& t) {
  t.count = 0;
  Presence::OnlineContact online[20];
  uint8_t onlineN = Presence::getOnlineContacts(t.group, online, 20);
  for (uint8_t i = 0; i < onlineN && t.count < t.cap; i++) {
    setKey(t.keys[t.count], online[i].device_id);
    snprintf(t.labels[t.count], kLabelLen, "%s", online[i].display_name);
    t.count++;
  }

  Presence::RecentContact recent[20];
  uint8_t recentN = Presence::getRecentContacts(t.group, recent, 20);
  for (uint8_t i = 0; i < recentN && t.count < t.cap; i++) {
    bool alreadyOnline = false;
    for (uint8_t j = 0; j < onlineN; j++) {
      if (strcmp(online[j].device_id, recent[i].device_id) == 0) {
        alreadyOnline = true;
        break;
      }
    }
    if (alreadyOnline) continue;
    setKey(t.keys[t.count], recent[i].device_id);
    const char* name = (recent[i].last_known_name[0] != '\0') ? recent[i].last_known_name : recent[i].device_id;
    snprintf(t.labels[t.count], kLabelLen, "%s (offline)", name);
    t.count++;
  }

  // Disambiguate duplicate display names with the last 4 hex of device_id.
  for (uint8_t i = 0; i < t.count; i++) {
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < t.count; j++) {
      if (strcmp(t.labels[i], t.labels[j]) == 0) {
        appendDeviceSuffix(t.labels[i], kLabelLen, t.keys[i]);
        appendDeviceSuffix(t.labels[j], kLabelLen, t.keys[j]);
      }
    }
  }

  if (t.count >= t.cap) dropReadRow(t, -1);
  if (t.count < t.cap) {
    setKey(t.keys[t.count], MessageStore::kEveryone);
    snprintf(t.labels[t.count], kLabelLen, "Everyone");
    t.count++;
  }
  bindItems(t);
  refresh(t, 0);  // then the unread conversations not listed above
}

}  // namespace RecipientRows
