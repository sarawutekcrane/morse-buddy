#include "core/radio_transport.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <new>
#include <string.h>

#include "core/hooks.h"
#include "core/identity.h"
#include "core/packet_codec.h"
#include "core/radio_audio.h"
#include "core/settings.h"
#include "core/sleep.h"
#include "core/sound_facade.h"
#include "core/mqtt_manager.h"

namespace RadioTransport {

namespace {

// ---- little-endian helpers (this module's own wire format) ----------------
void writeU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}
uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// ---- big-endian helpers (STUN, RFC 5389, network byte order) --------------
uint16_t readU16BE(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
void writeU16BE(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[1] = static_cast<uint8_t>(v & 0xFF);
}
void writeU32BE(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[3] = static_cast<uint8_t>(v & 0xFF);
}

constexpr size_t kDeviceIdLen = PacketCodec::kSenderDeviceIdLen;  // 16
constexpr size_t kIdLen = PacketCodec::kMessageIdLen;             // 24

// =============================================================================
// AudioScope dispatch (PK_RADIO_AUDIO has one registerNetworkPacketHandler
// slot, same constraint as PK_MESSAGE in Phase 2/3; this module owns that
// slot and fans out by scope so race.cpp's Room voice doesn't need a second,
// refused registration).
// =============================================================================
struct AudioScopeEntry {
  bool used;
  AudioScope scope;
  AudioScopeHandlerFn fn;
};
constexpr uint8_t kMaxAudioScopes = 4;
AudioScopeEntry g_audioScopeHandlers[kMaxAudioScopes];

AudioScopeHandlerFn findAudioScopeHandler(AudioScope scope) {
  for (auto& e : g_audioScopeHandlers) {
    if (e.used && e.scope == scope) return e.fn;
  }
  return nullptr;
}

// =============================================================================
// Wire formats
// =============================================================================
constexpr uint8_t BUSY_CLAIM = 0;
constexpr uint8_t BUSY_RELEASE = 1;
constexpr uint8_t REPLY_GRANT = 0;
constexpr uint8_t REPLY_DENY = 1;
constexpr uint8_t SESSION_START = 0;
constexpr uint8_t SESSION_ACK = 1;
constexpr uint8_t UDP_PUNCH = 0;
constexpr uint8_t UDP_VOICE = 1;

constexpr uint16_t kUdpMagic = 0x4D42;  // "MB" — top two bits '01', never matches a STUN header ('00')
constexpr uint16_t kUdpPort = 5005;
constexpr const char* kStunHost = "stun.l.google.com";
constexpr uint16_t kStunPort = 19302;

constexpr uint32_t kClaimRefreshMs = 2000;
constexpr uint32_t kClaimReplyTimeoutMs = 1500;
constexpr uint32_t kBusyTtlMs = 5000;
constexpr uint32_t kNegotiateTimeoutMs = 2000;
constexpr uint32_t kJitterFrameMs = 20;
constexpr uint32_t kBroadcastSpeakerStaleMs = 500;

size_t buildBusyPayload(uint8_t type, const char* claimerDeviceId, uint8_t* out, size_t cap) {
  size_t need = 1 + kDeviceIdLen;
  if (cap < need) return 0;
  out[0] = type;
  memset(out + 1, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + 1), claimerDeviceId, kDeviceIdLen - 1);
  return need;
}
bool parseBusyPayload(const uint8_t* data, uint16_t len, uint8_t* outType, char* outDeviceId) {
  if (data == nullptr || len < 1 + kDeviceIdLen) return false;
  *outType = data[0];
  memcpy(outDeviceId, data + 1, kDeviceIdLen);
  outDeviceId[kDeviceIdLen - 1] = '\0';
  return true;
}

size_t buildReplyPayload(uint8_t type, const char* responderDeviceId, uint8_t* out, size_t cap) {
  size_t need = 1 + kDeviceIdLen;
  if (cap < need) return 0;
  out[0] = type;
  memset(out + 1, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + 1), responderDeviceId, kDeviceIdLen - 1);
  return need;
}
bool parseReplyPayload(const uint8_t* data, uint16_t len, uint8_t* outType, char* outDeviceId) {
  if (data == nullptr || len < 1 + kDeviceIdLen) return false;
  *outType = data[0];
  memcpy(outDeviceId, data + 1, kDeviceIdLen);
  outDeviceId[kDeviceIdLen - 1] = '\0';
  return true;
}

