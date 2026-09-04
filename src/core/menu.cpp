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

void trampolineText() {
  ScreenHandlerFn fn = getModeHandler(Modes::TEXT);
  Menu::pushScreen(fn != nullptr ? fn : comingSoonScreen);
}
void trampolineEnigma() {
  ScreenHandlerFn fn = getModeHandler(Modes::ENIGMA);
  Menu::pushScreen(fn != nullptr ? fn : comingSoonScreen);
}
void trampolineRadio() {
  ScreenHandlerFn fn = getModeHandler(Modes::RADIO);
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

ListMenu g_mainMenuList;

void mainMenuScreen() {
  if (Menu::consumeJustEntered()) {
    g_mainMenuList.configure(kMainMenuItems, kMainMenuItemCount);
  }
  Display::drawStatusBar();
  g_mainMenuList.tick("Morse Buddy");
}

}  // namespace

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

void ListMenu::configure(const SettingItem* items, uint8_t count) {
  items_ = items;
  count_ = count;
  selected_ = 0;
}

void ListMenu::tick(const char* title) {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE && count_ > 0) {
      int16_t next = static_cast<int16_t>(selected_) + e.value;
      if (next < 0) next = static_cast<int16_t>(count_) - 1;
      if (next >= static_cast<int16_t>(count_)) next = 0;
      selected_ = static_cast<uint8_t>(next);
    } else if (Input::isMenuConfirm(e) && count_ > 0) {
      ScreenHandlerFn fn = items_[selected_].onSelect;
      if (fn != nullptr) Menu::pushScreen(fn);
    } else if (Input::isBack(e)) {
      Menu::goBack();
    }
  }

  Adafruit_ST7789& tft = Display::tft();
  Display::clearContentArea();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  int16_t y = Display::kStatusBarHeight + 2;
  if (title != nullptr) {
    tft.setCursor(2, y);
    tft.print(title);
    y += 12;
  }

  for (uint8_t i = 0; i < count_; i++) {
    tft.setCursor(2, y);
    tft.print(i == selected_ ? "> " : "  ");
    tft.print(items_[i].label);
    y += 12;
  }
}

namespace {
ConfirmPromptConfig g_confirmConfig;
bool g_confirmYesSelected = true;
}  // namespace

namespace Menu {

void startConfirmPrompt(const ConfirmPromptConfig& config) {
  g_confirmConfig = config;
  g_confirmYesSelected = config.defaultYes;
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

  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, Display::kStatusBarHeight + 4);
  tft.print(g_confirmConfig.line1);
  if (g_confirmConfig.line2 != nullptr) {
    tft.setCursor(2, Display::kStatusBarHeight + 16);
    tft.print(g_confirmConfig.line2);
  }
  tft.setCursor(2, Display::kStatusBarHeight + 40);
  tft.print(g_confirmYesSelected ? "> Yes    No" : "  Yes  > No");
}

}  // namespace Menu

void comingSoonScreen() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isMenuConfirm(e) || Input::isBack(e)) {
      Menu::goBack();
    }
  }

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(60, 60);
  tft.print("Coming Soon");
}
