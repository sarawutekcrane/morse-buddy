#include "core/settings.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/mixed_text_entry.h"
#include "core/modes.h"
#include "core/morse.h"
#include "core/sleep.h"
#include "core/storage_init.h"

// =============================================================================
// Persistence (RAM cache backed by compact NVS namespaces, Addendum 4.4).
// =============================================================================
namespace {

Settings::WifiSlot g_wifiSlots[Settings::kMaxWifiSlots];
Settings::FamilyGroup g_groups[Settings::kMaxGroups];
uint8_t g_groupCount = 0;

char g_myName[17] = "Me";
uint8_t g_brightness = 100;
uint8_t g_speakerVolume = 80;
Settings::TypingDisplay g_typingDisplay = Settings::TypingDisplay::MIXED;
bool g_muteRadioOutsideRadio = false;
uint8_t g_wpm = 15;
uint8_t g_sleepTimeoutMinutes = 5;
uint8_t g_practiceLevel = 1;

void loadWifiSlots() {
  size_t got = Storage::wifi().getBytes("slots", g_wifiSlots, sizeof(g_wifiSlots));
  if (got != sizeof(g_wifiSlots)) memset(g_wifiSlots, 0, sizeof(g_wifiSlots));
}
void saveWifiSlots() { Storage::wifi().putBytes("slots", g_wifiSlots, sizeof(g_wifiSlots)); }

void loadGroups() {
  size_t got = Storage::group().getBytes("groups", g_groups, sizeof(g_groups));
  if (got != sizeof(g_groups)) memset(g_groups, 0, sizeof(g_groups));
  g_groupCount = 0;
  for (uint8_t i = 0; i < Settings::kMaxGroups; i++) {
    if (g_groups[i].configured) g_groupCount++;
  }
}
void saveGroups() { Storage::group().putBytes("groups", g_groups, sizeof(g_groups)); }

}  // namespace

// Forward declaration: Settings::init() registers this screen into the
// Morse Practice setting-item list before it is defined further below.
namespace {
void screenMorsePracticeLevelPicker();
}  // namespace

