#include "core/radio_transport.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_heap_caps.h>
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

void releaseAndResetPrivateCall();  // defined with the public API, below

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
constexpr uint32_t kClaimRetryIntervalMs = 250;   // Phase 4 acceptance: retry CLAIM every 250ms...
constexpr uint32_t kClaimTotalTimeoutMs = 1000;   // ...for up to 1s after the first published CLAIM before failing
// Gate 1: the 1 s window above is a reply window -- it starts when a CLAIM is
// actually published, not when one is queued. While the group's connection
// is lent to the MQTT worker (a Race heartbeat waiting up to 5 s for its
// PUBACK, a chat send, the Outbox), a CLAIM waits in that chain; counting
// the window from the enqueue timed it out before it ever left the device.
// The queue wait itself is capped here (normal heartbeat ~0.6 s), so a held
// PTT shows "Claiming..." for at most this + the reply window (3 s), then
// "Send timeout". A CLAIM published after that is followed, in the same
// chain, by the RELEASE for its session (see releaseAndResetPrivateCall()):
// the receiver grants and releases at once; its GRANT is a late reply here.
constexpr uint32_t kClaimQueueMaxMs = 2000;
constexpr uint32_t kBusyTtlMs = 5000;
constexpr uint32_t kNegotiateTimeoutMs = 2000;
constexpr uint32_t kJitterFrameMs = 20;
constexpr uint32_t kBroadcastSpeakerStaleMs = 500;

// CLAIM/RELEASE and GRANT/DENY both carry the exact session_id (Addendum:
// Channel Busy must be correlated per-session, not just per-device — a
// stale RELEASE or stale GRANT/DENY from an earlier call with the same
// peer must never affect a newer one).
size_t buildBusyPayload(uint8_t type, const char* claimerDeviceId, const char* sessionId, uint8_t* out, size_t cap) {
  size_t need = 1 + kDeviceIdLen + kIdLen;
  if (cap < need) return 0;
  size_t pos = 0;
  out[pos++] = type;
  memset(out + pos, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), claimerDeviceId, kDeviceIdLen - 1);
  pos += kDeviceIdLen;
  memset(out + pos, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), sessionId, kIdLen - 1);
  pos += kIdLen;
  return pos;
}
bool parseBusyPayload(const uint8_t* data, uint16_t len, uint8_t* outType, char* outDeviceId, char* outSessionId) {
  if (data == nullptr || len < 1 + kDeviceIdLen + kIdLen) return false;
  size_t pos = 0;
  *outType = data[pos++];
  memcpy(outDeviceId, data + pos, kDeviceIdLen);
  outDeviceId[kDeviceIdLen - 1] = '\0';
  pos += kDeviceIdLen;
  memcpy(outSessionId, data + pos, kIdLen);
  outSessionId[kIdLen - 1] = '\0';
  return true;
}

size_t buildReplyPayload(uint8_t type, const char* responderDeviceId, const char* sessionId, uint8_t* out,
                         size_t cap) {
  size_t need = 1 + kDeviceIdLen + kIdLen;
  if (cap < need) return 0;
  size_t pos = 0;
  out[pos++] = type;
  memset(out + pos, 0, kDeviceIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), responderDeviceId, kDeviceIdLen - 1);
  pos += kDeviceIdLen;
  memset(out + pos, 0, kIdLen);
  strncpy(reinterpret_cast<char*>(out + pos), sessionId, kIdLen - 1);
  pos += kIdLen;
  return pos;
}
bool parseReplyPayload(const uint8_t* data, uint16_t len, uint8_t* outType, char* outDeviceId, char* outSessionId) {
  if (data == nullptr || len < 1 + kDeviceIdLen + kIdLen) return false;
  size_t pos = 0;
  *outType = data[pos++];
  memcpy(outDeviceId, data + pos, kDeviceIdLen);
  outDeviceId[kDeviceIdLen - 1] = '\0';
  pos += kDeviceIdLen;
  memcpy(outSessionId, data + pos, kIdLen);
  outSessionId[kIdLen - 1] = '\0';
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
  // Radio fix (hardware: "[PERF][RADIO_AUDIO] serviceTick 289 ms" with
  // "[DIAG][MQTT] qos0 ... maxMs=288"): a frame is sent only if the socket
  // takes it without waiting, else skipped -- 50 frames a second of 640-byte
  // PCM (about 37 KB/s on the wire) can exceed what TCP carries over a
  // long round trip, and waiting for buffer space held the main loop.
  return MqttManager::publishBinaryNoWait(group_code, topic_suffix, wireBuf,
                                          static_cast<uint16_t>(PacketCodec::kHeaderSize + payloadLen));
}

namespace {

bool g_maintenanceMode = false;

void handleRadioAudioPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)topic;
  if (g_maintenanceMode) return;  // Phase 5 OTA: incoming Radio playback disabled
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
  char sessionId[kIdLen] = {0};
  uint32_t lastRefreshMs = 0;
};
ReceiverBusy g_receiverBusy;

