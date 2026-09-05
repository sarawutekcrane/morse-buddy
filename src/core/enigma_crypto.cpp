#include "core/enigma_crypto.h"

#include <string.h>

#include "core/crc32.h"

namespace EnigmaCrypto {

namespace {

// Addendum section 10.1 — exact wiring and notches. Index 0..4 = Rotor I..V.
const char* const kRotorWiring[kRotorTypeCount] = {
    "EKMFLGDQVZNTOWYHXUSPAIBRCJ",  // I
    "AJDKSIRUXBLHWTMCQGZNPYFVOE",  // II
    "BDFHJLCPRTXVZNYEIWGAKMUSQO",  // III
    "ESOVPZJAYQUIRHXLNFTGKDCMWB",  // IV
    "VZBRGITYUPSDNHLXAWMJQOFECK",  // V
};
const char kRotorNotch[kRotorTypeCount] = {'Q', 'E', 'V', 'J', 'Z'};
const char* const kReflectorB = "YRUHQSLDPXNGOKMIEBFZCWVJAT";

bool atNotch(const EnigmaKeyConfig& key, uint8_t slot) {
  char posLetter = static_cast<char>('A' + key.rotor_position[slot]);
  return posLetter == kRotorNotch[key.rotor_type[slot]];
}

// Addendum section 10.3 — generalized stepping.
void stepRotors(EnigmaKeyConfig* key) {
  uint8_t n = key->rotor_count;
  if (n == 0) return;

  bool stepFlag[kMaxRotors] = {false, false, false, false};
  stepFlag[n - 1] = true;  // rightmost always steps
  for (uint8_t i = 0; i + 1 < n; i++) {
    if (atNotch(*key, static_cast<uint8_t>(i + 1))) stepFlag[i] = true;  // rotor to the right at notch
  }
  for (uint8_t i = 1; i + 1 < n; i++) {
    if (atNotch(*key, i)) stepFlag[i] = true;  // internal rotor at its own notch (double-step)
  }

  for (uint8_t i = 0; i < n; i++) {
    if (stepFlag[i]) key->rotor_position[i] = static_cast<uint8_t>((key->rotor_position[i] + 1) % 26);
  }
}

int8_t applyPlugboard(int8_t idx, const EnigmaKeyConfig& key) {
  char c = static_cast<char>('A' + idx);
  for (uint8_t i = 0; i < key.plugboard_pair_count; i++) {
    if (key.plugboard_pairs[i][0] == c) return static_cast<int8_t>(key.plugboard_pairs[i][1] - 'A');
    if (key.plugboard_pairs[i][1] == c) return static_cast<int8_t>(key.plugboard_pairs[i][0] - 'A');
  }
  return idx;
}

int8_t rotorForward(int8_t idx, uint8_t rotorType, uint8_t position) {
  int shifted = (idx + position) % 26;
  int wired = kRotorWiring[rotorType][shifted] - 'A';
  return static_cast<int8_t>(((wired - position) % 26 + 26) % 26);
}

int8_t rotorReverse(int8_t idx, uint8_t rotorType, uint8_t position) {
  int shifted = (idx + position) % 26;
  char target = static_cast<char>('A' + shifted);
  int inv = 0;
  for (int i = 0; i < 26; i++) {
    if (kRotorWiring[rotorType][i] == target) {
      inv = i;
      break;
    }
  }
  return static_cast<int8_t>(((inv - position) % 26 + 26) % 26);
}

char encryptChar(char c, EnigmaKeyConfig* key) {
  int8_t idx = static_cast<int8_t>(c - 'A');

  if (key->rotor_count == 0) {
    // Zero-rotor special case (Addendum 10.4): no reflector, no rotor
    // path; plugboard applied once as the reciprocal substitution stage.
    idx = applyPlugboard(idx, *key);
    return static_cast<char>('A' + idx);
  }

  stepRotors(key);  // step before encrypting, as on a real keypress

  idx = applyPlugboard(idx, *key);
  for (int i = key->rotor_count - 1; i >= 0; i--) {
    idx = rotorForward(idx, key->rotor_type[i], key->rotor_position[i]);
  }
  idx = static_cast<int8_t>(kReflectorB[idx] - 'A');
  for (int i = 0; i < key->rotor_count; i++) {
    idx = rotorReverse(idx, key->rotor_type[i], key->rotor_position[i]);
  }
  idx = applyPlugboard(idx, *key);

  return static_cast<char>('A' + idx);
}

size_t canonicalize(const EnigmaKeyConfig& key, uint8_t* buf) {
  size_t pos = 0;
  buf[pos++] = key.rotor_count;
  for (uint8_t i = 0; i < key.rotor_count; i++) {
    buf[pos++] = key.rotor_type[i];
    buf[pos++] = key.rotor_position[i];
  }
  buf[pos++] = key.plugboard_pair_count;

  char sorted[kMaxPlugboardPairs][2];
  for (uint8_t i = 0; i < key.plugboard_pair_count; i++) {
    char a = key.plugboard_pairs[i][0];
    char b = key.plugboard_pairs[i][1];
    if (b < a) {
      char t = a;
      a = b;
      b = t;
    }
    sorted[i][0] = a;
    sorted[i][1] = b;
  }
  for (uint8_t i = 1; i < key.plugboard_pair_count; i++) {
    char ka = sorted[i][0];
    char kb = sorted[i][1];
    int32_t j = static_cast<int32_t>(i) - 1;
    while (j >= 0 && (sorted[j][0] > ka || (sorted[j][0] == ka && sorted[j][1] > kb))) {
      sorted[j + 1][0] = sorted[j][0];
      sorted[j + 1][1] = sorted[j][1];
      j--;
    }
    sorted[j + 1][0] = ka;
    sorted[j + 1][1] = kb;
  }
  for (uint8_t i = 0; i < key.plugboard_pair_count; i++) {
    buf[pos++] = static_cast<uint8_t>(sorted[i][0]);
    buf[pos++] = static_cast<uint8_t>(sorted[i][1]);
  }
  return pos;
}

}  // namespace

bool isValidRotorSelection(const EnigmaKeyConfig& key) {
  for (uint8_t i = 0; i < key.rotor_count; i++) {
    if (key.rotor_type[i] >= kRotorTypeCount) return false;
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < key.rotor_count; j++) {
      if (key.rotor_type[i] == key.rotor_type[j]) return false;
    }
  }
  return true;
}