namespace Settings {

void init() {
  Preferences& p = Storage::core();
  g_brightness = p.isKey("bright") ? p.getUChar("bright") : 100;
  g_speakerVolume = p.isKey("vol") ? p.getUChar("vol") : 80;
  g_typingDisplay = static_cast<TypingDisplay>(p.isKey("typDisp") ? p.getUChar("typDisp")
                                                                   : static_cast<uint8_t>(TypingDisplay::MIXED));
  g_muteRadioOutsideRadio = p.isKey("muteRadio") ? p.getBool("muteRadio") : false;
  g_wpm = p.isKey("wpm") ? p.getUChar("wpm") : 15;
  g_sleepTimeoutMinutes = p.isKey("sleepMin") ? p.getUChar("sleepMin") : 5;
  g_practiceLevel = p.isKey("practLvl") ? p.getUChar("practLvl") : 1;
  if (p.isKey("myName")) {
    p.getString("myName", g_myName, sizeof(g_myName));
  } else {
    strncpy(g_myName, "Me", sizeof(g_myName) - 1);
    g_myName[sizeof(g_myName) - 1] = '\0';
  }

  loadWifiSlots();
  loadGroups();

  Display::setBacklightPercent(g_brightness);
  Sleep::setTimeoutMinutes(g_sleepTimeoutMinutes);

  registerSettingItem(kMorsePracticeListId, SettingItem{"Level Select 1/2/3", screenMorsePracticeLevelPicker});
}

const char* getMyName() { return g_myName; }
void setMyName(const char* name) {
  strncpy(g_myName, name, sizeof(g_myName) - 1);
  g_myName[sizeof(g_myName) - 1] = '\0';
  Storage::core().putString("myName", g_myName);
}

uint8_t getBrightness() { return g_brightness; }
void setBrightness(uint8_t percent) {
  g_brightness = percent;
  Storage::core().putUChar("bright", percent);
}

uint8_t getSpeakerVolume() { return g_speakerVolume; }
void setSpeakerVolume(uint8_t percent) {
  g_speakerVolume = percent;
  Storage::core().putUChar("vol", percent);
}

TypingDisplay getTypingDisplay() { return g_typingDisplay; }
void setTypingDisplay(TypingDisplay value) {
  g_typingDisplay = value;
  Storage::core().putUChar("typDisp", static_cast<uint8_t>(value));
}

bool getMuteRadioOutsideRadio() { return g_muteRadioOutsideRadio; }
void setMuteRadioOutsideRadio(bool value) {
  g_muteRadioOutsideRadio = value;
  Storage::core().putBool("muteRadio", value);
}

uint8_t getWpm() { return g_wpm; }
void setWpm(uint8_t wpm) {
  g_wpm = wpm;
  Storage::core().putUChar("wpm", wpm);
}

uint8_t getSleepTimeoutMinutes() { return g_sleepTimeoutMinutes; }
void setSleepTimeoutMinutes(uint8_t minutes) {
  g_sleepTimeoutMinutes = minutes;
  Storage::core().putUChar("sleepMin", minutes);
}

uint8_t getPracticeLevel() { return g_practiceLevel; }
void setPracticeLevel(uint8_t level) {
  g_practiceLevel = level;
  Storage::core().putUChar("practLvl", level);
}

WifiSlot getWifiSlot(uint8_t index) {
  if (index >= kMaxWifiSlots) return WifiSlot{false, {0}, {0}};
  return g_wifiSlots[index];
}
void setWifiSlot(uint8_t index, const WifiSlot& slot) {
  if (index >= kMaxWifiSlots) return;
  g_wifiSlots[index] = slot;
  saveWifiSlots();
}
void clearWifiSlot(uint8_t index) {
  if (index >= kMaxWifiSlots) return;
  g_wifiSlots[index] = WifiSlot{false, {0}, {0}};
  saveWifiSlots();
}
bool hasAnyWifiConfigured() {
  for (uint8_t i = 0; i < kMaxWifiSlots; i++) {
    if (g_wifiSlots[i].configured) return true;
  }
  return false;
}

uint8_t getGroupCount() { return g_groupCount; }
FamilyGroup getGroup(uint8_t index) {
  if (index >= g_groupCount) return FamilyGroup{false, {0}, {0}};
  return g_groups[index];
}
bool isGroupCodeUnique(const char* code) {
  for (uint8_t i = 0; i < g_groupCount; i++) {
    if (strcmp(g_groups[i].code, code) == 0) return false;
  }
  return true;
}
bool addGroup(const char* name, const char* code) {
  if (g_groupCount >= kMaxGroups) return false;
  if (!isGroupCodeUnique(code)) return false;
  FamilyGroup& g = g_groups[g_groupCount];
  g.configured = true;
  strncpy(g.name, name, sizeof(g.name) - 1);
  g.name[sizeof(g.name) - 1] = '\0';
  strncpy(g.code, code, sizeof(g.code) - 1);
  g.code[sizeof(g.code) - 1] = '\0';
  g_groupCount++;
  saveGroups();
  return true;
}
void renameGroup(uint8_t index, const char* newName) {
  if (index >= g_groupCount) return;
  strncpy(g_groups[index].name, newName, sizeof(g_groups[index].name) - 1);
  g_groups[index].name[sizeof(g_groups[index].name) - 1] = '\0';
  saveGroups();
}
void deleteGroup(uint8_t index) {
  if (index >= g_groupCount) return;
  for (uint8_t i = index; i + 1 < g_groupCount; i++) g_groups[i] = g_groups[i + 1];
  g_groupCount--;
  g_groups[g_groupCount] = FamilyGroup{false, {0}, {0}};
  saveGroups();
}

}  // namespace Settings

