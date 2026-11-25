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

// Reasons the deep-sleep timeout is held off, each set and cleared by its
// own owner. Sleep stays inhibited while ANY reason is set, so one owner
// clearing its reason never resumes the timeout for another (an OTA update
// ending while PTT is still held, or a PTT release during an OTA update).
// Clearing a reason that was set also resets the inactivity timer, so the
// device does not sleep the instant a long inhibited stretch ends. The
// user's Sleep Timeout setting is never changed.
enum class InhibitReason : uint8_t {
  OTA_MAINTENANCE = 1 << 0,  // Phase 5 section 19: "Sleep Timeout suspended"
  RADIO_AUDIO = 1 << 1,      // a live Radio/Race voice session (setRadioAudioActive())
};
void setInhibited(InhibitReason reason, bool inhibited);
bool isInhibited();

// Phase 5: OTA Maintenance Mode suspends the deep-sleep timeout entirely
// for the duration of an update -- the OTA_MAINTENANCE reason above.
void setSuspended(bool suspended);

// Call every loop() iteration. Enters deep sleep once the timeout elapses:
// runs BeforeSleep hooks, turns the backlight and battery-sense divider
// off, then calls esp_deep_sleep_start() with no wake source configured
// (power-cycle-only wake; never call this from within an ISR).
void update();

}  // namespace Sleep
