#include "core/enigma_keys.h"

#include <Arduino.h>
#include <string.h>

#include "core/storage_init.h"
#include "core/storage_messages.h"

namespace EnigmaKeys {

namespace {

using EnigmaCrypto::EnigmaKeyConfig;

constexpr uint8_t kMaxRecords = 30;

struct KeyRecord {
  bool active;
  char group_code[33];
  char contact_key[MessageStore::kContactKeyLen];
  EnigmaKeyConfig senderKey;
  uint32_t senderKeyGeneration;
  EnigmaKeyConfig receiveKey;
};

KeyRecord g_records[kMaxRecords];

EnigmaKeyConfig defaultKey() {
  EnigmaKeyConfig k;
  memset(&k, 0, sizeof(k));
  return k;
}

void saveAll() { Storage::enigma().putBytes("keys", g_records, sizeof(g_records)); }

void loadAll() {
  size_t got = Storage::enigma().getBytes("keys", g_records, sizeof(g_records));
  if (got != sizeof(g_records)) memset(g_records, 0, sizeof(g_records));
}

KeyRecord* find(const char* group_code, const char* contact_key) {
  for (auto& r : g_records) {
    if (r.active && strcmp(r.group_code, group_code) == 0 && strcmp(r.contact_key, contact_key) == 0) return &r;
  }
  return nullptr;
}

KeyRecord* findOrCreate(const char* group_code, const char* contact_key) {
  KeyRecord* r = find(group_code, contact_key);
  if (r != nullptr) return r;
  for (auto& c : g_records) {
    if (!c.active) {
      c.active = true;
      strncpy(c.group_code, group_code, sizeof(c.group_code) - 1);
      c.group_code[sizeof(c.group_code) - 1] = '\0';
      strncpy(c.contact_key, contact_key, sizeof(c.contact_key) - 1);
      c.contact_key[sizeof(c.contact_key) - 1] = '\0';
      c.senderKey = defaultKey();
      c.senderKeyGeneration = 0;
      c.receiveKey = defaultKey();
      return &c;
    }
  }
  return nullptr;  // pool exhausted; caller falls back to an unsaved default
}

void writeU32LE(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}
uint32_t readU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
void writeU16LE(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
uint16_t readU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

}  // namespace

void init() { loadAll(); }

EnigmaKeyConfig getSenderKey(const char* group_code, const char* contact_key) {
  KeyRecord* r = find(group_code, contact_key);
  return (r != nullptr) ? r->senderKey : defaultKey();
}

uint32_t getSenderKeyGeneration(const char* group_code, const char* contact_key) {
  KeyRecord* r = find(group_code, contact_key);
  return (r != nullptr) ? r->senderKeyGeneration : 0;
}

void setSenderKey(const char* group_code, const char* contact_key, const EnigmaKeyConfig& key) {
  KeyRecord* r = findOrCreate(group_code, contact_key);
  if (r == nullptr) return;
  r->senderKey = key;
  r->senderKeyGeneration++;
  saveAll();
}

EnigmaKeyConfig getReceiveKey(const char* group_code, const char* contact_key) {
  KeyRecord* r = find(group_code, contact_key);
  return (r != nullptr) ? r->receiveKey : defaultKey();
}

void setReceiveKey(const char* group_code, const char* contact_key, const EnigmaKeyConfig& key) {
  KeyRecord* r = findOrCreate(group_code, contact_key);
  if (r == nullptr) return;
  r->receiveKey = key;
  saveAll();
}

size_t encodeKeyConfig(const EnigmaKeyConfig& key, uint8_t* out, size_t outSize) {
  if (outSize < kKeyConfigWireSize) return 0;
  size_t pos = 0;
  out[pos++] = key.rotor_count;
  for (uint8_t i = 0; i < EnigmaCrypto::kMaxRotors; i++) out[pos++] = key.rotor_type[i];
  for (uint8_t i = 0; i < EnigmaCrypto::kMaxRotors; i++) out[pos++] = key.rotor_position[i];
  out[pos++] = key.plugboard_pair_count;
  for (uint8_t i = 0; i < EnigmaCrypto::kMaxPlugboardPairs; i++) {
    out[pos++] = static_cast<uint8_t>(key.plugboard_pairs[i][0]);
    out[pos++] = static_cast<uint8_t>(key.plugboard_pairs[i][1]);
  }
  return pos;
}

bool decodeKeyConfig(const uint8_t* data, size_t len, EnigmaKeyConfig* outKey) {
  if (len < kKeyConfigWireSize) return false;
  size_t pos = 0;
  outKey->rotor_count = data[pos++];
  for (uint8_t i = 0; i < EnigmaCrypto::kMaxRotors; i++) outKey->rotor_type[i] = data[pos++];
  for (uint8_t i = 0; i < EnigmaCrypto::kMaxRotors; i++) outKey->rotor_position[i] = data[pos++];
  outKey->plugboard_pair_count = data[pos++];
  for (uint8_t i = 0; i < EnigmaCrypto::kMaxPlugboardPairs; i++) {
    outKey->plugboard_pairs[i][0] = static_cast<char>(data[pos++]);
    outKey->plugboard_pairs[i][1] = static_cast<char>(data[pos++]);
  }
  if (outKey->rotor_count > EnigmaCrypto::kMaxRotors) return false;
  if (outKey->plugboard_pair_count > EnigmaCrypto::kMaxPlugboardPairs) return false;
  return true;
}

size_t encodeEnigmaPayload(const char* ciphertext, uint32_t fingerprint, uint32_t senderKeyGeneration,
                          const EnigmaKeyConfig& embeddedSenderKey, uint8_t* out, size_t outSize) {
  size_t textLen = strlen(ciphertext);
  if (textLen > EnigmaCrypto::kMaxEscapedLen) return 0;
  size_t total = 2 + textLen + 4 + 4 + kKeyConfigWireSize;
  if (outSize < total) return 0;

  size_t pos = 0;
  writeU16LE(&out[pos], static_cast<uint16_t>(textLen));
  pos += 2;
  memcpy(&out[pos], ciphertext, textLen);
  pos += textLen;
  writeU32LE(&out[pos], fingerprint);
  pos += 4;
  writeU32LE(&out[pos], senderKeyGeneration);
  pos += 4;
  pos += encodeKeyConfig(embeddedSenderKey, &out[pos], outSize - pos);
  return pos;
}

bool decodeEnigmaPayload(const uint8_t* data, size_t len, char* outCiphertext, size_t outCiphertextCapacity,
                        uint32_t* outFingerprint, uint32_t* outSenderKeyGeneration,
                        EnigmaKeyConfig* outEmbeddedKey) {
  if (len < 2) return false;
  size_t pos = 0;
  uint16_t textLen = readU16LE(&data[pos]);
  pos += 2;
  if (textLen > EnigmaCrypto::kMaxEscapedLen) return false;
  if (pos + textLen + 4 + 4 + kKeyConfigWireSize > len) return false;
  if (outCiphertextCapacity < static_cast<size_t>(textLen) + 1) return false;

  memcpy(outCiphertext, &data[pos], textLen);
  outCiphertext[textLen] = '\0';
  pos += textLen;

  *outFingerprint = readU32LE(&data[pos]);
  pos += 4;
  *outSenderKeyGeneration = readU32LE(&data[pos]);
  pos += 4;

  return decodeKeyConfig(&data[pos], len - pos, outEmbeddedKey);
}

}  // namespace EnigmaKeys
