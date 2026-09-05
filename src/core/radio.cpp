#include "core/radio.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/hooks.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/presence.h"
#include "core/radio_transport.h"
#include "core/settings.h"
#include "core/storage_messages.h"

namespace {

// ---- forward declarations ---------------------------------------------------
void screenNoFamilyGroups();
void screenGroupSelect();
void screenRecipient();
void screenTalk();

// ---- shared selection state -------------------------------------------------
char g_selectedGroupCode[33];
char g_selectedContactKey[MessageStore::kContactKeyLen];

// =============================================================================
// Mute Radio Outside Radio Mode baseline availability (Phase 4 section 10:
// "Mute Off: equals ONLINE"). Mode3/Race Room/Race Guess transitions (owned
// by this file and race.cpp respectively) only matter while Mute is On.
// =============================================================================
void onSettingsChanged(const SettingsChangeInfo& info) {
  if (info.event != SET_MUTE_RADIO_CHANGED) return;
  Presence::setOwnRadioAvailable(!Settings::getMuteRadioOutsideRadio());
  Presence::republishOwnPresenceAllGroups();
}

void serviceInit() {
  // Storage::init()/Settings::init() haven't run yet at global-constructor
  // time, so this NVS-backed read must happen in an AppService.init
  // callback (same pattern as EnigmaKeys::init / Morse Practice settings).
  Presence::setOwnRadioAvailable(!Settings::getMuteRadioOutsideRadio());
  registerSettingsChangeHook(onSettingsChanged);
}

// =============================================================================
// No Family Groups / Group Select / Recipient — same shape as Text/Enigma/
// Friend's, minus "recent offline" (you can't call someone who isn't
// online) and with Everyone unconditionally included either way.
// =============================================================================
void screenNoFamilyGroups() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) Menu::goBack();
  }
  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(6, 60);
  tft.print("No Family Groups");
  tft.setCursor(6, 76);
  tft.print("Add one in Settings");
}

SettingItem g_groupSelectItems[Settings::kMaxGroups];
char g_groupSelectLabelBuf[Settings::kMaxGroups][21];
ListMenu g_groupSelectListMenu;

void groupSelectTrampoline() {
  uint8_t idx = g_groupSelectListMenu.selectedIndex();
  Settings::FamilyGroup g = Settings::getGroup(idx);
  strncpy(g_selectedGroupCode, g.code, sizeof(g_selectedGroupCode) - 1);
  g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
  Menu::goBack();
  Menu::pushScreen(screenRecipient);
}

void screenGroupSelect() {
  if (Menu::consumeJustEntered()) {
    uint8_t n = Settings::getGroupCount();
    for (uint8_t i = 0; i < n; i++) {
      Settings::FamilyGroup g = Settings::getGroup(i);
      strncpy(g_groupSelectLabelBuf[i], g.name, sizeof(g_groupSelectLabelBuf[i]) - 1);
      g_groupSelectLabelBuf[i][sizeof(g_groupSelectLabelBuf[i]) - 1] = '\0';
      g_groupSelectItems[i] = SettingItem{g_groupSelectLabelBuf[i], groupSelectTrampoline};
    }
    g_groupSelectListMenu.configure(g_groupSelectItems, n);
  }
  Display::drawStatusBar();
  g_groupSelectListMenu.tick("Select Group");
}

constexpr uint8_t kMaxRecipientItems = 21;  // Everyone + up to 20 online
SettingItem g_recipientItems[kMaxRecipientItems];
char g_recipientLabelBuf[kMaxRecipientItems][40];
char g_recipientContactKeys[kMaxRecipientItems][MessageStore::kContactKeyLen];
uint8_t g_recipientItemCount = 0;
ListMenu g_recipientListMenu;

void appendDeviceSuffix(char* label, size_t labelBufSize, const char* device_id) {
  size_t idLen = strlen(device_id);
  if (idLen < 4) return;
  char suffix[10];
  snprintf(suffix, sizeof(suffix), " [%s]", device_id + idLen - 4);
  size_t len = strlen(label);
  size_t room = (labelBufSize > len) ? (labelBufSize - len - 1) : 0;
  strncat(label, suffix, room);
}

void recipientTrampoline() {
  uint8_t idx = g_recipientListMenu.selectedIndex();
  if (idx < g_recipientItemCount) {
    strncpy(g_selectedContactKey, g_recipientContactKeys[idx], sizeof(g_selectedContactKey) - 1);
    g_selectedContactKey[sizeof(g_selectedContactKey) - 1] = '\0';
  }
  Menu::goBack();
  Menu::pushScreen(screenTalk);
}

