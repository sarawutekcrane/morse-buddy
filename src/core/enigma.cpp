#include "core/enigma.h"

#include <Arduino.h>
#include <string.h>

#include "core/display.h"
#include "core/enigma_crypto.h"
#include "core/enigma_keys.h"
#include "core/hooks.h"
#include "core/identity.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/modes.h"
#include "core/morse.h"
#include "core/mqtt_manager.h"
#include "core/notifications.h"
#include "core/packet_codec.h"
#include "core/presence.h"
#include "core/settings.h"
#include "core/storage_messages.h"
#include "core/text_message.h"
#include "core/wifi_manager.h"

namespace {

using EnigmaCrypto::EnigmaKeyConfig;

// ---- forward declarations --------------------------------------------------
void screenNoFamilyGroups();
void screenGroupSelect();
void screenRecipient();
void screenContactMenu();
void screenEnigmaChat();
void screenKeyEditor();
void screenReveal();

// ---- shared selection state -------------------------------------------------
char g_selectedGroupCode[33];
char g_selectedContactKey[MessageStore::kContactKeyLen];

// ---- local payload: lock state + cached decode (Addendum section 6) -------
constexpr uint8_t LOCK_BLACK = 0;
constexpr uint8_t LOCK_WHITE = 1;
constexpr uint8_t LOCK_UNLOCKED = 2;

size_t encodeLocalPayload(uint8_t lockState, const char* cachedText, uint8_t* out, size_t outSize) {
  size_t textLen = strlen(cachedText);
  if (textLen > 250) textLen = 250;
  size_t total = 3 + textLen;
  if (outSize < total) return 0;
  out[0] = lockState;
  out[1] = static_cast<uint8_t>(textLen & 0xFF);
  out[2] = static_cast<uint8_t>((textLen >> 8) & 0xFF);
  if (textLen > 0) memcpy(out + 3, cachedText, textLen);
  return total;
}

void decodeLocalPayload(const uint8_t* data, uint16_t len, uint8_t* outLock, char* outText, size_t outTextCap) {
  if (data == nullptr || len < 3) {
    *outLock = LOCK_BLACK;
    outText[0] = '\0';
    return;
  }
  *outLock = data[0];
  uint16_t textLen = static_cast<uint16_t>(data[1] | (data[2] << 8));
  if (static_cast<size_t>(3) + textLen > len) textLen = 0;
  if (textLen + 1 > outTextCap) textLen = static_cast<uint16_t>(outTextCap - 1);
  if (textLen > 0) memcpy(outText, data + 3, textLen);
  outText[textLen] = '\0';
}

// =============================================================================
// No Family Groups
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

// =============================================================================
// Group Select
// =============================================================================
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

// =============================================================================
// Recipient (Online / Recent Offline / Everyone — Everyone is allowed here)
// =============================================================================
constexpr uint8_t kMaxRecipientItems = 41;
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
  Menu::pushScreen(screenContactMenu);
}

void screenRecipient() {
  if (Menu::consumeJustEntered()) {
    g_recipientItemCount = 0;

    Presence::OnlineContact online[20];
    uint8_t onlineN = Presence::getOnlineContacts(g_selectedGroupCode, online, 20);
    for (uint8_t i = 0; i < onlineN && g_recipientItemCount < kMaxRecipientItems; i++) {
      strncpy(g_recipientContactKeys[g_recipientItemCount], online[i].device_id,
              sizeof(g_recipientContactKeys[0]) - 1);
      g_recipientContactKeys[g_recipientItemCount][sizeof(g_recipientContactKeys[0]) - 1] = '\0';
      snprintf(g_recipientLabelBuf[g_recipientItemCount], sizeof(g_recipientLabelBuf[0]), "%s",
               online[i].display_name);
      g_recipientItems[g_recipientItemCount] = SettingItem{g_recipientLabelBuf[g_recipientItemCount],
                                                            recipientTrampoline};
      g_recipientItemCount++;
    }

    Presence::RecentContact recent[20];
    uint8_t recentN = Presence::getRecentContacts(g_selectedGroupCode, recent, 20);
    for (uint8_t i = 0; i < recentN && g_recipientItemCount < kMaxRecipientItems; i++) {
      bool alreadyOnline = false;
      for (uint8_t j = 0; j < onlineN; j++) {
        if (strcmp(online[j].device_id, recent[i].device_id) == 0) {
          alreadyOnline = true;
          break;
        }
      }
      if (alreadyOnline) continue;
      strncpy(g_recipientContactKeys[g_recipientItemCount], recent[i].device_id,
              sizeof(g_recipientContactKeys[0]) - 1);
      g_recipientContactKeys[g_recipientItemCount][sizeof(g_recipientContactKeys[0]) - 1] = '\0';
      const char* name = (recent[i].last_known_name[0] != '\0') ? recent[i].last_known_name : recent[i].device_id;
      snprintf(g_recipientLabelBuf[g_recipientItemCount], sizeof(g_recipientLabelBuf[0]), "%s (offline)", name);
      g_recipientItems[g_recipientItemCount] = SettingItem{g_recipientLabelBuf[g_recipientItemCount],
                                                            recipientTrampoline};
      g_recipientItemCount++;
    }

    for (uint8_t i = 0; i < g_recipientItemCount; i++) {
      for (uint8_t j = static_cast<uint8_t>(i + 1); j < g_recipientItemCount; j++) {
        if (strcmp(g_recipientLabelBuf[i], g_recipientLabelBuf[j]) == 0) {
          appendDeviceSuffix(g_recipientLabelBuf[i], sizeof(g_recipientLabelBuf[i]), g_recipientContactKeys[i]);
          appendDeviceSuffix(g_recipientLabelBuf[j], sizeof(g_recipientLabelBuf[j]), g_recipientContactKeys[j]);
        }
      }
    }

    if (g_recipientItemCount < kMaxRecipientItems) {
      strncpy(g_recipientContactKeys[g_recipientItemCount], MessageStore::kEveryone,
              sizeof(g_recipientContactKeys[0]) - 1);
      g_recipientContactKeys[g_recipientItemCount][sizeof(g_recipientContactKeys[0]) - 1] = '\0';
      snprintf(g_recipientLabelBuf[g_recipientItemCount], sizeof(g_recipientLabelBuf[0]), "Everyone");
      g_recipientItems[g_recipientItemCount] = SettingItem{g_recipientLabelBuf[g_recipientItemCount],
                                                            recipientTrampoline};
      g_recipientItemCount++;
    }

    g_recipientListMenu.configure(g_recipientItems, g_recipientItemCount);
  }
  Display::drawStatusBar();
  g_recipientListMenu.tick("Recipient");
}

