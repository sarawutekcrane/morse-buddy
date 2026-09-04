#include "core/mqtt_manager.h"

#include <Arduino.h>
#include <MQTT.h>
#include <WiFi.h>
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
};

GroupClient g_clients[Settings::kMaxGroups];

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

struct QueuedMessage {
  bool used = false;
  char group_code[33] = {0};
  char topic[64] = {0};
  uint16_t len = 0;
  uint8_t data[kMaxStoredPacketSize];
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

  QueuedMessage& q = g_queue[g_queueTail];
  q.used = true;
  strncpy(q.group_code, gc->group_code, sizeof(q.group_code) - 1);
  q.group_code[sizeof(q.group_code) - 1] = '\0';
  strncpy(q.topic, topic, sizeof(q.topic) - 1);
  q.topic[sizeof(q.topic) - 1] = '\0';
  q.len = static_cast<uint16_t>(length);
  memcpy(q.data, bytes, q.len);  // never keep a pointer into the callback's own buffer

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

void drainReceiveQueue() {
  while (g_queueCount > 0) {
    QueuedMessage& q = g_queue[g_queueHead];
    g_queueHead = static_cast<uint8_t>((g_queueHead + 1) % kQueueCapacity);
    g_queueCount--;

    const char* suffix = extractTopicSuffix(q.topic);
    if (suffix == nullptr) continue;

    if (strncmp(suffix, "presence/", 9) == 0) {
      Presence::handleIncoming(q.group_code, reinterpret_cast<const char*>(q.data), q.len);
      continue;
    }

    PacketCodec::Header hdr;
    const uint8_t* payload = nullptr;
    if (!PacketCodec::decodeHeader(q.data, q.len, &hdr, &payload)) continue;  // malformed: log+ignore

    NetworkPacketHandlerFn handler = getNetworkPacketHandler(hdr.packet_kind);
    if (handler != nullptr) handler(q.group_code, suffix, payload, hdr.payload_length);
    // else: unregistered kind (nothing has claimed it yet in this phase) — ignored safely.
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

void connectGroupIfNeeded(GroupClient& gc) {
  if (gc.mqtt->connected()) return;

  char willTopic[48];
  snprintf(willTopic, sizeof(willTopic), "morsebuddy/%s/presence/%s", gc.group_code, Identity::deviceId());
  char willPayload[110];
  snprintf(willPayload, sizeof(willPayload), "MBP1|%s|%s|OFFLINE|0|%lu", Identity::deviceId(),
           Settings::getMyName(), static_cast<unsigned long>(WifiManager::getUnixTime()));
  gc.mqtt->setWill(willTopic, willPayload, /*retained=*/true, /*qos=*/1);
  gc.mqtt->setOptions(/*keepAlive=*/20, /*cleanSession=*/false, /*timeout=*/5000);

  if (gc.mqtt->connect(gc.client_id, nullptr, nullptr)) {
    subscribeAll(gc);
    Presence::publishOnline(gc.group_code);
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

void onBeforeSleep() {
  uint32_t start = millis();
  for (auto& gc : g_clients) {
    if (millis() - start >= 2000) break;
    if (gc.active && gc.mqtt != nullptr && gc.mqtt->connected()) {
      Presence::publishOffline(gc.group_code);
    }
  }
}

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
  if (!WifiManager::isConnected()) return;  // never block; just wait until WiFi is up
  for (auto& gc : g_clients) {
    if (!gc.active || gc.mqtt == nullptr) continue;
    gc.mqtt->loop();
    if (!gc.mqtt->connected()) connectGroupIfNeeded(gc);
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
