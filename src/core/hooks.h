#pragma once
// Fixed-capacity extension registries (Technical Architecture Addendum FINAL v3, section 3).
//
// All tables here are statically zero-initialized POD arrays so that
// cross-translation-unit static-initialization order never matters
// (Addendum 3.12). Later phases register into these tables from their own
// .cpp files via small static auto-registrar helper objects; this file and
// hooks.cpp are never edited again once a registry's shape is fixed.

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Shared screen-handler type.
//
// A ScreenHandlerFn is the non-blocking per-frame update function for a
// screen. main.cpp's loop() calls tickRegisteredServices() and then the
// current active screen's ScreenHandlerFn once per iteration (see menu.h).
// The function must return quickly (poll input non-blocking, update local
// state, redraw if needed) — it must never block waiting for input.
// ---------------------------------------------------------------------------
using ScreenHandlerFn = void (*)();

// =============================================================================
// 3.1 Before-sleep hook
// =============================================================================
using BeforeSleepHook = void (*)();

static const uint8_t kMaxBeforeSleepHooks = 8;

bool registerBeforeSleepHook(BeforeSleepHook fn);
void runBeforeSleepHooks();

// =============================================================================
// 3.2 Settings-change hook
// =============================================================================
enum SettingsChangeEvent : uint8_t {
  SET_WIFI_CHANGED,
  SET_GROUP_ADDED,
  SET_GROUP_DELETED,
  SET_GROUP_NAME_CHANGED,
  SET_MY_NAME_CHANGED,
  SET_MUTE_RADIO_CHANGED
};

struct SettingsChangeInfo {
  SettingsChangeEvent event;
  uint8_t index;
  char group_code[33];  // populated for group events; old Code is preserved for DELETE
};

using SettingsChangeHook = void (*)(const SettingsChangeInfo& info);

static const uint8_t kMaxSettingsChangeHooks = 8;

bool registerSettingsChangeHook(SettingsChangeHook fn);
void fireSettingsChangeHooks(const SettingsChangeInfo& info);

// =============================================================================
// 3.3 Unified Thread message renderer + event dispatcher
//
// StoredMessageView/MessageRef are owned by Phase 2's storage engine. Phase 1
// only needs the registry to exist and compile so the generic chat screen
// (built in Phase 2) never hardcodes behavior by message type.
// =============================================================================
struct StoredMessageView;
struct MessageRef;

enum MessageEventType : uint8_t {
  EVT_ENCODER_SHORT,
  EVT_DOT_HOLD_START,
  EVT_DOT_HOLD_END,
  EVT_COMBINED_REVEAL
};

using RenderFn = void (*)(const StoredMessageView& msg, char* outBuffer, size_t outBufferSize);
using MessageEventFn = void (*)(const MessageRef& ref, MessageEventType eventType);

static const uint8_t kMaxMessageTypes = 8;

bool registerMessageType(uint8_t type, RenderFn renderFn, MessageEventFn eventFn);
RenderFn getMessageRenderFn(uint8_t type);
MessageEventFn getMessageEventFn(uint8_t type);

// =============================================================================
// 3.4 Empty compose-line hook
// =============================================================================
using EmptyLineActionFn = void (*)(uint8_t mode, const char* group_code, const char* contact_key);

void registerEmptyLineAction(EmptyLineActionFn fn);
void invokeEmptyLineAction(uint8_t mode, const char* group_code, const char* contact_key);

// =============================================================================
// 3.5 Deep-link navigation
// =============================================================================
void navigateToChatDirect(uint8_t mode, const char* group_code, const char* contact_key);

// Radio does not exist until Phase 4; earlier phases may store a pending
// navigation intent for it instead of navigating immediately.
struct PendingRadioIntent {
  bool valid;
  char group_code[33];
  char contact_key[17];
};

void setPendingRadioIntent(const char* group_code, const char* contact_key);
const PendingRadioIntent& getPendingRadioIntent();
void clearPendingRadioIntent();

// =============================================================================
// 3.7 Mode handler registry
// =============================================================================
static const uint8_t kMaxModes = 8;

bool registerModeHandler(uint8_t mode, ScreenHandlerFn fn);
ScreenHandlerFn getModeHandler(uint8_t mode);

// =============================================================================
// 3.8 Number Guessing sub-mode registry
// =============================================================================
static const uint8_t kMaxGameSubModes = 8;

bool registerGameSubModeHandler(uint8_t submode, ScreenHandlerFn fn);
ScreenHandlerFn getGameSubModeHandler(uint8_t submode);

// =============================================================================
// 3.9 Dynamic-list registration without heap growth
// =============================================================================
struct SettingItem {
  const char* label;
  ScreenHandlerFn onSelect;
};

static const uint8_t kMaxSettingListItems = 16;
static const uint8_t kMaxSettingLists = 8;

bool registerSettingItem(uint8_t listId, const SettingItem& item);
uint8_t getSettingItemCount(uint8_t listId);
const SettingItem* getSettingItem(uint8_t listId, uint8_t index);

// =============================================================================
// 3.10 Connectivity status provider
// =============================================================================
enum ConnectivityStatus : uint8_t {
  CONN_OFFLINE = 0,
  CONN_WIFI_ONLY = 1,
  CONN_ONLINE = 2
};

using ConnectivityStatusFn = uint8_t (*)();

void registerConnectivityStatusProvider(ConnectivityStatusFn fn);
uint8_t getConnectivityStatus();

// =============================================================================
// 3.11 Network packet-kind handler registry
//
// Declared now per the Addendum so Phase 2 does not need to redesign this
// header; unused until Phase 2 implements the MQTT receive loop.
// =============================================================================
using NetworkPacketHandlerFn = void (*)(const char* group_code,
                                         const char* topic,
                                         const uint8_t* payload,
                                         size_t payloadLen);

static const uint8_t kMaxPacketKinds = 32;

bool registerNetworkPacketHandler(uint8_t packetKind, NetworkPacketHandlerFn fn);
NetworkPacketHandlerFn getNetworkPacketHandler(uint8_t packetKind);

// =============================================================================
// 3.12 Background App Service Registry
// =============================================================================
using ServiceInitFn = void (*)();
using ServiceTickFn = void (*)();

struct AppService {
  ServiceInitFn init;
  ServiceTickFn tick;
};

static const uint8_t kMaxAppServices = 16;

bool registerAppService(const AppService& service);
void initRegisteredServices();
void tickRegisteredServices();
