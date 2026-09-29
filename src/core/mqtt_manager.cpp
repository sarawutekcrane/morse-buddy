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

// Fix Phase 1C: explicit per-group setup stage, so a group's 13-subscribe
// sequence can be spread across many ticks (one topic per eligible turn)
// instead of run to completion inside one blocking connectGroupIfNeeded()
// call. kNeedConnect is both the initial state and the state every
// failure/transport-loss path resets a client back to, so "a fresh attempt
// is starting" is always exactly "stage just became kNeedConnect".
enum class SetupStage : uint8_t {
  kNeedConnect,       // not connected; next eligible operation is connect()
  kSubscribing,       // connected; subscribeCursor names the next topic to subscribe
  kNeedPublishOnline, // all 13 subscriptions succeeded; next op is the ONLINE publish
  kDone,              // fully Ready (see GroupClient::ready)
};

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
  //
  // Fix Phase 1C: the staged setup functions below set this to a FRESH
  // deadline on every UNFINISHED-setup failure (connect/subscribe/
  // publishOnline), and leave it UNTOUCHED (the prior deadline) when a
  // client that had already reached Ready (see `ready` below) later loses
  // its transport -- this is a deliberate, traced behavior choice, not an
  // oversight: since a healthy connection typically stays up far longer
  // than kMqttReconnectIntervalMs before eventually dropping, reusing the
  // prior deadline is (in the common case) already in the past by the time
  // Ready is lost, so the OBSERVED effect is "reconnect attempted
  // immediately" -- except when the drop happens within
  // kMqttReconnectIntervalMs of reaching Ready, where the remainder of
  // that original window still throttles it (existing anti-thrash
  // protection, unaffected by this candidate).
  uint32_t nextReconnectAttemptMs = 0;

  // Fix Phase 1C: staged-setup bookkeeping. `ready` is the ONLY field
  // isGroupReady()/isAnyGroupReady() read (alongside a defensive
  // connected() re-check, see their definitions) -- it becomes true only
  // once every one of the sequential checks in doPublishOnlineOperation()
  // has actually passed, and is reset to false by every path that resets
  // `stage` back to kNeedConnect (setup failure, Ready-loss, WiFi-loss
  // recovery, maintenance entry).
  SetupStage stage = SetupStage::kNeedConnect;
  uint8_t subscribeCursor = 0;
  bool ready = false;

  // Fix Phase 1C: setup timing, deliberately kept as TWO separate,
  // differently-named figures -- see the final per-attempt summary log for
  // why these must never be treated as the same quantity. `attemptStartMs`
  // is stamped once, at the very first operation of a fresh attempt
  // (always inside doConnectOperation(), the only place stage transitions
  // FROM kNeedConnect); `busyMsSum` accumulates each individual
  // operation's own blocking elapsed time across however many ticks the
  // whole attempt spans, reset to 0 at that same moment.
  uint32_t setupAttemptStartMs = 0;
  uint32_t setupBusyMsSum = 0;
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

// Hardware Diagnostic #4.9t: TEMPORARY synchronous experiment, not a
// non-blocking fix -- a separate constant from kMqttSetupTimeoutMsExperiment
// above (never reused/renamed), scoped only to one QoS1 publishBinary()
// call at a time. Hardware evidence shows QoS1 binary publishes
// repeatedly returning false at ~507ms with lastError=-9 and
// connectedAfter=0, while a later publish succeeds at ~466ms -- this
// widens the wait for that one publish()'s PUBACK to see whether more
// time changes the outcome; it does not by itself prove broker fault or
// establish the exact packet-level cause. Restored to
// kMqttCommandTimeoutMs immediately after that one call, on every
// success/failure path, exactly like #4.9g's own restore pattern.
constexpr uint32_t kMqttPublishTimeoutMsExperiment = 1500;

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

// Hardware Fix #4.9d Part A3, extended by Fix Phase 1C: round-robin cursor
// so that, across consecutive ticks, every group with an eligible setup
// operation (connect due, or already connected and mid-subscribe/needs its
// ONLINE publish) eventually gets its turn -- a single persistently-broken
// group parked at a low array index can never starve the others by always
// winning the scan. Previously this only ever gated whether a group got to
// run its WHOLE atomic connectGroupIfNeeded() attempt; now it gates which
// group gets the AT-MOST-ONE setup operation performed globally this tick
// (see hasEligibleSetupOperation()/performEligibleSetupOperation() and the
// scan in serviceTick()).
uint8_t g_nextSetupCandidateIndex = 0;