bool isValidPlugboard(const EnigmaKeyConfig& key) {
  bool used[26] = {false};
  for (uint8_t i = 0; i < key.plugboard_pair_count; i++) {
    char a = key.plugboard_pairs[i][0];
    char b = key.plugboard_pairs[i][1];
    if (a == b) return false;  // A-A impossible
    if (a < 'A' || a > 'Z' || b < 'A' || b > 'Z') return false;
    if (used[a - 'A'] || used[b - 'A']) return false;
    used[a - 'A'] = true;
    used[b - 'A'] = true;
  }
  return true;
}

void run(const char* input, char* output, size_t outputCapacity, EnigmaKeyConfig key) {
  size_t len = strlen(input);
  size_t n = (len < outputCapacity - 1) ? len : outputCapacity - 1;
  for (size_t i = 0; i < n; i++) {
    output[i] = encryptChar(input[i], &key);
  }
  output[n] = '\0';
}

bool normalizeAndEscape(const char* input, char* out, size_t outCapacity) {
  size_t pos = 0;
  size_t cap = (outCapacity - 1 < kMaxEscapedLen) ? outCapacity - 1 : kMaxEscapedLen;
  for (const char* p = input; *p != '\0'; p++) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');

    if (c == ' ') {
      if (pos + 2 > cap) return false;
      out[pos++] = 'X';
      out[pos++] = 'Q';
    } else if (c == 'X') {
      if (pos + 2 > cap) return false;
      out[pos++] = 'X';
      out[pos++] = 'X';
    } else if (c >= 'A' && c <= 'Z') {
      if (pos + 1 > cap) return false;
      out[pos++] = c;
    }
    // digits/punctuation: dropped
  }
  out[pos] = '\0';
  return true;
}

void reverseEscape(const char* input, char* out, size_t outCapacity) {
  size_t pos = 0;
  size_t i = 0;
  size_t len = strlen(input);
  while (i < len && pos + 1 < outCapacity) {
    char c = input[i];
    if (c == 'X' && i + 1 < len) {
      char next = input[i + 1];
      if (next == 'X') {
        out[pos++] = 'X';
        i += 2;
        continue;
      }
      if (next == 'Q') {
        out[pos++] = ' ';
        i += 2;
        continue;
      }
    }
    out[pos++] = c;
    i++;
  }
  out[pos] = '\0';
}

uint32_t computeFingerprint(const EnigmaKeyConfig& key) {
  uint8_t buf[64];
  size_t len = canonicalize(key, buf);
  return Crc32::compute(buf, len);
}

bool keysEqual(const EnigmaKeyConfig& a, const EnigmaKeyConfig& b) {
  uint8_t bufA[64];
  uint8_t bufB[64];
  size_t lenA = canonicalize(a, bufA);
  size_t lenB = canonicalize(b, bufB);
  if (lenA != lenB) return false;
  return memcmp(bufA, bufB, lenA) == 0;
}

}  // namespace EnigmaCrypto
