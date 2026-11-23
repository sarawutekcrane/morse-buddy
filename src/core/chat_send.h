#pragma once
// Asynchronous chat send (hardware device A: "[PERF][CHAT_SEND] total=1195"
// of which "[PERF][MQTT] publish ... qos=1 ... 605 ms" was the PUBACK wait
// on the main task). Used by Text (text_message.cpp) and Enigma (enigma.cpp).
//
// The ORDER of a send is unchanged: publish first, then store the record
// once, with the flags the publish result decides (0 when the publish
// succeeded, FLAG_PENDING_OUTBOX otherwise, so the Outbox retries it --
// exactly as the synchronous path). Only the wait in between moves: the
// publish runs on the MQTT connection worker (MqttManager::publishBinaryAsync)
// and the store runs later on the MAIN task, from MqttManager's service tick,
// when the result is back. No filesystem work ever runs on another task.
//
// Message identity: the record is stored from a copy of the exact wire bytes
// that were published (same message_id, timestamp, payload); the Enigma
// local payload is copied too. Nothing is re-encoded.
//
// Crash window: a reset between the publish and the store loses the local
// record -- the same window the synchronous path has (publish, then store),
// only longer (the publish's duration, at most the worker's 5 s timeout).
//
// One send per group at a time: while a group's send is in flight the
// caller refuses a new one (the draft stays), so records are stored in the
// order they were sent and the Outbox never sees a later message first.
//
// Header-only (inline functions share one set of statics across the units
// that include it), so the host test builds that link text_message.cpp and
// enigma.cpp need no extra source file.
#include <Arduino.h>
#include <string.h>

#include <new>

#include "core/mqtt_manager.h"
#include "core/settings.h"
#include "core/storage_messages.h"

namespace ChatSend {

struct Outcome {
  const char* group;
  const char* contact;
  bool published;    // the publish succeeded (QoS1: PUBACK received)
  bool stored;       // appendStoredMessage() returned true
  bool groupGone;    // the group was deleted meanwhile: nothing stored
  const char* text;  // the draft text that was sent (for the retention rule)
};
using DoneFn = void (*)(const Outcome&);

struct InFlight {
  uint32_t ticket;
  uint32_t startedMs;
  uint32_t timestamp;
  char group[sizeof(Settings::FamilyGroup::code)];
  char contact[MessageStore::kContactKeyLen];
  uint8_t* wire;
  uint16_t wireLen;
  uint8_t* local;
  uint16_t localLen;
  char* text;
  DoneFn done;
};

inline InFlight** slots() {
  static InFlight* s[Settings::kMaxGroups] = {};
  return s;
}

inline void freeRecord(InFlight* f) {
  if (f == nullptr) return;
  delete[] f->wire;
  delete[] f->local;
  delete[] f->text;
  delete f;
}

// True while a send for this group waits for its publish result.
inline bool inFlight(const char* group) {
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    InFlight* f = slots()[i];
    if (f != nullptr && strcmp(f->group, group) == 0) return true;
  }
  return false;
}

inline void onPublished(uint32_t ticket, bool ok) {
  InFlight* f = nullptr;
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (slots()[i] != nullptr && slots()[i]->ticket == ticket) {
      f = slots()[i];
      slots()[i] = nullptr;
      break;
    }
  }
  if (f == nullptr) return;  // not ours (cannot happen: tickets are unique)
  uint32_t waitedMs = millis() - f->startedMs;
  Outcome o = {f->group, f->contact, ok, false, false, f->text};
  uint32_t storeStart = millis();
  // A group deleted while its send was in flight has no conversation to
  // store into any more (its records were removed with it).
  o.groupGone = Settings::isGroupCodeUnique(f->group);
  if (!o.groupGone) {
    uint16_t flags = ok ? 0 : MessageStore::FLAG_PENDING_OUTBOX;
    MessageRef outRef;
    o.stored = MessageStore::appendStoredMessage(f->group, f->contact, MessageStore::Direction::SENT, flags,
                                                 f->timestamp, f->wire, f->wireLen, f->local, f->localLen, &outRef);
  }
  uint32_t storeMs = millis() - storeStart;
  // waited = wall time between the send and its result (the main loop kept
  // running meanwhile; NOT blocking time). store = blocking time of this
  // tick's append.
  Serial.printf("[PERF][CHAT_SEND] async-complete waited=%lu ms store=%lu ms publishOk=%d storeOk=%d groupGone=%d\n",
                static_cast<unsigned long>(waitedMs), static_cast<unsigned long>(storeMs), ok ? 1 : 0,
                o.stored ? 1 : 0, o.groupGone ? 1 : 0);
  if (f->done != nullptr) f->done(o);
  freeRecord(f);
}

// Starts an asynchronous send. False when it could not start (another send
// for this group in flight, no memory, or publishBinaryAsync() refused: no
// connection, worker busy, maintenance) -- nothing was published then and
// the caller uses its synchronous path, unchanged.
inline bool start(const char* group, const char* contact, const char* topicSuffix, uint32_t timestamp,
                  const uint8_t* wire, uint16_t wireLen, const uint8_t* local, uint16_t localLen, const char* text,
                  DoneFn done) {
  if (inFlight(group)) return false;
  uint8_t idx = Settings::kMaxGroups;
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (slots()[i] == nullptr) {
      idx = i;
      break;
    }
  }
  if (idx == Settings::kMaxGroups) return false;
  InFlight* f = new (std::nothrow) InFlight();
  if (f == nullptr) return false;
  size_t textLen = strlen(text);
  f->wire = new (std::nothrow) uint8_t[wireLen];
  f->local = localLen > 0 ? new (std::nothrow) uint8_t[localLen] : nullptr;
  f->text = new (std::nothrow) char[textLen + 1];
  if (f->wire == nullptr || (localLen > 0 && f->local == nullptr) || f->text == nullptr) {
    freeRecord(f);
    return false;
  }
  memcpy(f->wire, wire, wireLen);
  f->wireLen = wireLen;
  if (localLen > 0) memcpy(f->local, local, localLen);
  f->localLen = localLen;
  memcpy(f->text, text, textLen + 1);
  strncpy(f->group, group, sizeof(f->group) - 1);
  f->group[sizeof(f->group) - 1] = '\0';
  strncpy(f->contact, contact, sizeof(f->contact) - 1);
  f->contact[sizeof(f->contact) - 1] = '\0';
  f->timestamp = timestamp;
  f->done = done;
  f->startedMs = millis();
  f->ticket = MqttManager::publishBinaryAsync(group, topicSuffix, wire, wireLen, false, 1, onPublished);
  if (f->ticket == 0) {
    freeRecord(f);
    return false;
  }
  slots()[idx] = f;  // the result is delivered from a later service tick, never from inside the call above
  return true;
}

}  // namespace ChatSend
