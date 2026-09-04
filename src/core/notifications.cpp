#include "core/notifications.h"

#include <Arduino.h>
#include <string.h>

#include "core/hooks.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/sleep.h"
#include "core/sound_facade.h"
#include "core/storage_init.h"

namespace Notifications {

namespace {

constexpr uint8_t kMaxSummaries = 24;

struct Summary {
  bool active;
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  uint16_t unread_count;
  uint8_t badge_mask;
};

Summary g_summaries[kMaxSummaries];

void saveSummaries() { Storage::notify().putBytes("summary", g_summaries, sizeof(g_summaries)); }

void loadSummaries() {
  size_t got = Storage::notify().getBytes("summary", g_summaries, sizeof(g_summaries));
  if (got != sizeof(g_summaries)) memset(g_summaries, 0, sizeof(g_summaries));
}

Summary* findSummary(const char* group_code, const char* contact_key) {
  for (auto& s : g_summaries) {
    if (s.active && strcmp(s.group_code, group_code) == 0 && strcmp(s.contact_key, contact_key) == 0) return &s;
  }
  return nullptr;
}

Summary* findOrCreateSummary(const char* group_code, const char* contact_key) {
  Summary* s = findSummary(group_code, contact_key);
  if (s != nullptr) return s;
  for (auto& c : g_summaries) {
    if (!c.active) {
      c.active = true;
      strncpy(c.group_code, group_code, sizeof(c.group_code) - 1);
      c.group_code[sizeof(c.group_code) - 1] = '\0';
      strncpy(c.contact_key, contact_key, sizeof(c.contact_key) - 1);
      c.contact_key[sizeof(c.contact_key) - 1] = '\0';
      c.unread_count = 0;
      c.badge_mask = 0;
      return &c;
    }
  }
  // Pool exhausted (very unlikely: 24 concurrent unread conversations):
  // reuse whichever slot currently has no unread messages, or give up.
  for (auto& c : g_summaries) {
    if (c.unread_count == 0) {
      c.active = true;
      strncpy(c.group_code, group_code, sizeof(c.group_code) - 1);
      c.group_code[sizeof(c.group_code) - 1] = '\0';
      strncpy(c.contact_key, contact_key, sizeof(c.contact_key) - 1);
      c.contact_key[sizeof(c.contact_key) - 1] = '\0';
      c.badge_mask = 0;
      return &c;
    }
  }
  return nullptr;
}

bool hasBadge(uint8_t mask) {
  for (auto& s : g_summaries) {
    if (s.active && s.unread_count > 0 && (s.badge_mask & mask)) return true;
  }
  return false;
}

void onSettingsChanged(const SettingsChangeInfo& info) {
  if (info.event != SET_GROUP_DELETED) return;
  bool changed = false;
  for (auto& s : g_summaries) {
    if (s.active && strcmp(s.group_code, info.group_code) == 0) {
      s.active = false;
      changed = true;
    }
  }
  if (changed) saveSummaries();
}

void serviceInit() {
  loadSummaries();
  registerSettingsChangeHook(onSettingsChanged);
  Menu::registerMainMenuBadge(MainMenuIndex::TEXT, hasAnyUnreadText);
  Menu::registerMainMenuBadge(MainMenuIndex::ENIGMA, hasAnyUnreadEnigma);
  Menu::registerMainMenuBadge(MainMenuIndex::TRAINING_GAME, hasAnyUnreadTrainingGame);
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = nullptr;
    registerAppService(svc);
  }
};
Registrar g_registrar;

}  // namespace

void onMessageArrived(const char* group_code, const char* contact_key, const MessageRef& ref,
                      uint8_t badgeMask, bool conversationCurrentlyOpen) {
  if (conversationCurrentlyOpen) {
    MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD);
    return;
  }

  MessageStore::updateLocalFlags(ref, MessageStore::FLAG_UNREAD, 0);
  Summary* s = findOrCreateSummary(group_code, contact_key);
  if (s != nullptr) {
    s->unread_count++;
    s->badge_mask = static_cast<uint8_t>(s->badge_mask | badgeMask);
    saveSummaries();
  }

  playTone(1000, 150, SOUND_NOTIFICATION);
  Sleep::notifyActivity();
}

void clearUnread(const char* group_code, const char* contact_key, const MessageRef& ref) {
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  if (!(view.header.flags & MessageStore::FLAG_UNREAD)) return;

  MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_UNREAD);
  Summary* s = findSummary(group_code, contact_key);
  if (s != nullptr && s->unread_count > 0) {
    s->unread_count--;
    saveSummaries();
  }
}

uint16_t getUnreadCount(const char* group_code, const char* contact_key) {
  Summary* s = findSummary(group_code, contact_key);
  return s != nullptr ? s->unread_count : 0;
}

bool hasAnyUnreadText() { return hasBadge(BADGE_TEXT); }
bool hasAnyUnreadEnigma() { return hasBadge(BADGE_ENIGMA); }
bool hasAnyUnreadTrainingGame() { return hasBadge(BADGE_TRAINING_GAME); }

}  // namespace Notifications
