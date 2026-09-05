#pragma once
// Notification framework (Addendum section 16, Phase 2 section 14).
// Generic across message types: Phase 2 uses it for TEXT; Phase 3
// (Enigma, Game challenge) and Phase 4 (Race invitation) reuse the same
// API without this file changing — the caller decides which Main Menu
// badge category a given arrival lights up.

#include <stdint.h>

#include "core/storage_messages.h"

namespace Notifications {

enum BadgeCategory : uint8_t {
  BADGE_TEXT = 1u << 0,
  BADGE_ENIGMA = 1u << 1,
  BADGE_TRAINING_GAME = 1u << 2,
};

// Call right after appendStoredMessage() for an incoming record. If the
// exact conversation is currently open, this saves it as read (no
// tone/badge); otherwise it marks UNREAD, updates the per-conversation
// summary, sounds the notification tone, and resets the Sleep timer.
void onMessageArrived(const char* group_code, const char* contact_key, const MessageRef& ref,
                      uint8_t badgeMask, bool conversationCurrentlyOpen);

// Call when the cursor lands on an unread history line.
void clearUnread(const char* group_code, const char* contact_key, const MessageRef& ref);

uint16_t getUnreadCount(const char* group_code, const char* contact_key);

bool hasAnyUnreadText();
bool hasAnyUnreadEnigma();
bool hasAnyUnreadTrainingGame();

// Phase 4: a Race invitation is a bare PK_RACE_INVITE packet, not a
// MessageStore record, so it cannot light BADGE_TRAINING_GAME through
// onMessageArrived(). Menu::registerMainMenuBadge holds only one callback
// per Main Menu item, and that slot is already this module's own
// hasAnyUnreadTrainingGame, so Race Mode ORs into the same Main Menu badge
// through this flag instead of registering a second, overwriting callback.
void setRaceInvitePending(bool pending);

}  // namespace Notifications
