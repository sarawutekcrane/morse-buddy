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
// exact conversation is currently open and readIfOpen is true (the Friend
// game's arrivals), this saves it as read (no tone/badge). Otherwise it
// marks UNREAD and counts it in the per-conversation summary under its
// stored message type; and, unless the conversation is open, sounds the
// notification tone and resets the Sleep timer. The Text and Enigma chats
// pass readIfOpen false: an arrival is never read by being on screen (only
// a move onto it reads it, readMovedOnto()). The badges follow the stored
// type (TEXT -> Text, ENIGMA -> Enigma, GAME -> both); badgeMask is used only
// if the record cannot be read back.
void onMessageArrived(const char* group_code, const char* contact_key, const MessageRef& ref,
                      uint8_t badgeMask, bool conversationCurrentlyOpen, bool readIfOpen = true);

// Call on a received message's explicit DOT action: reads that one message.
// No-op if it is already read; the count follows only a successful rewrite.
void clearUnread(const char* group_code, const char* contact_key, const MessageRef& ref);

// Call when the user moves a Text or Enigma chat's cursor onto a message
// whose index entry is unread (every such move, moves held while the chat
// loaded and replayed included -- never for an initial, restored or
// followed selection). Asks to read exactly that message, identified by
// sequence AND message id: nothing older, nothing else. Never writes or
// lists storage. True: accepted (or already pending) -- the flag is cleared
// on flash by the background work, one rewrite per step, retried after a
// failure; the count follows only once that rewrite succeeded. Kept in RAM
// only: a power loss before the rewrite leaves the message unread. False:
// refused (32 reads already pending, or no memory for the list) -- nothing
// recorded; the message stays unread and the caller tells the user.
bool readMovedOnto(const char* group_code, const char* contact_key, uint32_t sequence, const char* message_id);

uint16_t getUnreadCount(const char* group_code, const char* contact_key);

// Read-only, from the in-memory summaries (no storage access): whether the
// conversation (group_code, contact_key) -- or, contact_key nullptr, any
// conversation of that group, its Everyone included -- may hold unread
// received messages of the badge category (BADGE_TEXT or BADGE_ENIGMA), by
// the same rule as the Main Menu badges: Text = TEXT or GAME records,
// Enigma = ENIGMA or GAME (a Friend game record appears in both chats). An
// UNRESOLVED conversation (counts unknown) answers by its category flags,
// i.e. conservatively "possibly unread".
bool hasUnreadIn(const char* group_code, const char* contact_key, uint8_t badgeMask);

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
