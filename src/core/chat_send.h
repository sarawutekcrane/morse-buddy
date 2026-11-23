#pragma once
// Asynchronous chat send (hardware device A: "[PERF][CHAT_SEND] total=1195"
// of which "[PERF][MQTT] publish ... qos=1 ... 605 ms" was the PUBACK wait
// on the main task). Used by Text (text_message.cpp) and Enigma (enigma.cpp).
//
// Store first, then publish:
//  1. The caller stores the record durably, on the main task, flagged
//     FLAG_PENDING_OUTBOX -- exactly as an offline send is stored. From here
//     the message survives a reset or power loss: the Outbox's boot pass
//     finds the pending record and publishes it (same message_id; receivers
//     drop a message_id they already hold -- MessageStore's dedup ring, the
//     newest 32 records of the conversation). Delivery is at-least-once,
//     never exactly-once.
//  2. start() hands the same wire bytes to the MQTT worker
//     (MqttManager::publishBinaryAsync()); the PUBACK wait never blocks the
//     main loop.
//  3. The result arrives on the main task (MqttManager's service tick). On
//     success the pending flag is cleared (updateLocalFlags(), main task);
//     on failure the record stays pending and the Outbox retries it.
// While a record's publish is out, the Outbox skips it (holds()), so it is
// not published twice. No filesystem work ever runs on another task.
//
// Header-only (inline functions share one set of statics across the units
// that include it), so the host test builds that link text_message.cpp,
// enigma.cpp and outbox.cpp need no extra source file.
#include <Arduino.h>
#include <string.h>

#include <new>

#include "core/mqtt_manager.h"
#include "core/settings.h"
#include "core/storage_messages.h"

namespace ChatSend {

struct Outcome {
  MessageRef ref;
  bool published;    // the publish succeeded (QoS1: PUBACK received)
  bool flagCleared;  // ...and the record's pending flag was cleared (else the Outbox republishes it)
  bool groupGone;    // the group was deleted meanwhile (its records with it)
};
using DoneFn = void (*)(const Outcome&);

struct InFlight {
  uint32_t ticket;
  uint32_t startedMs;
  MessageRef ref;
  DoneFn done;
};

// At most this many chat publishes out at once (all groups): the worker's
// per-group chain (kMaxLentPublishes, 4) bounds it already; this bounds RAM.
constexpr uint8_t kMaxInFlight = 8;

inline InFlight** slots() {
  static InFlight* s[kMaxInFlight] = {};
  return s;
}

inline bool sameRef(const MessageRef& a, const MessageRef& b) {
  return a.sequence == b.sequence && strcmp(a.contact_key, b.contact_key) == 0 && strcmp(a.group_code, b.group_code) == 0;
}

// True while this record's first publish is with the worker.
inline bool holds(const MessageRef& ref) {
  for (uint8_t i = 0; i < kMaxInFlight; i++) {
    if (slots()[i] != nullptr && sameRef(slots()[i]->ref, ref)) return true;
  }
  return false;
}

inline void onPublished(uint32_t ticket, bool ok) {
  InFlight* f = nullptr;
  for (uint8_t i = 0; i < kMaxInFlight; i++) {
    if (slots()[i] != nullptr && slots()[i]->ticket == ticket) {
      f = slots()[i];
      slots()[i] = nullptr;
      break;
    }
  }
  if (f == nullptr) return;  // not ours (cannot happen: tickets are unique)
  uint32_t waitedMs = millis() - f->startedMs;
  Outcome o;
  o.ref = f->ref;
  o.published = ok;
  o.flagCleared = false;
  o.groupGone = Settings::isGroupCodeUnique(f->ref.group_code);
  uint32_t clearStart = millis();
  if (ok && !o.groupGone) o.flagCleared = MessageStore::updateLocalFlags(f->ref, 0, MessageStore::FLAG_PENDING_OUTBOX);
  uint32_t clearMs = millis() - clearStart;
  // waited = wall time between the send and its result (the main loop kept
  // running meanwhile; NOT blocking time). clear = blocking time of this
  // tick's flag rewrite (0 when the publish failed: the record stays pending).
  Serial.printf("[PERF][CHAT_SEND] async-complete waited=%lu ms clear=%lu ms publishOk=%d flagCleared=%d groupGone=%d\n",
                static_cast<unsigned long>(waitedMs), static_cast<unsigned long>(clearMs), ok ? 1 : 0,
                o.flagCleared ? 1 : 0, o.groupGone ? 1 : 0);
  DoneFn done = f->done;
  delete f;
  if (done != nullptr) done(o);
}

// Starts the publish of a record already stored with FLAG_PENDING_OUTBOX.
// False when it could not start (no memory, too many out, or
// publishBinaryAsync() refused: no connection, the group's chain full,
// worker queue full, maintenance) -- the record simply stays pending for
// the Outbox; nothing waits and nothing is published here.
inline bool start(const MessageRef& ref, const char* topicSuffix, const uint8_t* wire, uint16_t wireLen, DoneFn done) {
  uint8_t idx = kMaxInFlight;
  for (uint8_t i = 0; i < kMaxInFlight; i++) {
    if (slots()[i] == nullptr) {
      idx = i;
      break;
    }
  }
  if (idx == kMaxInFlight) return false;
  InFlight* f = new (std::nothrow) InFlight();
  if (f == nullptr) return false;
  f->ref = ref;
  f->done = done;
  f->startedMs = millis();
  f->ticket = MqttManager::publishBinaryAsync(ref.group_code, topicSuffix, wire, wireLen, false, 1, onPublished);
  if (f->ticket == 0) {
    delete f;
    return false;
  }
  slots()[idx] = f;  // the result is delivered from a later service tick, never from inside the call above
  return true;
}

}  // namespace ChatSend
