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
#include "core/ui_scratch.h"
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
  bool justEntered = Menu::consumeJustEntered();

  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (Input::isBack(e)) Menu::goBack();
  }

  Display::drawStatusBar();
  if (!justEntered) return;

  Display::setFont(Display::Font::PRIMARY);
  Display::clearContentArea();
  int16_t lh = Display::lineHeight();
  Display::printLine(6, 60, "No Family Groups");
  Display::printLine(6, 60 + lh, "Add one in Settings");
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

// Hardware Fix #4 issue 6: every step/slot transition goes through this one
// function (including re-entering the SAME step for the next rotor/pair
// slot, e.g. ROTOR_TYPE -> ROTOR_TYPE for rotor 2), so it is the single
// correct place to flag "the value row's label genuinely changed, a full
// row redraw is acceptable" -- consumed once by the next render.
bool g_keyEditStepEntered = true;

void enterStep(KeyEditStep step) {
  g_editStep = step;
  g_keyEditStepEntered = true;
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

bool g_keyEditorDirty = true;
bool g_keyEditorNeedsFullRedraw = true;
char g_lastKeyEditorWarnLine[40] = {0};

// Value-row tracking (Hardware Fix #4 issue 6). LABEL_VALUE steps (Rotor
// Count/Plugboard Pairs/Rotor Type/Rotor Position/Pair Letter 1/Pair
// Letter 2) diff a stable label against a separately-drawn dynamic value;
// TOGGLE_TWO steps (Method Choice, Confirm) diff a marker moving between
// two fixed, never-repainted labels -- same pattern as MixedTextEntry's
// CONFIRM selector and Menu's Yes/No prompt.
enum class KeyEditRowKind : uint8_t { LABEL_VALUE, TOGGLE_TWO };
char g_lastKeyEditLabel[32] = {0};
char g_lastKeyEditValue[8] = {0};
bool g_lastKeyEditToggleASelected = true;

void screenKeyEditor() {
  if (Menu::consumeJustEntered()) {
    g_keyEditorDirty = true;
    g_keyEditorNeedsFullRedraw = true;
  }

  Input::update();
  InputEvent e;
  bool hadEvent = false;
  while (Input::popEvent(e)) {
    hadEvent = true;
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

  if (hadEvent) g_keyEditorDirty = true;
  Display::drawStatusBar();
  if (!g_keyEditorDirty) return;
  g_keyEditorDirty = false;

  Display::setFont(Display::Font::PRIMARY);
  int16_t lh = Display::lineHeight();
  int16_t titleY = Display::kStatusBarHeight + 2;
  int16_t valueY = static_cast<int16_t>(titleY + lh);
  int16_t warnY = static_cast<int16_t>(valueY + lh);

  bool firstDraw = g_keyEditorNeedsFullRedraw;
  if (firstDraw) {
    Display::clearContentArea();
    Display::printLine(2, titleY, g_editingSender ? "Sender Key" : "Receive Key");
    g_keyEditorNeedsFullRedraw = false;
  }

  // Value row (Hardware Fix #4 issue 6): LABEL_VALUE steps keep a stable
  // label separate from a dynamic value so rotating never repaints the
  // label; TOGGLE_TWO steps (Method Choice, Confirm) keep two fixed
  // labels with only a "> " marker moving between them. A step or
  // rotor/pair slot change (g_keyEditStepEntered, set by enterStep())
  // allows a full value-row redraw since the label genuinely changed --
  // still never a full content-area clear outside first entry.
  KeyEditRowKind kind;
  char label[32] = {0};
  char value[8] = {0};
  const char* toggleLabelA = nullptr;
  const char* toggleLabelB = nullptr;
  bool toggleASelected = false;
  switch (g_editStep) {
    case KeyEditStep::ROTOR_COUNT:
      kind = KeyEditRowKind::LABEL_VALUE;
      snprintf(label, sizeof(label), "Rotor Count: ");
      snprintf(value, sizeof(value), "%d", g_pickerIndex);
      break;
    case KeyEditStep::PLUGBOARD_COUNT:
      kind = KeyEditRowKind::LABEL_VALUE;
      snprintf(label, sizeof(label), "Plugboard Pairs: ");
      snprintf(value, sizeof(value), "%d", g_pickerIndex);
      break;
    case KeyEditStep::METHOD_CHOICE:
      kind = KeyEditRowKind::TOGGLE_TWO;
      toggleLabelA = "Manual Setup";
      toggleLabelB = "Randomize All";
      toggleASelected = (g_pickerIndex == 0);
      break;
    case KeyEditStep::ROTOR_TYPE: {
      static const char* const kRotorNames[] = {"I", "II", "III", "IV", "V"};
      kind = KeyEditRowKind::LABEL_VALUE;
      snprintf(label, sizeof(label), "Rotor %d Type: ", g_editRotorSlot + 1);
      snprintf(value, sizeof(value), "%s", kRotorNames[g_pickerIndex]);
      break;
    }
    case KeyEditStep::ROTOR_POSITION:
      kind = KeyEditRowKind::LABEL_VALUE;
      snprintf(label, sizeof(label), "Rotor %d Pos: ", g_editRotorSlot + 1);
      snprintf(value, sizeof(value), "%c", static_cast<char>('A' + g_pickerIndex));
      break;
    case KeyEditStep::PLUG_LETTER1:
      kind = KeyEditRowKind::LABEL_VALUE;
      snprintf(label, sizeof(label), "Pair %d Letter 1: ", g_editPairSlot + 1);
      snprintf(value, sizeof(value), "%c", static_cast<char>('A' + g_pickerIndex));
      break;
    case KeyEditStep::PLUG_LETTER2:
      kind = KeyEditRowKind::LABEL_VALUE;
      // Stable portion includes the already-confirmed first pair letter.
      snprintf(label, sizeof(label), "Pair %d Letter 2: %c", g_editPairSlot + 1, g_editFirstLetter);
      snprintf(value, sizeof(value), "%c", static_cast<char>('A' + g_pickerIndex));
      break;
    case KeyEditStep::CONFIRM:
      kind = KeyEditRowKind::TOGGLE_TWO;
      toggleLabelA = "Save";
      toggleLabelB = "Cancel";
      toggleASelected = g_confirmSaveSelected;
      break;
  }

  bool stepChanged = firstDraw || g_keyEditStepEntered;
  g_keyEditStepEntered = false;
  int16_t markerW = static_cast<int16_t>(Display::textWidth(">") + 4);

  if (kind == KeyEditRowKind::LABEL_VALUE) {
    int16_t valueX = static_cast<int16_t>(2 + Display::textWidth(label));
    if (stepChanged) {
      if (!firstDraw) {
        int16_t eraseW = static_cast<int16_t>(Display::kScreenWidth - 2);
        Display::tft().fillRect(2, valueY, eraseW, lh, ST77XX_BLACK);
      }
      Display::printLine(2, valueY, label);
      Display::printLine(valueX, valueY, value);
    } else if (strcmp(value, g_lastKeyEditValue) != 0) {
      // The hot path: only the rotated value changed. The label is
      // provably unchanged (same step, same slot), so it is never
      // touched -- only the value's own glyph-bounds cell is erased.
      int16_t oldW = Display::textWidth(g_lastKeyEditValue);
      int16_t newW = Display::textWidth(value);
      int16_t eraseW = static_cast<int16_t>((oldW > newW ? oldW : newW) + 4);
      int16_t maxW = static_cast<int16_t>(Display::kScreenWidth - valueX);
      if (eraseW > maxW) eraseW = maxW;
      if (eraseW < 0) eraseW = 0;
      Display::tft().fillRect(valueX, valueY, eraseW, lh, ST77XX_BLACK);
      Display::printLine(valueX, valueY, value);
    }
    strncpy(g_lastKeyEditLabel, label, sizeof(g_lastKeyEditLabel) - 1);
    g_lastKeyEditLabel[sizeof(g_lastKeyEditLabel) - 1] = '\0';
    strncpy(g_lastKeyEditValue, value, sizeof(g_lastKeyEditValue) - 1);
    g_lastKeyEditValue[sizeof(g_lastKeyEditValue) - 1] = '\0';
  } else {
    // TOGGLE_TWO: both labels are fixed text -- only the "> " marker
    // between them ever moves on a plain toggle (same pattern as
    // MixedTextEntry's CONFIRM selector and Menu's Yes/No prompt).
    int16_t aMarkerX = 2;
    int16_t aLabelX = static_cast<int16_t>(aMarkerX + markerW);
    int16_t aLabelW = Display::textWidth(toggleLabelA);
    int16_t gapW = Display::textWidth("    ");
    int16_t bMarkerX = static_cast<int16_t>(aLabelX + aLabelW + gapW);
    int16_t bLabelX = static_cast<int16_t>(bMarkerX + markerW);
    if (stepChanged) {
      if (!firstDraw) {
        int16_t eraseW = static_cast<int16_t>(Display::kScreenWidth - 2);
        Display::tft().fillRect(2, valueY, eraseW, lh, ST77XX_BLACK);
      }
      if (toggleASelected) Display::printLine(aMarkerX, valueY, ">");
      Display::printLine(aLabelX, valueY, toggleLabelA);
      if (!toggleASelected) Display::printLine(bMarkerX, valueY, ">");
      Display::printLine(bLabelX, valueY, toggleLabelB);
    } else if (toggleASelected != g_lastKeyEditToggleASelected) {
      int16_t oldMarkerX = g_lastKeyEditToggleASelected ? aMarkerX : bMarkerX;
      int16_t newMarkerX = toggleASelected ? aMarkerX : bMarkerX;
      Display::tft().fillRect(oldMarkerX, valueY, markerW, lh, ST77XX_BLACK);
      Display::tft().fillRect(newMarkerX, valueY, markerW, lh, ST77XX_BLACK);
      Display::printLine(newMarkerX, valueY, ">");
    }
    g_lastKeyEditToggleASelected = toggleASelected;
    g_lastKeyEditLabel[0] = '\0';
    g_lastKeyEditValue[0] = '\0';
  }

  char warnLine[40];
  if (g_editStep == KeyEditStep::CONFIRM && g_working.rotor_count == 0 && g_working.plugboard_pair_count == 0) {
    snprintf(warnLine, sizeof(warnLine), "Warning: plaintext passthrough");
  } else {
    warnLine[0] = '\0';
  }
  if (firstDraw || strcmp(warnLine, g_lastKeyEditorWarnLine) != 0) {
    if (!firstDraw) Display::tft().fillRect(0, warnY, Display::kScreenWidth, lh, ST77XX_BLACK);
    if (warnLine[0] != '\0') Display::printLine(2, warnY, warnLine);
    strncpy(g_lastKeyEditorWarnLine, warnLine, sizeof(g_lastKeyEditorWarnLine) - 1);
    g_lastKeyEditorWarnLine[sizeof(g_lastKeyEditorWarnLine) - 1] = '\0';
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
  if (!EnigmaKeys::decodeEnigmaPayload(view.typePayload, view.typePayloadLen, ciphertext, sizeof(ciphertext), &fp,
                                       &gen, &g_revealedKey)) {
    // Malformed/corrupt payload: fail safe to an empty key (0 rotors, 0
    // plugboard pairs) rather than leaving g_revealedKey holding whatever
    // out-of-range bytes decodeKeyConfig() rejected -- screenReveal() would
    // otherwise index kNames[] with an unvalidated rotor_type.
    memset(&g_revealedKey, 0, sizeof(g_revealedKey));
  }
}

bool g_revealDirty = true;

void screenReveal() {
  if (Menu::consumeJustEntered()) g_revealDirty = true;

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

  Display::drawStatusBar();
  if (!g_revealDirty) return;
  g_revealDirty = false;

  Display::clearContentArea();

  // Title uses PRIMARY (Hardware Fix #1); the rotor-by-rotor detail list
  // below stays at COMPACT -- up to kMaxRotors=4 entries plus 3 more lines
  // (title/count/plugs+instruction) would overflow the 121px content area
  // at PRIMARY's larger line height (re-verified per Fix #1 item 14: this
  // is one of the explicitly higher-risk "icon/text density" screens).
  Display::setFont(Display::Font::PRIMARY);
  int16_t primaryLh = Display::lineHeight();
  Display::printLine(2, Display::kStatusBarHeight + 2, "Revealed Key");

  Display::setFont(Display::Font::COMPACT);
  int16_t lh = Display::lineHeight();
  int16_t y = Display::kStatusBarHeight + 2 + primaryLh;

  char line[48];
  snprintf(line, sizeof(line), "Rotors: %d", g_revealedKey.rotor_count);
  Display::printLine(2, y, line);
  y += lh;

  for (uint8_t i = 0; i < g_revealedKey.rotor_count; i++) {
    static const char* const kNames[] = {"I", "II", "III", "IV", "V"};
    snprintf(line, sizeof(line), "  %s @ %c", kNames[g_revealedKey.rotor_type[i]],
             static_cast<char>('A' + g_revealedKey.rotor_position[i]));
    Display::printLine(2, y, line);
    y += lh;
  }
  snprintf(line, sizeof(line), "Plugs: %d", g_revealedKey.plugboard_pair_count);
  Display::printLine(2, y, line);
  y += lh;
  Display::printLine(2, y, "DOT: use as Receive Key");
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
// Which of the selected logical message's own wrapped visual rows is
// focused (Hardware Fix #4.3a issue 1) -- 0 is its first row. Lets a
// message taller than the history viewport be scrolled through row by
// row while g_historyCursor keeps pointing at the same logical message
// the whole time. Always 0 while g_historyCursor == kNoHistoryCursor.
uint16_t g_historyRowOffset = 0;

char g_composeText[EnigmaCrypto::kMaxEscapedLen + 1];
uint8_t g_composeLen = 0;

// Hardware Fix #4.3 issue E: buildComposePrefixSuffix()'s RAW-mode output
// (one-way canonical Morse text -- dots/dashes/slashes/spaces, up to
// Morse::kMaxPatternLength symbol chars plus a trailing space per
// confirmed character) can be far longer than g_composeText itself, so the
// display-side buffer that holds it is sized from that same worst case
// instead of an arbitrary guess -- this is the buffer the task's "do NOT
// merely enlarge composePrefix[64]" instruction refers to; enlarging it
// alone would still show only one un-wrapped line; wrapping alone would
// still lose confirmed text past the old 64-byte cap before it ever
// reached the wrapper. Both are needed together.
//
// Hardware Fix #4.4b: at 1801 bytes, this is no longer a permanent global
// array -- four screens each keeping one of these in static .bss
// overflowed the ESP32's DRAM segment at link time. screenEnigmaChat()
// instead obtains it from UiScratch::ensure(Slot::A, ...) on each render
// (heap-backed, allocated once and reused, never freed/realloced per
// frame -- see ui_scratch.h).
constexpr size_t kComposePrefixCap = EnigmaCrypto::kMaxEscapedLen * (Morse::kMaxPatternLength + 1) + 1;
char g_composePattern[Morse::kMaxPatternLength + 1];
uint8_t g_composePatternLen = 0;
uint32_t g_lastMorseReleaseMs = 0;
Morse::WordGapState g_composeWordGap;
const char* g_composeError = nullptr;

bool g_indexDirty = true;
uint16_t g_indexTotal = 0;

void markIndexDirty() { g_indexDirty = true; }
void refreshIndexIfNeeded() {
  if (!g_indexDirty) return;
  g_indexTotal = MessageStore::loadConversationIndex(g_selectedGroupCode, g_selectedContactKey);
  g_indexDirty = false;
}

// True while the pattern currently being keyed (g_composePattern) is known
// to start a new word -- captured once, at that pattern's FIRST symbol
// (see captureWordBoundaryOnSymbolStart()), and left untouched by symbol
// 2/3/... of the same pattern. Only consumed -- as an actual ASCII space,
// committed immediately before the decoded character, before
// EnigmaCrypto::normalizeAndEscape() ever sees it at send time -- by
// finalizeComposeChar()'s NORMAL letter finalization; a delete prosign or
// a special-command clear discard it without ever writing a space
// (Hardware Fix #4.2). normalizeAndEscape()/reverseEscape() already
// round-trip a space through the escaped ciphertext alphabet unchanged,
// so no crypto changes are needed for "HELLO WORLD" to decrypt back to
// "HELLO WORLD".
bool g_currentPatternStartsNewWord = false;

void resetComposePattern() {
  g_composePatternLen = 0;
  g_composePattern[0] = '\0';
}
void clearDraft() {
  g_composeLen = 0;
  g_composeText[0] = '\0';
  resetComposePattern();
  g_composeError = nullptr;
  g_currentPatternStartsNewWord = false;
  Morse::cancelWordGap(&g_composeWordGap);
}

// Called on every DOT_PRESS_START, before the new symbol is accepted into
// the pattern buffer (Hardware Fix #4.2). Only captures whether the
// pattern now starting is a new word -- and only at that pattern's FIRST
// symbol (g_composePatternLen == 0); symbol 2/3/... of a multi-symbol
// character (e.g. W = .--) must never re-resolve or overwrite this flag.
// Never mutates compose text itself -- see finalizeComposeChar().
void captureWordBoundaryOnSymbolStart() {
  if (g_composePatternLen != 0) return;
  g_currentPatternStartsNewWord =
      Morse::consumeWordBoundaryOnSymbolStart(&g_composeWordGap, Settings::getWpm(), millis());
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

// Draws a history row's optional per-type status icon (currently only
// Enigma's own lock state) in its fixed kLockIconCellWidth cell before the
// sender name -- shape AND color both carry the state, replacing the old
// user-visible [B]/[W]/[U] text tokens (Hardware Fix #4 issue 5).
void drawRowIcon(int16_t x, int16_t y, MessageIconKind icon) {
  switch (icon) {
    case MessageIconKind::LOCK_CLOSED_RED:
      Display::drawLockIcon(x, y, false, ST77XX_RED);
      break;
    case MessageIconKind::LOCK_CLOSED_YELLOW:
      Display::drawLockIcon(x, y, false, ST77XX_YELLOW);
      break;
    case MessageIconKind::LOCK_OPEN_GREEN:
      Display::drawLockIcon(x, y, true, ST77XX_GREEN);
      break;
    case MessageIconKind::NONE:
      break;
  }
}

// =============================================================================
// Multi-line history + compose layout (Hardware Fix #4.3 issue E). A single
// logical message/compose line can now span more than one visual row,
// word-wrapped to the real pixel width of the currently-active font
// (Display::wrapText()) instead of being clipped at a fixed character
// count (the old lineBuf[48]/composePrefix[64]). This never changes
// message storage, wire format, or crypto -- it only changes how the
// already-decoded text is laid out on screen.
// =============================================================================

// Matches EnigmaCrypto's own ciphertext[kMaxEscapedLen+1] (201) and
// decodeLocalPayload's cachedText[251] caps used by renderEnigmaMessage()
// above -- no RenderFn implementation (Enigma's or any other message
// type's) can ever produce more text than those already hold, so sizing
// the display-side buffer to match can only recover text the old
// lineBuf[48] used to silently clip, never mask a truncation that isn't
// already there.
constexpr size_t kHistoryLineBufCap = 251;

// Hardware Fix #4.3a issue 1: a message's row COUNT is no longer capped by
// a fixed-size wrap-span array (the old kMaxHistoryWrapLines silently
// dropped anything past it, and even below that cap there was no way to
// bring rows past the viewport height into view). loadHistoryRow() now
// only loads text/sender/icon and the body width to wrap at; individual
// rows are produced on demand by streaming through Display::wrapLineAt(),
// so a message's visual row count is bounded only by its own text length
// (already capped at kHistoryLineBufCap by the source data itself), never
// by an unrelated guessed array size.
// Hardware Fix #4.7: two width regimes per message instead of one. TRUE
// ROW 0 (compact cursor cell + icon cell + CYAN sender prefix + WHITE
// body) is narrower than every row after it, which drops the icon/sender
// entirely and starts right after the compact cursor cell
// (continuationX == labelX). Every consumer of a message's row layout
// (row counting, viewport placement, focused-row stepping, historyRowY(),
// and actual drawing) reads these same four fields off one
// HistoryRowInfo, so they can never disagree.
struct HistoryRowInfo {
  char lineBuf[kHistoryLineBufCap];
  char senderPrefix[24];
  MessageIconKind icon;
  int16_t firstBodyX;
  int16_t firstBodyWidth;
  int16_t continuationX;
  int16_t continuationWidth;
};

// Loads history entry `index` and computes where its body text wraps.
// `labelX` is the fixed compact-cursor-cell-relative icon-cell X already
// used by every history row's TRUE first row (2 + Display::kCursorCellWidth).
void loadHistoryRow(uint16_t index, int16_t labelX, HistoryRowInfo* out) {
  out->lineBuf[0] = '?';
  out->lineBuf[1] = '\0';
  out->senderPrefix[0] = '\0';
  out->icon = MessageIconKind::NONE;
  out->continuationX = labelX;
  out->continuationWidth = static_cast<int16_t>(Display::kScreenWidth - out->continuationX);
  const MessageStore::ConversationIndexEntry* entry = MessageStore::getIndexEntry(index);
  if (entry != nullptr) {
    MessageRef ref = refForIndexEntry(*entry);
    StoredMessageView view;
    if (MessageStore::loadMessage(ref, &view)) {
      RenderFn renderFn = getMessageRenderFn(view.envelope.message_type);
      if (renderFn != nullptr) renderFn(view, out->lineBuf, sizeof(out->lineBuf));
      MessageStore::buildSenderPrefix(view.envelope, out->senderPrefix, sizeof(out->senderPrefix));
      MessageIconFn iconFn = getMessageIconFn(view.envelope.message_type);
      if (iconFn != nullptr) out->icon = iconFn(view);
    }
  }
  int16_t textX = static_cast<int16_t>(labelX + Display::kLockIconCellWidth);
  out->firstBodyX = static_cast<int16_t>(textX + Display::textWidth(out->senderPrefix));
  out->firstBodyWidth = static_cast<int16_t>(Display::kScreenWidth - out->firstBodyX);
}

// Streams through an already-loaded message's wrapped rows via
// Display::wrapLineAt(), never materializing them all at once. Returns the
// total row count (always >= 1: an empty body still occupies one row
// rather than vanishing, same as before).
uint16_t countHistoryRows(const HistoryRowInfo& info) {
  size_t len = strlen(info.lineBuf);
  if (len == 0) return 1;
  uint16_t rows = 0;
  size_t pos = 0;
  int16_t width = info.firstBodyWidth;
  while (pos < len) {
    uint16_t s, l;
    if (!Display::wrapLineAt(info.lineBuf, pos, width, &s, &l)) break;
    pos = static_cast<size_t>(s) + l;
    rows++;
    width = info.continuationWidth;  // row 0 wraps at firstBodyWidth; every row after at continuationWidth
  }
  return (rows > 0) ? rows : 1;
}

uint16_t countHistoryRowsFor(uint16_t index, int16_t labelX) {
  HistoryRowInfo info;
  loadHistoryRow(index, labelX, &info);
  return countHistoryRows(info);
}

// Picks the history viewport's starting position as a (message index,
// rows-already-scrolled-past-within-that-message) pair instead of a plain
// message index (Hardware Fix #4.3a issue 1), so a single logical message
// taller than the viewport can itself be windowed mid-message -- the bug
// this corrective round fixes: previously only whole messages could be
// skipped, so a message alone taller than the viewport could never have
// its later rows brought into view.
//
// Default placement is bottom-anchored (fills the viewport with the most
// recent rows, splitting the oldest included message mid-way if it alone
// doesn't fit the remaining budget) exactly as before. When a message is
// focused (g_historyCursor != kNoHistoryCursor), that default window is
// kept AS-IS whenever the focused row already falls inside it -- so a
// simple cursor move that stays within the visible rows never moves the
// viewport (preserving the marker-only partial redraw) -- and is shifted
// the minimum amount otherwise so the focused row becomes visible.
void computeHistoryViewport(uint16_t viewportLines, int16_t labelX, uint16_t* outStartIdx,
                            uint16_t* outStartRowSkip) {
  *outStartIdx = 0;
  *outStartRowSkip = 0;
  if (g_indexTotal == 0) return;

  int32_t budget = viewportLines;
  uint16_t defIdx = 0;
  uint16_t defSkip = 0;
  uint16_t idx = g_indexTotal;
  bool placedAny = false;
  while (idx > 0) {
    idx--;
    uint16_t rows = countHistoryRowsFor(idx, labelX);
    if (static_cast<int32_t>(rows) <= budget) {
      budget -= rows;
      defIdx = idx;
      defSkip = 0;
      placedAny = true;
      if (budget == 0) break;
    } else {
      // This message alone is taller than the remaining budget -- show
      // only its last `budget` rows so the viewport still fills from the
      // bottom (rather than either overflowing or being skipped whole).
      defIdx = idx;
      defSkip = static_cast<uint16_t>(rows - static_cast<uint16_t>(budget));
      placedAny = true;
      break;
    }
  }
  if (!placedAny) {
    defIdx = 0;
    defSkip = 0;
  }

  if (g_historyCursor == kNoHistoryCursor) {
    *outStartIdx = defIdx;
    *outStartRowSkip = defSkip;
    return;
  }

  if (g_historyCursor < defIdx || (g_historyCursor == defIdx && g_historyRowOffset < defSkip)) {
    // Focus is above (older than) the default window -- scroll up so the
    // focused row becomes the viewport's first row.
    *outStartIdx = g_historyCursor;
    *outStartRowSkip = g_historyRowOffset;
    return;
  }

  // Rows strictly before (g_historyCursor, g_historyRowOffset), counting
  // forward from (defIdx, defSkip).
  int32_t rowsBefore;
  {
    HistoryRowInfo first;
    loadHistoryRow(defIdx, labelX, &first);
    if (defIdx == g_historyCursor) {
      rowsBefore = static_cast<int32_t>(g_historyRowOffset) - static_cast<int32_t>(defSkip);
    } else {
      rowsBefore = static_cast<int32_t>(countHistoryRows(first)) - static_cast<int32_t>(defSkip);
      for (uint16_t i = static_cast<uint16_t>(defIdx + 1); i < g_historyCursor; i++) {
        rowsBefore += countHistoryRowsFor(i, labelX);
      }
      rowsBefore += g_historyRowOffset;
    }
  }

  if (rowsBefore < static_cast<int32_t>(viewportLines)) {
    // Already visible inside the default window -- keep it stable.
    *outStartIdx = defIdx;
    *outStartRowSkip = defSkip;
    return;
  }

  // Focus is below the default window -- shift forward one row at a time
  // (bounded by exactly how far off-window the focus is) until it just
  // fits as the last visible row.
  uint16_t curIdx = defIdx;
  uint16_t curSkip = defSkip;
  while (rowsBefore >= static_cast<int32_t>(viewportLines)) {
    uint16_t rows = countHistoryRowsFor(curIdx, labelX);
    if (static_cast<uint16_t>(curSkip + 1) < rows) {
      curSkip++;
    } else {
      curIdx++;
      curSkip = 0;
    }
    rowsBefore--;
  }
  *outStartIdx = curIdx;
  *outStartRowSkip = curSkip;
}

// Y of visual row `targetRowIdx` of history index `targetIdx`, given the
// viewport currently starts at (startIdx, startRowSkip).
int16_t historyRowY(uint16_t startIdx, uint16_t startRowSkip, uint16_t targetIdx, uint16_t targetRowIdx,
                    int16_t labelX, int16_t contentTop, int16_t lh) {
  int32_t rows = 0;
  for (uint16_t i = startIdx; i <= targetIdx; i++) {
    HistoryRowInfo info;
    loadHistoryRow(i, labelX, &info);
    uint16_t skip = (i == startIdx) ? startRowSkip : 0;
    uint16_t limit = (i == targetIdx) ? targetRowIdx : countHistoryRows(info);
    if (limit > skip) rows += static_cast<int32_t>(limit - skip);
  }
  return static_cast<int16_t>(contentTop + rows * lh);
}

constexpr uint8_t kMaxComposeWrapLines = 128;
constexpr size_t kComposeTailSrcCap = 96;

struct ComposeLayout {
  uint16_t prefixStarts[kMaxComposeWrapLines];
  uint16_t prefixLens[kMaxComposeWrapLines];
  uint8_t prefixCount;    // rows wrapping the confirmed compose text alone
  char tailSrc[kComposeTailSrcCap];
  uint16_t tailStarts[8];
  uint16_t tailLens[8];
  uint8_t tailCount;      // rows wrapping (last confirmed line + in-progress suffix)
  uint8_t confirmedRows;  // prefixCount>0 ? prefixCount-1 : 0 -- stable across suffix changes
  uint8_t totalRows;      // confirmedRows + tailCount, always >= 1
};

// Splits the compose line into rows that are provably stable while a
// Morse pattern is being keyed in (every row except the very last) and
// rows that depend on the in-progress suffix (the last confirmed line
// re-wrapped together with the suffix). Greedy word-wrap only ever makes
// decisions using text already seen, so appending the suffix can only
// affect where the last confirmed line's own wrap ended -- never any
// earlier line -- which is what lets a single dot/dash repaint just the
// tail instead of the whole compose block (Hardware Fix #3 corrective
// item 3's intent, preserved under multi-line layout).
void buildComposeLayout(const char* prefix, const char* suffix, int16_t widthPx, ComposeLayout* out) {
  out->prefixCount = Display::wrapText(prefix, widthPx, out->prefixStarts, out->prefixLens, kMaxComposeWrapLines);
  out->confirmedRows = (out->prefixCount > 0) ? static_cast<uint8_t>(out->prefixCount - 1) : 0;

  size_t tp = 0;
  if (out->prefixCount > 0) {
    size_t lastStart = out->prefixStarts[out->prefixCount - 1];
    size_t lastLen = out->prefixLens[out->prefixCount - 1];
    if (lastLen >= kComposeTailSrcCap) lastLen = kComposeTailSrcCap - 1;
    memcpy(out->tailSrc, prefix + lastStart, lastLen);
    tp = lastLen;
  }
  size_t suffixLen = strlen(suffix);
  if (tp + suffixLen >= kComposeTailSrcCap) suffixLen = kComposeTailSrcCap - 1 - tp;
  memcpy(out->tailSrc + tp, suffix, suffixLen);
  tp += suffixLen;
  out->tailSrc[tp] = '\0';

  out->tailCount = Display::wrapText(out->tailSrc, widthPx, out->tailStarts, out->tailLens, 8);
  if (out->tailCount == 0) {
    out->tailStarts[0] = 0;
    out->tailLens[0] = 0;
    out->tailCount = 1;  // always at least one (possibly empty) row for the caret
  }
  out->totalRows = static_cast<uint8_t>(out->confirmedRows + out->tailCount);
}

// Slices out the text of logical compose row `rowIdx` (0-based over the
// full, unwindowed row list -- confirmed rows first, then tail rows).
void composeRowText(const ComposeLayout& layout, const char* prefix, uint8_t rowIdx, char* out, size_t outCap) {
  size_t s, l;
  if (rowIdx < layout.confirmedRows) {
    s = layout.prefixStarts[rowIdx];
    l = layout.prefixLens[rowIdx];
    if (l >= outCap) l = outCap - 1;
    memcpy(out, prefix + s, l);
  } else {
    uint8_t tIdx = static_cast<uint8_t>(rowIdx - layout.confirmedRows);
    s = layout.tailStarts[tIdx];
    l = layout.tailLens[tIdx];
    if (l >= outCap) l = outCap - 1;
    memcpy(out, layout.tailSrc + s, l);
  }
  out[l] = '\0';
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
  // Hardware Fix #4.3 issue F: only embed a real, user-chosen name (env is
  // already zeroed above) -- see the matching comment in text_message.cpp.
  if (Settings::hasCustomMyName()) {
    strncpy(env.sender_name_cache, Settings::getMyName(), sizeof(env.sender_name_cache) - 1);
  }
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
  // NORMAL character finalization is the only place a word separator is
  // ever actually committed (Hardware Fix #4.2) -- and only as one atomic
  // write together with the character it separates, so a boundary is
  // never left as a trailing space with no room for the letter that was
  // supposed to follow it.
  bool needsSpace = g_currentPatternStartsNewWord && g_composeLen > 0 && g_composeText[g_composeLen - 1] != ' ';
  size_t needed = needsSpace ? 2 : 1;
  if (g_composeLen + needed <= EnigmaCrypto::kMaxEscapedLen) {
    if (needsSpace) g_composeText[g_composeLen++] = ' ';
    g_composeText[g_composeLen++] = c;
    g_composeText[g_composeLen] = '\0';
  } else if (g_composeLen < EnigmaCrypto::kMaxEscapedLen) {
    g_composeText[g_composeLen++] = c;
    g_composeText[g_composeLen] = '\0';
  }
  g_currentPatternStartsNewWord = false;
  resetComposePattern();
  Morse::armWordGap(&g_composeWordGap, g_lastMorseReleaseMs);
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

// Splits the compose line into a "prefix" (derived solely from confirmed
// g_composeText, so it is provably unchanged while a Morse pattern is being
// keyed in) and a "suffix" (the in-progress g_composePattern, the only part
// that changes on every dot/dash). In LETTERS_ONLY mode the pattern isn't
// shown at all, so the suffix is always empty. Callers use this split to
// avoid repainting confirmed compose text on every symbol (Hardware Fix #3
// corrective item 3). Encryption/key/message semantics are untouched -- this
// only affects how the draft is drawn.
void buildComposePrefixSuffix(char* prefix, size_t prefixSize, char* suffix, size_t suffixSize) {
  Settings::TypingDisplay mode = Settings::getTypingDisplay();
  if (mode == Settings::TypingDisplay::LETTERS_ONLY) {
    strncpy(prefix, g_composeText, prefixSize - 1);
    prefix[prefixSize - 1] = '\0';
    suffix[0] = '\0';
  } else if (mode == Settings::TypingDisplay::MIXED) {
    strncpy(prefix, g_composeText, prefixSize - 1);
    prefix[prefixSize - 1] = '\0';
    snprintf(suffix, suffixSize, "%s%s", (g_composePatternLen > 0 ? " " : ""), g_composePattern);
  } else {
    buildCanonicalRawMorse(g_composeText, prefix, prefixSize);
    strncpy(suffix, g_composePattern, suffixSize - 1);
    suffix[suffixSize - 1] = '\0';
  }
}

void handleComposeEvent(const InputEvent& e) {
  if (e.type == InputEventType::DOT_PRESS_START) {
    // Only captures whether this new pattern starts a new word (Hardware
    // Fix #4.2) -- never mutates compose text itself. The space (if any)
    // is committed later, only at NORMAL letter finalization; a delete
    // prosign discards the captured flag below instead of consuming it as
    // a space.
    captureWordBoundaryOnSymbolStart();
  } else if (e.type == InputEventType::DOT_RELEASE) {
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
      // A delete prosign never commits a word separator -- it must remove
      // the previous REAL confirmed character, not an auto-inserted space
      // that was never actually written to compose text (Hardware Fix
      // #4.2).
      g_currentPatternStartsNewWord = false;
      if (g_composeLen > 0) {
        g_composeLen--;
        g_composeText[g_composeLen] = '\0';
      }
      resetComposePattern();
      Morse::cancelWordGap(&g_composeWordGap);
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

bool g_enigmaChatDirty = true;

// Hardware Fix #3: same three-way redraw split as ListMenu, plus separate
// wasIndexDirty-driven "content changed" and independently-diffed error
// and compose rows. Stored message history is never touched by a plain
// cursor move or by compose activity, only by a genuine content or
// viewport change.
//
// Hardware Fix #4.3 issue E extends this: a history message and the
// compose line can each now span multiple visual rows (word-wrapped by
// real pixel width instead of being truncated), so "the viewport itself
// moved" is tracked directly via g_enigmaChatLastViewportLines (how many
// rows the history region actually had last frame) rather than assumed
// constant -- any change forces a full below-status-bar repaint, since
// every row's Y position shifts together. g_enigmaChatLastComposeSkipped
// catches the one case that doesn't change the history viewport size but
// still moves compose row positions: an already-overflowing compose area
// (more wrapped rows than fit) getting one row longer, which keeps the
// same number of visible compose rows but slides the visible window down
// by one. g_enigmaChatLastComposeRowText snapshots each currently-visible
// compose row's own text so a single dot/dash only repaints the one row
// it actually changed, never the whole compose block.
bool g_enigmaChatNeedsFullRedraw = true;
int16_t g_enigmaChatLastStartIdx = -1;
// Hardware Fix #4.3a issue 1: the viewport's starting position within its
// first visible message (how many of that message's own rows are already
// scrolled past) and the focused message's own row offset are now tracked
// alongside the message-index-only state above, so a pure intra-message
// scroll (same message, same viewport window otherwise) is detected and
// redrawn correctly instead of looking like "nothing changed".
int16_t g_enigmaChatLastStartRowSkip = -1;
uint16_t g_enigmaChatLastCursor = kNoHistoryCursor;
uint16_t g_enigmaChatLastRowOffset = 0;
char g_enigmaChatLastErrorLine[40] = {0};
bool g_enigmaChatLastComposeFocused = true;
constexpr uint16_t kNoViewportLines = 0xFFFF;
uint16_t g_enigmaChatLastViewportLines = kNoViewportLines;
constexpr uint8_t kMaxShownComposeRows = 10;
uint8_t g_enigmaChatLastComposeSkipped = 0xFF;
char g_enigmaChatLastComposeRowText[kMaxShownComposeRows][64] = {{0}};

void screenEnigmaChat() {
  if (Menu::consumeJustEntered()) {
    clearDraft();
    g_historyCursor = kNoHistoryCursor;
    g_historyRowOffset = 0;
    TextMessage::setOpenConversation(g_selectedGroupCode, g_selectedContactKey);
    markIndexDirty();
    g_enigmaChatDirty = true;
    g_enigmaChatNeedsFullRedraw = true;
  }

  // Needed before input handling below: deciding how far ENCODER_ROTATE can
  // step within a multi-row message requires measuring text width, which
  // depends on the active font (Hardware Fix #4.3a issue 1). Cheap/stateless
  // to set every tick, same as every other per-tick font selection in this
  // codebase (see Display::setFont()'s own header comment).
  Display::setFont(Display::Font::PRIMARY);

  Input::update();
  InputEvent e;
  bool hadEvent = false;
  while (Input::popEvent(e)) {
    hadEvent = true;
    if (e.type == InputEventType::ENCODER_ROTATE) {
      refreshIndexIfNeeded();
      // Hardware Fix #4.3a issue 1: a logical message can span more visual
      // rows than the viewport, so rotating steps through THAT message's
      // own rows (g_historyRowOffset) before moving to the next/previous
      // logical message -- g_historyCursor keeps pointing at the same
      // message the whole time a multi-row message is being read, and
      // every one of its rows becomes reachable this way, never just the
      // first few.
      int16_t rotLabelX = static_cast<int16_t>(2 + Display::kCursorCellWidth);
      if (g_historyCursor == kNoHistoryCursor) {
        if (e.value < 0 && g_indexTotal > 0) {
          g_historyCursor = static_cast<uint16_t>(g_indexTotal - 1);
          uint16_t rows = countHistoryRowsFor(g_historyCursor, rotLabelX);
          g_historyRowOffset = static_cast<uint16_t>((rows > 0) ? rows - 1 : 0);
        }
      } else if (e.value < 0) {
        if (g_historyRowOffset > 0) {
          g_historyRowOffset--;
        } else if (g_historyCursor > 0) {
          g_historyCursor--;
          uint16_t rows = countHistoryRowsFor(g_historyCursor, rotLabelX);
          g_historyRowOffset = static_cast<uint16_t>((rows > 0) ? rows - 1 : 0);
        }
      } else {
        uint16_t rows = countHistoryRowsFor(g_historyCursor, rotLabelX);
        if (static_cast<uint16_t>(g_historyRowOffset + 1) < rows) {
          g_historyRowOffset++;
        } else if (g_historyCursor + 1 >= g_indexTotal) {
          g_historyCursor = kNoHistoryCursor;
          g_historyRowOffset = 0;
        } else {
          g_historyCursor++;
          g_historyRowOffset = 0;
        }
      }
    } else if (g_historyCursor != kNoHistoryCursor) {
      handleHistoryFocusEvent(e);
    } else {
      handleComposeEvent(e);
    }
  }
  if (hadEvent) g_enigmaChatDirty = true;

  // Word-boundary resolution is no longer a per-tick check (Hardware Fix
  // #4.1): it only happens at the next DOT_PRESS_START, in
  // handleComposeEvent() above, so idling past 7 dit before pressing Send
  // never mutates compose text on its own.
  if (g_historyCursor == kNoHistoryCursor && g_composePatternLen > 0) {
    if (millis() - g_lastMorseReleaseMs >= Morse::letterGapMs(Settings::getWpm())) {
      finalizeComposeChar();
      g_enigmaChatDirty = true;
    }
  }

  bool wasIndexDirty = g_indexDirty;
  refreshIndexIfNeeded();
  if (wasIndexDirty) g_enigmaChatDirty = true;

  Display::drawStatusBar();
  if (!g_enigmaChatDirty) return;
  g_enigmaChatDirty = false;

  // Font was already set to PRIMARY above (before input handling); still
  // current here since nothing else runs in between.
  int16_t lh = Display::lineHeight();

  int16_t contentTop = Display::kStatusBarHeight + 2;
  int16_t contentHeight = Display::kScreenHeight - contentTop;
  uint16_t totalLines = (contentHeight > 0) ? static_cast<uint16_t>(contentHeight / lh) : 0;
  if (totalLines < 3) totalLines = 3;  // at least 1 history row + error row + compose row

  // Hardware Fix #4.4b: this screen's large RAW-Morse compose expansion
  // buffer now comes from the shared heap-backed scratch pool instead of
  // its own permanent static array (ui_scratch.h). Checked before any
  // drawing happens this pass, so a failure never leaves a half-drawn
  // screen or touches g_composeText/the draft.
  char* composePrefixBuf = UiScratch::ensure(UiScratch::Slot::A, kComposePrefixCap);
  if (composePrefixBuf == nullptr) {
    Display::clearContentArea();
    Display::printLine(2, contentTop, "Memory Low");
    g_enigmaChatNeedsFullRedraw = true;  // force a full redraw once a later pass succeeds
    return;
  }

  // Hardware Fix #4.7: compact cursor cell (Display::kCursorCellWidth)
  // instead of the old PRIMARY-font ">" glyph + padding, on both history
  // and compose -- compose has no sender prefix, so its text starts right
  // after this same cell (Part G).
  int16_t labelX = static_cast<int16_t>(2 + Display::kCursorCellWidth);  // history icon-cell X
  int16_t composePrefixX = labelX;                                       // compose shares the same left margin
  int16_t composeWidth = static_cast<int16_t>(Display::kScreenWidth - composePrefixX);

  // Compose text (Hardware Fix #4.3 issue E): word-wrapped by real pixel
  // width instead of clipped at a fixed character count. Confirmed rows
  // (everything except the last) are provably stable while a pattern is
  // being keyed in -- only the last confirmed line + in-progress suffix is
  // ever re-wrapped on a dot/dash.
  char composeSuffix[Morse::kMaxPatternLength + 2];
  buildComposePrefixSuffix(composePrefixBuf, kComposePrefixCap, composeSuffix, sizeof(composeSuffix));
  bool composeFocused = (g_historyCursor == kNoHistoryCursor);

  ComposeLayout layout;
  buildComposeLayout(composePrefixBuf, composeSuffix, composeWidth, &layout);

  uint16_t maxComposeRows = (totalLines > 1) ? static_cast<uint16_t>(totalLines - 1) : 1;  // reserve 1 row for error line
  if (maxComposeRows > kMaxShownComposeRows) maxComposeRows = kMaxShownComposeRows;
  uint16_t shownComposeRows = (layout.totalRows < maxComposeRows) ? layout.totalRows : maxComposeRows;
  if (shownComposeRows == 0) shownComposeRows = 1;
  uint8_t skippedComposeRows = static_cast<uint8_t>(layout.totalRows - shownComposeRows);

  // History gets whatever vertical space compose doesn't need this frame
  // (Hardware Fix #4.3 issue E: "as compose grows, history viewport must
  // shrink accordingly"); can reach 0 when compose alone fills the screen.
  uint16_t viewportLines =
      static_cast<uint16_t>(totalLines - 1 - shownComposeRows);  // -1 = fixed error-line row

  uint16_t startIdx = 0;
  uint16_t startRowSkip = 0;
  computeHistoryViewport(viewportLines, labelX, &startIdx, &startRowSkip);
  int16_t errorY = static_cast<int16_t>(contentTop + viewportLines * lh);
  int16_t composeY = static_cast<int16_t>(errorY + lh);

  bool firstDraw = g_enigmaChatNeedsFullRedraw;
  // Any change to how many rows the history viewport has means every Y
  // coordinate below the status bar shifted, so history, error, and
  // compose all need a full repaint together (Hardware Fix #4.3 issue E).
  bool historyLayoutChanged = firstDraw || (viewportLines != g_enigmaChatLastViewportLines);
  bool contentChanged = !historyLayoutChanged && wasIndexDirty;
  bool scrolled = !historyLayoutChanged && !contentChanged &&
                  (static_cast<int16_t>(startIdx) != g_enigmaChatLastStartIdx ||
                   static_cast<int16_t>(startRowSkip) != g_enigmaChatLastStartRowSkip);
  bool selectionOnlyChanged = !historyLayoutChanged && !contentChanged && !scrolled &&
                              (g_historyCursor != g_enigmaChatLastCursor ||
                               g_historyRowOffset != g_enigmaChatLastRowOffset);

  if (historyLayoutChanged || contentChanged || scrolled) {
    if (historyLayoutChanged) {
      Display::clearContentArea();
    } else {
      int16_t regionH = static_cast<int16_t>(errorY - contentTop);
      if (regionH < 0) regionH = 0;
      Display::tft().fillRect(0, contentTop, Display::kScreenWidth, regionH, ST77XX_BLACK);
    }
    // Hardware Fix #4.3a issue 1: streams each message's rows on demand via
    // Display::wrapLineAt() instead of a precomputed fixed-size wrap-span
    // array, and the FIRST message drawn can start mid-message at
    // startRowSkip (icon+sender then only appear if its true row 0, rowIdx
    // 0, is actually the row being drawn) -- so a single logical message
    // taller than the whole viewport still has every one of its rows
    // reachable as the viewport scrolls, instead of being stuck showing
    // only its first few rows forever.
    int16_t y = contentTop;
    uint16_t rowsDrawn = 0;
    for (uint16_t i = startIdx; i < g_indexTotal && rowsDrawn < viewportLines; i++) {
      HistoryRowInfo info;
      loadHistoryRow(i, labelX, &info);
      uint16_t rowSkip = (i == startIdx) ? startRowSkip : 0;
      size_t len = strlen(info.lineBuf);
      size_t pos = 0;
      uint16_t rowIdx = 0;
      while (rowsDrawn < viewportLines) {
        int16_t rowWidth = (rowIdx == 0) ? info.firstBodyWidth : info.continuationWidth;
        uint16_t s = 0, l = 0;
        bool has = (len == 0) ? (rowIdx == 0) : Display::wrapLineAt(info.lineBuf, pos, rowWidth, &s, &l);
        if (!has) break;
        if (rowIdx >= rowSkip) {
          int16_t rowBodyX = (rowIdx == 0) ? info.firstBodyX : info.continuationX;
          if (rowIdx == 0) {
            // Icon cell + CYAN sender name appear on the message's true
            // FIRST row only; continuation rows start right after the
            // compact cursor cell, with no icon/sender indentation
            // (Hardware Fix #4 issues 2/5, extended for issue E/4.3a,
            // Hardware Fix #4.7 Part E).
            drawRowIcon(labelX, y, info.icon);
            int16_t textX = static_cast<int16_t>(labelX + Display::kLockIconCellWidth);
            Display::printLineColored(textX, y, info.senderPrefix, ST77XX_CYAN);
          }
          // The marker sits on the exact focused row (g_historyRowOffset),
          // which computeHistoryViewport() always keeps inside the drawn
          // range for the selected message -- it may be a continuation row
          // when that message's true row 0 has been scrolled off.
          if (i == g_historyCursor && rowIdx == g_historyRowOffset) Display::drawSelectionCursor(2, y);
          // Hardware Fix #4.4 issue F: Display::wrapLineAt() guarantees
          // l <= kPrintLineMaxChars for any span it returns, so this clamp
          // can never actually trigger -- kept only as defense-in-depth,
          // tied to the shared constant rather than a magic 64.
          char lineChunk[Display::kPrintLineBufferSize];
          size_t clen = l;
          if (clen > Display::kPrintLineMaxChars) clen = Display::kPrintLineMaxChars;
          memcpy(lineChunk, info.lineBuf + s, clen);
          lineChunk[clen] = '\0';
          Display::printLine(rowBodyX, y, lineChunk);
          y = static_cast<int16_t>(y + lh);
          rowsDrawn++;
        }
        if (len == 0) break;
        pos = static_cast<size_t>(s) + l;
        rowIdx++;
      }
    }
    g_enigmaChatNeedsFullRedraw = false;
  } else if (selectionOnlyChanged && viewportLines > 0) {
    // Hardware Fix #4.3a issue 1 audit: guarded on viewportLines > 0 so a
    // focused message never gets its cursor marker drawn into the
    // error/compose region below just because the history viewport
    // currently has zero rows (nothing is visible to mark in that case).
    if (g_enigmaChatLastCursor != kNoHistoryCursor) {
      int16_t oldY = historyRowY(startIdx, startRowSkip, g_enigmaChatLastCursor, g_enigmaChatLastRowOffset, labelX,
                                 contentTop, lh);
      Display::tft().fillRect(2, oldY, Display::kCursorCellWidth, lh, ST77XX_BLACK);
    }
    if (g_historyCursor != kNoHistoryCursor) {
      int16_t newY = historyRowY(startIdx, startRowSkip, g_historyCursor, g_historyRowOffset, labelX, contentTop, lh);
      Display::tft().fillRect(2, newY, Display::kCursorCellWidth, lh, ST77XX_BLACK);
      Display::drawSelectionCursor(2, newY);
    }
  }

  // Error and compose rows are diffed independently of the history rows
  // above them (Hardware Fix #3, Section F/G); historyLayoutChanged forces
  // a full redraw of both since clearContentArea() already wiped them.
  char errorLine[40];
  if (g_composeError != nullptr) {
    snprintf(errorLine, sizeof(errorLine), "%s", g_composeError);
  } else {
    errorLine[0] = '\0';
  }
  if (historyLayoutChanged || strcmp(errorLine, g_enigmaChatLastErrorLine) != 0) {
    if (!historyLayoutChanged) Display::tft().fillRect(0, errorY, Display::kScreenWidth, lh, ST77XX_BLACK);
    if (errorLine[0] != '\0') Display::printLine(2, errorY, errorLine);
    strncpy(g_enigmaChatLastErrorLine, errorLine, sizeof(g_enigmaChatLastErrorLine) - 1);
    g_enigmaChatLastErrorLine[sizeof(g_enigmaChatLastErrorLine) - 1] = '\0';
  }

  bool composeFocusChanged = (composeFocused != g_enigmaChatLastComposeFocused);
  // A change in which rows are visible (row count via historyLayoutChanged,
  // or the visible window sliding while compose is already overflowing)
  // moves every compose row's Y/content, so redraw the whole block; only
  // then is a stable per-row text diff (below) valid.
  bool composeWindowChanged = (skippedComposeRows != g_enigmaChatLastComposeSkipped);
  bool composeBlockChanged = historyLayoutChanged || composeWindowChanged;

  if (composeBlockChanged) {
    if (!historyLayoutChanged) {
      int16_t blockH = static_cast<int16_t>(shownComposeRows * lh);
      Display::tft().fillRect(0, composeY, Display::kScreenWidth, blockH, ST77XX_BLACK);
    }
    for (uint16_t shownRow = 0; shownRow < shownComposeRows; shownRow++) {
      uint8_t rowIdx = static_cast<uint8_t>(skippedComposeRows + shownRow);
      char rowText[64];
      composeRowText(layout, composePrefixBuf, rowIdx, rowText, sizeof(rowText));
      int16_t y = static_cast<int16_t>(composeY + shownRow * lh);
      // Hardware Fix #4.7 Part F: the cursor belongs to LOGICAL compose row
      // 0 (rowIdx == 0), never merely the first VISIBLE row (shownRow == 0)
      // -- when skippedComposeRows > 0, logical row 0 has scrolled off and
      // no shown row gets a cursor at all.
      if (rowIdx == 0 && composeFocused) Display::drawSelectionCursor(2, y);
      Display::printLine(composePrefixX, y, rowText);
      strncpy(g_enigmaChatLastComposeRowText[shownRow], rowText, sizeof(g_enigmaChatLastComposeRowText[shownRow]) - 1);
      g_enigmaChatLastComposeRowText[shownRow][sizeof(g_enigmaChatLastComposeRowText[shownRow]) - 1] = '\0';
    }
  } else {
    for (uint16_t shownRow = 0; shownRow < shownComposeRows; shownRow++) {
      uint8_t rowIdx = static_cast<uint8_t>(skippedComposeRows + shownRow);
      char rowText[64];
      composeRowText(layout, composePrefixBuf, rowIdx, rowText, sizeof(rowText));
      int16_t y = static_cast<int16_t>(composeY + shownRow * lh);
      bool textChanged = strcmp(rowText, g_enigmaChatLastComposeRowText[shownRow]) != 0;
      bool markerNeedsRedraw = (rowIdx == 0) && composeFocusChanged;
      if (textChanged) {
        Display::tft().fillRect(0, y, Display::kScreenWidth, lh, ST77XX_BLACK);
        if (rowIdx == 0 && composeFocused) Display::drawSelectionCursor(2, y);
        Display::printLine(composePrefixX, y, rowText);
        strncpy(g_enigmaChatLastComposeRowText[shownRow], rowText, sizeof(g_enigmaChatLastComposeRowText[shownRow]) - 1);
        g_enigmaChatLastComposeRowText[shownRow][sizeof(g_enigmaChatLastComposeRowText[shownRow]) - 1] = '\0';
      } else if (markerNeedsRedraw) {
        Display::tft().fillRect(2, y, Display::kCursorCellWidth, lh, ST77XX_BLACK);
        if (composeFocused) Display::drawSelectionCursor(2, y);
      }
    }
  }

  g_enigmaChatLastComposeFocused = composeFocused;
  g_enigmaChatLastComposeSkipped = skippedComposeRows;
  g_enigmaChatLastViewportLines = viewportLines;
  g_enigmaChatLastStartIdx = static_cast<int16_t>(startIdx);
  g_enigmaChatLastStartRowSkip = static_cast<int16_t>(startRowSkip);
  g_enigmaChatLastCursor = g_historyCursor;
  g_enigmaChatLastRowOffset = g_historyRowOffset;
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

  // Hardware Fix #4 issue 5: no user-visible [B]/[W]/[U] text tokens -- the
  // lock state is conveyed by drawRowIcon()'s icon+color instead. An
  // unlocked message's plaintext is already known locally, so it is shown
  // directly (not hidden behind the hold gesture); BLACK/WHITE keep
  // showing ciphertext by default, unchanged from before.
  if (lockState == LOCK_UNLOCKED) {
    snprintf(outBuffer, outBufferSize, "%s", cachedText);
    return;
  }
  snprintf(outBuffer, outBufferSize, "%s", ciphertext);
}

MessageIconKind enigmaMessageIcon(const StoredMessageView& msg) {
  uint8_t lockState;
  char cachedText[251];
  decodeLocalPayload(msg.localPayload, msg.localPayloadLen, &lockState, cachedText, sizeof(cachedText));
  if (lockState == LOCK_UNLOCKED) return MessageIconKind::LOCK_OPEN_GREEN;
  if (lockState == LOCK_WHITE) return MessageIconKind::LOCK_CLOSED_YELLOW;
  return MessageIconKind::LOCK_CLOSED_RED;  // LOCK_BLACK
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

// Storage::init() runs from setup() after every global constructor has
// already run, so EnigmaKeys::init()'s NVS read must happen in an
// AppService.init callback (invoked by initRegisteredServices(), also from
// setup(), after Storage::init()) rather than directly in this constructor.
void serviceInit() { EnigmaKeys::init(); }

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = nullptr;
    registerAppService(svc);
    registerModeHandler(Modes::ENIGMA, screenEnigmaEntry);
    registerMessageType(PacketCodec::MSG_TYPE_ENIGMA, renderEnigmaMessage, onEnigmaMessageEvent, enigmaMessageIcon);
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