// =============================================================================
// Contact Menu: Chat / Sender Key Settings
// =============================================================================
void startSenderKeyEditor();

void contactMenuChatTrampoline() {
  Menu::goBack();
  Menu::pushScreen(screenEnigmaChat);
}
void contactMenuSenderKeyTrampoline() {
  startSenderKeyEditor();
  Menu::goBack();
  Menu::pushScreen(screenKeyEditor);
}

const SettingItem kContactMenuItems[] = {
    {"Chat", contactMenuChatTrampoline},
    {"Sender Key Settings", contactMenuSenderKeyTrampoline},
};
ListMenu g_contactMenuListMenu;

void screenContactMenu() {
  if (Menu::consumeJustEntered()) g_contactMenuListMenu.configure(kContactMenuItems, 2);
  Display::drawStatusBar();
  g_contactMenuListMenu.tick("Enigma Contact");
}

// =============================================================================
// Key Editor (shared by Sender Key Settings and Receive Key Settings)
// =============================================================================
enum class KeyEditStep : uint8_t {
  ROTOR_COUNT,
  PLUGBOARD_COUNT,
  METHOD_CHOICE,
  ROTOR_TYPE,
  ROTOR_POSITION,
  PLUG_LETTER1,
  PLUG_LETTER2,
  CONFIRM,
};

KeyEditStep g_editStep;
EnigmaKeyConfig g_working;
uint8_t g_editRotorSlot = 0;
uint8_t g_editPairSlot = 0;
char g_editFirstLetter = 'A';
int16_t g_pickerIndex = 0;
bool g_editingSender = true;
bool g_hasTriggeringRef = false;
MessageRef g_triggeringRef;
bool g_confirmSaveSelected = true;

void attemptReceiveKey(const MessageRef& triggeringRef, const EnigmaKeyConfig& attemptedKey);

int16_t wrapClamp(int16_t v, int16_t lo, int16_t hi) {
  if (v < lo) return hi;
  if (v > hi) return lo;
  return v;
}

int16_t firstAvailableRotorType() {
  for (uint8_t t = 0; t < EnigmaCrypto::kRotorTypeCount; t++) {
    bool used = false;
    for (uint8_t i = 0; i < g_editRotorSlot; i++) {
      if (g_working.rotor_type[i] == t) used = true;
    }
    if (!used) return t;
  }
  return 0;
}

int16_t nextValidRotorType(int16_t current, int8_t direction) {
  for (uint8_t tries = 0; tries < EnigmaCrypto::kRotorTypeCount; tries++) {
    current = static_cast<int16_t>((current + direction + EnigmaCrypto::kRotorTypeCount) %
                                   EnigmaCrypto::kRotorTypeCount);
    bool used = false;
    for (uint8_t i = 0; i < g_editRotorSlot; i++) {
      if (g_working.rotor_type[i] == current) used = true;
    }
    if (!used) return current;
  }
  return current;
}

int16_t firstAvailableLetter(int16_t extraExclude) {
  for (int16_t c = 0; c < 26; c++) {
    if (c == extraExclude) continue;
    bool used = false;
    for (uint8_t i = 0; i < g_editPairSlot; i++) {
      if (g_working.plugboard_pairs[i][0] - 'A' == c || g_working.plugboard_pairs[i][1] - 'A' == c) used = true;
    }
    if (!used) return c;
  }
  return 0;
}

int16_t nextValidLetter(int16_t current, int8_t direction, int16_t extraExclude) {
  for (uint8_t tries = 0; tries < 26; tries++) {
    current = static_cast<int16_t>((current + direction + 26) % 26);
    if (current == extraExclude) continue;
    bool used = false;
    for (uint8_t i = 0; i < g_editPairSlot; i++) {
      if (g_working.plugboard_pairs[i][0] - 'A' == current || g_working.plugboard_pairs[i][1] - 'A' == current) {
        used = true;
      }
    }
    if (!used) return current;
  }
  return current;
}

void enterStep(KeyEditStep step) {
  g_editStep = step;
  switch (step) {
    case KeyEditStep::ROTOR_COUNT:
      g_pickerIndex = g_working.rotor_count;
      break;
    case KeyEditStep::PLUGBOARD_COUNT:
      g_pickerIndex = g_working.plugboard_pair_count;
      break;
    case KeyEditStep::METHOD_CHOICE:
      g_pickerIndex = 0;
      break;
    case KeyEditStep::ROTOR_TYPE:
      g_pickerIndex = firstAvailableRotorType();
      break;
    case KeyEditStep::ROTOR_POSITION:
      g_pickerIndex = g_working.rotor_position[g_editRotorSlot];
      break;
    case KeyEditStep::PLUG_LETTER1:
      g_pickerIndex = firstAvailableLetter(-1);
      break;
    case KeyEditStep::PLUG_LETTER2:
      g_pickerIndex = firstAvailableLetter(g_editFirstLetter - 'A');
      break;
    case KeyEditStep::CONFIRM:
      g_confirmSaveSelected = true;
      break;
  }
}

