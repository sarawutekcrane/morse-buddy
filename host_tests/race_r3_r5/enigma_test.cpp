// HOST TEST (not production). Enigma received-message display (owner
// request after hardware: "D EE" sent with no rotors and no plugboard showed
// "DXQEE" on the receiver before decoding). Compiles the ACTUAL
// src/core/enigma.cpp into this translation unit (its renderer, icon, arrival
// handler and key-attempt evaluation) and links the actual enigma_crypto.cpp,
// enigma_keys.cpp, menu.cpp and text_message.cpp against the shared stubs'
// message store. Every ciphertext here is produced by the ACTUAL
// EnigmaCrypto::normalizeAndEscape() + run() with a real key, and every
// decode is the actual attemptReceiveKey() round trip.
#include "stubs.h"
#include "core/enigma.cpp"
#include "core/sound_facade.h"
#include <Preferences.h>
#include <cstdio>
#include <string>
#include <vector>

namespace Sleep { void notifyActivity() {} }
void playTone(uint16_t, uint16_t, SoundClass) {}
static Preferences g_notifyPrefs;
namespace Storage { Preferences& notify() { return g_notifyPrefs; } }
namespace MessageStore {
bool updateLocalFlags(const MessageRef&, uint16_t, uint16_t) { return true; }
void setOnEvictedCallback(EvictedCallback) {}
}

static int g_fail = 0, g_pass = 0;
static std::string g_test;
#define CHECK(c, ...)                                                        \
  do {                                                                       \
    if (c) { g_pass++; } else {                                              \
      g_fail++; printf("FAIL [%s] line %d: ", g_test.c_str(), __LINE__);     \
      printf(__VA_ARGS__); printf("\n");                                     \
    }                                                                        \
  } while (0)

static const char* kG = "GROUP1";
static const char* kBob = "B0B000000001";
static unsigned g_msgNo = 0;

using EnigmaCrypto::EnigmaKeyConfig;

static EnigmaKeyConfig zeroKey() { EnigmaKeyConfig k{}; return k; }
static EnigmaKeyConfig rotorKey(uint8_t a, uint8_t b, uint8_t c) {
  EnigmaKeyConfig k{};
  k.rotor_count = 3;
  k.rotor_type[0] = 0; k.rotor_type[1] = 1; k.rotor_type[2] = 2;
  k.rotor_position[0] = a; k.rotor_position[1] = b; k.rotor_position[2] = c;
  return k;
}
static EnigmaKeyConfig plugKey(const char* pairs) {  // "AXBQ" -> A-X, B-Q; no rotors
  EnigmaKeyConfig k{};
  size_t n = strlen(pairs) / 2;
  k.plugboard_pair_count = static_cast<uint8_t>(n);
  for (size_t i = 0; i < n; i++) { k.plugboard_pairs[i][0] = pairs[2 * i]; k.plugboard_pairs[i][1] = pairs[2 * i + 1]; }
  return k;
}

static std::string escape(const char* plain) {
  char out[EnigmaCrypto::kMaxEscapedLen + 1];
  if (!EnigmaCrypto::normalizeAndEscape(plain, out, sizeof(out))) return "";
  return out;
}
static std::string encrypt(const std::string& escaped, const EnigmaKeyConfig& key) {
  char out[EnigmaCrypto::kMaxEscapedLen + 1];
  EnigmaCrypto::run(escaped.c_str(), out, sizeof(out), key);
  return out;
}
static std::string unescape(const std::string& s) {
  char out[EnigmaCrypto::kMaxEscapedLen + 1];
  EnigmaCrypto::reverseEscape(s.c_str(), out, sizeof(out));
  return out;
}

// A received Enigma message through the ACTUAL arrival handler (stored
// locked, LOCK_BLACK), carrying `cipher` and the sender's key embedded.
static MessageRef arriveEnigma(const std::string& cipher, const EnigmaKeyConfig& senderKey) {
  PacketCodec::MessageEnvelope env{};
  snprintf(env.message_id, sizeof(env.message_id), "MSG%020u", ++g_msgNo);
  env.schema_version = 1;
  env.message_type = PacketCodec::MSG_TYPE_ENIGMA;
  strncpy(env.sender_device_id, kBob, sizeof(env.sender_device_id) - 1);
  strncpy(env.sender_name_cache, "Bob", sizeof(env.sender_name_cache) - 1);
  strncpy(env.group_code, kG, sizeof(env.group_code) - 1);
  env.timestamp = 1700000100u + g_msgNo;
  uint8_t payload[340];
  size_t n = EnigmaKeys::encodeEnigmaPayload(cipher.c_str(), EnigmaCrypto::computeFingerprint(senderKey), 1, senderKey,
                                             payload, sizeof(payload));
  handleEnigmaArrival(kG, kBob, env, env.timestamp, payload, static_cast<uint16_t>(n));
  return T::store.back().ref;
}
static T::Rec* recOf(const MessageRef& ref) {
  for (auto& r : T::store)
    if (r.ref.sequence == ref.sequence) return &r;
  return nullptr;
}
static std::string shown(const MessageRef& ref) {  // what the chat history draws for this record
  StoredMessageView v;
  if (!MessageStore::loadMessage(ref, &v)) return "(not loaded)";
  char out[300];
  renderEnigmaMessage(v, out, sizeof(out));
  return out;
}
static MessageIconKind icon(const MessageRef& ref) {
  StoredMessageView v;
  MessageStore::loadMessage(ref, &v);
  return enigmaMessageIcon(v);
}
static uint8_t lockOf(const MessageRef& ref) {
  StoredMessageView v;
  MessageStore::loadMessage(ref, &v);
  uint8_t lock = 0xFF;
  char text[251];
  decodeLocalPayload(v.localPayload, v.localPayloadLen, &lock, text, sizeof(text));
  return lock;
}
static std::string cachedOf(const MessageRef& ref) {
  StoredMessageView v;
  MessageStore::loadMessage(ref, &v);
  uint8_t lock = 0xFF;
  char text[251];
  decodeLocalPayload(v.localPayload, v.localPayloadLen, &lock, text, sizeof(text));
  return text;
}
static void attempt(const MessageRef& ref, const EnigmaKeyConfig& key) {
  strcpy(g_selectedGroupCode, kG);
  strcpy(g_selectedContactKey, kBob);
  attemptReceiveKey(ref, key);
}
static void fresh() { T::reset(); }

