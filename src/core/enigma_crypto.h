#pragma once
// Exact Enigma engine (Addendum section 10). This is a simplified game
// cipher, not secure encryption — implemented exactly as specified, no
// alternative/external Enigma choices.

#include <stddef.h>
#include <stdint.h>

namespace EnigmaCrypto {

constexpr uint8_t kMaxRotors = 4;
constexpr uint8_t kMaxPlugboardPairs = 6;
constexpr uint8_t kRotorTypeCount = 5;  // Rotor I..V

// Logical key (Addendum 10.6). No Ringstellung.
struct EnigmaKeyConfig {
  uint8_t rotor_count;                          // 0-4
  uint8_t rotor_type[kMaxRotors];               // 0-4 = Rotor I..V; only [0, rotor_count) meaningful
  uint8_t rotor_position[kMaxRotors];           // 0-25 = A..Z start position; reset every message
  uint8_t plugboard_pair_count;                 // 0-6
  char plugboard_pairs[kMaxPlugboardPairs][2];  // uppercase letters; only [0, plugboard_pair_count) meaningful
};

// No rotor type repeated among the selected rotor_count slots.
bool isValidRotorSelection(const EnigmaKeyConfig& key);

// No letter appears in more than one plugboard pair; no A-A pair.
bool isValidPlugboard(const EnigmaKeyConfig& key);

// Runs the full Enigma signal path over `input` (A-Z only) into `output`.
// Reciprocal: the same call encrypts or decrypts. `key` is taken by value
// and always starts from its configured start positions — no rotor state
// carries over between calls/messages (Addendum 10.2).
void run(const char* input, char* output, size_t outputCapacity, EnigmaKeyConfig key);

// ---- Reversible plaintext normalization (Addendum 10.5) --------------------
constexpr size_t kMaxEscapedLen = 200;

// Uppercases, drops digits/punctuation, escapes literal X as XX and a word
// space as XQ. False if the escaped result would exceed kMaxEscapedLen
// (caller shows "Message too long after Enigma encoding").
bool normalizeAndEscape(const char* input, char* out, size_t outCapacity);

// Reverses the escape after a correct decrypt: XX -> X, XQ -> space. An
// unrecognized lone X sequence (only possible on a wrong-key White
// preview) is passed through as-is rather than treated as a fatal error.
void reverseEscape(const char* input, char* out, size_t outCapacity);

// ---- Fingerprint + equality (Addendum 10.7, 10.8) --------------------------
// Canonical CRC32 over: rotor_count, (type,position) per slot in slot
// order, plugboard_pair_count, each pair internally sorted (AB == BA),
// then the pair list sorted alphabetically. A compact grouping aid only —
// never the sole proof of key equality.
uint32_t computeFingerprint(const EnigmaKeyConfig& key);

// The authoritative correctness check: full canonical equality, not CRC32
// alone (two different keys could in principle collide on fingerprint).
bool keysEqual(const EnigmaKeyConfig& a, const EnigmaKeyConfig& b);

}  // namespace EnigmaCrypto
