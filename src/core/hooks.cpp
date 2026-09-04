#include "core/hooks.h"

#include <string.h>

// =============================================================================
// 3.1 Before-sleep hook
// =============================================================================
namespace {
BeforeSleepHook g_beforeSleepHooks[kMaxBeforeSleepHooks];
uint8_t g_beforeSleepHookCount = 0;
}  // namespace

bool registerBeforeSleepHook(BeforeSleepHook fn) {
  if (fn == nullptr || g_beforeSleepHookCount >= kMaxBeforeSleepHooks) return false;
  g_beforeSleepHooks[g_beforeSleepHookCount++] = fn;
  return true;
}

void runBeforeSleepHooks() {
  for (uint8_t i = 0; i < g_beforeSleepHookCount; i++) {
    g_beforeSleepHooks[i]();
  }
}

// =============================================================================
// 3.2 Settings-change hook
// =============================================================================
namespace {
SettingsChangeHook g_settingsChangeHooks[kMaxSettingsChangeHooks];
uint8_t g_settingsChangeHookCount = 0;
}  // namespace

bool registerSettingsChangeHook(SettingsChangeHook fn) {
  if (fn == nullptr || g_settingsChangeHookCount >= kMaxSettingsChangeHooks) return false;
  g_settingsChangeHooks[g_settingsChangeHookCount++] = fn;
  return true;
}

void fireSettingsChangeHooks(const SettingsChangeInfo& info) {
  for (uint8_t i = 0; i < g_settingsChangeHookCount; i++) {
    g_settingsChangeHooks[i](info);
  }
}

// =============================================================================
// 3.3 Unified Thread message renderer + event dispatcher
// =============================================================================
namespace {
struct MessageTypeEntry {
  bool used;
  uint8_t type;
  RenderFn renderFn;
  MessageEventFn eventFn;
};
MessageTypeEntry g_messageTypes[kMaxMessageTypes];
}  // namespace

bool registerMessageType(uint8_t type, RenderFn renderFn, MessageEventFn eventFn) {
  for (uint8_t i = 0; i < kMaxMessageTypes; i++) {
    if (g_messageTypes[i].used && g_messageTypes[i].type == type) return false;  // already registered
  }
  for (uint8_t i = 0; i < kMaxMessageTypes; i++) {
    if (!g_messageTypes[i].used) {
      g_messageTypes[i] = {true, type, renderFn, eventFn};
      return true;
    }
  }
  return false;
}

RenderFn getMessageRenderFn(uint8_t type) {
  for (uint8_t i = 0; i < kMaxMessageTypes; i++) {
    if (g_messageTypes[i].used && g_messageTypes[i].type == type) return g_messageTypes[i].renderFn;
  }
  return nullptr;
}

MessageEventFn getMessageEventFn(uint8_t type) {
  for (uint8_t i = 0; i < kMaxMessageTypes; i++) {
    if (g_messageTypes[i].used && g_messageTypes[i].type == type) return g_messageTypes[i].eventFn;
  }
  return nullptr;
}

// =============================================================================
// 3.4 Empty compose-line hook
// =============================================================================
namespace {
EmptyLineActionFn g_emptyLineAction = nullptr;
}  // namespace

void registerEmptyLineAction(EmptyLineActionFn fn) {
  g_emptyLineAction = fn;
}

void invokeEmptyLineAction(uint8_t mode, const char* group_code, const char* contact_key) {
  if (g_emptyLineAction != nullptr) {
    g_emptyLineAction(mode, group_code, contact_key);
  }
}

// =============================================================================
// 3.5 Deep-link navigation
// =============================================================================
namespace {
PendingRadioIntent g_pendingRadioIntent = {false, {0}, {0}};
}  // namespace

void navigateToChatDirect(uint8_t mode, const char* group_code, const char* contact_key) {
  // Phase 1 has no chat screen yet (Phase 2 implements the Unified
  // Conversation Thread). The function exists now so later phases and the
  // Recent Contacts / notification UI can call it without a signature
  // change; until a chat screen is registered this is a documented no-op.
  (void)mode;
  (void)group_code;
  (void)contact_key;
}

void setPendingRadioIntent(const char* group_code, const char* contact_key) {
  g_pendingRadioIntent.valid = true;
  strncpy(g_pendingRadioIntent.group_code, group_code, sizeof(g_pendingRadioIntent.group_code) - 1);
  g_pendingRadioIntent.group_code[sizeof(g_pendingRadioIntent.group_code) - 1] = '\0';
  strncpy(g_pendingRadioIntent.contact_key, contact_key, sizeof(g_pendingRadioIntent.contact_key) - 1);
  g_pendingRadioIntent.contact_key[sizeof(g_pendingRadioIntent.contact_key) - 1] = '\0';
}

const PendingRadioIntent& getPendingRadioIntent() {
  return g_pendingRadioIntent;
}

void clearPendingRadioIntent() {
  g_pendingRadioIntent.valid = false;
  g_pendingRadioIntent.group_code[0] = '\0';
  g_pendingRadioIntent.contact_key[0] = '\0';
}

// =============================================================================
// 3.7 Mode handler registry
// =============================================================================
namespace {
struct ModeEntry {
  bool used;
  uint8_t mode;
  ScreenHandlerFn fn;
};
ModeEntry g_modeHandlers[kMaxModes];
}  // namespace

