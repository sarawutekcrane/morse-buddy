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

  // Later phases register their own background services (WiFi/MQTT/NTP in
  // Phase 2, tone/audio in Phase 3, Radio/Race in Phase 4) from their own
  // translation units; this call site never needs to change.
  initRegisteredServices();
}

void loop() {
  tickRegisteredServices();
  Power::tick();
  Menu::tick();
  Sleep::update();
}
