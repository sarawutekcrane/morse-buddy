#include "core/storage_init.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "core/display.h"
#include "core/input.h"

namespace {

Preferences g_core;
Preferences g_wifi;
Preferences g_group;
Preferences g_recent;
Preferences g_enigma;
Preferences g_notify;
Preferences g_solo;

constexpr const char* kPartitionLabel = "littlefs";  // matches partitions.csv Name column

void drawBlockingMessage(const char* line1, const char* line2, const char* line3) {
  Adafruit_ST7789& tft = Display::tft();
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, 20);
  tft.print(line1);
  tft.setCursor(4, 40);
  tft.print(line2);
  tft.setCursor(4, 70);
  tft.print(line3);
}

// Blocks until DOT/DASH is pressed and released (any duration). Only used
// during the one-time boot storage-failure flow, never during normal
// operation.
void waitForDotConfirm() {
  InputEvent e;
  for (;;) {
    Input::update();
    while (Input::popEvent(e)) {
      if (e.type == InputEventType::DOT_RELEASE) return;
    }
    delay(5);
  }
}

// Mounts LittleFS with the required error/acknowledge/retry/format-confirm
// flow (Addendum section 8 "Mount failure"). Never auto-formats silently.
void mountLittleFsWithFlow() {
  if (LittleFS.begin(false, "/littlefs", 10, kPartitionLabel)) return;

  drawBlockingMessage("Storage Error", "LittleFS mount failed.", "Press DOT to retry");
  waitForDotConfirm();

  if (LittleFS.begin(false, "/littlefs", 10, kPartitionLabel)) return;

  drawBlockingMessage("Storage Error", "Mount failed again.", "DOT: Format and Continue");
  waitForDotConfirm();

  // Explicit destructive action, only after two failures and user
  // acknowledgement — never silent.
  LittleFS.begin(true, "/littlefs", 10, kPartitionLabel);
}

}  // namespace

namespace Storage {

void init() {
  mountLittleFsWithFlow();

  g_core.begin("mb_core", false);
  g_wifi.begin("mb_wifi", false);
  g_group.begin("mb_group", false);
  g_recent.begin("mb_recent", false);
  g_enigma.begin("mb_enigma", false);
  g_notify.begin("mb_notify", false);
  g_solo.begin("mb_solo", false);
}

Preferences& core() { return g_core; }
Preferences& wifi() { return g_wifi; }
Preferences& group() { return g_group; }
Preferences& recent() { return g_recent; }
Preferences& enigma() { return g_enigma; }
Preferences& notify() { return g_notify; }
Preferences& solo() { return g_solo; }

void removeGroupDirectoryIfPresent(const char* group_code) {
  char path[48];
  snprintf(path, sizeof(path), "/messages/%s", group_code);

  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  // One level of contact_key subdirectories, one level of message files.
  File contactDir = dir.openNextFile();
  while (contactDir) {
    char contactPath[80];
    snprintf(contactPath, sizeof(contactPath), "%s", contactDir.path());
    bool isDir = contactDir.isDirectory();
    contactDir.close();

    if (isDir) {
      File inner = LittleFS.open(contactPath);
      if (inner) {
        File msgFile = inner.openNextFile();
        while (msgFile) {
          char msgPath[128];
          snprintf(msgPath, sizeof(msgPath), "%s", msgFile.path());
          msgFile.close();
          LittleFS.remove(msgPath);
          msgFile = inner.openNextFile();
        }
        inner.close();
      }
      LittleFS.rmdir(contactPath);
    } else {
      LittleFS.remove(contactPath);
    }

    contactDir = dir.openNextFile();
  }
  dir.close();
  LittleFS.rmdir(path);
}

}  // namespace Storage
