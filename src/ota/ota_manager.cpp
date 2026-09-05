#include "ota/ota_manager.h"

#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <string.h>

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#include "core/display.h"
#include "core/hooks.h"
#include "core/input.h"
#include "core/menu.h"
#include "core/mqtt_manager.h"
#include "core/power.h"
#include "core/radio_transport.h"
#include "core/sleep.h"
#include "core/sound_facade.h"
#include "core/storage_init.h"
#include "core/wifi_manager.h"
#include "ota/firmware_version.h"
#include "ota/ota_config.h"
#include "ota/ota_https.h"
#include "ota/ota_manifest.h"

// =============================================================================
// Arduino Core 2.0.17 rollback lifecycle override (Phase 5 corrected
// Section I). initArduino() (cores/esp32/esp32-hal-misc.c) contains:
//
//   #ifdef CONFIG_APP_ROLLBACK_ENABLE
//     if (!verifyRollbackLater()) {
//       ...esp_ota_get_state_partition... if PENDING_VERIFY:
//         verifyOta() ? esp_ota_mark_app_valid_cancel_rollback()
//                     : esp_ota_mark_app_invalid_rollback_and_reboot();
//     }
//   #endif
//
// with verifyRollbackLater() weak-defaulting to false and verifyOta()
// weak-defaulting to true -- i.e. by default Arduino auto-validates a
// freshly-flashed image before our own AppService::init ever runs. This
// override makes that whole block a no-op (by returning true so
// !verifyRollbackLater() is false) so our own runPostOtaValidation()
// below is the only thing that ever marks an image valid/invalid.
extern "C" bool verifyRollbackLater() { return true; }

