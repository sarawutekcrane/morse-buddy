#pragma once
// One persistent MQTT connection per Family Group (Addendum sections 1.3,
// 1.4, 6; Phase 2 sections 2, 4, 5). Registers itself as a Background App
// Service and reacts to Phase 1's SettingsChange/BeforeSleep hooks —
// main.cpp and settings.cpp need no changes.

#include <stddef.h>
#include <stdint.h>

namespace MqttManager {

constexpr const char* kBrokerHost = "broker.hivemq.com";
constexpr uint16_t kBrokerPort = 1883;

// TRANSPORT-ONLY meaning, UNCHANGED by the Phase 1C staged-setup candidate:
// true the instant the underlying MQTT connection is up, which can be
// BEFORE this device's own 13 subscriptions or its ONLINE presence publish
// have necessarily completed. Correct for gating a bare outbound publish
// that has no dependency on this device's OWN subscriptions (chat/game/
// race sends, Outbox flush, a retained-clear attempt) -- publishRaw()/
// publishBinary() only ever need the wire to be up. NOT sufficient for an
// action that depends on RECEIVING a reply on one of this device's own
// subscribed topics -- see isGroupReady() below for that narrower need.
bool isGroupConnected(const char* group_code);
bool isAnyGroupConnected();

// Fix Phase 1C, additive: STRICTLY STRONGER and separate from
// isGroupConnected() above -- true only once transport is connected AND
// all 13 subscriptions succeeded AND Presence::publishOnline() itself
// returned true with the connection still live afterward. A new readiness
// query existing does not by itself make every caller safe: use this only
// where the action genuinely depends on this device's own subscriptions
// already being live (see mqtt_manager.cpp's own definition for the exact,
// audited list of callers that do and do not need it).
//
// Fix Phase 1C (round 3): also checks WifiManager::isConnected() first
// (read-only, non-mutating) so this cannot answer true from a stale
// internal ready flag during a WiFi outage window before the next
// reconciling serviceTick() runs. isGroupConnected() above intentionally
// gets no such check -- its transport-only contract stays exactly as
// documented above.
bool isGroupReady(const char* group_code);
bool isAnyGroupReady();

// Phase 5 OTA Maintenance Mode (section 19). On entry: best-effort
// publishes OFFLINE presence for every group (bounded, same as the
// existing BeforeSleep hook -- never blocks OTA if publish fails), then
// disconnects every group's MQTT client and pauses reconnect/receive
// processing. On exit: simply stops suppressing serviceTick(), which
// reconnects each group exactly like a normal WiFi-drop recovery.
void setMaintenanceModeActive(bool active);

// topic_suffix is relative to "morsebuddy/<group_code>/", e.g.
// "presence/AABBCCDDEEFF", "msg/AABBCCDDEEFF", "broadcast". Returns false
// if this group has no connected client right now (caller's job to fall
// back to the Outbox).
bool publishRaw(const char* group_code, const char* topic_suffix, const char* payload, bool retained, int qos);
bool publishBinary(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                   bool retained, int qos);

}  // namespace MqttManager
