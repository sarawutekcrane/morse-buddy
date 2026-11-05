#include "core/outbox.h"

#include <Arduino.h>
#include <string.h>

#include <new>

#include "core/hooks.h"
#include "core/mqtt_manager.h"
#include "core/packet_codec.h"
#include "core/storage_messages.h"

namespace Outbox {

namespace {

// Device A on 2ed04b6: "[PERF][OUTBOX] scan 5143 ms found=0" -- one call
// (findMessagesByPredicate() over every conversation) loaded every stored
// record to find none pending, blocking the main loop for 5 s. Now:
//  - a pass over storage finds the records left pending on flash (a send
//    that could not be published, durable across reboots and power loss).
//    It is incremental like the Notifications sweep: one step per kStepMs,
//    each at most kWalkEntriesPerStep directory entries, one conversation
//    listing (names only), or up to kReadsPerStep header-only reads, ending
//    early once kStepBudgetMs is spent. It runs once after boot (once a
//    group is connected, as the earlier scan did), and again after a change made outside
//    the store (MessageStore::getExternalMutationCount()); a pass that could
//    not read everything is repeated after kRetryMs. Nothing is skipped
//    silently.
//  - a record written pending later is queued at once
//    (MessageStore::setOnPendingWrittenCallback()): a send never causes a
//    scan.
//  - queued records are published one per step while their group is
//    connected (retried after kRetryMs otherwise); the pending flag is
//    cleared on success, as before.
//  - a queued record that cannot be read at publication is not dropped
//    unless its absence is confirmed (a reliable names-only listing of its
//    conversation without it): an inconclusive failure keeps it queued,
//    retried after a per-record backoff (kReadRetryMinMs doubling, at most
//    kReadRetryMaxMs), so it never blocks the other queued records and
//    never causes a storage pass. A record read and found no longer
//    pending is dropped (published or cleared elsewhere).
// The queue and the pass state live on the heap while in use. A record
// that does not fit the queue stays pending on flash and is found again by
// the next pass.
constexpr uint32_t kStepMs = 25;
constexpr uint32_t kStepBudgetMs = 20;
constexpr uint8_t kWalkEntriesPerStep = 16;
constexpr uint8_t kReadsPerStep = 4;
constexpr uint8_t kQueueSize = 16;
constexpr uint32_t kRetryMs = 5000;
constexpr uint32_t kReadRetryMinMs = 1000;
constexpr uint32_t kReadRetryMaxMs = 60000;
constexpr uint32_t kPassRetryMaxMs = 300000;  // an incomplete pass is repeated after a growing pause, at most this

struct Entry {
  MessageRef ref;
  uint32_t retryAtMs;   // not tried before this (a read failed)
  uint8_t readFailures; // inconclusive read failures in a row
};
struct Queue {
  Entry items[kQueueSize];
  uint8_t count;
};
Queue* g_queue = nullptr;
bool g_queueOverflowed = false;  // something pending was not queued: another pass finds it
uint32_t g_publishRetryAtMs = 0;

struct Pass {
  MessageStore::ConversationWalk* walk;
  char group[PacketCodec::kGroupCodeLen];
  char contact[MessageStore::kContactKeyLen];
  bool haveConversation;
  bool listed;
  bool incomplete;
  uint16_t n, pos;
  uint32_t externalAtStart;
  uint32_t startMs, busyMs;
  uint16_t records, found;
  uint32_t seqs[MessageStore::kMaxMessagesPerThread];
};
Pass* g_pass = nullptr;
bool g_passDone = false;          // a complete pass ran since boot (and since the last external change)
uint32_t g_externalAtPassStart = 0;  // getExternalMutationCount() when the last pass began
uint32_t g_passAgainAtMs = 0;     // after an incomplete pass, or an overflowed queue
uint32_t g_passRetryMs = kRetryMs;  // the pause before repeating an incomplete pass (doubles, reset by a complete one)
bool g_passAgain = false;
uint32_t g_nextStepMs = 0;

bool sameRef(const MessageRef& a, const MessageRef& b) {
  return a.sequence == b.sequence && strcmp(a.contact_key, b.contact_key) == 0 && strcmp(a.group_code, b.group_code) == 0;
}

void enqueue(const MessageRef& ref) {
  if (g_queue == nullptr) {
    g_queue = new (std::nothrow) Queue;
    if (g_queue == nullptr) {
      g_queueOverflowed = true;
      return;
    }
    g_queue->count = 0;
  }
  for (uint8_t i = 0; i < g_queue->count; i++) {
    if (sameRef(g_queue->items[i].ref, ref)) return;  // queued already (its retry state kept)
  }
  if (g_queue->count >= kQueueSize) {
    g_queueOverflowed = true;
    return;
  }
  Entry& e = g_queue->items[g_queue->count++];
  e.ref = ref;
  e.retryAtMs = millis();
  e.readFailures = 0;
}

void removeAt(uint8_t k) {
  for (uint8_t i = k + 1; i < g_queue->count; i++) g_queue->items[i - 1] = g_queue->items[i];
  g_queue->count--;
  if (g_queue->count == 0) {
    delete g_queue;
    g_queue = nullptr;
  }
}

// The entry goes behind the others (their turn first).
void moveToBack(uint8_t k) {
  Entry e = g_queue->items[k];
  for (uint8_t i = k + 1; i < g_queue->count; i++) g_queue->items[i - 1] = g_queue->items[i];
  g_queue->items[g_queue->count - 1] = e;
}

// True only if the record is confirmed gone: its conversation listed
// reliably (names only, no record opened) without it. A listing that fails,
// or no memory for it, is inconclusive.
bool confirmedAbsent(const MessageRef& ref) {
  uint32_t* seqs = new (std::nothrow) uint32_t[MessageStore::kMaxMessagesPerThread];
  if (seqs == nullptr) return false;
  bool reliable = false;
  uint16_t n = MessageStore::listMessageSequences(ref.group_code, ref.contact_key, seqs, MessageStore::kMaxMessagesPerThread,
                                                  &reliable);
  bool listed = false;
  for (uint16_t i = 0; i < n && !listed; i++) listed = seqs[i] == ref.sequence;
  delete[] seqs;
  return reliable && !listed;
}

void onPendingWritten(const MessageRef& ref) { enqueue(ref); }

// Publishes the oldest queued record if its group is connected. True if
// this step did storage or network work.
bool publishStep() {
  if (g_queue == nullptr) return false;
  uint32_t now = millis();
  if (static_cast<int32_t>(now - g_publishRetryAtMs) < 0) return false;
  // The oldest entry whose read retry is due (a record waiting on one does
  // not hold up the rest).
  uint8_t k = 0;
  while (k < g_queue->count && static_cast<int32_t>(now - g_queue->items[k].retryAtMs) < 0) k++;
  if (k == g_queue->count) return false;
  MessageRef ref = g_queue->items[k].ref;
  if (!MqttManager::isGroupConnected(ref.group_code)) {
    // Still offline: the rest of the queue first, this one later.
    if (g_queue->count > 1) moveToBack(k);
    g_publishRetryAtMs = millis() + kRetryMs;
    return false;
  }
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) {
    if (confirmedAbsent(ref)) {
      removeAt(k);  // gone (deleted, evicted): nothing to send
      return true;
    }
    // Inconclusive (unreadable now): kept, tried again later.
    Entry& e = g_queue->items[k];
    if (e.readFailures < 255) e.readFailures++;
    uint32_t backoff = kReadRetryMinMs;
    for (uint8_t i = 1; i < e.readFailures && backoff < kReadRetryMaxMs; i++) backoff *= 2;
    if (backoff > kReadRetryMaxMs) backoff = kReadRetryMaxMs;
    e.retryAtMs = millis() + backoff;
    Serial.printf("[OUTBOX] queued record %s/%s/%lu unreadable (%u); retried in %lu ms\n", ref.group_code, ref.contact_key,
                  static_cast<unsigned long>(ref.sequence), static_cast<unsigned>(e.readFailures),
                  static_cast<unsigned long>(backoff));
    if (g_queue->count > 1) moveToBack(k);
    return true;
  }
  if (!(view.header.flags & MessageStore::FLAG_PENDING_OUTBOX)) {
    removeAt(k);  // read, and no longer pending: published (or cleared) already
    return true;
  }
  const char* suffix;
  char suffixBuf[24];
  if (strcmp(ref.contact_key, MessageStore::kEveryone) == 0) {
    suffix = "broadcast";
  } else {
    snprintf(suffixBuf, sizeof(suffixBuf), "msg/%s", ref.contact_key);
    suffix = suffixBuf;
  }
  uint32_t publishStart = millis();
  bool ok = MqttManager::publishBinary(ref.group_code, suffix, view.wirePacket, view.wirePacketLen,
                                       /*retained=*/false, /*qos=*/1);
  uint32_t publishElapsed = millis() - publishStart;
  if (publishElapsed >= 20) {
    Serial.printf("[PERF][OUTBOX] publish %lu ms group=%s ok=%d\n", static_cast<unsigned long>(publishElapsed),
                  ref.group_code, ok ? 1 : 0);
  }
  if (!ok) {
    g_publishRetryAtMs = millis() + kRetryMs;
    return true;
  }
  MessageStore::updateLocalFlags(ref, 0, MessageStore::FLAG_PENDING_OUTBOX);
  removeAt(k);
  return true;
}

void endPass() {
  uint32_t external = MessageStore::getExternalMutationCount();
  bool complete = !g_pass->incomplete && external == g_pass->externalAtStart;
  uint32_t wallMs = millis() - g_pass->startMs;
  if (g_pass->busyMs >= 20 || wallMs >= 20) {
    Serial.printf("[PERF][OUTBOX] pass wall=%lu ms busy=%lu ms records=%u found=%u complete=%d\n",
                  static_cast<unsigned long>(wallMs), static_cast<unsigned long>(g_pass->busyMs),
                  static_cast<unsigned>(g_pass->records), static_cast<unsigned>(g_pass->found), complete ? 1 : 0);
  }
  MessageStore::endConversationWalk(g_pass->walk);
  if (complete) {
    g_passDone = true;
    g_passAgain = false;
    g_passRetryMs = kRetryMs;
  } else {
    // Repeated after a growing pause: a record that stays unreadable does not
    // cause a whole-store pass every few seconds.
    g_passAgain = true;
    g_passAgainAtMs = millis() + g_passRetryMs;
    g_passRetryMs = g_passRetryMs * 2 < kPassRetryMaxMs ? g_passRetryMs * 2 : kPassRetryMaxMs;
  }
  delete g_pass;
  g_pass = nullptr;
}

void startPass() {
  g_pass = new (std::nothrow) Pass;
  if (g_pass == nullptr) return;  // tried again at the next step
  memset(g_pass, 0, sizeof(Pass));
  g_pass->externalAtStart = MessageStore::getExternalMutationCount();
  g_externalAtPassStart = g_pass->externalAtStart;
  g_pass->startMs = millis();
  g_pass->walk = MessageStore::beginConversationWalk();
  if (g_pass->walk == nullptr) g_pass->incomplete = true;
  g_passAgain = false;
  if (g_queueOverflowed) g_queueOverflowed = false;  // this pass finds what did not fit
}

// One bounded step of the pass.
void passStep() {
  uint32_t stepStart = millis();
  Pass& p = *g_pass;
  if (p.walk == nullptr) {
    endPass();
    return;
  }
  if (!p.haveConversation) {
    MessageStore::WalkResult r = MessageStore::stepConversationWalk(p.walk, kWalkEntriesPerStep, p.group, sizeof(p.group),
                                                                    p.contact, sizeof(p.contact));
    if (r == MessageStore::WalkResult::FOUND) {
      p.haveConversation = true;
      p.listed = false;
    } else if (r != MessageStore::WalkResult::MORE) {
      if (r == MessageStore::WalkResult::FAILED) p.incomplete = true;
      p.busyMs += millis() - stepStart;
      endPass();
      return;
    }
  } else if (!p.listed) {
    bool reliable = false;
    p.n = MessageStore::listMessageSequences(p.group, p.contact, p.seqs, MessageStore::kMaxMessagesPerThread, &reliable);
    if (!reliable) p.incomplete = true;  // what was listed is still checked
    p.pos = 0;
    p.listed = true;
  } else {
    MessageRef ref;
    strncpy(ref.group_code, p.group, sizeof(ref.group_code) - 1);
    ref.group_code[sizeof(ref.group_code) - 1] = '\0';
    strncpy(ref.contact_key, p.contact, sizeof(ref.contact_key) - 1);
    ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
    for (uint8_t k = 0; k < kReadsPerStep && p.pos < p.n; k++) {
      if (k > 0 && millis() - stepStart >= kStepBudgetMs) break;
      ref.sequence = p.seqs[p.pos++];
      MessageStore::StoredHeader hdr;
      if (!MessageStore::readMessageHeader(ref, &hdr)) {
        p.incomplete = true;  // unreadable (or removed since the listing): the next pass looks again
        continue;
      }
      p.records++;
      if (hdr.flags & MessageStore::FLAG_PENDING_OUTBOX) {
        p.found++;
        enqueue(ref);
      }
    }
    if (p.pos >= p.n) p.haveConversation = false;
  }
  p.busyMs += millis() - stepStart;
}

void serviceInit() { MessageStore::setOnPendingWrittenCallback(onPendingWritten); }

void serviceTick() {
  uint32_t now = millis();
  if (static_cast<int32_t>(now - g_nextStepMs) < 0) return;
  g_nextStepMs = now + kStepMs;
  if (publishStep()) return;
  if (MessageStore::chatIndexLoading()) return;  // a chat loading its index goes first (as in Notifications)
  if (g_pass == nullptr) {
    // A change outside the store since the last pass began, or a full queue,
    // starts one at once; the first pass after boot too. A pass that could
    // not read everything (also the first) is repeated only after its
    // growing pause.
    bool externalChange = MessageStore::getExternalMutationCount() != g_externalAtPassStart;
    bool due = externalChange || g_queueOverflowed || (!g_passDone && !g_passAgain) ||
               (g_passAgain && static_cast<int32_t>(now - g_passAgainAtMs) >= 0);
    // As before: storage is looked through only once a group is connected
    // (nothing could be published before).
    if (!due || !MqttManager::isAnyGroupConnected()) return;
  }
  // One background storage step per main-loop pass, shared with Notifications.
  if (!claimBackgroundStorageStep(BG_OUTBOX)) {
    g_nextStepMs = now;  // not this pass: the next one
    return;
  }
  if (g_pass == nullptr) {
    startPass();
    if (g_pass == nullptr) return;
  }
  passStep();
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

// Publishes what is queued now (each record once), without waiting for the
// service's steps.
void tryFlushNow() {
  for (uint8_t i = 0; g_queue != nullptr && i < kQueueSize; i++) {
    g_publishRetryAtMs = 0;
    if (!publishStep()) break;
  }
}

}  // namespace Outbox
