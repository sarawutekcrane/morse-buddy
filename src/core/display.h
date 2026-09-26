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

// Single explicit source of truth (Hardware Fix #4.4 issue A) for the
// longest byte span printLine() can ever draw in full: its internal
// working buffer is exactly this size, so any caller -- including
// wrapLineAt()/wrapText() below -- that keeps its own spans/copies at or
// under kPrintLineMaxChars bytes is guaranteed nothing it hands to
// printLine() can be silently clipped.
constexpr size_t kPrintLineBufferSize = 64;
constexpr size_t kPrintLineMaxChars = kPrintLineBufferSize - 1;

// Prints one line of text with its TOP-LEFT corner at (x, topY), using
// whichever font is currently active -- handles the COMPACT-vs-PRIMARY
// baseline difference internally. Clips (truncates, never wraps) so the
// drawn text never extends past kScreenWidth, regardless of font or
// string length; also clips to kPrintLineMaxChars bytes regardless of
// pixel width, so pass at most that many bytes if the full text must
// actually be drawn (see kPrintLineMaxChars above).
void printLine(int16_t x, int16_t topY, const char* text);

// Same as printLine(), but prints in `color565` and then immediately
// restores ST77XX_WHITE, so callers never need to track/restore text
// color themselves (Hardware Fix #4.7: e.g. a CYAN sender-name prefix
// immediately followed by WHITE message body drawn via plain printLine()).
void printLineColored(int16_t x, int16_t topY, const char* text, uint16_t color565);

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

// Compact selection/focus cursor for space-constrained conversation UI
// (Hardware Fix #4.7): a small solid RED right-pointing triangle primitive
// instead of the PRIMARY-font ">" glyph, so the Text/Enigma/Friend Unified
// Thread history and compose rows can recover most of the width the old
// glyph+padding cell wasted. This is NOT a general menu-cursor replacement
// -- ordinary ListMenu screens keep their existing "> " marker.
// Hardware Fix #4.7f: widened from 6 to 9 -- on the real 1.14" TFT, a
// 4px-wide triangle drawn at x=2 inside a 6px cell left only ~2px before
// FreeSans9pt7b text starting at 2+kCursorCellWidth, tight enough to read
// as touching/overlapping the first glyph. The triangle itself is
// unchanged (same width/height, same draw X) -- only this budget grows,
// so every caller that derives its text/erase X from
// 2 + Display::kCursorCellWidth automatically gets the extra gap without
// any per-file change.
// Hardware Fix #4.7g: widened again from 9 to 11 -- real hardware still
// perceived the ~5px gap left by Fix #4.7f as too tight; this raises the
// perceived black gap to ~7px, still with zero change to the triangle
// itself or its draw X.
constexpr int16_t kCursorCellWidth = 11;     // fixed horizontal budget callers reserve for this cursor's cell
constexpr int16_t kCursorTriangleWidth = 4;  // point-to-base width
constexpr int16_t kCursorTriangleHeight = 7; // base height

// Draws the triangle with its leftmost (base) edge at x, vertically
// centered within the PRIMARY text row that starts at rowTop (using the
// currently active font's lineHeight()). Pure TFT-primitive drawing --
// never touches text color/font state, so it's safe to call between any
// printLine()/printLineColored() calls with nothing to restore afterward.
void drawSelectionCursor(int16_t x, int16_t rowTop);

// Hardware Fix #4.8b: compact sender/body boundary for the Unified Thread
// history rows (Text/Enigma/Friend), replacing the old ": " suffix on the
// sender name. On a 240x135 TFT, a proportional PRIMARY font plus Morse
// dots/dashes made "WUT:.- . .." read with no visible start boundary; a
// literal colon-plus-space, multiple spaces, "|", or a Unicode glyph were
// all explicitly rejected as either too wide or unreliable across fonts.
// A small solid TFT-primitive bar, drawn in the sender's own color right
// after their name, reads as a clear divider at minimal width cost.
//
// Hardware Fix #4.9d Part B2: a real-hardware retest found the bar sitting
// too close to the sender name -- a small lead gap is now inserted BEFORE
// the bar too, so the row reads as "name  |  body" with breathing room on
// both sides of the divider, not just after it.
constexpr int16_t kSenderDividerLeadGap = 2;  // gap between the sender name and the bar
constexpr int16_t kSenderDividerWidth = 3;    // solid bar width
constexpr int16_t kSenderBodyGap = 2;         // fixed gap between the bar and the WHITE message body that follows

