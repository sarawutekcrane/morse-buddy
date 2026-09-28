#include "core/mqtt_manager.h"

#include <Arduino.h>
#include <MQTT.h>
#include <WiFi.h>
#include <new>
#include <string.h>

#include "core/crc32.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/packet_codec.h"
#include "core/presence.h"
#include "core/settings.h"
#include "core/wifi_manager.h"

// NOTE: this file targets 256dpi/MQTT@2.5.2 (Addendum section 1.2) from
// memory of its public API (begin/setOptions/setWill/onMessageAdvanced/
// connect/publish/subscribe/loop/connected/disconnect). Verify against the
// actual installed library headers on first build; method names/signatures
// here are the most likely spot to need a small adjustment.

namespace MqttManager {

namespace {

struct GroupClient {
  bool active = false;
  char group_code[33] = {0};
  char client_id[24] = {0};
  WiFiClient net;
  MQTTClient* mqtt = nullptr;
  // Hardware Fix #4.9d Part A1: rate-limits reconnect attempts. Real
  // hardware testing found DOT/DASH, encoder rotation/push, and menu
  // navigation ALL delayed together (a global loop-starvation symptom, not
  // an individual input debounce defect) whenever a group was disconnected
  // -- the old serviceTick() called connectGroupIfNeeded() on every
  // disconnected group on EVERY tick, and MQTTClient::connect() is
  // synchronous, so a single unreachable broker blocked the whole Arduino/
  // main task (and therefore Input::popEvent()/Menu::tick()) for the
  // entire command timeout, repeatedly. 0 means "never attempted yet /
  // eligible immediately"; otherwise the millis() deadline before which no
  // further attempt is made.
  //
  // Hardware Fix #4.9f: Part A1's ORIGINAL implementation called
  // subscribeAll()/Presence::publishOnline() and THEN reset this back to
  // 0 unconditionally, on connect() alone having returned true -- it never
  // checked whether subscribeAll()/publishOnline() themselves actually
  // succeeded or left the connection dead. The defect was failing to
  // account for setup failure, not the ORDER of the reset: a connection
  // lost during that setup window (a real, observed failure mode:
  // subscribeAll() ignored every subscribe() result, and 256dpi/MQTT
  // v2.5.2 closes the connection on a subscribe error) still got the
  // reset-to-0 treatment, so the very next serviceTick() could retry
  // immediately, completely bypassing this cooldown.
  // connectGroupIfNeeded() now sets this to a fresh deadline after EVERY
  // actual attempt -- success or any failure stage -- never back to 0.
  // This field only ever gates connectGroupIfNeeded() itself; a client
  // that IS connected returns from that function before this is even
  // read, so normal mqtt->loop() servicing in serviceTick() is never
  // affected by it.
  uint32_t nextReconnectAttemptMs = 0;
};

GroupClient g_clients[Settings::kMaxGroups];
bool g_maintenanceMode = false;

// Hardware Fix #4.9d Part A1/A2, corrected by #4.9f: a disconnected group
// may retry at most once every 5000ms (rather than once per tick), and the
// MQTT-level command timeout used for a connect attempt's protocol
// exchange is bounded to 500ms instead of the old 5000ms. This does NOT
// bound the entire connect() call -- MQTTClient::connect() first performs
// the underlying TCP/DNS work via WiFiClient::connect(host, port), which
// this timeout parameter has no effect on, before any MQTT-level exchange
// even starts. Both the throttle AND the reduced command timeout are
// necessary together: throttling alone would still let one attempt block
// the UI task for however long that unbounded TCP/DNS phase takes, every
// time its turn came up.
constexpr uint32_t kMqttReconnectIntervalMs = 5000;
constexpr uint32_t kMqttCommandTimeoutMs = 500;

// Hardware Diagnostic #4.9g: TEMPORARY experiment only, not a final
// latency fix. Hardware evidence shows msg/<device_id> repeatedly failing
// to subscribe at 504-507ms with lastError=-9 (LWMQTT_MISSING_OR_WRONG_
// PACKET, returned when lwmqtt_subscribe()'s wait finishes without a
// SUBACK) while presence/+ succeeds -- this narrows the failure to "no
// SUBACK arrived for this one topic within kMqttCommandTimeoutMs (500ms)"
// but does NOT by itself establish WHY (broker-side delay, packet loss,
// or something else). This wider timeout is applied ONLY for the
// duration of one setup attempt (connect + subscribeAll + publishOnline)
// to see whether msg/<device_id> succeeds given more time to wait for its
// SUBACK -- it is restored to kMqttCommandTimeoutMs immediately after
// setup finishes, on every success/failure path, so healthy connected
// clients' normal loop()/publish() servicing is never affected. A
// success here would narrow the cause further (a slow-but-eventually-
// answering SUBACK) but would NOT itself prove the broker was at fault,
// and a longer timeout does not make the UI-blocking synchronous call
// non-blocking -- it can only make a single blocking attempt take longer.
constexpr uint32_t kMqttSetupTimeoutMsExperiment = 1500;

// Hardware Fix #4.9f: single source of truth for computing the next
// allowed reconnect deadline, used after every actual connection/setup
// attempt (see connectGroupIfNeeded()) regardless of where it stopped
// (connect failure, subscribe failure, publishOnline leaving the
// connection down, or full success). 0 is reserved as
// GroupClient::nextReconnectAttemptMs's "eligible immediately" sentinel
// (the pre-first-attempt default) -- on the vanishingly rare millis()
// rollover tick where now + kMqttReconnectIntervalMs would wrap to exactly
// 0, this nudges the deadline to 1 instead, so a just-finished attempt can
// never be silently reinterpreted as "never attempted yet".
uint32_t computeNextReconnectDeadline(uint32_t now) {
  uint32_t deadline = now + kMqttReconnectIntervalMs;
  return (deadline == 0) ? 1 : deadline;
}

// Hardware Fix #4.9d Part A3: round-robin cursor so that, across
// consecutive ticks, every disconnected group eventually gets its turn to
// attempt reconnecting -- a single persistently-broken group parked at a
// low array index can never starve the others by always winning the scan.
uint8_t g_nextReconnectCandidateIndex = 0;

GroupClient* findClientSlot(const char* group_code) {
  for (auto& c : g_clients) {
    if (c.active && strcmp(c.group_code, group_code) == 0) return &c;
  }
  return nullptr;
}

GroupClient* findClientByMqttPtr(MQTTClient* client) {
  for (auto& c : g_clients) {
    if (c.active && c.mqtt == client) return &c;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Bounded receive queue (Addendum section 3.11): the MQTT callback only
// copies and enqueues; all parsing/dispatch happens from the AppService
// tick, after client->loop() has returned.
// ---------------------------------------------------------------------------
constexpr uint8_t kQueueCapacity = 16;
constexpr uint16_t kMaxStoredPacketSize = 1024;

// `data` is a heap pointer sized to the exact incoming packet length (not a
// fixed 1024-byte array) — kMaxStoredPacketSize remains the cap on what a
// single packet may occupy, it just no longer pre-reserves that much .bss
// for every one of the 16 slots regardless of what's actually queued.
// Lifecycle: onMqttMessage() allocates+copies+takes ownership on enqueue;
// drainReceiveQueue() frees exactly once and nulls the pointer after a
// slot's payload has been fully processed, whatever path that took
// (malformed topic, presence, malformed packet, unknown kind, or a
// successful dispatch) — see processQueuedMessage()/drainReceiveQueue().
struct QueuedMessage {
  bool used = false;
  char group_code[33] = {0};
  char topic[64] = {0};
  uint16_t len = 0;
  uint8_t* data = nullptr;
};

QueuedMessage g_queue[kQueueCapacity];
uint8_t g_queueHead = 0;
uint8_t g_queueTail = 0;
uint8_t g_queueCount = 0;

void onMqttMessage(MQTTClient* client, char topic[], char bytes[], int length) {
  if (length < 0 || static_cast<size_t>(length) > kMaxStoredPacketSize) return;  // drop oversized
  if (g_queueCount >= kQueueCapacity) return;  // queue full; drop (QoS0 traffic may legitimately be dropped)

  GroupClient* gc = findClientByMqttPtr(client);
  if (gc == nullptr) return;

  // Allocate (sized exactly to this packet) and copy BEFORE touching the
  // queue slot or its indexes, so a failed allocation can't corrupt state
  // or overwrite another item — the slot at g_queueTail is only touched
  // once we know the payload copy has already succeeded.
  uint8_t* buf = nullptr;
  if (length > 0) {
    buf = new (std::nothrow) uint8_t[static_cast<size_t>(length)];
    if (buf == nullptr) {
      Serial.println("[mqtt] payload allocation failed; dropping incoming packet");
      return;  // drop safely: queue indexes/state untouched, nothing else to free
    }
    memcpy(buf, bytes, static_cast<size_t>(length));  // never keep a pointer into the callback's own buffer
  }
  // length == 0 is a legitimate empty payload (e.g. a zero-length retained
  // "clear" publish), not an allocation failure — buf stays null and is
  // queued as such; decodeHeader()/handleIncoming() both bounds-check len
  // before ever touching the pointer.

  QueuedMessage& q = g_queue[g_queueTail];
  delete[] q.data;  // defensive: should already be null in correct steady-state operation
  q.data = buf;
  q.used = true;
  strncpy(q.group_code, gc->group_code, sizeof(q.group_code) - 1);
  q.group_code[sizeof(q.group_code) - 1] = '\0';
  strncpy(q.topic, topic, sizeof(q.topic) - 1);
  q.topic[sizeof(q.topic) - 1] = '\0';
  q.len = static_cast<uint16_t>(length);

  g_queueTail = static_cast<uint8_t>((g_queueTail + 1) % kQueueCapacity);
  g_queueCount++;
}

// Topic is "morsebuddy/<group_code>/<suffix>"; returns a pointer to
// <suffix> within `fullTopic`, or nullptr if malformed.
const char* extractTopicSuffix(const char* fullTopic) {
  const char* p = strchr(fullTopic, '/');
  if (p == nullptr) return nullptr;
  p = strchr(p + 1, '/');
  if (p == nullptr) return nullptr;
  return p + 1;
}

// One dequeued item's worth of dispatch — factored out of drainReceiveQueue
// so every exit from this function (malformed topic, presence, malformed
// packet, unregistered kind, or a successful dispatch) returns to exactly
// one place that frees the slot's payload, guaranteeing it happens once
// regardless of which path was taken.
void processQueuedMessage(const QueuedMessage& q) {
  const char* suffix = extractTopicSuffix(q.topic);
  if (suffix == nullptr) return;

  if (strncmp(suffix, "presence/", 9) == 0) {
    Presence::handleIncoming(q.group_code, reinterpret_cast<const char*>(q.data), q.len);
    return;
  }

  PacketCodec::Header hdr;
  const uint8_t* payload = nullptr;
  if (!PacketCodec::decodeHeader(q.data, q.len, &hdr, &payload)) return;  // malformed: log+ignore

  NetworkPacketHandlerFn handler = getNetworkPacketHandler(hdr.packet_kind);
  if (handler != nullptr) handler(q.group_code, suffix, payload, hdr.payload_length);
  // else: unregistered kind (nothing has claimed it yet in this phase) — ignored safely.
}

void drainReceiveQueue() {
  // Hardware Diagnostic #4.9e Part B2 item 4: instrumentation only.
  uint32_t drainStart = millis();
  uint16_t drainedCount = 0;
  while (g_queueCount > 0) {
    QueuedMessage& q = g_queue[g_queueHead];
    g_queueHead = static_cast<uint8_t>((g_queueHead + 1) % kQueueCapacity);
    g_queueCount--;

    processQueuedMessage(q);
    drainedCount++;

    // Free exactly once, then reset the slot, no matter which path
    // processQueuedMessage took.
    delete[] q.data;
    q.data = nullptr;
    q.used = false;
    q.len = 0;
  }
  uint32_t drainElapsed = millis() - drainStart;
  if (drainElapsed >= 20) {
    Serial.printf("[PERF][MQTT] drain %lu ms count=%u\n", static_cast<unsigned long>(drainElapsed),
                  static_cast<unsigned>(drainedCount));
  }
}

// ---------------------------------------------------------------------------
// Connect / subscribe
// ---------------------------------------------------------------------------
void buildClientId(const char* group_code, char* outBuf, size_t outBufSize) {
  uint32_t crc = Crc32::computeStr(group_code);
  snprintf(outBuf, outBufSize, "%s_%08lX", Identity::deviceId(), static_cast<unsigned long>(crc));
}

// Hardware Fix #4.9f Part B: subscribeAll() used to ignore every
// subscribe() result and always report success by construction (a void
// return). Verified against real hardware logs and 256dpi/MQTT v2.5.2's
// own behavior (a subscribe error CLOSES the connection), a failed
// subscribe silently left later topics never subscribed AND the
// connection potentially already dead, while connectGroupIfNeeded() still
// went on to call Presence::publishOnline() and treat setup as fully
// successful. Now returns bool, checks each ACTUAL subscribe() result,
// and stops issuing further subscribes the moment one fails -- the 13
// topics, their exact order, and their existing QoS are unchanged; only
// whether a failure is noticed and stops the sequence is new.
//
// Hardware Diagnostic #4.9e Part B2, extended by #4.9f Part C: 256dpi/MQTT's
// subscribe() is SUSPECTED to wait synchronously for the broker's SUBACK
// within the configured command timeout, based on the library's general
// design and the hardware timing evidence gathered so far -- this has not
// yet been confirmed by an actual error/timeout captured on hardware via
// the diagnostics below. Per-subscribe timing (carrying the actual
// result, lastError(), and current connection state, snapshotted
// immediately after the call) pinpoints exactly which of the 13 topics (if
// any) is slow or failing, not just that subscribeAll() as a whole was.
bool subscribeAll(GroupClient& gc) {
  uint32_t subscribeAllStart = millis();
  const char* dev = Identity::deviceId();

  char msgSuffix[24];
  snprintf(msgSuffix, sizeof(msgSuffix), "msg/%s", dev);
  char busySuffix[24];
  snprintf(busySuffix, sizeof(busySuffix), "radio/busy/%s", dev);
  char busyReplySuffix[32];
  snprintf(busyReplySuffix, sizeof(busyReplySuffix), "radio/busy_reply/%s", dev);
  char audioSuffix[24];
  snprintf(audioSuffix, sizeof(audioSuffix), "radio/audio/%s", dev);

  struct TopicSpec {
    const char* suffix;
    int qos;
  };
  // Exact same 13 topics, same order, same QoS as before this fix.
  const TopicSpec kTopics[] = {
      {"presence/+", 1},   {msgSuffix, 1},       {"broadcast", 1},          {"race/invite", 1},
      {"race/join", 1},    {"race/round", 1},    {"race/solved", 1},        {"race/reset", 1},
      {busySuffix, 0},     {busyReplySuffix, 0}, {"radio/session", 0},      {audioSuffix, 0},
      {"radio/audio/broadcast", 0},
  };
  constexpr size_t kTopicCount = sizeof(kTopics) / sizeof(kTopics[0]);

  bool allOk = true;
  for (size_t i = 0; i < kTopicCount; i++) {
    char topic[80];
    snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", gc.group_code, kTopics[i].suffix);
    uint32_t t0 = millis();
    bool ok = gc.mqtt->subscribe(topic, kTopics[i].qos);
    uint32_t elapsed = millis() - t0;
    // Part C: log on failure OR duration >=20ms; lastError()/connected()
    // snapshotted immediately after the call, before anything else runs.
    if (!ok || elapsed >= 20) {
      Serial.printf("[PERF][MQTT] subscribe group=%s topic=%s %lu ms result=%d lastError=%d connected=%d\n",
                    gc.group_code, kTopics[i].suffix, static_cast<unsigned long>(elapsed), ok ? 1 : 0,
                    static_cast<int>(gc.mqtt->lastError()), gc.mqtt->connected() ? 1 : 0);
    }
    if (!ok) {
      allOk = false;
      break;  // Part B: stop at the first failure -- remaining topics are never subscribed this attempt.
    }
  }

  uint32_t subscribeAllElapsed = millis() - subscribeAllStart;
  if (subscribeAllElapsed >= 20) {
    Serial.printf("[PERF][MQTT] subscribeAll group=%s %lu ms result=%d\n", gc.group_code,
                  static_cast<unsigned long>(subscribeAllElapsed), allOk ? 1 : 0);
  }
  return allOk;
}

// Hardware Fix #4.9d Part A1: `now` is passed in (the single millis()
// snapshot serviceTick() already took for this whole call) rather than
// read again here, and a synchronous connect attempt is only actually
// made once every kMqttReconnectIntervalMs -- a disconnected/unreachable
// group can no longer consume the UI loop on every single tick.
//
// Hardware Fix #4.9f: that per-group throttle governs how often a NEW
// attempt may START, but it does not by itself guarantee every attempt
// finishes cleanly -- see gc.nextReconnectAttemptMs's own field comment
// and the cooldown logic at the bottom of this function for the specific
// gap (a connection lost partway through setup) real hardware evidence
// found and this fix closes.
void connectGroupIfNeeded(GroupClient& gc, uint32_t now) {
  if (gc.mqtt->connected()) return;
  // Rollover-safe "deadline not yet reached" check (same idiom already
  // used by sound_facade.cpp's isPlaying()) -- 0 always means due now.
  if (gc.nextReconnectAttemptMs != 0 && static_cast<int32_t>(now - gc.nextReconnectAttemptMs) < 0) return;

  char willTopic[48];
  snprintf(willTopic, sizeof(willTopic), "morsebuddy/%s/presence/%s", gc.group_code, Identity::deviceId());
  char willPayload[110];
  // Feature Fix #4.8 section 2I: append the same optional trailing color
  // index Presence::buildPayload() now sends on every ONLINE/OFFLINE
  // publish, so a peer that only ever sees this device via its LWT (an
  // unclean disconnect) still resolves its personal color the same way.
  // Format-compatible: still tag "MBP1", still OFFLINE/radio=0, just one
  // more optional field appended after the existing ones.
  snprintf(willPayload, sizeof(willPayload), "MBP1|%s|%s|OFFLINE|0|%lu|%u", Identity::deviceId(),
           Settings::getMyName(), static_cast<unsigned long>(WifiManager::getUnixTime()),
           Settings::getMyColorIndex());
  gc.mqtt->setWill(willTopic, willPayload, /*retained=*/true, /*qos=*/1);
  // Hardware Fix #4.9d Part A2: 5000ms was unacceptable to block the UI
  // task on -- bounded to kMqttCommandTimeoutMs (500ms) instead. Note:
  // MQTTClient::connect() itself remains fully synchronous underneath
  // (the underlying WiFiClient::connect(host, port) call it uses is not
  // bounded by this timeout parameter), which is exactly why the
  // kMqttReconnectIntervalMs throttle above is mandatory too, not
  // optional -- see this file's header comment / the final report for the
  // verified state of a bounded-connect WiFiClient override attempt.
  //
  // Hardware Diagnostic #4.9g: TEMPORARILY uses kMqttSetupTimeoutMsExperiment
  // (1500ms) instead of kMqttCommandTimeoutMs for this whole setup attempt
  // -- connect, subscribeAll, and publishOnline all share whatever timeout
  // MQTTClient was last given via setOptions()/setTimeout(), so this one
  // call covers all three stages. Restored to kMqttCommandTimeoutMs
  // immediately after setup finishes, below, on every success/failure
  // path -- a healthy connected client's normal loop()/publish() calls
  // after this function returns always use the original 500ms.
  gc.mqtt->setOptions(/*keepAlive=*/20, /*cleanSession=*/false, /*timeout=*/kMqttSetupTimeoutMsExperiment);

  // Hardware Diagnostic #4.9e Part B2 item 2, extended by #4.9f Part C: an
  // actual connect attempt is rate-limited to once per
  // kMqttReconnectIntervalMs per group (Part A1) -- this can never spam --
  // so it's printed unconditionally (not gated to >=20ms like the other
  // diagnostics here), because MQTTClient::connect() is the single
  // most-suspected blocking call for the reported multi-second stall, and
  // even a FAST result here is useful signal. lastError()/returnCode()
  // (a CONNECT result, not a SUBACK result) are snapshotted immediately
  // after the call.
  uint32_t connectStart = millis();
  bool connected = gc.mqtt->connect(gc.client_id, nullptr, nullptr);
  uint32_t connectElapsed = millis() - connectStart;
  Serial.printf("[PERF][MQTT] connect group=%s startMs=%lu %lu ms result=%d lastError=%d returnCode=%d\n",
                gc.group_code, static_cast<unsigned long>(connectStart),
                static_cast<unsigned long>(connectElapsed), connected ? 1 : 0,
                static_cast<int>(gc.mqtt->lastError()), static_cast<int>(gc.mqtt->returnCode()));

  // Hardware Fix #4.9f Part B: setup only counts as successful once EVERY
  // stage below has actually succeeded, in order -- connect, then all 13
  // subscribes, then Presence::publishOnline() with the connection still
  // up afterward. Any earlier stage failing (or the connection dropping
  // out from under a later one) stops setup right there; failStage
  // records exactly where, for the summary log below.
  bool setupOk = false;
  const char* failStage = "connect";

  if (connected) {
    failStage = "subscribe";
    // Part B: publishOnline() only runs once ALL subscriptions succeeded
    // AND the client is still connected (subscribeAll() itself can leave
    // the connection dead partway through -- 256dpi/MQTT v2.5.2 closes the
    // connection on a subscribe error).
    if (subscribeAll(gc) && gc.mqtt->connected()) {
      failStage = "publishOnline";
      Presence::publishOnline(gc.group_code);
      // Part B: check connection state again after publishOnline -- a
      // publish can also fail/close the connection.
      if (gc.mqtt->connected()) setupOk = true;
    }
  }

  // Hardware Diagnostic #4.9g: restore the original command timeout right
  // after setup finishes, on every success/failure path above -- this is
  // NOT gated on setupOk/failStage, so it always runs regardless of where
  // (or whether) setup stopped early. Every normal mqtt->loop()/publish()
  // call this client makes from here on (until its next disconnect-and-
  // reconnect-attempt cycle) uses kMqttCommandTimeoutMs again.
  gc.mqtt->setTimeout(kMqttCommandTimeoutMs);

  // Hardware Fix #4.9f Part A, corrected by #4.9f follow-up: after EVERY
  // actual attempt above -- full success or a failure at any stage --
  // establish a FRESH cooldown deadline from a FRESH millis() reading
  // taken HERE, not from `now` (the parameter captured once at the top of
  // serviceTick(), before any of this function's own connect/subscribe/
  // publish work ran). Using the stale `now` would undercount the actual
  // cooldown by however long that work took -- often hundreds of
  // milliseconds per the hardware timing evidence -- silently shrinking
  // the intended 5-second window. Never reset back to 0 on success: while
  // this client stays connected, gc.mqtt->connected() at the top of this
  // function always returns before this deadline is even read, so a
  // healthy connection's normal mqtt->loop() servicing is completely
  // unaffected by it; the deadline only takes effect the next time THIS
  // connection is lost, closing the exact gap the hardware evidence found
  // (a connection lost during setup could previously reconnect on the
  // very next tick, bypassing this cooldown entirely).
  gc.nextReconnectAttemptMs = computeNextReconnectDeadline(millis());

  // Part C, extended by #4.9g: one setup summary per actual attempt, now
  // also recording the experimental setup timeout used for this attempt
  // for traceability.
  Serial.printf("[PERF][MQTT] setup group=%s result=%d stage=%s connected=%d nextRetryMs=%lu setupTimeoutMs=%lu\n",
                gc.group_code, setupOk ? 1 : 0, setupOk ? "none" : failStage, gc.mqtt->connected() ? 1 : 0,
                static_cast<unsigned long>(gc.nextReconnectAttemptMs),
                static_cast<unsigned long>(kMqttSetupTimeoutMsExperiment));
}

GroupClient* findOrCreateClientSlot(const char* group_code) {
  GroupClient* existing = findClientSlot(group_code);
  if (existing != nullptr) return existing;

  for (auto& c : g_clients) {
    if (!c.active) {
      c.active = true;
      strncpy(c.group_code, group_code, sizeof(c.group_code) - 1);
      c.group_code[sizeof(c.group_code) - 1] = '\0';
      buildClientId(group_code, c.client_id, sizeof(c.client_id));
      c.mqtt = new MQTTClient(1024, 256);
      c.mqtt->begin(kBrokerHost, kBrokerPort, c.net);
      c.mqtt->onMessageAdvanced(onMqttMessage);
      return &c;
    }
  }
  return nullptr;  // all 5 slots active; should not happen (max 5 groups exist)
}

// Group deletion sequence (Addendum section 5): offline publish, then one
// reconnect with cleanSession=true to request broker-side session cleanup,
// then disconnect and free. Best-effort and bounded by each call's own
// connect timeout; run synchronously here since deletion is a rare,
// deliberate user action, not a per-frame concern.
void destroyClientForGroup(const char* group_code) {
  GroupClient* gc = findClientSlot(group_code);
  if (gc == nullptr) return;

  if (gc->mqtt->connected()) {
    Presence::publishOffline(group_code);
    char selfTopic[48];
    snprintf(selfTopic, sizeof(selfTopic), "morsebuddy/%s/presence/%s", group_code, Identity::deviceId());
    gc->mqtt->publish(selfTopic, "", /*retained=*/true, /*qos=*/1);  // zero-length retained: remove our record
    gc->mqtt->disconnect();
  }

  gc->mqtt->setOptions(20, /*cleanSession=*/true, 5000);
  if (gc->mqtt->connect(gc->client_id, nullptr, nullptr)) {
    gc->mqtt->disconnect();
  }

  delete gc->mqtt;
  gc->mqtt = nullptr;
  gc->active = false;
  memset(gc->group_code, 0, sizeof(gc->group_code));
}

void onSettingsChanged(const SettingsChangeInfo& info) {
  switch (info.event) {
    case SET_GROUP_ADDED: {
      uint8_t n = Settings::getGroupCount();
      if (info.index < n) {
        Settings::FamilyGroup g = Settings::getGroup(info.index);
        findOrCreateClientSlot(g.code);  // connects once WiFi/tick picks it up
      }
      break;
    }
    case SET_GROUP_DELETED:
      destroyClientForGroup(info.group_code);
      break;
    case SET_MY_NAME_CHANGED:
      Presence::republishOwnPresenceAllGroups();
      break;
    default:
      break;
  }
}

// Bounded best-effort OFFLINE publish across every connected group. Shared
// by the existing BeforeSleep hook and Phase 5 OTA Maintenance Mode entry
// (section 19: "best-effort publish OFFLINE Presence for all groups; do
// not block OTA indefinitely if Presence publish fails").
void publishOfflineAllGroupsBounded() {
  uint32_t start = millis();
  for (auto& gc : g_clients) {
    if (millis() - start >= 2000) break;
    if (gc.active && gc.mqtt != nullptr && gc.mqtt->connected()) {
      Presence::publishOffline(gc.group_code);
    }
  }
}

void onBeforeSleep() { publishOfflineAllGroupsBounded(); }

uint8_t connectivityStatusProvider() {
  if (!WifiManager::isConnected()) return CONN_OFFLINE;
  for (auto& gc : g_clients) {
    if (gc.active && gc.mqtt != nullptr && gc.mqtt->connected()) return CONN_ONLINE;
  }
  return CONN_WIFI_ONLY;
}

void serviceInit() {
  uint8_t n = Settings::getGroupCount();
  for (uint8_t i = 0; i < n; i++) {
    Settings::FamilyGroup g = Settings::getGroup(i);
    findOrCreateClientSlot(g.code);
  }
  registerSettingsChangeHook(onSettingsChanged);
  registerBeforeSleepHook(onBeforeSleep);
  registerConnectivityStatusProvider(connectivityStatusProvider);
}

void serviceTick() {
  if (g_maintenanceMode) return;  // Phase 5 OTA: reconnect/receive paused
  if (!WifiManager::isConnected()) return;  // never block; just wait until WiFi is up

  uint32_t now = millis();

  // mqtt->loop() is the library's own per-connection servicing (keepalive/
  // incoming-message pump). Hardware Diagnostic #4.9e Part B2 item 1 /
  // Part C, reaffirmed by #4.9f: do NOT assume this is always
  // non-blocking, and do NOT assume every reconnect path is already fully
  // throttled just because Part A1 rate-limits connectGroupIfNeeded() --
  // 256dpi/MQTT uses synchronous lwmqtt operations internally, so a
  // CONNECTED client can still momentarily wait on network data within
  // loop(), and #4.9f found a real gap where a connection lost during
  // setup could bypass the cooldown entirely (see connectGroupIfNeeded()).
  // Timed individually so a single slow group is visible, not just the
  // whole for-loop; an already-disconnected client that returns fast never
  // prints here (Part B2's "do not print for every disconnected client on
  // every tick"), but one that WAS connected and comes back from loop()
  // either erroring or no longer connected always does, regardless of
  // duration, since that's a real connectivity event worth seeing.
  for (auto& gc : g_clients) {
    if (!gc.active || gc.mqtt == nullptr) continue;
    bool connectedBefore = gc.mqtt->connected();
    uint32_t loopStart = millis();
    bool loopOk = gc.mqtt->loop();
    uint32_t loopElapsed = millis() - loopStart;
    bool connectedAfter = gc.mqtt->connected();

    if (connectedBefore && (!loopOk || !connectedAfter)) {
      Serial.printf(
          "[PERF][MQTT] loop-error group=%s %lu ms loopResult=%d connectedBefore=1 connectedAfter=%d lastError=%d\n",
          gc.group_code, static_cast<unsigned long>(loopElapsed), loopOk ? 1 : 0, connectedAfter ? 1 : 0,
          static_cast<int>(gc.mqtt->lastError()));
    } else if (loopElapsed >= 20) {
      Serial.printf("[PERF][MQTT] loop group=%s %lu ms connected=%d\n", gc.group_code,
                    static_cast<unsigned long>(loopElapsed), connectedAfter ? 1 : 0);
    }
  }

  // Hardware Fix #4.9d Part A3: the potentially-blocking reconnect
  // attempt itself is limited to AT MOST ONE group per tick, chosen
  // round-robin starting from g_nextReconnectCandidateIndex, so a single
  // broken/unreachable group parked at a low array index can never starve
  // the others' turns. connectGroupIfNeeded() still no-ops internally if
  // that group isn't actually due yet (Part A1's throttle), so this loop
  // just finds the next disconnected candidate to OFFER a turn to.
  for (uint8_t attempts = 0; attempts < Settings::kMaxGroups; attempts++) {
    uint8_t idx = g_nextReconnectCandidateIndex;
    g_nextReconnectCandidateIndex = static_cast<uint8_t>((g_nextReconnectCandidateIndex + 1) % Settings::kMaxGroups);
    GroupClient& gc = g_clients[idx];
    if (gc.active && gc.mqtt != nullptr && !gc.mqtt->connected()) {
      connectGroupIfNeeded(gc, now);
      break;
    }
  }

  drainReceiveQueue();
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = serviceTick;
    registerAppService(svc);
  }
};
Registrar g_registrar;

}  // namespace

bool isGroupConnected(const char* group_code) {
  GroupClient* gc = findClientSlot(group_code);
  return gc != nullptr && gc->mqtt != nullptr && gc->mqtt->connected();
}

bool isAnyGroupConnected() {
  for (auto& gc : g_clients) {
    if (gc.active && gc.mqtt != nullptr && gc.mqtt->connected()) return true;
  }
  return false;
}

void setMaintenanceModeActive(bool active) {
  if (active == g_maintenanceMode) return;
  if (active) {
    // Set the flag first so serviceTick() stops reconnecting/re-queuing
    // for the rest of this call, then drain what's already queued
    // ourselves -- serviceTick() (the only other caller of
    // drainReceiveQueue()) is now gated behind this same flag and would
    // otherwise never run again for the whole OTA HTTPS/TLS operation,
    // leaving every already-queued payload's heap allocation parked for
    // that entire duration. onMqttMessage() only ever enqueues (parsing/
    // dispatch happens exclusively in drainReceiveQueue()), so everything
    // already sitting in g_queue[] was fully received before this call
    // and is safe to process synchronously right here, exactly like a
    // normal serviceTick() would -- this preserves existing message
    // persistence/dedup behavior instead of discarding queued messages.
    g_maintenanceMode = true;
    drainReceiveQueue();

    // Defensive verification, not expected to ever fire: drainReceiveQueue()
    // unconditionally loops until g_queueCount reaches 0, and nothing on
    // that synchronous path can re-enter onMqttMessage() to re-queue
    // something (that only ever happens from inside gc.mqtt->loop(),
    // which serviceTick() -- now paused -- is the only caller of). Kept
    // as an explicit, leak-proof fallback rather than trusting that
    // invariant silently: if it ever fires, every dynamic payload still
    // marked used is freed exactly once here rather than left allocated
    // for the duration of OTA.
    if (g_queueCount != 0) {
      Serial.printf("[mqtt] maintenance mode: %u receive-queue payload(s) left after drain; forcing release\n",
                   static_cast<unsigned>(g_queueCount));
      for (auto& q : g_queue) {
        if (q.used) {
          delete[] q.data;
          q.data = nullptr;
          q.used = false;
          q.len = 0;
        }
      }
      g_queueHead = 0;
      g_queueTail = 0;
      g_queueCount = 0;
    }

    publishOfflineAllGroupsBounded();
    for (auto& gc : g_clients) {
      if (gc.active && gc.mqtt != nullptr && gc.mqtt->connected()) gc.mqtt->disconnect();
    }
  } else {
    g_maintenanceMode = false;  // un-pauses serviceTick(), which reconnects normally
  }
}

bool publishRaw(const char* group_code, const char* topic_suffix, const char* payload, bool retained, int qos) {
  GroupClient* gc = findClientSlot(group_code);
  if (gc == nullptr || gc->mqtt == nullptr || !gc->mqtt->connected()) return false;
  char topic[80];
  snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", group_code, topic_suffix);
  return gc->mqtt->publish(topic, payload, retained, qos);
}

// Hardware Diagnostic #4.9s: QoS1 publishes only, timed around the actual
// publish() call -- QoS0 (radio audio) keeps its original direct path,
// with no timing/error-state reads/logging, to avoid per-audio-packet
// overhead. result/lastError()/connected() are snapshotted in that exact
// order, immediately after publish() returns and before any
// Serial.printf(); connectedBefore=1 is a literal, justified by the
// connected() guard above -- no extra connected() call is added just to
// log it. returnCode() is deliberately NOT read here: it is
// connection-related, not the current publish's own error. Logged only
// on failure or when the call took >=20ms.
bool publishBinary(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                   bool retained, int qos) {
  GroupClient* gc = findClientSlot(group_code);
  if (gc == nullptr || gc->mqtt == nullptr || !gc->mqtt->connected()) return false;
  char topic[80];
  snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", group_code, topic_suffix);
  if (qos == 1) {
    uint32_t publishStart = millis();
    bool result = gc->mqtt->publish(topic, reinterpret_cast<const char*>(data), static_cast<int>(len), retained, qos);
    uint32_t publishElapsed = millis() - publishStart;
    int lastErr = static_cast<int>(gc->mqtt->lastError());
    bool connectedAfter = gc->mqtt->connected();
    if (!result || publishElapsed >= 20) {
      Serial.printf(
          "[PERF][MQTT] publish group=%s qos=%d retained=%d bytes=%u %lu ms result=%d lastError=%d "
          "connectedBefore=1 connectedAfter=%d\n",
          group_code, qos, retained ? 1 : 0, static_cast<unsigned>(len),
          static_cast<unsigned long>(publishElapsed), result ? 1 : 0, lastErr, connectedAfter ? 1 : 0);
    }
    return result;
  }
  return gc->mqtt->publish(topic, reinterpret_cast<const char*>(data), static_cast<int>(len), retained, qos);
}

}  // namespace MqttManager
