#include "ota/ota_manifest.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ota/ota_config.h"

namespace OtaManifest {

namespace {

constexpr uint8_t kFieldVersion = 1u << 0;
constexpr uint8_t kFieldBuild = 1u << 1;
constexpr uint8_t kFieldHardware = 1u << 2;
constexpr uint8_t kFieldSize = 1u << 3;
constexpr uint8_t kFieldSha256 = 1u << 4;
constexpr uint8_t kFieldPath = 1u << 5;
constexpr uint8_t kAllMandatoryFields =
    kFieldVersion | kFieldBuild | kFieldHardware | kFieldSize | kFieldSha256 | kFieldPath;

constexpr size_t kMaxLineLen = 200;

// Copies `value` (length valueLen, not NUL-terminated) into `out` (capacity
// outCap) with a trailing NUL. False if it would not fit.
bool copyBounded(const char* value, size_t valueLen, char* out, size_t outCap) {
  if (valueLen >= outCap) return false;
  memcpy(out, value, valueLen);
  out[valueLen] = '\0';
  return true;
}

// Strict base-10 parse of a bounded, NUL-terminated token: digits only, no
// sign, no whitespace, no leading/trailing garbage, must fit uint32_t.
bool parseStrictU32(const char* value, uint32_t* outVal) {
  if (value[0] == '\0') return false;
  for (const char* p = value; *p != '\0'; p++) {
    if (!isdigit(static_cast<unsigned char>(*p))) return false;
  }
  errno = 0;
  char* end = nullptr;
  unsigned long v = strtoul(value, &end, 10);
  if (errno == ERANGE || end == value || *end != '\0') return false;
  if (v > 0xFFFFFFFFUL) return false;
  *outVal = static_cast<uint32_t>(v);
  return true;
}

bool isHex64(const char* value) {
  size_t n = strlen(value);
  if (n != OtaManifest::kSha256HexLen) return false;
  for (size_t i = 0; i < n; i++) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool pathLooksSafe(const char* value) {
  if (value[0] != '/') return false;
  if (strstr(value, "..") != nullptr) return false;
  if (strstr(value, "://") != nullptr) return false;
  return true;
}

}  // namespace

ParseResult parse(const char* buf, size_t len, Manifest* out) {
  if (buf == nullptr || out == nullptr) return ParseResult::MISSING_FIELD;
  if (len == 0) return ParseResult::BAD_MAGIC;
  if (len > OtaConfig::kManifestMaxSize) return ParseResult::TOO_LARGE;

  memset(out, 0, sizeof(*out));
  uint8_t seen = 0;

  size_t pos = 0;
  bool firstLine = true;

  while (pos < len) {
    // Extract one line into a bounded local buffer, never reading past len.
    size_t lineStart = pos;
    while (pos < len && buf[pos] != '\n') pos++;
    size_t lineLen = pos - lineStart;
    if (pos < len) pos++;  // consume '\n'

    if (lineLen > 0 && buf[lineStart + lineLen - 1] == '\r') lineLen--;  // tolerate CRLF
    if (lineLen == 0) continue;                                          // blank line: skip
    if (lineLen > kMaxLineLen) return ParseResult::FIELD_TOO_LONG;

    char line[kMaxLineLen + 1];
    memcpy(line, buf + lineStart, lineLen);
    line[lineLen] = '\0';

    if (firstLine) {
      firstLine = false;
      if (strncmp(line, "MBOTA", 5) != 0) return ParseResult::BAD_MAGIC;
      if (strcmp(line, "MBOTA1") != 0) return ParseResult::UNSUPPORTED_SCHEMA;
      continue;
    }

    const char* eq = strchr(line, '=');
    if (eq == nullptr) continue;  // not a recognizable field; ignore (forward compatibility)
    size_t keyLen = static_cast<size_t>(eq - line);
    const char* value = eq + 1;
    size_t valueLen = lineLen - keyLen - 1;

    if (keyLen == 7 && strncmp(line, "version", 7) == 0) {
      if (seen & kFieldVersion) return ParseResult::DUPLICATE_FIELD;
      if (!copyBounded(value, valueLen, out->version, sizeof(out->version))) return ParseResult::FIELD_TOO_LONG;
      seen |= kFieldVersion;
    } else if (keyLen == 5 && strncmp(line, "build", 5) == 0) {
      if (seen & kFieldBuild) return ParseResult::DUPLICATE_FIELD;
      char tmp[16];
      if (!copyBounded(value, valueLen, tmp, sizeof(tmp))) return ParseResult::MALFORMED_NUMBER;
      if (!parseStrictU32(tmp, &out->build)) return ParseResult::MALFORMED_NUMBER;
      seen |= kFieldBuild;
    } else if (keyLen == 8 && strncmp(line, "hardware", 8) == 0) {
      if (seen & kFieldHardware) return ParseResult::DUPLICATE_FIELD;
      if (!copyBounded(value, valueLen, out->hardware, sizeof(out->hardware))) return ParseResult::FIELD_TOO_LONG;
      seen |= kFieldHardware;
    } else if (keyLen == 4 && strncmp(line, "size", 4) == 0) {
      if (seen & kFieldSize) return ParseResult::DUPLICATE_FIELD;
      char tmp[16];
      if (!copyBounded(value, valueLen, tmp, sizeof(tmp))) return ParseResult::MALFORMED_NUMBER;
      if (!parseStrictU32(tmp, &out->size)) return ParseResult::MALFORMED_NUMBER;
      if (out->size == 0) return ParseResult::MALFORMED_NUMBER;
      seen |= kFieldSize;
    } else if (keyLen == 6 && strncmp(line, "sha256", 6) == 0) {
      if (seen & kFieldSha256) return ParseResult::DUPLICATE_FIELD;
      if (!copyBounded(value, valueLen, out->sha256_hex, sizeof(out->sha256_hex))) return ParseResult::MALFORMED_SHA256;
      if (!isHex64(out->sha256_hex)) return ParseResult::MALFORMED_SHA256;
      seen |= kFieldSha256;
    } else if (keyLen == 4 && strncmp(line, "path", 4) == 0) {
      if (seen & kFieldPath) return ParseResult::DUPLICATE_FIELD;
      if (!copyBounded(value, valueLen, out->path, sizeof(out->path))) return ParseResult::BAD_PATH;
      if (!pathLooksSafe(out->path)) return ParseResult::BAD_PATH;
      seen |= kFieldPath;
    }
    // else: unknown non-critical field, ignored for forward compatibility.
  }

  if (firstLine) return ParseResult::BAD_MAGIC;  // empty manifest, no magic line at all
  if ((seen & kAllMandatoryFields) != kAllMandatoryFields) return ParseResult::MISSING_FIELD;
  return ParseResult::OK;
}

const char* resultToString(ParseResult r) {
  switch (r) {
    case ParseResult::OK: return "OK";
    case ParseResult::TOO_LARGE: return "manifest too large";
    case ParseResult::BAD_MAGIC: return "bad manifest magic";
    case ParseResult::UNSUPPORTED_SCHEMA: return "unsupported manifest schema";
    case ParseResult::MISSING_FIELD: return "manifest missing mandatory field";
    case ParseResult::DUPLICATE_FIELD: return "manifest has duplicate field";
    case ParseResult::MALFORMED_NUMBER: return "manifest has malformed numeric field";
    case ParseResult::MALFORMED_SHA256: return "manifest has malformed sha256";
    case ParseResult::BAD_PATH: return "manifest has unsafe path";
    case ParseResult::FIELD_TOO_LONG: return "manifest field too long";
  }
  return "unknown manifest error";
}

}  // namespace OtaManifest
