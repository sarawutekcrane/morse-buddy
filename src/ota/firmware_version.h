#pragma once
// Application identity (Phase 5 section 8). Compiled into the firmware
// image; the OTA manifest is compared against these values, never the
// other way around. Bump FW_BUILD_NUMBER (and normally FW_VERSION) on
// every release that gets packaged through tools/make_ota_release.py.
//
// TEST ONLY — NEVER MERGE INTO PRODUCTION
// This branch (test/ota-rollback-health-fail) carries a dedicated test
// identity (FW_BUILD_NUMBER=9001) so an intentionally-failing OTA test
// image can never be confused with, or leave last_failed_ota_build state
// that could affect, real production Build 2 (build=2) or Build 3
// (build=3). See ota_manager.cpp's runHealthCheck() for the forced
// failure this identity exists to isolate.

#include <stdint.h>

// Human-facing. Not used for update-order comparison.
constexpr const char* FW_VERSION = "1.0.0-RBTEST";

// Authoritative update-order value. Must be strictly greater on every
// released firmware than on the one before it (Phase 5 section 8: never
// use a timestamp alone as firmware identity).
constexpr uint32_t FW_BUILD_NUMBER = 9001;

// Must match exactly (case-sensitive, full string compare) between the
// manifest's hardware= field and this device before an OTA is accepted.
constexpr const char* FW_HARDWARE_ID = "MORSE_BUDDY_ESP32_114_V1";

// Manifest wire schema this firmware understands (Phase 5 section 10).
constexpr uint32_t OTA_SCHEMA = 1;
