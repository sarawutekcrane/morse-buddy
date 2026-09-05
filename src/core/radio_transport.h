#pragma once
// Radio networking (Addendum section 14; Phase 4 sections 4-8). Owns:
//  - Private Channel Busy (claim/grant/deny/TTL), receiver-authoritative
//  - Private call transport state machine (STUN -> UDP direct -> MQTT
//    fallback), with a 3-packet jitter buffer on the UDP path
//  - Everyone Radio (MQTT broadcast only, no claim/STUN)
// Registers its own packet handlers (PK_RADIO_BUSY, PK_RADIO_BUSY_REPLY,
// PK_RADIO_SESSION, PK_RADIO_AUDIO) through Phase 2's Network Packet
// Handler Registry and its own AppService tick — radio.cpp (UI) and
// race.cpp (Room voice) are the only callers.

#include <stddef.h>
#include <stdint.h>

namespace RadioTransport {

enum class PrivateState : uint8_t { IDLE, CLAIMING, NEGOTIATING, UDP_DIRECT, MQTT_FALLBACK, STOPPING };

// ---- Shared MQTT-fallback / broadcast / room audio envelope (PK_RADIO_AUDIO
// has exactly one registerNetworkPacketHandler slot, same constraint as
// PK_MESSAGE in Phase 2/3; this module owns that slot and fans out by scope
// so race.cpp's Room voice doesn't need a second, refused registration) ----
enum AudioScope : uint8_t { AUDIO_PRIVATE = 0, AUDIO_BROADCAST = 1, AUDIO_RACE_ROOM = 2 };

// roomKey is the invite_id for AUDIO_RACE_ROOM, ignored (nullptr) otherwise.
using AudioScopeHandlerFn = void (*)(const char* group_code, const char* sender_device_id, const char* roomKey,
                                     uint16_t sequence, const int16_t* samples, size_t sampleCount);

// One handler per scope; returns false if that scope is already claimed.
bool registerAudioScopeHandler(AudioScope scope, AudioScopeHandlerFn fn);

// Wraps `samples` in the shared envelope and publishes it as PK_RADIO_AUDIO
// (QoS0, <700 bytes) to `topic_suffix` within `group_code`.
bool publishAudioPacket(const char* group_code, const char* topic_suffix, AudioScope scope, const char* roomKey,
                        uint16_t sequence, const int16_t* samples, size_t sampleCount);

// Which foreground screen currently governs Mute-On background playback
// (Phase 4 section 9: "Mute On: play only Mode 3 / Race Room screen").
enum class UiContext : uint8_t { NONE, RADIO_TALK, RACE_ROOM };
void setUiContext(UiContext ctx);

// Phase 5 OTA Maintenance Mode (section 19). While active: blocks any new
// Private/Broadcast call from starting, force-stops one already in
// progress (regardless of caller/callee side), and drops incoming
// PK_RADIO_AUDIO frames instead of playing them. Race Room voice is
// unaffected directly -- it is screen-lifecycle-bound in race.cpp and
// cannot be running concurrently with the Firmware Update screen -- but
// its incoming audio is still dropped by the same PK_RADIO_AUDIO guard.
void setMaintenanceModeActive(bool active);

// ---- Private (one recipient) ------------------------------------------------
// Starts a PTT burst: claims the channel, and on GRANT negotiates transport
// and starts streaming captured mic audio. No-op if already active.
void startPrivateCall(const char* group_code, const char* recipient_device_id);

// PTT released: publishes RELEASE (if we hold the claim), stops capture and
// tears the session down locally, regardless of state.
void stopPrivateCall();

PrivateState getPrivateState();

// True immediately after a DENY while PTT is still held (Talk screen shows
// the no-mic icon for exactly this window per Phase 4 section 2).
bool isPrivateDenied();

// ---- Everyone (group broadcast) --------------------------------------------
void startBroadcastCall(const char* group_code);
void stopBroadcastCall();
bool isBroadcasting();

}  // namespace RadioTransport
