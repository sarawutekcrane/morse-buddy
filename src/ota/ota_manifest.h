#pragma once
// Strict ASCII OTA manifest parser (Phase 5 section 10). No JSON, matching
// the rest of Morse Buddy's wire formats. Every field lands in a
// fixed-size buffer; parse() never reads or writes past the caller-given
// length, and a value that would not fit its field's buffer is rejected
// rather than silently truncated.
//
// Exact wire format:
//   MBOTA1
//   version=1.1.0
//   build=2
//   hardware=MORSE_BUDDY_ESP32_114_V1
//   size=1000123
//   sha256=<64 lowercase/uppercase hex chars>
//   path=/morse-buddy/releases/1.1.0/firmware.bin

#include <stddef.h>
#include <stdint.h>

namespace OtaManifest {

constexpr size_t kVersionLen = 32;
constexpr size_t kHardwareLen = 48;
constexpr size_t kSha256HexLen = 64;
constexpr size_t kPathLen = 160;

struct Manifest {
  char version[kVersionLen];
  uint32_t build;
  char hardware[kHardwareLen];
  uint32_t size;
  char sha256_hex[kSha256HexLen + 1];
  char path[kPathLen];
};

enum class ParseResult : uint8_t {
  OK,
  TOO_LARGE,
  BAD_MAGIC,
  UNSUPPORTED_SCHEMA,
  MISSING_FIELD,
  DUPLICATE_FIELD,
  MALFORMED_NUMBER,
  MALFORMED_SHA256,
  BAD_PATH,
  FIELD_TOO_LONG,
};

// `buf` need not be NUL-terminated; exactly `len` bytes are considered
// (the caller is responsible for enforcing the network-side size cap
// before calling this, e.g. OtaConfig::kManifestMaxSize).
ParseResult parse(const char* buf, size_t len, Manifest* out);

const char* resultToString(ParseResult r);

}  // namespace OtaManifest