void randomizeWorkingKey() {
  for (uint8_t i = 0; i < g_working.rotor_count; i++) {
    int16_t t;
    bool used;
    do {
      t = static_cast<int16_t>(random(EnigmaCrypto::kRotorTypeCount));
      used = false;
      for (uint8_t j = 0; j < i; j++) {
        if (g_working.rotor_type[j] == t) used = true;
      }
    } while (used);
    g_working.rotor_type[i] = static_cast<uint8_t>(t);
    g_working.rotor_position[i] = static_cast<uint8_t>(random(26));
  }
  bool usedLetter[26] = {false};
  for (uint8_t i = 0; i < g_working.plugboard_pair_count; i++) {
    int16_t a, b;
    do {
      a = static_cast<int16_t>(random(26));
    } while (usedLetter[a]);
    usedLetter[a] = true;
    do {
      b = static_cast<int16_t>(random(26));
    } while (usedLetter[b]);
    usedLetter[b] = true;
    g_working.plugboard_pairs[i][0] = static_cast<char>('A' + a);
    g_working.plugboard_pairs[i][1] = static_cast<char>('A' + b);
  }
}

void finishKeyEditor(bool save) {
  if (save) {
    if (g_editingSender) {
      EnigmaKeys::setSenderKey(g_selectedGroupCode, g_selectedContactKey, g_working);
    } else if (g_hasTriggeringRef) {
      attemptReceiveKey(g_triggeringRef, g_working);
    } else {
      EnigmaKeys::setReceiveKey(g_selectedGroupCode, g_selectedContactKey, g_working);
    }
  }
  Menu::goBack();
}

void onConfirmCurrentStep() {
  switch (g_editStep) {
    case KeyEditStep::ROTOR_COUNT:
      g_working.rotor_count = static_cast<uint8_t>(g_pickerIndex);
      enterStep(KeyEditStep::PLUGBOARD_COUNT);
      break;
    case KeyEditStep::PLUGBOARD_COUNT:
      g_working.plugboard_pair_count = static_cast<uint8_t>(g_pickerIndex);
      enterStep(KeyEditStep::METHOD_CHOICE);
      break;
    case KeyEditStep::METHOD_CHOICE:
      if (g_pickerIndex == 0) {
        g_editRotorSlot = 0;
        if (g_working.rotor_count > 0) {
          enterStep(KeyEditStep::ROTOR_TYPE);
        } else {
          g_editPairSlot = 0;
          enterStep(g_working.plugboard_pair_count > 0 ? KeyEditStep::PLUG_LETTER1 : KeyEditStep::CONFIRM);
        }
      } else {
        randomizeWorkingKey();
        enterStep(KeyEditStep::CONFIRM);
      }
      break;
    case KeyEditStep::ROTOR_TYPE:
      g_working.rotor_type[g_editRotorSlot] = static_cast<uint8_t>(g_pickerIndex);
      enterStep(KeyEditStep::ROTOR_POSITION);
      break;
    case KeyEditStep::ROTOR_POSITION:
      g_working.rotor_position[g_editRotorSlot] = static_cast<uint8_t>(g_pickerIndex);
      g_editRotorSlot++;
      if (g_editRotorSlot < g_working.rotor_count) {
        enterStep(KeyEditStep::ROTOR_TYPE);
      } else {
        g_editPairSlot = 0;
        enterStep(g_working.plugboard_pair_count > 0 ? KeyEditStep::PLUG_LETTER1 : KeyEditStep::CONFIRM);
      }
      break;
    case KeyEditStep::PLUG_LETTER1:
      g_editFirstLetter = static_cast<char>('A' + g_pickerIndex);
      enterStep(KeyEditStep::PLUG_LETTER2);
      break;
    case KeyEditStep::PLUG_LETTER2:
      g_working.plugboard_pairs[g_editPairSlot][0] = g_editFirstLetter;
      g_working.plugboard_pairs[g_editPairSlot][1] = static_cast<char>('A' + g_pickerIndex);
      g_editPairSlot++;
      if (g_editPairSlot < g_working.plugboard_pair_count) {
        enterStep(KeyEditStep::PLUG_LETTER1);
      } else {
        enterStep(KeyEditStep::CONFIRM);
      }
      break;
    case KeyEditStep::CONFIRM:
      finishKeyEditor(g_confirmSaveSelected);
      break;
  }
}

void handleEditorBack() {
  if (g_editStep == KeyEditStep::CONFIRM) {
    // "Encoder long returns to editing" — resume at the last editing step
    // rather than perfectly restoring mid-entry state.
    if (g_working.plugboard_pair_count > 0) {
      g_editPairSlot = static_cast<uint8_t>(g_working.plugboard_pair_count - 1);
      enterStep(KeyEditStep::PLUG_LETTER1);
    } else if (g_working.rotor_count > 0) {
      g_editRotorSlot = static_cast<uint8_t>(g_working.rotor_count - 1);
      enterStep(KeyEditStep::ROTOR_TYPE);
    } else {
      enterStep(KeyEditStep::METHOD_CHOICE);
    }
  } else {
    Menu::goBack();  // discards the working copy (Addendum 10.11)
  }
}

void startSenderKeyEditor() {
  g_editingSender = true;
  g_hasTriggeringRef = false;
  g_working = EnigmaKeys::getSenderKey(g_selectedGroupCode, g_selectedContactKey);
  enterStep(KeyEditStep::ROTOR_COUNT);
}

