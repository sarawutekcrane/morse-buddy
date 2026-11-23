#include "core/mqtt_manager.h"

#include <Arduino.h>
#include <MQTT.h>
#include <WiFi.h>
#include <errno.h>
#include <new>
#include <string.h>

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"

#include "core/crc32.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/packet_codec.h"
#include "core/presence.h"
#include "core/settings.h"
#include "core/wifi_manager.h"

// Targets 256dpi/MQTT@2.5.2 (MQTTClient.cpp/lwmqtt read for this design):
//  - connect() resolves and opens the TCP connection itself (via the
//    Client's connect(host, port): on ESP32 that is WiFiGenericClass::
//    hostByName(), up to 15 s, then a TCP connect with a 3000 ms default
//    timeout) and then waits up to `timeout` for the CONNACK.
//    connect(id, user, pass, /*skip=*/true) uses an already-open Client
//    instead.
//  - subscribe() and a QoS1 publish() wait up to `timeout` for their
//    SUBACK/PUBACK; any error closes the connection (Client::stop()).
//  - loop() reads only bytes already available (waiting up to `timeout`
//    for the rest of a partly received packet) and sends PINGREQ when due
//    without waiting for the PINGRESP.
//  - One MQTTClient and its Client must never be used by two tasks at once.
//
// Ownership (connection worker, MQTT fix):
//  - Every connection is a heap Conn (socket + MQTTClient). Exactly one task
//    owns a Conn at any time and only that task touches it.
//  - The connection worker (one FreeRTOS task) owns a Conn from creation
//    through DNS, TCP connect, CONNECT/CONNACK, the 10 subscriptions and the
//    ONLINE publish, then hands it to the main task through a queue. The
//    main task owns an adopted Conn (loop(), publishes) until it closes it
//    or passes it back to the worker in a cleanup job.
//  - Main-task state (g_clients[]) is touched by the main task only. The
//    worker reads only its job (a copy), immutable data (device id, topic
//    list) and the per-slot generation atomics; the receive queue is shared
//    under a spinlock.
//  - Each slot has at most one setup job outstanding, and it stays
//    outstanding until the main task has received its result: a stale
//    result is never adopted (generation check), and the next attempt, or
//    the group's cleanup job, is only queued after that result is back, so
//    no two connections ever use one client id at once. The worker runs
//    jobs strictly in queue order.
//
// What still blocks the main task (NOT eliminated by the worker; each is
// bounded, from the library's own code; a healthy link costs ~1 round trip):
//  - loop(): reads only bytes already available, but a packet received in
//    part makes lwmqtt wait up to kMqttCommandTimeoutMs (500 ms) for the rest;
//    its PINGREQ write is bounded by kSendBudgetMs (500 ms).
//  - Synchronous QoS1 publishes wait for the PUBACK: publishBinary() up to
//    kMqttPublishTimeoutMsExperiment (1500 ms), publishRaw() (presence, when
//    the connection is not lent) up to 500 ms, plus the write (<= 500 ms);
//    QoS0 publishes: the write only. Still synchronous: the Friend game
//    (it shows the PUBACK result), radio (QoS0), presence when not lent, and
//    a chat send whose store failed. On the worker instead
//    (publishBinaryAsync()): Text/Enigma chat sends (core/chat_send.h), the
//    Outbox retry, every Race publish (state heartbeat, requests, replies).
//
// Connection states (callers pick the one they need):
//  - isGroupConnected()/isGroupReady(), the ON/WIFI status: the group has a
//    live session -- adopted by the main task OR lent to the worker for a
//    publish chain. A loan never flips ON to WIFI.
//  - Publishable now on the main task: adopted and not lent. While lent,
//    publishBinary() returns false at once (a Friend move or radio frame in
//    that window fails as on any failed publish; QoS0 radio frames are
//    dropped), publishRaw() (presence) joins the chain, publishBinaryAsync()
//    joins the chain (at most kMaxLentPublishes). Nothing ever touches the
//    client from two tasks.
//  - Received messages are dispatched on the main task (cooperative drain:
//    8 ms budget between items; one slow handler is not interrupted).
//  - OTA maintenance entry and before-sleep: OFFLINE publishes, at most 2 s
//    in all, plus up to 500 ms per connection closed with an OFFLINE publish.
//  - While Wi-Fi is down loop() is not called, so a stale socket may still
//    take a publish until its PUBACK wait (above) runs out and closes it.

namespace MqttManager {

namespace {

// ---------------------------------------------------------------------------
// Timing and retry policy
// ---------------------------------------------------------------------------
// Main task (unchanged from before this fix): the MQTT command timeout for
// loop() and publishRaw(), and the wider wait for a QoS1 publishBinary()'s
// PUBACK (Hardware Diagnostic #4.9t's experiment, kept as it was).
constexpr uint32_t kMqttCommandTimeoutMs = 500;
constexpr uint32_t kMqttPublishTimeoutMsExperiment = 1500;

// Connection worker only -- never on the main task, so these can allow for a
// slow broker or link without delaying input. Hardware evidence: "2047 ms
// lastError=-9 returnCode=6" is DNS+TCP (~550 ms) followed by the old
// 1500 ms CONNACK wait expiring (returnCode 6 = LWMQTT_UNKNOWN_RETURN_CODE:
// no CONNACK was decoded, not a refusal); "3005 ms lastError=-3" is the
// ESP32 WiFiClient's 3000 ms TCP connect timeout.
constexpr int32_t kTcpConnectTimeoutMs = 4000;
constexpr uint32_t kHandshakeAckTimeoutMs = 5000;  // CONNACK, each SUBACK, the ONLINE PUBACK

// A write never waits longer than this for socket buffer space (the ESP32
// WiFiClient::write() retries up to 10 x 1 s with no progress): see
// BoundedWriteClient.
constexpr uint32_t kSendBudgetMs = 500;

// Retry after a failed setup attempt: 2 s, 4, 8, 16, 32, then 60 s, each
// +-20% jittered; reset by a successful setup. After a connection that
// reached Ready is lost, the next attempt waits until kReadyAntiThrashMs
// after Ready was reached (the existing anti-thrash window).
constexpr uint32_t kRetryMinMs = 2000;
constexpr uint32_t kRetryMaxMs = 60000;
constexpr uint32_t kReadyAntiThrashMs = 5000;
// A deleted group's broker-session cleanup (cleanSession connection) that
// fails on the network is retried on the same schedule, at most this many
// attempts in all; one skipped for OTA maintenance is redone after it and
// does not count.
constexpr uint8_t kCleanupMaxAttempts = 3;

// Broker endpoints (addresses DNS returned for kBrokerHost during this
// boot; never a built-in address). An endpoint that failed at TCP or MQTT
// level is avoided for 30 s, doubling per consecutive failure up to 10 min,
// while another one that worked recently is available.
constexpr uint8_t kMaxEndpoints = 4;
constexpr uint32_t kEndpointAvoidBaseMs = 30000;
constexpr uint32_t kEndpointAvoidMaxMs = 600000;
constexpr uint32_t kEndpointMaxAgeMs = 6UL * 3600UL * 1000UL;  // an alternative must have come from DNS this recently

// Full topic buffers ("morsebuddy/<code up to 32>/presence/<12-hex id>" is up
// to 66 bytes): the size publishRaw()/publishBinary() always used. Before
// this fix the will and presence-clear topics used 48 bytes and were
// silently truncated for group codes longer than 14 characters.
constexpr size_t kTopicBufLen = 80;

constexpr uint32_t kWorkerStackBytes = 6144;
constexpr UBaseType_t kWorkerPriority = 1;
constexpr BaseType_t kWorkerCore = 0;  // the Arduino loop runs on core 1
constexpr UBaseType_t kJobQueueDepth = 8;
// Publishes chained on one group's lent connection (publishBinaryAsync()):
// bounds how much the worker holds for a group; beyond it a publish is
// refused (the caller's retry applies) rather than queued without limit.
constexpr uint8_t kMaxLentPublishes = 4;
constexpr UBaseType_t kResultQueueDepth = 8;

// Rollover-safe "deadline reached" (0 = no deadline: eligible now).
bool deadlineReached(uint32_t now, uint32_t deadline) {
  return deadline == 0 || static_cast<int32_t>(now - deadline) >= 0;
}

// 0 is the "eligible immediately" sentinel; a deadline that wraps to
// exactly 0 is nudged to 1 (Hardware Fix #4.9f's rule).
uint32_t deadlineAfter(uint32_t now, uint32_t delayMs) {
  uint32_t d = now + delayMs;
  return d == 0 ? 1 : d;
}

uint32_t retryDelayMs(uint8_t consecutiveFailures) {
  uint8_t shift = consecutiveFailures == 0 ? 0 : static_cast<uint8_t>(consecutiveFailures - 1);
  if (shift > 5) shift = 5;
  uint32_t d = kRetryMinMs << shift;
  if (d > kRetryMaxMs) d = kRetryMaxMs;
  uint32_t jitter = d / 5;
  return d - jitter + static_cast<uint32_t>(esp_random() % (2 * jitter + 1));
}

void formatIp(uint32_t ip, char* out, size_t outSize) {  // ip as stored in sin_addr (network byte order)
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&ip);
  snprintf(out, outSize, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

// ---------------------------------------------------------------------------
// Connection objects
// ---------------------------------------------------------------------------
// WiFiClient with a bounded write(): one non-blocking send at a time, waiting
// at most kSendBudgetMs in total for buffer space. Returns what was sent (0:
// nothing; lwmqtt then reports LWMQTT_NETWORK_FAILED_WRITE and the MQTT
// client closes the connection, which the owner then sees as a loss).
class BoundedWriteClient : public WiFiClient {
 public:
  using WiFiClient::write;
  size_t write(const uint8_t* buf, size_t size) override {
    int fd = this->fd();
    if (fd < 0 || buf == nullptr || size == 0) return 0;
    uint32_t start = millis();
    size_t sent = 0;
    while (sent < size) {
      fd_set set;
      FD_ZERO(&set);
      FD_SET(fd, &set);
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 20000;
      int ready = select(fd + 1, nullptr, &set, nullptr, &tv);
      if (ready < 0) break;
      if (ready > 0) {
        int r = send(fd, buf + sent, size - sent, MSG_DONTWAIT);
        if (r > 0) {
          sent += static_cast<size_t>(r);
          continue;
        }
        if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          stop();
          break;
        }
      }
      if (static_cast<uint32_t>(millis()) - start >= kSendBudgetMs) break;
    }
    return sent;
  }
};

struct Conn;

// The MQTT client of one Conn; the receive callback finds its group through
// owner() (only the task that owns the Conn ever runs that callback).
class ConnMqttClient : public MQTTClient {
 public:
  explicit ConnMqttClient(Conn* owner) : MQTTClient(1024, 256), owner_(owner) {}
  Conn* owner() const { return owner_; }