// =============================================================================
// Screens
// =============================================================================
namespace {

// ---- forward declarations (mutual navigation) ------------------------------
void screenConnectivity();
void screenEditMyName();
void screenDisplaySound();
void screenSpeedPower();
void screenBrightnessAdjust();
void screenVolumeAdjust();
void screenTypingDisplayPicker();
void screenMuteRadioPicker();
void screenWpmAdjust();
void screenSleepTimeoutAdjust();
void screenWifiSlotDetail();
void screenWifiScanResults();
void screenWifiPasswordEntry();
void screenWifiTestConnection();
void screenGroupDetail();
void screenGroupRenameEntry();
void screenAddGroupName();
void screenAddGroupCode();
void screenNumberGuessing();

// ---- shared numeric-adjust widget (Brightness/Volume/WPM/Sleep Timeout) ----
constexpr int16_t kNoWarn = -32768;

struct NumberAdjustState {
  const char* title;
  int16_t minV, maxV, step, value, warnAt;
  const char* warnText;
  void (*format)(int16_t, char*, size_t);
  void (*onSave)(int16_t);
};
NumberAdjustState g_numAdjust;

void numberAdjustTick() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      int32_t nv = static_cast<int32_t>(g_numAdjust.value) + static_cast<int32_t>(e.value) * g_numAdjust.step;
      if (nv < g_numAdjust.minV) nv = g_numAdjust.minV;
      if (nv > g_numAdjust.maxV) nv = g_numAdjust.maxV;
      g_numAdjust.value = static_cast<int16_t>(nv);
    } else if (Input::isMenuConfirm(e)) {
      if (g_numAdjust.onSave != nullptr) g_numAdjust.onSave(g_numAdjust.value);
      Menu::goBack();
      return;
    } else if (Input::isBack(e)) {
      Menu::goBack();
      return;
    }
  }

  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, Display::kStatusBarHeight + 4);
  tft.print(g_numAdjust.title);

  char buf[24];
  if (g_numAdjust.format != nullptr) {
    g_numAdjust.format(g_numAdjust.value, buf, sizeof(buf));
  } else {
    snprintf(buf, sizeof(buf), "%d", g_numAdjust.value);
  }
  tft.setCursor(2, Display::kStatusBarHeight + 22);
  tft.print(buf);

  if (g_numAdjust.warnAt != kNoWarn && g_numAdjust.value >= g_numAdjust.warnAt) {
    tft.setCursor(2, Display::kStatusBarHeight + 40);
    tft.print(g_numAdjust.warnText);
  }
}

void formatPercent(int16_t v, char* buf, size_t n) { snprintf(buf, n, "%d%%", v); }
void formatSleepTimeout(int16_t v, char* buf, size_t n) {
  if (v == 0) snprintf(buf, n, "Disabled");
  else snprintf(buf, n, "%d min", v);
}

void saveBrightness(int16_t v) {
  Settings::setBrightness(static_cast<uint8_t>(v));
  Display::setBacklightPercent(static_cast<uint8_t>(v));
}
void saveVolume(int16_t v) { Settings::setSpeakerVolume(static_cast<uint8_t>(v)); }
void saveWpm(int16_t v) { Settings::setWpm(static_cast<uint8_t>(v)); }
void saveSleepTimeout(int16_t v) {
  Settings::setSleepTimeoutMinutes(static_cast<uint8_t>(v));
  Sleep::setTimeoutMinutes(static_cast<uint8_t>(v));
}

void screenBrightnessAdjust() {
  if (Menu::consumeJustEntered()) {
    g_numAdjust = {"Brightness", 0, 100, 5, static_cast<int16_t>(Settings::getBrightness()),
                   kNoWarn, nullptr, formatPercent, saveBrightness};
  }
  Display::drawStatusBar();
  numberAdjustTick();
}
void screenVolumeAdjust() {
  if (Menu::consumeJustEntered()) {
    g_numAdjust = {"Speaker Volume", 0, 100, 5, static_cast<int16_t>(Settings::getSpeakerVolume()),
                   kNoWarn, nullptr, formatPercent, saveVolume};
  }
  Display::drawStatusBar();
  numberAdjustTick();
}
void screenWpmAdjust() {
  if (Menu::consumeJustEntered()) {
    g_numAdjust = {"WPM", Morse::kMinWpm, Morse::kMaxWpm, 1, static_cast<int16_t>(Settings::getWpm()),
                   static_cast<int16_t>(Morse::kWpmWarnThreshold), "Warning: high speed", nullptr, saveWpm};
  }
  Display::drawStatusBar();
  numberAdjustTick();
}
void screenSleepTimeoutAdjust() {
  if (Menu::consumeJustEntered()) {
    g_numAdjust = {"Sleep Timeout", 0, 30, 1, static_cast<int16_t>(Settings::getSleepTimeoutMinutes()),
                   kNoWarn, nullptr, formatSleepTimeout, saveSleepTimeout};
  }
  Display::drawStatusBar();
  numberAdjustTick();
}