void startReceiveKeyEditorFor(const MessageRef& ref) {
  g_editingSender = false;
  g_hasTriggeringRef = true;
  g_triggeringRef = ref;
  g_working = EnigmaKeys::getReceiveKey(g_selectedGroupCode, g_selectedContactKey);
  enterStep(KeyEditStep::ROTOR_COUNT);
}

void screenKeyEditor() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      switch (g_editStep) {
        case KeyEditStep::ROTOR_COUNT:
          g_pickerIndex = wrapClamp(static_cast<int16_t>(g_pickerIndex + e.value), 0, EnigmaCrypto::kMaxRotors);
          break;
        case KeyEditStep::PLUGBOARD_COUNT:
          g_pickerIndex =
              wrapClamp(static_cast<int16_t>(g_pickerIndex + e.value), 0, EnigmaCrypto::kMaxPlugboardPairs);
          break;
        case KeyEditStep::METHOD_CHOICE:
          g_pickerIndex = static_cast<int16_t>(1 - g_pickerIndex);
          break;
        case KeyEditStep::ROTOR_TYPE:
          g_pickerIndex = nextValidRotorType(g_pickerIndex, static_cast<int8_t>(e.value));
          break;
        case KeyEditStep::ROTOR_POSITION:
          g_pickerIndex = static_cast<int16_t>((g_pickerIndex + e.value + 26) % 26);
          break;
        case KeyEditStep::PLUG_LETTER1:
          g_pickerIndex = nextValidLetter(g_pickerIndex, static_cast<int8_t>(e.value), -1);
          break;
        case KeyEditStep::PLUG_LETTER2:
          g_pickerIndex = nextValidLetter(g_pickerIndex, static_cast<int8_t>(e.value), g_editFirstLetter - 'A');
          break;
        case KeyEditStep::CONFIRM:
          g_confirmSaveSelected = !g_confirmSaveSelected;
          break;
      }
    } else if (Input::isMenuConfirm(e)) {
      onConfirmCurrentStep();
    } else if (Input::isBack(e)) {
      handleEditorBack();
    }
  }

  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print(g_editingSender ? "Sender Key" : "Receive Key");

  char line[40];
  switch (g_editStep) {
    case KeyEditStep::ROTOR_COUNT:
      snprintf(line, sizeof(line), "Rotor Count: %d", g_pickerIndex);
      break;
    case KeyEditStep::PLUGBOARD_COUNT:
      snprintf(line, sizeof(line), "Plugboard Pairs: %d", g_pickerIndex);
      break;
    case KeyEditStep::METHOD_CHOICE:
      snprintf(line, sizeof(line), "%s", g_pickerIndex == 0 ? "> Manual Setup" : "> Randomize All");
      break;
    case KeyEditStep::ROTOR_TYPE: {
      static const char* const kRotorNames[] = {"I", "II", "III", "IV", "V"};
      snprintf(line, sizeof(line), "Rotor %d Type: %s", g_editRotorSlot + 1, kRotorNames[g_pickerIndex]);
      break;
    }
    case KeyEditStep::ROTOR_POSITION:
      snprintf(line, sizeof(line), "Rotor %d Pos: %c", g_editRotorSlot + 1, static_cast<char>('A' + g_pickerIndex));
      break;
    case KeyEditStep::PLUG_LETTER1:
      snprintf(line, sizeof(line), "Pair %d Letter 1: %c", g_editPairSlot + 1, static_cast<char>('A' + g_pickerIndex));
      break;
    case KeyEditStep::PLUG_LETTER2:
      snprintf(line, sizeof(line), "Pair %d Letter 2: %c%c", g_editPairSlot + 1, g_editFirstLetter,
               static_cast<char>('A' + g_pickerIndex));
      break;
    case KeyEditStep::CONFIRM:
      snprintf(line, sizeof(line), "%s", g_confirmSaveSelected ? "> Save    Cancel" : "  Save  > Cancel");
      break;
  }
  tft.setCursor(2, Display::kStatusBarHeight + 20);
  tft.print(line);

  if (g_editStep == KeyEditStep::CONFIRM && g_working.rotor_count == 0 && g_working.plugboard_pair_count == 0) {
    tft.setCursor(2, Display::kStatusBarHeight + 40);
    tft.print("Warning: plaintext passthrough");
  }
}

// =============================================================================
// Reveal Correct Key (EVT_COMBINED_REVEAL on BLACK/WHITE)
// =============================================================================
MessageRef g_revealRef;
EnigmaKeyConfig g_revealedKey;

void startRevealFlow(const MessageRef& ref, const StoredMessageView& view) {
  g_revealRef = ref;
  char ciphertext[EnigmaCrypto::kMaxEscapedLen + 1];
  uint32_t fp, gen;
  EnigmaKeys::decodeEnigmaPayload(view.typePayload, view.typePayloadLen, ciphertext, sizeof(ciphertext), &fp, &gen,
                                  &g_revealedKey);
}

void screenReveal() {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isMenuConfirm(e)) {
      attemptReceiveKey(g_revealRef, g_revealedKey);
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
  tft.setCursor(2, Display::kStatusBarHeight + 2);
  tft.print("Revealed Key");

  char line[48];
  snprintf(line, sizeof(line), "Rotors: %d", g_revealedKey.rotor_count);
  tft.setCursor(2, Display::kStatusBarHeight + 16);
  tft.print(line);

  int16_t y = Display::kStatusBarHeight + 28;
  for (uint8_t i = 0; i < g_revealedKey.rotor_count; i++) {
    static const char* const kNames[] = {"I", "II", "III", "IV", "V"};
    snprintf(line, sizeof(line), "  %s @ %c", kNames[g_revealedKey.rotor_type[i]],
             static_cast<char>('A' + g_revealedKey.rotor_position[i]));
    tft.setCursor(2, y);
    tft.print(line);
    y += 10;
  }
  snprintf(line, sizeof(line), "Plugs: %d", g_revealedKey.plugboard_pair_count);
  tft.setCursor(2, y);
  tft.print(line);
  y += 10;
  tft.setCursor(2, y);
  tft.print("DOT: use as Receive Key");
}

