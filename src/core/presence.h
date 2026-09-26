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
  // Feature Fix #4.8: the peer's own chosen personal color, as observed in
  // its most recent ONLINE presence payload. IdentityColor::kInvalidColor
  // for a legacy peer (no trailing color field) or one never observed to
  // have chosen a color yet.
  uint8_t color_index;
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

// Resolves a device_id to a human-friendly display name for UI use only
// (Hardware Fix #4 issue 10, e.g. Race participant rows): self resolves to
// Settings::getMyName(), otherwise the current online display name, then
// the cached Recent Contacts name, then the raw device_id itself as a
// last-resort fallback. Read-only -- never creates or mutates presence or
// Recent Contacts state, and does not alter any network/state semantics.
void resolveDisplayName(const char* group_code, const char* device_id, char* out, size_t outSize);

// Feature Fix #4.8: read-only personal-color resolver, mirroring
// resolveDisplayName()'s exact same three-tier fallback shape (self ->
// current online -> persisted recent/offline), but for the sender's
// chosen IdentityColor palette index instead of their name. Never mutates
// any presence/Recent Contacts state. Returns IdentityColor::kInvalidColor
// when nothing is known (legacy/never-observed peer) -- callers render
// that as the existing CYAN sender-name fallback so old peers stay
// readable.
uint8_t resolveColorIndex(const char* group_code, const char* device_id);

// Feature Fix #4.8 section 2G/2R: centralized identity conflict checks
// against every KNOWN family member (current online contacts + persisted
// Recent Contacts) across every configured Family Group, ignoring this
// device's own identity. Read-only. `outSize`-bounded conflictingGroupCode
// is written (if non-null) with the code of the first conflicting group
// found; safe to pass nullptr if the caller doesn't need it.
//
// Name: any exact match by a known OTHER member anywhere is a conflict --
// name uniqueness is mandatory among all known family members.
bool isNameInUseByKnownMember(const char* candidateName, char* conflictingGroupCode, size_t outSize);

// Color: a conflict is reported for a group only while that group still
// has at least one unused palette color (IdentityColor::kColorCount) left
// -- once a family's known membership already occupies all 12 colors,
// color reuse is allowed rather than blocking additional members.
bool isColorInUseByKnownMember(uint8_t candidateColor, char* conflictingGroupCode, size_t outSize);

}  // namespace Presence