// Fix Phase 1C: tracks WiFi's own connected/disconnected transitions
// (distinct from any individual group's MQTT transport) so serviceTick()
// can detect exactly the tick WiFi recovers from a drop and force every
// active client to restart its staged setup from scratch -- see the
// WiFi-recovery handling at the top of serviceTick(). Initialized true so
// the very first serviceTick() call (WiFi already up, nothing to recover
// from) never spuriously triggers that reset path.
bool g_wifiWasConnected = true;

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
// Lifecycle: onMqttMessage() allocates+copies+takes ownership on enqueue.
// Fix Phase 1A: drainReceiveQueue() now detaches a slot's contents into a
// local QueuedMessage copy and frees the original slot's ownership (data=
// nullptr, used=false) BEFORE calling processQueuedMessage() — not after.
// This struct has no user-declared destructor or copy constructor, so the
// implicit copy is a plain shallow memberwise copy (the pointer value is
// duplicated, nothing is deep-copied or auto-freed); ownership of the one
// live payload always follows whichever copy the code chooses to free —
// see drainReceiveQueue() for why the slot is relinquished first.
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
// one place. `q` is a detached local copy owned by the caller (see Fix
// Phase 1A in drainReceiveQueue()), not a live reference into g_queue[] —
// this function never frees q.data itself; the caller frees it exactly
// once after this function returns, regardless of which path was taken.
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
    // Fix Phase 1A: detach this slot's contents into a local copy, and
    // relinquish the original slot's ownership (data=nullptr, used=false)
    // BEFORE calling processQueuedMessage() — not after, as this used to
    // do by keeping a live reference into g_queue[g_queueHead] across the
    // whole dispatch. A dispatched handler can itself call
    // publishBinary()/publishRaw(), which reach the MQTT library's own
    // publish()/subscribe(); whether that library pumps a synchronous
    // socket read internally and reentrantly invokes onMqttMessage()
    // before returning is not something this file can rule out (256dpi/
    // MQTT's source is not available in this environment to verify either
    // way — see this file's own header note). g_queueCount was already
    // decremented for this slot below, so once the ring has wrapped near
    // g_queueTail == this index is reachable, and a reentrant enqueue
    // landing there while the old code still held `q` as a live reference
    // would delete[] and overwrite the very payload dispatch was using —
    // a use-after-free. Taking a full value copy and clearing the
    // original slot's fields first closes that window: whatever a
    // reentrant onMqttMessage() call does to g_queue[idx] from here on, it
    // is working with a slot this function has already relinquished, never
    // one still referenced by `local`.
    uint8_t idx = g_queueHead;
    QueuedMessage local = g_queue[idx];
    g_queue[idx].data = nullptr;
    g_queue[idx].used = false;
    g_queue[idx].len = 0;
    g_queueHead = static_cast<uint8_t>((idx + 1) % kQueueCapacity);
    g_queueCount--;

    processQueuedMessage(local);
    drainedCount++;

    // Free exactly once, from the detached local copy — never touch
    // g_queue[idx] again past this point; processQueuedMessage() above may
    // already have caused a different, still-live item to be enqueued
    // there (or wherever the tail now points), and this function must
    // never clear or free a slot it no longer owns.
    delete[] local.data;
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

// Fix Phase 1C: the 13-topic list, in the SAME order and with the SAME QoS
// as before this candidate -- only WHEN each is attempted changes (staged,
// one per eligible tick, via doSubscribeOperation() below, not all 13
// back-to-back inside one blocking call). Built once (Identity::deviceId()
// is immutable for the device's lifetime after Identity::init()) rather
// than reconstructed on every subscribe operation.
struct TopicSpec {
  char suffix[32];
  int qos;
};
constexpr size_t kTopicCount = 13;
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
    char audioSuffix[24];
    snprintf(audioSuffix, sizeof(audioSuffix), "radio/audio/%s", dev);  // truncated by 1 byte, reproduced as-is

    snprintf(specs[0].suffix, sizeof(specs[0].suffix), "presence/+");
    specs[0].qos = 1;
    snprintf(specs[1].suffix, sizeof(specs[1].suffix), "%s", msgSuffix);
    specs[1].qos = 1;
    snprintf(specs[2].suffix, sizeof(specs[2].suffix), "broadcast");
    specs[2].qos = 1;
    snprintf(specs[3].suffix, sizeof(specs[3].suffix), "race/invite");
    specs[3].qos = 1;
    snprintf(specs[4].suffix, sizeof(specs[4].suffix), "race/join");
    specs[4].qos = 1;
    snprintf(specs[5].suffix, sizeof(specs[5].suffix), "race/round");
    specs[5].qos = 1;
    snprintf(specs[6].suffix, sizeof(specs[6].suffix), "race/solved");
    specs[6].qos = 1;
    snprintf(specs[7].suffix, sizeof(specs[7].suffix), "race/reset");
    specs[7].qos = 1;
    snprintf(specs[8].suffix, sizeof(specs[8].suffix), "%s", busySuffix);
    specs[8].qos = 0;
    snprintf(specs[9].suffix, sizeof(specs[9].suffix), "%s", busyReplySuffix);
    specs[9].qos = 0;
    snprintf(specs[10].suffix, sizeof(specs[10].suffix), "radio/session");
    specs[10].qos = 0;
    snprintf(specs[11].suffix, sizeof(specs[11].suffix), "%s", audioSuffix);
    specs[11].qos = 0;
    snprintf(specs[12].suffix, sizeof(specs[12].suffix), "radio/audio/broadcast");
    specs[12].qos = 0;
    built = true;
  }
  return specs;
}

