#include "core/identity.h"

#include <Arduino.h>
#include <WiFi.h>

#include "core/storage_init.h"

namespace {
char g_deviceId[13] = "000000000000";

// Reads a stored u32. getULong() reports no status: a failed read returns
// its default. So the key is read twice with different defaults and the value
// is accepted only if both reads agree -- a failed read can then agree only
// when the stored value equals its default, which is the value returned.
// Any disagreement (either or both reads failed) is a failure: *out is left
// unchanged.
bool readStored(Preferences& p, const char* key, uint32_t* out) {
  uint32_t a = p.getULong(key, 0);
  uint32_t b = p.getULong(key, 0xFFFFFFFFu);
  if (a != b) return false;
  *out = a;
  return true;
}

// Race sequence numbers (reserveCounter) used idCounter before raceSeq
// existed, so idCounter's value bounds every Race number that may have been
// handed out. That bound is copied to raceSeq once, before idCounter may
// change at all (nextId() would otherwise raise it harmlessly but also wrap
// it to 0, or overwrite it after a failed read). True once raceSeq exists.
// Only idCounter's current value is known: a wrap that already happened on
// an earlier firmware is not detectable, and no earlier maximum is recovered.
bool raceFloorStored(Preferences& p) {
  if (p.isKey("raceSeq")) return true;
  uint32_t floor = 0;
  if (p.isKey("idCounter") && !readStored(p, "idCounter", &floor)) return false;  // unreadable: retry later
  return p.putULong("raceSeq", floor) == sizeof(uint32_t);
}
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
  raceFloorStored(Storage::core());  // migrate before anything can call nextId(); retried there if it fails
}

const char* deviceId() { return g_deviceId; }

void nextId(char* outBuf, size_t outBufSize) {
  Preferences& p = Storage::core();
  uint32_t counter = p.getULong("idCounter", 0) + 1;
  // idCounter is not stored until Race's floor is (as when this write fails:
  // the same id may then be returned again).
  if (raceFloorStored(p)) p.putULong("idCounter", counter);
  snprintf(outBuf, outBufSize, "%s-%lu", g_deviceId, static_cast<unsigned long>(counter));
}

bool reserveCounter(uint32_t* out) {
  Preferences& p = Storage::core();
  if (!raceFloorStored(p)) return false;  // migration not stored yet (retried on every call)
  uint32_t current;
  if (!readStored(p, "raceSeq", &current)) return false;  // unreadable: never restart from a default
  if (current == 0xFFFFFFFFu) return false;  // exhausted: never wrap
  uint32_t next = current + 1;
  if (p.putULong("raceSeq", next) != sizeof(uint32_t)) return false;  // not durably reserved
  *out = next;
  return true;
}

}  // namespace Identity