size_t buildSessionPayload(uint8_t type, const char* sessionId, const char* fromId, const char* toId, IPAddress ip,
                           uint16_t port, uint8_t* out, size_t cap) {
  size_t need = 1 + kIdLen + kDeviceIdLen + kDeviceIdLen + 4 + 2;
  if (cap < need) return 0;
  size_t pos = 0;
  out[pos++] = type;
  memset(out + pos, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), sessionId, kIdLen - 1);
  pos += kIdLen;
  memset(out + pos, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), fromId, kDeviceIdLen - 1);
  pos += kDeviceIdLen;
  memset(out + pos, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), toId, kDeviceIdLen - 1);
  pos += kDeviceIdLen;
  out[pos++] = ip[0];
  out[pos++] = ip[1];
  out[pos++] = ip[2];
  out[pos++] = ip[3];
  writeU16(out + pos, port);
  pos += 2;
  return pos;
}
bool parseSessionPayload(const uint8_t* data, uint16_t len, uint8_t* outType, char* outSessionId, char* outFromId,
                         char* outToId, IPAddress* outIp, uint16_t* outPort) {
  size_t need = 1 + kIdLen + kDeviceIdLen + kDeviceIdLen + 4 + 2;
  if (data == nullptr || len < need) return false;
  size_t pos = 0;
  *outType = data[pos++];
  memcpy(outSessionId, data + pos, kIdLen);
  outSessionId[kIdLen - 1] = '\0';
  pos += kIdLen;
  memcpy(outFromId, data + pos, kDeviceIdLen);
  outFromId[kDeviceIdLen - 1] = '\0';
  pos += kDeviceIdLen;
  memcpy(outToId, data + pos, kDeviceIdLen);
  outToId[kDeviceIdLen - 1] = '\0';
  pos += kDeviceIdLen;
  *outIp = IPAddress(data[pos], data[pos + 1], data[pos + 2], data[pos + 3]);
  pos += 4;
  *outPort = readU16(data + pos);
  return true;
}

// MQTT-fallback / broadcast / room audio envelope (Phase 4 sections 7-8, 17).
size_t buildAudioPayload(AudioScope scope, const char* senderDeviceId, const char* roomKey, uint16_t sequence,
                         const int16_t* samples, size_t sampleCount, uint8_t* out, size_t cap) {
  size_t pcmBytes = sampleCount * 2;
  size_t need = 1 + kDeviceIdLen + kIdLen + 2 + 2 + pcmBytes;
  if (cap < need) return 0;
  size_t pos = 0;
  out[pos++] = static_cast<uint8_t>(scope);
  memset(out + pos, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), senderDeviceId, kDeviceIdLen - 1);
  pos += kDeviceIdLen;
  memset(out + pos, 0, kIdLen);
  if (roomKey != nullptr) strncpy(reinterpret_cast<char*>(out + pos), roomKey, kIdLen - 1);
  pos += kIdLen;
  writeU16(out + pos, sequence);
  pos += 2;
  writeU16(out + pos, static_cast<uint16_t>(pcmBytes));
  pos += 2;
  for (size_t i = 0; i < sampleCount; i++) {
    writeU16(out + pos, static_cast<uint16_t>(samples[i]));
    pos += 2;
  }
  return pos;
}

}  // namespace

bool registerAudioScopeHandler(AudioScope scope, AudioScopeHandlerFn fn) {
  for (auto& e : g_audioScopeHandlers) {
    if (e.used && e.scope == scope) return false;
  }
  for (auto& e : g_audioScopeHandlers) {
    if (!e.used) {
      e = {true, scope, fn};
      return true;
    }
  }
  return false;
}

bool publishAudioPacket(const char* group_code, const char* topic_suffix, AudioScope scope, const char* roomKey,
                        uint16_t sequence, const int16_t* samples, size_t sampleCount) {
  uint8_t payload[1 + kDeviceIdLen + kIdLen + 2 + 2 + RadioAudio::kFrameBytes];
  size_t payloadLen = buildAudioPayload(scope, Identity::deviceId(), roomKey, sequence, samples, sampleCount, payload,
                                       sizeof(payload));
  if (payloadLen == 0) return false;

  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_AUDIO, static_cast<uint16_t>(payloadLen), wireBuf,
                                 sizeof(wireBuf))) {
    return false;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, payloadLen);
  return MqttManager::publishBinary(group_code, topic_suffix, wireBuf,
                                    static_cast<uint16_t>(PacketCodec::kHeaderSize + payloadLen), false, 0);
}

namespace {

void handleRadioAudioPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)topic;
  if (payload == nullptr || payloadLen < 1 + kDeviceIdLen + kIdLen + 2 + 2) return;
  size_t pos = 0;
  AudioScope scope = static_cast<AudioScope>(payload[pos++]);
  char sender[kDeviceIdLen];
  memcpy(sender, payload + pos, kDeviceIdLen);
  sender[kDeviceIdLen - 1] = '\0';
  pos += kDeviceIdLen;
  char roomKey[kIdLen];
  memcpy(roomKey, payload + pos, kIdLen);
  roomKey[kIdLen - 1] = '\0';
  pos += kIdLen;
  uint16_t sequence = readU16(payload + pos);
  pos += 2;
  uint16_t pcmBytes = readU16(payload + pos);
  pos += 2;
  if (pos + pcmBytes > payloadLen) return;

  size_t sampleCount = pcmBytes / 2;
  if (sampleCount > RadioAudio::kFrameSamples) sampleCount = RadioAudio::kFrameSamples;
  static int16_t samples[RadioAudio::kFrameSamples];
  for (size_t i = 0; i < sampleCount; i++) samples[i] = static_cast<int16_t>(readU16(payload + pos + i * 2));

  AudioScopeHandlerFn fn = findAudioScopeHandler(scope);
  if (fn != nullptr) fn(group_code, sender, roomKey, sequence, samples, sampleCount);
}

