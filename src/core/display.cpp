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
// Hardware Fix #4.7c: the actual measured PRIMARY glyph ink height (the `h`
// computePrimaryMetrics() already measures via getTextBounds()), kept
// separate from g_primaryLineHeight -- lineHeight() includes ~40% extra
// leading so consecutive PRIMARY lines never visually touch, but that
// leading is not part of the visible glyph box, so centering the red
// selection-cursor triangle on lineHeight()/2 places it below where the
// ink actually sits. Fallback matches g_primaryLineHeight's own fallback
// ratio until computePrimaryMetrics() runs.
int16_t g_primaryInkHeight = 13;
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
  g_primaryInkHeight = static_cast<int16_t>(h);
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
  // Real hardware testing (ideaspark ESP32 1.14" board, hardware validation
  // Fix #1) reported a readable, correctly-oriented landscape UI at
  // rotation 1 -- not a rotated/mirrored image -- with the actual reported
  // defect being continuous flicker/scrolling/tearing, fixed below and in
  // menu.cpp/display's dirty-tracking. That is gross orientation only: a
  // pixel-level colstart/rowstart check (whether edge rows/columns are
  // clipped or shifted a few pixels) has NOT been explicitly tested and
  // must not be reported as confirmed until a hardware retest specifically
  // checks screen edges. Add a colstart/rowstart override only if such a
  // retest reports a concrete edge/offset symptom.
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
  char buf[kPrintLineBufferSize];
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

void printLineColored(int16_t x, int16_t topY, const char* text, uint16_t color565) {
  g_tft.setTextColor(color565);
  printLine(x, topY, text);
  g_tft.setTextColor(ST77XX_WHITE);
}

bool wrapLineAt(const char* text, size_t from, int16_t maxWidthPx, uint16_t* outStart, uint16_t* outLen) {
  if (text == nullptr || maxWidthPx <= 0) return false;
  size_t len = strlen(text);
  size_t pos = from;
  // A wrap boundary normalizes away the one space that would otherwise be
  // the redundant last character of the previous line (same convention as
  // any word-wrapping renderer); it never drops a non-space character, and
  // never touches an intentional multi-character token (e.g. Enigma RAW
  // mode's "/" word-gap marker is its own character, untouched here).
  while (pos < len && text[pos] == ' ') pos++;
  if (pos >= len) return false;

  // Scratch buffer for width-measuring a candidate line via
  // getTextBounds(), which needs a null-terminated string. Hardware Fix
  // #4.4 issue A: this MUST equal kPrintLineBufferSize, not some smaller
  // guessed size -- widthOf() below refuses to report a verdict for any
  // span it cannot measure in full (rather than silently measuring a
  // truncated prefix and reporting THAT width), so kScratch is also the
  // hard ceiling on how long a returned span can ever be. Keeping it in
  // lock-step with printLine()'s own buffer is what guarantees a caller
  // that copies exactly *outLen bytes into a kPrintLineBufferSize buffer
  // and calls printLine() can never have any of those bytes clipped.
  constexpr size_t kScratch = kPrintLineBufferSize;
  char scratch[kScratch];
  auto widthOf = [&](size_t start, size_t count) -> int16_t {
    if (count > kScratch - 1) return INT16_MAX;  // can't measure the whole span -- never claim it fits
    memcpy(scratch, text + start, count);
    scratch[count] = '\0';
    return textWidth(scratch);
  };

  size_t lineStart = pos;
  size_t lineEnd = lineStart;
  size_t wordScan = lineStart;

  while (wordScan <= len) {
    size_t wordEnd = wordScan;
    while (wordEnd < len && text[wordEnd] != ' ') wordEnd++;
    if (widthOf(lineStart, wordEnd - lineStart) <= maxWidthPx) {
      lineEnd = wordEnd;
      if (wordEnd >= len) break;
      wordScan = wordEnd + 1;
      continue;
    }
    if (lineEnd > lineStart) break;  // an earlier word already fit -- stop the line there
    // Not even the first word fits within maxWidthPx (or it's simply too
    // long to ever measure/draw as one span) -- hard-split it by character
    // so it is never dropped, only spread across more lines. widthOf()'s
    // own kPrintLineMaxChars ceiling bounds `chars` the same way it bounds
    // the word-fit check above, so this can never grow past what
    // printLine() can draw in full either.
    size_t chars = 1;
    while (lineStart + chars < wordEnd && widthOf(lineStart, chars + 1) <= maxWidthPx) chars++;
    lineEnd = lineStart + chars;
    break;
  }
  if (lineEnd <= lineStart) lineEnd = lineStart + 1;  // guarantee forward progress
  if (lineEnd - lineStart > kPrintLineMaxChars) {
    // Belt-and-suspenders: no path above should reach this (every accepted
    // span was itself verified to measure at <= kPrintLineMaxChars bytes),
    // but never return a span longer than what printLine() can draw in
    // full, whatever the reason.
    lineEnd = lineStart + kPrintLineMaxChars;
  }

  *outStart = static_cast<uint16_t>(lineStart);
  *outLen = static_cast<uint16_t>(lineEnd - lineStart);
  return true;
}