// Fix Phase 1C: single place every UNFINISHED-setup failure (connect fails;
// any one of the 13 subscribes fails; publishOnline() returns false or the
// connection is no longer up afterward) funnels through. Closes the same
// gap Hardware Fix #4.9f's cooldown logic closed for the old atomic
// connectGroupIfNeeded() -- explicitly disconnect()s if the transport is
// still (or again) live so a failed attempt never leaves a connected-but-
// not-Ready client stuck, then a FRESH kMqttReconnectIntervalMs cooldown
// from THIS failure moment, distinct from Ready-loss (see the
// mqtt->loop() servicing section of serviceTick(), which preserves the
// PRIOR deadline instead -- gc.ready is what distinguishes the two cases).
void handleSetupFailure(GroupClient& gc, const char* stage) {
  if (gc.mqtt->connected()) gc.mqtt->disconnect();
  gc.ready = false;
  gc.stage = SetupStage::kNeedConnect;
  gc.subscribeCursor = 0;
  gc.nextReconnectAttemptMs = computeNextReconnectDeadline(millis());

  uint32_t wallMs = millis() - gc.setupAttemptStartMs;
  Serial.printf(
      "[PERF][MQTT] setup group=%s result=0 stage=%s connected=%d nextRetryMs=%lu wallMs=%lu busyMsSum=%lu "
      "setupTimeoutMs=%lu\n",
      gc.group_code, stage, gc.mqtt->connected() ? 1 : 0, static_cast<unsigned long>(gc.nextReconnectAttemptMs),
      static_cast<unsigned long>(wallMs), static_cast<unsigned long>(gc.setupBusyMsSum),
      static_cast<unsigned long>(kMqttSetupTimeoutMsExperiment));
}

// One of at most one setup operation performed globally per serviceTick()
// call (see the round-robin scan in serviceTick()). Only entered when
// gc.stage == kNeedConnect and the per-group cooldown has elapsed.
//
// Hardware Fix #4.9d Part A2 / #4.9f / #4.9g's reasoning for the throttle,
// the will payload shape, and the 500ms-vs-1500ms timeout all carry over
// unchanged from the old connectGroupIfNeeded() -- only the granularity
// (this is now ONE operation of a multi-tick attempt, not the whole
// attempt) is new.
void doConnectOperation(GroupClient& gc) {
  // Fresh attempt starts here -- the only place stage transitions FROM
  // kNeedConnect (every failure/loss path resets it back to kNeedConnect),
  // so wall/busy timing for a NEW attempt always resets exactly once, here.
  gc.setupAttemptStartMs = millis();
  gc.setupBusyMsSum = 0;

  char willTopic[48];
  snprintf(willTopic, sizeof(willTopic), "morsebuddy/%s/presence/%s", gc.group_code, Identity::deviceId());
  char willPayload[110];
  // Feature Fix #4.8 section 2I: append the same optional trailing color
  // index Presence::buildPayload() now sends on every ONLINE/OFFLINE
  // publish, so a peer that only ever sees this device via its LWT (an
  // unclean disconnect) still resolves its personal color the same way.
  snprintf(willPayload, sizeof(willPayload), "MBP1|%s|%s|OFFLINE|0|%lu|%u", Identity::deviceId(),
           Settings::getMyName(), static_cast<unsigned long>(WifiManager::getUnixTime()),
           Settings::getMyColorIndex());
  gc.mqtt->setWill(willTopic, willPayload, /*retained=*/true, /*qos=*/1);

  // Fix Phase 1C: timeout is scoped to THIS ONE connect() call, then
  // restored to kMqttCommandTimeoutMs immediately after -- before
  // returning to any other application work this tick -- on both outcomes
  // below. setOptions() (not setTimeout()) is used here, exactly as the
  // old code did, since keepAlive/cleanSession must also be (re)applied
  // for a connect attempt; subscribe/publishOnline operations below use
  // the narrower setTimeout()-only widen/restore instead.
  gc.mqtt->setOptions(/*keepAlive=*/20, /*cleanSession=*/false, /*timeout=*/kMqttSetupTimeoutMsExperiment);

  uint32_t connectStart = millis();
  bool connected = gc.mqtt->connect(gc.client_id, nullptr, nullptr);
  uint32_t connectElapsed = millis() - connectStart;
  gc.mqtt->setTimeout(kMqttCommandTimeoutMs);
  gc.setupBusyMsSum += connectElapsed;

  Serial.printf("[PERF][MQTT] connect group=%s startMs=%lu %lu ms result=%d lastError=%d returnCode=%d\n",
                gc.group_code, static_cast<unsigned long>(connectStart),
                static_cast<unsigned long>(connectElapsed), connected ? 1 : 0,
                static_cast<int>(gc.mqtt->lastError()), static_cast<int>(gc.mqtt->returnCode()));

  if (connected) {
    gc.stage = SetupStage::kSubscribing;
    gc.subscribeCursor = 0;
  } else {
    handleSetupFailure(gc, "connect");
  }
}