// g_call (PrivateCall) is declared further below; these two are forward-
// declared so handleRadioBusyPacket — which needs to react to a same-
// session RELEASE/TTL-expiry on the callee side — can compile here without
// moving the Busy protocol code down past the rest of the private-call
// state. Implementations sit right after `PrivateCall g_call;` is declared.
void resetPrivateCall();
void releaseCalleeCallIfMatches(const char* claimerDeviceId, const char* sessionId);

const char* outcomeName(MqttManager::ControlOutcome o) {
  return o == MqttManager::ControlOutcome::PUBLISHED ? "published"
         : o == MqttManager::ControlOutcome::QUEUED  ? "queued"
                                                      : "failed";
}

// A queued control packet's actual result (GRANT/DENY, SESSION_START/ACK,
// RELEASE): logged when it fails after being queued, the case a "queued"
// line alone cannot show.
uint32_t g_replyTicket = 0;  // the last queued GRANT/DENY, for its queue time
uint32_t g_replyQueuedAtMs = 0;
void onControlPublishDone(uint32_t ticket, bool ok) {
  if (ticket != 0 && ticket == g_replyTicket) {
    g_replyTicket = 0;
    Serial.printf("[RADIO][BUSY] queued reply ticket=%lu %s queuedMs=%lu\n", static_cast<unsigned long>(ticket),
                  ok ? "published" : "failed", static_cast<unsigned long>(millis() - g_replyQueuedAtMs));
    return;
  }
  if (!ok) Serial.printf("[RADIO][MQTT] queued control packet ticket=%lu failed\n", static_cast<unsigned long>(ticket));
}

MqttManager::ControlOutcome sendBusyReply(const char* group_code, const char* toDeviceId, const char* sessionId,
                                          uint8_t type) {
  uint8_t payload[1 + kDeviceIdLen + kIdLen];
  size_t len = buildReplyPayload(type, Identity::deviceId(), sessionId, payload, sizeof(payload));
  if (len == 0) return MqttManager::ControlOutcome::FAILED;
  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_BUSY_REPLY, static_cast<uint16_t>(len), wireBuf,
                                 sizeof(wireBuf))) {
    return MqttManager::ControlOutcome::FAILED;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
  char topicSuffix[32];
  snprintf(topicSuffix, sizeof(topicSuffix), "radio/busy_reply/%s", toDeviceId);
  uint32_t ticket = 0;
  MqttManager::ControlOutcome o =
      MqttManager::publishControl(group_code, topicSuffix, wireBuf, static_cast<uint16_t>(PacketCodec::kHeaderSize + len),
                                  onControlPublishDone, &ticket);
  if (o == MqttManager::ControlOutcome::QUEUED) {
    g_replyTicket = ticket;
    g_replyQueuedAtMs = millis();
  }
  return o;
}