namespace Ota {

namespace {

// Whether the actual pinned toolchain/bootloader this file was compiled
// against defines CONFIG_APP_ROLLBACK_ENABLE (Phase 5 section 33: verify,
// never assume). This is a real compile-time fact about the framework
// bundled sdkconfig, not a guess -- if a future toolchain swap disables
// bootloader rollback, this flips to false automatically and every branch
// below that depends on it is skipped, without this file changing.
#ifdef CONFIG_APP_ROLLBACK_ENABLE
constexpr bool kBootloaderRollbackCompiledIn = true;
#else
constexpr bool kBootloaderRollbackCompiledIn = false;
#endif

// =============================================================================
// Anti-loop OTA recovery metadata (locked Phase 5 requirement). All four
// keys live in Storage::core() (already opened for the firmware's
// lifetime) alongside every other scalar Settings value, same
// isKey()-guarded-read / put-write convention as settings.cpp.
// =============================================================================
constexpr const char* kKeyPending = "otaPending";
constexpr const char* kKeyPrevBuild = "otaPrevBuild";
constexpr const char* kKeyTargetBuild = "otaTgtBuild";
constexpr const char* kKeyLastFailBuild = "otaLastFail";

bool nvsGetPending() {
  return Storage::core().isKey(kKeyPending) ? Storage::core().getBool(kKeyPending) : false;
}
void nvsSetPending(bool v) { Storage::core().putBool(kKeyPending, v); }

uint32_t nvsGetPrevBuild() {
  return Storage::core().isKey(kKeyPrevBuild) ? Storage::core().getUInt(kKeyPrevBuild) : 0;
}
void nvsSetPrevBuild(uint32_t v) { Storage::core().putUInt(kKeyPrevBuild, v); }

uint32_t nvsGetTargetBuild() {
  return Storage::core().isKey(kKeyTargetBuild) ? Storage::core().getUInt(kKeyTargetBuild) : 0;
}
void nvsSetTargetBuild(uint32_t v) { Storage::core().putUInt(kKeyTargetBuild, v); }

// Defaults to 0 / none (locked requirement 2).
uint32_t nvsGetLastFailBuild() {
  return Storage::core().isKey(kKeyLastFailBuild) ? Storage::core().getUInt(kKeyLastFailBuild) : 0;
}
void nvsSetLastFailBuild(uint32_t v) { Storage::core().putUInt(kKeyLastFailBuild, v); }

// Lightweight application-level health check (Phase 5 section 34): only
// verifies firmware-internal integrity, never external services. Reaching
// this line at all (inside AppService::init, called from
// initRegisteredServices() after Storage::init()/Settings::init() already
// succeeded in main.cpp's setup()) is itself strong evidence the boot
// sequence got this far without hitting Storage::init()'s own blocking
// failure/format prompt; the checks below are the additional concrete
// signals section 34 asks for. "slots" is the WiFi-credential key any
// device that could possibly have reached an OTA update at all must have
// written at some point (OTA requires a working WiFi connection).
bool runHealthCheck() {
  bool nvsOk = Storage::wifi().isKey("slots");
  bool fsOk = LittleFS.totalBytes() > 0;
  bool heapOk = ESP.getFreeHeap() > 10 * 1024;
  Serial.printf("[ota] health check: nvs=%d fs=%d heap=%d (%u bytes free)\n", nvsOk, fsOk, heapOk,
               static_cast<unsigned>(ESP.getFreeHeap()));
  return nvsOk && fsOk && heapOk;
}

// Runs once at boot (called from AppService::init). Implements the
// corrected Section I early-boot sequence plus the locked anti-loop
// requirements 4-7, PLUS (Phase 5 audit turn) explicit handling for
// interrupted/inconsistent metadata: update_pending/previous_build/
// target_build are written in that exact order with update_pending last
// (see runDownloadAndInstallBlocking()), so any power loss between
// Update.end() committing the new boot partition and our own NVS writes
// finishing can only ever leave update_pending==false -- never true with
// a stale/mismatched target_build. That already prevents an incorrect
// automatic-rollback trigger from a torn write. What it can leave behind
// is esp_ota itself reporting PENDING_VERIFY for a build our own
// bookkeeping never actually recorded (or recorded differently) -- that
// case is handled explicitly below by resolving PENDING_VERIFY to VALID
// and continuing on the currently-running firmware, and deliberately
// never invalidating/rolling back on ambiguous data, since doing so is
// exactly how two slots could end up alternating rollbacks forever.
void runPostOtaValidation() {
  bool pending = nvsGetPending();
  uint32_t targetBuild = nvsGetTargetBuild();
  uint32_t prevBuild = nvsGetPrevBuild();
  uint32_t lastFailBuild = nvsGetLastFailBuild();

  // Requirement 7: automatic-rollback recovery completion. This does NOT
  // depend on esp_ota's own PENDING_VERIFY state -- the previous-good
  // build was already marked valid the last time *it* was the freshly
  // installed build, so by the time we're back here its partition state
  // is plain VALID, not PENDING_VERIFY. Our own bookkeeping is the only
  // signal that this boot is a rollback-recovery boot at all. Requires
  // pending==true so an unrelated coincidental build-number match can
  // never be misread as a recovery event.
  if (pending && FW_BUILD_NUMBER == prevBuild && lastFailBuild != 0 && lastFailBuild == targetBuild) {
    nvsSetPending(false);
    // last_failed_ota_build is deliberately retained (requirement 7) so
    // the Firmware Update screen can still warn if the server offers that
    // same failed build again (requirement 9).
    Serial.printf("[ota] automatic rollback recovery succeeded: back on build %u after build %u failed its health check\n",
                 static_cast<unsigned>(FW_BUILD_NUMBER), static_cast<unsigned>(lastFailBuild));
    return;  // normal startup continues; never perform another rollback here
  }

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  bool pendingVerify =
      (esp_ota_get_state_partition(running, &state) == ESP_OK) && (state == ESP_OTA_IMG_PENDING_VERIFY);

  if (!pendingVerify) {
    // Not a post-OTA boot from esp_ota's own point of view (the normal
    // case for every ordinary boot). If our own bookkeeping still thinks
    // one is in flight -- stale from an interrupted write, a manual
    // rollback, or a USB reflash outside the OTA path entirely -- clear
    // it now rather than letting it misfire on some later boot. Never
    // triggers rollback: requirement 8 ("a failed build must never cause
    // an automatic reinstall") applies equally to never taking an
    // automatic *action* off data we can't corroborate.
    if (pending) {
      Serial.println("[ota] clearing stale update_pending: esp_ota reports this partition is not PENDING_VERIFY");
      nvsSetPending(false);
    }
    return;
  }

  // esp_ota independently confirms this partition is pending its
  // first-boot verification. Requirement 4 only wants the real health
  // check to run when our own bookkeeping exactly corroborates that --
  // this running build must equal the target we ourselves recorded
  // writing. When it does NOT (update_pending was never durably set
  // before a power loss, or names a different build than the one
  // actually running), the safe resolution is to mark the currently-
  // running firmware valid -- it has, after all, already reached
  // AppService::init, which is itself real evidence of a working boot --
  // and continue with it, rather than leave PENDING_VERIFY unresolved
  // forever or guess at an automatic rollback with no reliable metadata
  // to justify it.
  if (!(pending && FW_BUILD_NUMBER == targetBuild)) {
    Serial.printf(
        "[ota] inconsistent OTA metadata at a PENDING_VERIFY boot (pending=%d target_build=%u running_build=%u) "
        "-- marking this build valid without an automatic-rollback decision and continuing\n",
        pending, static_cast<unsigned>(targetBuild), static_cast<unsigned>(FW_BUILD_NUMBER));
    nvsSetPending(false);
    if (kBootloaderRollbackCompiledIn) esp_ota_mark_app_valid_cancel_rollback();
    return;
  }

  // Exact match: this is genuinely the build we ourselves just installed.
  bool healthy = runHealthCheck();
  if (healthy) {
    nvsSetPending(false);
    // Requirement 5: do not set last_failed_ota_build on PASS.
    if (kBootloaderRollbackCompiledIn) esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[ota] post-update health check passed; build marked valid");
  } else {
    // Requirement 6: persist last_failed_ota_build BEFORE triggering rollback.
    nvsSetLastFailBuild(targetBuild);
    Serial.println("[ota] post-update health check FAILED");
    if (kBootloaderRollbackCompiledIn) {
      esp_ota_mark_app_invalid_rollback_and_reboot();  // does not return on success
      Serial.println("[ota] rollback call returned unexpectedly; restarting manually");
      ESP.restart();
    } else {
      Serial.println("[ota] bootloader rollback not compiled in -- manual rollback from Settings required");
    }
  }
}

// =============================================================================
// OTA Maintenance Mode (Phase 5 sections 19-20). Narrow, additive calls
// into sleep.h/mqtt_manager.h/radio_transport.h -- none of those modules
// are restructured, each just gained one new setMaintenanceModeActive-
// shaped entry point.
// =============================================================================
bool g_maintenanceActive = false;

void enterMaintenanceMode() {
  if (g_maintenanceActive) return;
  g_maintenanceActive = true;
  Sleep::setSuspended(true);
  RadioTransport::setMaintenanceModeActive(true);
  stopCurrentToneSound();
  MqttManager::setMaintenanceModeActive(true);  // best-effort OFFLINE publish, then disconnect
}

void leaveMaintenanceMode() {
  if (!g_maintenanceActive) return;
  g_maintenanceActive = false;
  MqttManager::setMaintenanceModeActive(false);
  RadioTransport::setMaintenanceModeActive(false);
  Sleep::setSuspended(false);
}

// =============================================================================
// Firmware Update screen state machine.
// =============================================================================
enum class UiState : uint8_t {
  MAIN,
  CHECKING,
  UP_TO_DATE,
  UPDATE_AVAILABLE,
  UPDATE_AVAILABLE_PREV_FAILED,
  WIFI_REQUIRED,
  TIME_REQUIRED,
  SERVER_ERROR,
  LOW_BATTERY,
  LOW_HEAP,
  PREPARING,
  DOWNLOADING,
  SUCCESS,
  FAILED,
  CONFIRM_ROLLBACK,
  ROLLBACK_FAILED,
  ROLLING_BACK,
};

UiState g_uiState = UiState::MAIN;
OtaManifest::Manifest g_lastManifest;
char g_errorMsg[48] = {0};
bool g_failedWasWifiLoss = false;

uint8_t g_mainSelected = 0;
bool g_mainCanRollback = false;

uint8_t g_promptSelected = 1;  // 0 = optionA, 1 = optionB (default Cancel/Back)

bool g_checkingDrawn = false;
bool g_preparingDrawn = false;

void drawLine(int16_t y, const char* text) {
  Adafruit_ST7789& tft = Display::tft();
  tft.setCursor(2, y);
  tft.print(text);
}

void drawTwoOptionPrompt(const char* line1, const char* line2, const char* labelA, const char* labelB,
                         uint8_t selected) {
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  drawLine(Display::kStatusBarHeight + 4, line1);
  if (line2 != nullptr) drawLine(Display::kStatusBarHeight + 16, line2);
  tft.setCursor(2, Display::kStatusBarHeight + 40);
  tft.print(selected == 0 ? "> " : "  ");
  tft.print(labelA);
  tft.setCursor(2, Display::kStatusBarHeight + 52);
  tft.print(selected == 1 ? "> " : "  ");
  tft.print(labelB);
}

void drawMessage(const char* line1, const char* line2) {
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  drawLine(Display::kStatusBarHeight + 4, line1);
  if (line2 != nullptr) drawLine(Display::kStatusBarHeight + 16, line2);
}

void resetToMain() {
  g_uiState = UiState::MAIN;
  g_mainCanRollback = Update.canRollBack();
  g_mainSelected = 0;
}

// ---- Check for Update ------------------------------------------------------
void runCheckForUpdateBlocking() {
  if (!WifiManager::isConnected()) {
    g_uiState = UiState::WIFI_REQUIRED;
    return;
  }
  if (!WifiManager::isNtpSynced()) {
    g_uiState = UiState::TIME_REQUIRED;
    return;
  }
  if (OtaConfig::isPlaceholderServer()) {
    snprintf(g_errorMsg, sizeof(g_errorMsg), "Update Server Not Configured");
    g_uiState = UiState::SERVER_ERROR;
    return;
  }

  OtaHttps::Result r = OtaHttps::fetchManifest(&g_lastManifest);
  if (r != OtaHttps::Result::OK) {
    snprintf(g_errorMsg, sizeof(g_errorMsg), "%s", OtaHttps::resultToString(r));
    g_uiState = UiState::SERVER_ERROR;
    return;
  }

  if (strcmp(g_lastManifest.hardware, OtaConfig::kHardwareId) != 0) {
    snprintf(g_errorMsg, sizeof(g_errorMsg), "Wrong hardware");
    g_uiState = UiState::SERVER_ERROR;
    return;
  }

  if (g_lastManifest.build <= FW_BUILD_NUMBER) {
    g_uiState = UiState::UP_TO_DATE;
    return;
  }

  uint32_t lastFail = nvsGetLastFailBuild();
  g_promptSelected = 1;  // default Cancel, every time this screen is (re)entered
  g_uiState =
      (lastFail != 0 && g_lastManifest.build == lastFail) ? UiState::UPDATE_AVAILABLE_PREV_FAILED : UiState::UPDATE_AVAILABLE;
}

// ---- Install flow -----------------------------------------------------------
void otaProgressCallback(size_t written, size_t expected) {
  uint8_t percent = expected > 0 ? static_cast<uint8_t>((written * 100) / expected) : 0;
  Display::clearContentArea();
  Adafruit_ST7789& tft = Display::tft();
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  drawLine(Display::kStatusBarHeight + 4, "Updating Firmware");
  tft.setCursor(2, Display::kStatusBarHeight + 24);
  tft.print(percent);
  tft.print("%");
}

// Deliberately blocking (see runDownloadAndInstallBlocking()'s comment):
// screens in this codebase are otherwise strictly non-blocking-per-tick,
// but Phase 5 section 25 explicitly requires the device to be
// unresponsive to Encoder/DOT/DASH input for the whole duration of an
// active flash write ("Once flash writing begins, do not provide an
// ordinary Cancel button"), and Sleep is already suspended by
// enterMaintenanceMode(). Chunking a TLS+Update.write() stream across
// non-blocking per-frame ticks would need a much larger custom
// socket-state-machine than Phase 5's scope justifies. The Task Watchdog
// is fed via yield() inside OtaHttps::downloadAndInstall()'s own chunk
// loop (Phase 5 section 56.1), and progress is drawn directly from the
// ProgressFn callback since control does not return to this function's
// caller until the whole download finishes.
void runDownloadAndInstallBlocking() {
  g_uiState = UiState::DOWNLOADING;
  otaProgressCallback(0, g_lastManifest.size);

  OtaHttps::Result r = OtaHttps::downloadAndInstall(g_lastManifest, otaProgressCallback);

  if (r != OtaHttps::Result::OK) {
    leaveMaintenanceMode();
    snprintf(g_errorMsg, sizeof(g_errorMsg), "%s", OtaHttps::resultToString(r));
    g_failedWasWifiLoss = (r == OtaHttps::Result::DOWNLOAD_INTERRUPTED || r == OtaHttps::Result::DOWNLOAD_TIMEOUT);
    g_uiState = UiState::FAILED;
    return;
  }

  // Locked anti-loop requirement 3: persist recovery metadata BEFORE
  // rebooting into the newly installed image.
  nvsSetPrevBuild(FW_BUILD_NUMBER);
  nvsSetTargetBuild(g_lastManifest.build);
  nvsSetPending(true);

  g_uiState = UiState::SUCCESS;
  drawMessage("Update Complete", "Restarting...");
  delay(1200);
  ESP.restart();
}

void beginInstallFlow() {
  if (!WifiManager::isConnected()) {
    g_uiState = UiState::WIFI_REQUIRED;
    return;
  }
  if (!WifiManager::isNtpSynced()) {
    g_uiState = UiState::TIME_REQUIRED;
    return;
  }
  if (Power::getBatteryPercent() < OtaConfig::kMinBatteryPercent) {
    g_uiState = UiState::LOW_BATTERY;
    return;
  }

  // Quiesce Radio/MQTT/tones/Sleep BEFORE the heap check, per section 18
  // ("Use a provisional minimum free-heap guard... after unnecessary
  // services have been quiesced").
  enterMaintenanceMode();

  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[ota] free heap after quiescing = %u bytes (guard = %u)\n", static_cast<unsigned>(freeHeap),
               static_cast<unsigned>(OtaConfig::kMinFreeHeapBytes));
  if (freeHeap < OtaConfig::kMinFreeHeapBytes) {
    leaveMaintenanceMode();
    g_uiState = UiState::LOW_HEAP;
    return;
  }

