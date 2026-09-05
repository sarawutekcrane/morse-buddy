#include "ota/ota_https.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <new>
#include <string.h>

#include "ota/ota_cert.h"
#include "ota/ota_config.h"

namespace OtaHttps {

namespace {

void hexEncode(const uint8_t* digest, size_t len, char* out) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = kHex[(digest[i] >> 4) & 0xF];
    out[i * 2 + 1] = kHex[digest[i] & 0xF];
  }
  out[len * 2] = '\0';
}

}  // namespace

const char* resultToString(Result r) {
  switch (r) {
    case Result::OK: return "OK";
    case Result::HTTPS_CONNECT_FAILED: return "Secure connection failed";
    case Result::TLS_VERIFY_FAILED: return "Secure connection failed";
    case Result::MANIFEST_NOT_FOUND: return "Update server error";
    case Result::MANIFEST_TOO_LARGE: return "Update server error";
    case Result::MANIFEST_INVALID: return "Update server error";
    case Result::IMAGE_TOO_LARGE: return "Firmware too large";
    case Result::CONTENT_LENGTH_MISMATCH: return "Connection lost";
    case Result::DOWNLOAD_TIMEOUT: return "Connection lost";
    case Result::DOWNLOAD_INTERRUPTED: return "Connection lost";
    case Result::WRITE_FAILED: return "Write failed";
    case Result::SHA256_MISMATCH: return "Firmware verification failed";
    case Result::IMAGE_FINALIZE_FAILED: return "Update failed";
    case Result::BOOT_PARTITION_FAILED: return "Update failed";
  }
  return "Update failed";
}

