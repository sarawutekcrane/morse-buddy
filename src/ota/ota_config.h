#pragma once
// Compile-time-only OTA server configuration (Phase 5 section 9). Remote
// Configuration is out of scope for this phase: none of this is editable
// from Morse Buddy Settings, and no other file should hardcode a second
// copy of any of these values.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ota/firmware_version.h"

namespace OtaConfig {

// A real production OTA host + matching root CA (ota_cert.h) have not been
// supplied yet. This placeholder intentionally does not resolve to
// anything: isPlaceholderServer() below gates every network attempt so the
// Firmware Update screen shows "Update Server / Not Configured" instead of
// trying (and failing) a real connection. Replace this string with the
// real HTTPS origin when one exists; do not remove the guard itself.
constexpr const char* kBaseUrl = "https://ota.invalid.example";
constexpr const char* kManifestPath = "/morse-buddy/manifest.txt";
constexpr const char* kHardwareId = FW_HARDWARE_ID;

// Phase 5 section 10: hard manifest size cap.
constexpr size_t kManifestMaxSize = 2048;

// Phase 5 section 15: bounded streaming chunk, never a ~1MB RAM buffer.
constexpr size_t kDownloadChunkSize = 4096;

// Phase 5 section 26.
constexpr uint32_t kHttpsConnectTimeoutMs = 10000;
constexpr uint32_t kReadNoProgressTimeoutMs = 15000;
constexpr uint32_t kOverallUpdateTimeoutMs = 5UL * 60UL * 1000UL;

// Phase 5 section 17.
constexpr uint8_t kMinBatteryPercent = 30;

// Phase 5 section 18 (provisional; only tunable after real-device testing).
constexpr uint32_t kMinFreeHeapBytes = 80UL * 1024UL;

inline bool isPlaceholderServer() { return strcmp(kBaseUrl, "https://ota.invalid.example") == 0; }

}  // namespace OtaConfig
