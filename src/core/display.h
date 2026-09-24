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
// Internally redraws only when the connectivity status or battery percent
// actually changed since the last draw (Hardware Fix #1) -- safe to call
// unconditionally every tick from every screen, exactly as before; the
// status bar's own pixels are never touched by clearContentArea(), so a
// skipped redraw can never leave a stale value on screen.
void drawStatusBar();

// =============================================================================
// Typography (Hardware Fix #1). Two fonts:
//   COMPACT - the original built-in 5x7 font at setTextSize(1). Used for
//             the status bar and any screen that needs dense/small text.
//   PRIMARY - a real GFX font (~1.5x the visual size of COMPACT; Adafruit
//             GFX's setTextSize() only scales the built-in font by whole
//             integers, so setTextSize(2) would be ~2x -- too large for
//             240x135). Used for menu/title/body text.
// Bundled with the already-declared Adafruit GFX Library dependency (no
// new asset). A custom GFX font's cursor Y is its baseline, not its
// top-left corner, unlike the built-in font -- printLine() is the one
// place that accounts for this so call sites never have to.
// =============================================================================
enum class Font : uint8_t { COMPACT, PRIMARY };

// Selects the active font for tft()/printLine() calls until changed again.
// Screens that call other screens' shared widgets (ListMenu, MixedTextEntry,
// the numeric-adjust widget) don't need to restore COMPACT themselves --
// each of those widgets sets the font it needs on every tick.
void setFont(Font f);
Font currentFont();

// Top-to-top pixel distance between consecutive lines for the currently
// active font. Computed once at init() from the real font metrics (never
// hardcoded), so callers never need to know exact glyph dimensions.
int16_t lineHeight();

// Prints one line of text with its TOP-LEFT corner at (x, topY), using
// whichever font is currently active -- handles the COMPACT-vs-PRIMARY
// baseline difference internally. Clips (truncates, never wraps) so the
// drawn text never extends past kScreenWidth, regardless of font or
// string length.
void printLine(int16_t x, int16_t topY, const char* text);

// Pixel width `text` would occupy in the currently active font -- for
// screens that need to right-align or fit-check before drawing.
int16_t textWidth(const char* text);

// Fixed-size (kLockIconCellWidth x kLockIconCellHeight) status icon: a tiny
// padlock drawn with TFT primitives, not a font glyph (the installed GFX
// font cannot be assumed to have a Unicode lock glyph -- Hardware Fix #4
// issue 5). `open`: true draws an open-shackle lock, false a closed one --
// shape and color both carry the status, never color alone. Callers erase
// the icon's own cell (kLockIconCellWidth wide) before redrawing, same as
// any other partial-redraw region; this never touches pixels outside it.
constexpr int16_t kLockIconCellWidth = 12;
constexpr int16_t kLockIconCellHeight = 12;
void drawLockIcon(int16_t x, int16_t y, bool open, uint16_t color565);

}  // namespace Display