// =============================================================================
// UI context (Phase 4 section 9: Mute-On background playback gate)
// =============================================================================
UiContext g_uiContext = UiContext::NONE;

bool backgroundPlaybackAllowed() {
  if (!Settings::getMuteRadioOutsideRadio()) return true;
  return g_uiContext == UiContext::RADIO_TALK || g_uiContext == UiContext::RACE_ROOM;
}

// =============================================================================
// Receiver-authoritative Channel Busy (Phase 4 section 4)
// =============================================================================
struct ReceiverBusy {
  bool busy = false;
  char claimerDeviceId[kDeviceIdLen] = {0};
  uint32_t lastRefreshMs = 0;
};
ReceiverBusy g_receiverBusy;

void resetPrivateCall();  // defined with the rest of the private-call state, below

void sendBusyReply(const char* group_code, const char* toDeviceId, uint8_t type) {
  uint8_t payload[1 + kDeviceIdLen];
  size_t len = buildReplyPayload(type, Identity::deviceId(), payload, sizeof(payload));
  if (len == 0) return;
  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_BUSY_REPLY, static_cast<uint16_t>(len), wireBuf,
                                 sizeof(wireBuf))) {
    return;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
  char topicSuffix[32];
  snprintf(topicSuffix, sizeof(topicSuffix), "radio/busy_reply/%s", toDeviceId);
  MqttManager::publishBinary(group_code, topicSuffix, wireBuf,
                             static_cast<uint16_t>(PacketCodec::kHeaderSize + len), false, 0);
}

void handleRadioBusyPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)topic;
  uint8_t type;
  char claimer[kDeviceIdLen];
  if (!parseBusyPayload(payload, static_cast<uint16_t>(payloadLen), &type, claimer)) return;

  if (type == BUSY_CLAIM) {
    bool sameClaimant = g_receiverBusy.busy && strcmp(g_receiverBusy.claimerDeviceId, claimer) == 0;
    if (!g_receiverBusy.busy || sameClaimant) {
      g_receiverBusy.busy = true;
      strncpy(g_receiverBusy.claimerDeviceId, claimer, sizeof(g_receiverBusy.claimerDeviceId) - 1);
      g_receiverBusy.lastRefreshMs = millis();
      sendBusyReply(group_code, claimer, REPLY_GRANT);
    } else {
      sendBusyReply(group_code, claimer, REPLY_DENY);
    }
  } else if (type == BUSY_RELEASE) {
    if (g_receiverBusy.busy && strcmp(g_receiverBusy.claimerDeviceId, claimer) == 0) {
      g_receiverBusy.busy = false;
    }
    // Tear down our own (callee-side) call state too — otherwise it would
    // stay stale forever and permanently refuse this peer's next call
    // (handleRadioSessionPacket rejects a new session_id while g_call is
    // still non-IDLE from the ended one).
    if (!g_call.isCaller && g_call.state != PrivateState::IDLE && strcmp(g_call.peer_device_id, claimer) == 0) {
      resetPrivateCall();
    }
  }
}

void jitterReset();  // defined with the rest of the jitter buffer, below

// =============================================================================
// Private call state (caller or callee — only one role active at a time,
// since the receiver-authoritative Busy channel already bounds this device
// to at most one concurrent private call).
// =============================================================================
struct PreBuffer {
  int16_t* data = nullptr;
  uint16_t capacityFrames = 0;
  uint16_t count = 0;
};
constexpr uint16_t kPreBufferMaxFrames = 100;  // ~2s @ 20ms/frame (Phase 4 section 5)
PreBuffer g_preBuffer;

void allocatePreBuffer() {
  if (g_preBuffer.data != nullptr) return;
  g_preBuffer.data = new (std::nothrow) int16_t[static_cast<size_t>(kPreBufferMaxFrames) * RadioAudio::kFrameSamples];
  g_preBuffer.capacityFrames = (g_preBuffer.data != nullptr) ? kPreBufferMaxFrames : 0;
  g_preBuffer.count = 0;
}
void freePreBuffer() {
  delete[] g_preBuffer.data;
  g_preBuffer.data = nullptr;
  g_preBuffer.capacityFrames = 0;
  g_preBuffer.count = 0;
}
void bufferPreFrame(const int16_t* samples, size_t count) {
  if (g_preBuffer.data == nullptr || g_preBuffer.count >= g_preBuffer.capacityFrames) return;  // full: drop
  memcpy(g_preBuffer.data + static_cast<size_t>(g_preBuffer.count) * RadioAudio::kFrameSamples, samples,
        count * sizeof(int16_t));
  g_preBuffer.count++;
}

