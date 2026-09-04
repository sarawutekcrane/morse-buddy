#include "core/identity.h"

#include <Arduino.h>
#include <WiFi.h>

#include "core/storage_init.h"

namespace {
char g_deviceId[13] = "000000000000";
}  // namespace

namespace Identity {

void init() {
  // WiFi.macAddress() returns a valid MAC only once the driver has a mode
  // set; WIFI_STA here is a one-time, connection-free initialization so
  // device_id is available immediately, before the WiFi manager ever
  // calls WiFi.begin().
  WiFi.mode(WIFI_STA);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(g_deviceId, sizeof(g_deviceId), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
           mac[5]);
}

const char* deviceId() { return g_deviceId; }

void nextId(char* outBuf, size_t outBufSize) {
  Preferences& p = Storage::core();
  uint32_t counter = p.getULong("idCounter", 0) + 1;
  p.putULong("idCounter", counter);
  snprintf(outBuf, outBufSize, "%s-%lu", g_deviceId, static_cast<unsigned long>(counter));
}

}  // namespace Identity
