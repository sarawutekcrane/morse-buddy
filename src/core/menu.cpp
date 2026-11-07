#include "core/menu.h"

#include <Arduino.h>

#include "core/display.h"
#include "core/input.h"
#include "core/modes.h"
#include "core/settings.h"

namespace {

constexpr uint8_t kMaxStackDepth = 12;
ScreenHandlerFn g_stack[kMaxStackDepth];
uint8_t g_stackSize = 0;
bool g_justEntered = false;
uint32_t g_navigationEpoch = 0;  // Phase 2C-2: see Menu::navigationEpoch()

// These are one-shot dispatchers, not real screens: they must remove
// themselves from the navigation stack (goBack) before pushing their
// target, otherwise backing out of the target would land back on the
// trampoline, which would just unconditionally re-push the same target —
// the user could never actually leave. Every "delegate to another screen"
// trampoline in this codebase follows this same goBack()-then-pushScreen()
// pattern.
void trampolineText() {
  ScreenHandlerFn fn = getModeHandler(Modes::TEXT);
  Menu::goBack();
  Menu::pushScreen(fn != nullptr ? fn : comingSoonScreen);
}
void trampolineEnigma() {
  ScreenHandlerFn fn = getModeHandler(Modes::ENIGMA);
  Menu::goBack();
  Menu::pushScreen(fn != nullptr ? fn : comingSoonScreen);
}
void trampolineRadio() {
  ScreenHandlerFn fn = getModeHandler(Modes::RADIO);
  Menu::goBack();
  Menu::pushScreen(fn != nullptr ? fn : comingSoonScreen);
}

const SettingItem kMainMenuItems[] = {
    {"Text Message", trampolineText},
    {"Enigma Cipher", trampolineEnigma},
    {"Radio (Walkie-Talkie)", trampolineRadio},
    {"Training Game", Settings::screenTrainingGame},
    {"Settings", Settings::screenRoot},
};
constexpr uint8_t kMainMenuItemCount = sizeof(kMainMenuItems) / sizeof(kMainMenuItems[0]);

BadgeFn g_mainMenuBadges[kMainMenuItemCount] = {nullptr, nullptr, nullptr, nullptr, nullptr};

ListMenu g_mainMenuList;

void mainMenuScreen() {
  if (Menu::consumeJustEntered()) {
    g_mainMenuList.configure(kMainMenuItems, kMainMenuItemCount, g_mainMenuBadges);
  }
  Display::drawStatusBar();
  g_mainMenuList.tick("Morse Buddy");
}

}  // namespace

namespace Menu {
void registerMainMenuBadge(uint8_t itemIndex, BadgeFn fn) {
  if (itemIndex < kMainMenuItemCount) g_mainMenuBadges[itemIndex] = fn;
}
}  // namespace Menu

namespace Menu {

void init() {
  g_stackSize = 0;
  g_stack[g_stackSize++] = mainMenuScreen;
  g_justEntered = true;
  g_navigationEpoch++;
}

void tick() {
  if (g_stackSize == 0) return;
  ScreenHandlerFn top = g_stack[g_stackSize - 1];
  if (top != nullptr) top();
}

void pushScreen(ScreenHandlerFn fn) {
  if (fn == nullptr) return;
  if (g_stackSize < kMaxStackDepth) {
    g_stack[g_stackSize++] = fn;
    g_justEntered = true;
    g_navigationEpoch++;
  }
}

void goBack() {
  if (g_stackSize > 1) {
    g_stackSize--;
    g_justEntered = true;
    g_navigationEpoch++;
  }
}

bool consumeJustEntered() {
  bool v = g_justEntered;
  g_justEntered = false;
  return v;
}

uint32_t navigationEpoch() { return g_navigationEpoch; }

}  // namespace Menu