struct PrivateCall {
  PrivateState state = PrivateState::IDLE;
  bool isCaller = false;
  char group_code[33] = {0};
  char peer_device_id[kDeviceIdLen] = {0};
  char session_id[kIdLen] = {0};
  uint32_t claimSentMs = 0;
  uint32_t negotiateStartMs = 0;
  bool myEndpointKnown = false;
  IPAddress myIp;
  uint16_t myPort = 0;
  bool peerEndpointKnown = false;
  IPAddress peerIp;
  uint16_t peerPort = 0;
  uint16_t txSequence = 0;
  bool denied = false;
};
PrivateCall g_call;

WiFiUDP g_udp;
bool g_udpBegun = false;
uint8_t g_stunTransactionId[12];
bool g_stunPending = false;

void ensureUdpBegun() {
  if (g_udpBegun) return;
  g_udpBegun = g_udp.begin(kUdpPort);
}

void sendStunRequest() {
  ensureUdpBegun();
  uint8_t req[20];
  writeU16BE(req, 0x0001);
  writeU16BE(req + 2, 0x0000);
  writeU32BE(req + 4, 0x2112A442);
  for (int i = 0; i < 12; i++) {
    g_stunTransactionId[i] = static_cast<uint8_t>(esp_random() & 0xFF);
    req[8 + i] = g_stunTransactionId[i];
  }
  if (g_udp.beginPacket(kStunHost, kStunPort)) {
    g_udp.write(req, sizeof(req));
    g_udp.endPacket();
    g_stunPending = true;
  }
}

