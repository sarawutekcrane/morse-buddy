#pragma once
// Enigma Cipher mode (Addendum section 10; Phase 3 sections 2-7). Self-
// registering: registers Modes::ENIGMA and the MSG_TYPE_ENIGMA message
// type. Enigma gets its own Chat screen (compose semantics differ from
// Text's — encrypt-on-send, three lock states) but renders the same
// Unified Thread generically, same as Text's.

namespace Enigma {

// Used by Number Guessing's post-challenge-send quick-switch ("Enigma:
// direct Chat same contact, skip Contact Menu" — Phase 3 section 11).
void navigateToChatDirect(const char* group_code, const char* contact_key);

}  // namespace Enigma
