#include "core/presence.h"

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>

#include "core/hooks.h"
#include "core/identity.h"
#include "core/identity_color.h"
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
  // Feature Fix #4.8: kInvalidColor until an ONLINE payload with a valid
  // trailing color field is observed for this device.
  uint8_t color_index = IdentityColor::kInvalidColor;
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
  target->color_index = IdentityColor::kInvalidColor;
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

// ---------------------------------------------------------------------------
// Persisted personal-color cache (Feature Fix #4.8 section 2K), kept as its
// own SEPARATE blob under the same mb_recent namespace ("colors" key) --
// deliberately not folded into RecentContactRecord/the existing "recent"
// blob, so that blob's layout/size never changes just to add one byte,
// preserving every existing device's persisted Recent Contacts across this
// firmware upgrade. Bounded the same way (kMaxGroups groups x
// kMaxRecentPerGroup devices/group) and evicted by the same
// least-recently-touched policy, but keyed purely by (group_code,
// device_id) -- last_seen_timestamp isn't meaningful for a color cache, so
// eviction here simply reuses the first unused slot, or (when full) an
// arbitrary occupied one; losing one rarely-touched cached color under
// long-term churn is a purely cosmetic degradation (falls back to CYAN),
// never a correctness issue.
struct RecentColorRecord {
  bool used;
  char device_id[13];
  uint8_t color_index;
};

struct RecentColorsForGroup {
  bool active;
  char group_code[33];
  RecentColorRecord contacts[kMaxRecentPerGroup];
};

// Memory Fix #4.9c: this cache no longer lives in static .bss (it was
// contributing 1.6KB+ toward a DRAM segment overflow) -- only a small
// pointer is static, with the actual Settings::kMaxGroups-element array
// allocated once, on first use, from the ordinary internal heap. This is
// the same "small pointer in .bss, backing storage allocated once, no
// per-frame allocation" pattern already used successfully by UiScratch.
// The persisted NVS blob's layout is completely unaffected -- it is still
// exactly Settings::kMaxGroups RecentColorsForGroup records, byte-for-byte
// identical to what Feature Fix #4.8 originally wrote.
RecentColorsForGroup* g_recentColors = nullptr;

// Exact byte length of the logical array, independent of how it happens to
// be stored -- every getBytes()/putBytes()/memset() call below uses this,
// NEVER sizeof(g_recentColors) (which, now that g_recentColors is a
// pointer, would silently shrink to the pointer's own size instead of the
// array it points to).
constexpr size_t kRecentColorsBytes = sizeof(RecentColorsForGroup) * Settings::kMaxGroups;

// Allocates the backing array the first time it's needed and leaves it
// allocated for the rest of the program's life -- never freed or
// reallocated during normal runtime. calloc() zero-initializes it, so a
// freshly allocated cache starts in exactly the same all-zero state the
// old static array began in before any NVS load. Safe to call repeatedly:
// a prior successful allocation short-circuits immediately, and a prior
// failed attempt can be retried later from normal task context.
bool ensureRecentColorsStorage() {
  if (g_recentColors != nullptr) return true;
  g_recentColors = static_cast<RecentColorsForGroup*>(calloc(Settings::kMaxGroups, sizeof(RecentColorsForGroup)));
  return g_recentColors != nullptr;
}

void saveRecentColorsTables() {
  // Nothing allocated (e.g. the one-time heap allocation never succeeded)
  // -- there is nothing meaningful to persist yet; never dereference a
  // null backing pointer.
  if (g_recentColors == nullptr) return;
  Storage::recent().putBytes("colors", g_recentColors, kRecentColorsBytes);
}

void loadRecentColorsTables() {
  // Feature Fix #4.8's offline color cache is purely cosmetic metadata
  // (Presence/messaging/identity all keep working without it, falling
  // back to CYAN) -- a failed allocation here must never crash or block
  // boot, it simply leaves the cache unavailable until a later successful
  // ensureRecentColorsStorage() call.
  if (!ensureRecentColorsStorage()) return;
  size_t got = Storage::recent().getBytes("colors", g_recentColors, kRecentColorsBytes);
  if (got != kRecentColorsBytes) memset(g_recentColors, 0, kRecentColorsBytes);
}

