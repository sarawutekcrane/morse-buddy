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
  if (!Settings::hasConfiguredIdentity()) {
    // Hardware Fix #4.7 / Feature Fix #4.8: a device without a complete
    // Name + Color identity is not usable yet -- push this LAST so it
    // ends up top-most, above WiFi setup if that was also pushed
    // (required boot priority: 1. mandatory identity, 2. WiFi setup if
    // missing, 3. Main Menu). Unlike WiFi setup, this screen cannot be
    // cancelled past; see screenSetName(). hasConfiguredIdentity()
    // (name AND color) replaces the old name-only gate, but an existing
    // valid name from before Feature Fix #4.8 is never discarded --
    // screenSetName() itself handles that migration case by asking only
    // for a color.
    Menu::pushScreen(Settings::screenSetName);
  }

  // Later phases register their own background services (WiFi/MQTT/NTP in
  // Phase 2, tone/audio in Phase 3, Radio/Race in Phase 4) from their own
  // translation units; this call site never needs to change.
  initRegisteredServices();
}

// Hardware Diagnostic #4.9e Part B1: real-hardware testing after #4.9d
// still showed DOT/DASH, encoder rotation/push, and menu navigation ALL
// delayed together, globally, for 2-3 seconds -- since the independent
// input samplers are esp_timer-driven and already validated (#4.7b/#4.8b),
// a delay affecting every input type at once points at the Arduino main
// task itself being blocked somewhere in loop(), before/during
// Menu::tick(). This section-by-section timing (gated to >=20ms so normal
// fast ticks never print) plus a loop-to-loop gap check is pure
// instrumentation -- it changes no behavior, only whether a line is
// printed. See mqtt_manager.cpp/outbox.cpp/wifi_manager.cpp/
// radio_transport.cpp/radio_audio.cpp/sound_i2s.cpp for the matching
// diagnostics one level deeper, in case the loop-level numbers alone
// don't pinpoint which AppService is the actual offender.
namespace {
uint32_t g_lastLoopEntryMs = 0;
bool g_haveLastLoopEntry = false;
}  // namespace

void loop() {
  uint32_t loopStart = millis();
  if (g_haveLastLoopEntry) {
    uint32_t gap = loopStart - g_lastLoopEntryMs;
    if (gap >= 50) {
      Serial.printf("[PERF] LOOP GAP=%lu ms\n", static_cast<unsigned long>(gap));
    }
  }
  g_lastLoopEntryMs = loopStart;
  g_haveLastLoopEntry = true;

  uint32_t t0 = millis();
  tickRegisteredServices();
  uint32_t t1 = millis();
  if (t1 - t0 >= 20) Serial.printf("[PERF] services=%lu ms\n", static_cast<unsigned long>(t1 - t0));

  Power::tick();
  uint32_t t2 = millis();
  if (t2 - t1 >= 20) Serial.printf("[PERF] power=%lu ms\n", static_cast<unsigned long>(t2 - t1));

  Menu::tick();
  uint32_t t3 = millis();
  if (t3 - t2 >= 20) Serial.printf("[PERF] menu=%lu ms\n", static_cast<unsigned long>(t3 - t2));

  Sleep::update();
  uint32_t t4 = millis();
  if (t4 - t3 >= 20) Serial.printf("[PERF] sleep=%lu ms\n", static_cast<unsigned long>(t4 - t3));
}