// =============================================================================
// Key attempt evaluation (Addendum section 6)
// =============================================================================
struct SenderTypeCtx {
  const char* senderDeviceId;
};

bool senderEnigmaPredicate(const MessageStore::StoredHeader& hdr, const PacketCodec::MessageEnvelope& env,
                           void* ctxRaw) {
  (void)hdr;
  auto* ctx = static_cast<SenderTypeCtx*>(ctxRaw);
  return env.message_type == PacketCodec::MSG_TYPE_ENIGMA && strcmp(env.sender_device_id, ctx->senderDeviceId) == 0;
}

void attemptReceiveKey(const MessageRef& triggeringRef, const EnigmaKeyConfig& attemptedKey) {
  StoredMessageView triggerView;
  if (!MessageStore::loadMessage(triggeringRef, &triggerView)) return;

  char triggerCiphertext[EnigmaCrypto::kMaxEscapedLen + 1];
  uint32_t targetFp = 0;
  uint32_t targetGen = 0;
  EnigmaKeyConfig embeddedKey;
  if (!EnigmaKeys::decodeEnigmaPayload(triggerView.typePayload, triggerView.typePayloadLen, triggerCiphertext,
                                       sizeof(triggerCiphertext), &targetFp, &targetGen, &embeddedKey)) {
    return;
  }

  bool correct = EnigmaCrypto::keysEqual(attemptedKey, embeddedKey);
  EnigmaKeys::setReceiveKey(g_selectedGroupCode, g_selectedContactKey, attemptedKey);

  SenderTypeCtx ctx{triggerView.envelope.sender_device_id};
  constexpr uint16_t kMaxMatches = 32;
  MessageRef matches[kMaxMatches];
  uint16_t n = MessageStore::findMessagesByPredicate(g_selectedGroupCode, g_selectedContactKey, senderEnigmaPredicate,
                                                     &ctx, matches, kMaxMatches);
  uint16_t limit = (n < kMaxMatches) ? n : kMaxMatches;

  for (uint16_t i = 0; i < limit; i++) {
    StoredMessageView mv;
    if (!MessageStore::loadMessage(matches[i], &mv)) continue;

    char mCiphertext[EnigmaCrypto::kMaxEscapedLen + 1];
    uint32_t mFp = 0;
    uint32_t mGen = 0;
    EnigmaKeyConfig mEmbedded;
    if (!EnigmaKeys::decodeEnigmaPayload(mv.typePayload, mv.typePayloadLen, mCiphertext, sizeof(mCiphertext), &mFp,
                                         &mGen, &mEmbedded)) {
      continue;
    }
    if (mFp != targetFp || mGen != targetGen) continue;

    char decrypted[EnigmaCrypto::kMaxEscapedLen + 1];
    EnigmaCrypto::run(mCiphertext, decrypted, sizeof(decrypted), correct ? mEmbedded : attemptedKey);
    char plaintext[EnigmaCrypto::kMaxEscapedLen + 1];
    EnigmaCrypto::reverseEscape(decrypted, plaintext, sizeof(plaintext));

    uint8_t newLock = correct ? LOCK_UNLOCKED : LOCK_WHITE;
    uint8_t localBuf[280];
    size_t localLen = encodeLocalPayload(newLock, plaintext, localBuf, sizeof(localBuf));
    if (localLen > 0) MessageStore::updateTypeLocalPayload(matches[i], localBuf, localLen);
  }
}

// =============================================================================
// Chat: history viewport + compose line (Enigma-specific send/lock UI)
// =============================================================================
constexpr uint16_t kNoHistoryCursor = 0xFFFF;
uint16_t g_historyCursor = kNoHistoryCursor;

char g_composeText[EnigmaCrypto::kMaxEscapedLen + 1];
uint8_t g_composeLen = 0;
char g_composePattern[Morse::kMaxPatternLength + 1];
uint8_t g_composePatternLen = 0;
uint32_t g_lastMorseReleaseMs = 0;
const char* g_composeError = nullptr;

bool g_indexDirty = true;
uint16_t g_indexTotal = 0;

void markIndexDirty() { g_indexDirty = true; }
void refreshIndexIfNeeded() {
  if (!g_indexDirty) return;
  g_indexTotal = MessageStore::loadConversationIndex(g_selectedGroupCode, g_selectedContactKey);
  g_indexDirty = false;
}

void resetComposePattern() {
  g_composePatternLen = 0;
  g_composePattern[0] = '\0';
}
void clearDraft() {
  g_composeLen = 0;
  g_composeText[0] = '\0';
  resetComposePattern();
  g_composeError = nullptr;
}

MessageRef refForIndexEntry(const MessageStore::ConversationIndexEntry& entry) {
  MessageRef ref;
  strncpy(ref.group_code, g_selectedGroupCode, sizeof(ref.group_code) - 1);
  ref.group_code[sizeof(ref.group_code) - 1] = '\0';
  strncpy(ref.contact_key, g_selectedContactKey, sizeof(ref.contact_key) - 1);
  ref.contact_key[sizeof(ref.contact_key) - 1] = '\0';
  ref.sequence = entry.sequence;
  return ref;
}

