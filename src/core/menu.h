#pragma once
// Screen navigation stack + generic wrapping list widget.
//
// A "screen" is any ScreenHandlerFn (void(*)()): a non-blocking per-frame
// update function that polls Input itself and draws itself. Menu::tick()
// calls only the current top-of-stack screen every loop() iteration.

#include "core/hooks.h"  // ScreenHandlerFn, SettingItem

namespace Menu {

// Builds and pushes the Main Menu as the root (bottom of stack) screen.
void init();

// Call every loop() iteration.
void tick();

void pushScreen(ScreenHandlerFn fn);

// Pops one level. The root Main Menu is never popped.
void goBack();

// True exactly once for the first tick() after a push/pop changed the
// current screen; screens call this to run one-time setup (e.g. reset a
// list's selection, or call MixedTextEntry::start()).
bool consumeJustEntered();

}  // namespace Menu

// Reusable wrapping list widget (Addendum: "all menu/list encoder
// navigation wraps"). DOT/DASH confirms the highlighted item by pushing
// its onSelect screen; Encoder long goes back.
class ListMenu {
 public:
  void configure(const SettingItem* items, uint8_t count);
  void tick(const char* title);
  uint8_t selectedIndex() const { return selected_; }

 private:
  const SettingItem* items_ = nullptr;
  uint8_t count_ = 0;
  uint8_t selected_ = 0;
};

// Fallback screen for any unregistered Mode/Game-submode handler.
void comingSoonScreen();

// Generic two-option (Yes/No) confirmation prompt, e.g. "Delete this
// group? All history will be lost." Rotate toggles the option, DOT/DASH
// confirms, Encoder long cancels (treated as No). onYes/onNo are called
// just before Menu::goBack() runs.
struct ConfirmPromptConfig {
  const char* line1;
  const char* line2;  // may be nullptr
  bool defaultYes;
  void (*onYes)();  // may be nullptr
  void (*onNo)();   // may be nullptr
};

namespace Menu {
void startConfirmPrompt(const ConfirmPromptConfig& config);
void confirmPromptScreen();
}  // namespace Menu
