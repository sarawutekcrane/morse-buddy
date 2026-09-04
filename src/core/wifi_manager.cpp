#include "core/wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "core/hooks.h"
#include "core/settings.h"

namespace {

constexpr uint32_t kPerSlotTimeoutMs = 15000;
constexpr uint32_t kRetryIntervalMs = 60000;
constexpr uint32_t kNtpRetryIntervalMs = 60000;
constexpr time_t kNtpSyncedThreshold = 1600000000;  // ~Sep 2020; well past any un-synced clock value

enum class State : uint8_t { CONNECTING, WAIT_RETRY };
State g_state = State::WAIT_RETRY;
uint8_t g_slotIndex = 0;
uint8_t g_slotsAttemptedThisCycle = 0;
uint32_t g_stateStartMs = 0;

bool g_ntpStarted = false;
uint32_t g_lastNtpAttemptMs = 0;

void tryNextSlotOrGiveUp() {
  for (; g_slotsAttemptedThisCycle < Settings::kMaxWifiSlots;) {
    Settings::WifiSlot s = Settings::getWifiSlot(g_slotIndex);
    g_slotIndex = static_cast<uint8_t>((g_slotIndex + 1) % Settings::kMaxWifiSlots);
    g_slotsAttemptedThisCycle++;
    if (s.configured) {
      WiFi.begin(s.ssid, s.password);
      g_stateStartMs = millis();
      g_state = State::CONNECTING;
      return;
    }
  }
  // No configured slot connected within this cycle (or none configured at all).
  g_state = State::WAIT_RETRY;
  g_stateStartMs = millis();
}

void startCycle() {
  g_slotsAttemptedThisCycle = 0;
  tryNextSlotOrGiveUp();
}

void startNtpIfNeeded() {
  uint32_t now = millis();
  if (WifiManager::isNtpSynced()) return;
  if (g_ntpStarted && (now - g_lastNtpAttemptMs) < kNtpRetryIntervalMs) return;
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  g_ntpStarted = true;
  g_lastNtpAttemptMs = now;
}

void serviceInit() { startCycle(); }

void serviceTick() {
  uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    startNtpIfNeeded();
    return;
  }

  if (g_state == State::CONNECTING) {
    if (now - g_stateStartMs >= kPerSlotTimeoutMs) tryNextSlotOrGiveUp();
  } else {
    if (now - g_stateStartMs >= kRetryIntervalMs) startCycle();
  }
}

struct Registrar {
  Registrar() {
    AppService svc;
    svc.init = serviceInit;
    svc.tick = serviceTick;
    registerAppService(svc);
  }
};
Registrar g_registrar;

}  // namespace

namespace WifiManager {

bool isConnected() { return WiFi.status() == WL_CONNECTED; }

bool isNtpSynced() { return time(nullptr) > kNtpSyncedThreshold; }

uint32_t getUnixTime() {
  if (!isNtpSynced()) return 0;
  return static_cast<uint32_t>(time(nullptr));
}

}  // namespace WifiManager