RecentColorsForGroup* findRecentColorsTable(const char* group_code) {
  // Memory Fix #4.9c: safe to call even if the heap allocation never
  // happened/succeeded -- every existing caller already null-checks this
  // function's return value (it could already return nullptr for "no
  // table found"), so this simply becomes another way to reach that same,
  // already-handled outcome.
  if (g_recentColors == nullptr) return nullptr;
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (g_recentColors[i].active && strcmp(g_recentColors[i].group_code, group_code) == 0) return &g_recentColors[i];
  }
  return nullptr;
}

RecentColorsForGroup* findOrCreateRecentColorsTable(const char* group_code) {
  RecentColorsForGroup* t = findRecentColorsTable(group_code);
  if (t != nullptr) return t;
  // Memory Fix #4.9c: retry the one-time allocation here too, in case the
  // boot-time attempt (loadRecentColorsTables()) ever failed -- a later
  // successful allocation from normal task context still lets the cache
  // start working, rather than staying permanently disabled for the rest
  // of the session.
  if (!ensureRecentColorsStorage()) return nullptr;
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (!g_recentColors[i].active) {
      g_recentColors[i].active = true;
      strncpy(g_recentColors[i].group_code, group_code, sizeof(g_recentColors[i].group_code) - 1);
      g_recentColors[i].group_code[sizeof(g_recentColors[i].group_code) - 1] = '\0';
      for (auto& c : g_recentColors[i].contacts) c.used = false;
      return &g_recentColors[i];
    }
  }
  return nullptr;
}

RecentColorRecord* findRecentColorRecord(RecentColorsForGroup* t, const char* device_id) {
  for (auto& c : t->contacts) {
    if (c.used && strcmp(c.device_id, device_id) == 0) return &c;
  }
  return nullptr;
}

RecentColorRecord* claimRecentColorSlot(RecentColorsForGroup* t, const char* device_id) {
  RecentColorRecord* target = nullptr;
  for (auto& c : t->contacts) {
    if (!c.used) {
      target = &c;
      break;
    }
  }
  if (target == nullptr) target = &t->contacts[0];  // full: reuse the first slot (see comment above)
  target->used = true;
  strncpy(target->device_id, device_id, sizeof(target->device_id) - 1);
  target->device_id[sizeof(target->device_id) - 1] = '\0';
  target->color_index = IdentityColor::kInvalidColor;
  return target;
}

// Only ever called with a VALID color (see call sites) -- an incoming
// legacy/no-color payload leaves whatever was already cached untouched
// rather than overwriting known-good data with "unknown" (section 2K:
// "preserve UNKNOWN unless a valid newer value is received", which is
// really "preserve whatever is cached unless a valid newer value replaces
// it"). Persists only on an actual change, never every frame.
void touchRecentColor(const char* group_code, const char* device_id, uint8_t color_index) {
  RecentColorsForGroup* t = findOrCreateRecentColorsTable(group_code);
  if (t == nullptr) return;
  RecentColorRecord* r = findRecentColorRecord(t, device_id);
  if (r == nullptr) r = claimRecentColorSlot(t, device_id);
  if (r->color_index == color_index) return;  // no actual change -- skip the NVS write
  r->color_index = color_index;
  saveRecentColorsTables();
}

// Feature Fix #4.8a: strict decimal-integer parser for the optional
// trailing Presence color-index token -- deliberately NOT atoi(), which
// would silently accept "4x" as 4, "" as 0, or a negative-looking string
// as some wrapped unsigned value. Accepts ONLY a complete run of decimal
// digits (no sign, no leading/trailing garbage, not empty) whose value
// falls in [0, IdentityColor::kColorCount); writes *outIndex and returns
// true on success, otherwise returns false and leaves *outIndex
// completely untouched (callers pre-set it to IdentityColor::kInvalidColor
// so a rejected/missing token never overwrites an already-known color).
bool parseStrictColorIndex(const char* s, uint8_t* outIndex) {
  if (s == nullptr || s[0] == '\0') return false;
  uint32_t value = 0;
  for (const char* p = s; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') return false;
    value = value * 10 + static_cast<uint32_t>(*p - '0');
    if (value >= IdentityColor::kColorCount) return false;  // also bounds runaway accumulation on a long digit run
  }
  *outIndex = static_cast<uint8_t>(value);
  return true;
}

bool g_ownRadioAvailable = true;