 private:
  Conn* owner_;
};

std::atomic<int> g_liveConns(0);  // Conn objects currently allocated (diagnostics, tests)

struct Conn {
  Conn() : mqtt(this) { g_liveConns.fetch_add(1); }
  ~Conn() { g_liveConns.fetch_sub(1); }
  Conn(const Conn&) = delete;
  Conn& operator=(const Conn&) = delete;
  BoundedWriteClient net;  // declared before mqtt: mqtt holds a reference to it
  ConnMqttClient mqtt;
  char group_code[33] = {0};
  uint32_t endpoint = 0;  // broker address used (network byte order)
};

// ---------------------------------------------------------------------------
// Bounded receive queue (Addendum section 3.11): the MQTT callback only
// copies and enqueues; all parsing/dispatch happens from the AppService
// tick, after client->loop() has returned.
//
// MQTT fix: the callback now also runs on the connection worker (retained
// messages arriving while it waits for a SUBACK), so the ring's slots and
// indexes are guarded by a spinlock. Only pointer/index updates happen
// inside it: allocation, copying, freeing and dispatch stay outside.
// ---------------------------------------------------------------------------
constexpr uint8_t kQueueCapacity = 16;
constexpr uint16_t kMaxStoredPacketSize = 1024;
// Fix Phase 1D: soft elapsed-time budget for the NORMAL (service-tick)
// drain only -- see drainReceiveQueueCooperative() below. Not used by
// drainReceiveQueueFull() (maintenance entry), which must still drain to
// empty unconditionally.
constexpr uint32_t kQueueDrainSoftBudgetMs = 8;

// `data` is a heap pointer sized to the exact incoming packet length;
// kMaxStoredPacketSize caps what one packet may occupy. Ownership follows
// whichever copy the code chooses to free: a dequeued item is detached into
// a local copy and the slot relinquished BEFORE dispatch (Fix Phase 1A), so
// a reentrant enqueue can never free a payload still being dispatched.
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
portMUX_TYPE g_queueMux = portMUX_INITIALIZER_UNLOCKED;

uint8_t queueCount() {
  portENTER_CRITICAL(&g_queueMux);
  uint8_t n = g_queueCount;
  portEXIT_CRITICAL(&g_queueMux);
  return n;
}

void onMqttMessage(MQTTClient* client, char topic[], char bytes[], int length) {
  if (client == nullptr || length < 0 || static_cast<size_t>(length) > kMaxStoredPacketSize) return;
  if (queueCount() >= kQueueCapacity) return;  // full: drop (QoS0 traffic may legitimately be dropped)
  const Conn* conn = static_cast<ConnMqttClient*>(client)->owner();  // every client is a ConnMqttClient

  // Allocate and copy before touching the ring; a zero-length payload (a
  // retained "clear") is queued with a null pointer.
  uint8_t* buf = nullptr;
  if (length > 0) {
    buf = new (std::nothrow) uint8_t[static_cast<size_t>(length)];
    if (buf == nullptr) {
      Serial.println("[mqtt] payload allocation failed; dropping incoming packet");
      return;
    }
    memcpy(buf, bytes, static_cast<size_t>(length));
  }
  QueuedMessage item;
  item.used = true;
  strncpy(item.group_code, conn->group_code, sizeof(item.group_code) - 1);
  strncpy(item.topic, topic, sizeof(item.topic) - 1);
  item.len = static_cast<uint16_t>(length);
  item.data = buf;

  bool queued = false;
  uint8_t* stale = nullptr;
  portENTER_CRITICAL(&g_queueMux);
  if (g_queueCount < kQueueCapacity) {
    stale = g_queue[g_queueTail].data;  // defensive: null in correct steady-state operation
    g_queue[g_queueTail] = item;
    g_queueTail = static_cast<uint8_t>((g_queueTail + 1) % kQueueCapacity);
    g_queueCount++;
    queued = true;
  }
  portEXIT_CRITICAL(&g_queueMux);
  delete[] stale;
  if (!queued) delete[] buf;  // filled meanwhile by the other task
}

// Detaches the head item into `out` (the caller then owns out.data).
bool dequeueOne(QueuedMessage* out) {
  bool got = false;
  portENTER_CRITICAL(&g_queueMux);
  if (g_queueCount > 0) {
    uint8_t idx = g_queueHead;
    *out = g_queue[idx];
    g_queue[idx].data = nullptr;
    g_queue[idx].used = false;
    g_queue[idx].len = 0;
    g_queueHead = static_cast<uint8_t>((idx + 1) % kQueueCapacity);
    g_queueCount--;
    got = true;
  }
  portEXIT_CRITICAL(&g_queueMux);
  return got;
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

bool groupHasActiveClient(const char* group_code);  // main-task state; defined below

// One dequeued item's dispatch. `q` is a detached copy owned by the caller,
// which frees q.data once afterwards whatever path is taken here.
void processQueuedMessage(const QueuedMessage& q) {
  // MQTT fix: a message received for a group deleted since (it may have
  // been queued by the worker while that group's connection was still being
  // set up) is dropped, never dispatched for a group that no longer exists.
  if (!groupHasActiveClient(q.group_code)) return;
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
  // else: unregistered kind -- ignored safely.
}

// Dequeues, dispatches and frees exactly one item (Fix Phase 1A ownership:
// the slot is relinquished before dispatch). False if the queue was empty.
bool dequeueAndDispatchOne() {
  QueuedMessage local;
  if (!dequeueOne(&local)) return false;
  processQueuedMessage(local);
  delete[] local.data;
  return true;
}

// Fix Phase 1D: maintenance-mode full drain (setMaintenanceModeActive()
// only): until empty, no snapshot cap, no time budget.
void drainReceiveQueueFull() {
  uint32_t drainStart = millis();
  uint16_t drainedCount = 0;
  while (dequeueAndDispatchOne()) drainedCount++;
  uint32_t drainElapsed = millis() - drainStart;
  if (drainElapsed >= 20) {
    Serial.printf("[PERF][MQTT] drain %lu ms count=%u\n", static_cast<unsigned long>(drainElapsed),
                  static_cast<unsigned>(drainedCount));
  }
}

// Fix Phase 1D: normal service-tick drain, bounded by an entry snapshot of
// the count and a soft elapsed budget checked between items (the first item
// always completes; a slow handler is not interrupted). Never discards.
void drainReceiveQueueCooperative() {
  uint32_t drainStart = millis();
  uint16_t drainedCount = 0;
  uint8_t toProcess = queueCount();  // entry snapshot
  for (uint8_t i = 0; i < toProcess; i++) {
    if (!dequeueAndDispatchOne()) break;
    drainedCount++;
    if (i + 1 >= toProcess) break;
    uint32_t nowMs = static_cast<uint32_t>(millis());
    if (nowMs - drainStart >= kQueueDrainSoftBudgetMs) break;
  }
  uint32_t drainElapsed = static_cast<uint32_t>(millis()) - drainStart;
  uint8_t remaining = queueCount();
  if (drainElapsed >= 20) {
    Serial.printf("[PERF][MQTT] drain %lu ms count=%u remaining=%u deferred=%d\n",
                  static_cast<unsigned long>(drainElapsed), static_cast<unsigned>(drainedCount),
                  static_cast<unsigned>(remaining), remaining != 0 ? 1 : 0);
  }
}

// Frees every payload still queued (maintenance entry's defensive fallback).
void releaseQueuedPayloads() {
  uint8_t* toFree[kQueueCapacity];
  uint8_t n = 0;
  portENTER_CRITICAL(&g_queueMux);
  for (auto& q : g_queue) {
    if (q.used) {
      toFree[n++] = q.data;
      q.data = nullptr;
      q.used = false;
      q.len = 0;
    }
  }
  g_queueHead = 0;
  g_queueTail = 0;
  g_queueCount = 0;
  portEXIT_CRITICAL(&g_queueMux);
  for (uint8_t i = 0; i < n; i++) delete[] toFree[i];
}

// ---------------------------------------------------------------------------
// Client id and topics
// ---------------------------------------------------------------------------
void buildClientId(const char* group_code, char* outBuf, size_t outBufSize) {
  uint32_t crc = Crc32::computeStr(group_code);
  snprintf(outBuf, outBufSize, "%s_%08lX", Identity::deviceId(), static_cast<unsigned long>(crc));
}

// The topic list (10 since the Race room protocol replaced the five race/*
// topics with r2/+ and r2q/<own id>), in the same order and with the same
// QoS as before; subscribed by the connection worker during setup. Built
// once, by the main task before the worker starts (ensureWorker()); read-only
// afterwards (Identity::deviceId() is immutable after Identity::init()).
struct TopicSpec {
  char suffix[32];
  int qos;
};
// Race rooms (design r8 section 3): the five old race/* topics are replaced by
// r2/+ (retained room states) and r2q/<own id> (requests and replies), so the
// list is 10 topics.
constexpr size_t kTopicCount = 10;
const TopicSpec* getTopicSpecs() {
  static TopicSpec specs[kTopicCount];
  static bool built = false;
  if (!built) {
    const char* dev = Identity::deviceId();
    // Fix Phase 1C (corrected): the four device-specific suffixes are
    // first built into intermediate buffers whose sizes EXACTLY match the
    // pre-candidate subscribeAll()'s own msgSuffix[24]/busySuffix[24]/
    // busyReplySuffix[32]/audioSuffix[24], then copied into this file's
    // uniform TopicSpec::suffix[32] storage. This is deliberate, not an
    // oversight: an earlier revision of this candidate built every suffix
    // directly into the uniform 32-byte storage, which incidentally
    // widened audioSuffix's effective capacity and silently STOPPED
    // truncating it. audioSuffix[24] is one byte too small for
    // "radio/audio/<12-hex-char-device-id>" (12 + 12 + 1 = 25 bytes
    // needed) and has always truncated the last hex character of the
    // subscribed topic -- a real, pre-existing correctness issue, but
    // fixing it was never requested for this candidate and was not
    // re-verified against whatever the corresponding publish side
    // actually sends. Silently widening the buffer here would be an
    // unrequested behavior change smuggled into a staged-setup-timing
    // candidate. The truncation is reproduced byte-for-byte instead; see
    // this candidate's final report for the separate correctness task
    // this belongs to (sender/receiver audit + two-device validation).
    char msgSuffix[24];
    snprintf(msgSuffix, sizeof(msgSuffix), "msg/%s", dev);
    char busySuffix[24];
    snprintf(busySuffix, sizeof(busySuffix), "radio/busy/%s", dev);
    char busyReplySuffix[32];
    snprintf(busyReplySuffix, sizeof(busyReplySuffix), "radio/busy_reply/%s", dev);
    char raceSuffix[24];
    snprintf(raceSuffix, sizeof(raceSuffix), "r2q/%s", dev);
    char audioSuffix[24];
    snprintf(audioSuffix, sizeof(audioSuffix), "radio/audio/%s", dev);  // truncated by 1 byte, reproduced as-is

    snprintf(specs[0].suffix, sizeof(specs[0].suffix), "presence/+");
    specs[0].qos = 1;
    snprintf(specs[1].suffix, sizeof(specs[1].suffix), "%s", msgSuffix);
    specs[1].qos = 1;
    snprintf(specs[2].suffix, sizeof(specs[2].suffix), "broadcast");
    specs[2].qos = 1;
    snprintf(specs[3].suffix, sizeof(specs[3].suffix), "r2/+");
    specs[3].qos = 1;
    snprintf(specs[4].suffix, sizeof(specs[4].suffix), "%s", raceSuffix);
    specs[4].qos = 1;
    snprintf(specs[5].suffix, sizeof(specs[5].suffix), "%s", busySuffix);
    specs[5].qos = 0;
    snprintf(specs[6].suffix, sizeof(specs[6].suffix), "%s", busyReplySuffix);
    specs[6].qos = 0;
    snprintf(specs[7].suffix, sizeof(specs[7].suffix), "radio/session");
    specs[7].qos = 0;
    snprintf(specs[8].suffix, sizeof(specs[8].suffix), "%s", audioSuffix);
    specs[8].qos = 0;
    snprintf(specs[9].suffix, sizeof(specs[9].suffix), "radio/audio/broadcast");
    specs[9].qos = 0;
    built = true;
  }
  return specs;
}


// ---------------------------------------------------------------------------
// Jobs and results (main task <-> connection worker, by value through
// FreeRTOS queues; a Conn pointer in one is an ownership transfer)
// ---------------------------------------------------------------------------
enum class JobType : uint8_t { kSetup, kCleanup, kPublish };

// Where a setup attempt stopped.
enum class Stage : uint8_t { kNone, kCanceled, kAlloc, kDns, kTcp, kConnack, kSubscribe, kPublishOnline };

const char* stageName(Stage s) {
  switch (s) {
    case Stage::kNone: return "none";
    case Stage::kCanceled: return "canceled";
    case Stage::kAlloc: return "alloc";
    case Stage::kDns: return "dns";
    case Stage::kTcp: return "tcp";
    case Stage::kConnack: return "connack";
    case Stage::kSubscribe: return "subscribe";
    case Stage::kPublishOnline: return "publishOnline";
  }
  return "?";
}

struct Job {
  JobType type;
  uint8_t slot;
  uint32_t generation;
  char group_code[33];
  char client_id[24];
  // kSetup: the will (this device's OFFLINE presence) and the ONLINE
  // presence it publishes last. kCleanup: the OFFLINE presence to publish
  // on `conn` before clearing it.
  char willOrOffline[Presence::kPresencePayloadMax];
  char online[Presence::kPresencePayloadMax];
  Conn* conn;  // kCleanup: the group's connection to close (may be null); kPublish: the lent connection
  // kPublish: one publish on the lent connection; the worker frees `payload`.
  char suffix[32];  // "morsebuddy/" + code (<= 32) + "/" + suffix (<= 31) fits kTopicBufLen
  uint8_t* payload;
  uint16_t payloadLen;
  bool retained;
  uint8_t qos;
  uint32_t ticket;
  void (*done)(uint32_t ticket, bool ok);  // kPublish: called with the result on the main task (may be null)
};

struct Result {
  JobType type;
  uint8_t slot;
  uint32_t generation;
  bool ok;
  Stage failedAt;
  int lastError;
  int returnCode;
  uint32_t endpoint;
  uint32_t elapsedMs;
  Conn* conn;  // kSetup and ok: the ready connection, now the main task's; kPublish: the lent one, back
  uint32_t ticket;  // kPublish
  void (*done)(uint32_t ticket, bool ok);  // kPublish
};

QueueHandle_t g_jobQueue = nullptr;
QueueHandle_t g_resultQueue = nullptr;
TaskHandle_t g_workerTask = nullptr;

// Written by the main task only; read by the worker between steps. A job
// whose generation no longer matches its slot's is canceled (before its
// ONLINE publish) and its result is never adopted.
std::atomic<uint32_t> g_slotGeneration[Settings::kMaxGroups];
// Set for OTA maintenance and before sleep: the worker starts no network
// step for a setup job while it is set.
std::atomic<bool> g_workerSuspended(false);

// ---------------------------------------------------------------------------
// Broker endpoints (connection worker only)
// ---------------------------------------------------------------------------
struct Endpoint {
  bool used = false;
  uint32_t ip = 0;  // network byte order
  uint32_t lastSeenMs = 0;  // last returned by DNS
  uint32_t lastOkMs = 0;
  uint32_t avoidUntilMs = 0;
  uint8_t failures = 0;  // consecutive
  bool everOk = false;
};
Endpoint g_endpoints[kMaxEndpoints];

Endpoint* findEndpoint(uint32_t ip) {
  for (auto& e : g_endpoints)
    if (e.used && e.ip == ip) return &e;
  return nullptr;
}

bool endpointAvoided(const Endpoint& e, uint32_t now) {
  return e.failures > 0 && static_cast<int32_t>(e.avoidUntilMs - now) > 0;
}

Endpoint* noteResolvedEndpoint(uint32_t ip, uint32_t now) {
  Endpoint* e = findEndpoint(ip);
  if (e == nullptr) {
    for (auto& c : g_endpoints) {
      if (!c.used) {
        e = &c;
        break;
      }
    }
    if (e == nullptr) {  // full: replace the one DNS returned least recently
      e = &g_endpoints[0];
      for (auto& c : g_endpoints)
        if (static_cast<int32_t>(c.lastSeenMs - e->lastSeenMs) < 0) e = &c;
    }
    *e = Endpoint();
    e->used = true;
    e->ip = ip;
  }
  e->lastSeenMs = now;
  return e;
}

bool endpointRecent(const Endpoint& e, uint32_t now) {
  return static_cast<uint32_t>(now - e.lastSeenMs) <= kEndpointMaxAgeMs;
}

// The address to try, from the addresses this lookup returned (`resolved`,
// in DNS order) and those earlier lookups returned within kEndpointMaxAgeMs:
//  1. one from this lookup that is not being avoided -- one that has
//     worked before first, then the others in DNS order (so a second
//     address of the same answer is tried after the first failed, with no
//     earlier success needed);
//  2. else an earlier lookup's address that is not being avoided -- the
//     most recently working one, then one never tried yet;
//  3. else (all avoided) the address whose avoidance ends first.
// Avoidance is a preference, never a hard block. Only addresses DNS
// returned are ever used. 0: nothing known (DNS down, empty table).
uint32_t chooseEndpoint(const uint32_t* resolved, uint8_t n, uint32_t now) {
  for (uint8_t i = 0; i < n; i++) noteResolvedEndpoint(resolved[i], now);
  for (int pass = 0; pass < 2; pass++) {
    for (uint8_t i = 0; i < n; i++) {
      const Endpoint* e = findEndpoint(resolved[i]);
      if (e != nullptr && !endpointAvoided(*e, now) && (pass == 1 || e->everOk)) return resolved[i];
    }
  }
  const Endpoint* best = nullptr;
  for (const auto& e : g_endpoints) {  // earlier lookups' addresses that worked
    if (!e.used || !e.everOk || endpointAvoided(e, now) || !endpointRecent(e, now)) continue;
    if (best == nullptr || static_cast<int32_t>(e.lastOkMs - best->lastOkMs) > 0) best = &e;
  }
  if (best != nullptr) return best->ip;
  for (const auto& e : g_endpoints) {  // earlier lookups' addresses never tried
    if (!e.used || e.everOk || e.failures != 0 || !endpointRecent(e, now)) continue;
    if (best == nullptr || static_cast<int32_t>(e.lastSeenMs - best->lastSeenMs) > 0) best = &e;
  }
  if (best != nullptr) return best->ip;
  for (const auto& e : g_endpoints) {  // all avoided: the one free soonest
    if (!e.used || !endpointRecent(e, now)) continue;
    if (best == nullptr || static_cast<int32_t>(e.avoidUntilMs - best->avoidUntilMs) < 0) best = &e;
  }
  return best != nullptr ? best->ip : 0;
}

// ESP32 lwIP keeps ONE address per name (the first A record of the answer)
// until its TTL expires, so a later lookup would return the same failing
// address again. After an endpoint failure the DNS cache is cleared -- in
// lwIP's own thread, as its API requires -- so the next lookup asks the DNS
// server again and may learn another address. At most once per failed
// attempt, so bounded by the retry backoff. (Other names cached by lwIP
// are simply looked up again on their next use.)
void clearDnsCacheInTcpipThread(void*) { dns_clear_cache(); }

void noteEndpointFailure(uint32_t ip, uint32_t now) {
  Endpoint* e = findEndpoint(ip);
  if (e == nullptr) return;
  if (e->failures < 255) e->failures++;
  uint8_t shift = static_cast<uint8_t>(e->failures - 1);
  if (shift > 5) shift = 5;
  uint32_t avoid = kEndpointAvoidBaseMs << shift;
  if (avoid > kEndpointAvoidMaxMs) avoid = kEndpointAvoidMaxMs;
  e->avoidUntilMs = now + avoid;
  tcpip_callback(clearDnsCacheInTcpipThread, nullptr);
}

void noteEndpointOk(uint32_t ip, uint32_t now) {
  Endpoint* e = findEndpoint(ip);
  if (e == nullptr) return;
  e->failures = 0;
  e->avoidUntilMs = 0;
  e->everOk = true;
  e->lastOkMs = now;
}

// lwIP's own resolver (thread-safe, unlike WiFiGenericClass::hostByName()).
// Fills `out` with up to `cap` distinct IPv4 addresses (network byte order)
// in the answer's order; returns how many. (On this ESP32 lwIP an answer
// carries one address; a resolver returning several is handled the same way.)
uint8_t resolveBroker(uint32_t* out, uint8_t cap) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  struct addrinfo* res = nullptr;
  int err = lwip_getaddrinfo(kBrokerHost, nullptr, &hints, &res);
  uint8_t n = 0;
  if (err == 0) {
    for (struct addrinfo* a = res; a != nullptr && n < cap; a = a->ai_next) {
      if (a->ai_family != AF_INET || a->ai_addr == nullptr) continue;
      uint32_t ip = reinterpret_cast<struct sockaddr_in*>(a->ai_addr)->sin_addr.s_addr;
      bool dup = ip == 0;
      for (uint8_t i = 0; i < n && !dup; i++) dup = out[i] == ip;
      if (!dup) out[n++] = ip;
    }
  }
  if (res != nullptr) lwip_freeaddrinfo(res);
  return n;
}

// ---------------------------------------------------------------------------
// Connection worker
// ---------------------------------------------------------------------------
// Opens the TCP connection to an endpoint and runs CONNECT/CONNACK on `c`.
// cleanSession=false and the will for a setup; cleanSession=true and no will
// for a cleanup. Fills r's error fields; false on failure (c left closed).
bool openConnection(Conn& c, const char* clientId, bool cleanSession, const char* willPayload, Result& r,
                    uint32_t& dnsMs, uint32_t& tcpMs, uint32_t& connackMs) {
  uint32_t t0 = millis();
  uint32_t resolved[kMaxEndpoints];
  uint8_t resolvedCount = resolveBroker(resolved, kMaxEndpoints);
  uint32_t now = millis();
  dnsMs = now - t0;
  c.endpoint = chooseEndpoint(resolved, resolvedCount, now);
  r.endpoint = c.endpoint;
  if (c.endpoint == 0) {
    r.failedAt = Stage::kDns;
    return false;
  }

  t0 = millis();
  int tcp = c.net.connect(IPAddress(c.endpoint), kBrokerPort, kTcpConnectTimeoutMs);
  tcpMs = millis() - t0;
  if (tcp <= 0) {
    r.failedAt = Stage::kTcp;
    r.lastError = LWMQTT_NETWORK_FAILED_CONNECT;
    noteEndpointFailure(c.endpoint, millis());
    return false;
  }

  c.mqtt.begin(c.net);
  c.mqtt.onMessageAdvanced(onMqttMessage);
  c.mqtt.setKeepAlive(20);
  c.mqtt.setCleanSession(cleanSession);
  c.mqtt.setTimeout(kHandshakeAckTimeoutMs);
  if (willPayload != nullptr) {
    char willTopic[kTopicBufLen];
    snprintf(willTopic, sizeof(willTopic), "morsebuddy/%s/presence/%s", c.group_code, Identity::deviceId());
    c.mqtt.setWill(willTopic, willPayload, /*retained=*/true, /*qos=*/1);
  }
  t0 = millis();
  bool connected = c.mqtt.connect(clientId, nullptr, nullptr, /*skip=*/true);
  connackMs = millis() - t0;
  r.lastError = static_cast<int>(c.mqtt.lastError());
  r.returnCode = static_cast<int>(c.mqtt.returnCode());
  if (!connected) {
    r.failedAt = Stage::kConnack;
    // A refusal (CONNACK received, return code != 0) is the broker
    // answering: not this endpoint's fault.
    if (r.lastError != LWMQTT_CONNECTION_DENIED) noteEndpointFailure(c.endpoint, millis());
    c.net.stop();
    return false;
  }
  return true;
}

bool jobCanceled(const Job& job) {
  return g_workerSuspended.load() || g_slotGeneration[job.slot].load() != job.generation;
}

void postResult(const Result& r) {
  // The main task drains results every tick (and while entering
  // maintenance); a result is never dropped, since it may carry a Conn and
  // it is what releases the slot's one outstanding job.
  while (xQueueSend(g_resultQueue, &r, portMAX_DELAY) != pdTRUE) {
  }
}

void runSetupJob(const Job& job) {
  uint32_t start = millis();
  Result r;
  memset(&r, 0, sizeof(r));
  r.type = JobType::kSetup;
  r.slot = job.slot;
  r.generation = job.generation;
  r.failedAt = Stage::kNone;

  Conn* c = nullptr;
  uint32_t dnsMs = 0, tcpMs = 0, connackMs = 0, subscribeMs = 0, onlineMs = 0;
  if (jobCanceled(job)) {
    r.failedAt = Stage::kCanceled;
  } else if ((c = new (std::nothrow) Conn()) == nullptr) {
    r.failedAt = Stage::kAlloc;
  } else {
    strncpy(c->group_code, job.group_code, sizeof(c->group_code) - 1);
    if (openConnection(*c, job.client_id, /*cleanSession=*/false, job.willOrOffline, r, dnsMs, tcpMs, connackMs)) {
      const TopicSpec* specs = getTopicSpecs();
      uint32_t t0 = millis();
      for (size_t i = 0; i < kTopicCount && r.failedAt == Stage::kNone; i++) {
        if (jobCanceled(job)) {
          r.failedAt = Stage::kCanceled;
          break;
        }
        char topic[80];
        snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", job.group_code, specs[i].suffix);
        if (!c->mqtt.subscribe(topic, specs[i].qos)) {
          r.failedAt = Stage::kSubscribe;
          r.lastError = static_cast<int>(c->mqtt.lastError());
          Serial.printf("[PERF][MQTT] subscribe group=%s topic=%s result=0 lastError=%d\n", job.group_code,
                        specs[i].suffix, r.lastError);
        }
      }
      subscribeMs = millis() - t0;
      if (r.failedAt == Stage::kNone && jobCanceled(job)) r.failedAt = Stage::kCanceled;
      if (r.failedAt == Stage::kNone) {
        // Last step, as before: setup only counts once the ONLINE presence
        // itself was acknowledged and the connection is still up.
        char selfTopic[kTopicBufLen];
        snprintf(selfTopic, sizeof(selfTopic), "morsebuddy/%s/presence/%s", job.group_code, Identity::deviceId());
        t0 = millis();
        bool published = c->mqtt.publish(selfTopic, job.online, /*retained=*/true, /*qos=*/1);
        onlineMs = millis() - t0;
        if (!published || !c->mqtt.connected()) {
          r.failedAt = Stage::kPublishOnline;
          r.lastError = static_cast<int>(c->mqtt.lastError());
        }
      }
      if (r.failedAt == Stage::kSubscribe || r.failedAt == Stage::kPublishOnline) {
        noteEndpointFailure(c->endpoint, millis());
      }
    }
  }

  if (r.failedAt == Stage::kNone) {
    // From here on the main task owns it: its loop()/publishRaw() timeout.
    c->mqtt.setTimeout(kMqttCommandTimeoutMs);
    noteEndpointOk(c->endpoint, millis());
    r.ok = true;
    r.conn = c;
  } else if (c != nullptr) {
    if (c->mqtt.connected()) c->mqtt.disconnect();
    delete c;
  }
  r.elapsedMs = millis() - start;

  char ip[16];
  formatIp(r.endpoint, ip, sizeof(ip));
  Serial.printf(
      "[PERF][MQTT][worker] setup group=%s result=%d stage=%s ip=%s dnsMs=%lu tcpMs=%lu connackMs=%lu "
      "subscribeMs=%lu onlineMs=%lu totalMs=%lu lastError=%d returnCode=%d stackFree=%u\n",
      job.group_code, r.ok ? 1 : 0, stageName(r.failedAt), ip, static_cast<unsigned long>(dnsMs),
      static_cast<unsigned long>(tcpMs), static_cast<unsigned long>(connackMs),
      static_cast<unsigned long>(subscribeMs), static_cast<unsigned long>(onlineMs),
      static_cast<unsigned long>(r.elapsedMs), r.lastError, r.returnCode,
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  postResult(r);
}

// Group deletion (Addendum section 5): OFFLINE presence and a zero-length
// retained publish (removes the record) on the group's connection if it is
// up, disconnect, then one connection with cleanSession=true under the same
// client id to ask the broker to drop the session. Best-effort, as before.
// The cleanSession connection is skipped while suspended (OTA/sleep).
void runCleanupJob(const Job& job) {
  uint32_t start = millis();
  Result r;
  memset(&r, 0, sizeof(r));
  r.type = JobType::kCleanup;
  r.slot = job.slot;
  r.generation = job.generation;
  r.failedAt = Stage::kNone;

  if (job.conn != nullptr) {
    Conn* c = job.conn;
    if (c->mqtt.connected()) {
      char selfTopic[kTopicBufLen];
      snprintf(selfTopic, sizeof(selfTopic), "morsebuddy/%s/presence/%s", job.group_code, Identity::deviceId());
      c->mqtt.setTimeout(kHandshakeAckTimeoutMs);
      c->mqtt.publish(selfTopic, job.willOrOffline, /*retained=*/true, /*qos=*/1);
      c->mqtt.publish(selfTopic, "", /*retained=*/true, /*qos=*/1);  // zero-length retained: remove our record
      c->mqtt.disconnect();
    }
    delete c;
  }

  if (!g_workerSuspended.load()) {
    Conn* t = new (std::nothrow) Conn();
    if (t == nullptr) {
      r.failedAt = Stage::kAlloc;
    } else {
      strncpy(t->group_code, job.group_code, sizeof(t->group_code) - 1);
      uint32_t dnsMs = 0, tcpMs = 0, connackMs = 0;
      if (openConnection(*t, job.client_id, /*cleanSession=*/true, nullptr, r, dnsMs, tcpMs, connackMs)) {
        r.ok = true;
        t->mqtt.disconnect();
      }
      delete t;
    }
  } else {
    r.failedAt = Stage::kCanceled;
  }
  r.elapsedMs = millis() - start;
  Serial.printf("[MQTT][worker] cleanup group=%s sessionCleared=%d stage=%s totalMs=%lu\n", job.group_code,
                r.ok ? 1 : 0, stageName(r.failedAt), static_cast<unsigned long>(r.elapsedMs));
  postResult(r);
}

// One publish on a connection the main task lent for it (publishBinaryAsync()):
// the PUBACK wait happens here, not on the main loop. The connection always
// goes back with the result, open or closed (a failed publish closes it, as
// the library does); the main task decides what becomes of it.
void runPublishJob(const Job& job) {
  Result r;
  memset(&r, 0, sizeof(r));
  r.type = JobType::kPublish;
  r.slot = job.slot;
  r.generation = job.generation;
  r.ticket = job.ticket;
  r.done = job.done;
  r.conn = job.conn;
  Conn* c = job.conn;
  uint32_t start = millis();
  // A canceled loan (Wi-Fi recovery, maintenance, sleep) publishes nothing
  // more: its connection comes back closed. A publish after an earlier one
  // of the same loan failed finds the connection closed and fails at once.
  if (c != nullptr && !jobCanceled(job) && c->mqtt.connected()) {
    char topic[kTopicBufLen];
    snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", job.group_code, job.suffix);
    c->mqtt.setTimeout(kHandshakeAckTimeoutMs);
    r.ok = c->mqtt.publish(topic, reinterpret_cast<const char*>(job.payload), static_cast<int>(job.payloadLen),
                           job.retained, job.qos);
    r.lastError = static_cast<int>(c->mqtt.lastError());
    c->mqtt.setTimeout(kMqttCommandTimeoutMs);  // back to the main task's timeout
  }
  delete[] job.payload;
  r.elapsedMs = millis() - start;
  if (!r.ok || r.elapsedMs >= 20) {
    Serial.printf("[PERF][MQTT][worker] publish group=%s qos=%u bytes=%u %lu ms result=%d lastError=%d\n",
                  job.group_code, static_cast<unsigned>(job.qos), static_cast<unsigned>(job.payloadLen),
                  static_cast<unsigned long>(r.elapsedMs), r.ok ? 1 : 0, r.lastError);
  }
  postResult(r);
}

void runJob(const Job& job) {
  if (job.type == JobType::kSetup) {
    runSetupJob(job);
  } else if (job.type == JobType::kCleanup) {
    runCleanupJob(job);
  } else {
    runPublishJob(job);
  }
}

void workerMain(void*) {
  for (;;) {
    Job job;
    if (xQueueReceive(g_jobQueue, &job, portMAX_DELAY) == pdTRUE) runJob(job);
  }
}

// ---------------------------------------------------------------------------
// Main-task state: one slot per Family Group
// ---------------------------------------------------------------------------
enum class SlotState : uint8_t {
  kFree,
  kActive,    // a configured group
  kDraining,  // deleted; until its cleanup job has completed (a setup job still out comes back first)
};

struct GroupClient {
  SlotState state = SlotState::kFree;
  char group_code[33] = {0};
  char client_id[24] = {0};
  // Adopted connection (main task only). Non-null only once setup has fully
  // completed on the worker: transport up, 10 subscriptions and the ONLINE
  // publish acknowledged.
  Conn* conn = nullptr;
  bool ready = false;
  bool jobInFlight = false;  // one setup job with the worker; cleared only by its result
  uint32_t jobGeneration = 0;
  uint32_t nextAttemptMs = 0;  // 0: eligible immediately
  uint8_t consecutiveFailures = 0;
  // The ONLINE payload handed to the worker (CRC and timestamp), to tell at
  // adoption whether this device's presence changed meanwhile.
  uint32_t onlineCrc = 0;
  uint32_t onlineTs = 0;
  // kDraining: the cleanup job is with the worker / failed cleanup attempts.
  // The slot (and so its client id, see clientIdDraining()) stays reserved
  // until the cleanup is done or given up.
  bool cleanupInFlight = false;
  uint8_t cleanupAttempts = 0;
  // The adopted connection is lent to the worker for a chain of publishes
  // (publishBinaryAsync(), and publishRaw() while lent): `lentPublishes`
  // queued or running, at most kMaxLentPublishes, published by the worker in
  // order; `conn` is null and `lentConn` is the worker's meanwhile. It still
  // counts as the group's live connection: no setup or cleanup (same client
  // id) starts, and the group still reads as connected. It comes back with
  // the chain's last result; each publish's callback runs with its own.
  uint8_t lentPublishes = 0;
  Conn* lentConn = nullptr;
  uint32_t lentGeneration = 0;
};

bool isLent(const GroupClient& c) { return c.lentPublishes > 0; }

GroupClient g_clients[Settings::kMaxGroups];
bool g_maintenanceMode = false;
// Wi-Fi's own connected/disconnected transitions, to detect the tick it
// recovers. Initialized true so the first tick never runs the recovery path.
bool g_wifiWasConnected = true;

uint8_t slotIndex(const GroupClient& c) { return static_cast<uint8_t>(&c - g_clients); }

GroupClient* findClientSlot(const char* group_code) {
  for (auto& c : g_clients) {
    if (c.state == SlotState::kActive && strcmp(c.group_code, group_code) == 0) return &c;
  }
  return nullptr;
}

bool groupHasActiveClient(const char* group_code) { return findClientSlot(group_code) != nullptr; }

// Cancels the slot's outstanding setup job, if any (it stops before its
// ONLINE publish) and makes its result stale.
void bumpGeneration(uint8_t slot) { g_slotGeneration[slot].fetch_add(1); }

// Closes and frees a connection the main task owns. With publishOffline,
// first publishes this device's OFFLINE presence on it (maintenance/sleep:
// a graceful disconnect suppresses the will).
void disposeConn(Conn* c, bool publishOffline) {
  if (c == nullptr) return;
  if (c->mqtt.connected()) {
    if (publishOffline) {
      char payload[Presence::kPresencePayloadMax];
      Presence::buildOwnPresencePayload(payload, sizeof(payload), "OFFLINE", WifiManager::getUnixTime());
      char topic[kTopicBufLen];
      snprintf(topic, sizeof(topic), "morsebuddy/%s/presence/%s", c->group_code, Identity::deviceId());
      c->mqtt.publish(topic, payload, /*retained=*/true, /*qos=*/1);
    }
    c->mqtt.disconnect();
  }
  delete c;
}

// The slot's adopted connection is gone or must go: frees it and clears Ready.
void dropAdoptedConn(GroupClient& c, bool publishOffline) {
  disposeConn(c.conn, publishOffline);
  c.conn = nullptr;
  c.ready = false;
}

// Maintenance: a deleted group's connection is not kept open through OTA. It
// publishes OFFLINE and clears its retained record (as its cleanup job
// would) and is closed now; the broker-session cleanup runs after maintenance.
void retireDrainingConnForMaintenance(GroupClient& c) {
  if (c.conn == nullptr) return;
  if (c.conn->mqtt.connected()) {
    char topic[kTopicBufLen];
    snprintf(topic, sizeof(topic), "morsebuddy/%s/presence/%s", c.group_code, Identity::deviceId());
    char payload[Presence::kPresencePayloadMax];
    Presence::buildOwnPresencePayload(payload, sizeof(payload), "OFFLINE", WifiManager::getUnixTime());
    c.conn->mqtt.publish(topic, payload, /*retained=*/true, /*qos=*/1);
    c.conn->mqtt.publish(topic, "", /*retained=*/true, /*qos=*/1);
  }
  dropAdoptedConn(c, /*publishOffline=*/false);
}

// The single place a slot gets a new identity (fresh or reused); every field
// is reset, and the generation bumped so nothing from a prior occupant can
// be mistaken for this one.
GroupClient* findOrCreateClientSlot(const char* group_code) {
  GroupClient* existing = findClientSlot(group_code);
  if (existing != nullptr) return existing;
  for (auto& c : g_clients) {
    if (c.state != SlotState::kFree) continue;
    c = GroupClient();
    c.state = SlotState::kActive;
    strncpy(c.group_code, group_code, sizeof(c.group_code) - 1);
    buildClientId(group_code, c.client_id, sizeof(c.client_id));
    bumpGeneration(slotIndex(c));
    return &c;
  }
  return nullptr;  // every slot active or draining: ensureSlotsForConfiguredGroups() retries
}

// Creates a slot for every configured group that has none (e.g. a group
// added while the slot of a just-deleted one was still draining). RAM only.
void ensureSlotsForConfiguredGroups() {
  uint8_t n = Settings::getGroupCount();
  for (uint8_t i = 0; i < n; i++) {
    Settings::FamilyGroup g = Settings::getGroup(i);
    if (!g.configured || g.code[0] == '\0') continue;
    if (findClientSlot(g.code) == nullptr) findOrCreateClientSlot(g.code);
  }
}

bool clientIdDraining(const char* client_id) {
  for (auto& c : g_clients) {
    if (c.state == SlotState::kDraining && strcmp(c.client_id, client_id) == 0) return true;
  }
  return false;
}

bool ensureWorker() {
  if (g_workerTask != nullptr) return true;
  if (g_jobQueue == nullptr) g_jobQueue = xQueueCreate(kJobQueueDepth, sizeof(Job));
  if (g_resultQueue == nullptr) g_resultQueue = xQueueCreate(kResultQueueDepth, sizeof(Result));
  if (g_jobQueue == nullptr || g_resultQueue == nullptr) return false;
  getTopicSpecs();  // built here, before the worker can read it
  if (xTaskCreatePinnedToCore(workerMain, "mqtt_conn", kWorkerStackBytes, nullptr, kWorkerPriority, &g_workerTask,
                              kWorkerCore) != pdPASS) {
    g_workerTask = nullptr;
    static uint32_t lastLogMs = 0;
    uint32_t now = millis();
    if (lastLogMs == 0 || now - lastLogMs >= 10000) {
      Serial.println("[mqtt] connection worker could not be started; retrying");
      lastLogMs = now == 0 ? 1 : now;
    }
    return false;
  }
  return true;
}

// Queues a deleted group's cleanup once its setup job (if any) is back and
// its retry time has come. Not during maintenance and not while Wi-Fi is
// down (it needs the network); the slot stays draining meanwhile.
void postPendingCleanups() {
  if (g_maintenanceMode || g_jobQueue == nullptr || !WifiManager::isConnected()) return;
  uint32_t now = millis();
  for (auto& c : g_clients) {
    if (c.state != SlotState::kDraining || c.jobInFlight || c.cleanupInFlight || isLent(c)) continue;
    if (!deadlineReached(now, c.nextAttemptMs)) continue;
    Job job;
    memset(&job, 0, sizeof(job));
    job.type = JobType::kCleanup;
    job.slot = slotIndex(c);
    job.generation = g_slotGeneration[job.slot].load();
    strncpy(job.group_code, c.group_code, sizeof(job.group_code) - 1);
    strncpy(job.client_id, c.client_id, sizeof(job.client_id) - 1);
    Presence::buildOwnPresencePayload(job.willOrOffline, sizeof(job.willOrOffline), "OFFLINE",
                                      WifiManager::getUnixTime());
    job.conn = c.conn;
    if (xQueueSend(g_jobQueue, &job, 0) != pdTRUE) return;  // queue full: next tick
    c.conn = nullptr;  // the connection now belongs to the job
    c.cleanupInFlight = true;
  }
}

void freeSlot(GroupClient& c) {
  uint8_t slot = slotIndex(c);
  c = GroupClient();
  bumpGeneration(slot);
}

void handleCleanupResult(const Result& r) {
  GroupClient& c = g_clients[r.slot];
  if (c.state != SlotState::kDraining || !c.cleanupInFlight) return;  // defensive: not this slot's cleanup
  c.cleanupInFlight = false;
  if (r.ok) {
    freeSlot(c);
    return;
  }
  if (r.failedAt == Stage::kCanceled) {
    c.nextAttemptMs = 0;  // skipped for maintenance: redone after it, not an attempt
    Serial.printf("[MQTT] cleanup group=%s deferred until maintenance ends\n", c.group_code);
    return;
  }
  if (++c.cleanupAttempts >= kCleanupMaxAttempts) {
    Serial.printf("[MQTT] cleanup group=%s broker session not cleared after %u attempts; giving up\n", c.group_code,
                  static_cast<unsigned>(c.cleanupAttempts));
    freeSlot(c);
    return;
  }
  c.nextAttemptMs = deadlineAfter(millis(), retryDelayMs(c.cleanupAttempts));
}

// Queues a setup job for every group due for an attempt. Posting copies a
// few hundred bytes: nothing here waits on the network.
void postDueSetupJobs(uint32_t now) {
  if (g_jobQueue == nullptr) return;
  for (auto& c : g_clients) {
    if (c.state != SlotState::kActive || c.conn != nullptr || c.jobInFlight || isLent(c)) continue;
    if (!deadlineReached(now, c.nextAttemptMs)) continue;
    if (clientIdDraining(c.client_id)) continue;  // its cleanup (same client id) must run first
    Job job;
    memset(&job, 0, sizeof(job));
    job.type = JobType::kSetup;
    job.slot = slotIndex(c);
    job.generation = g_slotGeneration[job.slot].load();
    strncpy(job.group_code, c.group_code, sizeof(job.group_code) - 1);
    strncpy(job.client_id, c.client_id, sizeof(job.client_id) - 1);
    // Will: unchanged format (Feature Fix #4.8 section 2I).
    snprintf(job.willOrOffline, sizeof(job.willOrOffline), "MBP1|%s|%s|OFFLINE|0|%lu|%u", Identity::deviceId(),
             Settings::getMyName(), static_cast<unsigned long>(WifiManager::getUnixTime()),
             Settings::getMyColorIndex());
    uint32_t ts = WifiManager::getUnixTime();
    Presence::buildOwnPresencePayload(job.online, sizeof(job.online), "ONLINE", ts);
    if (xQueueSend(g_jobQueue, &job, 0) != pdTRUE) return;  // queue full: next tick
    c.jobInFlight = true;
    c.jobGeneration = job.generation;
    c.onlineCrc = Crc32::computeStr(job.online);
    c.onlineTs = ts;
  }
}

void adopt(GroupClient& c, const Result& r) {
  c.conn = r.conn;
  c.ready = true;
  c.consecutiveFailures = 0;
  c.nextAttemptMs = deadlineAfter(millis(), kReadyAntiThrashMs);  // anti-thrash window if Ready is lost
  // This device's presence changed while the worker was connecting (a
  // rename, radio availability): publish the current one now.
  char current[Presence::kPresencePayloadMax];
  Presence::buildOwnPresencePayload(current, sizeof(current), "ONLINE", c.onlineTs);
  if (Crc32::computeStr(current) != c.onlineCrc) Presence::publishOnline(c.group_code);
}

void handleSetupResult(const Result& r) {
  GroupClient& c = g_clients[r.slot];
  // Only the slot's own outstanding job's result releases it (a slot never
  // has two, and is never freed or reused while one is out); anything else
  // is closed and otherwise ignored.
  if (!c.jobInFlight || r.generation != c.jobGeneration) {
    if (r.ok) disposeConn(r.conn, /*publishOffline=*/false);
    return;
  }
  c.jobInFlight = false;
  bool current = c.state == SlotState::kActive && r.generation == g_slotGeneration[r.slot].load();
  if (r.ok) {
    if (current && c.conn == nullptr && !g_maintenanceMode && WifiManager::isConnected() && r.conn->mqtt.connected()) {
      adopt(c, r);
      return;
    }
    if (c.state == SlotState::kDraining && c.conn == nullptr) {
      c.conn = r.conn;  // the group was deleted meanwhile: its cleanup job publishes OFFLINE and closes it
      if (g_maintenanceMode) retireDrainingConnForMaintenance(c);
      return;
    }
    disposeConn(r.conn, /*publishOffline=*/g_maintenanceMode);
    if (c.state != SlotState::kActive) return;
    if (!current) {
      c.nextAttemptMs = 0;  // canceled (Wi-Fi recovery): try again as soon as allowed
      return;
    }
    // Current, but Wi-Fi went down or the connection died before it could be
    // adopted: a failed attempt.
    if (c.consecutiveFailures < 255) c.consecutiveFailures++;
    c.nextAttemptMs = deadlineAfter(millis(), retryDelayMs(c.consecutiveFailures));
    return;
  }
  if (c.state != SlotState::kActive) return;
  if (!current || r.failedAt == Stage::kCanceled) {
    c.nextAttemptMs = 0;
    return;
  }
  if (c.consecutiveFailures < 255) c.consecutiveFailures++;
  c.nextAttemptMs = deadlineAfter(millis(), retryDelayMs(c.consecutiveFailures));
  char ip[16];
  formatIp(r.endpoint, ip, sizeof(ip));
  Serial.printf("[MQTT] setup group=%s failed stage=%s ip=%s lastError=%d returnCode=%d failures=%u retryInMs=%lu\n",
                c.group_code, stageName(r.failedAt), ip, r.lastError, r.returnCode,
                static_cast<unsigned>(c.consecutiveFailures),
                static_cast<unsigned long>(c.nextAttemptMs - millis()));
}

// The lent connection is back: adopted again if the group is still current
// and it is still open; kept for the cleanup if the group was deleted
// meanwhile; otherwise closed (a failed publish already closed it: a
// Ready-loss, retried after the anti-thrash window stamped at Ready). The
// caller's callback runs last, once.
void handlePublishResult(const Result& r) {
  GroupClient& c = g_clients[r.slot];
  if (!isLent(c)) {  // cannot happen: every publish job belongs to its slot's loan
    Serial.printf("[mqtt] publish result for slot %u without a loan (ignored)\n", static_cast<unsigned>(r.slot));
    if (r.done != nullptr) r.done(r.ticket, r.ok);
    return;
  }
  c.lentPublishes--;
  if (c.lentPublishes == 0) {  // the chain's last result: the connection is the main task's again
    Conn* conn = c.lentConn;
    c.lentConn = nullptr;
    bool current = c.state == SlotState::kActive && c.lentGeneration == g_slotGeneration[r.slot].load() &&
                   !g_maintenanceMode && WifiManager::isConnected();
    if (current && conn != nullptr && conn->mqtt.connected()) {
      c.conn = conn;
    } else if (c.state == SlotState::kDraining && c.conn == nullptr) {
      c.conn = conn;  // deleted meanwhile: its cleanup publishes OFFLINE and closes it
      if (g_maintenanceMode) retireDrainingConnForMaintenance(c);
    } else {
      disposeConn(conn, /*publishOffline=*/g_maintenanceMode);
      if (c.state == SlotState::kActive) {
        c.ready = false;
        if (!current) c.nextAttemptMs = 0;  // canceled meanwhile (Wi-Fi recovery, maintenance): as soon as allowed
      }
    }
  }
  if (r.done != nullptr) r.done(r.ticket, r.ok);
}

void processWorkerResults() {
  if (g_resultQueue == nullptr) return;
  Result r;
  while (xQueueReceive(g_resultQueue, &r, 0) == pdTRUE) {
    if (r.type == JobType::kSetup) handleSetupResult(r);
    else if (r.type == JobType::kCleanup) handleCleanupResult(r);
    else handlePublishResult(r);
  }
}

// Wi-Fi came back: every adopted connection died with the link, and any job
// in flight began before or during the outage. Close the former, cancel the
// latter, and attempt again at once.
void onWifiRecovered() {
  for (auto& c : g_clients) {
    if (c.state != SlotState::kActive) continue;
    if (c.conn != nullptr) dropAdoptedConn(c, /*publishOffline=*/false);
    if (c.jobInFlight || isLent(c)) bumpGeneration(slotIndex(c));  // a lent connection comes back stale: closed
    c.consecutiveFailures = 0;
    c.nextAttemptMs = 0;
  }
}

void onSettingsChanged(const SettingsChangeInfo& info) {
  switch (info.event) {
    case SET_GROUP_ADDED: {
      uint8_t n = Settings::getGroupCount();
      if (info.index < n) {
        Settings::FamilyGroup g = Settings::getGroup(info.index);
        findOrCreateClientSlot(g.code);  // the worker connects it when the service tick queues its setup
      }
      break;
    }
    case SET_GROUP_DELETED: {
      // Group deletion (Addendum section 5) never waits on the network
      // here: the group's slot is retired at once (no publish, no
      // reconnect, no dispatch for it from now on) and its cleanup runs on
      // the worker -- after its setup job, if one is in flight.
      GroupClient* c = findClientSlot(info.group_code);
      if (c == nullptr) break;
      bumpGeneration(slotIndex(*c));
      c->ready = false;
      c->state = SlotState::kDraining;  // keeps c->conn for the cleanup job
      c->nextAttemptMs = 0;              // from now on: the cleanup's retry time
      c->cleanupAttempts = 0;
      postPendingCleanups();
      break;
    }
    case SET_MY_NAME_CHANGED:
      Presence::republishOwnPresenceAllGroups();
      break;
    default:
      break;
  }
}

// Bounded best-effort OFFLINE publish across every adopted connection.
// Shared by the BeforeSleep hook and OTA Maintenance Mode entry (section 19:
// "do not block OTA indefinitely if Presence publish fails").
void publishOfflineAllGroupsBounded() {
  uint32_t start = millis();
  for (auto& gc : g_clients) {
    if (millis() - start >= 2000) break;
    if (gc.state == SlotState::kActive && gc.conn != nullptr && gc.conn->mqtt.connected()) {
      Presence::publishOffline(gc.group_code);
    }
  }
}

// Deep sleep follows these hooks unconditionally (wake is a power cycle): no
// new setup step may start, and any setup in flight stops before its ONLINE
// publish.
void onBeforeSleep() {
  publishOfflineAllGroupsBounded();
  g_workerSuspended.store(true);
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) bumpGeneration(i);
}

// The group's connection is up: adopted and open, or lent to the worker for
// one publish (it was open when lent; its result says otherwise if not).
bool linkUp(GroupClient& gc) {
  return gc.state == SlotState::kActive && ((gc.conn != nullptr && gc.conn->mqtt.connected()) || isLent(gc));
}

uint8_t connectivityStatusProvider() {
  if (!WifiManager::isConnected()) return CONN_OFFLINE;
  for (auto& gc : g_clients) {
    if (linkUp(gc)) return CONN_ONLINE;
  }
  return CONN_WIFI_ONLY;
}

void serviceInit() {
  ensureSlotsForConfiguredGroups();
  ensureWorker();
  registerSettingsChangeHook(onSettingsChanged);
  registerBeforeSleepHook(onBeforeSleep);
  registerConnectivityStatusProvider(connectivityStatusProvider);
}

// ---- Diagnostics for the qos0 publish path (Fix Phase 1E: instrumentation
// only, no behavior change) ---------------------------------------------
// Bounded, RAM-only aggregate -- no dynamic allocation, no payload content.
// Covers every publishBinary() call that takes the qos0 (i.e. non-qos1)
// path below: radio/busy, radio/busy_reply, radio/session and radio/audio*
// ALL use qos=0, so this is NOT audio-specific -- control packets (CLAIM/
// RELEASE/GRANT/DENY/SESSION_START/SESSION_ACK) share this exact path and
// this exact counter. Only the actual gc->mqtt->publish() call itself is
// timed/counted; the shared connected() guard inside publishBinary() below
// (common to both the qos1 and qos0 paths) returns before either path is
// reached and is intentionally NOT counted here as a distinct "refusal"
// bucket -- doing so
// would require restructuring that shared guard, which is out of this
// round's scope. A window's summary is emitted at most once per second,
// only when the window contains a slow call (>=20ms, the SAME threshold
// the existing qos1 PERF line already uses) or an actual publish()
// failure; fast, healthy qos0 traffic stays completely silent.
//
// This is a SEPARATE measurement from radio_audio.cpp's own
// callbackElapsed timing: when this call happens from inside a RadioAudio
// capture callback, that callback's own elapsed time (recorded in
// radio_audio.cpp) already CONTAINS this call's elapsed time -- the two
// must never be added together when reading both logs.
//
// Declared here, ahead of serviceTick(), specifically so serviceTick() can
// call flushQos0DiagIfDue() directly below (Fix Phase 1E, corrected) --
// both live in this file's same top-level anonymous namespace.
struct Qos0Diag {
  uint32_t windowStartMs = 0;
  uint32_t attempted = 0;
  uint32_t succeeded = 0;
  uint32_t failed = 0;
  uint32_t elapsedSumMs = 0;
  uint32_t elapsedMaxMs = 0;
  int lastFailErr = 0;
  bool lastFailConnectedAfter = false;
  bool notable = false;
};
Qos0Diag g_qos0Diag;

constexpr uint32_t kQos0DiagWindowMs = 1000;
constexpr uint32_t kQos0DiagSlowMs = 20;  // matches publishBinary()'s existing qos1 PERF threshold elsewhere in this file

void recordQos0Publish(uint32_t elapsedMs, bool result, int lastErr, bool connectedAfter) {
  g_qos0Diag.attempted++;
  if (result) {
    g_qos0Diag.succeeded++;
  } else {
    g_qos0Diag.failed++;
    g_qos0Diag.lastFailErr = lastErr;
    g_qos0Diag.lastFailConnectedAfter = connectedAfter;
  }
  g_qos0Diag.elapsedSumMs += elapsedMs;
  if (elapsedMs > g_qos0Diag.elapsedMaxMs) g_qos0Diag.elapsedMaxMs = elapsedMs;
  if (elapsedMs >= kQos0DiagSlowMs || !result) g_qos0Diag.notable = true;
}

// Rollover-safe: plain uint32_t subtraction, same idiom as
// drainReceiveQueueCooperative() elsewhere in this file. Checked from TWO
// places (Fix Phase 1E, corrected): opportunistically at the end of each
// actual qos0 publish() call, so fast, back-to-back traffic doesn't wait an
// extra tick once due; AND unconditionally at the very top of serviceTick()
// on every tick, before its maintenance/WiFi early returns -- that second
// call site is what guarantees a pending notable window is still flushed
// once due even if no further qos0 publish() ever occurs (e.g. a single
// slow or failing call with no follow-up traffic). Both call sites are
// harmless no-ops when the window isn't due yet, and neither performs any
// network operation or extra polling -- only millis() and this in-RAM
// state are read. The one-second rate limit and window-reset behavior are
// unchanged regardless of which call site triggers the flush.
void flushQos0DiagIfDue(uint32_t nowMs) {
  uint32_t windowElapsedMs = nowMs - g_qos0Diag.windowStartMs;
  if (windowElapsedMs < kQos0DiagWindowMs) return;
  if (g_qos0Diag.notable) {
    Serial.printf(
        "[DIAG][MQTT] qos0 tMs=%lu windowMs=%lu attempted=%lu succeeded=%lu failed=%lu sumMs=%lu maxMs=%lu "
        "lastFailErr=%d lastFailConnAfter=%d\n",
        static_cast<unsigned long>(nowMs), static_cast<unsigned long>(windowElapsedMs),
        static_cast<unsigned long>(g_qos0Diag.attempted), static_cast<unsigned long>(g_qos0Diag.succeeded),
        static_cast<unsigned long>(g_qos0Diag.failed), static_cast<unsigned long>(g_qos0Diag.elapsedSumMs),
        static_cast<unsigned long>(g_qos0Diag.elapsedMaxMs), g_qos0Diag.lastFailErr,
        g_qos0Diag.lastFailConnectedAfter ? 1 : 0);
  }
  g_qos0Diag = Qos0Diag{};
  g_qos0Diag.windowStartMs = nowMs;
}

void serviceTick() {
  // Fix Phase 1E (corrected): flush a pending notable qos0 diagnostic
  // window unconditionally, before any early return below.
  flushQos0DiagIfDue(static_cast<uint32_t>(millis()));
  ensureWorker();
  // Results first, every tick and in every mode: they release the slot's
  // outstanding job and carry connections that must be adopted or closed.
  processWorkerResults();
  if (g_maintenanceMode) return;  // Phase 5 OTA: reconnect/receive paused

  bool wifiConnected = WifiManager::isConnected();
  if (wifiConnected && !g_wifiWasConnected) onWifiRecovered();
  g_wifiWasConnected = wifiConnected;
  ensureSlotsForConfiguredGroups();
  postPendingCleanups();
  if (!wifiConnected) return;  // never block; just wait until WiFi is up

  // Adopted connections: the library's own servicing (keepalive, incoming
  // packets). loop() only reads bytes already available, waiting at most
  // kMqttCommandTimeoutMs for the rest of a partly received packet, and
  // never waits for a PINGRESP; its writes are bounded by kSendBudgetMs.
  // A connection found closed -- by this loop() or by any earlier publish
  // from another AppService -- is freed here, and the group is attempted
  // again after its anti-thrash window.
  for (auto& gc : g_clients) {
    if (gc.state != SlotState::kActive || gc.conn == nullptr) continue;
    uint32_t loopStart = millis();
    bool loopOk = gc.conn->mqtt.loop();
    uint32_t loopElapsed = millis() - loopStart;
    bool connectedAfter = gc.conn->mqtt.connected();
    if (!loopOk || !connectedAfter) {
      Serial.printf("[PERF][MQTT] loop-error group=%s %lu ms loopResult=%d connectedAfter=%d lastError=%d\n",
                    gc.group_code, static_cast<unsigned long>(loopElapsed), loopOk ? 1 : 0, connectedAfter ? 1 : 0,
                    static_cast<int>(gc.conn->mqtt.lastError()));
    } else if (loopElapsed >= 20) {
      Serial.printf("[PERF][MQTT] loop group=%s %lu ms connected=1\n", gc.group_code,
                    static_cast<unsigned long>(loopElapsed));
    }
    if (!connectedAfter) dropAdoptedConn(gc, /*publishOffline=*/false);  // nextAttemptMs: the window stamped at Ready
  }

  postDueSetupJobs(static_cast<uint32_t>(millis()));

  // Fix Phase 1D: the normal, cooperative (bounded) drain.
  drainReceiveQueueCooperative();
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

// TRANSPORT-ONLY meaning, unchanged: true while this group's connection is up
// and usable by the main task for a bare outbound publish (publishRaw()/
// publishBinary() only ever need the wire to be up). Timing note (MQTT fix):
// the main task first sees a connection when the connection worker hands it
// over, which is after setup has completed, so this now turns true together
// with isGroupReady() instead of a few setup steps earlier. Do NOT use this
// to decide whether an action that depends on RECEIVING something addressed
// to this device is safe -- see isGroupReady() below for that.
bool isGroupConnected(const char* group_code) {
  GroupClient* gc = findClientSlot(group_code);
  return gc != nullptr && linkUp(*gc);
}

bool isAnyGroupConnected() {
  for (auto& gc : g_clients) {
    if (linkUp(gc)) return true;
  }
  return false;
}

// Fix Phase 1C, unchanged contract: STRICTLY STRONGER than isGroupConnected()
// -- true only once transport is connected AND all 10 subscriptions
// succeeded AND the ONLINE presence publish was acknowledged with the
// connection still up, and never while Wi-Fi is down (read-only check, so
// a stale flag during an outage cannot answer true). Audited callers that
// need it: RadioTransport::startPrivateCall() and its BUSY_CLAIM branch,
// Presence::republishOwnPresenceAllGroups(), and the Race room engine
// (roomReady() in race.cpp). Chat/game sends and the Outbox flush only need
// isGroupConnected().
bool isGroupReady(const char* group_code) {
  if (!WifiManager::isConnected()) return false;
  GroupClient* gc = findClientSlot(group_code);
  return gc != nullptr && linkUp(*gc) && gc->ready;
}

bool isAnyGroupReady() {
  if (!WifiManager::isConnected()) return false;
  for (auto& gc : g_clients) {
    if (linkUp(gc) && gc.ready) return true;
  }
  return false;
}

void setMaintenanceModeActive(bool active) {
  if (active == g_maintenanceMode) return;
  if (active) {
    // Set the flags first: no new setup step starts on the worker, and the
    // service tick stops reconnecting/receiving. Then drain what is already
    // queued ourselves (Fix Phase 1D: the FULL drain, so nothing queued is
    // discarded), with the defensive release kept as a leak-proof fallback.
    g_maintenanceMode = true;
    g_workerSuspended.store(true);
    drainReceiveQueueFull();
    uint8_t left = queueCount();
    if (left != 0) {
      Serial.printf("[mqtt] maintenance mode: %u receive-queue payload(s) left after drain; forcing release\n",
                    static_cast<unsigned>(left));
      releaseQueuedPayloads();
    }

    publishOfflineAllGroupsBounded();
    // Every connection is closed and every setup in flight canceled (its
    // result, whenever it arrives, is closed rather than adopted). A
    // deleted group's connection publishes OFFLINE and its record clear
    // here; its broker session cleanup runs after maintenance.
    for (auto& gc : g_clients) {
      if (gc.state == SlotState::kFree) continue;
      bumpGeneration(slotIndex(gc));
      if (gc.state == SlotState::kDraining) retireDrainingConnForMaintenance(gc);
      if (gc.conn != nullptr) dropAdoptedConn(gc, /*publishOffline=*/false);
      gc.nextAttemptMs = 0;
    }
  } else {
    // Reconnect exactly like a Wi-Fi recovery: at once.
    g_workerSuspended.store(false);
    g_maintenanceMode = false;
  }
}

// See mqtt_manager.h. Main task only. Posting copies the payload; nothing
// here waits on the network.
uint32_t publishBinaryAsync(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                            bool retained, int qos, PublishDoneFn done) {
  static uint32_t s_nextTicket = 0;
  GroupClient* gc = findClientSlot(group_code);
  if (gc == nullptr || gc->state != SlotState::kActive) return 0;
  if (g_jobQueue == nullptr || g_workerTask == nullptr || g_maintenanceMode) return 0;
  if (strlen(topic_suffix) >= sizeof(Job().suffix)) return 0;
  uint8_t slot = slotIndex(*gc);
  Conn* conn;
  if (isLent(*gc)) {
    // Joins the loan: published after the ones already queued, on the same
    // connection (only the worker touches it until the chain's last result).
    if (gc->lentPublishes >= kMaxLentPublishes) return 0;
    if (gc->lentGeneration != g_slotGeneration[slot].load()) return 0;  // canceled loan: comes back closed
    conn = gc->lentConn;
  } else {
    if (gc->conn == nullptr || !gc->conn->mqtt.connected()) return 0;
    conn = gc->conn;
  }
  uint8_t* copy = new (std::nothrow) uint8_t[len > 0 ? len : 1];
  if (copy == nullptr) return 0;
  if (len > 0) memcpy(copy, data, len);
  if (++s_nextTicket == 0) s_nextTicket = 1;
  Job job;
  memset(&job, 0, sizeof(job));
  job.type = JobType::kPublish;
  job.slot = slot;
  job.generation = g_slotGeneration[slot].load();
  strncpy(job.group_code, gc->group_code, sizeof(job.group_code) - 1);
  strncpy(job.suffix, topic_suffix, sizeof(job.suffix) - 1);
  job.payload = copy;
  job.payloadLen = len;
  job.retained = retained;
  job.qos = static_cast<uint8_t>(qos);
  job.ticket = s_nextTicket;
  job.done = done;
  job.conn = conn;
  if (xQueueSend(g_jobQueue, &job, 0) != pdTRUE) {  // bounded: the queue is full, nothing waits
    delete[] copy;
    return 0;
  }
  if (!isLent(*gc)) {
    gc->lentConn = conn;
    gc->conn = nullptr;
    gc->lentGeneration = job.generation;
  }
  gc->lentPublishes++;
  return job.ticket;
}

bool publishRaw(const char* group_code, const char* topic_suffix, const char* payload, bool retained, int qos) {
  GroupClient* gc = findClientSlot(group_code);
  // While the connection is lent, a presence update joins the worker's chain
  // instead of being lost (no caller acts on presence results: true here
  // means queued, not acknowledged).
  if (gc != nullptr && isLent(*gc)) {
    return publishBinaryAsync(group_code, topic_suffix, reinterpret_cast<const uint8_t*>(payload),
                              static_cast<uint16_t>(strlen(payload)), retained, qos, nullptr) != 0;
  }
  if (gc == nullptr || gc->conn == nullptr || !gc->conn->mqtt.connected()) return false;
  char topic[80];
  snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", group_code, topic_suffix);
  return gc->conn->mqtt.publish(topic, payload, retained, qos);
}

// Hardware Diagnostic #4.9s: QoS1 publishes, timed around the actual
// publish() call. result/lastError()/connected() are snapshotted in that
// exact order, immediately after publish() returns and before any
// Serial.printf(); connectedBefore=1 is a literal, justified by the
// connected() guard above -- no extra connected() call is added just to
// log it. returnCode() is deliberately NOT read here: it is
// connection-related, not the current publish's own error. Logged only
// on failure or when the call took >=20ms.
//
// Fix Phase 1E (corrected): the qos0 path below ("radio audio" is
// inaccurate -- radio/busy, radio/busy_reply, radio/session and
// radio/audio* control AND audio packets alike all use qos=0) previously
// had no timing/error-state reads at all, per-packet overhead being the
// reason. It now also snapshots result/lastError()/connected() per call,
// same as this qos1 block -- but logs only a bounded, rate-limited (at
// most once per second) aggregate summary, not a per-call line, so this
// remains no per-packet log overhead in the steady/healthy case. See
// Qos0Diag/recordQos0Publish()/flushQos0DiagIfDue() above.
//
// Hardware Diagnostic #4.9t: TEMPORARY synchronous experiment, not a
// non-blocking fix -- see kMqttPublishTimeoutMsExperiment's own comment. Main
// task only: an adopted connection always carries kMqttCommandTimeoutMs
// (the worker sets it at handover), and this call restores it before
// returning, so it is always entered with the normal timeout in effect.
bool publishBinary(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                   bool retained, int qos) {
  GroupClient* gc = findClientSlot(group_code);
  if (gc == nullptr || gc->conn == nullptr || !gc->conn->mqtt.connected()) return false;
  char topic[80];
  snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", group_code, topic_suffix);
  if (qos == 1) {
    gc->conn->mqtt.setTimeout(kMqttPublishTimeoutMsExperiment);
    uint32_t publishStart = millis();
    bool result = gc->conn->mqtt.publish(topic, reinterpret_cast<const char*>(data), static_cast<int>(len), retained, qos);
    uint32_t publishElapsed = millis() - publishStart;
    int lastErr = static_cast<int>(gc->conn->mqtt.lastError());
    bool connectedAfter = gc->conn->mqtt.connected();
    // Restore before any logging/return, on both success and failure.
    gc->conn->mqtt.setTimeout(kMqttCommandTimeoutMs);
    if (!result || publishElapsed >= 20) {
      Serial.printf(
          "[PERF][MQTT] publish group=%s qos=%d retained=%d bytes=%u %lu ms result=%d lastError=%d "
          "connectedBefore=1 connectedAfter=%d publishTimeoutMs=%lu\n",
          group_code, qos, retained ? 1 : 0, static_cast<unsigned>(len),
          static_cast<unsigned long>(publishElapsed), result ? 1 : 0, lastErr, connectedAfter ? 1 : 0,
          static_cast<unsigned long>(kMqttPublishTimeoutMsExperiment));
    }
    return result;
  }
  // Fix Phase 1E: qos0 path -- the actual publish() call, arguments, and
  // return value are UNCHANGED; only newly-added local variables capture
  // what's needed for the bounded, rate-limited diagnostic below. Order
  // matches the existing qos1 block immediately above: result, lastError,
  // connected() are snapshotted in that exact order, immediately after
  // publish() returns, before any recording/logging.
  uint32_t qos0Start = static_cast<uint32_t>(millis());
  bool qos0Result = gc->conn->mqtt.publish(topic, reinterpret_cast<const char*>(data), static_cast<int>(len), retained, qos);
  uint32_t qos0Elapsed = static_cast<uint32_t>(millis()) - qos0Start;
  int qos0LastErr = static_cast<int>(gc->conn->mqtt.lastError());
  bool qos0ConnectedAfter = gc->conn->mqtt.connected();
  recordQos0Publish(qos0Elapsed, qos0Result, qos0LastErr, qos0ConnectedAfter);
  flushQos0DiagIfDue(static_cast<uint32_t>(millis()));
  return qos0Result;
}

}  // namespace MqttManager
