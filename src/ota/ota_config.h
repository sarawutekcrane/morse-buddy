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

// Real production OTA host (GitHub Pages project site for the
// sarawutekcrane/morse-buddy-ota repo), verified independently: HTTPS,
// TLS 1.2, valid *.github.io leaf chaining to Let's Encrypt R... ->
// ISRG Root X1 (the pinned trust anchor in ota_cert.h). kBaseUrl is the
// true origin only (no path) so both the manifest and every firmware
// path from it -- which already carry their own "/morse-buddy-ota/..."
// prefix -- resolve correctly off the same kBaseUrl with no path
// duplication:
//   kBaseUrl + kManifestPath  == https://sarawutekcrane.github.io/morse-buddy-ota/manifest.txt
//   kBaseUrl + manifest.path  == https://sarawutekcrane.github.io/morse-buddy-ota/releases/<ver>/firmware.bin
constexpr const char* kBaseUrl = "https://sarawutekcrane.github.io";
constexpr const char* kManifestPath = "/morse-buddy-ota/manifest.txt";
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

// Unchanged logic, deliberately: this still compares kBaseUrl against the
// literal placeholder string it used to hold, not against "is kBaseUrl
// non-empty". Now that kBaseUrl is the real host, this comparison is
// simply always false, so the real host is treated as configured with no
// further change needed here -- and the guard reactivates on its own if
// kBaseUrl is ever reverted to the placeholder during future development.
inline bool isPlaceholderServer() { return strcmp(kBaseUrl, "https://ota.invalid.example") == 0; }

}  // namespace OtaConfig