void buildPayload(char* out, size_t outSize, const char* status) {
  uint32_t ts = WifiManager::getUnixTime();
  // Hardware Fix #4.3a issue 2: same fallback policy as
  // MessageStore::buildSenderPrefix()/Presence::resolveDisplayName()'s self
  // branch -- a never-configured device broadcasts its own device id in
  // this field instead of the compiled "Me" placeholder, so remote devices
  // never cache and display "Me" as if it were this device's chosen name.
  // Packet format (field count/order/delimiters) is unchanged.
  const char* name = Settings::hasCustomMyName() ? Settings::getMyName() : Identity::deviceId();
  // Feature Fix #4.8 section 2H: an optional trailing color-index field,
  // APPENDED after the existing 5 fields -- the tag stays "MBP1" since
  // this is a backward-compatible optional field a legacy receiver
  // already ignores (it only reads the leading fields it recognizes).
  // IdentityColor::kInvalidColor (0xFF, printed as "255") is sent as-is
  // when no color is configured yet -- never a fabricated color.
  snprintf(out, outSize, "MBP1|%s|%s|%s|%d|%lu|%u", Identity::deviceId(), name, status, g_ownRadioAvailable ? 1 : 0,
           static_cast<unsigned long>(ts), Settings::getMyColorIndex());
}

void publishStatus(const char* group_code, const char* status) {
  char payload[110];
  buildPayload(payload, sizeof(payload), status);
  char topic[48];
  snprintf(topic, sizeof(topic), "presence/%s", Identity::deviceId());
  MqttManager::publishRaw(group_code, topic, payload, /*retained=*/true, /*qos=*/1);
}

void onSettingsChanged(const SettingsChangeInfo& info) {
  if (info.event == SET_MY_NAME_CHANGED) {
    // Hardware Fix #4.7a: a device can have WiFi already configured but no
    // valid name yet, so it may already be publishing presence under its
    // temporary device-ID identity (buildPayload()'s existing
    // !hasCustomMyName() fallback) before Set Name is completed. Without
    // this, peers would keep showing that device-ID identity until some
    // unrelated reconnect/publish happened to occur -- republishing
    // immediately on every name change (first-time save or a later rename
    // via the regular My Name editor) means every configured group's
    // peers see the real chosen name right away, no reconnect required.
    // republishOwnPresenceAllGroups() is declared in presence.h, included
    // at the top of this file, so this unqualified call resolves via the
    // enclosing Presence namespace even though its definition appears
    // later in this same file -- no forward declaration needed.
    republishOwnPresenceAllGroups();
    return;
  }
  if (info.event != SET_GROUP_DELETED) return;
  GroupPresenceTable* t = findTable(info.group_code);
  if (t != nullptr) t->active = false;
  RecentContactsForGroup* rt = findRecentTable(info.group_code);
  if (rt != nullptr) {
    rt->active = false;
    saveRecentTables();
  }
  // Feature Fix #4.8: the persisted recent-color cache is cleaned up
  // alongside the existing Recent Contacts cleanup above, on the exact
  // same group-delete event.
  RecentColorsForGroup* ct = findRecentColorsTable(info.group_code);
  if (ct != nullptr) {
    ct->active = false;
    saveRecentColorsTables();
  }
}