// One subscribe per eligible turn, at gc.subscribeCursor. Stops the whole
// attempt at the first failure (Hardware Fix #4.9f Part B's behavior,
// unchanged) via handleSetupFailure(); on success, advances the cursor and
// transitions to kNeedPublishOnline once all 13 have succeeded.
void doSubscribeOperation(GroupClient& gc) {
  const TopicSpec* specs = getTopicSpecs();
  const TopicSpec& spec = specs[gc.subscribeCursor];
  char topic[80];
  snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", gc.group_code, spec.suffix);

  // Fix Phase 1C: scoped to this one subscribe() call via setTimeout()
  // only (keepAlive/cleanSession already set by the connect operation
  // above and never need re-applying here) -- restored immediately after,
  // before returning to other application work this tick, on both
  // outcomes.
  gc.mqtt->setTimeout(kMqttSetupTimeoutMsExperiment);
  uint32_t t0 = millis();
  bool ok = gc.mqtt->subscribe(topic, spec.qos);
  uint32_t elapsed = millis() - t0;
  gc.mqtt->setTimeout(kMqttCommandTimeoutMs);
  gc.setupBusyMsSum += elapsed;

  if (!ok || elapsed >= 20) {
    Serial.printf("[PERF][MQTT] subscribe group=%s topic=%s %lu ms result=%d lastError=%d connected=%d\n",
                  gc.group_code, spec.suffix, static_cast<unsigned long>(elapsed), ok ? 1 : 0,
                  static_cast<int>(gc.mqtt->lastError()), gc.mqtt->connected() ? 1 : 0);
  }

  if (!ok) {
    handleSetupFailure(gc, "subscribe");
    return;
  }
  gc.subscribeCursor++;
  if (gc.subscribeCursor >= kTopicCount) {
    gc.stage = SetupStage::kNeedPublishOnline;
  }
}

// Final setup operation: the ONLINE presence publish. Setup only counts as
// complete once publishOnline() itself reports true AND the connection is
// still live afterward (Fix Phase 1B's propagated bool closes the exact
// gap Hardware Fix #4.9f Part B could not -- "connected() after" alone
// never proved the publish itself succeeded).
void doPublishOnlineOperation(GroupClient& gc) {
  gc.mqtt->setTimeout(kMqttSetupTimeoutMsExperiment);
  uint32_t t0 = millis();
  bool publishOk = Presence::publishOnline(gc.group_code);
  uint32_t elapsed = millis() - t0;
  gc.mqtt->setTimeout(kMqttCommandTimeoutMs);
  gc.setupBusyMsSum += elapsed;

  bool connectedAfter = gc.mqtt->connected();
  bool setupOk = publishOk && connectedAfter;

  if (!setupOk || elapsed >= 20) {
    Serial.printf("[PERF][MQTT] publishOnline group=%s %lu ms result=%d connected=%d\n", gc.group_code,
                  static_cast<unsigned long>(elapsed), publishOk ? 1 : 0, connectedAfter ? 1 : 0);
  }

  if (setupOk) {
    gc.stage = SetupStage::kDone;
    gc.ready = true;
    // Fix Phase 1C (corrected): stamp a FRESH cooldown deadline at the
    // moment Ready is actually reached, rather than leaving
    // nextReconnectAttemptMs untouched (whatever it happened to be from
    // an earlier failure, or its 0 "never attempted" sentinel on a
    // first-ever success). This is what reconcileClientConnectivity()
    // later PRESERVES on Ready-loss -- making "preserve the prior
    // deadline" a deliberate, well-defined anti-thrash window measured
    // from when Ready was reached, not an accidental inheritance from an
    // unrelated earlier event (or, worse, the 0 sentinel being
    // "preserved" only by coincidence). Uses the same
    // computeNextReconnectDeadline() helper as every other deadline
    // assignment, so the zero-sentinel/rollover-safety guarantees it
    // already provides carry over unchanged.
    gc.nextReconnectAttemptMs = computeNextReconnectDeadline(millis());
    // Fix Phase 1C: wallMs (spans every tick from the first connect
    // attempt to this completion, INCLUDES inter-tick gaps spent servicing
    // other groups/application work) vs busyMsSum (the running total of
    // each operation's own blocking elapsed -- comparable to the old
    // atomic connectGroupIfNeeded()'s single subscribeAllElapsed-style
    // number) are deliberately different, differently-named quantities:
    // never conflate them when reading this log.
    uint32_t wallMs = millis() - gc.setupAttemptStartMs;
    Serial.printf(
        "[PERF][MQTT] setup group=%s result=1 stage=none connected=1 nextRetryMs=%lu wallMs=%lu busyMsSum=%lu "
        "setupTimeoutMs=%lu\n",
        gc.group_code, static_cast<unsigned long>(gc.nextReconnectAttemptMs), static_cast<unsigned long>(wallMs),
        static_cast<unsigned long>(gc.setupBusyMsSum), static_cast<unsigned long>(kMqttSetupTimeoutMsExperiment));
  } else {
    handleSetupFailure(gc, "publishOnline");
  }
}

