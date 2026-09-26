#include "core/identity_color.h"

namespace IdentityColor {

namespace {

// One central RGB888->RGB565 conversion, so every palette entry below is
// authored in ordinary 8-bit-per-channel RGB (easy to eyeball/tune) and
// converted once, here, rather than callers hand-computing RGB565 bit
// patterns themselves.
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

struct PaletteEntry {
  uint16_t color;
  const char* name;
};

// 12 bright, TFT-readable, non-semantic hues (Feature Fix #4.8 section 2A).
// None of these collides with BLACK/WHITE/pure RED (255,0,0)/pure YELLOW
// (255,255,0)/pure GREEN (0,255,0) -- all of which already carry fixed UI
// meaning elsewhere (message body, selection cursor, Enigma lock states).
const PaletteEntry kPalette[kColorCount] = {
    {rgb565(0, 255, 255), "Cyan"},
    {rgb565(64, 196, 255), "Sky Blue"},
    {rgb565(41, 121, 255), "Azure"},
    {rgb565(170, 0, 255), "Violet"},
    {rgb565(255, 0, 255), "Magenta"},
    {rgb565(255, 105, 180), "Hot Pink"},
    {rgb565(255, 145, 0), "Orange"},
    {rgb565(255, 196, 0), "Amber"},
    {rgb565(174, 234, 0), "Lime"},
    {rgb565(0, 229, 160), "Mint"},
    {rgb565(29, 233, 182), "Turquoise"},
    {rgb565(179, 136, 255), "Lavender"},
};

}  // namespace

bool isValid(uint8_t index) { return index < kColorCount; }

uint16_t color565(uint8_t index) { return isValid(index) ? kPalette[index].color : 0xFFFF; }

const char* colorName(uint8_t index) { return isValid(index) ? kPalette[index].name : "?"; }

}  // namespace IdentityColor
