#pragma once
// Settings tree + Training Game shell (Phase 1 sections 5-10).

#include <stdint.h>

#include "core/hooks.h"  // ScreenHandlerFn

namespace Settings {

constexpr uint8_t kMaxWifiSlots = 3;
constexpr uint8_t kMaxGroups = 5;

struct WifiSlot {
  bool configured;
  char ssid[33];
  char password[65];
};

struct FamilyGroup {
  bool configured;
  char name[21];
  char code[33];
};

enum class TypingDisplay : uint8_t { MORSE_ONLY = 0, LETTERS_ONLY = 1, MIXED = 2 };

// Loads every persisted value from Storage and applies the ones with an
// immediate hardware effect (backlight, sleep timeout). Call once after
// Storage::init().
void init();

// ---- My Name -------------------------------------------------------------
// Single canonical maximum for the user's chosen name, shared by every UI
// site (Set Name, My Name editor, compact conversation layout) so they can
// never disagree (Hardware Fix #4.7). A valid configured name is 1..
// kMaxMyNameLen characters -- there is no compiled "Me"/default placeholder.
constexpr uint8_t kMaxMyNameLen = 4;

// Empty ("") until a valid name has been configured. Never returns a
// placeholder.
const char* getMyName();
// name must already be 1..kMaxMyNameLen characters (MixedTextEntry
// enforces this while typing); persists to NVS key "myName".
void setMyName(const char* name);
// True once a VALID (1..kMaxMyNameLen character) user-selected name
// exists, whether just set this session or loaded from NVS. False for a
// never-configured device AND for a legacy NVS value longer than
// kMaxMyNameLen (Hardware Fix #4.7 migration rule) -- both cases require
// screenSetName() before normal use. Callers that present this device's
// name to OTHER people (a message sender label, a contact's resolved
// display name) check this first so an unconfigured device is never shown
// under a fabricated name (Hardware Fix #4.3 issue F).
bool hasCustomMyName();

// ---- Personal Identity Color (Feature Fix #4.8) ---------------------------
// A Morse Buddy user identity is Name + Personal Color; the color
// distinguishes family members' sender names in the Unified Thread.
// Persisted as a palette INDEX (never a raw RGB565 value) so the palette
// itself can be defined once, centrally, in identity_color.h/.cpp.
//
// IdentityColor::kInvalidColor until a valid color has been configured --
// never a fabricated default, matching the existing Name migration rule.
uint8_t getMyColorIndex();
uint16_t getMyColor565();  // IdentityColor::color565(getMyColorIndex()); safe even if not configured
bool hasCustomMyColor();

// True once BOTH a valid name and a valid color are configured -- the
// gate main.cpp uses for the mandatory first-run identity workflow
// (Feature Fix #4.8), replacing the old hasCustomMyName()-only check. An
// existing valid 1..kMaxMyNameLen name from before this fix is preserved
// and never discarded; such a device is simply asked for a color only.
bool hasConfiguredIdentity();

// Atomic identity save: validates name (non-null, length 1..kMaxMyNameLen)
// AND colorIndex (IdentityColor::isValid()) BEFORE touching any persisted
// or RAM state -- an invalid name is never silently truncated and an
// invalid color index is never silently substituted. On success, persists
// both name and color and returns true; the caller is responsible for
// firing the settings-change notification exactly once afterward (see
// SET_MY_NAME_CHANGED). Returns false, with NO state changed at all, if
// either input is invalid -- a rejected save can never leave a
// half-updated identity (e.g. new name with the old color, or vice
// versa).
bool setMyIdentity(const char* name, uint8_t colorIndex);

// ---- Display & Sound -------------------------------------------------------
uint8_t getBrightness();  // 0-100
void setBrightness(uint8_t percent);
uint8_t getSpeakerVolume();  // 0-100, persist only (Phase 3 backend uses it)
void setSpeakerVolume(uint8_t percent);
TypingDisplay getTypingDisplay();
void setTypingDisplay(TypingDisplay value);
bool getMuteRadioOutsideRadio();
void setMuteRadioOutsideRadio(bool value);

// ---- Speed & Power ---------------------------------------------------------
uint8_t getWpm();  // 1-50
void setWpm(uint8_t wpm);
uint8_t getSleepTimeoutMinutes();  // 0 = Disabled, else 1-30
void setSleepTimeoutMinutes(uint8_t minutes);

// ---- Morse Practice ---------------------------------------------------------
uint8_t getPracticeLevel();  // 1-3
void setPracticeLevel(uint8_t level);

// Setting-item registry list ID (Addendum section 3.9) for the Morse
// Practice screen. Phase 1 registers "Level Select 1/2/3" into it; Phase 3
// registers Audio Preview and Reveal Answer into this same list ID without
// touching this file.
constexpr uint8_t kMorsePracticeListId = 0;

// Phase 1's "Start Practice" item had no real game to point at yet, so it
// was hardcoded to the built-in Coming Soon screen. This lets Phase 3
// register the actual gameplay screen without touching that hardcoding
// site more than once; unregistered (fn == nullptr) keeps today's
// Coming Soon behavior.
void registerMorsePracticeStartHandler(ScreenHandlerFn fn);

// ---- WiFi slots ---------------------------------------------------------
WifiSlot getWifiSlot(uint8_t index);
void setWifiSlot(uint8_t index, const WifiSlot& slot);
void clearWifiSlot(uint8_t index);
bool hasAnyWifiConfigured();

// ---- Family Groups ---------------------------------------------------------
uint8_t getGroupCount();
FamilyGroup getGroup(uint8_t index);
bool isGroupCodeUnique(const char* code);
bool addGroup(const char* name, const char* code);  // false if full or code not unique
void renameGroup(uint8_t index, const char* newName);
void deleteGroup(uint8_t index);

// ---- Screens (ScreenHandlerFn; push via Menu::pushScreen) -------------------
void screenRoot();          // Settings root list
void screenWifiSlots();     // Connectivity > WiFi
void screenFamilyGroups();  // Connectivity > Family Groups
void screenTrainingGame();  // Main Menu > Training Game
// Mandatory first-run name entry (Hardware Fix #4.7). Pushed from main.cpp
// when !hasCustomMyName() so it sits on top of the startup stack; a
// CANCELLED MixedTextEntry session re-enters itself instead of going back,
// so there is no way to reach whatever screen is underneath (WiFi setup or
// Main Menu) without saving a valid name first.
void screenSetName();

}  // namespace Settings