void serviceInit() {
  loadRecentTables();
  loadRecentColorsTables();
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
  // Feature Fix #4.8a: the wire timestamp token occupies the 6th field --
  // it MUST be consumed here, in its real position, before the OPTIONAL
  // 7th (trailing color-index) field. #4.8's original parser skipped
  // straight from radioStr to what it called colorStr, which was actually
  // still reading the TIMESTAMP token; the real color field was never
  // parsed at all. This value remains semantically unused by Presence,
  // exactly as before this fix -- receiptTs (WifiManager's own clock,
  // just below) is still what Recent Contacts/color are stamped with, not
  // the sender's own wire timestamp.
  char* timestampStr = strtok_r(nullptr, "|", &saveptr);
  (void)timestampStr;  // consumed only to advance past it to the real color token
  // OPTIONAL trailing color-index field. Its own strtok_r() call above
  // (timestampStr) already advances past the timestamp whether or not a
  // 7th field follows, so colorStr is simply nullptr for any legacy
  // peer's shorter payload -- never rejected, just treated as "no color
  // observed" (colorIdx stays IdentityColor::kInvalidColor).
  char* colorStr = strtok_r(nullptr, "|", &saveptr);
  if (deviceId == nullptr || displayName == nullptr || status == nullptr) return;
  if (strcmp(deviceId, Identity::deviceId()) == 0) return;  // exclude self

  bool online = (strcmp(status, "ONLINE") == 0);
  bool radioAvail = (radioStr != nullptr) && (atoi(radioStr) != 0);
  // Feature Fix #4.8a: strict parsing -- accepts ONLY a complete decimal
  // integer in [0, kColorCount) (see parseStrictColorIndex() above), never
  // a loose atoi() that would silently turn "abc"/"4x"/"" into color 0.
  // Leaves colorIdx at kInvalidColor (untouched) for anything else,
  // including a missing token, a negative-looking string, an out-of-range
  // value, or trailing garbage after otherwise-valid digits.
  uint8_t colorIdx = IdentityColor::kInvalidColor;
  parseStrictColorIndex(colorStr, &colorIdx);
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
    // Feature Fix #4.8: a legacy/no-color ONLINE payload leaves the
    // already-cached online color_index alone (it stays whatever it was,
    // e.g. kInvalidColor if never observed) instead of stomping a
    // previously-observed valid color back to unknown.
    if (IdentityColor::isValid(colorIdx)) e->color_index = colorIdx;
    touchRecentOnline(group_code, deviceId, displayName, receiptTs);
    if (IdentityColor::isValid(colorIdx)) touchRecentColor(group_code, deviceId, colorIdx);
  } else {
    OnlineEntry* e = findOnlineEntry(t, deviceId);
    if (e != nullptr) e->used = false;
    touchRecentOfflineOnly(group_code, deviceId, receiptTs);
    // OFFLINE/LWT: same "never overwrite with unknown" rule as the name
    // cache above -- only ever refresh the recent color cache when this
    // OFFLINE payload actually carries a valid one (e.g. an LWT that was
    // set with a configured color).
    if (IdentityColor::isValid(colorIdx)) touchRecentColor(group_code, deviceId, colorIdx);
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
      outArr[n].color_index = e.color_index;
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

void resolveDisplayName(const char* group_code, const char* device_id, char* out, size_t outSize) {
  if (strcmp(device_id, Identity::deviceId()) == 0) {
    // Hardware Fix #4.3 issue F: only resolve to a real, user-chosen name.
    // A never-configured device falls back to its own device id, the same
    // fallback MessageStore::buildSenderPrefix() already uses for a peer
    // with no cached name, instead of showing the compiled "Me" default as
    // if it were this device's actual chosen name (e.g. in a Race Room
    // participant list's own "(You)" row).
    const char* name = Settings::hasCustomMyName() ? Settings::getMyName() : Identity::deviceId();
    strncpy(out, name, outSize - 1);
    out[outSize - 1] = '\0';
    return;
  }

  GroupPresenceTable* t = findTable(group_code);
  if (t != nullptr) {
    OnlineEntry* e = findOnlineEntry(t, device_id);
    if (e != nullptr && e->display_name[0] != '\0') {
      strncpy(out, e->display_name, outSize - 1);
      out[outSize - 1] = '\0';
      return;
    }
  }

  RecentContactsForGroup* rt = findRecentTable(group_code);
  if (rt != nullptr) {
    for (auto& c : rt->contacts) {
      if (c.used && strcmp(c.device_id, device_id) == 0 && c.last_known_name[0] != '\0') {
        strncpy(out, c.last_known_name, outSize - 1);
        out[outSize - 1] = '\0';
        return;
      }
    }
  }

  strncpy(out, device_id, outSize - 1);
  out[outSize - 1] = '\0';
}

// Feature Fix #4.8: mirrors resolveDisplayName()'s exact fallback shape
// (self -> current online -> persisted recent/offline) but for the
// sender's chosen personal color instead of their name. Read-only.
uint8_t resolveColorIndex(const char* group_code, const char* device_id) {
  if (strcmp(device_id, Identity::deviceId()) == 0) return Settings::getMyColorIndex();

  GroupPresenceTable* t = findTable(group_code);
  if (t != nullptr) {
    OnlineEntry* e = findOnlineEntry(t, device_id);
    if (e != nullptr && IdentityColor::isValid(e->color_index)) return e->color_index;
  }

  RecentColorsForGroup* rt = findRecentColorsTable(group_code);
  if (rt != nullptr) {
    RecentColorRecord* r = findRecentColorRecord(rt, device_id);
    if (r != nullptr && IdentityColor::isValid(r->color_index)) return r->color_index;
  }

  return IdentityColor::kInvalidColor;
}

bool isNameInUseByKnownMember(const char* candidateName, char* conflictingGroupCode, size_t outSize) {
  if (candidateName == nullptr) return false;
  uint8_t groupCount = Settings::getGroupCount();
  for (uint8_t gi = 0; gi < groupCount; gi++) {
    Settings::FamilyGroup g = Settings::getGroup(gi);
    if (!g.configured) continue;

    GroupPresenceTable* t = findTable(g.code);
    if (t != nullptr) {
      for (auto& e : t->online) {
        if (e.used && strcmp(e.device_id, Identity::deviceId()) != 0 && strcmp(e.display_name, candidateName) == 0) {
          if (conflictingGroupCode != nullptr) {
            strncpy(conflictingGroupCode, g.code, outSize - 1);
            conflictingGroupCode[outSize - 1] = '\0';
          }
          return true;
        }
      }
    }

    RecentContactsForGroup* rt = findRecentTable(g.code);
    if (rt != nullptr) {
      for (auto& c : rt->contacts) {
        if (c.used && strcmp(c.device_id, Identity::deviceId()) != 0 &&
            strcmp(c.last_known_name, candidateName) == 0) {
          if (conflictingGroupCode != nullptr) {
            strncpy(conflictingGroupCode, g.code, outSize - 1);
            conflictingGroupCode[outSize - 1] = '\0';
          }
          return true;
        }
      }
    }
  }
  return false;
}

namespace {
uint8_t countSetBits(uint16_t mask) {
  uint8_t n = 0;
  while (mask != 0) {
    n = static_cast<uint8_t>(n + (mask & 1));
    mask = static_cast<uint16_t>(mask >> 1);
  }
  return n;
}
}  // namespace

bool isColorInUseByKnownMember(uint8_t candidateColor, char* conflictingGroupCode, size_t outSize) {
  if (!IdentityColor::isValid(candidateColor)) return false;
  uint8_t groupCount = Settings::getGroupCount();
  for (uint8_t gi = 0; gi < groupCount; gi++) {
    Settings::FamilyGroup g = Settings::getGroup(gi);
    if (!g.configured) continue;

    // A bitmask of every DISTINCT color currently occupied by a known
    // OTHER member of this group -- ORing bits from both the online and
    // recent tables is naturally immune to one device appearing in both
    // (the same bit is simply set twice), so this never double-counts a
    // single device the way a per-device tally would (section 2R).
    uint16_t occupiedMask = 0;
    GroupPresenceTable* t = findTable(g.code);
    if (t != nullptr) {
      for (auto& e : t->online) {
        if (e.used && strcmp(e.device_id, Identity::deviceId()) != 0 && IdentityColor::isValid(e.color_index)) {
          occupiedMask = static_cast<uint16_t>(occupiedMask | (1u << e.color_index));
        }
      }
    }
    RecentColorsForGroup* rt = findRecentColorsTable(g.code);
    if (rt != nullptr) {
      for (auto& c : rt->contacts) {
        if (c.used && strcmp(c.device_id, Identity::deviceId()) != 0 && IdentityColor::isValid(c.color_index)) {
          occupiedMask = static_cast<uint16_t>(occupiedMask | (1u << c.color_index));
        }
      }
    }

    bool candidateUsed = (occupiedMask & (1u << candidateColor)) != 0;
    if (!candidateUsed) continue;
    // Section 2G/2J: only a conflict while an unused palette color still
    // exists for this group -- once membership already occupies all 12,
    // color reuse is allowed rather than blocking additional members.
    if (countSetBits(occupiedMask) < IdentityColor::kColorCount) {
      if (conflictingGroupCode != nullptr) {
        strncpy(conflictingGroupCode, g.code, outSize - 1);
        conflictingGroupCode[outSize - 1] = '\0';
      }
      return true;
    }
  }
  return false;
}

}  // namespace Presence