void sendEnigmaMessage() {
  char escaped[EnigmaCrypto::kMaxEscapedLen + 1];
  if (!EnigmaCrypto::normalizeAndEscape(g_composeText, escaped, sizeof(escaped))) {
    g_composeError = "Message too long after Enigma encoding";
    return;
  }
  if (strlen(escaped) == 0) {
    g_composeError = "No A-Z content";
    return;
  }

  EnigmaKeyConfig senderKey = EnigmaKeys::getSenderKey(g_selectedGroupCode, g_selectedContactKey);
  char ciphertext[EnigmaCrypto::kMaxEscapedLen + 1];
  EnigmaCrypto::run(escaped, ciphertext, sizeof(ciphertext), senderKey);
  uint32_t fingerprint = EnigmaCrypto::computeFingerprint(senderKey);
  uint32_t generation = EnigmaKeys::getSenderKeyGeneration(g_selectedGroupCode, g_selectedContactKey);

  uint8_t typePayload[320];
  size_t typePayloadLen =
      EnigmaKeys::encodeEnigmaPayload(ciphertext, fingerprint, generation, senderKey, typePayload,
                                      sizeof(typePayload));
  if (typePayloadLen == 0) {
    g_composeError = "Message too long after Enigma encoding";
    return;
  }

  char messageId[PacketCodec::kMessageIdLen];
  Identity::nextId(messageId, sizeof(messageId));
  PacketCodec::MessageEnvelope env;
  memset(&env, 0, sizeof(env));
  strncpy(env.message_id, messageId, sizeof(env.message_id) - 1);
  env.schema_version = 1;
  env.message_type = PacketCodec::MSG_TYPE_ENIGMA;
  strncpy(env.sender_device_id, Identity::deviceId(), sizeof(env.sender_device_id) - 1);
  strncpy(env.sender_name_cache, Settings::getMyName(), sizeof(env.sender_name_cache) - 1);
  strncpy(env.group_code, g_selectedGroupCode, sizeof(env.group_code) - 1);
  env.timestamp = WifiManager::getUnixTime();

  static uint8_t wireBuf[PacketCodec::kHeaderSize + 340];
  size_t wireLen =
      PacketCodec::encodeMessagePacket(env, typePayload, static_cast<uint16_t>(typePayloadLen), wireBuf,
                                       sizeof(wireBuf));
  if (wireLen == 0) return;

  bool isEveryone = strcmp(g_selectedContactKey, MessageStore::kEveryone) == 0;
  char topicSuffix[24];
  if (isEveryone) {
    snprintf(topicSuffix, sizeof(topicSuffix), "broadcast");
  } else {
    snprintf(topicSuffix, sizeof(topicSuffix), "msg/%s", g_selectedContactKey);
  }

  bool online = MqttManager::isGroupConnected(g_selectedGroupCode);
  bool published = false;
  if (online) {
    published = MqttManager::publishBinary(g_selectedGroupCode, topicSuffix, wireBuf,
                                           static_cast<uint16_t>(wireLen), false, 1);
  }
  uint16_t flags = published ? 0 : MessageStore::FLAG_PENDING_OUTBOX;

  char normalizedPlaintext[EnigmaCrypto::kMaxEscapedLen + 1];
  EnigmaCrypto::reverseEscape(escaped, normalizedPlaintext, sizeof(normalizedPlaintext));
  uint8_t localPayload[280];
  size_t localLen = encodeLocalPayload(LOCK_UNLOCKED, normalizedPlaintext, localPayload, sizeof(localPayload));

  MessageRef outRef;
  MessageStore::appendStoredMessage(g_selectedGroupCode, g_selectedContactKey, MessageStore::Direction::SENT, flags,
                                    env.timestamp, wireBuf, static_cast<uint16_t>(wireLen), localPayload, localLen,
                                    &outRef);

  clearDraft();
  markIndexDirty();
}

void finalizeComposeChar() {
  char c = Morse::decodePattern(g_composePattern);
  if (g_composeLen < EnigmaCrypto::kMaxEscapedLen) {
    g_composeText[g_composeLen++] = c;
    g_composeText[g_composeLen] = '\0';
  }
  resetComposePattern();
}

void buildCanonicalRawMorse(const char* text, char* out, size_t outSize) {
  size_t pos = 0;
  out[0] = '\0';
  size_t len = strlen(text);
  for (size_t i = 0; i < len && pos + 1 < outSize; i++) {
    char c = text[i];
    if (c == ' ') {
      out[pos++] = '/';
    } else {
      char pat[Morse::kMaxPatternLength + 1];
      if (Morse::encodeChar(c, pat, sizeof(pat))) {
        size_t patLen = strlen(pat);
        if (pos + patLen < outSize) {
          memcpy(out + pos, pat, patLen);
          pos += patLen;
        }
      }
    }
    if (pos + 1 < outSize) out[pos++] = ' ';
  }
  out[pos] = '\0';
}

void buildComposeDisplay(char* out, size_t outSize) {
  Settings::TypingDisplay mode = Settings::getTypingDisplay();
  if (mode == Settings::TypingDisplay::LETTERS_ONLY) {
    strncpy(out, g_composeText, outSize - 1);
    out[outSize - 1] = '\0';
  } else if (mode == Settings::TypingDisplay::MIXED) {
    snprintf(out, outSize, "%s%s%s", g_composeText, (g_composePatternLen > 0 ? " " : ""), g_composePattern);
  } else {
    char raw[64];
    buildCanonicalRawMorse(g_composeText, raw, sizeof(raw));
    snprintf(out, outSize, "%s%s", raw, g_composePattern);
  }
}

