#pragma once
// HTTPS transport for OTA (Phase 5 sections 11-16). Owns the actual
// network I/O: fetching+parsing the manifest, and streaming the firmware
// body straight into the inactive OTA partition with SHA-256 computed
// while streaming. Never buffers a whole manifest/firmware image in a
// static/global buffer (Phase 5 section 48) — every buffer here is either
// a small bounded stack array or a heap allocation scoped to one call and
// freed before returning.
//
// ota_manager.cpp is the only caller; it owns the higher-level checks
// (WiFi/NTP/battery/heap/build-number/hardware-ID) that must all pass
// before either function here is invoked.

#include <stddef.h>
#include <stdint.h>

#include "ota/ota_manifest.h"

namespace OtaHttps {

enum class Result : uint8_t {
  OK = 0,
  HTTPS_CONNECT_FAILED,
  TLS_VERIFY_FAILED,
  MANIFEST_NOT_FOUND,
  MANIFEST_TOO_LARGE,
  MANIFEST_INVALID,
  IMAGE_TOO_LARGE,
  CONTENT_LENGTH_MISMATCH,
  DOWNLOAD_TIMEOUT,
  DOWNLOAD_INTERRUPTED,
  WRITE_FAILED,
  SHA256_MISMATCH,
  IMAGE_FINALIZE_FAILED,
  BOOT_PARTITION_FAILED,
};

const char* resultToString(Result r);

// Fetches OtaConfig::kBaseUrl + OtaConfig::kManifestPath over HTTPS (CA
// verified, never setInsecure()) and strictly parses it. `outManifest` is
// only meaningful when the return value is Result::OK.
Result fetchManifest(OtaManifest::Manifest* outManifest);

// Called at most once per integer-percent change so the caller can redraw
// the progress bar without hammering the TFT on every network packet.
using ProgressFn = void (*)(size_t bytesWritten, size_t expectedSize);

// Streams manifest.path (same trusted origin as the manifest) directly
// into the inactive OTA partition via the Update/esp_ota_ops APIs,
// verifying Content-Length == manifest.size before writing a single byte,
// rejecting chunked Transfer-Encoding responses (no Content-Length ==
// treated as a mismatch), and comparing a streaming SHA-256 against
// manifest.sha256_hex before Update.end() ever finalizes the image. Any
// failure aborts the in-progress Update and leaves the currently-running
// partition untouched and still bootable.
Result downloadAndInstall(const OtaManifest::Manifest& manifest, ProgressFn onProgress);

}  // namespace OtaHttps