bool parseStunResponse(const uint8_t* buf, size_t len, IPAddress* outIp, uint16_t* outPort) {
  if (len < 20) return false;
  uint16_t msgType = readU16BE(buf);
  uint16_t msgLen = readU16BE(buf + 2);
  uint32_t cookie = readU32BE(buf + 4);
  if (cookie != 0x2112A442 || msgType != 0x0101) return false;
  if (memcmp(buf + 8, g_stunTransactionId, 12) != 0) return false;

  size_t pos = 20;
  size_t end = (20 + msgLen <= len) ? (20 + msgLen) : len;
  while (pos + 4 <= end) {
    uint16_t attrType = readU16BE(buf + pos);
    uint16_t attrLen = readU16BE(buf + pos + 2);
    size_t valuePos = pos + 4;
    if (valuePos + attrLen > end) break;
    if (attrType == 0x0020 && attrLen >= 8 && buf[valuePos + 1] == 0x01) {  // XOR-MAPPED-ADDRESS, IPv4
      uint16_t xport = readU16BE(buf + valuePos + 2);
      uint32_t xaddr = readU32BE(buf + valuePos + 4);
      *outPort = static_cast<uint16_t>(xport ^ (0x2112A442 >> 16));
      uint32_t addr = xaddr ^ 0x2112A442;
      *outIp = IPAddress((addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
      return true;
    }
    if (attrType == 0x0001 && attrLen >= 8 && buf[valuePos + 1] == 0x01) {  // MAPPED-ADDRESS, IPv4
      *outPort = readU16BE(buf + valuePos + 2);
      uint32_t addr = readU32BE(buf + valuePos + 4);
      *outIp = IPAddress((addr >> 24) & 0xFF, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
      return true;
    }
    size_t padded = (attrLen + 3) & ~static_cast<size_t>(3);
    pos = valuePos + padded;
  }
  return false;
}

void sendUdpPunch() {
  if (!g_call.peerEndpointKnown) return;
  uint8_t pkt[2 + 1 + kIdLen];
  writeU16(pkt, kUdpMagic);
  pkt[2] = UDP_PUNCH;
  memset(pkt + 3, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(pkt + 3), g_call.session_id, kIdLen - 1);
  if (g_udp.beginPacket(g_call.peerIp, g_call.peerPort)) {
    g_udp.write(pkt, sizeof(pkt));
    g_udp.endPacket();
  }
}

void sendUdpVoice(const int16_t* samples, size_t count) {
  if (!g_call.peerEndpointKnown) return;
  uint8_t pkt[2 + 1 + kIdLen + 2 + RadioAudio::kFrameBytes];
  size_t pos = 0;
  writeU16(pkt, kUdpMagic);
  pos += 2;
  pkt[pos++] = UDP_VOICE;
  memset(pkt + pos, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(pkt + pos), g_call.session_id, kIdLen - 1);
  pos += kIdLen;
  writeU16(pkt + pos, g_call.txSequence++);
  pos += 2;
  for (size_t i = 0; i < count; i++) {
    writeU16(pkt + pos, static_cast<uint16_t>(samples[i]));
    pos += 2;
  }
  if (g_udp.beginPacket(g_call.peerIp, g_call.peerPort)) {
    g_udp.write(pkt, pos);
    g_udp.endPacket();
  }
}

void buildRadioAudioTopic(const char* deviceId, char* out, size_t outSize) {
  snprintf(out, outSize, "radio/audio/%s", deviceId);
}

void sendMqttVoice(const int16_t* samples, size_t count) {
  char topicSuffix[32];
  buildRadioAudioTopic(g_call.peer_device_id, topicSuffix, sizeof(topicSuffix));
  publishAudioPacket(g_call.group_code, topicSuffix, AUDIO_PRIVATE, nullptr, g_call.txSequence++, samples, count);
}

void flushPreBuffer() {
  for (uint16_t i = 0; i < g_preBuffer.count; i++) {
    const int16_t* frame = g_preBuffer.data + static_cast<size_t>(i) * RadioAudio::kFrameSamples;
    if (g_call.state == PrivateState::UDP_DIRECT) {
      sendUdpVoice(frame, RadioAudio::kFrameSamples);
    } else {
      sendMqttVoice(frame, RadioAudio::kFrameSamples);
    }
  }
  freePreBuffer();
}

void resetPrivateCall() {
  RadioAudio::stopCapture();
  setRadioAudioActive(false);
  freePreBuffer();
  g_stunPending = false;
  jitterReset();
  g_call = PrivateCall{};
}

void onPrivateMicFrame(const int16_t* samples, size_t count) {
  switch (g_call.state) {
    case PrivateState::NEGOTIATING:
      bufferPreFrame(samples, count);
      break;
    case PrivateState::UDP_DIRECT:
      sendUdpVoice(samples, count);
      break;
    case PrivateState::MQTT_FALLBACK:
      sendMqttVoice(samples, count);
      break;
    default:
      break;
  }
}

void beginNegotiation() {
  g_call.state = PrivateState::NEGOTIATING;
  g_call.negotiateStartMs = millis();
  g_call.myEndpointKnown = false;
  g_call.peerEndpointKnown = false;
  allocatePreBuffer();
  RadioAudio::startCapture(onPrivateMicFrame);
  sendStunRequest();
}

void handleRadioBusyReplyPacket(const char* group_code, const char* topic, const uint8_t* payload,
                                size_t payloadLen) {
  (void)group_code;
  (void)topic;
  if (g_call.state != PrivateState::CLAIMING) return;
  uint8_t type;
  char responder[kDeviceIdLen];
  if (!parseReplyPayload(payload, static_cast<uint16_t>(payloadLen), &type, responder)) return;
  if (strcmp(responder, g_call.peer_device_id) != 0) return;

  if (type == REPLY_GRANT) {
    beginNegotiation();
  } else if (type == REPLY_DENY) {
    g_call.state = PrivateState::IDLE;
    g_call.denied = true;
    setRadioAudioActive(false);
  }
}

void publishSessionStart() {
  uint8_t payload[1 + kIdLen + kDeviceIdLen + kDeviceIdLen + 4 + 2];
  size_t len = buildSessionPayload(SESSION_START, g_call.session_id, Identity::deviceId(), g_call.peer_device_id,
                                   g_call.myIp, g_call.myPort, payload, sizeof(payload));
  if (len == 0) return;
  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_SESSION, static_cast<uint16_t>(len), wireBuf,
                                 sizeof(wireBuf))) {
    return;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
  MqttManager::publishBinary(g_call.group_code, "radio/session", wireBuf,
                             static_cast<uint16_t>(PacketCodec::kHeaderSize + len), false, 0);
}

void publishSessionAck() {
  uint8_t payload[1 + kIdLen + kDeviceIdLen + kDeviceIdLen + 4 + 2];
  size_t len = buildSessionPayload(SESSION_ACK, g_call.session_id, Identity::deviceId(), g_call.peer_device_id,
                                   g_call.myIp, g_call.myPort, payload, sizeof(payload));
  if (len == 0) return;
  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_SESSION, static_cast<uint16_t>(len), wireBuf,
                                 sizeof(wireBuf))) {
    return;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
  MqttManager::publishBinary(g_call.group_code, "radio/session", wireBuf,
                             static_cast<uint16_t>(PacketCodec::kHeaderSize + len), false, 0);
}

void handleRadioSessionPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)topic;
  uint8_t type;
  char sessionId[kIdLen], fromId[kDeviceIdLen], toId[kDeviceIdLen];
  IPAddress ip;
  uint16_t port;
  if (!parseSessionPayload(payload, static_cast<uint16_t>(payloadLen), &type, sessionId, fromId, toId, &ip, &port)) {
    return;
  }
  if (strcmp(toId, Identity::deviceId()) != 0) return;  // not addressed to me

  if (type == SESSION_START) {
    // Callee side: a valid private claim must already have granted `fromId`.
    if (!g_receiverBusy.busy || strcmp(g_receiverBusy.claimerDeviceId, fromId) != 0) return;
    if (g_call.state != PrivateState::IDLE && strcmp(g_call.session_id, sessionId) != 0) return;

    strncpy(g_call.group_code, group_code, sizeof(g_call.group_code) - 1);
    strncpy(g_call.peer_device_id, fromId, sizeof(g_call.peer_device_id) - 1);
    strncpy(g_call.session_id, sessionId, sizeof(g_call.session_id) - 1);
    g_call.isCaller = false;
    g_call.peerIp = ip;
    g_call.peerPort = port;
    g_call.peerEndpointKnown = true;
    g_call.state = PrivateState::NEGOTIATING;
    g_call.negotiateStartMs = millis();
    if (!g_call.myEndpointKnown) sendStunRequest();
    sendUdpPunch();
  } else if (type == SESSION_ACK) {
    if (strcmp(g_call.session_id, sessionId) != 0 || !g_call.isCaller) return;
    g_call.peerIp = ip;
    g_call.peerPort = port;
    g_call.peerEndpointKnown = true;
    sendUdpPunch();
  }
}

