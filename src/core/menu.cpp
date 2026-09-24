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
  }
}

void goBack() {
  if (g_stackSize > 1) {
    g_stackSize--;
    g_justEntered = true;
  }
}

bool consumeJustEntered() {
  bool v = g_justEntered;
  g_justEntered = false;
  return v;
}

}  // namespace Menu

void ListMenu::configure(const SettingItem* items, uint8_t count, const BadgeFn* badges) {
  items_ = items;
  badges_ = badges;
  count_ = count;
  selected_ = 0;
  dirty_ = true;
  for (auto& b : lastBadge_) b = false;
}

void ListMenu::tick(const char* title) {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE && count_ > 0) {
      uint8_t prev = selected_;
      int16_t next = static_cast<int16_t>(selected_) + e.value;
      if (next < 0) next = static_cast<int16_t>(count_) - 1;
      if (next >= static_cast<int16_t>(count_)) next = 0;
      selected_ = static_cast<uint8_t>(next);
      if (selected_ != prev) dirty_ = true;
    } else if (Input::isMenuConfirm(e) && count_ > 0) {
      ScreenHandlerFn fn = items_[selected_].onSelect;
      if (fn != nullptr) Menu::pushScreen(fn);
    } else if (Input::isBack(e)) {
      Menu::goBack();
    }
  }

  // Badges are polled every tick regardless of dirty_ (they're arbitrary
  // caller-provided bool fns, e.g. unread-message checks, and must stay
  // live) but only trigger a redraw when a value actually flips, per
  // Hardware Fix #1. Beyond kBadgeCacheCap items the per-item cache can't
  // be used safely, so those lists are conservatively always redrawn when
  // badges_ is set (correct, just not optimized -- no current caller
  // exceeds the cap).
  bool canTrackBadges = (badges_ != nullptr) && (count_ <= kBadgeCacheCap);
  bool badgesChanged = (badges_ != nullptr) && !canTrackBadges;
  bool badgeNow[kBadgeCacheCap];
  if (canTrackBadges) {
    for (uint8_t i = 0; i < count_; i++) {
      badgeNow[i] = (badges_[i] != nullptr) && badges_[i]();
      if (badgeNow[i] != lastBadge_[i]) badgesChanged = true;
    }
  }

  if (!dirty_ && !badgesChanged) return;

  if (canTrackBadges) {
    for (uint8_t i = 0; i < count_; i++) lastBadge_[i] = badgeNow[i];
  }
  dirty_ = false;

  Display::setFont(Display::Font::PRIMARY);
  Display::clearContentArea();
  int16_t lh = Display::lineHeight();

  int16_t y = Display::kStatusBarHeight + 2;
  if (title != nullptr) {
    Display::printLine(2, y, title);
    y += lh;
  }

  // Viewport scroll: show as many rows as fit below the title, keeping
  // `selected_` inside the visible window at all times (Hardware Fix #1 --
  // required now that PRIMARY's larger line height means fewer rows fit
  // than the old compact font, e.g. up to 41-item Recipient lists).
  int16_t remaining = Display::kScreenHeight - y;
  uint8_t visibleRows = (remaining > 0) ? static_cast<uint8_t>(remaining / lh) : 0;
  if (visibleRows == 0) visibleRows = 1;  // always show at least the selected row

  int16_t startIdx = 0;
  if (count_ > visibleRows) {
    if (selected_ >= visibleRows) startIdx = static_cast<int16_t>(selected_) - visibleRows + 1;
    int16_t maxStart = static_cast<int16_t>(count_) - static_cast<int16_t>(visibleRows);
    if (startIdx > maxStart) startIdx = maxStart;
    if (startIdx < 0) startIdx = 0;
  }

  for (uint8_t i = static_cast<uint8_t>(startIdx); i < count_ && i < startIdx + visibleRows; i++) {
    bool badge = (badges_ != nullptr && badges_[i] != nullptr && badges_[i]());
    char line[56];
    snprintf(line, sizeof(line), "%s%s%s", i == selected_ ? "> " : "  ", items_[i].label, badge ? " *" : "");
    Display::printLine(2, y, line);
    y += lh;
  }
}

namespace {
ConfirmPromptConfig g_confirmConfig;
bool g_confirmYesSelected = true;
bool g_confirmDirty = true;
}  // namespace

namespace Menu {

void startConfirmPrompt(const ConfirmPromptConfig& config) {
  g_confirmConfig = config;
  g_confirmYesSelected = config.defaultYes;
  g_confirmDirty = true;
}

void confirmPromptScreen() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      g_confirmYesSelected = !g_confirmYesSelected;
      g_confirmDirty = true;
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
  // content-area dirty gate below (Hardware Fix #1 correction).
  Display::drawStatusBar();
  if (!g_confirmDirty) return;
  g_confirmDirty = false;

  Display::setFont(Display::Font::PRIMARY);
  Display::clearContentArea();
  int16_t lh = Display::lineHeight();
  int16_t y = Display::kStatusBarHeight + 4;
  Display::printLine(2, y, g_confirmConfig.line1);
  y += lh;
  if (g_confirmConfig.line2 != nullptr) {
    Display::printLine(2, y, g_confirmConfig.line2);
    y += lh;
  }
  Display::printLine(2, y + 8, g_confirmYesSelected ? "> Yes    No" : "  Yes  > No");
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
