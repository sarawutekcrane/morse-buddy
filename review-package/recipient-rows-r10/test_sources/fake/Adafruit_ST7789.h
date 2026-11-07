#pragma once
// HOST STUB (not production).
#include <stdint.h>
#define ST77XX_BLACK 0x0000
#define ST77XX_WHITE 0xFFFF
#define ST77XX_RED 0xF800
#define ST77XX_YELLOW 0xFFE0
#define ST77XX_GREEN 0x07E0
#define ST77XX_CYAN 0x07FF
class Adafruit_ST7789 {
 public:
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t c);
  // storage_init.cpp's boot prompt (syntax checks only; never called by host tests)
  void fillScreen(uint16_t) {}
  void setTextSize(uint8_t) {}
  void setTextColor(uint16_t) {}
  void setCursor(int16_t, int16_t) {}
  void print(const char*) {}
};
