#include "core/sleep.h"

#include <Arduino.h>
#include <esp_sleep.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/power.h"

namespace {
uint8_t g_timeoutMinutes = 5;  // 0 = Disabled
uint32_t g_lastActivityMs = 0;
uint8_t g_inhibitMask = 0;  // InhibitReason bits; sleep.h

void enterDeepSleep() {
  runBeforeSleepHooks();

  Display::setBacklightPercent(0);
  Power::setDividerEnabled(false);

  // No wake source configured: wake is power-switch cycle only (Addendum
  // section 19). Never call this from an ISR.
  esp_deep_sleep_start();
}
}  // namespace

namespace Sleep {

void init(uint8_t timeoutMinutes) {
  g_timeoutMinutes = timeoutMinutes;
  g_lastActivityMs = millis();
}

void setTimeoutMinutes(uint8_t minutes) {
  g_timeoutMinutes = minutes;
  g_lastActivityMs = millis();
}

uint8_t getTimeoutMinutes() { return g_timeoutMinutes; }

void notifyActivity() { g_lastActivityMs = millis(); }

void setInhibited(InhibitReason reason, bool inhibited) {
  uint8_t bit = static_cast<uint8_t>(reason);
  bool wasSet = (g_inhibitMask & bit) != 0;
  if (inhibited) {
    g_inhibitMask |= bit;
    return;
  }
  g_inhibitMask &= static_cast<uint8_t>(~bit);
  // Reuse the existing activity-reset API rather than a second timer: when
  // a reason that was set ends (Maintenance Mode ending, whether OTA
  // succeeded, failed or was aborted; a PTT session ending -- the release
  // edge itself is not an activity event) the inactivity clock gets a
  // fresh baseline, so the device never resumes counting from a stale
  // timestamp and falls straight into deep sleep. Clearing a reason that
  // was not set (a reset path with no live session) changes nothing.
  if (wasSet) notifyActivity();
}

bool isInhibited() { return g_inhibitMask != 0; }

void setSuspended(bool suspended) { setInhibited(InhibitReason::OTA_MAINTENANCE, suspended); }

void update() {
  if (g_inhibitMask != 0) return;
  if (g_timeoutMinutes == 0) return;  // Disabled

  uint32_t timeoutMs = static_cast<uint32_t>(g_timeoutMinutes) * 60000UL;
  if (millis() - g_lastActivityMs >= timeoutMs) {
    enterDeepSleep();
  }
}

}  // namespace Sleep
