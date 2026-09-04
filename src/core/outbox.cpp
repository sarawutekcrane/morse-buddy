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

void flushPending() {
  MessageRef refs[kMaxBatch];
  uint16_t n = MessageStore::findMessagesByPredicate(nullptr, nullptr, isPendingPredicate, nullptr, refs, kMaxBatch);
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

    bool ok = MqttManager::publishBinary(refs[i].group_code, suffix, view.wirePacket, view.wirePacketLen,
                                         /*retained=*/false, /*qos=*/1);
    if (ok) {
      MessageStore::updateLocalFlags(refs[i], 0, MessageStore::FLAG_PENDING_OUTBOX);
    }
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
