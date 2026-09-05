#pragma once
// Enigma Sender/Receive key storage, scoped by group_code + contact_key
// (Addendum section 4/10.11; Phase 3 section 4). Also owns the wire codec
// for the Enigma type_payload (Addendum section 7.5) since it's built
// entirely out of EnigmaKeyConfig fields this module already owns.

#include <stddef.h>
#include <stdint.h>

#include "core/enigma_crypto.h"

namespace EnigmaKeys {

void init();  // loads the persisted key table from mb_enigma

// Defaults (Addendum 10.11): 0 rotors + 0 plugboard pairs, generation 0,
// for any group_code+contact_key that has never had a key saved.
EnigmaCrypto::EnigmaKeyConfig getSenderKey(const char* group_code, const char* contact_key);
uint32_t getSenderKeyGeneration(const char* group_code, const char* contact_key);

// Increments sender_key_generation by 1 every call, even if `key` equals
// the previous sender key (Addendum 10.8).
void setSenderKey(const char* group_code, const char* contact_key, const EnigmaCrypto::EnigmaKeyConfig& key);

EnigmaCrypto::EnigmaKeyConfig getReceiveKey(const char* group_code, const char* contact_key);
void setReceiveKey(const char* group_code, const char* contact_key, const EnigmaCrypto::EnigmaKeyConfig& key);

// ---- Enigma type_payload wire codec (Addendum 7.5) -------------------------
constexpr size_t kKeyConfigWireSize = 1 + EnigmaCrypto::kMaxRotors * 2 + 1 + EnigmaCrypto::kMaxPlugboardPairs * 2;

size_t encodeKeyConfig(const EnigmaCrypto::EnigmaKeyConfig& key, uint8_t* out, size_t outSize);
bool decodeKeyConfig(const uint8_t* data, size_t len, EnigmaCrypto::EnigmaKeyConfig* outKey);

// ciphertext must be NUL-terminated, <= EnigmaCrypto::kMaxEscapedLen bytes.
size_t encodeEnigmaPayload(const char* ciphertext, uint32_t fingerprint, uint32_t senderKeyGeneration,
                          const EnigmaCrypto::EnigmaKeyConfig& embeddedSenderKey, uint8_t* out, size_t outSize);

// outCiphertext must be at least EnigmaCrypto::kMaxEscapedLen+1 bytes.
bool decodeEnigmaPayload(const uint8_t* data, size_t len, char* outCiphertext, size_t outCiphertextCapacity,
                        uint32_t* outFingerprint, uint32_t* outSenderKeyGeneration,
                        EnigmaCrypto::EnigmaKeyConfig* outEmbeddedKey);

}  // namespace EnigmaKeys
