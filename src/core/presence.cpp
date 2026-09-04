#include "core/presence.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "core/hooks.h"
#include "core/identity.h"
#include "core/mqtt_manager.h"
#include "core/settings.h"
#include "core/storage_init.h"
#include "core/wifi_manager.h"

namespace Presence {

namespace {

// ---------------------------------------------------------------------------
// Per-group online-contact RAM table (Addendum section 5: cap 20/group).
// ---------------------------------------------------------------------------
constexpr uint8_t kMaxOnlinePerGroup = 20;

struct OnlineEntry {
  bool used = false;
  char device_id[13] = {0};
  char display_name[17] = {0};
  bool radio_available = false;
  uint32_t lastObservedMs = 0;
};

struct GroupPresenceTable {
  bool active = false;
  char group_code[33] = {0};
  OnlineEntry online[kMaxOnlinePerGroup];
};

GroupPresenceTable g_tables[Settings::kMaxGroups];

GroupPresenceTable* findTable(const char* group_code) {
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (g_tables[i].active && strcmp(g_tables[i].group_code, group_code) == 0) return &g_tables[i];
  }
  return nullptr;
}

GroupPresenceTable* findOrCreateTable(const char* group_code) {
  GroupPresenceTable* t = findTable(group_code);
  if (t != nullptr) return t;
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (!g_tables[i].active) {
      g_tables[i].active = true;
      strncpy(g_tables[i].group_code, group_code, sizeof(g_tables[i].group_code) - 1);
      g_tables[i].group_code[sizeof(g_tables[i].group_code) - 1] = '\0';
      for (auto& e : g_tables[i].online) e.used = false;
      return &g_tables[i];
    }
  }
  return nullptr;  // all 5 slots active; should not happen (max 5 groups exist)
}

OnlineEntry* findOnlineEntry(GroupPresenceTable* t, const char* device_id) {
  for (auto& e : t->online) {
    if (e.used && strcmp(e.device_id, device_id) == 0) return &e;
  }
  return nullptr;
}

OnlineEntry* claimOnlineSlot(GroupPresenceTable* t, const char* device_id) {
  OnlineEntry* target = nullptr;
  for (auto& e : t->online) {
    if (!e.used) {
      target = &e;
      break;
    }
  }
  if (target == nullptr) {
    // Full: retain the 20 most recently observed (Addendum section 5).
    target = &t->online[0];
    for (auto& e : t->online) {
      if (e.lastObservedMs < target->lastObservedMs) target = &e;
    }
  }
  target->used = true;
  strncpy(target->device_id, device_id, sizeof(target->device_id) - 1);
  target->device_id[sizeof(target->device_id) - 1] = '\0';
  target->display_name[0] = '\0';
  target->radio_available = false;
  return target;
}

// ---------------------------------------------------------------------------
// Recent Contacts cache (Addendum section 5), persisted as one blob in
// mb_recent. Max 20 per group; oldest by last_seen_timestamp evicted.
// ---------------------------------------------------------------------------
constexpr uint8_t kMaxRecentPerGroup = 20;

struct RecentContactRecord {
  bool used;
  char device_id[13];
  char last_known_name[17];
  uint32_t last_seen_timestamp;
};

struct RecentContactsForGroup {
  bool active;
  char group_code[33];
  RecentContactRecord contacts[kMaxRecentPerGroup];
};

RecentContactsForGroup g_recent[Settings::kMaxGroups];

void saveRecentTables() { Storage::recent().putBytes("recent", g_recent, sizeof(g_recent)); }

void loadRecentTables() {
  size_t got = Storage::recent().getBytes("recent", g_recent, sizeof(g_recent));
  if (got != sizeof(g_recent)) memset(g_recent, 0, sizeof(g_recent));
}

RecentContactsForGroup* findRecentTable(const char* group_code) {
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (g_recent[i].active && strcmp(g_recent[i].group_code, group_code) == 0) return &g_recent[i];
  }
  return nullptr;
}

RecentContactsForGroup* findOrCreateRecentTable(const char* group_code) {
  RecentContactsForGroup* t = findRecentTable(group_code);
  if (t != nullptr) return t;
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (!g_recent[i].active) {
      g_recent[i].active = true;
      strncpy(g_recent[i].group_code, group_code, sizeof(g_recent[i].group_code) - 1);
      g_recent[i].group_code[sizeof(g_recent[i].group_code) - 1] = '\0';
      for (auto& c : g_recent[i].contacts) c.used = false;
      return &g_recent[i];
    }
  }
  return nullptr;
}

