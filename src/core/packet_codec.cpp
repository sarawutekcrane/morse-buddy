#include "core/packet_codec.h"

#include <string.h>

namespace {

void writeU16LE(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
uint16_t readU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
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

}  // namespace

namespace PacketCodec {

bool encodeHeader(uint8_t packetKind, uint16_t payloadLength, uint8_t* outBuf, size_t outBufSize) {
  if (outBufSize < kHeaderSize) return false;
  writeU16LE(outBuf, kMagic);
  outBuf[2] = kProtocolVersion;
  outBuf[3] = packetKind;
  writeU16LE(outBuf + 4, payloadLength);
  return true;
}

bool decodeHeader(const uint8_t* data, size_t len, Header* outHeader, const uint8_t** outPayload) {
  if (data == nullptr || outHeader == nullptr || outPayload == nullptr) return false;
  if (len < kHeaderSize) return false;

  uint16_t magic = readU16LE(data);
  uint8_t version = data[2];
  uint8_t kind = data[3];
  uint16_t payloadLen = readU16LE(data + 4);

  if (magic != kMagic) return false;
  if (version != kProtocolVersion) return false;
  if (static_cast<size_t>(payloadLen) > len - kHeaderSize) return false;

  outHeader->magic = magic;
  outHeader->protocol_version = version;
  outHeader->packet_kind = kind;
  outHeader->payload_length = payloadLen;
  *outPayload = data + kHeaderSize;
  return true;
}

size_t encodeMessagePacket(const MessageEnvelope& env, const uint8_t* typePayload, uint16_t typePayloadLen,
                           uint8_t* outBuf, size_t outBufSize) {
  size_t innerLen = kEnvelopeFixedSize + 2 + typePayloadLen;
  if (innerLen > 0xFFFF) return 0;
  size_t totalLen = kHeaderSize + innerLen;
  if (outBuf == nullptr || outBufSize < totalLen) return 0;

  encodeHeader(PK_MESSAGE, static_cast<uint16_t>(innerLen), outBuf, outBufSize);

  uint8_t* p = outBuf + kHeaderSize;
  memcpy(p, env.message_id, kMessageIdLen);
  p += kMessageIdLen;
  *p++ = env.schema_version;
  *p++ = env.message_type;
  memcpy(p, env.sender_device_id, kSenderDeviceIdLen);
  p += kSenderDeviceIdLen;
  memcpy(p, env.sender_name_cache, kSenderNameCacheLen);
  p += kSenderNameCacheLen;
  memcpy(p, env.group_code, kGroupCodeLen);
  p += kGroupCodeLen;
  writeU32LE(p, env.timestamp);
  p += 4;
  writeU16LE(p, typePayloadLen);
  p += 2;
  if (typePayloadLen > 0) {
    memcpy(p, typePayload, typePayloadLen);
    p += typePayloadLen;
  }

  return totalLen;
}

bool decodeMessageEnvelope(const uint8_t* payload, size_t payloadLen, MessageEnvelope* outEnv,
                           const uint8_t** outTypePayload, uint16_t* outTypePayloadLen) {
  if (payload == nullptr || outEnv == nullptr || outTypePayload == nullptr || outTypePayloadLen == nullptr) {
    return false;
  }
  if (payloadLen < kEnvelopeFixedSize + 2) return false;

  const uint8_t* p = payload;
  memcpy(outEnv->message_id, p, kMessageIdLen);
  p += kMessageIdLen;
  outEnv->schema_version = *p++;
  outEnv->message_type = *p++;
  memcpy(outEnv->sender_device_id, p, kSenderDeviceIdLen);
  p += kSenderDeviceIdLen;
  memcpy(outEnv->sender_name_cache, p, kSenderNameCacheLen);
  p += kSenderNameCacheLen;
  memcpy(outEnv->group_code, p, kGroupCodeLen);
  p += kGroupCodeLen;
  outEnv->timestamp = readU32LE(p);
  p += 4;
  uint16_t typeLen = readU16LE(p);
  p += 2;

  size_t consumed = static_cast<size_t>(p - payload);
  if (static_cast<size_t>(typeLen) > payloadLen - consumed) return false;

  // Defensive NUL-termination against a malformed/adversarial peer.
  outEnv->message_id[kMessageIdLen - 1] = '\0';
  outEnv->sender_device_id[kSenderDeviceIdLen - 1] = '\0';
  outEnv->sender_name_cache[kSenderNameCacheLen - 1] = '\0';
  outEnv->group_code[kGroupCodeLen - 1] = '\0';

  *outTypePayload = p;
  *outTypePayloadLen = typeLen;
  return true;
}

size_t encodeTextPayload(const char* decodedText, uint8_t* outBuf, size_t outBufSize) {
  if (decodedText == nullptr || outBuf == nullptr) return 0;
  size_t textLen = strlen(decodedText);
  if (textLen > kMaxDecodedTextLen) return 0;
  size_t total = 2 + textLen;
  if (outBufSize < total) return 0;

  writeU16LE(outBuf, static_cast<uint16_t>(textLen));
  if (textLen > 0) memcpy(outBuf + 2, decodedText, textLen);
  return total;
}

bool decodeTextPayload(const uint8_t* data, size_t len, char* outText, size_t outTextBufSize) {
  if (data == nullptr || outText == nullptr) return false;
  if (len < 2) return false;

  uint16_t textLen = readU16LE(data);
  if (static_cast<size_t>(textLen) > len - 2) return false;
  if (textLen > kMaxDecodedTextLen) return false;
  if (outTextBufSize < static_cast<size_t>(textLen) + 1) return false;

  memcpy(outText, data + 2, textLen);
  outText[textLen] = '\0';
  return true;
}

}  // namespace PacketCodec
