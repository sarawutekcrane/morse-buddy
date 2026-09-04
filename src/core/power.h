#pragma once
// Battery sensing on the isolated high-side switched divider (Addendum
// sections 2.1 and 20). GPIO26 controls the N-MOS/P-MOS divider switch;
// GPIO34 is the ADC node (through a 10nF settling capacitor). The ESP32
// GPIO never connects to BAT+ directly.

#include <stdint.h>

namespace Power {

// Configures GPIO26/GPIO34, characterizes the ADC (esp_adc_cal, 11dB
// attenuation), and takes one immediate provisional reading.
void init();

// Call every loop() iteration. Runs the non-blocking periodic sampling
// state machine: 10 samples every 6 seconds, divider briefly enabled per
// sample, averaged into a new battery percent once per 10-sample cycle.
void tick();

// Forces the divider off. Used by Sleep before entering deep sleep.
void setDividerEnabled(bool enabled);

uint8_t getBatteryPercent();
float getBatteryVoltage();

bool isLowBattery();       // <=20%
bool isCriticalBattery();  // <=10%

}  // namespace Power