// Fix Phase 1C (corrected): reconciles a client whose transport is
// currently disconnected but whose staged-setup bookkeeping still reflects
// an earlier connected state (kSubscribing/kNeedPublishOnline/kDone, or
// ready=true). Checks the CURRENT connected() state directly, not a
// before/after snapshot taken only around one tick's own mqtt->loop()
// call: an earlier revision of this candidate only reconciled a client
// when `connectedBefore && !connectedAfter` was observed DURING this
// exact tick's loop() call. That missed any disconnection that had
// already happened BEFORE this tick started -- e.g. an application
// publish() from a completely different AppService's tick, earlier in the
// same main loop() iteration or a previous one, that caused the library
// to close the connection -- because connectedBefore would already read
// false in that case, so the old guard never fired at all. A client left
// in kDone/kSubscribing/kNeedPublishOnline while actually disconnected was
// then PERMANENTLY stuck there: hasEligibleSetupOperation() excludes
// kDone unconditionally, and requires connected()==true for the other two
// staged-in-progress stages, so nothing would ever make it eligible to
// retry again.
//
// The `gc.stage != SetupStage::kNeedConnect` guard applies the loss
// transition EXACTLY ONCE: once reconciled to kNeedConnect, a client that
// remains disconnected on every subsequent tick already reads
// stage==kNeedConnect, so this function correctly does nothing further on
// those later ticks -- it never re-triggers a fresh cooldown, and never
// re-"preserves" an already-preserved one, merely because the tick ran
// again while still disconnected.
void reconcileClientConnectivity(GroupClient& gc) {
  if (gc.mqtt->connected()) return;
  if (gc.stage == SetupStage::kNeedConnect) return;  // already idle; apply-once guard
  bool wasReady = gc.ready;
  gc.ready = false;
  gc.stage = SetupStage::kNeedConnect;
  gc.subscribeCursor = 0;
  // Fix Phase 1C (corrected): a client that reached Ready always has a
  // deadline explicitly (re-)stamped at that moment (see
  // doPublishOnlineOperation()) -- preserving it here reuses that
  // freshly-established anti-thrash window, not a stale or accidental
  // value. A client that never finished setup gets a fresh one from this
  // failure moment, same as handleSetupFailure().
  if (!wasReady) gc.nextReconnectAttemptMs = computeNextReconnectDeadline(millis());
}

// Fix Phase 1C: true iff `gc` has an operation eligible to run THIS tick --
// either it's due for a (re)connect attempt (same per-group cooldown
// throttle as before), or it's already connected and mid-subscribe/needs
// its ONLINE publish. reconcileClientConnectivity() (called from
// serviceTick() before this scan runs, and from handleSetupFailure() for a
// loss detected mid-operation) guarantees a disconnected client always
// reads stage==kNeedConnect by the time this runs, so this one check
// covers every stage uniformly.
bool hasEligibleSetupOperation(const GroupClient& gc, uint32_t now) {
  if (!gc.active || gc.mqtt == nullptr) return false;
  if (gc.stage == SetupStage::kDone) return false;
  if (gc.stage == SetupStage::kNeedConnect) {
    if (gc.mqtt->connected()) return false;  // defensive: loop() section reconciles this before we'd see it
    return gc.nextReconnectAttemptMs == 0 || static_cast<int32_t>(now - gc.nextReconnectAttemptMs) >= 0;
  }
  return gc.mqtt->connected();  // kSubscribing / kNeedPublishOnline: only while transport is actually still up
}

// Performs exactly the one operation `gc.stage` currently calls for.
void performEligibleSetupOperation(GroupClient& gc) {
  switch (gc.stage) {
    case SetupStage::kNeedConnect:
      doConnectOperation(gc);
      break;
    case SetupStage::kSubscribing:
      doSubscribeOperation(gc);
      break;
    case SetupStage::kNeedPublishOnline:
      doPublishOnlineOperation(gc);
      break;
    case SetupStage::kDone:
      break;  // unreachable via hasEligibleSetupOperation()'s guard
  }
}

