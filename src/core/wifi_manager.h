#pragma once
// Background WiFi reconnect manager + NTP (Addendum section 17, Phase 2
// section 1). Registers itself as a Background App Service (Addendum
// section 3.12) — main.cpp needs no changes.

#include <stdint.h>

namespace WifiManager {

bool isConnected();

bool isNtpSynced();
uint32_t getUnixTime();  // 0 if NTP not yet synced; never blocks

}  // namespace WifiManager
