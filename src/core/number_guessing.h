#pragma once
// Number Guessing shared mechanics (Addendum section 12; Phase 3 sections
// 8-9) + Play Solo. Play with Friend lives in number_guessing_friend.*
// since it needs the network/storage/empty-compose-hook machinery Solo
// doesn't.

#include <stdint.h>

#include "core/input.h"

namespace NumberGuessing {

constexpr uint8_t kSecretDigits = 4;
constexpr uint16_t kMaxAttempts = 255;

struct GuessResult {
  uint8_t a;
  uint8_t b;
};
GuessResult evaluate(const uint8_t secret[kSecretDigits], const uint8_t guess[kSecretDigits]);

// Shared digit-entry component (Addendum 9.5: DOT <500ms confirms the
// rotated-to digit, DOT held >=500ms deletes the previous digit, firing
// once at 500ms while still held; release after a delete does not also
// confirm). Duplicate digits are skipped while rotating.
struct DigitEntryState {
  uint8_t digits[kSecretDigits];
  uint8_t count;
  int8_t previewDigit;
  bool dotHeld;
  uint32_t dotPressStartMs;
  bool deleteFiredThisPress;
};

void resetDigitEntry(DigitEntryState* state);
void handleDigitEntryEvent(DigitEntryState* state, const InputEvent& e);
void tickDigitEntry(DigitEntryState* state);  // call every frame to catch the 500ms hold-delete edge

// Hardware Fix #4 issue 7: shared partial-redraw renderer for a digit-entry
// row ("<label><4 fixed digit cells>", e.g. "Guess: 12_ _"), used by every
// caller with a DigitEntryState (Play Solo, Play with Friend's answer/
// manual-secret-entry, Race's guess screen). Earlier revisions accumulated
// each cell's X from the previous cells' actual rendered width, which is
// unsafe on PRIMARY's proportional font -- a digit whose glyph happens to
// be narrower/wider than another could visibly shift every cell after it.
// This renderer instead reserves kSecretDigits FIXED-X cells sized from the
// single widest glyph any cell can ever show (0-9 or the '_' placeholder)
// plus padding, so no cell's position ever depends on another cell's
// content. Each cell (confirmed digit / active preview digit / not-yet-
// reached placeholder) is diffed and redrawn completely independently.
struct DigitRowRenderState {
  char lastCell[kSecretDigits] = {0, 0, 0, 0};
  bool neverDrawn = true;
};

// Call once when the owning screen is (re)entered, before the first
// renderDigitRow() call, so the next call does a full initial draw.
void resetDigitRowRenderState(DigitRowRenderState* rs);

// Draws/updates the row at (x, y) using the widget's current font
// (caller must have already called Display::setFont()).
void renderDigitRow(DigitRowRenderState* rs, int16_t x, int16_t y, const char* labelPrefix,
                    const DigitEntryState& state);

// Fixed cell pitch renderDigitRow() uses (widest 0-9/'_' glyph + padding),
// in the widget's current font.
int16_t digitCellWidth();

// Phase 2A: one recorded-attempt row ("1 2 3 4   2A2B") whose four digits
// share EXACT pixel X with renderDigitRow()'s cells for the same
// (x, labelPrefix), result text trailing after the fourth cell. Shared by
// Play Solo and Race's guess screen so both present A/B identically.
// Draws only; the caller clears the row region beforehand if needed.
void drawGuessHistoryRow(int16_t x, int16_t y, const char* labelPrefix, uint16_t guessValue, uint8_t a,
                         uint8_t b);

// ---- Same-screen guess-history browsing (Phase 2 UI) ------------------------
// Shared by Play Solo, Friend's answer screen and Race's guess screen. The
// digit-entry draft (DigitEntryState) is never modified while browsing, so
// leaving browsing restores the same digit position and preview value.
//
// Encoder, decreasing (-1) direction: at the lowest digit still selectable in
// the draft (duplicates skipped), or with a complete 4-digit draft, one more
// detent leaves the digit cell for the front of the Guess row; further
// detents select the newest retained history row, then older ones (clamped at
// the oldest retained row). Increasing (+1) goes back down through newer rows
// to the Guess-row front, then to the digit cell. Increasing during digit
// editing is unchanged (wraps as before).
struct HistoryBrowseState {
  bool browsing;          // false: digit editing (normal)
  int16_t selected;       // while browsing: -1 = Guess-row front, k >= 0 = k-th newest retained row
  uint16_t scroll;        // rows hidden below the visible window (0 = newest row is just above Guess)
  bool dotOwnedByBrowse;  // a DOT press began while browsing; its release is swallowed wherever it lands
};

enum class BrowseResult : uint8_t {
  PASS,      // not a browsing event: handle it normally
  CONSUMED,  // handled by browsing (cursor moved, or swallowed); do not pass on
  RETURNED,  // browsing ended; digit editing resumes with the unchanged draft
};

void resetHistoryBrowse(HistoryBrowseState* b);

// Call for every popped event BEFORE the screen's own handling (after its
// Back and replay/stale-input filters). ENCODER_LONG (Back) always passes.
// While browsing, ENCODER_SHORT returns to digit editing (never submits or
// opens Exit) and every DOT/combined event is swallowed, so the draft can't
// change unseen. Leaving the digit cell is refused while a DOT is held, so a
// pending confirm or hold-delete always applies to the visible cell.
BrowseResult handleHistoryBrowseEvent(HistoryBrowseState* b, const DigitEntryState& entry, const InputEvent& e,
                                      uint16_t retainedCount);

// Keeps `selected` within the retained rows and the window over it; call
// before drawing with the number of history rows that currently fit.
void clampHistoryBrowse(HistoryBrowseState* b, uint16_t retainedCount, uint16_t visibleRows);

// Guess-row label, drawn highlighted while the cursor is at the Guess-row
// front. Clears exactly the label's own width first.
void drawGuessLabel(int16_t x, int16_t y, const char* labelPrefix, bool highlighted);

// The selection marker for a history row, drawn in the empty label area to
// the left of the row's digits (never over digits or the A/B result).
void drawHistorySelectionMarker(int16_t x, int16_t y);

}  // namespace NumberGuessing