uint8_t wrapText(const char* text, int16_t maxWidthPx, uint16_t* outStarts, uint16_t* outLens, uint8_t maxLines) {
  if (text == nullptr || maxLines == 0) return 0;
  uint8_t lineCount = 0;
  size_t pos = 0;
  while (lineCount < maxLines) {
    uint16_t s, l;
    if (!wrapLineAt(text, pos, maxWidthPx, &s, &l)) break;
    outStarts[lineCount] = s;
    outLens[lineCount] = l;
    lineCount++;
    pos = static_cast<size_t>(s) + l;
  }
  return lineCount;
}

void drawLockIcon(int16_t x, int16_t y, bool open, uint16_t color565) {
  // Drawn with TFT primitives (never a font glyph) inside a fixed
  // kLockIconCellWidth x kLockIconCellHeight cell, so text after it always
  // starts at the same X regardless of open/closed state (Hardware Fix #4
  // issue 5). Body is a small filled rounded rect; the shackle is an
  // outlined rounded rect whose bottom is hidden behind the body, leaving
  // just its top arch visible above the body -- closed sits centered,
  // open is shifted left and clear of the body to read as "unlatched".
  constexpr int16_t kBodyW = 8, kBodyH = 6;
  constexpr int16_t kShackleW = 6, kShackleH = 7;
  int16_t bodyX = static_cast<int16_t>(x + 2);
  int16_t bodyY = static_cast<int16_t>(y + 5);
  int16_t shackleX = static_cast<int16_t>(open ? x : x + 3);
  int16_t shackleY = y;
  g_tft.drawRoundRect(shackleX, shackleY, kShackleW, kShackleH, 2, color565);
  g_tft.fillRoundRect(bodyX, bodyY, kBodyW, kBodyH, 1, color565);
}

void drawSelectionCursor(int16_t x, int16_t rowTop) {
  // Hardware Fix #4.7c: on real hardware the RED cursor visibly sat lower
  // than the PRIMARY text it marks. lineHeight() includes ~40% leading
  // above the glyph ink (see computePrimaryMetrics()) so that consecutive
  // PRIMARY lines don't visually touch, but printLine() draws that ink
  // starting exactly at rowTop -- centering on the leading-inclusive
  // lineHeight()/2 therefore biases the triangle toward the leading below
  // the ink rather than the ink itself. Centering on the actually-measured
  // ink height (g_primaryInkHeight) instead aligns the triangle with the
  // visible glyphs. COMPACT keeps its original line-centered behavior
  // unchanged -- it never had this leading gap.
  //
  // Hardware Fix #4.7g: even after the #4.7c ink-height centering, real
  // hardware still perceived the triangle as sitting slightly below the
  // optical vertical center of PRIMARY text -- glyph ink is not
  // perfectly symmetric top/bottom around its bounding box, so a small
  // fixed optical correction (kPrimaryCursorYOffset) nudges it up a
  // couple of pixels. PRIMARY-only; COMPACT is untouched. This does not
  // touch printLine(), g_primaryAscent, g_primaryInkHeight, or
  // g_primaryLineHeight -- purely a cursor-drawing adjustment.
  constexpr int16_t kPrimaryCursorYOffset = -2;
  int16_t centerY = (g_currentFont == Display::Font::PRIMARY)
                         ? static_cast<int16_t>(rowTop + g_primaryInkHeight / 2 + kPrimaryCursorYOffset)
                         : static_cast<int16_t>(rowTop + lineHeight() / 2);
  constexpr int16_t kHalfHeight = kCursorTriangleHeight / 2;  // 3: apex-to-base half-span
  g_tft.fillTriangle(x, static_cast<int16_t>(centerY - kHalfHeight), x, static_cast<int16_t>(centerY + kHalfHeight),
                     static_cast<int16_t>(x + kCursorTriangleWidth), centerY, ST77XX_RED);
}

void drawSenderDivider(int16_t x, int16_t rowTop, const char* senderText, uint16_t color565) {
  // Hardware Fix #4.9d Part B1: a real-hardware retest found the bar
  // visibly taller/lower than the sender letters beside it -- the old
  // implementation sized it from g_primaryInkHeight, a metric measured
  // once at init() from a generic sample string ("Ag0Xy", chosen to
  // include a descender purely for metrics purposes), which does not
  // reliably match any GIVEN sender name's own ink box. Measuring
  // senderText itself, at the exact baseline printLine() uses for PRIMARY
  // text (rowTop + g_primaryAscent -- see printLine()'s own cursorY
  // computation above), makes the bar visually match THIS sender name's
  // real glyph bounds instead of a generic sample's. Every caller draws
  // this only on a PRIMARY-font sender row (the Unified Thread history/
  // compose screens never use COMPACT here), so no COMPACT variant is
  // needed. A null/empty senderText (defensive only -- every real caller
  // passes the same non-empty string it already drew as the sender name)
  // falls back to the previous g_primaryInkHeight-based sizing rather than
  // measuring nothing.
  int16_t barTop = rowTop;
  int16_t barHeight = g_primaryInkHeight;
  if (senderText != nullptr && senderText[0] != '\0') {
    int16_t x1, y1;
    uint16_t w, h;
    g_tft.getTextBounds(senderText, x, static_cast<int16_t>(rowTop + g_primaryAscent), &x1, &y1, &w, &h);
    if (h > 0) {
      barTop = y1;
      barHeight = static_cast<int16_t>(h);
    }
  }
  g_tft.fillRect(x, barTop, kSenderDividerWidth, barHeight, color565);
}

}  // namespace Display