bool registerModeHandler(uint8_t mode, ScreenHandlerFn fn) {
  for (uint8_t i = 0; i < kMaxModes; i++) {
    if (g_modeHandlers[i].used && g_modeHandlers[i].mode == mode) return false;
  }
  for (uint8_t i = 0; i < kMaxModes; i++) {
    if (!g_modeHandlers[i].used) {
      g_modeHandlers[i] = {true, mode, fn};
      return true;
    }
  }
  return false;
}

ScreenHandlerFn getModeHandler(uint8_t mode) {
  for (uint8_t i = 0; i < kMaxModes; i++) {
    if (g_modeHandlers[i].used && g_modeHandlers[i].mode == mode) return g_modeHandlers[i].fn;
  }
  return nullptr;
}

// =============================================================================
// 3.8 Number Guessing sub-mode registry
// =============================================================================
namespace {
struct GameSubModeEntry {
  bool used;
  uint8_t submode;
  ScreenHandlerFn fn;
};
GameSubModeEntry g_gameSubModes[kMaxGameSubModes];
}  // namespace

bool registerGameSubModeHandler(uint8_t submode, ScreenHandlerFn fn) {
  for (uint8_t i = 0; i < kMaxGameSubModes; i++) {
    if (g_gameSubModes[i].used && g_gameSubModes[i].submode == submode) return false;
  }
  for (uint8_t i = 0; i < kMaxGameSubModes; i++) {
    if (!g_gameSubModes[i].used) {
      g_gameSubModes[i] = {true, submode, fn};
      return true;
    }
  }
  return false;
}

ScreenHandlerFn getGameSubModeHandler(uint8_t submode) {
  for (uint8_t i = 0; i < kMaxGameSubModes; i++) {
    if (g_gameSubModes[i].used && g_gameSubModes[i].submode == submode) return g_gameSubModes[i].fn;
  }
  return nullptr;
}

// =============================================================================
// 3.9 Dynamic-list registration without heap growth
// =============================================================================
namespace {
struct SettingList {
  uint8_t count;
  SettingItem items[kMaxSettingListItems];
};
SettingList g_settingLists[kMaxSettingLists];
}  // namespace

bool registerSettingItem(uint8_t listId, const SettingItem& item) {
  if (listId >= kMaxSettingLists) return false;
  SettingList& list = g_settingLists[listId];
  if (list.count >= kMaxSettingListItems) return false;
  list.items[list.count++] = item;
  return true;
}

uint8_t getSettingItemCount(uint8_t listId) {
  if (listId >= kMaxSettingLists) return 0;
  return g_settingLists[listId].count;
}

const SettingItem* getSettingItem(uint8_t listId, uint8_t index) {
  if (listId >= kMaxSettingLists) return nullptr;
  if (index >= g_settingLists[listId].count) return nullptr;
  return &g_settingLists[listId].items[index];
}

// =============================================================================
// 3.10 Connectivity status provider
// =============================================================================
namespace {
uint8_t defaultConnectivityStatusProvider() {
  return CONN_OFFLINE;
}
ConnectivityStatusFn g_connectivityStatusFn = defaultConnectivityStatusProvider;
}  // namespace

void registerConnectivityStatusProvider(ConnectivityStatusFn fn) {
  g_connectivityStatusFn = (fn != nullptr) ? fn : defaultConnectivityStatusProvider;
}

uint8_t getConnectivityStatus() {
  return g_connectivityStatusFn();
}

// =============================================================================
// 3.11 Network packet-kind handler registry
// =============================================================================
namespace {
struct PacketHandlerEntry {
  bool used;
  uint8_t packetKind;
  NetworkPacketHandlerFn fn;
};
PacketHandlerEntry g_packetHandlers[kMaxPacketKinds];
}  // namespace

bool registerNetworkPacketHandler(uint8_t packetKind, NetworkPacketHandlerFn fn) {
  for (uint8_t i = 0; i < kMaxPacketKinds; i++) {
    if (g_packetHandlers[i].used && g_packetHandlers[i].packetKind == packetKind) return false;
  }
  for (uint8_t i = 0; i < kMaxPacketKinds; i++) {
    if (!g_packetHandlers[i].used) {
      g_packetHandlers[i] = {true, packetKind, fn};
      return true;
    }
  }
  return false;
}

NetworkPacketHandlerFn getNetworkPacketHandler(uint8_t packetKind) {
  for (uint8_t i = 0; i < kMaxPacketKinds; i++) {
    if (g_packetHandlers[i].used && g_packetHandlers[i].packetKind == packetKind) return g_packetHandlers[i].fn;
  }
  return nullptr;
}

// =============================================================================
// 3.12 Background App Service Registry
// =============================================================================
namespace {
AppService g_appServices[kMaxAppServices];
uint8_t g_appServiceCount = 0;
}  // namespace

bool registerAppService(const AppService& service) {
  if (g_appServiceCount >= kMaxAppServices) return false;
  g_appServices[g_appServiceCount++] = service;
  return true;
}

void initRegisteredServices() {
  for (uint8_t i = 0; i < g_appServiceCount; i++) {
    if (g_appServices[i].init != nullptr) g_appServices[i].init();
  }
}

void tickRegisteredServices() {
  for (uint8_t i = 0; i < g_appServiceCount; i++) {
    if (g_appServices[i].tick != nullptr) g_appServices[i].tick();
  }
}