void handleRadioBusyPacket(const char* group_code, const char* topic, const uint8_t* payload, size_t payloadLen) {
  (void)topic;
  uint8_t type;
  char claimer[kDeviceIdLen];
  char sessionId[kIdLen];
  if (!parseBusyPayload(payload, static_cast<uint16_t>(payloadLen), &type, claimer, sessionId)) return;

  if (type == BUSY_CLAIM) {
    // Fix Phase 1C: granting implicitly promises the claimer a
    // SESSION_START/SESSION_ACK exchange on OUR OWN radio/session
    // subscription (topic #11 of 13) shortly after -- if that subscription
    // isn't active yet (a real possibility once setup is staged across
    // ticks), the claimer's own SESSION_START would simply never be
    // delivered to us (plain MQTT: a message published before a
    // subscription takes effect is not retroactively delivered once it
    // does), leaving them stuck in NEGOTIATING with nothing to show for a
    // GRANT that promised otherwise. isGroupReady() implies subscribeAll()
    // already fully succeeded, so gating the grant on it closes this for
    // the whole session flow in one place, not just this one reply.
    // Deliberately do NOTHING (neither grant nor deny) when not ready --
    // sending DENY here would be actively misleading (it tells the caller
    // "permanently busy, stop retrying" via g_call.denied=true, when the
    // real situation is "temporarily not ready yet"); staying silent lets
    // the caller's own existing 250ms-retry/1s-total-timeout claim logic
    // (radio_transport.cpp's own kClaimRetryIntervalMs/kClaimTotalTimeoutMs)
    // naturally retry once we do become ready, or fail gracefully exactly
    // like reaching an unreachable receiver does today. This does NOT
    // apply to BUSY_RELEASE below -- releasing only ever tears down
    // existing state and promises no reply of its own, so it is never held
    // back by this same-group readiness check.
    if (!MqttManager::isGroupReady(group_code)) {
      Serial.printf("[RADIO][BUSY] claim from=%s sid=%s ignored: group not ready\n", claimer, sessionId);
      return;
    }

    // Same claimer is accepted even with a different session_id (a fresh
    // re-claim after a clean hang-up) — only a DIFFERENT claimer while
    // busy is denied.
    bool sameClaimant = g_receiverBusy.busy && strcmp(g_receiverBusy.claimerDeviceId, claimer) == 0;
    if (!g_receiverBusy.busy || sameClaimant) {
      // A same-claimant re-claim with a DIFFERENT session_id means the
      // caller's previous session was abandoned without a RELEASE (e.g. a
      // WiFi drop mid-call) and they are now redialing. Our own callee-side
      // g_call for that stale session would otherwise still be sitting in
      // NEGOTIATING/UDP_DIRECT/MQTT_FALLBACK and reject the upcoming
      // SESSION_START (session_id mismatch) until g_receiverBusy's TTL
      // sweep eventually clears it -- tear it down proactively here, the
      // same way an explicit RELEASE or that TTL sweep already would, so
      // the fresh call is not silently blocked.
      if (sameClaimant && strcmp(g_receiverBusy.sessionId, sessionId) != 0) {
        releaseCalleeCallIfMatches(claimer, g_receiverBusy.sessionId);
      }
      bool firstForSession = !sameClaimant || strcmp(g_receiverBusy.sessionId, sessionId) != 0;
      g_receiverBusy.busy = true;
      strncpy(g_receiverBusy.claimerDeviceId, claimer, sizeof(g_receiverBusy.claimerDeviceId) - 1);
      strncpy(g_receiverBusy.sessionId, sessionId, sizeof(g_receiverBusy.sessionId) - 1);
      g_receiverBusy.lastRefreshMs = millis();
      MqttManager::ControlOutcome o = sendBusyReply(group_code, claimer, sessionId, REPLY_GRANT);
      // Logged once per session (the caller's 2 s keep-alive CLAIMs are
      // re-granted silently) and on every reply not published at once.
      if (firstForSession || o != MqttManager::ControlOutcome::PUBLISHED) {
        Serial.printf("[RADIO][BUSY] claim from=%s sid=%s -> GRANT %s\n", claimer, sessionId, outcomeName(o));
      }
    } else {
      MqttManager::ControlOutcome o = sendBusyReply(group_code, claimer, sessionId, REPLY_DENY);
      Serial.printf("[RADIO][BUSY] claim from=%s sid=%s -> DENY (busy with %s) %s\n", claimer, sessionId,
                    g_receiverBusy.claimerDeviceId, outcomeName(o));
    }
  } else if (type == BUSY_RELEASE) {
    // Only honor a RELEASE that matches the exact session we granted —
    // otherwise a stale RELEASE from an earlier call with this same peer
    // could tear down a newer, still-active grant.
    if (g_receiverBusy.busy && strcmp(g_receiverBusy.claimerDeviceId, claimer) == 0 &&
        strcmp(g_receiverBusy.sessionId, sessionId) == 0) {
      g_receiverBusy.busy = false;
    }
    releaseCalleeCallIfMatches(claimer, sessionId);
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
// Heap safety (hardware: minimum free heap 12184 bytes): the full pre-buffer
// is 64000 bytes, by far the largest single allocation in the firmware. It
// is only as large as leaves kPreBufferHeapReserve of internal heap for the
// network stack, MQTT and LittleFS (and fits the largest free block); below
// kPreBufferMinFrames it is not allocated at all -- the same outcome as a
// failed allocation before (the words spoken while negotiating are lost,
// the call itself goes ahead).
constexpr size_t kPreBufferHeapReserve = 32 * 1024;
constexpr uint16_t kPreBufferMinFrames = 10;  // 200 ms
constexpr uint32_t kHeapCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
PreBuffer g_preBuffer;

uint16_t preBufferFramesFor(size_t freeBytes, size_t largestBlock) {
  constexpr size_t kFrameBytes = RadioAudio::kFrameSamples * sizeof(int16_t);
  size_t byFree = freeBytes > kPreBufferHeapReserve ? (freeBytes - kPreBufferHeapReserve) / kFrameBytes : 0;
  size_t byBlock = largestBlock / kFrameBytes;
  size_t frames = byFree < byBlock ? byFree : byBlock;
  if (frames > kPreBufferMaxFrames) frames = kPreBufferMaxFrames;
  return frames < kPreBufferMinFrames ? 0 : static_cast<uint16_t>(frames);
}

void allocatePreBuffer() {
  if (g_preBuffer.data != nullptr) return;
  size_t freeBefore = heap_caps_get_free_size(kHeapCaps);
  size_t largestBefore = heap_caps_get_largest_free_block(kHeapCaps);
  uint16_t frames = preBufferFramesFor(freeBefore, largestBefore);
  g_preBuffer.data =
      frames > 0 ? new (std::nothrow) int16_t[static_cast<size_t>(frames) * RadioAudio::kFrameSamples] : nullptr;
  g_preBuffer.capacityFrames = (g_preBuffer.data != nullptr) ? frames : 0;
  g_preBuffer.count = 0;
  Serial.printf("[HEAP][RADIO] prebuffer frames=%u bytes=%u freeBefore=%u largestBefore=%u minFree=%u\n",
                static_cast<unsigned>(g_preBuffer.capacityFrames),
                static_cast<unsigned>(g_preBuffer.capacityFrames * RadioAudio::kFrameSamples * sizeof(int16_t)),
                static_cast<unsigned>(freeBefore), static_cast<unsigned>(largestBefore),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(kHeapCaps)));
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
  uint32_t claimFirstSentMs = 0;  // when CLAIMING started — bounds the queue wait (kClaimQueueMaxMs) / unsent window
  uint32_t claimLastSentMs = 0;   // last CLAIM sent — paced at 250ms while CLAIMING, 2s once active
  uint32_t negotiateStartMs = 0;
  bool myEndpointKnown = false;
  IPAddress myIp;
  uint16_t myPort = 0;
  bool peerEndpointKnown = false;
  IPAddress peerIp;
  uint16_t peerPort = 0;
  uint16_t txSequence = 0;
  bool denied = false;
  uint8_t claimSends = 0;      // CLAIMs attempted while CLAIMING (diagnostics, failure kind)
  uint8_t claimSendFails = 0;  // ...of which refused at once or failed after queuing
  uint8_t claimQueued = 0;     // ...of which queued behind worker publishes (connection lent)
  uint8_t claimPublished = 0;  // ...of which actually written (at once, or reported by the worker)
  // Gate 1: a CLAIM in the worker's chain whose result is not back yet (0:
  // none). No retry is queued behind it -- it would only wait in the same
  // chain -- and the reply window has not started.
  uint32_t claimQueuedTicket = 0;
  uint32_t claimQueuedAtMs = 0;
  uint32_t claimPublishedMs = 0;  // first CLAIM actually written (0: none yet): starts the reply window
};
PrivateCall g_call;

// Radio fix (hardware: holding PTT, "Claiming..." went back to "Ready"): the
// outcome of the last claim that ended without a GRANT, kept -- unlike
// g_call, which the timeout resets -- until the PTT is released or a new
// press starts. g_failedSessionId lets a GRANT/DENY arriving after the
// timeout be recognized and logged ("late-reply").
PrivateFailure g_lastFailure = PrivateFailure::NONE;
char g_failedSessionId[kIdLen] = {0};
uint32_t g_failedAtMs = 0;

// Gate 1: a session given up (timeout or PTT release) while its CLAIM was
// still queued. Its RELEASE goes into the same chain, behind the CLAIM, so
// the receiver sees them in order. If that RELEASE could not be queued (the
// chain full), it is owed: published when that CLAIM's own result comes back
// -- the result frees a place in the chain (or ends the loan), and only a
// CLAIM that was actually written needs withdrawing.
//
// One entry per abandoned session, keyed by its CLAIM's ticket (final review:
// a single slot let a second session abandoned before the first CLAIM's
// result overwrite -- and delete -- the first session's owed RELEASE, so
// CLAIM(S1) went out with no RELEASE(S1)). Bounded: an entry lives only
// while its CLAIM is in the worker's queue (MqttManager: kJobQueueDepth 8,
// and every queued job reports exactly one result, canceled loans included),
// and kMaxOwedReleases caps the list besides. Heap-held, a node only while
// owed (rare); static RAM is the one head pointer.
struct OwedRelease {
  uint32_t claimTicket;
  char group_code[33];
  char peer_device_id[kDeviceIdLen];
  char session_id[kIdLen];
  OwedRelease* next;
};
constexpr uint8_t kMaxOwedReleases = 8;
OwedRelease* g_owedRelease = nullptr;  // list head

uint8_t owedReleaseCount() {
  uint8_t n = 0;
  for (OwedRelease* r = g_owedRelease; r != nullptr; r = r->next) n++;
  return n;
}

// Unlinks and returns the entry for this CLAIM ticket (nullptr: none).
OwedRelease* takeOwedRelease(uint32_t ticket) {
  for (OwedRelease** pp = &g_owedRelease; *pp != nullptr; pp = &(*pp)->next) {
    if ((*pp)->claimTicket == ticket) {
      OwedRelease* r = *pp;
      *pp = r->next;
      return r;
    }
  }
  return nullptr;
}

MqttManager::ControlOutcome sendBusyPacket(const char* group_code, const char* peer, uint8_t type, const char* sessionId,
                                           MqttManager::PublishDoneFn done, uint32_t* ticket) {
  if (ticket != nullptr) *ticket = 0;
  uint8_t payload[1 + kDeviceIdLen + kIdLen];
  size_t len = buildBusyPayload(type, Identity::deviceId(), sessionId, payload, sizeof(payload));
  if (len == 0) return MqttManager::ControlOutcome::FAILED;
  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_BUSY, static_cast<uint16_t>(len), wireBuf, sizeof(wireBuf))) {
    return MqttManager::ControlOutcome::FAILED;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
  char topicSuffix[32];
  snprintf(topicSuffix, sizeof(topicSuffix), "radio/busy/%s", peer);
  return MqttManager::publishControl(group_code, topicSuffix, wireBuf,
                                     static_cast<uint16_t>(PacketCodec::kHeaderSize + len), done, ticket);
}

