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

// Returns true while the item it's bound to should show an unread/pending
// badge marker.
using BadgeFn = bool (*)();

// Reusable wrapping list widget (Addendum: "all menu/list encoder
// navigation wraps"). DOT/DASH confirms the highlighted item by pushing
// its onSelect screen; Encoder long goes back.
//
// Hardware Fix #1: redraws only when something visible actually changed
// (configure() called, selection moved, or a badge flipped) instead of
// every tick, and scrolls a viewport window when there are more items than
// fit the content area at the larger PRIMARY font -- the selected item is
// always kept inside the visible window.
//
// Hardware Fix #2: a plain selection move within the same viewport (the
// overwhelmingly common case) no longer clears/redraws the title or any
// row label -- only the "> " marker glyph moves from the old selected row
// to the new one. A full clear+redraw only happens on the first draw after
// configure(); a viewport scroll or a badge change redraws just the list
// row region (never the title or status bar). See ListMenu::tick()'s
// implementation comment for the exact three-way redraw split.
class ListMenu {
 public:
  // badges, when given, must point to an array the same length as items
  // (entries may be nullptr for "no badge"); the array must outlive this
  // ListMenu instance (a static/global array, as with items). Badge-change
  // dirty-tracking only applies up to kBadgeCacheCap items; beyond that,
  // badges are treated as always-possibly-changed (correct, just not
  // optimized -- no caller currently passes more than a handful).
  static constexpr uint8_t kBadgeCacheCap = 8;

  // PUSH_SCREEN (default): DOT/DASH confirm pushes items_[selected_].onSelect
  // as a new screen, same as ever -- every existing navigation menu keeps
  // this behavior unchanged.
  // IN_PLACE (Hardware Fix #4.1): DOT/DASH confirm instead calls
  // items_[selected_].onSelect() directly, as a plain function call, with
  // no screen push and no navigation-stack change. For a persisted
  // setting picker (Typing Display, Mute Radio Outside, Audio Preview,
  // Reveal Answer, Select Level) this lets confirming a choice update the
  // saved value and its "*" badge in place, on the same screen, with no
  // clearContentArea()/full redraw and no re-entry through configure() --
  // eliminating the confirm-time flicker a push+goBack()+re-enter cycle
  // caused even though it was visually the same picker screen throughout.
  // An IN_PLACE onSelect must NOT call Menu::goBack() or push a screen;
  // it should only save/apply the value and fire any required hook.
  enum class SelectionMode : uint8_t { PUSH_SCREEN, IN_PLACE };

  // initialSelected: the cursor's starting row. Callers rendering a
  // persisted enum/bool picker pass the index matching the currently saved
  // value, so the picker opens on the active choice instead of always
  // snapping back to item 0 (Hardware Fix #4 issue 1). Defaults to 0 so
  // existing non-persisted-picker callers are unaffected. Out-of-range
  // values are clamped to 0.
  void configure(const SettingItem* items, uint8_t count, const BadgeFn* badges = nullptr,
                 uint8_t initialSelected = 0, SelectionMode selectionMode = SelectionMode::PUSH_SCREEN);
  void tick(const char* title);
  uint8_t selectedIndex() const { return selected_; }

 private:
  const SettingItem* items_ = nullptr;
  const BadgeFn* badges_ = nullptr;
  uint8_t count_ = 0;
  uint8_t selected_ = 0;
  SelectionMode selectionMode_ = SelectionMode::PUSH_SCREEN;
  bool needsFullRedraw_ = true;
  int16_t lastDrawnStartIdx_ = -1;  // -1: nothing drawn yet (forces firstDraw's own path anyway)
  uint8_t lastDrawnSelected_ = 0;
  bool lastBadge_[kBadgeCacheCap] = {};
};

namespace Menu {
// Registers a badge provider for one Main Menu item (Addendum/Phase 2
// section 14: "Main Menu Text badge"). itemIndex matches Modes::MainMenuIndex.
// fn is polled every Main Menu render; return true to show a marker next
// to that item's label. Phase 1 shipped with no badge concept at all —
// this is the one small additive hook agreed for Phase 2.
void registerMainMenuBadge(uint8_t itemIndex, BadgeFn fn);
}  // namespace Menu

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
