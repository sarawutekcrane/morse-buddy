#include "core/display.h"

#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <SPI.h>
#include <string.h>

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

// ---------------------------------------------------------------------------
// Status bar dirty-tracking (Hardware Fix #1). Real hardware testing on the
// ideaspark ESP32 1.14" board found every screen redrawing unconditionally
// every loop() iteration -- continuous SPI traffic that both flickered the
// TFT and starved Input::update() of loop time, causing the rotary encoder
// to miss detents. The status bar is drawn by nearly every screen every
// tick, so fixing it here once (rather than at each of ~15 call sites)
// removes most of that traffic in one place. Sentinels guarantee the very
// first call always draws (no real connectivity/battery value can equal
// 0xFF).
// ---------------------------------------------------------------------------
uint8_t g_lastConnStatus = 0xFF;
uint8_t g_lastBatteryPercent = 0xFF;

// ---------------------------------------------------------------------------
// Typography (Hardware Fix #1). PRIMARY metrics are computed once from the
// real font at init() -- never hardcoded -- so layout stays correct
// regardless of the exact bundled-font pixel dimensions.
// ---------------------------------------------------------------------------
Display::Font g_currentFont = Display::Font::COMPACT;
int16_t g_primaryAscent = 14;      // fallback until computePrimaryMetrics() runs
int16_t g_primaryLineHeight = 18;  // fallback until computePrimaryMetrics() runs
constexpr int16_t kCompactLineHeight = 10;  // built-in font: existing project convention

void computePrimaryMetrics() {
  g_tft.setFont(&FreeSans9pt7b);
  int16_t x1, y1;
  uint16_t w, h;
  // Sample a string with both ascenders and a descender ('g') at a known
  // baseline (y=50, comfortably clear of y=0) so y1 (top of ink, relative
  // to that baseline) and h (ink height) reflect the font's real extremes.
  g_tft.getTextBounds("Ag0Xy", 0, 50, &x1, &y1, &w, &h);
  g_primaryAscent = static_cast<int16_t>(50 - y1);
  // ~40% leading above the raw ink height, matching the ratio the existing
  // built-in-font screens already use (8px glyph height at 10-12px line
  // pitch), so consecutive PRIMARY lines never visually touch.
  int16_t withLeading = static_cast<int16_t>(h + (h * 2) / 5);
  g_primaryLineHeight = (withLeading > kCompactLineHeight) ? withLeading : (kCompactLineHeight + 4);
  g_tft.setFont(nullptr);  // restore built-in font as the driver's default
}

}  // namespace

namespace Display {

void init() {
  // Hardware SPI on the ESP32 default VSPI pins (SCK=18, MOSI=23), matching
  // the GPIO map exactly, so no custom SPIClass/pin remap is needed.
  g_tft.init(135, 240);
  // Native panel is 135x240 portrait; rotation 1 produces 240x135 landscape.
  // CONFIRMED on real ideaspark ESP32 1.14" hardware (hardware validation
  // Fix #1): image orientation, edges, and content all render correctly at
  // rotation 1 with no colstart/rowstart correction needed -- the only
  // defect hardware testing found was continuous full-screen redraw
  // (flicker/tear), fixed below and in menu.cpp/display's dirty-tracking,
  // not a geometry/offset problem. Do not add a colstart/rowstart override
  // without a new, separately-reported hardware symptom.
  g_tft.setRotation(1);
  g_tft.fillScreen(ST77XX_BLACK);

  pinMode(Pins::kTftBacklight, OUTPUT);
  ledcSetup(kBacklightLedcChannel, kBacklightFreqHz, kBacklightResolutionBits);
  ledcAttachPin(Pins::kTftBacklight, kBacklightLedcChannel);
  setBacklightPercent(100);

  computePrimaryMetrics();
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
  uint8_t conn = getConnectivityStatus();
  uint8_t batt = Power::getBatteryPercent();
  if (conn == g_lastConnStatus && batt == g_lastBatteryPercent) return;
  g_lastConnStatus = conn;
  g_lastBatteryPercent = batt;

  g_tft.fillRect(0, 0, kScreenWidth, kStatusBarHeight, ST77XX_BLACK);
  g_tft.drawFastHLine(0, kStatusBarHeight - 1, kScreenWidth, ST77XX_WHITE);

  Font savedFont = g_currentFont;
  setFont(Font::COMPACT);
  g_tft.setTextColor(ST77XX_WHITE);

  g_tft.setCursor(2, 3);
  g_tft.print(connectivityLabel(conn));

  char batteryText[6];
  snprintf(batteryText, sizeof(batteryText), "%u%%", batt);
  int16_t x1, y1;
  uint16_t w, h;
  g_tft.getTextBounds(batteryText, 0, 0, &x1, &y1, &w, &h);
  g_tft.setCursor(kScreenWidth - static_cast<int16_t>(w) - 2, 3);
  g_tft.print(batteryText);

  setFont(savedFont);
}

void setFont(Font f) {
  g_currentFont = f;
  g_tft.setFont(f == Font::PRIMARY ? &FreeSans9pt7b : nullptr);
  g_tft.setTextSize(1);
}

Font currentFont() { return g_currentFont; }

int16_t lineHeight() { return (g_currentFont == Font::PRIMARY) ? g_primaryLineHeight : kCompactLineHeight; }

int16_t textWidth(const char* text) {
  int16_t x1, y1;
  uint16_t w, h;
  g_tft.getTextBounds(text, 0, (g_currentFont == Font::PRIMARY) ? 50 : 0, &x1, &y1, &w, &h);
  return static_cast<int16_t>(w);
}

void printLine(int16_t x, int16_t topY, const char* text) {
  char buf[64];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // Clip (never wrap) so drawn text can never extend past kScreenWidth,
  // regardless of font or how long the caller's string is.
  int16_t maxWidth = kScreenWidth - x;
  if (maxWidth > 0) {
    while (buf[0] != '\0' && textWidth(buf) > maxWidth) {
      buf[strlen(buf) - 1] = '\0';
    }
  } else {
    buf[0] = '\0';
  }

  int16_t cursorY = (g_currentFont == Font::PRIMARY) ? static_cast<int16_t>(topY + g_primaryAscent) : topY;
  g_tft.setCursor(x, cursorY);
  g_tft.print(buf);
}

}  // namespace Display