void screenRecipient() {
  if (Menu::consumeJustEntered()) {
    g_recipientItemCount = 0;

    strncpy(g_recipientContactKeys[0], MessageStore::kEveryone, sizeof(g_recipientContactKeys[0]) - 1);
    g_recipientContactKeys[0][sizeof(g_recipientContactKeys[0]) - 1] = '\0';
    snprintf(g_recipientLabelBuf[0], sizeof(g_recipientLabelBuf[0]), "Everyone");
    g_recipientItems[0] = SettingItem{g_recipientLabelBuf[0], recipientTrampoline};
    g_recipientItemCount = 1;

    bool muteOn = Settings::getMuteRadioOutsideRadio();
    Presence::OnlineContact online[20];
    uint8_t onlineN = Presence::getOnlineContacts(g_selectedGroupCode, online, 20);
    for (uint8_t i = 0; i < onlineN && g_recipientItemCount < kMaxRecipientItems; i++) {
      if (muteOn && !online[i].radio_available) continue;  // Mute On: radio_available users only
      strncpy(g_recipientContactKeys[g_recipientItemCount], online[i].device_id,
              sizeof(g_recipientContactKeys[0]) - 1);
      g_recipientContactKeys[g_recipientItemCount][sizeof(g_recipientContactKeys[0]) - 1] = '\0';
      snprintf(g_recipientLabelBuf[g_recipientItemCount], sizeof(g_recipientLabelBuf[0]), "%s",
               online[i].display_name);
      g_recipientItems[g_recipientItemCount] =
          SettingItem{g_recipientLabelBuf[g_recipientItemCount], recipientTrampoline};
      g_recipientItemCount++;
    }

    for (uint8_t i = 1; i < g_recipientItemCount; i++) {
      for (uint8_t j = static_cast<uint8_t>(i + 1); j < g_recipientItemCount; j++) {
        if (strcmp(g_recipientLabelBuf[i], g_recipientLabelBuf[j]) == 0) {
          appendDeviceSuffix(g_recipientLabelBuf[i], sizeof(g_recipientLabelBuf[i]), g_recipientContactKeys[i]);
          appendDeviceSuffix(g_recipientLabelBuf[j], sizeof(g_recipientLabelBuf[j]), g_recipientContactKeys[j]);
        }
      }
    }

    g_recipientListMenu.configure(g_recipientItems, g_recipientItemCount);
  }
  Display::drawStatusBar();
  g_recipientListMenu.tick("Radio");
}

// =============================================================================
// Talk screen (Phase 4 section 2-3, 9-10): raw DOT press/release is PTT, not
// Morse-classified; Encoder navigation stays separate (only Encoder long
// exits, matching the rest of the app's back convention).
// =============================================================================
void leaveTalk() {
  RadioTransport::stopPrivateCall();
  RadioTransport::stopBroadcastCall();
  RadioTransport::setUiContext(RadioTransport::UiContext::NONE);
  if (Settings::getMuteRadioOutsideRadio()) {
    Presence::setOwnRadioAvailable(false);
    Presence::republishOwnPresenceAllGroups();
  }
}

void screenTalk() {
  bool isEveryone = strcmp(g_selectedContactKey, MessageStore::kEveryone) == 0;

  if (Menu::consumeJustEntered()) {
    RadioTransport::setUiContext(RadioTransport::UiContext::RADIO_TALK);
    if (Settings::getMuteRadioOutsideRadio()) {
      Presence::setOwnRadioAvailable(true);
      Presence::republishOwnPresenceAllGroups();
    }
  }

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) {
      leaveTalk();
      Menu::goBack();
      continue;
    }
    if (e.type == InputEventType::DOT_PRESS_START) {
      if (isEveryone) {
        RadioTransport::startBroadcastCall(g_selectedGroupCode);
      } else {
        RadioTransport::startPrivateCall(g_selectedGroupCode, g_selectedContactKey);
      }
    } else if (e.type == InputEventType::DOT_RELEASE) {
      if (isEveryone) {
        RadioTransport::stopBroadcastCall();
      } else {
        RadioTransport::stopPrivateCall();
      }
    }
    // Encoder rotate/short: no defined action here (Encoder navigation
    // remains separate from PTT, per Phase 4 section 2).
  }

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print(isEveryone ? "Everyone" : g_selectedContactKey);

  const char* status = "Ready";
  if (isEveryone) {
    if (RadioTransport::isBroadcasting()) status = "Talking...";
  } else if (RadioTransport::isPrivateDenied()) {
    status = "BUSY (no mic)";
  } else {
    switch (RadioTransport::getPrivateState()) {
      case RadioTransport::PrivateState::CLAIMING:
        status = "Claiming...";
        break;
      case RadioTransport::PrivateState::NEGOTIATING:
        status = "Connecting...";
        break;
      case RadioTransport::PrivateState::UDP_DIRECT:
        status = "Talking (direct)";
        break;
      case RadioTransport::PrivateState::MQTT_FALLBACK:
        status = "Talking (relay)";
        break;
      default:
        status = "Ready";
        break;
    }
  }
  tft.setCursor(2, Display::kStatusBarHeight + 20);
  tft.print(status);
}

// =============================================================================
// Mode entry point. Consumes a pending Radio navigation intent from Phase 3
// (Number Guessing Friend's quick-switch) before falling back to the normal
// 0/1/>1-group routing.
// =============================================================================
void screenRadioEntry() {
  const PendingRadioIntent& intent = getPendingRadioIntent();
  if (intent.valid) {
    strncpy(g_selectedGroupCode, intent.group_code, sizeof(g_selectedGroupCode) - 1);
    g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
    strncpy(g_selectedContactKey, intent.contact_key, sizeof(g_selectedContactKey) - 1);
    g_selectedContactKey[sizeof(g_selectedContactKey) - 1] = '\0';
    clearPendingRadioIntent();
    Menu::goBack();
    Menu::pushScreen(screenTalk);
    return;
  }

  uint8_t n = Settings::getGroupCount();
  ScreenHandlerFn target;
  if (n == 0) {
    target = screenNoFamilyGroups;
  } else if (n == 1) {
    Settings::FamilyGroup g = Settings::getGroup(0);
    strncpy(g_selectedGroupCode, g.code, sizeof(g_selectedGroupCode) - 1);
    g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
    target = screenRecipient;
  } else {
    target = screenGroupSelect;
  }
  Menu::goBack();
  Menu::pushScreen(target);
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = nullptr;
    registerAppService(svc);
    registerModeHandler(Modes::RADIO, screenRadioEntry);
  }
};
Registrar g_registrar;

}  // namespace
