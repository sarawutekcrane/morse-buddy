#pragma once
// Explicit binary wire codec (Addendum section 1.5 and 7). No JSON, no raw
// struct wire copies — every field is serialized/deserialized explicitly,
// little-endian, with a bounds check on every decode.

#include <stddef.h>
#include <stdint.h>

namespace PacketCodec {

// ---- Common packet header (Addendum section 7.1) ---------------------------
constexpr uint16_t kMagic = 0x4D42;  // "MB"
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 6;  // magic(2) + version(1) + kind(1) + payload_length(2)

// Race/Radio kinds are reserved here now (Phase 4 sends them only on their
// matching topics) so no later phase needs to reopen this header.
enum PacketKind : uint8_t {
  PK_MESSAGE = 1,
  PK_GAME_RESULT_CHUNK = 2,
  PK_RACE_INVITE = 3,
  PK_RACE_JOIN = 4,
  PK_RACE_ROUND = 5,
  PK_RACE_SOLVED = 6,
  PK_RACE_RESET = 7,
  PK_RADIO_BUSY = 8,
  PK_RADIO_BUSY_REPLY = 9,
  PK_RADIO_SESSION = 10,
  PK_RADIO_AUDIO = 11,
};

struct Header {
  uint16_t magic;
  uint8_t protocol_version;
  uint8_t packet_kind;
  uint16_t payload_length;
};

// Writes the 6-byte header at outBuf[0..5]. False if outBufSize < kHeaderSize.
bool encodeHeader(uint8_t packetKind, uint16_t payloadLength, uint8_t* outBuf, size_t outBufSize);

// Validates magic/version and that payload_length fits within `len` before
// trusting anything else in the packet. On success, *outPayload points at
// data + kHeaderSize (no copy). Malformed/wrong-version packets return false
// and must be logged and ignored, never parsed further.
bool decodeHeader(const uint8_t* data, size_t len, Header* outHeader, const uint8_t** outPayload);

// ---- PK_MESSAGE envelope (Addendum section 7.2) ----------------------------
constexpr size_t kMessageIdLen = 24;
constexpr size_t kSenderDeviceIdLen = 16;
constexpr size_t kSenderNameCacheLen = 17;
constexpr size_t kGroupCodeLen = 33;
constexpr size_t kEnvelopeFixedSize =
    kMessageIdLen + 1 + 1 + kSenderDeviceIdLen + kSenderNameCacheLen + kGroupCodeLen + 4;  // = 96

enum MessageType : uint8_t { MSG_TYPE_TEXT = 1, MSG_TYPE_ENIGMA = 2, MSG_TYPE_GAME = 3 };

struct MessageEnvelope {
  char message_id[kMessageIdLen];
  uint8_t schema_version;
  uint8_t message_type;
  char sender_device_id[kSenderDeviceIdLen];
  char sender_name_cache[kSenderNameCacheLen];
  char group_code[kGroupCodeLen];
  uint32_t timestamp;
};

// Builds a complete PK_MESSAGE wire packet: header + envelope +
// type_payload_length + type_payload. Returns total bytes written, or 0 if
// outBuf is too small or the payload would overflow the 16-bit length field.
size_t encodeMessagePacket(const MessageEnvelope& env, const uint8_t* typePayload, uint16_t typePayloadLen,
                           uint8_t* outBuf, size_t outBufSize);

// Decodes envelope fields from a PK_MESSAGE payload (i.e. the bytes after
// the common header) and bounds-checks the embedded type_payload before
// handing back a pointer/length into `payload` (no copy).
bool decodeMessageEnvelope(const uint8_t* payload, size_t payloadLen, MessageEnvelope* outEnv,
                           const uint8_t** outTypePayload, uint16_t* outTypePayloadLen);

// ---- TEXT type payload (Addendum section 7.4) ------------------------------
// Wire carries decoded text only, length-prefixed (never a fixed 201-byte
// block) — the receiver regenerates the raw-Morse display locally.
constexpr size_t kMaxDecodedTextLen = 200;

// decodedText must be NUL-terminated and <=200 bytes. Returns bytes
// written, or 0 on overflow/failure.
size_t encodeTextPayload(const char* decodedText, uint8_t* outBuf, size_t outBufSize);

// outText must be at least kMaxDecodedTextLen+1 bytes; NUL-terminated on
// success. False on a malformed or oversized payload.
bool decodeTextPayload(const uint8_t* data, size_t len, char* outText, size_t outTextBufSize);

}  // namespace PacketCodec