void ListMenu::configure(const SettingItem* items, uint8_t count, const BadgeFn* badges,
                          uint8_t initialSelected, SelectionMode selectionMode, BadgeLayout badgeLayout) {
  items_ = items;
  badges_ = badges;
  count_ = count;
  selected_ = (count > 0 && initialSelected < count) ? initialSelected : 0;
  selectionMode_ = selectionMode;
  badgeLayout_ = badgeLayout;
  needsFullRedraw_ = true;
  for (auto& b : lastBadge_) b = false;
}

void ListMenu::tick(const char* title) { tickImpl(title, nullptr, false); }

bool ListMenu::tickOwned(const char* title, InputFilterFn dropFilter) { return tickImpl(title, dropFilter, true); }

bool ListMenu::tickImpl(const char* title, InputFilterFn dropFilter, bool stopOnNavigation) {
  const uint32_t epochAtStart = Menu::navigationEpoch();
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (dropFilter != nullptr && dropFilter(e)) continue;  // Phase 2C-2 (tickOwned only)
    if (e.type == InputEventType::ENCODER_ROTATE && count_ > 0) {
      int16_t next = static_cast<int16_t>(selected_) + e.value;
      if (next < 0) next = static_cast<int16_t>(count_) - 1;
      if (next >= static_cast<int16_t>(count_)) next = 0;
      selected_ = static_cast<uint8_t>(next);
    } else if (Input::isMenuConfirm(e) && count_ > 0) {
      ScreenHandlerFn fn = items_[selected_].onSelect;
      if (fn != nullptr) {
        if (selectionMode_ == SelectionMode::IN_PLACE) {
          // Hardware Fix #4.1: called directly, not pushed -- stays on
          // this same ListMenu/screen with no navigation-stack change.
          // The callback must not call Menu::goBack() or push a screen.
          fn();
        } else {
          Menu::pushScreen(fn);
        }
      }
    } else if (Input::isBack(e)) {
      Menu::goBack();
    }
    // Phase 2C-2 (tickOwned only): this list is no longer the top screen.
    if (stopOnNavigation && Menu::navigationEpoch() != epochAtStart) return true;
  }

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t titleY = Display::kStatusBarHeight + 2;
  int16_t rowsTopY = static_cast<int16_t>(titleY + (title != nullptr ? lh : 0));

  // Viewport scroll: show as many rows as fit below the title, keeping
  // `selected_` inside the visible window at all times (Hardware Fix #1 --
  // required now that PRIMARY's larger line height means fewer rows fit
  // than the old compact font, e.g. up to 41-item Recipient lists).
  int16_t remaining = Display::kScreenHeight - rowsTopY;
  uint8_t visibleRows = (remaining > 0) ? static_cast<uint8_t>(remaining / lh) : 0;
  if (visibleRows == 0) visibleRows = 1;  // always show at least the selected row

  int16_t startIdx = 0;
  if (count_ > visibleRows) {
    if (selected_ >= visibleRows) startIdx = static_cast<int16_t>(selected_) - visibleRows + 1;
    int16_t maxStart = static_cast<int16_t>(count_) - static_cast<int16_t>(visibleRows);
    if (startIdx > maxStart) startIdx = maxStart;
    if (startIdx < 0) startIdx = 0;
  }

  // Hardware Fix #2/#4.1: redraw split, cheapest first.
  //   1. firstDraw (configure() just ran): the only path that clears the
  //      whole content area and (re)draws the title -- everything is new.
  //   2. scrolled (viewport start index moved) or forceFullBadgeRegion
  //      (badge count beyond the tracking cap): every visible row's
  //      logical content may have changed, so redraw the list ROW REGION
  //      only (never the title, never the status bar).
  //   3. same viewport, within the badge-tracking cap: the selection
  //      marker and each row's badge cell are diffed and redrawn
  //      completely independently of each other and of the label, so a
  //      plain rotate (marker only), an in-place picker confirm (badge
  //      cells only, marker unchanged), or both together in one tick
  //      never force a full row-region repaint. This is what eliminates
  //      the confirm-time flicker a push+goBack()+re-enter cycle caused.
  bool firstDraw = needsFullRedraw_;
  bool scrolled = !firstDraw && (startIdx != lastDrawnStartIdx_);
  bool selectionChanged = !firstDraw && !scrolled && (selected_ != lastDrawnSelected_);

  // Badges are polled every tick regardless of redraw state (they're
  // arbitrary caller-provided bool fns, e.g. unread-message checks, and
  // must stay live) -- for the visible rows only -- but only trigger a
  // redraw when a value actually flips, per Hardware Fix #1. lastBadge_
  // holds the visible rows' last drawn values (Hardware Fix #4.1: a
  // same-viewport flip redraws only those rows' badge cells); a scroll
  // redraws the row region anyway. Should more rows ever be visible than
  // kBadgeCacheCap, every tick conservatively redraws the row region.
  uint8_t shownRows = 0;
  if (count_ > static_cast<uint8_t>(startIdx))
    shownRows = static_cast<uint8_t>(count_ - startIdx) < visibleRows ? static_cast<uint8_t>(count_ - startIdx) : visibleRows;
  bool canTrackBadges = (badges_ != nullptr) && (shownRows <= kBadgeCacheCap);
  bool forceFullBadgeRegion = (badges_ != nullptr) && !canTrackBadges;
  bool badgeNow[kBadgeCacheCap];
  bool anyBadgeChanged = false;
  if (canTrackBadges) {
    for (uint8_t k = 0; k < shownRows; k++) {
      uint8_t i = static_cast<uint8_t>(startIdx + k);
      badgeNow[k] = (badges_[i] != nullptr) && badges_[i]();
      if (!firstDraw && !scrolled && badgeNow[k] != lastBadge_[k]) anyBadgeChanged = true;
    }
  }

  if (!firstDraw && !scrolled && !forceFullBadgeRegion && !anyBadgeChanged && !selectionChanged) return false;

  // Marker column: reserved width is measured from the marker glyph
  // itself (not assumed), and the label always starts at the same fixed X
  // regardless of whether a marker is drawn there -- PRIMARY is a
  // proportional GFX font, so "> " and "  " are not guaranteed equal
  // width, and concatenating them into one string (the pre-Fix-#2
  // approach) both prevented a marker-only update and risked the label
  // shifting a pixel or two depending on which was drawn. The badge
  // suffix (" *") is likewise drawn as its own separate cell, at
  // labelX + textWidth(label) -- computed from the actual label text,
  // never a fixed character-width assumption -- so a badge flip never
  // touches the label (Hardware Fix #4.1).
  int16_t markerW = static_cast<int16_t>(Display::textWidth(">") + 4);
  int16_t labelX = static_cast<int16_t>(2 + markerW);
  int16_t badgeCellW = static_cast<int16_t>(Display::textWidth(" *") + 4);
  bool reserved = (badgeLayout_ == BadgeLayout::RESERVED_COLUMN);
  // Right-aligned with the clipping measure (fitWidth(): the glyphs' actual
  // extent, as printLine() clips), with a small margin, so the badge is
  // never clipped at the screen edge.
  int16_t reservedBadgeX = static_cast<int16_t>(Display::kScreenWidth - Display::fitWidth(" *") - 2);
  auto badgeXFor = [&](uint8_t i) -> int16_t {
    return reserved ? reservedBadgeX : static_cast<int16_t>(labelX + Display::textWidth(items_[i].label));
  };
  // RESERVED_COLUMN: the label is cut (whole glyphs) to end before the badge
  // cell, badge drawn or not, so the layout never shifts.
  auto drawLabel = [&](uint8_t i, int16_t rowY) {
    if (!reserved) {
      Display::printLine(labelX, rowY, items_[i].label);
      return;
    }
    char cut[Display::kPrintLineBufferSize];
    strncpy(cut, items_[i].label, sizeof(cut) - 1);
    cut[sizeof(cut) - 1] = '\0';
    size_t n = strlen(cut);
    while (n > 0 && labelX + Display::fitWidth(cut) > reservedBadgeX) cut[--n] = '\0';
    Display::printLine(labelX, rowY, cut);
  };

  auto drawRowFull = [&](uint8_t i, int16_t rowY, bool badge) {
    if (i == selected_) Display::printLine(2, rowY, ">");
    drawLabel(i, rowY);
    if (badge) Display::printLine(badgeXFor(i), rowY, " *");
  };
  auto badgeOf = [&](uint8_t i) -> bool {
    if (canTrackBadges) return badgeNow[i - startIdx];
    return badges_ != nullptr && badges_[i] != nullptr && badges_[i]();
  };

  if (firstDraw) {
    Display::clearContentArea();
    if (title != nullptr) Display::printLine(2, titleY, title);
    for (uint8_t i = static_cast<uint8_t>(startIdx); i < count_ && i < startIdx + visibleRows; i++) {
      drawRowFull(i, static_cast<int16_t>(rowsTopY + (i - startIdx) * lh), badgeOf(i));
    }
    needsFullRedraw_ = false;
  } else if (scrolled || forceFullBadgeRegion) {
    int16_t regionH = static_cast<int16_t>(Display::kScreenHeight - rowsTopY);
    if (regionH < 0) regionH = 0;
    Display::tft().fillRect(0, rowsTopY, Display::kScreenWidth, regionH, ST77XX_BLACK);
    for (uint8_t i = static_cast<uint8_t>(startIdx); i < count_ && i < startIdx + visibleRows; i++) {
      drawRowFull(i, static_cast<int16_t>(rowsTopY + (i - startIdx) * lh), badgeOf(i));
    }
  } else {
    // Same viewport: erase each cell explicitly before drawing -- PRIMARY
    // (a GFX custom font) never draws with an opaque background, so a
    // stale glyph would otherwise remain visible under new content.
    if (selectionChanged) {
      int16_t oldY = static_cast<int16_t>(rowsTopY + (lastDrawnSelected_ - startIdx) * lh);
      int16_t newY = static_cast<int16_t>(rowsTopY + (selected_ - startIdx) * lh);
      Display::tft().fillRect(2, oldY, markerW, lh, ST77XX_BLACK);
      Display::tft().fillRect(2, newY, markerW, lh, ST77XX_BLACK);
      Display::printLine(2, newY, ">");
    }
    if (anyBadgeChanged) {
      for (uint8_t k = 0; k < shownRows; k++) {
        if (badgeNow[k] == lastBadge_[k]) continue;
        uint8_t i = static_cast<uint8_t>(startIdx + k);
        int16_t rowY = static_cast<int16_t>(rowsTopY + k * lh);
        int16_t badgeX = badgeXFor(i);
        Display::tft().fillRect(badgeX, rowY, badgeCellW, lh, ST77XX_BLACK);
        if (badgeNow[k]) Display::printLine(badgeX, rowY, " *");
      }
    }
  }

  if (canTrackBadges) {
    for (uint8_t k = 0; k < kBadgeCacheCap; k++) lastBadge_[k] = k < shownRows && badgeNow[k];
  }
  lastDrawnStartIdx_ = startIdx;
  lastDrawnSelected_ = selected_;
  return false;
}

