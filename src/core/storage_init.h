#pragma once
// NVS (Preferences) namespace helpers + LittleFS mount (Addendum sections
// 4.4 and 8, Phase 1 section 14).

#include <Preferences.h>
#include <stdint.h>

namespace Storage {

// Mounts LittleFS (custom "littlefs" partition) with the required
// error/acknowledge/retry/format-confirm flow, then opens every compact
// NVS namespace. Call once from setup(), after Display::init() and
// Input::init() (the failure flow draws a blocking prompt on the TFT).
void init();

// Compact NVS namespaces (Addendum section 4.4). Kept open for the
// firmware's lifetime; group/contact tables are stored as packed
// blobs/arrays inside these, never as long per-item keys.
Preferences& core();
Preferences& wifi();
Preferences& group();
Preferences& recent();
Preferences& enigma();
Preferences& notify();
Preferences& solo();

// Recursively removes /messages/<group_code> if it exists. Safe to call
// even when no message has ever been written for that group yet.
void removeGroupDirectoryIfPresent(const char* group_code);

}  // namespace Storage