  Serial.printf(
      "[ota] starting update: current=%s/%u available=%s/%u hardware=%s size=%u free_heap=%u battery=%u%%\n",
      FW_VERSION, static_cast<unsigned>(FW_BUILD_NUMBER), g_lastManifest.version,
      static_cast<unsigned>(g_lastManifest.build), g_lastManifest.hardware, static_cast<unsigned>(g_lastManifest.size),
      static_cast<unsigned>(freeHeap), Power::getBatteryPercent());

  g_uiState = UiState::PREPARING;
  g_preparingDrawn = false;
}

// ---- Manual rollback (corrected Section I: Update.canRollBack()/rollBack(),
// never esp_ota_get_last_invalid_partition(); locked requirement 12: this
// path never touches update_pending/previous_build/target_build/
// last_failed_ota_build -- those are exclusively the automatic
// OTA-health-check lifecycle's bookkeeping). --------------------------------
void performManualRollback() {
  if (!Update.canRollBack()) {
    g_uiState = UiState::ROLLBACK_FAILED;
    return;
  }
  if (!Update.rollBack()) {
    g_uiState = UiState::ROLLBACK_FAILED;
    return;
  }
  g_uiState = UiState::ROLLING_BACK;
  drawMessage("Restarting...", nullptr);
  delay(800);
  ESP.restart();
}

// =============================================================================
// screenFirmwareUpdate() dispatcher.
// =============================================================================
void tickTwoOption(void (*onSelectA)(), void (*onSelectB)()) {
  Input::update();
  InputEvent e;
  while (Input::popEvent(e)) {
    if (e.type == InputEventType::ENCODER_ROTATE) {
      g_promptSelected = static_cast<uint8_t>(1 - g_promptSelected);
    } else if (Input::isMenuConfirm(e)) {
      if (g_promptSelected == 0) {
        if (onSelectA != nullptr) onSelectA();
      } else {
        if (onSelectB != nullptr) onSelectB();
      }
      return;
    } else if (Input::isBack(e)) {
      if (onSelectB != nullptr) onSelectB();
      return;
    }
  }
}

void selectRetryCheck() {
  g_uiState = UiState::CHECKING;
  g_checkingDrawn = false;
}
void selectBackToMain() { resetToMain(); }
void selectUpdateConfirmed() { beginInstallFlow(); }
void selectRollbackConfirmed() { performManualRollback(); }
void selectRestartDevice() { ESP.restart(); }

// =============================================================================
// Background one-check-per-cold-boot (Phase 5 section 23): discovery only,
// never installs. Logged to Serial only -- wiring a Main-Menu-style badge
// for this was not requested anywhere in the Phase 5 spec and would need
// an unapproved additional Settings-item-badge touch, so per section 23's
// own "manual checking from Settings takes priority" clause this stays a
// log line; the user still gets the exact same information by opening
// Settings > System > Firmware Update > Check for Update.
// =============================================================================
bool g_backgroundCheckDone = false;

void runBackgroundAvailabilityCheck() {
  if (OtaConfig::isPlaceholderServer()) {
    Serial.println("[ota] background check skipped: update server not configured");
    return;
  }
  OtaManifest::Manifest m;
  OtaHttps::Result r = OtaHttps::fetchManifest(&m);
  if (r != OtaHttps::Result::OK) {
    Serial.printf("[ota] background check failed: %s\n", OtaHttps::resultToString(r));
    return;
  }
  if (m.build > FW_BUILD_NUMBER && strcmp(m.hardware, OtaConfig::kHardwareId) == 0) {
    Serial.printf("[ota] background check: firmware build %u available (running build %u)\n",
                 static_cast<unsigned>(m.build), static_cast<unsigned>(FW_BUILD_NUMBER));
  } else {
    Serial.println("[ota] background check: firmware is up to date");
  }
}

void serviceInit() { runPostOtaValidation(); }

void serviceTick() {
  if (g_backgroundCheckDone) return;
  if (!WifiManager::isConnected() || !WifiManager::isNtpSynced()) return;
  g_backgroundCheckDone = true;
  runBackgroundAvailabilityCheck();
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

void screenFirmwareUpdate() {
  if (Menu::consumeJustEntered()) resetToMain();

  switch (g_uiState) {
    case UiState::MAIN: {
      Input::update();
      InputEvent e;
      uint8_t itemCount = g_mainCanRollback ? 2 : 1;
      while (Input::popEvent(e)) {
        if (e.type == InputEventType::ENCODER_ROTATE && itemCount > 1) {
          g_mainSelected = static_cast<uint8_t>((g_mainSelected + 1) % itemCount);
        } else if (Input::isMenuConfirm(e)) {
          if (g_mainSelected == 0) {
            selectRetryCheck();
          } else {
            g_promptSelected = 1;
            g_uiState = UiState::CONFIRM_ROLLBACK;
          }
        } else if (Input::isBack(e)) {
          Menu::goBack();
          return;
        }
      }
      Display::drawStatusBar();
      Display::clearContentArea();
      Adafruit_ST7789& tft = Display::tft();
      tft.setTextSize(1);
      tft.setTextColor(ST77XX_WHITE);
      drawLine(Display::kStatusBarHeight + 2, "Firmware Update");
      char line[32];
      snprintf(line, sizeof(line), "Current: v%s", FW_VERSION);
      drawLine(Display::kStatusBarHeight + 16, line);
      snprintf(line, sizeof(line), "Build: %u", static_cast<unsigned>(FW_BUILD_NUMBER));
      drawLine(Display::kStatusBarHeight + 28, line);
      tft.setCursor(2, Display::kStatusBarHeight + 44);
      tft.print(g_mainSelected == 0 ? "> " : "  ");
      tft.print("Check for Update");
      if (g_mainCanRollback) {
        tft.setCursor(2, Display::kStatusBarHeight + 56);
        tft.print(g_mainSelected == 1 ? "> " : "  ");
        tft.print("Rollback to Previous");
      }
      return;
    }

    case UiState::CHECKING:
      if (!g_checkingDrawn) {
        drawMessage("Checking...", nullptr);
        g_checkingDrawn = true;
        return;
      }
      g_checkingDrawn = false;
      runCheckForUpdateBlocking();
      return;

    case UiState::UP_TO_DATE: {
      Input::update();
      InputEvent e;
      while (Input::popEvent(e)) {
        if (Input::isMenuConfirm(e) || Input::isBack(e)) {
          resetToMain();
          return;
        }
      }
      char line[32];
      snprintf(line, sizeof(line), "Current: v%s", FW_VERSION);
      drawMessage("Firmware is up to date", line);
      return;
    }

    case UiState::UPDATE_AVAILABLE:
    case UiState::UPDATE_AVAILABLE_PREV_FAILED: {
      char line1[40];
      char line2[40];
      snprintf(line1, sizeof(line1), "Current: v%s", FW_VERSION);
      snprintf(line2, sizeof(line2), "New: v%s", g_lastManifest.version);
      const char* labelA = (g_uiState == UiState::UPDATE_AVAILABLE_PREV_FAILED) ? "Retry Update" : "Update";
      if (g_uiState == UiState::UPDATE_AVAILABLE_PREV_FAILED) {
        Display::clearContentArea();
        Adafruit_ST7789& tft = Display::tft();
        tft.setTextSize(1);
        tft.setTextColor(ST77XX_WHITE);
        drawLine(Display::kStatusBarHeight + 2, "This firmware previously failed");
        drawLine(Display::kStatusBarHeight + 16, line1);
        drawLine(Display::kStatusBarHeight + 28, line2);
        tft.setCursor(2, Display::kStatusBarHeight + 44);
        tft.print(g_promptSelected == 0 ? "> " : "  ");
        tft.print(labelA);
        tft.setCursor(2, Display::kStatusBarHeight + 56);
        tft.print(g_promptSelected == 1 ? "> " : "  ");
        tft.print("Cancel");
        tickTwoOption(selectUpdateConfirmed, selectBackToMain);
      } else {
        drawTwoOptionPrompt(line1, line2, labelA, "Cancel", g_promptSelected);
        tickTwoOption(selectUpdateConfirmed, selectBackToMain);
      }
      return;
    }

    case UiState::WIFI_REQUIRED:
      drawTwoOptionPrompt("WiFi Required", nullptr, "Retry", "Back", g_promptSelected);
      tickTwoOption(selectRetryCheck, selectBackToMain);
      return;

    case UiState::TIME_REQUIRED:
      drawTwoOptionPrompt("Time Sync Required", nullptr, "Retry", "Back", g_promptSelected);
      tickTwoOption(selectRetryCheck, selectBackToMain);
      return;

    case UiState::SERVER_ERROR:
      drawTwoOptionPrompt(g_errorMsg, nullptr, "Retry", "Back", g_promptSelected);
      tickTwoOption(selectRetryCheck, selectBackToMain);
      return;

    case UiState::LOW_BATTERY: {
      Input::update();
      InputEvent e;
      while (Input::popEvent(e)) {
        if (Input::isMenuConfirm(e) || Input::isBack(e)) {
          resetToMain();
          return;
        }
      }
      drawMessage("Battery Too Low", "Charge before update");
      return;
    }

    case UiState::LOW_HEAP: {
      Input::update();
      InputEvent e;
      while (Input::popEvent(e)) {
        if (Input::isMenuConfirm(e) || Input::isBack(e)) {
          resetToMain();
          return;
        }
      }
      drawMessage("Update Cannot Start", "Not enough memory");
      return;
    }

    case UiState::PREPARING:
      if (!g_preparingDrawn) {
        drawMessage("Preparing Update...", nullptr);
        g_preparingDrawn = true;
        return;
      }
      runDownloadAndInstallBlocking();
      return;

    case UiState::DOWNLOADING:
      // otaProgressCallback() already drew the last known percentage;
      // runDownloadAndInstallBlocking() has not returned control to the
      // per-tick dispatcher yet by the time this case would otherwise run
      // (see that function's blocking-call justification comment).
      return;

    case UiState::SUCCESS:
      return;  // ESP.restart() already fired before control could return here

    case UiState::FAILED:
      if (g_failedWasWifiLoss) {
        drawTwoOptionPrompt("Update Failed", g_errorMsg, "Retry", "Restart", g_promptSelected);
        tickTwoOption(selectUpdateConfirmed, selectRestartDevice);
      } else {
        drawTwoOptionPrompt("Update Failed", g_errorMsg, "Retry", "Back", g_promptSelected);
        tickTwoOption(selectUpdateConfirmed, selectBackToMain);
      }
      return;

    case UiState::CONFIRM_ROLLBACK:
      drawTwoOptionPrompt("Rollback Firmware?", nullptr, "Rollback", "Cancel", g_promptSelected);
      tickTwoOption(selectRollbackConfirmed, selectBackToMain);
      return;

    case UiState::ROLLBACK_FAILED: {
      Input::update();
      InputEvent e;
      while (Input::popEvent(e)) {
        if (Input::isMenuConfirm(e) || Input::isBack(e)) {
          resetToMain();
          return;
        }
      }
      drawMessage("Update Failed", "Rollback failed");
      return;
    }

    case UiState::ROLLING_BACK:
      return;  // ESP.restart() already fired before control could return here
  }
}

}  // namespace Ota