// ---- Jitter buffer: 3 packets (Phase 4 section 6) --------------------------
struct JitterSlot {
  bool used = false;
  uint16_t seq = 0;
  int16_t samples[RadioAudio::kFrameSamples];
};
constexpr uint8_t kJitterSize = 3;
JitterSlot g_jitter[kJitterSize];
bool g_jitterStarted = false;
uint16_t g_nextPlaySeq = 0;
uint32_t g_lastJitterPlayMs = 0;

void jitterReset() {
  for (auto& s : g_jitter) s.used = false;
  g_jitterStarted = false;
}

void jitterInsert(uint16_t seq, const int16_t* samples, size_t count) {
  if (g_jitterStarted && static_cast<int16_t>(seq - g_nextPlaySeq) < 0) return;  // older than playback point: drop
  for (auto& s : g_jitter) {
    if (s.used && s.seq == seq) return;  // duplicate
  }
  JitterSlot* target = nullptr;
  for (auto& s : g_jitter) {
    if (!s.used) {
      target = &s;
      break;
    }
  }
  if (target == nullptr) {
    // Full: evict whichever buffered slot is furthest ahead, but only if the
    // new packet is actually earlier than it — otherwise just drop the new
    // (even-further-ahead) one instead.
    target = &g_jitter[0];
    for (auto& s : g_jitter) {
      if (static_cast<int16_t>(s.seq - target->seq) > 0) target = &s;
    }
    if (static_cast<int16_t>(seq - target->seq) >= 0) return;
  }
  target->used = true;
  target->seq = seq;
  size_t n = (count < RadioAudio::kFrameSamples) ? count : RadioAudio::kFrameSamples;
  memcpy(target->samples, samples, n * sizeof(int16_t));
  if (!g_jitterStarted) {
    g_jitterStarted = true;
    g_nextPlaySeq = seq;
  }
}

void jitterPlayTick() {
  if (!g_jitterStarted) return;
  uint32_t now = millis();
  if (now - g_lastJitterPlayMs < kJitterFrameMs) return;
  g_lastJitterPlayMs = now;

  JitterSlot* found = nullptr;
  for (auto& s : g_jitter) {
    if (s.used && s.seq == g_nextPlaySeq) {
      found = &s;
      break;
    }
  }
  if (found != nullptr) {
    if (backgroundPlaybackAllowed()) {
      RadioAudio::playFrame(found->samples, RadioAudio::kFrameSamples);
      Sleep::notifyActivity();
    }
    found->used = false;
  }
  g_nextPlaySeq++;
}