RecentContactRecord* findRecentRecord(RecentContactsForGroup* t, const char* device_id) {
  for (auto& c : t->contacts) {
    if (c.used && strcmp(c.device_id, device_id) == 0) return &c;
  }
  return nullptr;
}

RecentContactRecord* claimRecentSlot(RecentContactsForGroup* t, const char* device_id) {
  RecentContactRecord* target = nullptr;
  for (auto& c : t->contacts) {
    if (!c.used) {
      target = &c;
      break;
    }
  }
  if (target == nullptr) {
    target = &t->contacts[0];
    for (auto& c : t->contacts) {
      if (c.last_seen_timestamp < target->last_seen_timestamp) target = &c;
    }
  }
  target->used = true;
  strncpy(target->device_id, device_id, sizeof(target->device_id) - 1);
  target->device_id[sizeof(target->device_id) - 1] = '\0';
  target->last_known_name[0] = '\0';
  target->last_seen_timestamp = 0;
  return target;
}

// ONLINE presence observation: may refresh the cached display name.
void touchRecentOnline(const char* group_code, const char* device_id, const char* name, uint32_t timestamp) {
  RecentContactsForGroup* t = findOrCreateRecentTable(group_code);
  if (t == nullptr) return;
  RecentContactRecord* r = findRecentRecord(t, device_id);
  if (r == nullptr) r = claimRecentSlot(t, device_id);
  strncpy(r->last_known_name, name, sizeof(r->last_known_name) - 1);
  r->last_known_name[sizeof(r->last_known_name) - 1] = '\0';
  r->last_seen_timestamp = timestamp;
  saveRecentTables();
}

// OFFLINE/LWT observation: last-seen only, never overwrites the cached name
// with a potentially-stale LWT display_name (Addendum section 6.3).
void touchRecentOfflineOnly(const char* group_code, const char* device_id, uint32_t timestamp) {
  RecentContactsForGroup* t = findOrCreateRecentTable(group_code);
  if (t == nullptr) return;
  RecentContactRecord* r = findRecentRecord(t, device_id);
  if (r == nullptr) r = claimRecentSlot(t, device_id);
  r->last_seen_timestamp = timestamp;
  saveRecentTables();
}

bool g_ownRadioAvailable = true;

void buildPayload(char* out, size_t outSize, const char* status) {
  uint32_t ts = WifiManager::getUnixTime();
  snprintf(out, outSize, "MBP1|%s|%s|%s|%d|%lu", Identity::deviceId(), Settings::getMyName(), status,
           g_ownRadioAvailable ? 1 : 0, static_cast<unsigned long>(ts));
}

void publishStatus(const char* group_code, const char* status) {
  char payload[110];
  buildPayload(payload, sizeof(payload), status);
  char topic[48];
  snprintf(topic, sizeof(topic), "presence/%s", Identity::deviceId());
  MqttManager::publishRaw(group_code, topic, payload, /*retained=*/true, /*qos=*/1);
}

void onSettingsChanged(const SettingsChangeInfo& info) {
  if (info.event != SET_GROUP_DELETED) return;
  GroupPresenceTable* t = findTable(info.group_code);
  if (t != nullptr) t->active = false;
  RecentContactsForGroup* rt = findRecentTable(info.group_code);
  if (rt != nullptr) {
    rt->active = false;
    saveRecentTables();
  }
}

