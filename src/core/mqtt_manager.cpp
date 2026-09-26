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
  uint32_t nextReconnectAttemptMs = 0;
};

GroupClient g_clients[Settings::kMaxGroups];
bool g_maintenanceMode = false;

// Hardware Fix #4.9d Part A1/A2: a disconnected group may retry at most
// once every 5000ms (rather than once per tick), and any single connect
// attempt is now bounded to 500ms instead of the old 5000ms -- both
// necessary together, since MQTTClient::connect() is still fully
// synchronous: throttling alone would still let one attempt block the UI
// task for up to the old 5-second timeout when its turn came up.
constexpr uint32_t kMqttReconnectIntervalMs = 5000;
constexpr uint32_t kMqttCommandTimeoutMs = 500;

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
  while (g_queueCount > 0) {
    QueuedMessage& q = g_queue[g_queueHead];
    g_queueHead = static_cast<uint8_t>((g_queueHead + 1) % kQueueCapacity);
    g_queueCount--;

    processQueuedMessage(q);

    // Free exactly once, then reset the slot, no matter which path
    // processQueuedMessage took.
    delete[] q.data;
    q.data = nullptr;
    q.used = false;
    q.len = 0;
  }
}

// ---------------------------------------------------------------------------
// Connect / subscribe
// ---------------------------------------------------------------------------
void buildClientId(const char* group_code, char* outBuf, size_t outBufSize) {
  uint32_t crc = Crc32::computeStr(group_code);
  snprintf(outBuf, outBufSize, "%s_%08lX", Identity::deviceId(), static_cast<unsigned long>(crc));
}

void subscribeAll(GroupClient& gc) {
  const char* dev = Identity::deviceId();
  auto sub = [&](const char* suffix, int qos) {
    char topic[80];
    snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", gc.group_code, suffix);
    gc.mqtt->subscribe(topic, qos);
  };

  char msgSuffix[24];
  snprintf(msgSuffix, sizeof(msgSuffix), "msg/%s", dev);
  char busySuffix[24];
  snprintf(busySuffix, sizeof(busySuffix), "radio/busy/%s", dev);
  char busyReplySuffix[32];
  snprintf(busyReplySuffix, sizeof(busyReplySuffix), "radio/busy_reply/%s", dev);
  char audioSuffix[24];
  snprintf(audioSuffix, sizeof(audioSuffix), "radio/audio/%s", dev);

  sub("presence/+", 1);
  sub(msgSuffix, 1);
  sub("broadcast", 1);
  sub("race/invite", 1);
  sub("race/join", 1);
  sub("race/round", 1);
  sub("race/solved", 1);
  sub("race/reset", 1);
  sub(busySuffix, 0);
  sub(busyReplySuffix, 0);
  sub("radio/session", 0);
  sub(audioSuffix, 0);
  sub("radio/audio/broadcast", 0);
}

// Hardware Fix #4.9d Part A1: `now` is passed in (the single millis()
// snapshot serviceTick() already took for this whole call) rather than
// read again here, and a synchronous connect attempt is only actually
// made once every kMqttReconnectIntervalMs -- a disconnected/unreachable
// group can no longer consume the UI loop on every single tick.
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
  gc.mqtt->setOptions(/*keepAlive=*/20, /*cleanSession=*/false, /*timeout=*/kMqttCommandTimeoutMs);

  if (gc.mqtt->connect(gc.client_id, nullptr, nullptr)) {
    subscribeAll(gc);
    Presence::publishOnline(gc.group_code);
    gc.nextReconnectAttemptMs = 0;  // connected -- no throttle needed until it drops again
  } else {
    gc.nextReconnectAttemptMs = now + kMqttReconnectIntervalMs;
  }
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

  // mqtt->loop() is the library's own lightweight per-connection servicing
  // (keepalive/incoming-message pump) -- cheap and non-blocking regardless
  // of connection state, so every active client still gets it every tick.
  for (auto& gc : g_clients) {
    if (gc.active && gc.mqtt != nullptr) gc.mqtt->loop();
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

bool publishBinary(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                   bool retained, int qos) {
  GroupClient* gc = findClientSlot(group_code);
  if (gc == nullptr || gc->mqtt == nullptr || !gc->mqtt->connected()) return false;
  char topic[80];
  snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", group_code, topic_suffix);
  return gc->mqtt->publish(topic, reinterpret_cast<const char*>(data), static_cast<int>(len), retained, qos);
}

}  // namespace MqttManager