void handleIncomingUdp() {
  int packetSize = g_udp.parsePacket();
  if (packetSize <= 0) return;
  static uint8_t buf[700];
  int n = g_udp.read(buf, sizeof(buf));
  if (n <= 0) return;

  if (g_stunPending) {
    IPAddress ip;
    uint16_t port;
    if (parseStunResponse(buf, static_cast<size_t>(n), &ip, &port)) {
      g_stunPending = false;
      g_call.myIp = ip;
      g_call.myPort = port;
      g_call.myEndpointKnown = true;
      if (g_call.isCaller) {
        publishSessionStart();
      } else {
        publishSessionAck();
        sendUdpPunch();
      }
      return;
    }
  }

  if (static_cast<size_t>(n) < 3 + kIdLen || readU16(buf) != kUdpMagic) return;  // malformed / not ours
  uint8_t type = buf[2];
  char sessionId[kIdLen];
  memcpy(sessionId, buf + 3, kIdLen);
  sessionId[kIdLen - 1] = '\0';
  if (strcmp(sessionId, g_call.session_id) != 0) return;  // wrong session: reject

  if (g_call.state == PrivateState::NEGOTIATING) {
    g_call.state = PrivateState::UDP_DIRECT;
    jitterReset();
    if (g_call.isCaller) flushPreBuffer();
  }
  if (g_call.state != PrivateState::UDP_DIRECT) return;

  if (type == UDP_VOICE) {
    size_t hdr = 3 + kIdLen;  // magic(2) + type(1) + session_id(24), sequence follows
    if (static_cast<size_t>(n) < hdr + 2) return;
    uint16_t seq = readU16(buf + hdr);
    size_t pcmOffset = hdr + 2;
    size_t sampleCount = (static_cast<size_t>(n) - pcmOffset) / 2;
    if (sampleCount > RadioAudio::kFrameSamples) sampleCount = RadioAudio::kFrameSamples;
    static int16_t samples[RadioAudio::kFrameSamples];
    for (size_t i = 0; i < sampleCount; i++) samples[i] = static_cast<int16_t>(readU16(buf + pcmOffset + i * 2));
    jitterInsert(seq, samples, sampleCount);
  }
}

// =============================================================================
// Everyone (group broadcast) — MQTT only, no claim/STUN (Phase 4 section 8)
// =============================================================================
bool g_broadcastActive = false;
char g_broadcastGroup[33] = {0};
uint16_t g_broadcastTxSeq = 0;

void onBroadcastMicFrame(const int16_t* samples, size_t count) {
  publishAudioPacket(g_broadcastGroup, "radio/audio/broadcast", AUDIO_BROADCAST, nullptr, g_broadcastTxSeq++,
                     samples, count);
}

char g_currentBroadcastSpeaker[kDeviceIdLen] = {0};
uint32_t g_lastBroadcastFrameMs = 0;

void handlePrivateAudioScope(const char* group_code, const char* sender, const char* roomKey, uint16_t sequence,
                             const int16_t* samples, size_t sampleCount) {
  (void)group_code;
  (void)roomKey;
  (void)sequence;
  if (g_call.state != PrivateState::MQTT_FALLBACK && g_call.state != PrivateState::NEGOTIATING) return;
  if (strcmp(sender, g_call.peer_device_id) != 0) return;
  if (g_call.state == PrivateState::NEGOTIATING) g_call.state = PrivateState::MQTT_FALLBACK;
  if (!backgroundPlaybackAllowed()) return;
  RadioAudio::playFrame(samples, sampleCount);
  Sleep::notifyActivity();
}

void handleBroadcastAudioScope(const char* group_code, const char* sender, const char* roomKey, uint16_t sequence,
                               const int16_t* samples, size_t sampleCount) {
  (void)group_code;
  (void)roomKey;
  (void)sequence;
  if (!backgroundPlaybackAllowed()) return;
  bool sameSpeaker = strcmp(g_currentBroadcastSpeaker, sender) == 0;
  bool speakerStale = (millis() - g_lastBroadcastFrameMs) > kBroadcastSpeakerStaleMs;
  if (g_currentBroadcastSpeaker[0] != '\0' && !sameSpeaker && !speakerStale) return;  // another speaker owns it
  strncpy(g_currentBroadcastSpeaker, sender, sizeof(g_currentBroadcastSpeaker) - 1);
  g_lastBroadcastFrameMs = millis();
  RadioAudio::playFrame(samples, sampleCount);
  Sleep::notifyActivity();
}

