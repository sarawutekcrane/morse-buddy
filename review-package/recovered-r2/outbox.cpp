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
// The queue and the pass state live on the heap while in use. A record
// that does not fit the queue stays pending on flash and is found again by
// the next pass.
constexpr uint32_t kStepMs = 25;
constexpr uint32_t kStepBudgetMs = 20;
constexpr uint8_t kWalkEntriesPerStep = 16;
constexpr uint8_t kReadsPerStep = 4;
constexpr uint8_t kQueueSize = 16;
constexpr uint32_t kRetryMs = 5000;

struct Queue {
  MessageRef refs[kQueueSize];
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
uint32_t g_externalSeen = 0;      // getExternalMutationCount() a complete pass covered
uint32_t g_passAgainAtMs = 0;     // after an incomplete pass, or an overflowed queue
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
    if (sameRef(g_queue->refs[i], ref)) return;
  }
  if (g_queue->count >= kQueueSize) {
    g_queueOverflowed = true;
    return;
  }
  g_queue->refs[g_queue->count++] = ref;
}

void dropFront() {
  for (uint8_t i = 1; i < g_queue->count; i++) g_queue->refs[i - 1] = g_queue->refs[i];
  g_queue->count--;
  if (g_queue->count == 0) {
    delete g_queue;
    g_queue = nullptr;
  }
}

void onPendingWritten(const MessageRef& ref) { enqueue(ref); }

// Publishes the oldest queued record if its group is connected. True if
// this step did storage or network work.
bool publishStep() {
  if (g_queue == nullptr) return false;
  if (static_cast<int32_t>(millis() - g_publishRetryAtMs) < 0) return false;
  MessageRef ref = g_queue->refs[0];
  if (!MqttManager::isGroupConnected(ref.group_code)) {
    // Still offline: the rest of the queue first, this one later.
    if (g_queue->count > 1) {
      dropFront();
      enqueue(ref);
    }
    g_publishRetryAtMs = millis() + kRetryMs;
    return false;
  }
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view) || !(view.header.flags & MessageStore::FLAG_PENDING_OUTBOX)) {
    dropFront();  // gone, or already published
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
  dropFront();
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
    g_externalSeen = g_pass->externalAtStart;
  } else {
    g_passAgain = true;
    g_passAgainAtMs = millis() + kRetryMs;
  }
  delete g_pass;
  g_pass = nullptr;
}

void startPass() {
  g_pass = new (std::nothrow) Pass;
  if (g_pass == nullptr) return;  // tried again at the next step
  memset(g_pass, 0, sizeof(Pass));
  g_pass->externalAtStart = MessageStore::getExternalMutationCount();
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
    bool externalChange = MessageStore::getExternalMutationCount() != g_externalSeen;
    bool due = !g_passDone || externalChange || g_queueOverflowed ||
               (g_passAgain && static_cast<int32_t>(now - g_passAgainAtMs) >= 0);
    // As before: storage is looked through only once a group is connected
    // (nothing could be published before).
    if (!due || !MqttManager::isAnyGroupConnected()) return;
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
