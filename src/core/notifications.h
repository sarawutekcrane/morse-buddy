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
// tone/badge); otherwise it marks UNREAD, counts it in the per-conversation
// summary under its stored message type, sounds the notification tone, and
// resets the Sleep timer. The badges follow the stored type (TEXT -> Text,
// ENIGMA -> Enigma, GAME -> both); badgeMask is used only if the record
// cannot be read back.
void onMessageArrived(const char* group_code, const char* contact_key, const MessageRef& ref,
                      uint8_t badgeMask, bool conversationCurrentlyOpen);

// Call on a received message's explicit DOT action: reads that one message.
// No-op if it is already read.
void clearUnread(const char* group_code, const char* contact_key, const MessageRef& ref);

// Call when a history item becomes the selected one, with the open chat's
// conversation index (MessageStore::getIndexEntry) still loaded: if it is a
// received message, it and every older received message of the same type in
// this conversation become read -- older meaning at or above it in the
// history and stored no later than it. Messages below it, or stored after it,
// stay unread; other types are untouched. Bounded work here (a few record
// reads, no rewrite and no NVS write): the flags on flash are cleared in the
// background, and the request is saved so a reboot doesn't lose it.
void readUpTo(const char* group_code, const char* contact_key, uint16_t selectedIndex, uint16_t indexTotal);

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