void onClaimPublishDone(uint32_t ticket, bool ok);

// Sends (or resends) a CLAIM for the current g_call — used for the initial
// claim, the 250ms/1s retry cadence while CLAIMING, and the 2s keep-alive
// refresh once active. Always carries g_call's own session_id so replies
// and TTL tracking stay correlated to this exact call. Gate 1: published,
// queued (the connection lent to the worker) and failed are counted apart;
// while a CLAIM is still queued no further one is queued behind it.
bool sendClaim() {
  if (g_call.claimQueuedTicket != 0) return true;  // the previous CLAIM has not left yet
  bool claiming = g_call.state == PrivateState::CLAIMING;
  uint32_t ticket = 0;
  MqttManager::ControlOutcome o =
      sendBusyPacket(g_call.group_code, g_call.peer_device_id, BUSY_CLAIM, g_call.session_id, onClaimPublishDone, &ticket);
  if (claiming) g_call.claimSends++;
  if (o == MqttManager::ControlOutcome::PUBLISHED) {
    if (claiming) {
      g_call.claimPublished++;
      if (g_call.claimPublishedMs == 0) g_call.claimPublishedMs = millis() | 1;
    }
  } else if (o == MqttManager::ControlOutcome::QUEUED) {
    g_call.claimQueuedTicket = ticket;
    g_call.claimQueuedAtMs = millis();
    if (claiming) g_call.claimQueued++;
    Serial.printf("[RADIO][CLAIM] queued sid=%s ticket=%lu (connection lent to the MQTT worker)\n", g_call.session_id,
                  static_cast<unsigned long>(ticket));
  } else {
    if (claiming) g_call.claimSendFails++;
    Serial.printf("[RADIO][CLAIM] publish failed sid=%s state=%u\n", g_call.session_id,
                  static_cast<unsigned>(g_call.state));
  }
  return o != MqttManager::ControlOutcome::FAILED;
}