// ---- Typing Display / Mute Radio pickers -----------------------------------
void typingTrampolineMorse() { Settings::setTypingDisplay(Settings::TypingDisplay::MORSE_ONLY); Menu::goBack(); }
void typingTrampolineLetters() { Settings::setTypingDisplay(Settings::TypingDisplay::LETTERS_ONLY); Menu::goBack(); }
void typingTrampolineMixed() { Settings::setTypingDisplay(Settings::TypingDisplay::MIXED); Menu::goBack(); }
const SettingItem kTypingDisplayItems[] = {
    {"Morse Only", typingTrampolineMorse},
    {"Letters Only", typingTrampolineLetters},
    {"Mixed", typingTrampolineMixed},
};
ListMenu g_typingDisplayListMenu;
void screenTypingDisplayPicker() {
  if (Menu::consumeJustEntered()) g_typingDisplayListMenu.configure(kTypingDisplayItems, 3);
  Display::drawStatusBar();
  g_typingDisplayListMenu.tick("Typing Display");
}

void muteRadioTrampolineOff() {
  Settings::setMuteRadioOutsideRadio(false);
  SettingsChangeInfo info{SET_MUTE_RADIO_CHANGED, 0, {0}};
  fireSettingsChangeHooks(info);
  Menu::goBack();
}
void muteRadioTrampolineOn() {
  Settings::setMuteRadioOutsideRadio(true);
  SettingsChangeInfo info{SET_MUTE_RADIO_CHANGED, 0, {0}};
  fireSettingsChangeHooks(info);
  Menu::goBack();
}
const SettingItem kMuteRadioItems[] = {{"Off", muteRadioTrampolineOff}, {"On", muteRadioTrampolineOn}};
ListMenu g_muteRadioListMenu;
void screenMuteRadioPicker() {
  if (Menu::consumeJustEntered()) g_muteRadioListMenu.configure(kMuteRadioItems, 2);
  Display::drawStatusBar();
  g_muteRadioListMenu.tick("Mute Radio Outside");
}

// ---- Display & Sound / Speed & Power root lists ----------------------------
const SettingItem kDisplaySoundItems[] = {
    {"Brightness", screenBrightnessAdjust},
    {"Speaker Volume", screenVolumeAdjust},
    {"Typing Display", screenTypingDisplayPicker},
    {"Mute Radio Outside", screenMuteRadioPicker},
};
ListMenu g_displaySoundListMenu;
void screenDisplaySound() {
  if (Menu::consumeJustEntered()) g_displaySoundListMenu.configure(kDisplaySoundItems, 4);
  Display::drawStatusBar();
  g_displaySoundListMenu.tick("Display & Sound");
}

const SettingItem kSpeedPowerItems[] = {
    {"WPM", screenWpmAdjust},
    {"Sleep Timeout", screenSleepTimeoutAdjust},
};
ListMenu g_speedPowerListMenu;
void screenSpeedPower() {
  if (Menu::consumeJustEntered()) g_speedPowerListMenu.configure(kSpeedPowerItems, 2);
  Display::drawStatusBar();
  g_speedPowerListMenu.tick("Speed & Power");
}

// ---- My Name ----------------------------------------------------------------
void screenEditMyName() {
  if (Menu::consumeJustEntered()) {
    static const MixedTextEntryConfig cfg = {"My Name", FieldCharset::GENERAL_NAME, 16, 1, nullptr};
    MixedTextEntry::start(cfg, Settings::getMyName());
  }
  MixedTextEntry::tick();
  if (MixedTextEntry::isFinished()) {
    if (MixedTextEntry::result() == MixedTextEntryResult::SAVED) {
      Settings::setMyName(MixedTextEntry::getValue());
      SettingsChangeInfo info{SET_MY_NAME_CHANGED, 0, {0}};
      fireSettingsChangeHooks(info);
    }
    Menu::goBack();
  }
}

// ---- WiFi slots --------------------------------------------------------------
uint8_t g_currentWifiSlotIndex = 0;
char g_chosenSsid[33];

void wifiSlotTrampoline0() { g_currentWifiSlotIndex = 0; Menu::pushScreen(screenWifiSlotDetail); }
void wifiSlotTrampoline1() { g_currentWifiSlotIndex = 1; Menu::pushScreen(screenWifiSlotDetail); }
void wifiSlotTrampoline2() { g_currentWifiSlotIndex = 2; Menu::pushScreen(screenWifiSlotDetail); }
ScreenHandlerFn kWifiSlotTrampolines[Settings::kMaxWifiSlots] = {wifiSlotTrampoline0, wifiSlotTrampoline1,
                                                                 wifiSlotTrampoline2};