void serviceInit() {
  loadRecentTables();
  registerSettingsChangeHook(onSettingsChanged);
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

void handleIncoming(const char* group_code, const char* payload, uint16_t len) {
  char buf[110];
  uint16_t copyLen = (len < sizeof(buf) - 1) ? len : static_cast<uint16_t>(sizeof(buf) - 1);
  memcpy(buf, payload, copyLen);
  buf[copyLen] = '\0';

  char* saveptr = nullptr;
  char* tag = strtok_r(buf, "|", &saveptr);
  if (tag == nullptr || strcmp(tag, "MBP1") != 0) return;
  char* deviceId = strtok_r(nullptr, "|", &saveptr);
  char* displayName = strtok_r(nullptr, "|", &saveptr);
  char* status = strtok_r(nullptr, "|", &saveptr);
  char* radioStr = strtok_r(nullptr, "|", &saveptr);
  if (deviceId == nullptr || displayName == nullptr || status == nullptr) return;
  if (strcmp(deviceId, Identity::deviceId()) == 0) return;  // exclude self

  bool online = (strcmp(status, "ONLINE") == 0);
  bool radioAvail = (radioStr != nullptr) && (atoi(radioStr) != 0);
  uint32_t receiptTs = WifiManager::getUnixTime();

  GroupPresenceTable* t = findOrCreateTable(group_code);
  if (t == nullptr) return;

  if (online) {
    OnlineEntry* e = findOnlineEntry(t, deviceId);
    if (e == nullptr) e = claimOnlineSlot(t, deviceId);
    strncpy(e->display_name, displayName, sizeof(e->display_name) - 1);
    e->display_name[sizeof(e->display_name) - 1] = '\0';
    e->radio_available = radioAvail;
    e->lastObservedMs = millis();
    touchRecentOnline(group_code, deviceId, displayName, receiptTs);
  } else {
    OnlineEntry* e = findOnlineEntry(t, deviceId);
    if (e != nullptr) e->used = false;
    touchRecentOfflineOnly(group_code, deviceId, receiptTs);
  }
}

void publishOnline(const char* group_code) { publishStatus(group_code, "ONLINE"); }
void publishOffline(const char* group_code) { publishStatus(group_code, "OFFLINE"); }

bool isContactOnline(const char* group_code, const char* device_id) {
  GroupPresenceTable* t = findTable(group_code);
  return t != nullptr && findOnlineEntry(t, device_id) != nullptr;
}

bool isContactRadioAvailable(const char* group_code, const char* device_id) {
  GroupPresenceTable* t = findTable(group_code);
  if (t == nullptr) return false;
  OnlineEntry* e = findOnlineEntry(t, device_id);
  return e != nullptr && e->radio_available;
}

void setOwnRadioAvailable(bool available) { g_ownRadioAvailable = available; }

void republishOwnPresenceAllGroups() {
  uint8_t n = Settings::getGroupCount();
  for (uint8_t i = 0; i < n; i++) {
    Settings::FamilyGroup g = Settings::getGroup(i);
    if (MqttManager::isGroupConnected(g.code)) publishOnline(g.code);
  }
}

uint8_t getOnlineContacts(const char* group_code, OnlineContact* outArr, uint8_t capacity) {
  GroupPresenceTable* t = findTable(group_code);
  if (t == nullptr) return 0;
  uint8_t n = 0;
  for (auto& e : t->online) {
    if (e.used && n < capacity) {
      strncpy(outArr[n].device_id, e.device_id, sizeof(outArr[n].device_id) - 1);
      outArr[n].device_id[sizeof(outArr[n].device_id) - 1] = '\0';
      strncpy(outArr[n].display_name, e.display_name, sizeof(outArr[n].display_name) - 1);
      outArr[n].display_name[sizeof(outArr[n].display_name) - 1] = '\0';
      outArr[n].radio_available = e.radio_available;
      n++;
    }
  }
  for (uint8_t i = 1; i < n; i++) {
    OnlineContact key = outArr[i];
    int32_t j = static_cast<int32_t>(i) - 1;
    while (j >= 0) {
      int cmp = strcmp(outArr[j].display_name, key.display_name);
      if (cmp < 0 || (cmp == 0 && strcmp(outArr[j].device_id, key.device_id) <= 0)) break;
      outArr[j + 1] = outArr[j];
      j--;
    }
    outArr[j + 1] = key;
  }
  return n;
}

uint8_t getRecentContacts(const char* group_code, RecentContact* outArr, uint8_t capacity) {
  RecentContactsForGroup* t = findRecentTable(group_code);
  if (t == nullptr) return 0;
  uint8_t n = 0;
  for (auto& c : t->contacts) {
    if (c.used && n < capacity) {
      strncpy(outArr[n].device_id, c.device_id, sizeof(outArr[n].device_id) - 1);
      outArr[n].device_id[sizeof(outArr[n].device_id) - 1] = '\0';
      strncpy(outArr[n].last_known_name, c.last_known_name, sizeof(outArr[n].last_known_name) - 1);
      outArr[n].last_known_name[sizeof(outArr[n].last_known_name) - 1] = '\0';
      outArr[n].last_seen_timestamp = c.last_seen_timestamp;
      n++;
    }
  }
  // Most-recently-seen first.
  for (uint8_t i = 1; i < n; i++) {
    RecentContact key = outArr[i];
    int32_t j = static_cast<int32_t>(i) - 1;
    while (j >= 0 && outArr[j].last_seen_timestamp < key.last_seen_timestamp) {
      outArr[j + 1] = outArr[j];
      j--;
    }
    outArr[j + 1] = key;
  }
  return n;
}

}  // namespace Presence