Result fetchManifest(OtaManifest::Manifest* outManifest) {
  if (OtaConfig::isPlaceholderServer()) return Result::HTTPS_CONNECT_FAILED;

  WiFiClientSecure client;
  client.setCACert(kOtaRootCaPem);

  HTTPClient http;
  http.setConnectTimeout(OtaConfig::kHttpsConnectTimeoutMs);
  http.setTimeout(OtaConfig::kReadNoProgressTimeoutMs);

  char url[256];
  snprintf(url, sizeof(url), "%s%s", OtaConfig::kBaseUrl, OtaConfig::kManifestPath);
  Serial.printf("[ota] fetching manifest: %s\n", url);
  if (!http.begin(client, url)) {
    Serial.println("[ota] manifest fetch: http.begin() failed (malformed URL or TLS setup failure)");
    return Result::HTTPS_CONNECT_FAILED;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[ota] manifest fetch failed: HTTP GET returned %d\n", code);
    http.end();
    if (code == 404) return Result::MANIFEST_NOT_FOUND;
    return Result::HTTPS_CONNECT_FAILED;  // covers TLS handshake/verify failures surfaced by HTTPClient too
  }

  int contentLen = http.getSize();
  if (contentLen <= 0 || static_cast<size_t>(contentLen) > OtaConfig::kManifestMaxSize) {
    Serial.printf("[ota] manifest fetch failed: Content-Length=%d exceeds max=%u or is missing/chunked\n", contentLen,
                 static_cast<unsigned>(OtaConfig::kManifestMaxSize));
    http.end();
    return Result::MANIFEST_TOO_LARGE;
  }

  char* buf = new (std::nothrow) char[static_cast<size_t>(contentLen)];
  if (buf == nullptr) {
    Serial.printf("[ota] manifest fetch failed: could not allocate %d bytes\n", contentLen);
    http.end();
    return Result::MANIFEST_TOO_LARGE;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t lastProgressMs = millis();
  while (got < static_cast<size_t>(contentLen)) {
    if (millis() - lastProgressMs > OtaConfig::kReadNoProgressTimeoutMs) {
      Serial.printf("[ota] manifest fetch failed: no progress for >%ums (%u/%d bytes received)\n",
                   static_cast<unsigned>(OtaConfig::kReadNoProgressTimeoutMs), static_cast<unsigned>(got), contentLen);
      delete[] buf;
      http.end();
      return Result::DOWNLOAD_TIMEOUT;
    }
    if (!client.connected() && stream->available() <= 0) break;
    int avail = stream->available();
    if (avail <= 0) {
      delay(1);
      continue;
    }
    int n = stream->readBytes(buf + got, static_cast<size_t>(contentLen) - got);
    if (n > 0) {
      got += static_cast<size_t>(n);
      lastProgressMs = millis();
    }
  }
  http.end();

  if (got != static_cast<size_t>(contentLen)) {
    Serial.printf("[ota] manifest fetch failed: connection closed after %u/%d bytes\n", static_cast<unsigned>(got),
                 contentLen);
    delete[] buf;
    return Result::DOWNLOAD_INTERRUPTED;
  }

  OtaManifest::ParseResult pr = OtaManifest::parse(buf, got, outManifest);
  delete[] buf;
  if (pr != OtaManifest::ParseResult::OK) {
    Serial.printf("[ota] manifest parse failed: %s\n", OtaManifest::resultToString(pr));
    return Result::MANIFEST_INVALID;
  }
  Serial.printf("[ota] manifest OK: version=%s build=%u hardware=%s size=%u\n", outManifest->version,
               static_cast<unsigned>(outManifest->build), outManifest->hardware,
               static_cast<unsigned>(outManifest->size));
  return Result::OK;
}

Result downloadAndInstall(const OtaManifest::Manifest& manifest, ProgressFn onProgress) {
  if (OtaConfig::isPlaceholderServer()) return Result::HTTPS_CONNECT_FAILED;

  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (updatePartition == nullptr) {
    Serial.println("[ota] download aborted: no inactive OTA partition available");
    return Result::BOOT_PARTITION_FAILED;
  }
  if (manifest.size == 0 || manifest.size > updatePartition->size) {
    Serial.printf("[ota] download aborted: manifest size=%u exceeds inactive partition '%s' size=%u\n",
                 static_cast<unsigned>(manifest.size), updatePartition->label,
                 static_cast<unsigned>(updatePartition->size));
    return Result::IMAGE_TOO_LARGE;
  }

  WiFiClientSecure client;
  client.setCACert(kOtaRootCaPem);

  HTTPClient http;
  http.setConnectTimeout(OtaConfig::kHttpsConnectTimeoutMs);
  http.setTimeout(OtaConfig::kReadNoProgressTimeoutMs);

  char url[300];
  snprintf(url, sizeof(url), "%s%s", OtaConfig::kBaseUrl, manifest.path);
  Serial.printf("[ota] downloading firmware: %s -> partition '%s'\n", url, updatePartition->label);
  if (!http.begin(client, url)) {
    Serial.println("[ota] download aborted: http.begin() failed (malformed URL or TLS setup failure)");
    return Result::HTTPS_CONNECT_FAILED;
  }

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[ota] download aborted: HTTP GET returned %d\n", code);
    http.end();
    return Result::HTTPS_CONNECT_FAILED;
  }

  // No Content-Length (e.g. chunked Transfer-Encoding) reads back as -1
  // from HTTPClient::getSize() and is rejected here exactly like a mismatch
  // (Phase 5 section 56.3: chunked firmware responses must be rejected).
  int contentLen = http.getSize();
  if (contentLen <= 0 || static_cast<size_t>(contentLen) != manifest.size) {
    Serial.printf("[ota] download aborted: Content-Length=%d does not match manifest size=%u\n", contentLen,
                 static_cast<unsigned>(manifest.size));
    http.end();
    return Result::CONTENT_LENGTH_MISMATCH;
  }

  if (!Update.begin(manifest.size, U_FLASH)) {
    Serial.printf("[ota] download aborted: Update.begin() failed, error=%s\n", Update.errorString());
    http.end();
    return Result::WRITE_FAILED;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);  // 0 selects SHA-256 (not SHA-224)

  uint8_t* chunk = new (std::nothrow) uint8_t[OtaConfig::kDownloadChunkSize];
  if (chunk == nullptr) {
    mbedtls_sha256_free(&sha);
    Update.abort();
    http.end();
    return Result::WRITE_FAILED;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint32_t lastProgressMs = millis();
  uint32_t overallStartMs = millis();
  size_t lastReportedPercent = 101;
  Result failResult = Result::OK;

  while (written < manifest.size) {
    uint32_t now = millis();
    if (now - overallStartMs > OtaConfig::kOverallUpdateTimeoutMs) {
      failResult = Result::DOWNLOAD_TIMEOUT;
      break;
    }
    if (now - lastProgressMs > OtaConfig::kReadNoProgressTimeoutMs) {
      failResult = Result::DOWNLOAD_TIMEOUT;
      break;
    }
    if (!client.connected() && stream->available() <= 0) {
      failResult = Result::DOWNLOAD_INTERRUPTED;
      break;
    }

    int avail = stream->available();
    if (avail <= 0) {
      delay(1);
      yield();
      continue;
    }

    size_t want = manifest.size - written;
    if (want > OtaConfig::kDownloadChunkSize) want = OtaConfig::kDownloadChunkSize;
    int n = stream->readBytes(reinterpret_cast<char*>(chunk), want);
    if (n <= 0) {
      delay(1);
      continue;
    }

    mbedtls_sha256_update_ret(&sha, chunk, static_cast<size_t>(n));
    size_t w = Update.write(chunk, static_cast<size_t>(n));
    if (w != static_cast<size_t>(n)) {
      Serial.printf("[ota] download aborted: Update.write() wrote %u/%u bytes, error=%s\n", static_cast<unsigned>(w),
                   static_cast<unsigned>(n), Update.errorString());
      failResult = Result::WRITE_FAILED;
      break;
    }

    written += static_cast<size_t>(n);
    lastProgressMs = now;

    // Watchdog-safe chunked writes (Phase 5 section 56.1): yield after
    // every chunk so a long flash-write sequence never trips the Task
    // Watchdog Timer.
    yield();

    if (onProgress != nullptr) {
      size_t percent = (written * 100) / manifest.size;
      if (percent != lastReportedPercent) {
        onProgress(written, manifest.size);
        lastReportedPercent = percent;
      }
    }
  }

  delete[] chunk;
  http.end();

  if (failResult != Result::OK) {
    Serial.printf("[ota] download failed after %u/%u bytes: %s\n", static_cast<unsigned>(written),
                 static_cast<unsigned>(manifest.size), resultToString(failResult));
    mbedtls_sha256_free(&sha);
    Update.abort();
    return failResult;
  }
  if (written != manifest.size) {
    Serial.printf("[ota] download aborted: loop exited with %u/%u bytes written\n", static_cast<unsigned>(written),
                 static_cast<unsigned>(manifest.size));
    mbedtls_sha256_free(&sha);
    Update.abort();
    return Result::CONTENT_LENGTH_MISMATCH;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);

  char digestHex[65];
  hexEncode(digest, sizeof(digest), digestHex);
  Serial.printf("[ota] calculated sha256=%s expected=%s\n", digestHex, manifest.sha256_hex);

  if (strcasecmp(digestHex, manifest.sha256_hex) != 0) {
    Serial.println("[ota] download aborted: SHA-256 mismatch, image will not be activated");
    Update.abort();
    return Result::SHA256_MISMATCH;
  }

  // Update.end(true) performs the ESP32 image/header validation, then
  // esp_ota_set_boot_partition() on the inactive slot we just wrote.
  if (!Update.end(true)) {
    Serial.printf("[ota] download aborted: Update.end() failed, error=%s\n", Update.errorString());
    return Result::IMAGE_FINALIZE_FAILED;
  }

  Serial.printf("[ota] Update.end() succeeded; new boot partition selected\n");
  return Result::OK;
}

}  // namespace OtaHttps
