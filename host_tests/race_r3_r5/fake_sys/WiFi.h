#pragma once
// HOST STUB (not production): syntax-check surface only.
#include <stdint.h>
enum { WIFI_STA = 1 };
class WiFiClient {};
struct HostWiFi { void mode(int) {} void macAddress(uint8_t* m) { const uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xAA, 0xAA}; for (int i = 0; i < 6; i++) m[i] = mac[i]; } template <class... A> void begin(A...) {} };
extern HostWiFi WiFi;
