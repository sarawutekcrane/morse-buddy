// Morse Buddy firmware — Phase 1 (menu shell, registries, Settings, WiFi
// credential provisioning, battery, Sleep). See Phase_0_Overview_FINAL.md
// and Technical_Architecture_Addendum_FINAL.md for the full specification.

#include <Arduino.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/power.h"
#include "core/settings.h"
#include "core/sleep.h"
#include "core/storage_init.h"

namespace {
// Lightweight main-loop iteration timing instrumentation (Hardware
// Validation Fix #1, evidence requirement: report a concrete before/after
// loop-timing measurement, not just a subjective "feels better"). Adds one
// micros() read and a running min/max/avg per iteration -- negligible
// overhead -- and logs a summary over Serial roughly once a second. A
// short/consistent avg with a low max is evidence the per-loop TFT
// redraw work that used to run unconditionally (full clearContentArea() +
// full-menu redraw + status bar redraw every tick, before this fix) is
// gone; a long max spike would mean something is still blocking. Left in
// for this hardware-validation build so real-hardware numbers can be
// captured directly from the Serial monitor; safe to strip after retest.
uint32_t g_loopLastMicros = 0;
uint32_t g_loopMinUs = 0xFFFFFFFF;
uint32_t g_loopMaxUs = 0;
uint32_t g_loopSumUs = 0;
uint32_t g_loopCount = 0;
uint32_t g_loopWindowStartMs = 0;

void tickLoopTimingInstrumentation() {
  uint32_t nowUs = micros();
  if (g_loopLastMicros != 0) {
    uint32_t delta = nowUs - g_loopLastMicros;
    if (delta < g_loopMinUs) g_loopMinUs = delta;
    if (delta > g_loopMaxUs) g_loopMaxUs = delta;
    g_loopSumUs += delta;
    g_loopCount++;
  }
  g_loopLastMicros = nowUs;

  uint32_t nowMs = millis();
  if (nowMs - g_loopWindowStartMs >= 1000 && g_loopCount > 0) {
    Serial.printf("[loop] iters=%u avg=%uus min=%uus max=%uus\n", static_cast<unsigned>(g_loopCount),
                  static_cast<unsigned>(g_loopSumUs / g_loopCount), static_cast<unsigned>(g_loopMinUs),
                  static_cast<unsigned>(g_loopMaxUs));
    g_loopMinUs = 0xFFFFFFFF;
    g_loopMaxUs = 0;
    g_loopSumUs = 0;
    g_loopCount = 0;
    g_loopWindowStartMs = nowMs;
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);

  Display::init();
  Input::init();

  // May draw a blocking error/retry/format-confirm prompt on the TFT if
  // LittleFS fails to mount (Addendum section 8).
  Storage::init();

  // Reads the MAC and loads the persistent id_counter from mb_core; must
  // run after Storage::init() (identity.h's own documented contract) and
  // before initRegisteredServices() below, since mqtt_manager's
  // AppService::init already calls Identity::deviceId() while building
  // each group's MQTT client ID.
  Identity::init();

  Power::init();
  Sleep::init(5);  // default; Settings::init() below applies the persisted value

  Settings::init();

  Menu::init();
  if (!Settings::hasAnyWifiConfigured()) {
    // First boot with no WiFi slots: enter setup before Main Menu is shown.
    // Cancelling/failing falls back to Main Menu Offline (Phase 1 section 6).
    Menu::pushScreen(Settings::screenWifiSlots);
  }

  // Later phases register their own background services (WiFi/MQTT/NTP in
  // Phase 2, tone/audio in Phase 3, Radio/Race in Phase 4) from their own
  // translation units; this call site never needs to change.
  initRegisteredServices();
}

void loop() {
  tickLoopTimingInstrumentation();
  tickRegisteredServices();
  Power::tick();
  Menu::tick();
  Sleep::update();
}
