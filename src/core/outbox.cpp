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

bool isPendingPredicate(const MessageStore::StoredHeader& header, const PacketCodec::MessageEnvelope& envelope,
                        void* ctx) {
  (void)envelope;
  (void)ctx;
  return (header.flags & MessageStore::FLAG_PENDING_OUTBOX) != 0;
}

// Hardware Diagnostic #4.9e Part B3: instrumentation only, no behavior
// change. This is a strong suspect for the reported multi-second Send
// delay -- a QoS1 MqttManager::publishBinary() can synchronously wait for
// the broker's PUBACK within the MQTT command timeout (Part C: do not
// assume publish/subscribe are non-blocking), and this loop can run that
// wait up to kMaxBatch (32) times in a row for one flushPending() call, so
// per-publish + whole-flush timing will show whether that's what's
// actually happening on hardware.
void flushPending() {
  uint32_t flushStart = millis();

  MessageRef refs[kMaxBatch];
  uint32_t scanStart = millis();
  uint16_t n = MessageStore::findMessagesByPredicate(nullptr, nullptr, isPendingPredicate, nullptr, refs, kMaxBatch);
  uint32_t scanElapsed = millis() - scanStart;
  if (scanElapsed >= 20) {
    Serial.printf("[PERF][OUTBOX] scan %lu ms found=%u\n", static_cast<unsigned long>(scanElapsed),
                  static_cast<unsigned>(n));
  }
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
      MessageStore::updateLocalFlags(refs[i], 0, MessageStore::FLAG_PENDING_OUTBOX);
    }
  }

  uint32_t flushElapsed = millis() - flushStart;
  if (flushElapsed >= 20) {
    Serial.printf("[PERF][OUTBOX] flush %lu ms attempted=%u\n", static_cast<unsigned long>(flushElapsed),
                  static_cast<unsigned>(limit));
  }
}

void serviceTick() {
  uint32_t now = millis();
  if (now - g_lastScanMs < kScanIntervalMs) return;
  g_lastScanMs = now;
  if (!MqttManager::isAnyGroupConnected()) return;
  flushPending();
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

void tryFlushNow() { flushPending(); }

}  // namespace Outbox
