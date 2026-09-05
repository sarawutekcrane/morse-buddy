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

bool isGroupConnected(const char* group_code);
bool isAnyGroupConnected();

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