// Hardware Fix #4.9d Part B1: draws the divider bar with its left edge at
// x, sized to the ACTUAL ink of `senderText` -- the same string already
// drawn as the sender name -- rather than the generic PRIMARY font sample
// ("Ag0Xy", chosen to include a descender for METRICS purposes) the old
// signature used via g_primaryInkHeight. A real hardware retest found that
// generic sample's height did not reliably match a given sender name's own
// glyph box, making the bar look taller/lower than the letters beside it.
// Internally (PRIMARY font only): getTextBounds(senderText, x, rowTop +
// g_primaryAscent, ...) is measured at the exact baseline printLine() uses
// for this row, and the bar is drawn to the returned actual top/height --
// so it visually matches THIS sender name's ink box, not a generic sample.
// senderText == nullptr or "" falls back safely (uses the previous
// g_primaryInkHeight-based sizing) rather than measuring nothing.
// Pure TFT-primitive drawing, like drawSelectionCursor() -- never touches
// text color/font state. Callers only ever draw this on a message's TRUE
// first row, immediately after the sender name (plus kSenderDividerLeadGap),
// in the same color565 used for that name; continuation rows never call
// this (Hardware Fix #4.7b Part C: they start at senderX with no repeated
// sender/divider).
void drawSenderDivider(int16_t x, int16_t rowTop, const char* senderText, uint16_t color565);

// Greedy word-wrap of `text` into visual lines that each fit within
// `maxWidthPx` in the currently active font (Hardware Fix #4.3 issue E).
// Writes up to `maxLines` line spans as byte offsets into `text` --
// outStarts[i]/outLens[i], not copies; callers slice the substring
// themselves (e.g. via memcpy) using those offsets. Wraps on space
// boundaries where possible; a single word wider than maxWidthPx is
// hard-split by character so text is never silently dropped, only ever
// bounded by how many lines the caller has room to draw. Returns the
// number of lines actually produced (<= maxLines); if `text` needs more
// lines than maxLines, the text beyond the last produced line is simply
// not represented in the output -- callers that need "show the most
// recent lines" call this with a generous maxLines to get the full
// breakdown, then window the result themselves. Every returned span is at
// most kPrintLineMaxChars bytes (see wrapLineAt() below) -- a run of text
// too long or too narrow-glyphed to naturally break within that many
// bytes is wrapped early rather than ever exceeding it.
uint8_t wrapText(const char* text, int16_t maxWidthPx, uint16_t* outStarts, uint16_t* outLens, uint8_t maxLines);

// Streaming counterpart to wrapText() (Hardware Fix #4.3a issue 1): computes
// just the ONE wrapped line starting at byte offset `from` in `text`, using
// the identical greedy word-wrap / hard-split rules, and writes its span to
// *outStart/*outLen. Returns false (nothing written) once `from` is at or
// past the end of `text`. Callers stream through arbitrarily long text --
// advancing `from` by *outLen each call -- without needing an output array
// sized to a worst-case row count, so a message's true length (bounded only
// by its own existing storage capacity) can never be silently capped by an
// unrelated, guessed array size.
//
// *outLen is ALWAYS <= kPrintLineMaxChars (Hardware Fix #4.4 issue A): a
// candidate span is only ever accepted as "fits" after its FULL width (up
// to kPrintLineMaxChars bytes) was actually measured, never a truncated
// prefix of a longer span -- so a caller that copies exactly *outLen bytes
// starting at *outStart into a kPrintLineBufferSize-or-larger buffer and
// hands it to printLine() is guaranteed the whole span is drawn, and that
// advancing `from` by *outLen for the next call can never skip over bytes
// that were never actually drawn. A span that would otherwise need to
// exceed kPrintLineMaxChars (an unbroken run of text, or many narrow
// glyphs that would still fit maxWidthPx past that many bytes) is wrapped
// early at that byte boundary instead -- preferring an earlier break to
// ever losing or silently truncating text.
bool wrapLineAt(const char* text, size_t from, int16_t maxWidthPx, uint16_t* outStart, uint16_t* outLen);

}  // namespace Display
