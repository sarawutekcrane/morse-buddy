#pragma once
// HOST TEST (not production). Writes message records the way older firmware
// left them on flash: conversations of up to 300 records (the earlier
// retention), written directly in storage_messages.cpp's record format
// (atomicRewrite(): direction, flags, local_write_sequence,
// effective_sort_timestamp, wire length + bytes, local length + bytes) as
// /messages/<group>/<contact>/<%08lu>.msg under LittleFS.root. The current
// append path keeps only MessageStore::kRetainedMessagesPerThread, so it
// cannot build such a history. Every record written is read back through the
// ACTUAL MessageStore::loadMessage() and compared field by field; a mismatch
// aborts the test binary.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <filesystem>
#include <string>

#include "LittleFS.h"
#include "core/packet_codec.h"
#include "core/storage_messages.h"

inline MessageRef writeLegacyRecord(const char* group, const char* contact, MessageStore::Direction dir, uint16_t flags,
                                    uint32_t ts, const PacketCodec::MessageEnvelope& env, const char* body) {
  std::string dirPath = LittleFS.root + "/messages/" + group + "/" + contact;
  std::error_code ec;
  std::filesystem::create_directories(dirPath, ec);
  uint32_t maxOnDisk = 0;
  for (auto& e : std::filesystem::directory_iterator(dirPath, ec)) {
    uint32_t s = static_cast<uint32_t>(strtoul(e.path().filename().c_str(), nullptr, 10));
    if (s > maxOnDisk) maxOnDisk = s;
  }
  uint32_t seq = maxOnDisk + 1;  // continues after whatever is on disk
  uint8_t wire[512];
  size_t wn = PacketCodec::encodeMessagePacket(env, reinterpret_cast<const uint8_t*>(body), static_cast<uint16_t>(strlen(body)),
                                               wire, sizeof(wire));
  std::string rec;
  auto u16 = [&](uint16_t v) { rec.push_back(static_cast<char>(v & 0xFF)); rec.push_back(static_cast<char>(v >> 8)); };
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) rec.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); };
  rec.push_back(static_cast<char>(dir));
  u16(flags);
  u32(seq);
  u32(ts);
  u16(static_cast<uint16_t>(wn));
  rec.append(reinterpret_cast<const char*>(wire), wn);
  u16(0);  // no local payload
  char name[16];
  snprintf(name, sizeof(name), "/%08lu.msg", static_cast<unsigned long>(seq));
  FILE* f = fopen((dirPath + name).c_str(), "wb");
  if (f == nullptr || fwrite(rec.data(), 1, rec.size(), f) != rec.size()) { printf("legacy write failed\n"); exit(3); }
  fclose(f);
  MessageRef ref{};
  strncpy(ref.group_code, group, sizeof(ref.group_code) - 1);
  strncpy(ref.contact_key, contact, sizeof(ref.contact_key) - 1);
  ref.sequence = seq;
  StoredMessageView v;
  if (!MessageStore::loadMessage(ref, &v) || v.header.direction != dir || v.header.flags != flags ||
      v.header.effective_sort_timestamp != ts || v.header.local_write_sequence != seq ||
      strcmp(v.envelope.message_id, env.message_id) != 0 || v.envelope.message_type != env.message_type) {
    printf("legacy record does not read back through MessageStore::loadMessage (format drift)\n");
    exit(3);
  }
  MessageStore::notifyExternalStorageMutationAttempted();  // written outside the store: its caches are stale
  return ref;
}