// =============================================================================
// Background tick: claim refresh/TTL, negotiation timeout, UDP polling.
// =============================================================================
void serviceTick() {
  uint32_t now = millis();

  if (g_receiverBusy.busy && (now - g_receiverBusy.lastRefreshMs) > kBusyTtlMs) {
    if (!g_call.isCaller && g_call.state != PrivateState::IDLE &&
        strcmp(g_call.peer_device_id, g_receiverBusy.claimerDeviceId) == 0) {
      resetPrivateCall();  // crash fallback: same stale-session risk as the explicit RELEASE path
    }
    g_receiverBusy.busy = false;
  }

  if (g_call.state == PrivateState::CLAIMING) {
    if (now - g_call.claimSentMs > kClaimReplyTimeoutMs) {
      resetPrivateCall();  // no reply: give up quietly, matching a DENY-like outcome
    }
  } else if (g_call.state == PrivateState::NEGOTIATING) {
    if (now - g_call.negotiateStartMs > kNegotiateTimeoutMs) {
      g_call.state = PrivateState::MQTT_FALLBACK;
      if (g_call.isCaller) flushPreBuffer();
    }
  } else if (g_call.isCaller && g_call.state != PrivateState::IDLE && g_call.state != PrivateState::STOPPING) {
    if (now - g_call.claimSentMs > kClaimRefreshMs) {
      uint8_t payload[1 + kDeviceIdLen];
      size_t len = buildBusyPayload(BUSY_CLAIM, Identity::deviceId(), payload, sizeof(payload));
      if (len > 0) {
        uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
        if (PacketCodec::encodeHeader(PacketCodec::PK_RADIO_BUSY, static_cast<uint16_t>(len), wireBuf,
                                      sizeof(wireBuf))) {
          memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
          char topicSuffix[32];
          snprintf(topicSuffix, sizeof(topicSuffix), "radio/busy/%s", g_call.peer_device_id);
          MqttManager::publishBinary(g_call.group_code, topicSuffix, wireBuf,
                                     static_cast<uint16_t>(PacketCodec::kHeaderSize + len), false, 0);
        }
      }
      g_call.claimSentMs = now;
    }
  }

  if (g_udpBegun) handleIncomingUdp();
  if (g_call.state == PrivateState::UDP_DIRECT) jitterPlayTick();
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = nullptr;
    svc.tick = serviceTick;
    registerAppService(svc);
    registerNetworkPacketHandler(PacketCodec::PK_RADIO_BUSY, handleRadioBusyPacket);
    registerNetworkPacketHandler(PacketCodec::PK_RADIO_BUSY_REPLY, handleRadioBusyReplyPacket);
    registerNetworkPacketHandler(PacketCodec::PK_RADIO_SESSION, handleRadioSessionPacket);
    registerNetworkPacketHandler(PacketCodec::PK_RADIO_AUDIO, handleRadioAudioPacket);
    registerAudioScopeHandler(AUDIO_PRIVATE, handlePrivateAudioScope);
    registerAudioScopeHandler(AUDIO_BROADCAST, handleBroadcastAudioScope);
  }
};
Registrar g_registrar;

}  // namespace

void setUiContext(UiContext ctx) { g_uiContext = ctx; }

void startPrivateCall(const char* group_code, const char* recipient_device_id) {
  if (g_call.state != PrivateState::IDLE) return;
  ensureUdpBegun();
  strncpy(g_call.group_code, group_code, sizeof(g_call.group_code) - 1);
  strncpy(g_call.peer_device_id, recipient_device_id, sizeof(g_call.peer_device_id) - 1);
  Identity::nextId(g_call.session_id, sizeof(g_call.session_id));
  g_call.isCaller = true;
  g_call.denied = false;
  g_call.state = PrivateState::CLAIMING;
  g_call.claimSentMs = millis();
  setRadioAudioActive(true);

  uint8_t payload[1 + kDeviceIdLen];
  size_t len = buildBusyPayload(BUSY_CLAIM, Identity::deviceId(), payload, sizeof(payload));
  if (len == 0) return;
  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_BUSY, static_cast<uint16_t>(len), wireBuf, sizeof(wireBuf))) {
    return;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
  char topicSuffix[32];
  snprintf(topicSuffix, sizeof(topicSuffix), "radio/busy/%s", recipient_device_id);
  MqttManager::publishBinary(group_code, topicSuffix, wireBuf,
                             static_cast<uint16_t>(PacketCodec::kHeaderSize + len), false, 0);
}

void stopPrivateCall() {
  if (g_call.state == PrivateState::IDLE && !g_call.denied) return;
  if (g_call.isCaller && g_call.state != PrivateState::IDLE) {
    uint8_t payload[1 + kDeviceIdLen];
    size_t len = buildBusyPayload(BUSY_RELEASE, Identity::deviceId(), payload, sizeof(payload));
    if (len > 0) {
      uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
      if (PacketCodec::encodeHeader(PacketCodec::PK_RADIO_BUSY, static_cast<uint16_t>(len), wireBuf,
                                    sizeof(wireBuf))) {
        memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
        char topicSuffix[32];
        snprintf(topicSuffix, sizeof(topicSuffix), "radio/busy/%s", g_call.peer_device_id);
        MqttManager::publishBinary(g_call.group_code, topicSuffix, wireBuf,
                                   static_cast<uint16_t>(PacketCodec::kHeaderSize + len), false, 0);
      }
    }
  }
  resetPrivateCall();
}

PrivateState getPrivateState() { return g_call.state; }
bool isPrivateDenied() { return g_call.denied; }

void startBroadcastCall(const char* group_code) {
  if (g_broadcastActive) return;
  strncpy(g_broadcastGroup, group_code, sizeof(g_broadcastGroup) - 1);
  g_broadcastGroup[sizeof(g_broadcastGroup) - 1] = '\0';
  g_broadcastTxSeq = 0;
  g_broadcastActive = true;
  setRadioAudioActive(true);
  RadioAudio::startCapture(onBroadcastMicFrame);
}

void stopBroadcastCall() {
  if (!g_broadcastActive) return;
  RadioAudio::stopCapture();
  setRadioAudioActive(false);
  g_broadcastActive = false;
}

bool isBroadcasting() { return g_broadcastActive; }

}  // namespace RadioTransport