// The worker's result for a queued CLAIM (main task, MqttManager's tick).
void onClaimPublishDone(uint32_t ticket, bool ok) {
  if (ticket != 0 && ticket == g_call.claimQueuedTicket) {
    g_call.claimQueuedTicket = 0;
    unsigned long queuedMs = static_cast<unsigned long>(millis() - g_call.claimQueuedAtMs);
    if (g_call.state == PrivateState::CLAIMING) {
      if (ok) {
        g_call.claimPublished++;
        if (g_call.claimPublishedMs == 0) g_call.claimPublishedMs = millis() | 1;
      } else {
        g_call.claimSendFails++;
      }
    }
    Serial.printf("[RADIO][CLAIM] %s sid=%s queuedMs=%lu\n", ok ? "published" : "queued publish failed",
                  g_call.session_id, queuedMs);
    return;
  }
  // Any other CLAIM result belongs to a session already given up (its
  // RELEASE either queued behind it, or owed below).
  if (ticket == 0) return;
  Serial.printf("[RADIO][CLAIM] abandoned-claim %s ticket=%lu (session already given up)\n",
                ok ? "published" : "failed", static_cast<unsigned long>(ticket));
  OwedRelease* r = takeOwedRelease(ticket);
  if (r == nullptr) return;
  if (ok) {
    MqttManager::ControlOutcome o =
        sendBusyPacket(r->group_code, r->peer_device_id, BUSY_RELEASE, r->session_id, onControlPublishDone, nullptr);
    // FAILED here is a lost connection (this result freed a place in the
    // chain): the receiver's busy TTL is then the only release left.
    Serial.printf("[RADIO][CLAIM] owed RELEASE sid=%s %s%s\n", r->session_id, outcomeName(o),
                  o == MqttManager::ControlOutcome::FAILED ? " (connection lost: receiver TTL applies)" : "");
  } else {
    // A failed or canceled CLAIM was never written (the connection closed or
    // the loan was canceled): nothing reached the receiver to withdraw.
    Serial.printf("[RADIO][CLAIM] owed RELEASE sid=%s dropped: its CLAIM was not delivered\n", r->session_id);
  }
  delete r;
}

// Definition of the helper forward-declared alongside resetPrivateCall,
// above the Busy protocol code — tears down our own (callee-side) call
// state when a matching-session RELEASE or TTL expiry arrives, so it never
// lingers stale and blocks this peer's next call.
void releaseCalleeCallIfMatches(const char* claimerDeviceId, const char* sessionId) {
  if (!g_call.isCaller && g_call.state != PrivateState::IDLE &&
      strcmp(g_call.peer_device_id, claimerDeviceId) == 0 && strcmp(g_call.session_id, sessionId) == 0) {
    resetPrivateCall();
  }
}

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
  // Only a caller-side call owns the microphone and the live-voice flag
  // (startPrivateCall() sets it, beginNegotiation() starts capture on the
  // GRANT; a callee never does either). Cross-mode ownership fix: a callee
  // session ended by RELEASE or the busy TTL while this device transmits in
  // another mode (Race Room PTT, Everyone broadcast -- the incoming CLAIM is
  // granted whatever screen is shown) used to stop that mode's capture and
  // clear its flag (and so its deep-sleep inhibition) mid-transmission.
  if (g_call.isCaller) {
    RadioAudio::stopCapture();
    setRadioAudioActive(false);
  }
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
  uint8_t type;
  char responder[kDeviceIdLen];
  char sessionId[kIdLen];
  if (!parseReplyPayload(payload, static_cast<uint16_t>(payloadLen), &type, responder, sessionId)) return;
  if (g_call.state != PrivateState::CLAIMING) {
    // A reply to the claim that already timed out: logged (it tells a slow
    // receiver or broker from a lost CLAIM/GRANT), never acted on -- that
    // session was released when it timed out.
    if (g_failedSessionId[0] != '\0' && strcmp(sessionId, g_failedSessionId) == 0) {
      Serial.printf("[RADIO][CLAIM] late-reply type=%s from=%s sid=%s afterTimeoutMs=%lu\n",
                    type == REPLY_GRANT ? "GRANT" : "DENY", responder, sessionId,
                    static_cast<unsigned long>(millis() - g_failedAtMs));
    }
    return;
  }
  if (strcmp(responder, g_call.peer_device_id) != 0) return;
  if (strcmp(sessionId, g_call.session_id) != 0) return;  // stale GRANT/DENY for an earlier attempt

  Serial.printf("[RADIO][CLAIM] %s sid=%s afterMs=%lu afterPublishMs=%lu sends=%u queued=%u published=%u sendFails=%u\n",
                type == REPLY_GRANT ? "granted" : "denied", sessionId,
                static_cast<unsigned long>(millis() - g_call.claimFirstSentMs),
                static_cast<unsigned long>(g_call.claimPublishedMs != 0 ? millis() - g_call.claimPublishedMs : 0),
                static_cast<unsigned>(g_call.claimSends), static_cast<unsigned>(g_call.claimQueued),
                static_cast<unsigned>(g_call.claimPublished), static_cast<unsigned>(g_call.claimSendFails));
  if (type == REPLY_GRANT) {
    beginNegotiation();
  } else if (type == REPLY_DENY) {
    g_call.state = PrivateState::IDLE;
    g_call.denied = true;
    setRadioAudioActive(false);
  }
}

