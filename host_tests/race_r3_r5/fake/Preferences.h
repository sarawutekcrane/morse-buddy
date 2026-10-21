#pragma once
// HOST STUB (not production). In-memory Preferences (per instance, i.e. per
// NVS namespace) with the ESP32 Arduino 2.0.17 semantics the firmware relies
// on (verified against Preferences.cpp): putBytes returns len or 0 on
// failure; getBytes returns 0 if absent or the buffer is too small; isKey.
// Records the last bytes written per key so host tests can compare persisted
// blobs byte-for-byte, and lets a test fail writes.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <functional>
#include <map>
#include <string>
struct PrefsCapture { static std::map<std::string, std::string>& last() { static std::map<std::string, std::string> m; return m; } };
class Preferences {
 public:
  std::map<std::string, std::string> kv;
  std::function<bool(const char*)> failWrite;  // true -> this putBytes fails (nothing written)
  std::function<bool(const char*)> failRead;   // true -> this read fails (getULong returns its default, like nvs_get_u32 errors)
  int writes = 0;
  size_t putBytes(const char* key, const void* v, size_t len) {
    writes++;
    if (failWrite && failWrite(key)) return 0;
    PrefsCapture::last()[key].assign(static_cast<const char*>(v), len);
    kv[key].assign(static_cast<const char*>(v), len);
    return len;
  }
  size_t getBytes(const char* key, void* buf, size_t maxLen) {
    auto it = kv.find(key);
    if (it == kv.end() || it->second.size() > maxLen) return 0;
    memcpy(buf, it->second.data(), it->second.size());
    return it->second.size();
  }
  bool isKey(const char* key) { return kv.count(key) != 0; }
  bool begin(const char*, bool) { return true; }
  uint32_t getULong(const char* key, uint32_t def) {
    uint32_t v = def;
    if (failRead && failRead(key)) return def;
    if (kv.count(key) && kv[key].size() == 4) memcpy(&v, kv[key].data(), 4);
    return v;
  }
  size_t putULong(const char* key, uint32_t v) { return putBytes(key, &v, 4); }
};
