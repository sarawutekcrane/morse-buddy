#pragma once
// Sleep timeout + deep-sleep entry (Addendum section 19, Phase 1 section 15).

#include <stdint.h>

namespace Sleep {

// 0 means Disabled. Range when non-zero: 1-30 minutes.
void init(uint8_t timeoutMinutes);
void setTimeoutMinutes(uint8_t minutes);
uint8_t getTimeoutMinutes();

// Resets the inactivity timer. Called by Input on DOT/DASH press/hold,
// Encoder SW press/hold, and Encoder rotation (Phase 1 activity sources).
// Later phases add Notification and incoming-Radio-audio-played activity.
void notifyActivity();

// Call every loop() iteration. Enters deep sleep once the timeout elapses:
// runs BeforeSleep hooks, turns the backlight and battery-sense divider
// off, then calls esp_deep_sleep_start() with no wake source configured
// (power-cycle-only wake; never call this from within an ISR).
void update();

}  // namespace Sleep