void handleComposeEvent(const InputEvent& e) {
  if (e.type == InputEventType::DOT_RELEASE) {
    g_composeError = nullptr;
    if (e.durationMs >= Morse::kSpecialCommandMs) {
      clearDraft();
      return;
    }
    Morse::SymbolClass sc = Morse::classifyPress(e.durationMs, Settings::getWpm());
    if (g_composePatternLen < Morse::kMaxPatternLength) {
      g_composePattern[g_composePatternLen++] = (sc == Morse::SymbolClass::DOT) ? '.' : '-';
      g_composePattern[g_composePatternLen] = '\0';
    }
    g_lastMorseReleaseMs = millis();
    if (Morse::isDeletePattern(g_composePattern)) {
      if (g_composeLen > 0) {
        g_composeLen--;
        g_composeText[g_composeLen] = '\0';
      }
      resetComposePattern();
    }
  } else if (e.type == InputEventType::ENCODER_SHORT) {
    if (g_composeLen > 0) {
      sendEnigmaMessage();
    } else {
      invokeEmptyLineAction(Modes::ENIGMA, g_selectedGroupCode, g_selectedContactKey);
    }
  } else if (e.type == InputEventType::ENCODER_LONG) {
    TextMessage::clearOpenConversation();
    Menu::goBack();
  }
}

void dispatchHistoryMessageEvent(const MessageStore::ConversationIndexEntry& entry, MessageEventType eventType) {
  MessageRef ref = refForIndexEntry(entry);
  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  MessageEventFn fn = getMessageEventFn(view.envelope.message_type);
  if (fn != nullptr) fn(ref, eventType);
}

void handleHistoryFocusEvent(const InputEvent& e) {
  const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(g_historyCursor);
  if (e.type == InputEventType::DOT_PRESS_START) {
    if (entry != nullptr) dispatchHistoryMessageEvent(*entry, EVT_DOT_HOLD_START);
  } else if (e.type == InputEventType::DOT_RELEASE) {
    if (entry != nullptr) {
      dispatchHistoryMessageEvent(*entry, EVT_DOT_HOLD_END);
      MessageRef ref = refForIndexEntry(*entry);
      StoredMessageView view;
      if (MessageStore::loadMessage(ref, &view) && (view.header.flags & MessageStore::FLAG_UNREAD)) {
        Notifications::clearUnread(g_selectedGroupCode, g_selectedContactKey, ref);
      }
    }
  } else if (e.type == InputEventType::ENCODER_SHORT) {
    if (entry != nullptr) dispatchHistoryMessageEvent(*entry, EVT_ENCODER_SHORT);
  } else if (e.type == InputEventType::COMBINED_REVEAL) {
    if (entry != nullptr) dispatchHistoryMessageEvent(*entry, EVT_COMBINED_REVEAL);
  } else if (e.type == InputEventType::ENCODER_LONG) {
    TextMessage::clearOpenConversation();
    Menu::goBack();
  }
}

void screenEnigmaChat() {
  if (Menu::consumeJustEntered()) {
    clearDraft();
    g_historyCursor = kNoHistoryCursor;
    TextMessage::setOpenConversation(g_selectedGroupCode, g_selectedContactKey);
    markIndexDirty();
  }

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      refreshIndexIfNeeded();
      if (g_historyCursor == kNoHistoryCursor) {
        if (e.value < 0 && g_indexTotal > 0) g_historyCursor = static_cast<uint16_t>(g_indexTotal - 1);
      } else if (e.value < 0) {
        if (g_historyCursor > 0) g_historyCursor--;
      } else {
        if (g_historyCursor + 1 >= g_indexTotal) {
          g_historyCursor = kNoHistoryCursor;
        } else {
          g_historyCursor++;
        }
      }
    } else if (g_historyCursor != kNoHistoryCursor) {
      handleHistoryFocusEvent(e);
    } else {
      handleComposeEvent(e);
    }
  }

  if (g_historyCursor == kNoHistoryCursor && g_composePatternLen > 0) {
    if (millis() - g_lastMorseReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeComposeChar();
    }
  }

  refreshIndexIfNeeded();

  Display::drawStatusBar();
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  constexpr uint16_t kViewportLines = 4;
  uint16_t startIdx = 0;
  if (g_indexTotal > kViewportLines) startIdx = static_cast<uint16_t>(g_indexTotal - kViewportLines);
  if (g_historyCursor != kNoHistoryCursor) {
    if (g_historyCursor < startIdx) {
      startIdx = g_historyCursor;
    } else if (g_historyCursor >= startIdx + kViewportLines) {
      startIdx = static_cast<uint16_t>(g_historyCursor - kViewportLines + 1);
    }
  }

  int16_t y = Display::kStatusBarHeight + 2;
  for (uint16_t i = startIdx; i < g_indexTotal && i < startIdx + kViewportLines; i++) {
    const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(i);
    if (entry == nullptr) continue;
    MessageRef ref = refForIndexEntry(*entry);
    StoredMessageView view;
    char lineBuf[48] = "?";
    if (MessageStore::loadMessage(ref, &view)) {
      RenderFn renderFn = getMessageRenderFn(view.envelope.message_type);
      if (renderFn != nullptr) renderFn(view, lineBuf, sizeof(lineBuf));
    }
    tft.setCursor(2, y);
    tft.print(i == g_historyCursor ? "> " : "  ");
    tft.print(lineBuf);
    y += 10;
  }

  y = Display::kStatusBarHeight + 2 + kViewportLines * 10 + 4;
  if (g_composeError != nullptr) {
    tft.setCursor(2, y);
    tft.print(g_composeError);
    y += 10;
  }
  char composeLine[64];
  buildComposeDisplay(composeLine, sizeof(composeLine));
  tft.setCursor(2, y);
  tft.print(g_historyCursor == kNoHistoryCursor ? "> " : "  ");
  tft.print(composeLine);
}