SettingItem g_wifiSlotItems[Settings::kMaxWifiSlots];
char g_wifiSlotLabelBuf[Settings::kMaxWifiSlots][40];
ListMenu g_wifiSlotListMenu;

void wifiDetailScan() { Menu::pushScreen(screenWifiScanResults); }
void wifiDetailTest() { Menu::pushScreen(screenWifiTestConnection); }
void wifiDetailClear() {
  Settings::clearWifiSlot(g_currentWifiSlotIndex);
  SettingsChangeInfo info{SET_WIFI_CHANGED, g_currentWifiSlotIndex, {0}};
  fireSettingsChangeHooks(info);
  Menu::goBack();
}
const SettingItem kWifiDetailItems[] = {
    {"Scan & Select SSID", wifiDetailScan},
    {"Test Connection", wifiDetailTest},
    {"Clear Slot", wifiDetailClear},
};
ListMenu g_wifiDetailListMenu;
void screenWifiSlotDetail() {
  if (Menu::consumeJustEntered()) g_wifiDetailListMenu.configure(kWifiDetailItems, 3);
  Display::drawStatusBar();
  g_wifiDetailListMenu.tick("Slot Detail");
}

constexpr uint8_t kMaxScanResults = 12;
char g_scanSsids[kMaxScanResults][33];
uint8_t g_scanCount = 0;
uint8_t g_scanSelected = 0;

void screenWifiScanResults() {
  if (Menu::consumeJustEntered()) {
    g_scanCount = 0;
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n && g_scanCount < kMaxScanResults; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;  // hidden SSID unsupported
      strncpy(g_scanSsids[g_scanCount], ssid.c_str(), 32);
      g_scanSsids[g_scanCount][32] = '\0';
      g_scanCount++;
    }
    WiFi.scanDelete();
    g_scanSelected = 0;
  }

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE && g_scanCount > 0) {
      int16_t next = static_cast<int16_t>(g_scanSelected) + e.value;
      if (next < 0) next = static_cast<int16_t>(g_scanCount) - 1;
      if (next >= static_cast<int16_t>(g_scanCount)) next = 0;
      g_scanSelected = static_cast<uint8_t>(next);
    } else if (Input::isMenuConfirm(e) && g_scanCount > 0) {
      strncpy(g_chosenSsid, g_scanSsids[g_scanSelected], sizeof(g_chosenSsid) - 1);
      g_chosenSsid[sizeof(g_chosenSsid) - 1] = '\0';
      Menu::pushScreen(screenWifiPasswordEntry);
    } else if (Input::isBack(e)) {
      Menu::goBack();
    }
  }

  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print(g_scanCount == 0 ? "No networks found" : "Select SSID:");
  int16_t y = Display::kStatusBarHeight + 14;
  for (uint8_t i = 0; i < g_scanCount; i++) {
    tft.setCursor(2, y);
    tft.print(i == g_scanSelected ? "> " : "  ");
    tft.print(g_scanSsids[i]);
    y += 10;
  }
}

void screenWifiPasswordEntry() {
  if (Menu::consumeJustEntered()) {
    static const MixedTextEntryConfig cfg = {"WiFi Password", FieldCharset::WIFI_PASSWORD, 64, 0, nullptr};
    MixedTextEntry::start(cfg, "");
  }
  MixedTextEntry::tick();
  if (MixedTextEntry::isFinished()) {
    if (MixedTextEntry::result() == MixedTextEntryResult::SAVED) {
      Settings::WifiSlot slot{true, {0}, {0}};
      strncpy(slot.ssid, g_chosenSsid, sizeof(slot.ssid) - 1);
      strncpy(slot.password, MixedTextEntry::getValue(), sizeof(slot.password) - 1);
      Settings::setWifiSlot(g_currentWifiSlotIndex, slot);
      SettingsChangeInfo info{SET_WIFI_CHANGED, g_currentWifiSlotIndex, {0}};
      fireSettingsChangeHooks(info);
    }
    Menu::goBack();  // back to scan results
    Menu::goBack();  // back to slot detail
  }
}

enum class WifiTestState : uint8_t { CONNECTING, DONE };
WifiTestState g_wifiTestState = WifiTestState::DONE;
uint32_t g_wifiTestStartMs = 0;
bool g_wifiTestSuccess = false;

