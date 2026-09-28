#include "core/outbox.h"

#include <Arduino.h>
#include <string.h>

#include "core/hooks.h"
#include "core/mqtt_manager.h"
#include "core/packet_codec.h"
#include "core/storage_messages.h"

namespace Outbox {

namespace {

constexpr uint32_t kScanIntervalMs = 5000;
constexpr uint16_t kMaxBatch = 32;
uint32_t g_lastScanMs = 0;

// Hardware Diagnostic #4.9h: caches "the last COMPLETE scan found ZERO
// pending messages, and storage hasn't changed since" so repeated empty
// scans (hardware evidence: ~1575-1577ms each, blocking the main loop
// every kScanIntervalMs with nothing to actually send) can be skipped
// entirely instead of re-walking and re-decoding every stored message on
// every tick. Starts INVALID on every boot -- the first scan after boot,
// and every scan immediately after a real storage mutation, always runs
// for real; this only removes the redundant repeats of an already-known
// answer. See scanAndFlush() for exactly how this is established/
// invalidated, and serviceTick() for the only place it's consulted to
// skip work (tryFlushNow() never skips -- an explicit manual flush
// request always performs a real scan).
bool g_emptyScanCacheValid = false;
uint32_t g_emptyScanCacheGeneration = 0;

bool isPendingPredicate(const MessageStore::StoredHeader& header, const PacketCodec::MessageEnvelope& envelope,
                        void* ctx) {
  (void)envelope;
  (void)ctx;
  return (header.flags & MessageStore::FLAG_PENDING_OUTBOX) != 0;
}

// Hardware Diagnostic #4.9e Part B3, extended by #4.9h: instrumentation
// only in the scan/publish/flush timings themselves -- no change to their
// behavior. A QoS1 MqttManager::publishBinary() can synchronously wait
// for the broker's PUBACK within the MQTT command timeout (Part C: do not
// assume publish/subscribe are non-blocking), and this loop can run that
// wait up to kMaxBatch (32) times in a row for one call, so per-publish +
// whole-flush timing shows whether that's actually happening on hardware.
//
// #4.9h also establishes/invalidates the empty-scan cache here, in the
// one place an actual scan happens, so serviceTick() and tryFlushNow()
// (both call this) can never disagree about what a real scan found.
void scanAndFlush() {
  // Snapshot the generation BEFORE scanning: only if nothing bumps it
  // between here and the scan's completion is a zero-result scan safe to
  // trust as "still true" going forward.
  uint32_t generationBefore = MessageStore::getStorageChangeGeneration();

  uint32_t flushStart = millis();

  MessageRef refs[kMaxBatch];
  uint32_t scanStart = millis();
  // Hardware Diagnostic #4.9h follow-up: conservatively initialized false
  // before the call, matching findMessagesByPredicate()'s own "initialize
  // conservatively" guarantee -- only that call setting it true means the
  // scan (including a zero result) can be trusted as complete.
  bool scanReliable = false;
  uint16_t n = MessageStore::findMessagesByPredicate(nullptr, nullptr, isPendingPredicate, nullptr, refs, kMaxBatch,
                                                      &scanReliable);
  uint32_t scanElapsed = millis() - scanStart;
  if (scanElapsed >= 20) {
    Serial.printf("[PERF][OUTBOX] scan %lu ms found=%u\n", static_cast<unsigned long>(scanElapsed),
                  static_cast<unsigned>(n));
  }
  // n is findMessagesByPredicate()'s TRUE total match count across every
  // conversation -- it keeps counting past outRefsCapacity (kMaxBatch)
  // rather than capping there (see its own header comment) -- so `n == 0`
  // below means "genuinely nothing pending anywhere," never "the first
  // kMaxBatch happened to be empty" while more exist beyond the batch.
  uint16_t limit = (n < kMaxBatch) ? n : kMaxBatch;

  for (uint16_t i = 0; i < limit; i++) {
    if (!MqttManager::isGroupConnected(refs[i].group_code)) continue;  // still offline; retry later

    StoredMessageView view;
    if (!MessageStore::loadMessage(refs[i], &view)) continue;

    char suffixBuf[24];
    const char* suffix;
    if (strcmp(refs[i].contact_key, MessageStore::kEveryone) == 0) {
      suffix = "broadcast";
    } else {
      snprintf(suffixBuf, sizeof(suffixBuf), "msg/%s", refs[i].contact_key);
      suffix = suffixBuf;
    }

    uint32_t publishStart = millis();
    bool ok = MqttManager::publishBinary(refs[i].group_code, suffix, view.wirePacket, view.wirePacketLen,
                                         /*retained=*/false, /*qos=*/1);
    uint32_t publishElapsed = millis() - publishStart;
    if (publishElapsed >= 20) {
      Serial.printf("[PERF][OUTBOX] publish %lu ms group=%s ok=%d\n", static_cast<unsigned long>(publishElapsed),
                    refs[i].group_code, ok ? 1 : 0);
    }
    if (ok) {
      // Hardware Diagnostic #4.9h follow-up: correcting an earlier,
      // inaccurate claim here -- atomicRewrite() does NOT guarantee the
      // flag "stays set on disk" if this call fails; it removes the
      // existing final file before renaming the new one into place, so a
      // failure partway through can leave the record's on-disk state
      // genuinely uncertain (unchanged, gone, or a partial write), not
      // simply "still pending." What IS guaranteed is that atomicRewrite()
      // bumps the storage generation unconditionally before attempting
      // this, whether it succeeds or fails, so the empty-scan cache below
      // is never left trusting a stale answer regardless of the outcome --
      // see storage_messages.cpp's atomicRewrite()/g_storageChangeGeneration
      // comments for that invariant, which this file does not alter.
      MessageStore::updateLocalFlags(refs[i], 0, MessageStore::FLAG_PENDING_OUTBOX);
    }
  }

  uint32_t flushElapsed = millis() - flushStart;
  if (flushElapsed >= 20) {
    Serial.printf("[PERF][OUTBOX] flush %lu ms attempted=%u\n", static_cast<unsigned long>(flushElapsed),
                  static_cast<unsigned>(limit));
  }

  // Hardware Diagnostic #4.9h, corrected by the #4.9h follow-up: only a
  // scan that found ZERO matches AND was itself reliable (no detected
  // directory-open or record-load/decode failure -- see
  // findMessagesByPredicate()'s outScanReliable) AND saw the storage
  // generation unchanged across its own duration may establish the
  // empty-scan cache. A zero result from an UNRELIABLE scan (a transient
  // read/open failure hid a record rather than one genuinely not
  // existing) must NOT be cached -- caching it would hide a pending
  // message until some unrelated mutation or a reboot instead of being
  // retried on the next scheduled scan, which is exactly the regression
  // this follow-up closes. Every other outcome (n != 0, scanReliable ==
  // false, or the generation moved for any reason) conservatively marks
  // the cache invalid rather than leaving it ambiguous, so a later real
  // change -- or a later successful retry of a failed scan -- can never
  // be masked by a stale "empty" belief.
  uint32_t generationAfter = MessageStore::getStorageChangeGeneration();
  if (n == 0 && scanReliable && generationAfter == generationBefore) {
    g_emptyScanCacheValid = true;
    g_emptyScanCacheGeneration = generationAfter;
  } else {
    g_emptyScanCacheValid = false;
  }
}

void serviceTick() {
  uint32_t now = millis();
  if (now - g_lastScanMs < kScanIntervalMs) return;
  g_lastScanMs = now;
  if (!MqttManager::isAnyGroupConnected()) return;

  // Hardware Diagnostic #4.9h: skip the expensive scan+decode work only
  // when a previous COMPLETE scan already found zero pending messages and
  // storage has not changed since (same generation) -- this removes the
  // repeated ~1575ms empty scan the hardware evidence showed happening
  // every kScanIntervalMs with nothing to actually send, without touching
  // the 5000ms cadence itself (g_lastScanMs above is updated exactly as
  // before, on every eligible tick, whether this ends up skipping or
  // not). The very first scan after boot is never skipped
  // (g_emptyScanCacheValid starts false), and any tick following a real
  // storage mutation (a new pending append, a flag change, an eviction)
  // is never skipped either, since that mutation already moved the
  // generation this cached value no longer matches.
  if (g_emptyScanCacheValid && MessageStore::getStorageChangeGeneration() == g_emptyScanCacheGeneration) {
    return;
  }

  scanAndFlush();
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = nullptr;
    svc.tick = serviceTick;
    registerAppService(svc);
  }
};
Registrar g_registrar;

}  // namespace

// Hardware Diagnostic #4.9h: always performs a real scan -- deliberately
// bypasses the empty-scan cache serviceTick() consults, since an explicit
// manual flush request should never be silently skipped by a possibly
// stale-feeling cache. scanAndFlush() still updates that cache correctly
// at the end (same bookkeeping either caller reaches it through), so this
// never leaves the cache in a state serviceTick() would misread.
void tryFlushNow() { scanAndFlush(); }

}  // namespace Outbox