// =============================================================================
// TEXT... ENIGMA message type registration
// =============================================================================
bool g_holdActive = false;
MessageRef g_heldRef;

bool refsEqual(const MessageRef& a, const MessageRef& b) {
  return strcmp(a.group_code, b.group_code) == 0 && strcmp(a.contact_key, b.contact_key) == 0 &&
         a.sequence == b.sequence;
}

void renderEnigmaMessage(const StoredMessageView& msg, char* outBuffer, size_t outBufferSize) {
  char ciphertext[EnigmaCrypto::kMaxEscapedLen + 1];
  uint32_t fp, gen;
  EnigmaKeyConfig embedded;
  if (!EnigmaKeys::decodeEnigmaPayload(msg.typePayload, msg.typePayloadLen, ciphertext, sizeof(ciphertext), &fp,
                                       &gen, &embedded)) {
    snprintf(outBuffer, outBufferSize, "?");
    return;
  }

  uint8_t lockState;
  char cachedText[251];
  decodeLocalPayload(msg.localPayload, msg.localPayloadLen, &lockState, cachedText, sizeof(cachedText));

  if (g_holdActive && refsEqual(msg.ref, g_heldRef)) {
    if (lockState != LOCK_BLACK) {
      snprintf(outBuffer, outBufferSize, "%s", cachedText);
    } else {
      snprintf(outBuffer, outBufferSize, "[locked]");
    }
    return;
  }

  const char* icon = (lockState == LOCK_UNLOCKED) ? "[U]" : (lockState == LOCK_WHITE) ? "[W]" : "[B]";
  snprintf(outBuffer, outBufferSize, "%s %s", icon, ciphertext);
}

void onEnigmaMessageEvent(const MessageRef& ref, MessageEventType eventType) {
  if (eventType == EVT_DOT_HOLD_START) {
    g_holdActive = true;
    g_heldRef = ref;
    return;
  }
  if (eventType == EVT_DOT_HOLD_END) {
    g_holdActive = false;
    return;
  }

  StoredMessageView view;
  if (!MessageStore::loadMessage(ref, &view)) return;
  uint8_t lockState;
  char cachedText[251];
  decodeLocalPayload(view.localPayload, view.localPayloadLen, &lockState, cachedText, sizeof(cachedText));

  if (eventType == EVT_ENCODER_SHORT) {
    if (lockState == LOCK_BLACK || lockState == LOCK_WHITE) {
      startReceiveKeyEditorFor(ref);
      Menu::pushScreen(screenKeyEditor);
    }
    // Unlocked: no defined action here (hold-preview already shows plaintext).
  } else if (eventType == EVT_COMBINED_REVEAL) {
    if (lockState == LOCK_BLACK || lockState == LOCK_WHITE) {
      startRevealFlow(ref, view);
      Menu::pushScreen(screenReveal);
    }
    // Unlocked: gesture has no effect (Addendum/Phase 3 section 7).
  }
}

// =============================================================================
// Incoming Enigma arrival (via TextMessage's PK_MESSAGE dispatch-by-type)
// =============================================================================
void handleEnigmaArrival(const char* group_code, const char* contact_key, const PacketCodec::MessageEnvelope& env,
                         uint32_t effectiveTs, const uint8_t* typePayload, uint16_t typePayloadLen) {
  static uint8_t wireBuf[PacketCodec::kHeaderSize + 340];
  size_t wireLen = PacketCodec::encodeMessagePacket(env, typePayload, typePayloadLen, wireBuf, sizeof(wireBuf));
  if (wireLen == 0) return;

  uint8_t localPayload[8];
  size_t localLen = encodeLocalPayload(LOCK_BLACK, "", localPayload, sizeof(localPayload));

  MessageRef ref;
  bool ok = MessageStore::appendStoredMessage(group_code, contact_key, MessageStore::Direction::RECEIVED,
                                              MessageStore::FLAG_UNREAD, effectiveTs, wireBuf,
                                              static_cast<uint16_t>(wireLen), localPayload, localLen, &ref);
  if (!ok) return;

  bool conversationOpen = TextMessage::isConversationOpen(group_code, contact_key);
  if (conversationOpen) markIndexDirty();
  Notifications::onMessageArrived(group_code, contact_key, ref, Notifications::BADGE_ENIGMA, conversationOpen);
}

// =============================================================================
// Mode handler entry point
// =============================================================================
void screenEnigmaEntry() {
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
    registerModeHandler(Modes::ENIGMA, screenEnigmaEntry);
    registerMessageType(PacketCodec::MSG_TYPE_ENIGMA, renderEnigmaMessage, onEnigmaMessageEvent);
    TextMessage::registerIncomingMessageHandler(PacketCodec::MSG_TYPE_ENIGMA, handleEnigmaArrival);
  }
};
Registrar g_registrar;

}  // namespace

namespace Enigma {

// Resets the stack and leaves Recipient Selection (this group) underneath
// the directly-pushed Chat, so Encoder long from Chat lands on "that mode's
// normal Recipient Selection" (Phase 3 section 11's closing rule) rather
// than whatever deep Game-creation stack the quick-switch was raised from.
void navigateToChatDirect(const char* group_code, const char* contact_key) {
  strncpy(g_selectedGroupCode, group_code, sizeof(g_selectedGroupCode) - 1);
  g_selectedGroupCode[sizeof(g_selectedGroupCode) - 1] = '\0';
  strncpy(g_selectedContactKey, contact_key, sizeof(g_selectedContactKey) - 1);
  g_selectedContactKey[sizeof(g_selectedContactKey) - 1] = '\0';
  Menu::init();
  Menu::pushScreen(screenRecipient);
  Menu::pushScreen(screenEnigmaChat);
}

}  // namespace Enigma
