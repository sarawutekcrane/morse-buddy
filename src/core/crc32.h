#pragma once
// Standard CRC32 (IEEE 802.3 / zlib polynomial). Used for the stable MQTT
// Client ID (Addendum section 4.2) and reused by Phase 3 for Enigma key
// fingerprinting (Addendum section 10.7), so it lives here rather than
// being duplicated per-caller.

#include <stddef.h>
#include <stdint.h>

namespace Crc32 {

uint32_t compute(const uint8_t* data, size_t len);
uint32_t computeStr(const char* s);

}  // namespace Crc32
