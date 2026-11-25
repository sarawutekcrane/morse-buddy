#pragma once
// One persistent MQTT connection per Family Group (Addendum sections 1.3,
// 1.4, 6; Phase 2 sections 2, 4, 5). Registers itself as a Background App
// Service and reacts to Phase 1's SettingsChange/BeforeSleep hooks —
// main.cpp and settings.cpp need no changes.
//
// MQTT fix: each connection is set up (DNS, TCP, CONNECT/CONNACK, the 10
// subscriptions, the ONLINE presence publish) and each deleted group's
// broker session is cleaned up by a connection worker task, never on the
// main loop; a ready connection is then handed to the main task, which
// alone uses it (loop(), publishes) from then on. See mqtt_manager.cpp.

#include <stddef.h>
#include <stdint.h>

namespace MqttManager {

constexpr const char* kBrokerHost = "broker.hivemq.com";
constexpr uint16_t kBrokerPort = 1883;

// TRANSPORT-ONLY meaning, unchanged: true while this group's connection is
// up and usable by the main task. Correct for gating a bare outbound publish
// that has no dependency on this device's OWN subscriptions (chat/game/race
// sends, Outbox flush) -- publishRaw()/publishBinary() only ever need the
// wire to be up. NOT sufficient for an action that depends on RECEIVING a
// reply on one of this device's own subscribed topics -- see isGroupReady().
// Timing (MQTT fix): the main task only sees a connection once the worker
// has finished setting it up, so this turns true together with
// isGroupReady() rather than a few setup steps earlier.
bool isGroupConnected(const char* group_code);
bool isAnyGroupConnected();

// Fix Phase 1C, additive: STRICTLY STRONGER and separate from
// isGroupConnected() above -- true only once transport is connected AND
// all 10 subscriptions succeeded AND Presence::publishOnline() itself
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
// disconnects every group's MQTT client, cancels any connection setup in
// progress on the worker, and pauses reconnect/receive processing. On exit:
// reconnects each group exactly like a Wi-Fi recovery.
void setMaintenanceModeActive(bool active);

// topic_suffix is relative to "morsebuddy/<group_code>/", e.g.
// "presence/AABBCCDDEEFF", "msg/AABBCCDDEEFF", "broadcast". Returns false
// if this group has no connected client right now (caller's job to fall
// back to the Outbox).
bool publishRaw(const char* group_code, const char* topic_suffix, const char* payload, bool retained, int qos);
bool publishBinary(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                   bool retained, int qos);

// Asynchronous publish (MQTT fix, hardware device A: "[PERF][MQTT] publish
// ... qos=1 ... 605 ms" inside a chat send; a Race heartbeat's 1506 ms PUBACK
// timeout). The group's connection is lent to the connection worker, which
// publishes (worker timeouts: PUBACK 5 s) so the wait never blocks the main
// loop. Publishes started while it is lent join the same loan and go out in
// order, at most kMaxLentPublishes per group; the connection comes back with
// the last result. `done(ticket, ok)`, if not null, is called exactly once,
// on the main task, from MqttManager's service tick -- never from inside
// this call (ok: the publish succeeded; for QoS1 a PUBACK was received --
// the same evidence publishBinary() gives).
// Returns the ticket (non-zero) once queued; 0 when it could not be queued
// (group not active, no connection, the loan full or canceled, the worker
// queue full, no memory, OTA maintenance) -- nothing is published then and
// the caller does what it does after a failed publish (nothing waits).
// While the connection is lent the group still reads as connected/ready;
// publishBinary() for it returns false at once for QoS1, while publishRaw()
// and a QoS0 publishBinary() join the loan (true then means queued, not
// published -- radio control packets use publishControl(), which tells the
// two apart).
using PublishDoneFn = void (*)(uint32_t ticket, bool ok);
uint32_t publishBinaryAsync(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                            bool retained, int qos, PublishDoneFn done);

// Radio control packets (CLAIM/RELEASE, GRANT/DENY, SESSION_START/ACK; QoS0,
// not retained), with the outcome spelled out (Gate 1: a CLAIM queued behind
// a Race heartbeat's PUBACK wait is not a CLAIM sent):
//  - PUBLISHED: written now on the main task (the group's own connection).
//  - QUEUED: the connection is lent to the worker; the packet joined the
//    loan behind the publishes already queued, in order. *ticketOut gets the
//    ticket and `done(ticket, ok)` reports the actual result later, once, on
//    the main task (ok: written to the connection).
//  - FAILED: neither (no connection, the loan full, no memory, ...).
enum class ControlOutcome : uint8_t { FAILED, PUBLISHED, QUEUED };
ControlOutcome publishControl(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                              PublishDoneFn done, uint32_t* ticketOut);

// Radio audio frames (QoS0, not retained): published only if it costs no
// waiting -- the group's own connection, not lent to the worker, its socket
// writable right now. Otherwise the frame is skipped (false; counted in the
// "[DIAG][MQTT] qos0 ... skipped=" line) instead of holding the main loop
// for TCP buffer space. Never queues, so a late frame is never sent. The
// write itself (header + payload) is bounded to 50 ms in all: past that the
// frame is cut, and the library closes the connection (the group then
// reconnects) rather than holding the main loop for up to 500 ms (Gate 2).
bool publishBinaryNoWait(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len);

}  // namespace MqttManager
