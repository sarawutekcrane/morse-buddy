#include "core/sleep.h"

#include <Arduino.h>
#include <esp_sleep.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/power.h"

namespace {
uint8_t g_timeoutMinutes = 5;  // 0 = Disabled
uint32_t g_lastActivityMs = 0;
bool g_suspended = false;

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

void setSuspended(bool suspended) {
  g_suspended = suspended;
  if (!suspended) g_lastActivityMs = millis();
}

void update() {
  if (g_suspended) return;
  if (g_timeoutMinutes == 0) return;  // Disabled

  uint32_t timeoutMs = static_cast<uint32_t>(g_timeoutMinutes) * 60000UL;
  if (millis() - g_lastActivityMs >= timeoutMs) {
    enterDeepSleep();
  }
}

}  // namespace Sleep