bool publishSessionStart() {
  uint8_t payload[1 + kIdLen + kDeviceIdLen + kDeviceIdLen + 4 + 2];
  size_t len = buildSessionPayload(SESSION_START, g_call.session_id, Identity::deviceId(), g_call.peer_device_id,
                                   g_call.myIp, g_call.myPort, payload, sizeof(payload));
  if (len == 0) return false;
  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_SESSION, static_cast<uint16_t>(len), wireBuf,
                                 sizeof(wireBuf))) {
    return false;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
  MqttManager::ControlOutcome o =
      MqttManager::publishControl(g_call.group_code, "radio/session", wireBuf,
                                  static_cast<uint16_t>(PacketCodec::kHeaderSize + len), onControlPublishDone, nullptr);
  if (o != MqttManager::ControlOutcome::PUBLISHED)
    Serial.printf("[RADIO][SESSION] START %s sid=%s\n", outcomeName(o), g_call.session_id);
  return o != MqttManager::ControlOutcome::FAILED;
}

bool publishSessionAck() {
  uint8_t payload[1 + kIdLen + kDeviceIdLen + kDeviceIdLen + 4 + 2];
  size_t len = buildSessionPayload(SESSION_ACK, g_call.session_id, Identity::deviceId(), g_call.peer_device_id,
                                   g_call.myIp, g_call.myPort, payload, sizeof(payload));
  if (len == 0) return false;
  uint8_t wireBuf[PacketCodec::kHeaderSize + sizeof(payload)];
  if (!PacketCodec::encodeHeader(PacketCodec::PK_RADIO_SESSION, static_cast<uint16_t>(len), wireBuf,
                                 sizeof(wireBuf))) {
    return false;
  }
  memcpy(wireBuf + PacketCodec::kHeaderSize, payload, len);
  MqttManager::ControlOutcome o =
      MqttManager::publishControl(g_call.group_code, "radio/session", wireBuf,
                                  static_cast<uint16_t>(PacketCodec::kHeaderSize + len), onControlPublishDone, nullptr);
  if (o != MqttManager::ControlOutcome::PUBLISHED)
    Serial.printf("[RADIO][SESSION] ACK %s sid=%s\n", outcomeName(o), g_call.session_id);
  return o != MqttManager::ControlOutcome::FAILED;
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
    // Callee side: a valid private claim for this exact session must
    // already have been granted to `fromId` — rejects a START tied to a
    // stale/superseded claim from the same peer.
    if (!g_receiverBusy.busy || strcmp(g_receiverBusy.claimerDeviceId, fromId) != 0) return;
    if (strcmp(g_receiverBusy.sessionId, sessionId) != 0) return;
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
// Hardware Diagnostic #4.9e Part B4: instrumentation only, no behavior
// change -- total tick duration, gated to >=20ms.
void serviceTick() {
  uint32_t tickStart = millis();
  uint32_t now = tickStart;

  if (g_receiverBusy.busy && (now - g_receiverBusy.lastRefreshMs) > kBusyTtlMs) {
    // Crash fallback: same stale-session risk as the explicit RELEASE
    // path, so it goes through the same session-matched teardown helper.
    releaseCalleeCallIfMatches(g_receiverBusy.claimerDeviceId, g_receiverBusy.sessionId);
    g_receiverBusy.busy = false;
  }

  if (g_call.state == PrivateState::CLAIMING) {
    // Gate 1: the reply window runs from the first CLAIM actually published;
    // while one is still queued, the queue limit applies instead.
    uint32_t deadline = g_call.claimPublishedMs != 0  ? g_call.claimPublishedMs + kClaimTotalTimeoutMs
                        : g_call.claimQueuedTicket != 0 ? g_call.claimFirstSentMs + kClaimQueueMaxMs
                                                        : g_call.claimFirstSentMs + kClaimTotalTimeoutMs;
    if (static_cast<int32_t>(now - deadline) >= 0) {
      // No matching GRANT/DENY in time: the attempt fails, visibly ("No
      // response" 1 s after a published CLAIM, "Send timeout" when the
      // CLAIM was still queued at the 2 s queue limit, "Send failed" when
      // not one CLAIM could be published) until the PTT is released. A release cancels CLAIMING
      // first, so reaching here means the screen saw no release. RELEASE
      // goes out for this session, so a grant the receiver sends late is
      // withdrawn instead of holding its channel for the 5 s busy TTL.
      PrivateFailure f = g_call.claimPublishedMs != 0    ? PrivateFailure::NO_RESPONSE
                         : g_call.claimQueuedTicket != 0 ? PrivateFailure::SEND_TIMEOUT
                                                         : PrivateFailure::SEND_FAILED;
      Serial.printf(
          "[RADIO][CLAIM] timeout sid=%s peer=%s elapsedMs=%lu sends=%u queued=%u published=%u sendFails=%u "
          "stillQueued=%d result=%s\n",
          g_call.session_id, g_call.peer_device_id, static_cast<unsigned long>(now - g_call.claimFirstSentMs),
          static_cast<unsigned>(g_call.claimSends), static_cast<unsigned>(g_call.claimQueued),
          static_cast<unsigned>(g_call.claimPublished), static_cast<unsigned>(g_call.claimSendFails),
          g_call.claimQueuedTicket != 0 ? 1 : 0,
          f == PrivateFailure::NO_RESPONSE ? "no-response" : f == PrivateFailure::SEND_TIMEOUT ? "send-timeout" : "send-failed");
      strncpy(g_failedSessionId, g_call.session_id, sizeof(g_failedSessionId) - 1);
      g_failedSessionId[sizeof(g_failedSessionId) - 1] = '\0';
      g_failedAtMs = now;
      releaseAndResetPrivateCall();
      g_lastFailure = f;
    } else if (now - g_call.claimLastSentMs >= kClaimRetryIntervalMs) {
      sendClaim();
      g_call.claimLastSentMs = now;
    }
  } else if (g_call.state == PrivateState::NEGOTIATING) {
    if (now - g_call.negotiateStartMs > kNegotiateTimeoutMs) {
      g_call.state = PrivateState::MQTT_FALLBACK;
      if (g_call.isCaller) flushPreBuffer();
    }
  } else if (g_call.isCaller && g_call.state != PrivateState::IDLE && g_call.state != PrivateState::STOPPING) {
    if (now - g_call.claimLastSentMs > kClaimRefreshMs) {
      sendClaim();
      g_call.claimLastSentMs = now;
    }
  }

  if (g_udpBegun) handleIncomingUdp();
  if (g_call.state == PrivateState::UDP_DIRECT) jitterPlayTick();

  uint32_t tickElapsed = millis() - tickStart;
  if (tickElapsed >= 20) {
    Serial.printf("[PERF][RADIO_TRANSPORT] serviceTick %lu ms\n", static_cast<unsigned long>(tickElapsed));
  }
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

StartCallResult startPrivateCall(const char* group_code, const char* recipient_device_id) {
  if (g_maintenanceMode) return StartCallResult::UNAVAILABLE;  // Phase 5 OTA: no new Radio session may begin
  if (g_call.state != PrivateState::IDLE) return StartCallResult::UNAVAILABLE;
  // Fix Phase 1C: this call enters CLAIMING and starts sendClaim()'s own
  // retry timers immediately below, BEFORE any network I/O happens -- a
  // GRANT/DENY reply can only ever reach this device on its own
  // radio/busy_reply/<device> subscription (one of the 13), so starting a
  // claim before that subscription is confirmed active risks a reply that
  // silently can never be delivered (plain MQTT does not retroactively
  // deliver a message published before the matching subscribe() took
  // effect). isGroupReady() implies subscribeAll() already fully
  // succeeded, closing this for radio/session too (needed a few steps
  // later in the same flow) in the same one check. Returning NOT_READY
  // here (rather than silently no-op'ing, as this function used to) is
  // what lets the caller show real feedback instead of an apparently-dead
  // button; no state changes at all on this path -- CLAIMING is not
  // entered, no claim timer starts, mic capture is not activated.
  if (!MqttManager::isGroupReady(group_code)) return StartCallResult::NOT_READY;
  g_lastFailure = PrivateFailure::NONE;
  ensureUdpBegun();
  strncpy(g_call.group_code, group_code, sizeof(g_call.group_code) - 1);
  strncpy(g_call.peer_device_id, recipient_device_id, sizeof(g_call.peer_device_id) - 1);
  Identity::nextId(g_call.session_id, sizeof(g_call.session_id));
  g_call.isCaller = true;
  g_call.denied = false;
  g_call.state = PrivateState::CLAIMING;
  uint32_t now = millis();
  g_call.claimFirstSentMs = now;
  g_call.claimLastSentMs = now;
  setRadioAudioActive(true);
  Serial.printf("[RADIO][CLAIM] start sid=%s peer=%s group=%s\n", g_call.session_id, g_call.peer_device_id,
                g_call.group_code);
  sendClaim();
  return StartCallResult::ACCEPTED;
}

// Publishes a RELEASE (if we currently hold the claim as caller) and tears
// the local call down. Factored out of stopPrivateCall() so Phase 5's
// Maintenance Mode can force-stop a call from either side (see
// setMaintenanceModeActive below) without duplicating the RELEASE-build
// logic.
void releaseAndResetPrivateCall() {
  if (g_call.isCaller && g_call.state != PrivateState::IDLE) {
    // Gate 1: a CLAIM still queued is published after this point; the
    // RELEASE below joins the same chain behind it (same connection, same
    // order). Its late result is recognized as an abandoned session's.
    uint32_t queuedClaim = g_call.claimQueuedTicket;
    MqttManager::ControlOutcome o = sendBusyPacket(g_call.group_code, g_call.peer_device_id, BUSY_RELEASE,
                                                   g_call.session_id, onControlPublishDone, nullptr);
    const char* owed = "-";
    if (o == MqttManager::ControlOutcome::FAILED && queuedClaim != 0) {
      // The chain is full: owed until this CLAIM's result frees a place.
      // Earlier sessions' owed RELEASEs are kept (one entry each).
      OwedRelease* r = owedReleaseCount() < kMaxOwedReleases ? new (std::nothrow) OwedRelease() : nullptr;
      if (r != nullptr) {
        r->claimTicket = queuedClaim;
        strncpy(r->group_code, g_call.group_code, sizeof(r->group_code) - 1);
        r->group_code[sizeof(r->group_code) - 1] = '\0';
        strncpy(r->peer_device_id, g_call.peer_device_id, sizeof(r->peer_device_id) - 1);
        r->peer_device_id[sizeof(r->peer_device_id) - 1] = '\0';
        strncpy(r->session_id, g_call.session_id, sizeof(r->session_id) - 1);
        r->session_id[sizeof(r->session_id) - 1] = '\0';
        r->next = g_owedRelease;
        g_owedRelease = r;
        owed = "owed";
      } else {
        // No memory (or the bound reached): this RELEASE is lost -- reported
        // as such; the receiver's busy TTL is then the only release.
        owed = "LOST(no-memory-or-limit)";
      }
    }
    if (o != MqttManager::ControlOutcome::PUBLISHED) {
      Serial.printf("[RADIO][CLAIM] RELEASE %s sid=%s claimStillQueued=%d owed=%s\n", outcomeName(o), g_call.session_id,
                    queuedClaim != 0 ? 1 : 0, owed);
    }
  }
  resetPrivateCall();
}

void stopPrivateCall() {
  g_lastFailure = PrivateFailure::NONE;  // the failed attempt's status ends with the PTT press
  // Callee side: our own PTT press never actually claims anything while a
  // call is active (startPrivateCall no-ops unless state == IDLE), so the
  // matching release must not tear down an incoming call we're receiving.
  if (!g_call.isCaller) return;
  if (g_call.state == PrivateState::IDLE && !g_call.denied) return;
  releaseAndResetPrivateCall();
}

PrivateState getPrivateState() { return g_call.state; }
PrivateFailure getPrivateFailure() { return g_lastFailure; }
bool isPrivateDenied() { return g_call.denied; }

void startBroadcastCall(const char* group_code) {
  if (g_maintenanceMode) return;  // Phase 5 OTA: no new Radio session may begin
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

void setMaintenanceModeActive(bool active) {
  g_maintenanceMode = active;
  if (active) {
    // Force-stop any in-progress call regardless of caller/callee side --
    // stopPrivateCall()'s own isCaller guard is deliberate for a normal
    // PTT-release, but Maintenance Mode needs "no active PTT session is
    // running" (Phase 5 section 18) unconditionally. releaseAndResetPrivateCall()
    // already covers both roles: it publishes RELEASE only when we hold the
    // claim as caller (using g_call.session_id captured before the reset,
    // so it can never carry a stale/wrong session), then always tears down
    // our own g_call via resetPrivateCall() -- which itself frees the
    // Radio pre-buffer, clears the jitter buffer, and stops capture --
    // regardless of whether we were the caller or the callee.
    releaseAndResetPrivateCall();
    stopBroadcastCall();
    // No microphone runs in Maintenance Mode, whichever mode opened it (a
    // Race Room PTT too): resetPrivateCall() no longer stops capture it does
    // not own, so maintenance does it explicitly, as it effectively did
    // before. OTA's own sleep reason keeps the device awake meanwhile.
    RadioAudio::stopCapture();
    setRadioAudioActive(false);

    // g_receiverBusy is separate from g_call: it tracks a claim we, as
    // receiver, GRANTED to some other caller. resetPrivateCall() above
    // never touches it, and there is no wire message for a granter to
    // withdraw a grant, so the only safe local action is to drop our own
    // "busy" bookkeeping immediately -- otherwise a stale grant could
    // incorrectly DENY a legitimate new caller for up to kBusyTtlMs after
    // Maintenance Mode ends and MQTT reconnects, instead of self-healing
    // only via serviceTick()'s passive TTL sweep.
    g_receiverBusy = ReceiverBusy{};
  }
}

bool isBroadcasting() { return g_broadcastActive; }

}  // namespace RadioTransport