void screenWifiTestConnection() {
  if (Menu::consumeJustEntered()) {
    Settings::WifiSlot slot = Settings::getWifiSlot(g_currentWifiSlotIndex);
    if (slot.configured) {
      WiFi.begin(slot.ssid, slot.password);
      g_wifiTestState = WifiTestState::CONNECTING;
      g_wifiTestStartMs = millis();
    } else {
      g_wifiTestState = WifiTestState::DONE;
      g_wifiTestSuccess = false;
    }
  }

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isMenuConfirm(e) || Input::isBack(e)) {
      Menu::goBack();
      return;
    }
  }

  if (g_wifiTestState == WifiTestState::CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      g_wifiTestState = WifiTestState::DONE;
      g_wifiTestSuccess = true;
    } else if (millis() - g_wifiTestStartMs >= 15000) {
      g_wifiTestState = WifiTestState::DONE;
      g_wifiTestSuccess = false;
    }
  }

  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, Display::kStatusBarHeight + 20);
  tft.print(g_wifiTestState == WifiTestState::CONNECTING ? "Connecting..." : (g_wifiTestSuccess ? "Connected!" : "Failed"));
  tft.setCursor(2, Display::kStatusBarHeight + 40);
  tft.print("DOT to return");
}

// ---- Family Groups ------------------------------------------------------------
uint8_t g_currentGroupIndex = 0;
char g_pendingGroupName[21];

void groupTrampoline0() { g_currentGroupIndex = 0; Menu::pushScreen(screenGroupDetail); }
void groupTrampoline1() { g_currentGroupIndex = 1; Menu::pushScreen(screenGroupDetail); }
void groupTrampoline2() { g_currentGroupIndex = 2; Menu::pushScreen(screenGroupDetail); }
void groupTrampoline3() { g_currentGroupIndex = 3; Menu::pushScreen(screenGroupDetail); }
void groupTrampoline4() { g_currentGroupIndex = 4; Menu::pushScreen(screenGroupDetail); }
ScreenHandlerFn kGroupTrampolines[Settings::kMaxGroups] = {groupTrampoline0, groupTrampoline1, groupTrampoline2,
                                                           groupTrampoline3, groupTrampoline4};
void addGroupTrampoline() { Menu::pushScreen(screenAddGroupName); }

SettingItem g_groupListItems[Settings::kMaxGroups + 1];
char g_groupListLabelBuf[Settings::kMaxGroups][21];
ListMenu g_groupListMenu;

void groupDetailRename() { Menu::pushScreen(screenGroupRenameEntry); }
void groupDetailDeleteYes() {
  Settings::FamilyGroup g = Settings::getGroup(g_currentGroupIndex);
  SettingsChangeInfo info{SET_GROUP_DELETED, g_currentGroupIndex, {0}};
  strncpy(info.group_code, g.code, sizeof(info.group_code) - 1);
  info.group_code[sizeof(info.group_code) - 1] = '\0';
  fireSettingsChangeHooks(info);  // before erasing, per Addendum 3.2
  Storage::removeGroupDirectoryIfPresent(g.code);
  Settings::deleteGroup(g_currentGroupIndex);
  Menu::goBack();  // pop the now-invalid Detail screen; confirm prompt pops itself too
}
void groupDetailDelete() {
  ConfirmPromptConfig cfg{"Delete this group?", "All history will be lost.", false, groupDetailDeleteYes, nullptr};
  Menu::startConfirmPrompt(cfg);
  Menu::pushScreen(Menu::confirmPromptScreen);
}
const SettingItem kGroupDetailItems[] = {
    {"Edit Name", groupDetailRename},
    {"Delete Group", groupDetailDelete},
};
ListMenu g_groupDetailListMenu;
void screenGroupDetail() {
  if (Menu::consumeJustEntered()) g_groupDetailListMenu.configure(kGroupDetailItems, 2);
  Display::drawStatusBar();
  g_groupDetailListMenu.tick("Group");
}

