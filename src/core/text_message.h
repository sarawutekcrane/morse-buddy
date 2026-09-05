#pragma once
// Text Message mode (Addendum sections 6-16 assorted; Phase 2 sections
// 9-13). Entirely self-registering: a static registrar in text_message.cpp
// calls registerModeHandler(Modes::TEXT, ...), registerMessageType(...)
// for TEXT, and registerNetworkPacketHandler(PK_MESSAGE, ...) — no other
// file needs to change for Text to become fully functional.

#include <stddef.h>
#include <stdint.h>

#include "core/packet_codec.h"

namespace TextMessage {

// PK_MESSAGE carries TEXT/ENIGMA/GAME, discriminated by envelope.message_
// type, but the Network Packet Handler Registry (Addendum 3.11) allows
// only one handler per packet_kind — text_message.cpp already claimed
// PK_MESSAGE for its own arrival handling. This is the dispatch-by-type
// layer underneath that single claim: Phase 3's Enigma/Game register here
// instead of needing a second PK_MESSAGE registration (which the registry
// would refuse). Duplicate-detection (isDuplicateAndRecord) and the
// sender-timestamp/NTP fallback happen once before this handler runs, so
// every registered type gets them for free.
using IncomingMessageHandlerFn = void (*)(const char* group_code, const char* contact_key,
                                          const PacketCodec::MessageEnvelope& env, uint32_t effectiveTs,
                                          const uint8_t* typePayload, uint16_t typePayloadLen);

bool registerIncomingMessageHandler(uint8_t messageType, IncomingMessageHandlerFn fn);

// Shared "exact conversation currently open" tracker (Phase 3 section 15:
// "whether entered through Text, Enigma, or Game Chat"). Text's own Chat
// screen already updates this on entry/exit; Enigma's and Game's chat
// screens call the same functions so an arriving message in any of the
// three is correctly treated as already-read when its thread is open
// under any of them.
void setOpenConversation(const char* group_code, const char* contact_key);
void clearOpenConversation();
bool isConversationOpen(const char* group_code, const char* contact_key);

// Used by Number Guessing's post-challenge-send quick-switch ("Text: direct
// Chat same contact" — Phase 3 section 11). Resets the navigation stack and
// leaves Text's own Recipient Selection (for this group) underneath the
// pushed Chat screen, so Encoder long from Chat returns there per section
// 11's closing rule, not to whatever screen the quick-switch was raised from.
void navigateToChatDirect(const char* group_code, const char* contact_key);

}  // namespace TextMessage