namespace {
ConfirmPromptConfig g_confirmConfig;
bool g_confirmYesSelected = true;

// Hardware Fix #3A: full draw only on first entry (startConfirmPrompt());
// a Yes/No toggle redraws only the selector row below -- the prompt
// text/title area is drawn once and never touched again.
bool g_confirmNeedsFullRedraw = true;
bool g_confirmLastYesSelected = true;
}  // namespace

namespace Menu {

void startConfirmPrompt(const ConfirmPromptConfig& config) {
  g_confirmConfig = config;
  g_confirmYesSelected = config.defaultYes;
  g_confirmNeedsFullRedraw = true;
}

void confirmPromptScreen() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      g_confirmYesSelected = !g_confirmYesSelected;
    } else if (Input::isMenuConfirm(e)) {
      if (g_confirmYesSelected) {
        if (g_confirmConfig.onYes != nullptr) g_confirmConfig.onYes();
      } else {
        if (g_confirmConfig.onNo != nullptr) g_confirmConfig.onNo();
      }
      Menu::goBack();
      return;
    } else if (Input::isBack(e)) {
      if (g_confirmConfig.onNo != nullptr) g_confirmConfig.onNo();
      Menu::goBack();
      return;
    }
  }

  // This modal never clears/owns the status bar row itself, but it's still
  // visible underneath it, so keep it live every tick regardless of the
  // content-area gate below (Hardware Fix #1 correction).
  Display::drawStatusBar();

  bool firstDraw = g_confirmNeedsFullRedraw;
  bool selectionChanged = !firstDraw && (g_confirmYesSelected != g_confirmLastYesSelected);
  if (!firstDraw && !selectionChanged) return;

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t y = Display::kStatusBarHeight + 4;
  int16_t selectorY = static_cast<int16_t>(y + lh + (g_confirmConfig.line2 != nullptr ? lh : 0) + 8);

  // Yes/No labels are fixed text -- only the "> " marker between them ever
  // moves, so a plain toggle never touches label pixels (same class fix as
  // MixedTextEntry's CONFIRM selector; Hardware Fix #3 corrective re-scan).
  int16_t markerW = static_cast<int16_t>(Display::textWidth(">") + 4);
  int16_t yesMarkerX = 2;
  int16_t yesLabelX = static_cast<int16_t>(yesMarkerX + markerW);
  int16_t yesLabelW = Display::textWidth("Yes");
  int16_t gapW = Display::textWidth("    ");
  int16_t noMarkerX = static_cast<int16_t>(yesLabelX + yesLabelW + gapW);
  int16_t noLabelX = static_cast<int16_t>(noMarkerX + markerW);

  if (firstDraw) {
    Display::clearContentArea();
    Display::printLine(2, y, g_confirmConfig.line1);
    y += lh;
    if (g_confirmConfig.line2 != nullptr) {
      Display::printLine(2, y, g_confirmConfig.line2);
      y += lh;
    }
    if (g_confirmYesSelected) Display::printLine(yesMarkerX, selectorY, ">");
    Display::printLine(yesLabelX, selectorY, "Yes");
    if (!g_confirmYesSelected) Display::printLine(noMarkerX, selectorY, ">");
    Display::printLine(noLabelX, selectorY, "No");
    g_confirmNeedsFullRedraw = false;
  } else {
    // PRIMARY is a GFX custom font and never draws with an opaque
    // background, so each marker cell is explicitly erased before the
    // replacement is drawn -- the labels themselves are never touched.
    int16_t oldMarkerX = g_confirmLastYesSelected ? yesMarkerX : noMarkerX;
    int16_t newMarkerX = g_confirmYesSelected ? yesMarkerX : noMarkerX;
    Display::tft().fillRect(oldMarkerX, selectorY, markerW, lh, ST77XX_BLACK);
    Display::tft().fillRect(newMarkerX, selectorY, markerW, lh, ST77XX_BLACK);
    Display::printLine(newMarkerX, selectorY, ">");
  }

  g_confirmLastYesSelected = g_confirmYesSelected;
}

}  // namespace Menu

void comingSoonScreen() {
  // Captured before any input is processed below: Input::isBack() can call
  // Menu::goBack(), which sets the *new* top-of-stack screen's "just
  // entered" flag -- checking consumeJustEntered() after that point would
  // steal that screen's own entry signal instead of reading this screen's
  // own (established pattern: every screen consumes it as its first
  // statement, before Input::update()).
  bool justEntered = Menu::consumeJustEntered();

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isMenuConfirm(e) || Input::isBack(e)) {
      Menu::goBack();
    }
  }

  // Called unconditionally, before the content-area gate below, so
  // connectivity/battery stay fresh for as long as the user remains on
  // this screen (Hardware Fix #1 correction) -- drawStatusBar() is
  // internally dirty-gated, so this is safe/cheap every tick.
  Display::drawStatusBar();
  if (!justEntered) return;

  Display::setFont(Display::Font::PRIMARY);
  Display::clearContentArea();
  Display::printLine(60, 60, "Coming Soon");
}