void screenGroupRenameEntry() {
  if (Menu::consumeJustEntered()) {
    static const MixedTextEntryConfig cfg = {"Group Name", FieldCharset::GENERAL_NAME, 20, 1, nullptr};
    Settings::FamilyGroup g = Settings::getGroup(g_currentGroupIndex);
    MixedTextEntry::start(cfg, g.name);
  }
  MixedTextEntry::tick();
  if (MixedTextEntry::isFinished()) {
    if (MixedTextEntry::result() == MixedTextEntryResult::SAVED) {
      Settings::renameGroup(g_currentGroupIndex, MixedTextEntry::getValue());
      Settings::FamilyGroup g = Settings::getGroup(g_currentGroupIndex);
      SettingsChangeInfo info{SET_GROUP_NAME_CHANGED, g_currentGroupIndex, {0}};
      strncpy(info.group_code, g.code, sizeof(info.group_code) - 1);
      info.group_code[sizeof(info.group_code) - 1] = '\0';
      fireSettingsChangeHooks(info);
    }
    Menu::goBack();
  }
}

bool groupCodeValidator(const char* candidate) { return Settings::isGroupCodeUnique(candidate); }

void screenAddGroupName() {
  if (Menu::consumeJustEntered()) {
    static const MixedTextEntryConfig cfg = {"Group Name", FieldCharset::GENERAL_NAME, 20, 1, nullptr};
    MixedTextEntry::start(cfg, "");
  }
  MixedTextEntry::tick();
  if (MixedTextEntry::isFinished()) {
    if (MixedTextEntry::result() == MixedTextEntryResult::SAVED) {
      strncpy(g_pendingGroupName, MixedTextEntry::getValue(), sizeof(g_pendingGroupName) - 1);
      g_pendingGroupName[sizeof(g_pendingGroupName) - 1] = '\0';
      Menu::pushScreen(screenAddGroupCode);
    } else {
      Menu::goBack();
    }
  }
}

void screenAddGroupCode() {
  if (Menu::consumeJustEntered()) {
    static const MixedTextEntryConfig cfg = {"Group Code", FieldCharset::GROUP_CODE, 32, 1, groupCodeValidator};
    MixedTextEntry::start(cfg, "");
  }
  MixedTextEntry::tick();
  if (MixedTextEntry::isFinished()) {
    if (MixedTextEntry::result() == MixedTextEntryResult::SAVED) {
      bool ok = Settings::addGroup(g_pendingGroupName, MixedTextEntry::getValue());
      if (ok) {
        uint8_t newIndex = static_cast<uint8_t>(Settings::getGroupCount() - 1);
        Settings::FamilyGroup g = Settings::getGroup(newIndex);
        SettingsChangeInfo info{SET_GROUP_ADDED, newIndex, {0}};
        strncpy(info.group_code, g.code, sizeof(info.group_code) - 1);
        info.group_code[sizeof(info.group_code) - 1] = '\0';
        fireSettingsChangeHooks(info);
      }
    }
    Menu::goBack();  // pop code screen
    Menu::goBack();  // pop name screen -> back to Family Groups list
  }
}

// ---- Connectivity root --------------------------------------------------------
const SettingItem kConnectivityItems[] = {
    {"WiFi", Settings::screenWifiSlots},
    {"Family Groups", Settings::screenFamilyGroups},
};
ListMenu g_connectivityListMenu;
void screenConnectivity() {
  if (Menu::consumeJustEntered()) g_connectivityListMenu.configure(kConnectivityItems, 2);
  Display::drawStatusBar();
  g_connectivityListMenu.tick("Connectivity");
}

// ---- Settings root ---------------------------------------------------------
const SettingItem kSettingsRootItems[] = {
    {"Connectivity", screenConnectivity},
    {"My Name", screenEditMyName},
    {"Display & Sound", screenDisplaySound},
    {"Speed & Power", screenSpeedPower},
};
ListMenu g_settingsRootListMenu;

// ---- Training Game shell ----------------------------------------------------
void trampolineSolo() {
  ScreenHandlerFn fn = getGameSubModeHandler(GameSubModes::SOLO);
  Menu::pushScreen(fn != nullptr ? fn : comingSoonScreen);
}
void trampolineFriend() {
  ScreenHandlerFn fn = getGameSubModeHandler(GameSubModes::FRIEND);
  Menu::pushScreen(fn != nullptr ? fn : comingSoonScreen);
}
void trampolineRace() {
  ScreenHandlerFn fn = getGameSubModeHandler(GameSubModes::RACE);
  Menu::pushScreen(fn != nullptr ? fn : comingSoonScreen);
}
const SettingItem kNumberGuessingItems[] = {
    {"Play Solo", trampolineSolo},
    {"Play with Friend", trampolineFriend},
    {"Race Mode", trampolineRace},
};
ListMenu g_numberGuessingListMenu;
void screenNumberGuessing() {
  if (Menu::consumeJustEntered()) g_numberGuessingListMenu.configure(kNumberGuessingItems, 3);
  Display::drawStatusBar();
  g_numberGuessingListMenu.tick("Number Guessing");
}

