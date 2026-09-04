#include "core/crc32.h"

#include <string.h>

namespace Crc32 {

uint32_t compute(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

uint32_t computeStr(const char* s) {
  return compute(reinterpret_cast<const uint8_t*>(s), strlen(s));
}

}  // namespace Crc32
