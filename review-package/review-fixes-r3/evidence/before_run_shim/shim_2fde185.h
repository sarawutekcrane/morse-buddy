// BEFORE-RUN SHIM (not production): names the final tests' host stubs use
// that the reviewed candidate 2fde185's headers lack (its code never calls them).
#pragma once
#include <stdint.h>
#include "core/input.h"
enum BackgroundReader : uint8_t { BG_NOTIFICATIONS = 1, BG_OUTBOX = 2 };
bool claimBackgroundStorageStep(BackgroundReader who);
namespace Input {
uint32_t droppedEventCount();
bool queueFull();
bool peekEvent(InputEvent& outEvent);
void unpopEvent(const InputEvent& event);
}
