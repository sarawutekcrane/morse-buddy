#pragma once
// Presence (Addendum sections 6.3, 6.4; Phase 2 section 3). Owns the
// per-group online-contact RAM table and the persisted Recent Contacts
// cache (mb_recent), and publishes/parses the fixed MBP1 ASCII payload —
// the one deliberate exception to the binary codec (Addendum 1.5).

#include <stddef.h>
#include <stdint.h>

namespace Presence {

// Called by MqttManager when a raw payload arrives on a presence/+ topic.
void handleIncoming(const char* group_code, const char* payload, uint16_t len);

// Publishes our own ONLINE/OFFLINE presence to one group (retained, QoS1).
void publishOnline(const char* group_code);
void publishOffline(const char* group_code);

// Addendum section 6.4 API, exposed now for Phase 4.
bool isContactOnline(const char* group_code, const char* device_id);
bool isContactRadioAvailable(const char* group_code, const char* device_id);
void setOwnRadioAvailable(bool available);
void republishOwnPresenceAllGroups();

struct OnlineContact {
  char device_id[13];
  char display_name[17];
  bool radio_available;
};
// Returns up to `capacity` currently-online contacts for group_code,
// sorted by display name then device_id (Addendum section 5).
uint8_t getOnlineContacts(const char* group_code, OnlineContact* outArr, uint8_t capacity);

struct RecentContact {
  char device_id[13];
  char last_known_name[17];
  uint32_t last_seen_timestamp;
};
// Returns up to `capacity` Recent Contacts for group_code, most-recently-seen first.
uint8_t getRecentContacts(const char* group_code, RecentContact* outArr, uint8_t capacity);

}  // namespace Presence
