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
const char* getMyName();
void setMyName(const char* name);

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

}  // namespace Settings
