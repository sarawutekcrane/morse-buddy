#include "core/display.h"

#include <Arduino.h>
#include <SPI.h>

#include "core/hooks.h"
#include "core/pins.h"
#include "core/power.h"

namespace {

constexpr uint8_t kBacklightLedcChannel = 0;
constexpr uint32_t kBacklightFreqHz = 5000;
constexpr uint8_t kBacklightResolutionBits = 8;

Adafruit_ST7789 g_tft(Pins::kTftCs, Pins::kTftDc, Pins::kTftRst);
uint8_t g_backlightPercent = 100;

const char* connectivityLabel(uint8_t status) {
  switch (status) {
    case CONN_ONLINE:
      return "ON";
    case CONN_WIFI_ONLY:
      return "WIFI";
    default:
      return "OFF";
  }
}

}  // namespace

namespace Display {

void init() {
  // Hardware SPI on the ESP32 default VSPI pins (SCK=18, MOSI=23), matching
  // the GPIO map exactly, so no custom SPIClass/pin remap is needed.
  g_tft.init(135, 240);
  // Native panel is 135x240 portrait; rotation 1 produces 240x135 landscape.
  // NOTE: some ST7789 135x240 modules need a colstart/rowstart offset
  // correction. Verify on real hardware before changing this; do not
  // speculatively adjust pins/driver assumptions without that verification
  // (Addendum section 21).
  g_tft.setRotation(1);
  g_tft.fillScreen(ST77XX_BLACK);

  pinMode(Pins::kTftBacklight, OUTPUT);
  ledcSetup(kBacklightLedcChannel, kBacklightFreqHz, kBacklightResolutionBits);
  ledcAttachPin(Pins::kTftBacklight, kBacklightLedcChannel);
  setBacklightPercent(100);
}

Adafruit_ST7789& tft() { return g_tft; }

void setBacklightPercent(uint8_t percent) {
  if (percent > 100) percent = 100;
  g_backlightPercent = percent;
  uint32_t duty = (static_cast<uint32_t>(percent) * 255u) / 100u;
  ledcWrite(kBacklightLedcChannel, duty);
}

uint8_t getBacklightPercent() { return g_backlightPercent; }

void clearContentArea() {
  g_tft.fillRect(0, kStatusBarHeight, kScreenWidth, kScreenHeight - kStatusBarHeight, ST77XX_BLACK);
}

void drawStatusBar() {
  g_tft.fillRect(0, 0, kScreenWidth, kStatusBarHeight, ST77XX_BLACK);
  g_tft.drawFastHLine(0, kStatusBarHeight - 1, kScreenWidth, ST77XX_WHITE);

  g_tft.setTextSize(1);
  g_tft.setTextColor(ST77XX_WHITE);

  g_tft.setCursor(2, 3);
  g_tft.print(connectivityLabel(getConnectivityStatus()));

  char batteryText[6];
  snprintf(batteryText, sizeof(batteryText), "%u%%", Power::getBatteryPercent());
  int16_t x1, y1;
  uint16_t w, h;
  g_tft.getTextBounds(batteryText, 0, 0, &x1, &y1, &w, &h);
  g_tft.setCursor(kScreenWidth - static_cast<int16_t>(w) - 2, 3);
  g_tft.print(batteryText);
}

}  // namespace Display