void levelTrampoline1() { Settings::setPracticeLevel(1); Menu::goBack(); }
void levelTrampoline2() { Settings::setPracticeLevel(2); Menu::goBack(); }
void levelTrampoline3() { Settings::setPracticeLevel(3); Menu::goBack(); }
const SettingItem kLevelItems[] = {
    {"Level 1", levelTrampoline1},
    {"Level 2", levelTrampoline2},
    {"Level 3", levelTrampoline3},
};
ListMenu g_levelListMenu;
void screenMorsePracticeLevelPicker() {
  if (Menu::consumeJustEntered()) g_levelListMenu.configure(kLevelItems, 3);
  Display::drawStatusBar();
  g_levelListMenu.tick("Select Level");
}

void morsePracticeStart() { Menu::pushScreen(comingSoonScreen); }
SettingItem g_morsePracticeItems[1 + kMaxSettingListItems];
ListMenu g_morsePracticeListMenu;
void screenMorsePractice() {
  if (Menu::consumeJustEntered()) {
    uint8_t n = 0;
    g_morsePracticeItems[n++] = SettingItem{"Start Practice", morsePracticeStart};
    uint8_t regCount = getSettingItemCount(Settings::kMorsePracticeListId);
    for (uint8_t i = 0; i < regCount && n < (1 + kMaxSettingListItems); i++) {
      const SettingItem* it = getSettingItem(Settings::kMorsePracticeListId, i);
      if (it != nullptr) g_morsePracticeItems[n++] = *it;
    }
    g_morsePracticeListMenu.configure(g_morsePracticeItems, n);
  }
  Display::drawStatusBar();
  g_morsePracticeListMenu.tick("Morse Practice");
}

const SettingItem kTrainingGameItems[] = {
    {"Morse Practice", screenMorsePractice},
    {"Number Guessing", screenNumberGuessing},
};
ListMenu g_trainingGameListMenu;

}  // namespace

namespace Settings {

void screenRoot() {
  if (Menu::consumeJustEntered()) g_settingsRootListMenu.configure(kSettingsRootItems, 4);
  Display::drawStatusBar();
  g_settingsRootListMenu.tick("Settings");
}

void screenWifiSlots() {
  if (Menu::consumeJustEntered()) {
    for (uint8_t i = 0; i < kMaxWifiSlots; i++) {
      WifiSlot s = getWifiSlot(i);
      snprintf(g_wifiSlotLabelBuf[i], sizeof(g_wifiSlotLabelBuf[i]), "Slot %u: %s", i + 1,
               s.configured ? s.ssid : "Empty");
      g_wifiSlotItems[i] = SettingItem{g_wifiSlotLabelBuf[i], kWifiSlotTrampolines[i]};
    }
    g_wifiSlotListMenu.configure(g_wifiSlotItems, kMaxWifiSlots);
  }
  Display::drawStatusBar();
  g_wifiSlotListMenu.tick("WiFi Slots");
}

void screenFamilyGroups() {
  if (Menu::consumeJustEntered()) {
    uint8_t n = getGroupCount();
    for (uint8_t i = 0; i < n; i++) {
      FamilyGroup g = getGroup(i);
      strncpy(g_groupListLabelBuf[i], g.name, sizeof(g_groupListLabelBuf[i]) - 1);
      g_groupListLabelBuf[i][sizeof(g_groupListLabelBuf[i]) - 1] = '\0';
      g_groupListItems[i] = SettingItem{g_groupListLabelBuf[i], kGroupTrampolines[i]};
    }
    uint8_t itemCount = n;
    if (n < kMaxGroups) {
      g_groupListItems[itemCount++] = SettingItem{"Add Group", addGroupTrampoline};
    }
    g_groupListMenu.configure(g_groupListItems, itemCount);
  }
  Display::drawStatusBar();
  g_groupListMenu.tick("Family Groups");
}

void screenTrainingGame() {
  if (Menu::consumeJustEntered()) g_trainingGameListMenu.configure(kTrainingGameItems, 2);
  Display::drawStatusBar();
  g_trainingGameListMenu.tick("Training Game");
}

}  // namespace Settings