// Deterministic search: a plaintext whose ciphertext under `key` contains
// `target`, among plaintexts that do NOT contain it in escaped form, so the
// sequence only exists in the ciphertext.
static bool findCollision(const EnigmaKeyConfig& key, const char* target, std::string* outPlain) {
  uint32_t s = 12345;
  for (int attemptNo = 0; attemptNo < 200000; attemptNo++) {
    char plain[13];
    for (int i = 0; i < 12; i++) {
      s = s * 1103515245u + 12345u;
      uint32_t r = (s >> 16) % 27;
      plain[i] = r == 26 ? ' ' : static_cast<char>('A' + r);
    }
    plain[12] = '\0';
    std::string esc = escape(plain);
    if (esc.find(target) != std::string::npos) continue;
    if (encrypt(esc, key).find(target) != std::string::npos) {
      *outPlain = plain;
      return true;
    }
  }
  return false;
}

int main() {
  T::reset();

  // ---- Case A: no rotors, no plugboard ----
  const char* plains[] = {"D EE", "HELLO WORLD", "X", "XX", "XQ", "XXQ", "XXXX", "A   B", "  LEAD", "TRAIL  ", "  BOTH  ", "x q"};
  for (const char* p : plains) {
    g_test = std::string("EA1 zero key, '") + p + "': shown readable (spaces restored, X kept), still locked (red icon), stored bytes untouched";
    fresh();
    std::string esc = escape(p);
    std::string cipher = encrypt(esc, zeroKey());
    MessageRef ref = arriveEnigma(cipher, zeroKey());
    std::vector<uint8_t> wireBefore = recOf(ref)->wire, localBefore = recOf(ref)->local;
    std::string expected = unescape(esc);  // the sender's own history text (enigma.cpp send path)
    std::string got = shown(ref);
    CHECK(cipher == esc && got == expected && icon(ref) == MessageIconKind::LOCK_CLOSED_RED && lockOf(ref) == LOCK_BLACK &&
              recOf(ref)->wire == wireBefore && recOf(ref)->local == localBefore,
          "escaped '%s' cipher '%s' shown '%s' expected '%s' icon %d lock %u", esc.c_str(), cipher.c_str(), got.c_str(),
          expected.c_str(), static_cast<int>(icon(ref)), lockOf(ref));
  }
  g_test = "EA2 the hardware example: 'D EE' arrives as DXQEE and is shown as 'D EE'";
  fresh();
  MessageRef dee = arriveEnigma("DXQEE", zeroKey());
  CHECK(shown(dee) == "D EE", "shown '%s'", shown(dee).c_str());
  g_test = "EA3 zero key, hold preview unchanged: a locked (BLACK) message still holds to [locked]; the correct key unlocks it (green) with the same text";
  onEnigmaMessageEvent(dee, EVT_DOT_HOLD_START);
  std::string held = shown(dee);
  onEnigmaMessageEvent(dee, EVT_DOT_HOLD_END);
  attempt(dee, zeroKey());
  CHECK(held == "[locked]" && lockOf(dee) == LOCK_UNLOCKED && icon(dee) == MessageIconKind::LOCK_OPEN_GREEN &&
            shown(dee) == "D EE" && cachedOf(dee) == "D EE",
        "held '%s' lock %u shown '%s' cached '%s'", held.c_str(), lockOf(dee), shown(dee).c_str(), cachedOf(dee).c_str());
  g_test = "EA4 zero key, a wrong key attempt: WHITE (yellow), the default view stays readable; not marked unlocked";
  fresh();
  MessageRef z = arriveEnigma("DXQEE", zeroKey());
  attempt(z, rotorKey(1, 2, 3));
  CHECK(lockOf(z) == LOCK_WHITE && icon(z) == MessageIconKind::LOCK_CLOSED_YELLOW && shown(z) == "D EE",
        "lock %u icon %d shown '%s'", lockOf(z), static_cast<int>(icon(z)), shown(z).c_str());

  // ---- Case B: rotors ----
  for (const char* p : {"D EE", "HELLO WORLD", "X", "XX", "XQ", "XXQ", "XXXX", "A   B", "  BOTH  "}) {
    g_test = std::string("EB1 rotor key, '") + p + "': the ciphertext is shown exactly until decoded; the correct key decodes it once, in order";
    fresh();
    EnigmaKeyConfig key = rotorKey(4, 11, 19);
    std::string esc = escape(p);
    std::string cipher = encrypt(esc, key);
    MessageRef ref = arriveEnigma(cipher, key);
    std::string before = shown(ref);
    attempt(ref, key);
    CHECK(before == cipher && lockOf(ref) == LOCK_UNLOCKED && shown(ref) == unescape(esc) && encrypt(cipher, key) == esc,
          "before '%s' (cipher '%s'), after '%s' (want '%s'), round trip %d", before.c_str(), cipher.c_str(), shown(ref).c_str(),
          unescape(esc).c_str(), encrypt(cipher, key) == esc);
  }
  const char* targets[] = {"XQ", "XX", "XXQ", "XXXX"};
  for (const char* target : targets) {
    g_test = std::string("EB2 rotor ciphertext that happens to contain '") + target + "': shown verbatim (not read as an escape); the right key gives the original text back; a wrong key does not unlock";
    fresh();
    EnigmaKeyConfig key = rotorKey(7, 3, 22);
    std::string plain;
    bool found = findCollision(key, target, &plain);
    std::string esc = escape(plain.c_str());
    std::string cipher = encrypt(esc, key);
    MessageRef ref = arriveEnigma(cipher, key);
    std::string before = shown(ref);
    MessageRef ref2 = arriveEnigma(cipher, key);  // a second copy for the wrong key
    attempt(ref2, rotorKey(7, 3, 21));
    bool wrongStays = lockOf(ref2) == LOCK_WHITE && shown(ref2) == cipher;
    fresh();
    ref = arriveEnigma(cipher, key);
    attempt(ref, key);
    CHECK(found && before == cipher && before.find(target) != std::string::npos && wrongStays && lockOf(ref) == LOCK_UNLOCKED &&
              shown(ref) == unescape(esc) && unescape(encrypt(cipher, key)) == unescape(esc),
          "found %d plain '%s' cipher '%s' shown before '%s' wrong-key kept %d after '%s' (want '%s')", found, plain.c_str(),
          cipher.c_str(), before.c_str(), wrongStays, shown(ref).c_str(), unescape(esc).c_str());
  }

  // ---- Case C: plugboard, no rotors ----
  g_test = "EC1 plugboard only, no rotors: 'AB' with A-X, B-Q encrypts to XQ -- shown as XQ (never as a space); the right key gives AB back";
  fresh();
  EnigmaKeyConfig pk = plugKey("AXBQ");
  std::string cipher = encrypt(escape("AB"), pk);
  MessageRef pref = arriveEnigma(cipher, pk);
  std::string before = shown(pref);
  attempt(pref, pk);
  CHECK(cipher == "XQ" && before == "XQ" && lockOf(pref) == LOCK_UNLOCKED && shown(pref) == "AB", "cipher '%s' before '%s' after '%s'",
        cipher.c_str(), before.c_str(), shown(pref).c_str());
  g_test = "EC2 plugboard only, 'D EE' with D-M: the ciphertext is shown (no automatic unescape on a zero rotor count alone)";
  fresh();
  EnigmaKeyConfig pk2 = plugKey("DM");
  cipher = encrypt(escape("D EE"), pk2);
  pref = arriveEnigma(cipher, pk2);
  CHECK(cipher == "MXQEE" && shown(pref) == "MXQEE" && icon(pref) == MessageIconKind::LOCK_CLOSED_RED, "cipher '%s' shown '%s'",
        cipher.c_str(), shown(pref).c_str());

  g_test = "ED1 previous-firmware compatibility: the payload format and stored bytes are unchanged (display only); a message decoded with the right key keeps its unlocked text";
  fresh();
  MessageRef c1 = arriveEnigma("DXQEE", zeroKey());
  std::vector<uint8_t> tp = recOf(c1)->typePayload;
  char ct[EnigmaCrypto::kMaxEscapedLen + 1];
  uint32_t fp = 0, gen = 0;
  EnigmaKeyConfig emb{};
  bool decoded = EnigmaKeys::decodeEnigmaPayload(tp.data(), tp.size(), ct, sizeof(ct), &fp, &gen, &emb);
  shown(c1);
  CHECK(decoded && std::string(ct) == "DXQEE" && emb.rotor_count == 0 && emb.plugboard_pair_count == 0 &&
            recOf(c1)->typePayload == tp,
        "decoded %d ciphertext '%s'", decoded, ct);

  printf("%d checks passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
