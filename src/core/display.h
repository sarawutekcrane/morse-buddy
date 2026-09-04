#pragma once
// ST7789 TFT driver wrapper (Addendum section 21). Physical panel is
// 135x240; UI runs landscape 240x135. Adafruit GFX + Adafruit ST7789 only
// (not SSD1306).

#include <Adafruit_ST7789.h>
#include <stdint.h>

namespace Display {

// Landscape logical dimensions after rotation.
constexpr int16_t kScreenWidth = 240;
constexpr int16_t kScreenHeight = 135;

constexpr int16_t kStatusBarHeight = 14;

void init();

// Underlying driver, for screens that need to draw directly.
Adafruit_ST7789& tft();

// 0-100%, GPIO32, 5kHz, 8-bit PWM. 0% turns the backlight fully off.
void setBacklightPercent(uint8_t percent);
uint8_t getBacklightPercent();

// Clears the region below the status bar (the screen content area).
void clearContentArea();

// Draws the standard top status bar: WiFi/connectivity state + battery %.
// Uses the registered connectivity status provider and Power module.
void drawStatusBar();

}  // namespace Display