// Fix Phase 1C: this is the single choke point where a `g_clients[]` array
// slot is given a NEW identity, whether it was never used before or is
// being reused after a prior group's destroyClientForGroup() freed it --
// every staged-setup field is explicitly reset here, closing a pre-
// existing latent gap: destroyClientForGroup() never reset
// nextReconnectAttemptMs, so a slot reused before that deadline passed
// could throttle a brand-new group's very first connect attempt. Resetting
// it (and stage/subscribeCursor/ready/the timing fields) here, rather than
// at deletion time, covers every path that can hand a slot a new identity,
// not just deletion-then-reuse.
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
      c.nextReconnectAttemptMs = 0;  // eligible immediately, not throttled by a stale prior occupant's deadline
      c.stage = SetupStage::kNeedConnect;
      c.subscribeCursor = 0;
      c.ready = false;
      c.setupAttemptStartMs = 0;
      c.setupBusyMsSum = 0;
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

  // Fix Phase 1C: detect exactly the tick WiFi recovers from a drop (not
  // just "WiFi is down right now", which the early-return below already
  // handled before this candidate). While WiFi was down, serviceTick()
  // never ran at all, so no group's `connected()`/`ready` state was kept
  // honest -- the underlying TCP socket almost certainly died with the
  // WiFi link, but this file cannot assume WHEN the MQTT library itself
  // notices that (unverified, no library source available). Rather than
  // trust a possibly-stale gc.ready/connected() the instant WiFi returns,
  // every active client that had actual in-progress or completed setup
  // state is force-reset to restart staged setup from scratch -- using the
  // SAME wasReady-based cooldown rule as any other transport loss (fresh
  // cooldown if it hadn't finished setup yet; preserve the prior deadline
  // if it had already reached Ready).
  bool wifiConnected = WifiManager::isConnected();
  if (wifiConnected && !g_wifiWasConnected) {
    for (auto& gc : g_clients) {
      if (!gc.active || gc.mqtt == nullptr) continue;
      // Fix Phase 1C (corrected): a client already sitting at kNeedConnect
      // with the transport already down has NOTHING for this block to
      // reconcile -- touching it anyway would do one of two wrong things:
      // (a) a client that has NEVER attempted setup yet (cooldown still 0,
      // the "eligible immediately" sentinel) would have a cooldown
      // INVENTED for it here, delaying its very first connection attempt
      // for no reason -- it never failed anything; or (b) a client already
      // idling on an existing cooldown from an earlier real failure would
      // have that cooldown reset/extended merely because WiFi happened to
      // recover, which is not a reason to change it. Skipping both cases
      // and only reconciling a client with actual stale state (mid-setup,
      // or reached Ready, or -- defensively -- still reporting
      // connected()==true despite the outage) preserves immediate
      // first-attempt eligibility and any already-appropriate cooldown.
      if (gc.stage == SetupStage::kNeedConnect && !gc.mqtt->connected()) continue;
      bool wasReady = gc.ready;
      if (gc.mqtt->connected()) gc.mqtt->disconnect();
      gc.mqtt->setTimeout(kMqttCommandTimeoutMs);  // never left in a stale widened state across the outage
      gc.ready = false;
      gc.stage = SetupStage::kNeedConnect;
      gc.subscribeCursor = 0;
      if (!wasReady) gc.nextReconnectAttemptMs = computeNextReconnectDeadline(millis());
      // else: preserve the prior deadline, per the same Ready-loss rule as below.
    }
  }
  g_wifiWasConnected = wifiConnected;
  if (!wifiConnected) return;  // never block; just wait until WiFi is up

  uint32_t now = millis();

  // mqtt->loop() is the library's own per-connection servicing (keepalive/
  // incoming-message pump). Hardware Diagnostic #4.9e Part B2 item 1 /
  // Part C, reaffirmed by #4.9f: do NOT assume this is always
  // non-blocking, and do NOT assume every reconnect path is already fully
  // throttled just because Part A1 rate-limits the setup operations below
  // -- 256dpi/MQTT uses synchronous lwmqtt operations internally, so a
  // CONNECTED client can still momentarily wait on network data within
  // loop(). Timed individually so a single slow group is visible, not just
  // the whole for-loop; an already-disconnected client that returns fast
  // never prints here (Part B2's "do not print for every disconnected
  // client on every tick"), but one that WAS connected and comes back from
  // loop() either erroring or no longer connected always does, regardless
  // of duration, since that's a real connectivity event worth seeing.
  //
  // Fix Phase 1C (corrected): this is also where a transport loss BETWEEN
  // staged setup operations (or after reaching Ready) is reconciled, via
  // reconcileClientConnectivity() -- called unconditionally for every
  // active client below, regardless of whether a connectedBefore/After
  // transition was observed during THIS tick's own loop() call. That
  // distinction matters: a connection closed by an application publish()
  // from a different AppService's tick (or any other cause outside this
  // loop) already reads connected()==false by the time this tick starts,
  // so a check gated on "transitioned during this tick's loop()" would
  // never see it and the client would be stuck. See
  // reconcileClientConnectivity()'s own comment for the full trace.
  for (auto& gc : g_clients) {
    if (!gc.active || gc.mqtt == nullptr) continue;
    bool connectedBefore = gc.mqtt->connected();
    uint32_t loopStart = millis();
    bool loopOk = gc.mqtt->loop();
    uint32_t loopElapsed = millis() - loopStart;
    bool connectedAfter = gc.mqtt->connected();

    reconcileClientConnectivity(gc);

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

  // Fix Phase 1C: AT MOST ONE setup operation -- connect OR one subscribe
  // OR the ONLINE publish -- performed globally per tick, chosen
  // round-robin starting from g_nextSetupCandidateIndex across every group
  // with an eligible operation right now (hasEligibleSetupOperation()), so
  // a single broken/slow group can never starve the others' turns, and no
  // group's setup work blocks Input::popEvent()/Menu::tick() for longer
  // than one operation's own timeout. Note: this bounds the OPERATION
  // COUNT per tick, not wall-clock time -- a single operation can still
  // block for up to kMqttSetupTimeoutMsExperiment (plus connect()'s own
  // unbounded TCP/DNS phase for a connect operation specifically).
  for (uint8_t attempts = 0; attempts < Settings::kMaxGroups; attempts++) {
    uint8_t idx = g_nextSetupCandidateIndex;
    g_nextSetupCandidateIndex = static_cast<uint8_t>((g_nextSetupCandidateIndex + 1) % Settings::kMaxGroups);
    GroupClient& gc = g_clients[idx];
    if (hasEligibleSetupOperation(gc, now)) {
      performEligibleSetupOperation(gc);
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

// TRANSPORT-ONLY: true the instant the underlying MQTT connection is up,
// which (as of Fix Phase 1C's staged setup) can be BEFORE this device's own
// 13 subscriptions or its ONLINE presence publish have necessarily
// completed. Meaning UNCHANGED by this candidate -- every existing caller
// (Outbox flush eligibility, chat/game/race send-gates, group-deletion
// cleanup) was already written to treat "connected" as "safe to attempt a
// bare outbound publish", which remains true: publishRaw()/publishBinary()
// only ever need the wire to be up, never any of this device's OWN
// subscriptions. Do NOT use this to decide whether an action that depends
// on RECEIVING something addressed to this device (a reply on one of its
// own subscribed topics) is safe -- see isGroupReady() below for that.
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

// Fix Phase 1C, additive: true only once transport is connected AND all 13
// subscriptions succeeded AND Presence::publishOnline() itself returned
// true with the connection still live afterward (see
// doPublishOnlineOperation()). This is a STRICTLY STRONGER, separate
// contract from isGroupConnected() above, not a redefinition of it --
// every existing isGroupConnected()/isAnyGroupConnected() caller keeps its
// current (transport-only) behavior unchanged.
//
// Use this only for an action whose correctness actually depends on THIS
// device's own subscriptions already being active -- i.e. it is waiting to
// receive a reply addressed to it. A new query alone does not make a
// caller safe merely by existing; as of this candidate the audited callers
// that actually need it are RadioTransport::startPrivateCall() (waits for
// a GRANT/DENY on its own radio/busy_reply/<device> subscription) and
// RadioTransport::handleRadioBusyPacket()'s BUSY_CLAIM branch (granting
// implicitly promises the claimer a SESSION_START/ACK exchange that
// depends on THIS device's own radio/session subscription already being
// live), plus Presence::republishOwnPresenceAllGroups() (to avoid a
// redundant/out-of-order ONLINE announcement racing the staged setup
// engine's own single publishOnline() step for the same group). Every
// other publishRaw()/publishBinary() caller audited for this candidate
// (chat/game sends, Outbox flush, the race/round retained-clear, and
// Race's Join/Solved/Reset publishes) does NOT need it -- those really are
// bare outbound publishes with no dependency on this device's own
// subscriptions, so isGroupConnected() remains sufficient and correct for
// them, unchanged.
//
// Race's Invite and Start Round are the one documented exception (see
// publishInvite()/publishStartRound() in race.cpp): each anticipates a
// reply (JOIN, SOLVED respectively) on this device's own subscription. As
// of round 4, the user ACTION that would trigger either is gated on
// isGroupReady() at its sole call site (race.cpp's handleRoomAction()),
// before publishInvite()/publishStartRound() are even entered -- not by
// passing isGroupReady() into publishRaw()/publishBinary() themselves,
// which still gate on isGroupConnected() only, exactly like every other
// caller here.
// Fix Phase 1C (corrected): explicitly checks WifiManager::isConnected()
// first. While WiFi is down, serviceTick() returns immediately (see
// serviceTick()'s own early return) and never runs the WiFi-recovery
// reconciliation block, so gc.ready/gc.mqtt->connected() are not
// necessarily kept honest for the FULL duration of an outage -- a caller
// polling isGroupReady() DURING that window (before the next serviceTick()
// after WiFi returns has had a chance to reconcile anything) must not see
// a stale true. This check is READ-ONLY: it does not mutate gc.ready or
// gc.stage, so the `wasReady` information the WiFi-recovery block and
// reconcileClientConnectivity() need to decide fresh-vs-preserved cooldown
// is never destroyed by merely querying readiness during an outage.
bool isGroupReady(const char* group_code) {
  if (!WifiManager::isConnected()) return false;
  GroupClient* gc = findClientSlot(group_code);
  return gc != nullptr && gc->mqtt != nullptr && gc->mqtt->connected() && gc->ready;
}

bool isAnyGroupReady() {
  if (!WifiManager::isConnected()) return false;
  for (auto& gc : g_clients) {
    if (gc.active && gc.mqtt != nullptr && gc.mqtt->connected() && gc.ready) return true;
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
    // unconditionally loops until g_queueCount reaches 0, re-checking the
    // count on every iteration -- so even a reentrant onMqttMessage() call
    // during this drain would itself get picked up and drained before the
    // loop exits, not left behind. This is NOT limited to reentrancy via
    // gc.mqtt->loop() (which serviceTick() -- now paused -- is the only
    // caller of): a dispatched handler can itself call publishBinary()/
    // publishRaw(), and whether the MQTT library's publish()/subscribe()
    // pump a synchronous read internally and reentrantly invoke
    // onMqttMessage() before returning is not something this file can rule
    // out (256dpi/MQTT's source is not available here to verify). Kept as
    // an explicit, leak-proof fallback rather than trusting the drain-to-
    // empty invariant silently: if it ever fires for some other reason,
    // every dynamic payload still marked used is freed exactly once here
    // rather than left allocated for the duration of OTA.
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
    // Fix Phase 1C: force-disconnect every connected client exactly as
    // before, and explicitly reset each one's staged-setup bookkeeping
    // here too -- not strictly required for correctness (isGroupReady()
    // already re-checks connected() defensively, so a stale gc.ready
    // alongside connected()==false could never be misread as Ready), but
    // this keeps a freshly-exited-maintenance client's internal state
    // honestly consistent rather than relying on that defensive check.
    // nextReconnectAttemptMs is deliberately left untouched, exactly as
    // before this candidate -- maintenance entry/exit follows the same
    // "preserve the prior deadline" mechanism as any other Ready-loss,
    // and typical OTA/sleep windows already exceed kMqttReconnectIntervalMs
    // anyway, so reconnect after exit is virtually always immediate.
    for (auto& gc : g_clients) {
      if (!gc.active || gc.mqtt == nullptr) continue;
      if (gc.mqtt->connected()) gc.mqtt->disconnect();
      gc.mqtt->setTimeout(kMqttCommandTimeoutMs);
      gc.ready = false;
      gc.stage = SetupStage::kNeedConnect;
      gc.subscribeCursor = 0;
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
//
// Hardware Diagnostic #4.9t: TEMPORARY synchronous experiment, not a
// non-blocking fix -- see kMqttPublishTimeoutMsExperiment's own comment.
// Audited: publishBinary() is never called from within any of the staged
// setup operation functions' own widened-timeout windows (each of
// doConnectOperation()/doSubscribeOperation()/doPublishOnlineOperation()
// restores kMqttCommandTimeoutMs itself, immediately after its own single
// gc.mqtt call, before returning -- see Fix Phase 1C) and never reentrantly
// from the MQTT callback (onMessageAdvanced's onMqttMessage() only
// enqueues; dispatch to any handler that might call this happens later,
// via drainReceiveQueue(), which serviceTick() always calls after that
// tick's one setup operation, if any, has already restored the normal
// timeout) -- so this function can safely assume it is always entered with
// kMqttCommandTimeoutMs already in effect.
bool publishBinary(const char* group_code, const char* topic_suffix, const uint8_t* data, uint16_t len,
                   bool retained, int qos) {
  GroupClient* gc = findClientSlot(group_code);
  if (gc == nullptr || gc->mqtt == nullptr || !gc->mqtt->connected()) return false;
  char topic[80];
  snprintf(topic, sizeof(topic), "morsebuddy/%s/%s", group_code, topic_suffix);
  if (qos == 1) {
    gc->mqtt->setTimeout(kMqttPublishTimeoutMsExperiment);
    uint32_t publishStart = millis();
    bool result = gc->mqtt->publish(topic, reinterpret_cast<const char*>(data), static_cast<int>(len), retained, qos);
    uint32_t publishElapsed = millis() - publishStart;
    int lastErr = static_cast<int>(gc->mqtt->lastError());
    bool connectedAfter = gc->mqtt->connected();
    // Restore before any logging/return, on both success and failure.
    gc->mqtt->setTimeout(kMqttCommandTimeoutMs);
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
  return gc->mqtt->publish(topic, reinterpret_cast<const char*>(data), static_cast<int>(len), retained, qos);
}

}  // namespace MqttManager
