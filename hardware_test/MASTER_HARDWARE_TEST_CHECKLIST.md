# Morse Buddy — Master Hardware Test Checklist (Phase 0–5)

Source-derived test plan. Every electrical/timing/behavioral value below is
cited from the current repository source (file named per item); nothing is
invented. Any item hardware alone can answer is marked **HARDWARE
VALIDATION REQUIRED** rather than asserted as fact. This file is a TEST
PLAN ONLY — running it makes no firmware, version, or OTA-server change.

Test-ID prefixes used throughout (full field-by-field record format is in
`TEST_RESULT_TEMPLATE.md`):

```
HW-BOOT-###    first USB flash / boot sequence
HW-ID-###      Identity::init() / device ID
HW-UI-###      Display / Menu / Input
HW-NVS-###     Settings persistence
HW-PWR-###     Power / battery
HW-SLEEP-###   Sleep / wake
HW-WIFI-###    WiFi / NTP
HW-MQTT-###    MQTT / Presence
HW-TEXT-###    Text messaging
HW-ENIGMA-###  Enigma
HW-GAME-###    Number Guessing (Solo/Friend)
HW-PRACT-###   Morse Practice
HW-NOTIF-###   Notifications / sound priority
HW-RADIOHW-### Radio I2S hardware
HW-RADIO-###   Private Radio call
HW-STUN-###    STUN / UDP / MQTT fallback
HW-BCAST-###   Everyone / broadcast Radio
HW-RACE-###    Race Mode
HW-MEM-###     Memory / heap measurement
HW-OTA-###     OTA functional tests
HW-OTAFX-###   OTA failure-fixture tests (ota_test_cases/01–17)
HW-RB-###      Automatic rollback
HW-MRB-###     Manual rollback
HW-PL-###      Power loss during OTA
HW-PERSIST-### Persistence across OTA
HW-LEAK-###    Repeated-operation / leak tests
HW-E2E-###     Final end-to-end integration
```

---

## SECTION 1 — Required Test Equipment

| Item | Needed? | Why |
|---|---|---|
| Morse Buddy ESP32 boards | **3** (2 is the functional floor; 3 needed for Everyone/group, Race multi-player, broadcast contention) | see minimum-device table below |
| USB data cables (data-capable, not charge-only) | 1 per board in use | `pio run -t upload`, `pio device monitor` |
| PC with PlatformIO (`platform = espressif32@6.13.0`, `board = esp32dev`, per `platformio.ini`) | 1 | build/flash/monitor |
| TFT display (ST7789, wired per Section 2) | 1 per board | `Display::init()` assumes this exact controller (`Adafruit_ST7789`) |
| Rotary encoder w/ push switch | 1 per board | Menu/Settings navigation, ENCODER_LONG = Back |
| DOT/DASH momentary switch | 1 per board | Morse entry, PTT, menu confirm |
| INMP441 I2S MEMS microphone | 1 per board (2+ boards for any Radio test) | `radio_audio.cpp` mic capture |
| MAX98357A I2S amplifier + speaker | 1 per board | `sound_i2s.cpp` tones, `radio_audio.cpp` playback |
| LiPo battery (or bench supply feeding the same rail the divider senses) | at least 1, ideally per board | `Power` battery percent, OTA's 30% gate |
| Multimeter | 1 | verify battery divider voltage, confirm GPIO26/34 electrical behavior before trusting the on-screen % |
| Bench PSU (optional) | recommended | controlled power-loss tests (Section 30) without abusing a real battery |
| WiFi access point with Internet access | 1 | MQTT to `broker.hivemq.com:1883`, OTA HTTPS to `sarawutekcrane.github.io` |
| MQTT reachability | via the AP's Internet | `broker.hivemq.com:1883` is a **public, unencrypted (plain MQTT, not MQTTS)** broker — note for testers, not a defect to fix here |
| Write access to the live `morse-buddy-ota` GitHub Pages repo (Section 27) | **required** | **Corrected**: the production firmware has no configurable manifest URL — `OtaConfig::kBaseUrl` + `kManifestPath` are compile-time constants pointing only at `https://sarawutekcrane.github.io/morse-buddy-ota/manifest.txt`. There is no scratch-path or alternate-server option on the device. Every OTA failure-fixture test (Section 27) requires *temporarily replacing that exact live file* with one fixture at a time, then restoring the known-good manifest immediately after — see **OTA TEST MANIFEST SWAP PROCEDURE** before Section 27 |

### Minimum device count by feature

| Feature | Minimum devices |
|---|---|
| Boot / Identity / UI / Settings / Power / Sleep / Morse Practice (solo) | **1** |
| Text/Presence (private) | **2** |
| Private Radio | **2** |
| Play with Friend (Number Guessing) | **2** |
| Race Mode (functional) | **2** |
| Race Mode (multi-player ordering, owner handoff) | **3** |
| Everyone / group broadcast (Text and Radio) | **2** minimum, **3** to test group behavior with more than one listener/contender |
| Multi-device collision/identity test (validates the Phase 0–5 audit's Identity::init() fix) | **2** |

---

## SECTION 2 — Final Pin Map

All values from `src/core/pins.h` (the single source of truth every other
module includes — no other file hardcodes a pin number, confirmed by grep
during the Phase 0–5 audit).

| Function | GPIO | Direction / mode (from source) |
|---|---|---|
| TFT MOSI | 23 | hardware SPI (VSPI default) |
| TFT SCLK | 18 | hardware SPI (VSPI default) |
| TFT RST | 4 | driven by `Adafruit_ST7789` driver |
| TFT DC | 2 | driven by `Adafruit_ST7789` driver |
| TFT CS | 15 | driven by `Adafruit_ST7789` driver |
| TFT Backlight | 32 | `OUTPUT`, LEDC channel 0, 5 kHz, 8-bit PWM (`display.cpp`) |
| DOT/DASH | 21 | `INPUT_PULLUP`, **active LOW** (pressed = `digitalRead()==LOW`), software-debounced 10 ms (`input.cpp`) |
| Encoder CLK | 16 | `INPUT_PULLUP`, direct quadrature decode, 2 ms debounce |
| Encoder DT | 33 | `INPUT_PULLUP`, direct quadrature decode, 2 ms debounce |
| Encoder SW | 25 | `INPUT_PULLUP`, **active LOW**, 10 ms debounce, 500 ms long-press threshold |
| Mic BCLK | 14 | I2S port 0 (RX), 32-bit slot / 16 kHz (`radio_audio.cpp`) |
| Mic WS | 27 | I2S port 0 (RX) |
| Mic SD | 13 | I2S port 0 (RX), INMP441 data in |
| Speaker BCLK | 19 | I2S port 1 (TX), 16-bit / 16 kHz (`sound_i2s.cpp`, `radio_audio.cpp`) |
| Speaker WS | 22 | I2S port 1 (TX) |
| Speaker DIN | 5 | I2S port 1 (TX), MAX98357A data out |
| Battery ADC | 34 | ADC1 channel 6, 12-bit, 11 dB atten, **input-only pin** (GPIO34 has no output driver on ESP32 — matches its exclusively-ADC use in source) |
| Battery sense control | 26 | `OUTPUT`, **active HIGH enables the divider** (`digitalWrite(26, HIGH)` powers the sense path; `LOW` at boot and between samples — `power.cpp`) |

All four `INPUT_PULLUP` pins (DOT/DASH, Encoder CLK/DT/SW) require **no
external pull resistor** — the ESP32's internal pull-up is enabled in
`Input::init()` and every button/encoder contact is wired to GND when
active. **HARDWARE VALIDATION REQUIRED**: confirm the physical switches
are wired common-to-GND, not common-to-3V3 (a 3V3-common wiring would read
permanently "pressed").

### Pull-ups/pull-downs source does not configure

- Battery divider (R1/R2 = 100k/100k per `power.cpp`'s comment) is an
  external resistor pair on the sensed rail — not a GPIO pull, and not
  configured by firmware beyond driving GPIO26.
- I2S pins (14/27/13/19/22/5) use no internal pull in source; standard
  I2S wiring conventions apply. **HARDWARE VALIDATION REQUIRED** for the
  specific INMP441/MAX98357A breakout boards in use.

### Boot-strap pin concerns

This is general ESP32 hardware knowledge applied to the pins Morse Buddy
happens to use — the Morse Buddy source cannot itself define SoC-level
boot-strapping behavior, so treat every line below as **HARDWARE
VALIDATION REQUIRED**, not as something already proven safe by the
source review:

- **GPIO2 (TFT DC)** is an ESP32 boot-strapping pin — must be LOW or
  floating during boot for normal SPI-flash boot. If the ST7789 module
  or its reset circuit holds DC high during the ESP32's own boot-sample
  window, boot could fail. Verify the display module doesn't drive DC
  before the ESP32 has finished sampling strap pins.
- **GPIO15 (TFT CS)** is also a boot-strapping pin — has a default
  internal pull-up; pulling it LOW during boot silences ROM boot-log
  output on U0TXD and can affect timing-mode selection. Verify CS is not
  held low by the display module during power-up before `Display::init()`
  explicitly takes control of it.
- **GPIO5 (Speaker DIN)** is a boot-strapping pin affecting SDIO
  slave-mode timing selection (Morse Buddy doesn't use SDIO, so this is
  lower risk, but the MAX98357A module's own idle/pull state on this line
  during boot should still be checked).
- **GPIO12** is not used by any Morse Buddy peripheral (confirmed by
  grep across `pins.h` and all sources) — no MTDI/flash-voltage
  boot-strap conflict exists.
- **GPIO0/1/3/6–11** are not used by any Morse Buddy peripheral
  (confirmed directly against `pins.h`'s complete pin list) — no boot,
  UART0, or embedded-flash-SPI conflict exists.
- **GPIO34** is input-only by SoC design (no internal pull, no output
  driver) — its exclusively-ADC use in `power.cpp` is consistent with
  that constraint, not a concern.

**HW-BOOT-000**: Before first flash, visually/electrically confirm no
external circuit holds GPIO2, GPIO15, or GPIO5 in a state that would
block a normal boot, using the multimeter with the board unpowered and
then during a cold power-up. **HARDWARE VALIDATION REQUIRED.**

---

## SECTION 3 — Initial USB Flash

Partition layout in `partitions.csv` (verified against a real `pio run`
build in this session):

```
phy_init   0xD000   0x1000
otadata    0xE000   0x2000
ota_0      0x10000  0x160000
ota_1      0x170000 0x160000
nvs        0x2D0000 0x10000
littlefs   0x2E0000 0x120000
```

`board_build.filesystem = littlefs`, `board_build.partitions =
partitions.csv` (`platformio.ini`). `ota_0` sits exactly at PlatformIO's
default `0x10000` application-upload offset — no `upload_addr` override
exists or is needed.

**Erase/clean note**: this partition layout is a *breaking* change from
any pre-Phase-5 layout. If a board was ever flashed with older Morse
Buddy firmware, its old NVS and LittleFS contents become inaccessible
once this table is flashed — **this is expected and accepted** (documented
in `OTA_RELEASE_README.md`'s "Initial Phase 5 installation" section). For
brand-new, never-before-flashed boards this doesn't apply.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-BOOT-001 | `pio run` (build only) | `SUCCESS`, RAM/Flash summary printed | Matches your last local build: RAM ~36%, Flash ~78% of the 1,441,792-byte OTA slot (re-confirm exact numbers at flash time) |
| HW-BOOT-002 | `pio run -t upload` with board in download mode | Upload completes, ESP32 resets | esptool reports success; no verify-mismatch |
| HW-BOOT-003 | `pio device monitor` (115200 baud, `monitor_speed` in `platformio.ini`) immediately after upload | Boot log streams | Serial visible, no garbage (wrong baud) or silence |
| HW-BOOT-004 | Inspect boot log for LittleFS mount | `Storage::init()` runs before anything else in `setup()` | No "Storage Error" blocking prompt appears on the TFT (that only fires if mount genuinely fails) |
| HW-BOOT-005 | Confirm running partition | Device should boot `ota_0` (first-ever flash, `otadata` blank) | Cross-check via `esptool.py --port <p> read_flash_status` or by later observing `esp_ota_get_running_partition()`'s label logged by OTA code once you reach Section 22 |
| HW-BOOT-006 | Record: boot log, running partition, firmware version/build (Firmware Update screen shows `Current: v1.0.0 Build: 1`), free heap (`ESP.getFreeHeap()` — not logged at plain boot by any current source line outside OTA flows; read via `pio device monitor` if you add a temporary Serial print, or rely on the OTA screen's own heap logging in Section 24), reset reason | All fields captured for the test record | Documented in `TEST_RESULT_TEMPLATE.md` |
| HW-BOOT-007 | First-boot behavior with **no WiFi slots configured** | `main.cpp`: `if (!Settings::hasAnyWifiConfigured()) Menu::pushScreen(Settings::screenWifiSlots);` | WiFi setup screen appears before Main Menu |

---

## SECTION 4 — Boot / Identity (validates the Phase 0–5 audit's CRITICAL fix)

Source: `src/core/identity.cpp`, `src/main.cpp` (commit `8236e98`).
`Identity::init()` reads `WiFi.macAddress()` into a 12-hex-char device ID
and loads the persistent `idCounter` from `mb_core`; it is called once,
immediately after `Storage::init()`, before `initRegisteredServices()`.
Before this fix, this function was never called anywhere and every
device silently reported `"000000000000"` forever — this section exists
specifically to prove that regression cannot recur.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-ID-001 | On one device, find any UI surface that shows a device ID (Race Room online-contact list truncates to last 4 hex chars via `appendDeviceSuffix` in `radio.cpp`'s Recipient screen when two names collide — or add a temporary Serial print of `Identity::deviceId()` for direct confirmation) | 12 uppercase hex characters, **not** `000000000000` | Non-default ID observed |
| HW-ID-002 | Reboot the same device (power cycle, not deep sleep) | Device ID unchanged across reboot | Same 12 hex chars before/after |
| HW-ID-003 | Flash a **second** device from the same Build 1 binary | Second device's ID differs from the first (derived from its own WiFi MAC) | IDs are different — this is the direct proof the fix works |
| HW-ID-004 | With 2 devices in the same Family Group, both connect to MQTT | Each gets a distinct client ID (`Identity::deviceId() + CRC32(group_code)`, `mqtt_manager.cpp::buildClientId`) | Neither device is ever kicked off by a client-ID collision; both show ONLINE presence for each other simultaneously |
| HW-ID-005 | Device A sends a Text message to Device B | `sender_device_id` in the received envelope matches Device A's real ID | Correct sender shown, not a shared/default ID |
| HW-ID-006 | Device A starts a Private Radio call to Device B | Channel Busy CLAIM/GRANT addressed by real device IDs (`radio_transport.cpp`) | Call connects; a third device (if available) does not see itself addressed |
| HW-ID-007 | `Identity::nextId()` — create several IDs (send several messages) then reboot | `idCounter` in `mb_core` persists and never repeats a previously-issued ID | No duplicate message/session IDs across a reboot |

---

## SECTION 5 — Display / Menu / Input

Source: `display.cpp`, `menu.cpp`, `input.cpp`. Panel is native
135×240 portrait, `setRotation(1)` → 240×135 landscape
(`Display::kScreenWidth/kScreenHeight`). Status bar height
`kStatusBarHeight`. Backlight is PWM (not a hard on/off).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-UI-001 | Power on | TFT initializes, fills black, then draws Main Menu | No garbled/inverted colors — **HARDWARE VALIDATION REQUIRED**: some ST7789 135×240 panels need a colstart/rowstart offset the source comment explicitly flags as unverified |
| HW-UI-002 | Visual check of landscape orientation | 240 wide × 135 tall, status bar at top | Text reads left-to-right correctly oriented |
| HW-UI-003 | `Settings > Display & Sound`, change brightness | Backlight visibly dims/brightens via PWM duty | Smooth, no obvious flicker |
| HW-UI-004 | Every screen with >1 line of text (long group/contact names, message previews) | No text overlaps or is silently truncated in a way that's misleading (fixed 21/33/40-char buffers throughout) | **HARDWARE VALIDATION REQUIRED**: rendering is only provable on a real 240×135 panel |
| HW-UI-005 | Rotate encoder clockwise on any list screen | Selection moves down (wraps at end, `ListMenu::tick`) | Wraps correctly both directions |
| HW-UI-006 | Rotate encoder counter-clockwise | Selection moves up, wraps at top | As above |
| HW-UI-007 | Encoder short press (<500 ms) on Main Menu | No menu-navigation effect by itself (menu confirm is DOT/DASH release, `Input::isMenuConfirm`) — but Number Guessing/Race Guess use ENCODER_SHORT explicitly | Confirm the *inactive* screens ignore it; confirm Race Guess / digit confirm *do* respond |
| HW-UI-008 | Encoder long press (≥500 ms) anywhere but Main Menu | Navigates back one level (`Input::isBack`) | Main Menu itself does not pop further (stack floor) |
| HW-UI-009 | DOT/DASH short press-release on a list item | Selects/confirms that item (`Input::isMenuConfirm`: release before `Morse::kSpecialCommandMs`=2000ms) | Correct screen opens |
| HW-UI-010 | DOT/DASH held ≥2000 ms in a Morse-entry context | Classified `SPECIAL_COMMAND` (`Morse::classifyPress`), not DOT/DASH | Confirm in Enigma/Text compose that this triggers reveal/special behavior, not a normal keystroke |
| HW-UI-011 | Press DOT and Encoder SW together within 200 ms (`kCombinedWindowMs`) | `COMBINED_START` fires; holding ≥2000 ms (`kCombinedRevealMs`) fires `COMBINED_REVEAL` once | Only in the one screen (Enigma reveal) that consumes it — see HW-UI-013 |
| HW-UI-012 | Mode-specific input ownership: enter Text, Enigma, Radio, Number Guessing, Morse Practice, Race Guess in turn and try every gesture above | Only the mode that defines a special gesture responds to it; all others treat DOT/DASH/Encoder as their own generic Morse/menu input | No cross-mode leakage (e.g. Enigma's combined-reveal gesture is a no-op everywhere else) |
| HW-UI-013 | Combined DOT+DASH gesture *outside* Enigma (e.g. Main Menu, Text compose) | No special effect — remains a no-op per source (only Enigma's reveal screen consumes `COMBINED_REVEAL`) | Confirmed no accidental activation elsewhere |
| HW-UI-014 | From deep inside a Game/Enigma chat (quick-switch flow), trigger `navigateToChatDirect` (e.g. accept a Friend-game quick-switch to Text) | `Menu::init()` is deliberately reused as a **stack-reset primitive** (confirmed intentional in source, same pattern in `enigma.cpp`, `text_message.cpp`, `number_guessing_friend.cpp`) — stack becomes Main Menu → Recipient → Chat | Encoder-long from the resulting Chat screen lands on that mode's normal Recipient screen, not a stale deep stack |
| HW-UI-015 | Visit every root Main Menu item (Text, Enigma, Radio, Training Game, Settings) | Each opens correctly | All 5 reachable, `comingSoonScreen` never appears (all are implemented) |
| HW-UI-016 | Visit every Settings category (Connectivity→WiFi/Family Groups, My Name, Display & Sound, Speed & Power, System→Firmware Update) | Each opens correctly | All 6 (5 root + System added by Phase 5) reachable |
| HW-UI-017 | Visit Training Game → Morse Practice, Number Guessing → Solo/Friend/Race | Each opens correctly | All reachable via the sub-mode registry |

---

## SECTION 6 — Settings / NVS Persistence

NVS namespaces and keys (from `Storage::core/wifi/group/recent/enigma/notify/solo()` — full grep-verified table, no duplicates, no type collisions, confirmed during the Phase 0–5 audit):

| Namespace | Keys |
|---|---|
| `mb_core` | `myName`, `bright`, `vol`, `typDisp`, `muteRadio`, `wpm`, `sleepMin`, `practLvl`, `idCounter`, `prAudioPv`, `prRevealAns`, `otaPending`, `otaPrevBuild`, `otaTgtBuild`, `otaLastFail` |
| `mb_wifi` | `slots` (blob) |
| `mb_group` | `groups` (blob) |
| `mb_recent` | `recent` (blob) |
| `mb_enigma` | `keys` (blob) |
| `mb_notify` | `summary` (blob) |
| `mb_solo` | `state` (blob) |

For **every** row below: (1) change the value, (2) reboot (power cycle,
not deep sleep wake), (3) confirm persistence, (4) full power cycle
(unplug/replug or battery removal if practical), (5) confirm persistence
again.

| Test ID | Setting | Key | Expected persisted |
|---|---|---|---|
| HW-NVS-001 | My Name | `myName` | Exact string round-trips |
| HW-NVS-002 | Brightness | `bright` | 0–100 round-trips |
| HW-NVS-003 | Speaker volume | `vol` | 0–100 round-trips |
| HW-NVS-004 | Typing display mode | `typDisp` | enum round-trips |
| HW-NVS-005 | Mute Radio Outside Radio | `muteRadio` | bool round-trips |
| HW-NVS-006 | WPM | `wpm` | 1–50 round-trips |
| HW-NVS-007 | Sleep timeout | `sleepMin` | 0 (disabled) or 1–30 round-trips |
| HW-NVS-008 | Morse Practice level | `practLvl` | 1–3 round-trips |
| HW-NVS-009 | WiFi slots (up to 3) | `slots` | SSID/password round-trip for each configured slot |
| HW-NVS-010 | Family Groups (up to 5) | `groups` | name/code round-trip |
| HW-NVS-011 | Morse Practice Audio Preview toggle | `prAudioPv` | bool round-trips |
| HW-NVS-012 | Morse Practice Reveal Answer toggle | `prRevealAns` | bool round-trips |
| HW-NVS-013 | Enigma saved keys (per group/contact) | `keys` in `mb_enigma` | full rotor/plugboard config round-trips |
| HW-NVS-014 | Notification unread summaries | `summary` in `mb_notify` | unread counts/badges survive reboot |
| HW-NVS-015 | Solo game state | `state` in `mb_solo` | in-progress/history state survives reboot |
| HW-NVS-016 | `idCounter` | `mb_core` | monotonically continues after reboot, never resets to 0 |
| HW-NVS-017 | Confirm no key collision | all namespaces | cross-check the table above against a real NVS dump if tooling allows (`nvs_flash` partition tool); source-level grep already found zero duplicates |

---

## SECTION 7 — Power / Battery

Source: `power.cpp`. Divider ratio 2.0 (R1=R2=100k). GPIO26 HIGH enables
the sense divider; `LOW` at boot and between the 6-second sample cycles —
**never left permanently enabled** except for the brief 5 ms settle
window per sample. ADC1 channel 6 (GPIO34), 12-bit, 11 dB attenuation via
`esp_adc_cal`. LiPo voltage→percent table is a fixed 22-point lookup
(`kLipoTable`, 4.20V=100% down to 3.30V=0%).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-PWR-001 | Multimeter on GPIO26 at idle (between samples) | LOW (0V) | Divider confirmed off most of the time |
| HW-PWR-002 | Multimeter on GPIO26 during a sample (hard to catch manually — scope recommended) | Brief HIGH pulse, ~5 ms | **HARDWARE VALIDATION REQUIRED** |
| HW-PWR-003 | Multimeter on GPIO34 with divider enabled vs. disabled | Reads ~half the battery rail when enabled (divider ratio 2.0), near 0V when disabled | Confirms the 100k/100k assumption |
| HW-PWR-004 | Status bar battery % at known battery voltages (compare multimeter reading through the LiPo table) | Matches `kLipoTable`'s nearest interpolated point | **Record actual vs. table** — do not treat the displayed % as lab-accurate; it is a fixed-curve estimate, not a calibrated fuel gauge |
| HW-PWR-005 | Battery at ≤20% | `Power::isLowBattery()` becomes true | Any UI that surfaces low-battery state reflects it (no current screen text is asserted here beyond what OTA does — verify what, if anything, Settings/status bar shows) |
| HW-PWR-006 | Battery at ≤10% | `Power::isCriticalBattery()` becomes true | Same caveat as above |
| HW-PWR-007 | Attempt OTA "Update" confirm with battery <30% | `OtaConfig::kMinBatteryPercent=30` gate in `beginInstallFlow()` | Screen shows `Battery Too Low / Charge before update`; no download attempted |
| HW-PWR-008 | Attempt OTA "Update" confirm with battery ≥30% | Gate passes | Proceeds to heap check / PREPARING |
| HW-PWR-009 | Charging behavior (if a charge circuit is present) | Source has **no charger-connected signal or override** — battery % while charging is explicitly approximate per design | Do not fail this test for "wrong %" while actively charging; document actual vs. table only |
| HW-PWR-010 | Confirm no auto-shutdown exists | Source contains no low-battery forced power-off anywhere in `power.cpp`/`sleep.cpp` | Device continues running at 0–10% until physically powered off or truly dies |

---

## SECTION 8 — Sleep / Wake

Source: `sleep.cpp`, `input.cpp` (activity sources), Phase 4/5 additions.
Deep sleep is entered via `esp_deep_sleep_start()` with **no wake source
configured — wake is power-cycle only** (unplug/replug or a hard reset,
not a GPIO/timer wake). `Sleep::setSuspended()` (Phase 5) gates the whole
timeout check and resets the activity baseline the moment suspension ends.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-SLEEP-001 | Set sleep timeout to 1 minute, remain idle | Deep sleep after ~60s | Backlight off, `esp_deep_sleep_start()` triggers |
| HW-SLEEP-002 | Confirm wake requires power cycle | No GPIO/timer wakes the device | Device stays asleep until power is physically cycled — **HARDWARE VALIDATION REQUIRED** (confirm no accidental EXT0/timer wake fires) |
| HW-SLEEP-003 | Encoder rotation resets the inactivity timer | `Sleep::notifyActivity()` called in `updateQuadrature` | Timer visibly restarts |
| HW-SLEEP-004 | DOT/DASH press resets the timer | `Sleep::notifyActivity()` in `updateDotStateMachine` | Timer restarts |
| HW-SLEEP-005 | Incoming Radio audio actually played resets the timer | `Sleep::notifyActivity()` called from `jitterPlayTick`/`handlePrivateAudioScope`/`handleBroadcastAudioScope`/Race's `handleRoomAudioScope` — **only when audio is actually played**, not merely received | Confirm receiving a packet that's *dropped* (Mute On, wrong scope, background not allowed) does NOT reset sleep |
| HW-SLEEP-006 | Incoming Text message while idle | `Notifications::onMessageArrived` calls `Sleep::notifyActivity()` | Timer restarts on arrival |
| HW-SLEEP-007 | Confirm passive MQTT/WiFi background traffic that produces no user-visible event does not itself reset the timer indefinitely | No `notifyActivity()` call exists in `mqtt_manager.cpp`'s plain `serviceTick()`/reconnect path | Device can still sleep even while connected and idle |
| HW-SLEEP-008 | Trigger deep sleep, confirm `runBeforeSleepHooks()` ran | MQTT's `onBeforeSleep()` best-effort-publishes OFFLINE for connected groups (2s budget) | Other devices see this device go OFFLINE around sleep time |
| HW-SLEEP-009 | Enter OTA Maintenance Mode (confirm an Update) | `Sleep::setSuspended(true)` | Device does not sleep mid-download even if the configured timeout would otherwise elapse |
| HW-SLEEP-010 | Force an OTA failure (e.g. `02_wrong_sha` fixture) after entering Maintenance Mode, then sit idle afterward | `leaveMaintenanceMode()` → `Sleep::setSuspended(false)` → `notifyActivity()` gives a **fresh** baseline | Device does not immediately fall asleep the instant Maintenance Mode ends; normal timeout countdown resumes from zero |
| HW-SLEEP-011 | Confirm no double sleep/resume | Single `g_suspended` flag, single `g_lastActivityMs` | No stuck-asleep or stuck-awake state after repeated OTA attempts |

---

## SECTION 9 — WiFi / NTP

Source: `wifi_manager.cpp`. Per-slot connect timeout 15000ms
(`kPerSlotTimeoutMs`), full-cycle retry interval 60000ms
(`kRetryIntervalMs`), NTP retry interval 60000ms
(`kNtpRetryIntervalMs`), NTP-synced threshold ~Sep 2020
(`kNtpSyncedThreshold`). Up to 3 WiFi slots (`Settings::kMaxWifiSlots`).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-WIFI-001 | First boot, zero slots configured | WiFi setup screen shown before Main Menu | Matches HW-BOOT-007 |
| HW-WIFI-002 | Configure 1 valid slot | Connects within ~15s | Status bar shows WIFI/ONLINE tier |
| HW-WIFI-003 | Configure a slot with a wrong password | Never connects that slot; cycles to next slot after 15s, retries full cycle every 60s | Status bar stays OFFLINE/WIFI-only as appropriate; no crash/hang |
| HW-WIFI-004 | Configure all 3 slots, only the 3rd is valid | Device cycles through all 3, connects on the 3rd | Confirms round-robin slot logic |
| HW-WIFI-005 | Disconnect AP mid-session (power off the AP) | Device detects disconnect, retries | Reconnects automatically once AP returns |
| HW-WIFI-006 | AP returns after being down | Reconnects within one retry cycle | Presence republishes ONLINE |
| HW-WIFI-007 | Hidden SSID | Source uses plain `WiFi.begin(ssid, password)` with no hidden-network handling — **not explicitly supported**; do not expect it to work | Document actual behavior observed, do not treat failure as a bug |
| HW-WIFI-008 | NTP sync after WiFi connects | `configTime()` fires once connected; `isNtpSynced()` becomes true once `time(nullptr) > ~Sep2020` | Presence timestamps and OTA proceed once synced |
| HW-WIFI-009 | NTP temporarily unavailable (block NTP servers/ports if practical) | Messaging should continue to function (NTP is not a hard gate for MQTT/Text per source) | Confirm Text/Presence work without NTP; only OTA explicitly blocks on it |
| HW-WIFI-010 | NTP later becomes available | `isNtpSynced()` flips true on the next successful `configTime` retry (60s interval) | OTA's "Time Sync Required" screen (if shown) clears on Retry once synced |
| HW-WIFI-011 | Record actual connect/retry timings | Compare against the 15000/60000/60000 ms constants above | Note any material deviation |

---

## SECTION 10 — MQTT / Presence

Source: `mqtt_manager.cpp`, `presence.cpp`. One `MQTTClient` per group.
`cleanSession=false` normally; `true` only during the one-shot group-deletion
sequence. Receive queue capacity 16, per-packet cap 1024 bytes, **dynamic
per-packet heap allocation** (the Phase 4 DRAM-overflow fix — confirmed
still intact in the Phase 0–5 audit). Presence uses the `MBP1` ASCII
format (the one deliberate non-binary wire exception), retained QoS1.

Requires **≥2 physical devices**.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-MQTT-001 | Both devices join the same Family Group | Both connect to `broker.hivemq.com:1883` | `isGroupConnected()` true on both |
| HW-MQTT-002 | Device A comes online | Device B sees retained ONLINE presence | Device B's online-contact list shows A within one presence interval |
| HW-MQTT-003 | Device A goes offline (power off) | Device B eventually sees OFFLINE (LWT or explicit publish) | Confirm actual mechanism observed — **HARDWARE VALIDATION REQUIRED** for LWT timing specifics |
| HW-MQTT-004 | Reconnect Device A | Presence republishes ONLINE | B sees A online again |
| HW-MQTT-005 | Configure 2 groups on one device | Two independent `MQTTClient` connections, two independent subscribe sets | Presence/messages correctly scoped per group |
| HW-MQTT-006 | Delete a group | `destroyClientForGroup()`: offline publish, zero-length retained self-record clear, `cleanSession=true` reconnect, disconnect | Broker no longer shows this device's retained presence for that group |
| HW-MQTT-007 | Confirm client-ID uniqueness | See HW-ID-004 | No collision-induced disconnects |
| HW-MQTT-008 | Send a deliberately malformed packet (if you have a way to publish raw MQTT — e.g. a second MQTT client tool) to a subscribed topic | `decodeHeader()`/`decodeMessageEnvelope()` bounds-check and reject; handler is never called on invalid data | No crash, no corrupted state |
| HW-MQTT-009 | Publish an unregistered packet kind | `getNetworkPacketHandler()` returns null, silently ignored | No crash |
| HW-MQTT-010 | Burst: send ~16+ messages rapidly from A to B | Queue absorbs up to 16 in flight (`kQueueCapacity`), each payload heap-allocated to exact size, freed exactly once after `processQueuedMessage()` | No message corruption; queue drains completely; record free heap before/minimum-during/after |
| HW-MQTT-011 | Exceed queue capacity in one burst | Excess QoS0 packets legitimately dropped (`onMqttMessage`'s `g_queueCount >= kQueueCapacity` early-return) | No crash; queue recovers on next drain |

---

## SECTION 11 — Text Messaging

Source: `text_message.cpp`, `storage_messages.cpp`, `outbox.cpp`,
`notifications.cpp`. Requires **≥2 devices** (3 for Everyone/group
verification).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-TEXT-001 | A sends private message to B | B receives, correct sender/content | Delivered promptly while both online |
| HW-TEXT-002 | B replies | A receives | Bidirectional confirmed |
| HW-TEXT-003 | A sends to Everyone (`MessageStore::kEveryone`) | All group members receive | Broadcast topic used, not per-recipient |
| HW-TEXT-004 | Status/unread flags | New message marks unread unless that exact conversation is currently open | `FLAG_UNREAD` set/cleared correctly |
| HW-TEXT-005 | Open the exact conversation while a message arrives | No notification tone, no unread badge (per `onMessageArrived`'s `conversationCurrentlyOpen` branch) | Confirmed suppressed |
| HW-TEXT-006 | Same message_id delivered twice (retry/duplicate publish) | Dedup ring (`isDuplicateAndRecord`, 32-entry LRU pool of 8 conversations) rejects the repeat | No duplicate stored/shown |
| HW-TEXT-007 | Send while B is offline | Message stored with `FLAG_PENDING_OUTBOX` | Shown as pending on A |
| HW-TEXT-008 | B reconnects | `Outbox::flushPending()` (5s scan interval) delivers | B receives; A's pending flag clears |
| HW-TEXT-009 | Reboot A with an unread/pending message present | LittleFS persists it | Message still present, correct flags, after reboot |
| HW-TEXT-010 | Fill a conversation toward the 300-message cap (`kMaxMessagesPerThread`) | Oldest **non-pending** message evicted first at the 96 KiB/128 KiB low/target watermark; pending messages are never evicted (`findOldestNonPendingSequence` explicitly skips `FLAG_PENDING_OUTBOX`) | Confirm a still-pending message survives eviction pressure |
| HW-TEXT-011 | LittleFS capacity sanity check | Partition is now 1,179,648 bytes (1152 KiB) post-Phase-5, down from ~1,875 KiB | At ~460 bytes/message × 300/conversation ≈ 138 KB per full conversation — confirm several concurrent conversations fit comfortably before the low-space eviction logic engages |

---

## SECTION 12 — Enigma

Source: `enigma.cpp`, `enigma_crypto.h/.cpp`, `enigma_keys.h/.cpp`.
0–4 rotors (`kMaxRotors=4`), up to 6 plugboard pairs
(`kMaxPlugboardPairs=6`), max escaped ciphertext 200 chars
(`kMaxEscapedLen`). Default (never-configured) key is 0 rotors + 0
plugboard pairs, generation 0.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-ENIGMA-001 | Configure 0 rotors, plugboard-only (or nothing) | Encrypt/decrypt still functions (pure plugboard passthrough or literal passthrough) | Reciprocal: decrypting the ciphertext with the same key returns the original text |
| HW-ENIGMA-002 | Configure 1–4 rotors with distinct types/positions | Encrypt/decrypt reciprocal | Same reciprocity check |
| HW-ENIGMA-003 | Plugboard-only (0 rotors, ≥1 pair) | Reciprocal swap behavior | A→B and B→A both correct |
| HW-ENIGMA-004 | Save a sender key, send a message | `setSenderKey` increments `sender_key_generation` on **every** call, even re-saving an identical key | Generation counter strictly increases |
| HW-ENIGMA-005 | Receive an Enigma message with an embedded key | Received/locked state stored locally | Confirm this device cannot "bulk unlock" via any MQTT-delivered mechanism — unlocking is local-only per source design |
| HW-ENIGMA-006 | Local reveal/unlock gesture | Combined DOT+DASH reveal (HW-UI-011) unlocks the message on-screen | Only affects local display, never republished |
| HW-ENIGMA-007 | Reboot with saved keys present | `mb_enigma`'s `keys` blob persists | Same key config after reboot |
| HW-ENIGMA-008 | OTA Build 1→2 with saved Enigma keys present | OTA writes only the inactive app partition; NVS (`mb_enigma`) untouched | Keys survive the update — see Section 31 |

---

## SECTION 13 — Number Guessing

Source: `number_guessing.h/.cpp` (shared + Solo), `number_guessing_friend.cpp`
(Friend). `kSecretDigits=4`; the shared `DigitEntryState` **skips
duplicate digits while rotating**, so a completed guess is always 4
distinct digits by construction (not a separately-enforced check).
`kMaxAttempts=255` is the counter's type range, not necessarily an
enforced game-over limit — confirm actual on-device behavior rather than
assuming a hard cutoff.

**Solo** (1 device):

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-GAME-001 | Enter a guess | Always 4 distinct digits (rotation skips repeats) | Confirm you cannot land on an already-used digit while rotating |
| HW-GAME-002 | Guess containing a leading zero (e.g. 0123) | Source does not special-case digit position | Confirm leading zero is accepted — **HARDWARE VALIDATION REQUIRED** to confirm no UI-level rejection exists beyond the parser |
| HW-GAME-003 | Correct guess (`GuessResult.a == 4`) | Win state shown | Correct win handling |
| HW-GAME-004 | Wrong guess | a/b feedback shown, attempt recorded | Feedback matches actual digit-position/digit-present counts |
| HW-GAME-005 | Attempt-history persistence | `mb_solo`'s `state` blob | History survives reboot |

**Friend** (2 devices):

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-GAME-006 | A invites B | Challenge delivered via `MSG_TYPE_GAME` in the Unified Thread | B sees the challenge, notification badge (BADGE_TRAINING_GAME) fires |
| HW-GAME-007 | B responds | Response delivered back to A | Correct routing |
| HW-GAME-008 | Confirm no "Everyone" option exists for Friend challenges | Source: Friend targets a specific contact only | No broadcast challenge possible |
| HW-GAME-009 | Result/history chunking (`PK_GAME_RESULT_CHUNK`) | Chunked reassembly completes correctly | Full history round-trips without corruption |
| HW-GAME-010 | Deliberately interrupt mid-chunk-transfer (toggle WiFi off on receiver) | Incomplete chunk handled without corrupting stored state | On reconnect, resumes/retries cleanly, no partial/garbage record persisted |
| HW-GAME-011 | Duplicate chunk delivery (retry) | Existing dedup logic prevents double-apply | No duplicated history entries |
| HW-GAME-012 | Reboot mid-game | State persists appropriately | Resumable or cleanly reset per source's actual persistence scope — document actual behavior |

---

## SECTION 14 — Morse Practice

Source: `morse_practice.cpp`. `prAudioPv`/`prRevealAns` toggles persist
in `mb_core`.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-PRACT-001 | Enter character via DOT/DASH at various WPM | Correctly classified per `Morse::classifyPress` timing | Accurate decode across a WPM range (1–50, warn threshold ≥35 per `Morse::kWpmWarnThreshold`) |
| HW-PRACT-002 | Question progression | Advances correctly through the question list | No stuck/repeated question beyond intended design |
| HW-PRACT-003 | Audio Preview mode ON | Tone plays for the target character, no-score mode | Confirm scoring is suppressed while this toggle is active |
| HW-PRACT-004 | Reveal Answer mode ON | Answer shown, no-score mode | Same suppression check |
| HW-PRACT-005 | Normal scored mode | Score increments correctly | Score matches correct/incorrect answers |
| HW-PRACT-006 | Auto-level behavior | Level adjusts per source's actual logic | Document observed thresholds |
| HW-PRACT-007 | High score persistence | Survives reboot | Stored value round-trips |
| HW-PRACT-008 | Exit mid-practice, re-enter | No stale sound or input ownership left behind (`stopCurrentToneSound()` on exit paths) | Speaker silent immediately after exit; next mode's input works normally |
| HW-PRACT-009 | Sleep interaction | DOT/DASH activity resets sleep timer during practice | No unexpected sleep mid-session |

---

## SECTION 15 — Notifications / Sound Priority

Source: `notifications.cpp`, `sound_facade.cpp`. Priority is exactly
`Radio > Notification > Game` — `sound_facade.cpp`'s
`higherOrEqualPriority()` compares the two-class enum (`SOUND_GAME=1`,
`SOUND_NOTIFICATION=2`); Radio preempts both unconditionally via
`setRadioAudioActive(true)` calling `stopCurrentToneSound()`.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-NOTIF-001 | Text arrives, conversation not open | Badge (BADGE_TEXT) + 1000Hz/150ms tone (`SOUND_NOTIFICATION`) | Both fire |
| HW-NOTIF-002 | Enigma arrives | Badge (BADGE_ENIGMA) + tone | Correct badge target |
| HW-NOTIF-003 | Friend challenge arrives | Badge (BADGE_TRAINING_GAME) + tone | Correct badge target |
| HW-NOTIF-004 | Race invite arrives | Uses the **same** Training Game badge via `setRaceInvitePending()` OR'd with `hasAnyUnreadTrainingGame()` (not a second badge slot) | Badge shows; confirm it clears correctly when the invite is handled |
| HW-NOTIF-005 | Same conversation currently open when a message arrives | No tone, no badge (`conversationCurrentlyOpen` branch) | Confirmed suppressed |
| HW-NOTIF-006 | Read an unread message | Badge/unread count clears | `clearUnread()` behavior confirmed |
| HW-NOTIF-007 | Trigger a Game tone and a Notification tone in quick succession | Notification (2) preempts Game (1) if Game is still playing; Game does not preempt an in-progress Notification | Priority order confirmed both directions |
| HW-NOTIF-008 | Trigger a Radio call while a Notification/Game tone is playing | Radio immediately silences it (`setRadioAudioActive(true)` → `stopCurrentToneSound()`) and blocks new tones for the call's duration | No audio collision |
| HW-NOTIF-009 | Confirm only one sound backend is ever registered | `registerSoundBackend()` called exactly once (`sound_i2s.cpp`'s `Registrar`) | No double-registration possible by construction — code-level fact, not separately testable on hardware |

---

## SECTION 16 — Radio Audio Hardware

Source: `radio_audio.cpp`, `sound_i2s.cpp`. Mic: I2S port 0, RX, 32-bit
slot with INMP441's top-16-bits-of-32 technique, 16 kHz, mono
(`I2S_CHANNEL_FMT_ONLY_LEFT`). Speaker: I2S port 1, TX, 16-bit, 16 kHz,
mono. Frame = 320 samples / 20 ms = 640 bytes (`kFrameSamples`,
`kFrameBytes`).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-RADIOHW-001 | Speak into mic during a Private call | Captured audio audible on the other device | Intelligible speech — **HARDWARE VALIDATION REQUIRED** for actual quality |
| HW-RADIOHW-002 | Check sample rate/format assumptions | 16 kHz/16-bit/mono end to end | Confirm no unexpected resampling artifacts |
| HW-RADIOHW-003 | Frame timing | 320 samples every 20ms captured/played | **HARDWARE VALIDATION REQUIRED**: confirm no audible frame gaps/stutter |
| HW-RADIOHW-004 | Audible quality / clipping | `kAmplitude=3000` headroom for tones; Radio playback scales by `Settings::getSpeakerVolume()` | No clipping at max volume — reduce volume if distorted and note actual behavior |
| HW-RADIOHW-005 | Mic gain / background noise | INMP441 breakout-dependent | **HARDWARE VALIDATION REQUIRED** |
| HW-RADIOHW-006 | Speaker underrun (busy main loop) | `i2s_write`/`i2s_read` use zero timeout — never blocks the main loop, but can silently drop frames under load | Listen for dropouts during heavy concurrent activity (e.g. mid-OTA-adjacent operations, though Radio should be quiesced then) |
| HW-RADIOHW-007 | Heap before/during/after a sustained audio session | Record `ESP.getFreeHeap()` at each point | No net heap loss after the session ends and buffers are freed |

---

## SECTION 17 — Private Radio

Source: `radio_transport.cpp`. Requires **2 devices**. Constants:
CLAIM retry every 250ms (`kClaimRetryIntervalMs`) for up to 1000ms total
(`kClaimTotalTimeoutMs`), active-call refresh every 2000ms
(`kClaimRefreshMs`), receiver Busy TTL 5000ms (`kBusyTtlMs`), negotiate
timeout 2000ms (`kNegotiateTimeoutMs`).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-RADIO-001 | A presses PTT to call B | CLAIM sent, B auto-GRANTs (receiver-authoritative Busy) | Call connects |
| HW-RADIO-002 | B receives audio while A holds PTT | Callee playback works | Audible on B |
| HW-RADIO-003 | Introduce a third device attempting to CLAIM B while A↔B is active | B DENYs the third device (receiver already busy with a different claimer) | Third device sees DENY / "no mic" state |
| HW-RADIO-004 | Simulate a dropped CLAIM (e.g. brief WiFi flap on A right as PTT is pressed) | Retries every 250ms | **HARDWARE VALIDATION REQUIRED** — confirm audible/observable retry cadence |
| HW-RADIO-005 | No GRANT arrives within 1000ms total | PTT attempt fails, `g_call` resets | A's UI shows failed/no-mic state, no stuck CLAIMING |
| HW-RADIO-006 | A releases PTT normally | RELEASE published with the exact session_id, `g_call` torn down | B's receiver-busy record clears; B can now be claimed by someone else |
| HW-RADIO-007 | A stale/older-session RELEASE arrives (hard to force manually — note as best-effort) | Session-ID mismatch causes it to be ignored | **HARDWARE VALIDATION REQUIRED** if a deliberate stale-session injection isn't practical |
| HW-RADIO-008 | Active call held past 2000ms | CLAIM refresh republished automatically | Call stays alive past the refresh boundary |
| HW-RADIO-009 | Receiver's Busy TTL (5000ms) with no refresh (e.g. force-kill the caller device) | Busy record self-expires, callee's `g_call` torn down via `releaseCalleeCallIfMatches` | Peer becomes claimable again within ~5s of the caller vanishing |
| HW-RADIO-010 | End a call, inspect memory | Pre-buffer (~64 KB) freed, jitter buffer cleared (`resetPrivateCall()`) | Confirm free heap returns to pre-call baseline |
| HW-RADIO-011 | Callee's own DOT/DASH press during an **incoming** call | Per source, the callee's own PTT never tears down an incoming call (`stopPrivateCall()`'s `!g_call.isCaller` guard) | Call continues; confirm this explicitly, it's a deliberate, previously-audited behavior |

---

## SECTION 18 — UDP / STUN / MQTT Fallback

Source: `radio_transport.cpp`. UDP port 5005, STUN host
`stun.l.google.com:19302` (hand-rolled RFC 5389 client).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-STUN-001 | Normal call on an open network (no symmetric NAT/strict firewall) | STUN resolves public endpoint, UDP direct path used | Call transitions to `UDP_DIRECT` state |
| HW-STUN-002 | Record actual STUN round-trip latency | — | Note real-world value; source has no fixed expectation beyond the 2000ms negotiate timeout |
| HW-STUN-003 | Block UDP outbound (firewall rule) to force STUN/direct failure | Negotiate timeout (2000ms) elapses, falls back to `MQTT_FALLBACK` | Call continues over MQTT relay instead of failing outright |
| HW-STUN-004 | Compare audio quality/latency: UDP direct vs. MQTT fallback | MQTT fallback expected higher latency | Document both for reference |
| HW-STUN-005 | A stale UDP packet from an old/different session arrives (e.g. restart a call quickly) | Session-ID check in `handleIncomingUdp` rejects it (`strcmp(sessionId, g_call.session_id) != 0`) | No audio/state corruption from a leftover packet |

---

## SECTION 19 — Broadcast (Everyone) Radio

Source: `radio_transport.cpp`'s broadcast path, `race.cpp`'s Room voice
(same underlying `AudioScope` mechanism). MQTT-only, no claim/STUN.
Speaker "ownership" arbitration via `kBroadcastSpeakerStaleMs=500`.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-BCAST-001 | A starts Everyone broadcast | All group members receive over MQTT | No claim/negotiation delay (broadcast skips Channel Busy entirely) |
| HW-BCAST-002 | A stops | Broadcast ends cleanly | `stopBroadcastCall()` — capture stopped, audio flag cleared |
| HW-BCAST-003 | A broadcasting, B also starts broadcasting | Speaker-ownership arbitration: whichever is "current" wins until 500ms of silence from it (`g_currentBroadcastSpeaker`/staleness check) | Confirm actual contention behavior with 2 simultaneous talkers |
| HW-BCAST-004 | 3 devices: A broadcasts, B and C both listening | Both B and C receive identically | No per-listener limit observed |
| HW-BCAST-005 | Stale-timeout recovery | After 500ms silence from the current speaker, a new speaker can take over | Confirm handoff works, not permanently locked to the first speaker |

---

## SECTION 20 — Race Mode

Source: `race.cpp`. Requires **2 devices minimum, 3 for multi-player
ordering/owner-handoff**.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-RACE-001 | A invites (publishes PK_RACE_INVITE) | B sees invite, Training Game badge lights (shared with Friend, via `setRaceInvitePending`) | B can join |
| HW-RACE-002 | B joins | `addParticipant` tracks B | A sees B as a participant |
| HW-RACE-003 | A (owner) starts round | `publishStartRound()`, retained `PK_RACE_ROUND` | All participants receive the round/secret |
| HW-RACE-004 | Guess screen — enter a correct guess | `onLocalSolve()` — score increments, retained round cleared (zero-length retained publish) | Winner recorded, round ends cleanly |
| HW-RACE-005 | Late joiner arrives mid-round | Adopts the **retained** round even without having seen the (non-retained) invite (`handleRaceRoundPacket`'s `RoomPhase::NO_LOBBY` adoption path) | Late joiner can participate in the current round |
| HW-RACE-006 | Owner goes offline mid-lobby (before first round) | `checkOwnerOnline()` hands ownership to another online participant | New owner can start the round |
| HW-RACE-007 | Owner goes offline **after** at least one round has run | Room resets (`publishReset()` + `applyReset()`) rather than reassigning ownership | All participants return to `NO_LOBBY` |
| HW-RACE-008 | Reset | `PK_RACE_RESET` clears state on all devices | Everyone returns to a clean lobby state |
| HW-RACE-009 | Reconnect after a brief drop | Retained round/invite state re-syncs via MQTT retained messages | Device catches up correctly |
| HW-RACE-010 | Race Room voice (PTT in the Room screen) | Uses `AUDIO_RACE_ROOM` scope, gated on `g_isRoomScreenActive` and matching `invite_id` | Only other devices currently viewing that same Room hear it |
| HW-RACE-011 | Confirm Guess screen has no voice | `enterGuessScreen()` explicitly calls `stopRoomPtt()` before pushing the Guess screen | No PTT/audio control available while guessing |
| HW-RACE-012 | 3-device multi-player: verify participant ordering, scores, and owner handoff together | Combined scenario | All three devices agree on current state at every step |

---

## SECTION 21 — Memory / Heap Measurement

Record free heap **before / minimum during / after** for each scenario.
Largest-free-block and fragmentation indication only if your tooling
supports it (`ESP.getFreeHeap()`/`heap_caps_get_largest_free_block` if
you add a temporary diagnostic build — note that doing so would be a
source change outside this plan's scope; prefer read-only Serial
inspection of existing logs where possible).

| Test ID | Scenario | Known major allocations to watch |
|---|---|---|
| HW-MEM-001 | Idle, connected (WiFi+MQTT, UI idle) | baseline WiFi/MQTT stack usage |
| HW-MEM-002 | Text burst (Section 10 HW-MQTT-010) | dynamic MQTT receive-queue payloads (should return to baseline after drain) |
| HW-MEM-003 | Enigma encrypt/decrypt | stack-scoped only, no known persistent allocation |
| HW-MEM-004 | Number Guessing / Friend chunk exchange | chunk reassembly buffers |
| HW-MEM-005 | Private Radio call | **~64 KB pre-buffer** (`kPreBufferMaxFrames=100` × frame size, freed on call end/Maintenance Mode entry), jitter buffer (3 slots), I2S DMA buffers, UDP socket |
| HW-MEM-006 | Broadcast Radio | smaller footprint than Private (no pre-buffer/jitter/STUN/UDP) |
| HW-MEM-007 | Race Mode (Room voice active) | same broadcast-shaped audio path |
| HW-MEM-008 | OTA in progress | TLS (`WiFiClientSecure`, typically the single largest consumer on ESP32 Arduino), HTTPClient, 4 KB download chunk buffer (`kDownloadChunkSize`), mbedTLS SHA-256 context — cross-reference against the 80 KB provisional guard (`kMinFreeHeapBytes`) checked *after* Maintenance Mode quiesces Radio/MQTT |

Do not claim any of these "hardware-safe" until actually measured — this
section defines what to measure, not a pass/fail threshold beyond what
source already gates (the 80 KB OTA guard).

---

## SECTION 22 — Build 1 No-Update Test

Server currently (as of this writing) advertises Build 1 itself
(`version=1.0.0 build=1`, matching the running device) — no change made
by this plan.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-OTA-NOUPD-001 | Settings → System → Firmware Update → Check for Update | `runCheckForUpdateBlocking()`: `manifest.build(1) <= FW_BUILD_NUMBER(1)` → `UP_TO_DATE` | Screen shows `Firmware is up to date` |
| HW-OTA-NOUPD-002 | Confirm no side effects | No `OtaHttps::downloadAndInstall()` call, no `Update.begin()`, no reboot | Device returns to normal operation immediately |

---

## SECTION 23 — Publish Build 2 (procedure only — DO NOT execute yet)

Per `OTA_RELEASE_README.md`'s mandatory publish order:

1. Upload `firmware-1.0.1-build2.bin` to the HTTPS host at
   `/morse-buddy-ota/releases/1.0.1/firmware-1.0.1-build2.bin`.
2. Verify the URL returns HTTP 200.
3. Verify its `Content-Length` header equals **1128816** bytes.
4. Download it back and independently recompute SHA-256; confirm it
   equals `17e8f101076adc2609775d36d3d204a1730f08e75415879d599b03dae839adf3`.
5. **Only then** replace `manifest.txt` at
   `https://sarawutekcrane.github.io/morse-buddy-ota/manifest.txt` with
   the Build 2 manifest (`version=1.0.1 build=2 hardware=MORSE_BUDDY_ESP32_114_V1
   size=1128816 sha256=17e8f1... path=/morse-buddy-ota/releases/1.0.1/firmware-1.0.1-build2.bin`).
6. Verify the manifest is reachable and reads back correctly.
7. Only now proceed to Section 24 on a real device.

Never publish the manifest before the binary is confirmed reachable —
this ordering exists specifically so a device can never discover a
release whose binary isn't there yet.

---

## SECTION 24 — OTA Build 1 → Build 2

Only after Section 23 has actually been executed for real (not part of
this plan's own scope).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-OTA-12-001 | Preconditions: WiFi connected, NTP synced, battery ≥30%, sufficient heap | All four gates checked in `beginInstallFlow()` before `enterMaintenanceMode()`/heap check | Confirm each gate individually if practical (e.g. deliberately fail one, confirm the correct rejection screen, then fix it and retry) |
| HW-OTA-12-002 | Confirm Update | `enterMaintenanceMode()`: `Sleep::setSuspended(true)`, `RadioTransport::setMaintenanceModeActive(true)` (force-stops any call, clears receiver-busy), `stopCurrentToneSound()`, `MqttManager::setMaintenanceModeActive(true)` (drains receive queue, best-effort OFFLINE publish, disconnects) | All four effects observed: no sleep, any active Radio call force-ends, silence, MQTT disconnects on this device (other devices see it go offline) |
| HW-OTA-12-003 | Manifest fetch over HTTPS | TLS cert chain validated against pinned ISRG Root X1 (`ota_cert.h`), never `setInsecure()` | Fails closed if cert is ever wrong — do not test with an untrusted host |
| HW-OTA-12-004 | Firmware download | Content-Length checked == manifest size before `Update.begin()`; SHA-256 computed while streaming into the inactive OTA partition | Progress shown, no full-image RAM buffering |
| HW-OTA-12-005 | SHA-256 match | `Update.end(true)` finalizes, selects new boot partition | `esp_ota_set_boot_partition()` succeeds |
| HW-OTA-12-006 | Metadata persisted before reboot | `otaPrevBuild=1`, `otaTgtBuild=2`, `otaPending=true` written in that exact order (pending last) | Confirm via a controlled power-loss test later (Section 30), not required to directly observe here |
| HW-OTA-12-007 | Reboot | `ESP.restart()` | Device comes back up |
| HW-OTA-12-008 | Post-boot: `PENDING_VERIFY` state | `runPostOtaValidation()` runs in `Ota::serviceInit()` | Confirm via Serial: `post-update health check passed; build marked valid` |
| HW-OTA-12-009 | Confirm running build | Firmware Update screen shows `Current: v1.0.1 Build: 2` | Matches Build 2 identity |
| HW-OTA-12-010 | Confirm `otaPending` cleared | `esp_ota_mark_app_valid_cancel_rollback()` called | No further rollback risk for this build |
| HW-OTA-12-011 | NVS preserved | `mb_wifi`, `mb_group`, `mb_recent`, `mb_enigma`, `mb_notify`, `mb_solo`, `mb_core` (minus the 4 `ota*` keys which are expected to change) all intact | See Section 31 |
| HW-OTA-12-012 | LittleFS preserved | Message history, pending Outbox intact | See Section 31 |

---

## SECTION 25 — Publish Build 3 (procedure only)

Identical firmware-first/manifest-last process as Section 23, using:

```
Version 1.0.2
Build 3
SHA-256: b9d1c7adc91336939ea15badd349bbed3041f571c9c26308ca740ce996e7ba92
path=/morse-buddy-ota/releases/1.0.2/firmware-1.0.2-build3.bin
```

---

## SECTION 26 — OTA Build 2 → Build 3 (slot-direction verification)

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-OTA-23-001 | Record running slot **before** (should be whichever of `ota_0`/`ota_1` Build 2 landed on in Section 24) | `esp_ota_get_running_partition()->label` | Recorded |
| HW-OTA-23-002 | Perform the OTA (same procedure as Section 24, Build 2→3) | Target is the **other** slot (`esp_ota_get_next_update_partition()` always returns the inactive one) | Recorded target label differs from the running label in step 1 |
| HW-OTA-23-003 | Reboot, confirm running slot **after** | Now the slot that was inactive in step 1 | Proves the *other* direction (`ota_1`→`ota_0` or vice versa depending on step 1's result) actually works, not just one direction |
| HW-OTA-23-004 | Confirm Build 3 identity | `Current: v1.0.2 Build: 3` | Matches |
| HW-OTA-23-005 | Confirm validation status | Same health-check-passed / `otaPending` cleared flow as Section 24 | Repeat slot-switch proven safe in both directions |

---

## OTA TEST MANIFEST SWAP PROCEDURE (read before running Section 27)

**Corrected in this revision.** An earlier version of this document
stated that OTA failure fixtures could be tested via a separate
"scratch manifest path," implying the device could be pointed at some
alternate URL for testing. **That is incorrect and has been removed.**
The production firmware has no manifest-selection mechanism: `Ota`'s
`kBaseUrl`/`kManifestPath` (`src/ota/ota_config.h`) are compile-time
constants, and `runCheckForUpdateBlocking()`/`fetchManifest()` always
fetch exactly one URL:

```
https://sarawutekcrane.github.io/morse-buddy-ota/manifest.txt
```

There is no build flag, Settings screen, or Serial command that changes
this. The **only** way to exercise a Section 27 fixture on real hardware
is to temporarily overwrite that exact live file with the fixture's
`manifest.txt`, run the one test it is for, and then restore the real
manifest before doing anything else. This is a live, shared, public
resource — every device configured to check for updates (test hardware
or otherwise) will see whatever is published there for as long as it is
live. Treat every fixture deployment as a small, deliberate outage
window on production infrastructure, not a passive read.

### 1. Pre-test verification

1. `curl -s https://sarawutekcrane.github.io/morse-buddy-ota/manifest.txt`
   and save the output to a local file (e.g. `known_good_manifest.txt`).
   Do this **even if you believe you already know what is live** — this
   is your only proof of the exact bytes to restore afterward.
2. Confirm the saved manifest is well-formed (`MBOTA1` magic, all 6
   mandatory fields, plausible `version=`/`build=`) and matches whichever
   build is the current intended server baseline (Build 1 at the time of
   this writing; see the note at the end of this procedure for later in
   the sequence).
3. Confirm you have exactly one fixture selected from `ota_test_cases/`
   for this run — never queue multiple fixtures for one swap.

### 2. Fixture deployment

4. Copy the selected fixture's `ota_test_cases/<NN_name>/manifest.txt`
   over the live file at `/morse-buddy-ota/manifest.txt` in the GitHub
   Pages repo (commit + push, or the repo's normal publish path) —
   nothing else in the repo changes. For fixtures that also need a real
   firmware binary reachable (01, 02, 13, 14, 15 per Section 27's table),
   confirm that binary is already hosted at the path the fixture's
   manifest references **before** publishing the manifest — never publish
   a manifest whose firmware isn't there yet (same ordering rule as
   Section 23/25's real releases).
5. Publish the change.

### 3. Online verification

6. Wait for GitHub Pages to redeploy (typically under a minute, but
   confirm rather than assume).
7. `curl -s https://sarawutekcrane.github.io/morse-buddy-ota/manifest.txt`
   again and diff it byte-for-byte against the fixture file you intended
   to deploy. **Do not proceed to the hardware test until this matches
   exactly** — GitHub Pages caching can serve a stale file for a short
   window after a push.

### 4. Hardware test

8. Run **exactly one** ESP32 hardware test (the single Section 27 row
   this fixture exists for) against the now-live fixture manifest.
9. Record the result immediately per `TEST_RESULT_TEMPLATE.md` — screen
   text, Serial log lines, running build after, PASS/FAIL.

### 5. Restore procedure

10. Immediately after recording the result — before starting any other
    task, before lunch, before anything — overwrite
    `/morse-buddy-ota/manifest.txt` with the known-good manifest saved in
    step 1 (or the file described below) and publish that.

### 6. Post-restore verification

11. `curl -s https://sarawutekcrane.github.io/morse-buddy-ota/manifest.txt`
    one more time and confirm it byte-for-byte matches the known-good
    manifest from step 1. Do not consider the swap cycle complete, and do
    not start the next fixture, until this check passes.

### 7. Emergency restore procedure

If a test session is interrupted (crash, power loss, called away) while
a fixture manifest is still live: treat restoring the known-good manifest
as the single highest-priority action before anything else, including
before writing up results. If the saved `known_good_manifest.txt` from
step 1 is unavailable, use the Build 1 known-good manifest reproduced
below rather than leaving a failure fixture live indefinitely.

**NEVER leave a failure-test fixture as the live manifest after
testing.** Every device that checks for updates while a fixture is live
— including any hardware not currently part of your test session — will
see it.

### Current frozen Build 1 known-good restore manifest

```
MBOTA1
version=1.0.0
build=1
hardware=MORSE_BUDDY_ESP32_114_V1
size=1128816
sha256=3901180d5fc4ce898ea47a1f774c11b03b8a78e39108e2ab2444ce4ce8fc761f
path=/morse-buddy-ota/releases/1.0.0/firmware-1.0.0-build1.bin
```

**This is only correct while Build 1 is the intended server baseline.**
Once Section 23 (Publish Build 2) has actually been executed and Build 2
becomes the intended baseline, the restore manifest for every subsequent
fixture-swap cycle must be Build 2's real manifest instead — and again
Build 3's after Section 25. Always use step 1's freshly-`curl`'d copy of
whatever is live *before* your first swap of that session as the
authoritative restore target; the Build 1 text above is a documented
fallback for the current baseline, not a permanent constant.

---

## SECTION 27 — OTA Failure Fixtures (`ota_test_cases/01`–`17`)

For every fixture: follow the **OTA TEST MANIFEST SWAP PROCEDURE** above
in full — verify and save the current live manifest, publish the one
fixture, verify it online, run the single hardware test it exists for,
record the result, then immediately restore and re-verify the known-good
manifest before touching the next fixture. Never batch multiple fixtures
into one live-manifest window. All expected behavior below is exactly
what `ota_test_cases/README.md` traced from source in the prior session
— reproduced here as hardware test entries.

| Test ID | Fixture | Starting build | Server fixture needed | Expected parser result | Screen message | Serial | Firmware HTTP? | `Update.begin`? | Flash write? | Reboot? | Running build after |
|---|---|---|---|---|---|---|---|---|---|---|---|
| HW-OTAFX-01 | `01_valid_build2` | 1.0.0/1 | manifest + real Build 2 binary hosted | OK | `New Firmware Available` → success | `manifest OK...` → `Update.end() succeeded` | YES | YES | YES | YES | 1.0.1/2 |
| HW-OTAFX-02 | `02_wrong_sha` | 1.0.0/1 | manifest + real Build 2 binary hosted | OK (rejected later) | `Update Failed / Firmware verification failed` | `calculated sha256=<real> expected=deadbeef...` → `SHA-256 mismatch` | YES | YES | YES (full download+write, then discarded) | **NO** | 1.0.0/1 (unchanged) |
| HW-OTAFX-03 | `03_wrong_hardware` | 1.0.0/1 | manifest only | OK (rejected one layer up) | `Wrong hardware` | `manifest OK: ... hardware=...V2 ...` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-04 | `04_missing_required_field` | 1.0.0/1 | manifest only | MISSING_FIELD | `Update server error` | `manifest missing mandatory field` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-05 | `05_duplicate_required_field` | 1.0.0/1 | manifest only | DUPLICATE_FIELD | `Update server error` | `manifest has duplicate field` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-06 | `06_malformed_sha` | 1.0.0/1 | manifest only | MALFORMED_SHA256 | `Update server error` | `manifest has malformed sha256` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-07 | `07_zero_size` | 1.0.0/1 | manifest only | MALFORMED_NUMBER | `Update server error` | `manifest has malformed numeric field` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-08 | `08_oversize` | 1.0.0/1 | manifest only | OK (rejected at download confirm) | offered, then `Update Failed / Firmware too large` | `manifest size=1500000 exceeds inactive partition ... size=1441792` | NO (rejected before HTTP GET for firmware) | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-09 | `09_bad_path_parent` | 1.0.0/1 | manifest only | BAD_PATH | `Update server error` | `manifest has unsafe path` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-10 | `10_bad_path_absolute_url` | 1.0.0/1 | manifest only | BAD_PATH | `Update server error` | `manifest has unsafe path` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-11 | `11_same_build` | **1.0.0/1** | manifest only | OK | `Firmware is up to date` | `manifest OK: version=1.0.0 build=1 ...` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-12 | `12_downgrade` | **1.0.1/2** (device must already be on Build 2) | manifest only | OK | `Firmware is up to date` | same log as #11 | NO | NO | NO | NO | 1.0.1/2 (unchanged) |
| HW-OTAFX-13 | `13_firmware_404` | 1.0.0/1 | manifest + confirm the referenced filename genuinely does not exist | OK (rejected on firmware fetch) | offered, then `Update Failed / Secure connection failed` | `HTTP GET returned 404` | YES (returns 404) | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-14 | `14_content_length_mismatch` | 1.0.0/1 | manifest + real Build 2 binary hosted | OK (rejected on Content-Length check) | offered, then `Update Failed / Connection lost` | `Content-Length=1128816 does not match manifest size=1130000` | YES (200 received) | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-15 | `15_valid_build3` | **1.0.1/2** | manifest + real Build 3 binary hosted | OK | success | same shape as #01 | YES | YES | YES | YES | 1.0.2/3 |
| HW-OTAFX-16 | `16_unsupported_schema` | 1.0.0/1 | manifest only | UNSUPPORTED_SCHEMA | `Update server error` | `unsupported manifest schema` | NO | NO | NO | NO | 1.0.0/1 |
| HW-OTAFX-17 | `17_oversized_manifest_line` | 1.0.0/1 | manifest only | FIELD_TOO_LONG | `Update server error` | `manifest field too long` | NO | NO | NO | NO | 1.0.0/1 |

PASS criteria for every row: observed screen text and Serial line match
the table exactly, and the device is left in a normal, usable state
with the "Running build after" value confirmed via the Firmware Update
screen. Tests 02, 13, 14 require a live HTTPS server (per
`ota_test_cases/README.md`'s own caveat) — a manifest file alone cannot
exercise them.

---

## SECTION 28 — Automatic Rollback

**Corrected in this revision.** An earlier version of this section said
no induction method existed and marked it TBD. **A dedicated induction
method now exists** and is documented below — none of the `HW-RB-*`
entries in this section are TBD any longer.

Traced end-to-end from `ota_manager.cpp::runPostOtaValidation()`
(audited in a prior session turn, unchanged since):

- `esp_ota`'s `PENDING_VERIFY` state is checked independently every
  boot; combined with this device's own `otaPending`/`otaTgtBuild`
  bookkeeping, a genuine "just installed, health check due" boot is
  distinguished from an ambiguous one — an ambiguous state is resolved
  by marking the running build valid **without ever taking an
  automatic-rollback action**, specifically to prevent slot oscillation.
- On FAIL: `otaLastFail` is persisted **before**
  `esp_ota_mark_app_invalid_rollback_and_reboot()` is called.
- On the recovery boot (back on the previous build): matching
  `prevBuild`/`lastFailBuild`/`targetBuild` clears `otaPending`,
  **retains** `otaLastFail`, and takes no further rollback action.

### Induction method: dedicated test firmware (Build 9001)

A separate, isolated Git branch — `test/ota-rollback-health-fail`,
branched from the frozen `v1.0.0-build1` tag (commit
`8236e98644075b1bd5ed329fc8eb7b916c4e9531`) — carries a **TEST ONLY,
never-merge-into-production** firmware image built solely to exercise
this section:

| Field | Value |
|---|---|
| Version | `1.0.0-RBTEST` |
| Build | `9001` |
| Hardware | `MORSE_BUDDY_ESP32_114_V1` (unchanged) |
| Firmware size | `1128800` bytes |
| SHA-256 | `74825f657e4442add91bbf165d2ac567b1f11d11e9a850a57d1a9c989aef02bd` |
| Server firmware path (once published) | `/morse-buddy-ota/test/rollback/firmware-rbtest-build9001.bin` |

Build 9001 exists specifically so a persisted `otaLastFail=9001` can
never be confused with, or affect, production Build 2 (`build=2`) or
Build 3 (`build=3`) — see PASS criteria below.

**What the test firmware actually changes**, confirmed by source diff
against the frozen baseline (only 2 files, `firmware_version.h` and
`ota_manager.cpp`, touched — `runPostOtaValidation()` itself,
`verifyRollbackLater()`, the anti-loop metadata logic, `partitions.csv`,
and NVS layout are all byte-for-byte unchanged from Build 1):

- `runHealthCheck()` still computes and logs the real
  `nvsOk`/`fsOk`/`heapOk` result exactly as production does, but under a
  branch-only `OTA_TEST_FORCE_HEALTH_CHECK_FAIL` compile-time constant
  (no runtime, remote, MQTT, or web trigger — it cannot be toggled from
  a running device) it then unconditionally returns `false` and prints:
  ```
  [TEST] FORCED OTA HEALTH CHECK FAILURE
  ```
- This is only reached in the exact same circumstance the real check
  would run in — a genuine `PENDING_VERIFY` boot with matching OTA
  metadata — so ordinary boot behavior is unaffected on this test image,
  and the test hook **never calls rollback directly**. The existing,
  unmodified `runPostOtaValidation()` receives `healthy == false` from
  this call exactly as it would from a real failure, and takes its own
  normal production path from there: persists `otaLastFail = 9001`,
  logs `[ota] post-update health check FAILED`, then calls the real
  `esp_ota_mark_app_invalid_rollback_and_reboot()`.

**This test firmware must never be flashed directly via USB as a
substitute for Build 1**, and must never be merged into the production
branch. It is reached only via a real OTA hop from a running Build 1
device, exactly like any other release — see the sequence below.

### Rollback test sequence (future hardware procedure)

Prerequisites before starting: an ESP32 is running the frozen Build 1
image (known healthy, per Section 22's no-update test having already
passed), and the live manifest at `/morse-buddy-ota/manifest.txt` has
been restored and verified as the Build 1 manifest per the **OTA TEST
MANIFEST SWAP PROCEDURE** (before Section 27). This section reuses that
same swap-and-restore discipline — Build 9001 is never left as the live
manifest any longer than the single test requires.

1. Upload the RBTEST binary to the server path above, then verify the
   firmware URL returns HTTP 200 (same ordering rule as every other
   release: firmware reachable before any manifest references it).
2. Verify its `Content-Length` header equals **1128800** bytes.
3. Download it back and independently recompute SHA-256; confirm it
   equals `74825f657e4442add91bbf165d2ac567b1f11d11e9a850a57d1a9c989aef02bd`.
4. Temporarily replace the root live manifest
   (`/morse-buddy-ota/manifest.txt`) with the Build 9001 manifest:
   ```
   MBOTA1
   version=1.0.0-RBTEST
   build=9001
   hardware=MORSE_BUDDY_ESP32_114_V1
   size=1128800
   sha256=74825f657e4442add91bbf165d2ac567b1f11d11e9a850a57d1a9c989aef02bd
   path=/morse-buddy-ota/test/rollback/firmware-rbtest-build9001.bin
   ```
5. Verify the online manifest via `curl` byte-for-byte before touching
   the device (same rule as the swap procedure — never assume, always
   verify).
6. On the ESP32 running Build 1, open Settings → System → Firmware
   Update → Check for Update.
7. Confirm Build 9001 (`1.0.0-RBTEST`) is offered as `New Firmware
   Available`.
8. Confirm the update ("Update").
9. Confirm the firmware downloads and writes successfully (progress
   shown, `Update.end()` succeeds — same happy-path mechanics as
   Section 24, just with this manifest).
10. Confirm the device reboots into Build 9001 in `PENDING_VERIFY`
    state (same `esp_ota` state Section 24's Build 2 install reaches).
11. Confirm Serial contains exactly:
    ```
    [TEST] FORCED OTA HEALTH CHECK FAILURE
    ```
12. Confirm the existing, unmodified `runPostOtaValidation()` reports
    the health failure: Serial shows `[ota] post-update health check
    FAILED` immediately after line 11's output.
13. Confirm the bootloader/application rollback path returns the device
    to Build 1: `esp_ota_mark_app_invalid_rollback_and_reboot()` fires,
    device reboots, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (confirmed
    real on this pinned toolchain) selects the previous slot.
14. Confirm the running version/build is `1.0.0` / `Build 1` again on
    the Firmware Update screen.
15. Confirm `otaLastFail` (NVS `mb_core`/`otaLastFail`) now reads `9001`
    — check via the same mechanism used for HW-OTA-12-006/HW-RB earlier
    audits (a temporary Serial print or the Firmware Update screen's own
    "previously failed" warning in step 17).
16. Confirm the **next** normal reboot (plain power cycle, not another
    OTA attempt) stays on Build 1 and does **not** bounce back to Build
    9001 — repeat across ≥5 power cycles, watching for any
    `ota_0`↔`ota_1` oscillation.
17. While the server still advertises Build 9001 (manifest not yet
    restored), open Check for Update again and confirm the
    `UPDATE_AVAILABLE_PREV_FAILED` screen (`This firmware previously
    failed` / `Retry Update` / `Cancel`, default `Cancel`) appears —
    this is the same source path exercised by
    `runCheckForUpdateBlocking()`'s `lastFail`/`g_lastManifest.build`
    comparison used throughout Section 27.
18. Immediately restore the intended known-good server manifest (Build
    1's, per the OTA TEST MANIFEST SWAP PROCEDURE — **never leave Build
    9001 live longer than this one test requires**).
19. Verify the restored manifest online via `curl`, byte-for-byte,
    before considering the test session's manifest swap complete.
20. Confirm production Build 2 is not poisoned by the failed Build 9001:
    with the live manifest now advertising a real Build 2 (whenever that
    section of testing is reached), confirm Check for Update on a Build
    1 device offers plain `UPDATE_AVAILABLE` for Build 2 (not the
    `_PREV_FAILED` variant) — `otaLastFail=9001` only ever matches a
    manifest whose `build==9001`, per `g_lastManifest.build == lastFail`
    in `runCheckForUpdateBlocking()`, so an unrelated build number can
    never trigger that warning by coincidence.

### PASS criteria

- **Automatic rollback occurred**: step 13's reboot lands back on Build
  1 without any manual rollback action (`Update.rollBack()` from
  Section 29 is never invoked in this sequence).
- **Previous Build 1 remained bootable**: step 14 confirms the Firmware
  Update screen reports `1.0.0` / `Build 1`, and the device is otherwise
  fully usable (not stuck in a recovery/error state).
- **No `ota_0`/`ota_1` oscillation**: step 16 holds across ≥5 power
  cycles with no bounce back to Build 9001.
- **`last_failed_ota_build` = 9001**: step 15's NVS read confirms this
  exact value, distinct from any production build number.
- **Production Build 2/3 remain eligible later**: step 20 confirms a
  real Build 2 (or Build 3) offer is never suppressed or mis-flagged as
  previously-failed because of the unrelated Build 9001 failure record.
- **Persistent NVS/LittleFS data remain intact**: repeat the Section 31
  persistence table (Settings, WiFi slots, Family Groups, Recent
  contacts, Enigma keys, Notifications, Solo state, Text history,
  pending Outbox message) across this entire sequence — the RBTEST
  install and the subsequent rollback each only ever write to an OTA app
  partition, never to `nvs`/`littlefs`, exactly like every other OTA
  hop in this document.

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-RB-000 | Confirm starting condition: device on known-healthy Build 1, live manifest verified as Build 1 | Preconditions match Section 22/OTA TEST MANIFEST SWAP PROCEDURE state | Confirmed before proceeding to HW-RB-001 |
| HW-RB-001 | Sequence steps 1–2: RBTEST firmware URL returns HTTP 200, `Content-Length` = 1128800 | Firmware reachable at the expected size before any manifest references it | Both checks pass |
| HW-RB-002 | Sequence step 3: download RBTEST binary, recompute SHA-256 | Matches `74825f657e4442add91bbf165d2ac567b1f11d11e9a850a57d1a9c989aef02bd` | Exact match |
| HW-RB-003 | Sequence steps 4–5: swap live manifest to Build 9001, verify online via `curl` | Manifest matches the fixture byte-for-byte | Confirmed before touching the device |
| HW-RB-004 | Sequence steps 6–7: Check for Update on the Build 1 device | Build 9001 offered as `New Firmware Available` | Correct version/build shown |
| HW-RB-005 | Sequence steps 8–9: confirm Update, observe download/write | `Update.end()` succeeds, no error screen | Progress completes normally |
| HW-RB-006 | Sequence step 10: reboot | Device reboots into Build 9001, `esp_ota` reports `PENDING_VERIFY` | Confirmed via the same mechanism as HW-OTA-12-008 |
| HW-RB-007 | Sequence step 11: inspect Serial immediately after reboot | Serial contains exactly `[TEST] FORCED OTA HEALTH CHECK FAILURE` | Line present, verbatim |
| HW-RB-008 | Sequence step 12: confirm `otaLastFail` persisted before rollback | Serial shows `[ota] post-update health check FAILED`; NVS write happens before the rollback call (source-level fact, `nvsSetLastFailBuild()` precedes `esp_ota_mark_app_invalid_rollback_and_reboot()`) | Ordering confirmed via Serial sequence |
| HW-RB-009 | Sequence step 13: device reboots after `esp_ota_mark_app_invalid_rollback_and_reboot()` | Bootloader selects the previous slot (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, confirmed real on this pinned toolchain) | Device boots Build 1 again |
| HW-RB-010 | Sequence step 14: confirm running identity | Firmware Update screen shows `Current: v1.0.0` / `Build: 1` | Matches Build 1 exactly |
| HW-RB-011 | Sequence step 15: read `otaLastFail` | NVS `mb_core`/`otaLastFail` = 9001 | Exact value confirmed |
| HW-RB-012 | Sequence step 16: repeat power cycles ≥5× | Stays on Build 1 every time | No `ota_0`↔`ota_1` oscillation observed |
| HW-RB-013 | Sequence step 17: Check for Update while server still advertises 9001 | `UPDATE_AVAILABLE_PREV_FAILED` screen: `This firmware previously failed` / `Retry Update` / `Cancel`, default `Cancel` | Warning shown correctly, default selection is Cancel |
| HW-RB-014 | Sequence step 18: restore known-good manifest immediately | Live manifest matches the intended baseline again | Restore performed without delay |
| HW-RB-015 | Sequence step 19: verify restored manifest via `curl` | Byte-for-byte match to the intended known-good manifest | Confirmed online before ending the test session |
| HW-RB-016 | Sequence step 20: confirm Build 2/3 not poisoned | A later, real Build 2/3 offer shows plain `UPDATE_AVAILABLE`, never `_PREV_FAILED` | `otaLastFail=9001` never matches an unrelated build number |

---

## SECTION 29 — Manual Rollback

Source: `ota_manager.cpp`'s `performManualRollback()`, corrected to use
`Update.canRollBack()`/`Update.rollBack()` exclusively (never
`esp_ota_get_last_invalid_partition()`).

| Test ID | Procedure | Expected result | PASS criteria |
|---|---|---|---|
| HW-MRB-001 | After a successful OTA (Section 24/26), open Firmware Update screen | `Update.canRollBack()` true → `Rollback to Previous` item appears on the MAIN screen | Item visible only when a valid previous slot exists |
| HW-MRB-002 | Select it | `Rollback Firmware?` confirm, default `Cancel` | Confirm default selection is Cancel, not Rollback |
| HW-MRB-003 | Confirm Rollback | `Update.rollBack()` called | `Restarting...` shown, device reboots |
| HW-MRB-004 | Post-reboot | Boots the previous image | Firmware Update screen shows the prior version/build |
| HW-MRB-005 | Confirm `otaLastFail` **not** touched | Manual rollback path never writes any `ota*` NVS key (locked requirement 12, audited) | `otaLastFail` unchanged from before the manual rollback |
| HW-MRB-006 | Perform a normal OTA again after a manual rollback | Works exactly as Section 24 | No lingering bad state from the manual rollback |

---

## SECTION 30 — Power Loss During OTA

Use a bench PSU with a hard cutoff switch if available, rather than
yanking a LiPo repeatedly. **Do not claim PASS until physically
tested** — every row below states the expected-safe outcome per source,
not a confirmed result.

| Test ID | Interruption point | Expected safe recovery (per source) |
|---|---|---|
| HW-PL-001 | **A: before firmware download begins** (mid-Maintenance-Mode-entry, before any `Update.begin()`) | Currently-running app partition never touched; reboot returns to the same running build normally |
| HW-PL-002 | **B: during download/flash write** (mid-`Update.write()` loop) | Only the **inactive** partition is being written; `esp_ota_set_boot_partition()` is never called until `Update.end(true)` succeeds after the SHA check — running build boots normally on power restore |
| HW-PL-003 | **C: after write completes but before reboot** (between `Update.end(true)` succeeding and the deliberate `delay(1200); ESP.restart();`) — narrow window, may not be practically triggerable | If power is lost after `Update.end(true)` but before the NVS metadata writes (`otaPrevBuild`/`otaTgtBuild`/`otaPending`) complete, `otaPending` can only ever end up **false** (it's written last) — device boots the new partition with `PENDING_VERIFY` but no matching bookkeeping; `runPostOtaValidation()`'s inconsistent-metadata path marks it valid and continues rather than getting stuck |
| HW-PL-004 | **D: after metadata persistence, before reboot completes** | Bookkeeping complete (`otaPending=true`), reboot resumes normally, `PENDING_VERIFY` + matching metadata → normal health-check path runs |
| HW-PL-005 | **E: during the first boot's `PENDING_VERIFY` window** (interrupt right after reboot, before health check completes) | Next boot re-runs `runPostOtaValidation()` from scratch (it isn't a one-shot latch beyond the NVS state) — either resolves normally or, if `esp_ota`'s own state still shows `PENDING_VERIFY`, is re-evaluated safely |

---

## SECTION 31 — Persistence Across OTA

Before Build 1→2 and again before Build 2→3, deliberately create:

| Item | How | Verify after OTA |
|---|---|---|
| Settings values | Change brightness, volume, WPM, sleep timeout, typing display, mute-radio | All unchanged post-OTA |
| WiFi slots | Configure 2–3 slots | All unchanged |
| Family Groups | Configure 2+ groups | All unchanged |
| Recent contacts | Exchange presence with another device | Recent list intact |
| Enigma keys | Save a sender/receive key pair | Key config intact (HW-ENIGMA-007/008) |
| Notifications | Leave an unread message unread | Badge/unread count intact |
| Solo game state | Play a few Solo attempts | History intact |
| Text history | Send/receive several messages | All messages, correct order, correct flags |
| Pending Outbox message | Send while the peer is offline, **do not let it deliver before the OTA** | Still marked pending after OTA reboot; delivers normally once online again (Outbox logic resumes automatically) |

PASS criteria: every item above identical before and after **both**
OTA hops (1→2 and 2→3), since OTA only ever writes the inactive app
partition and never NVS/LittleFS.

---

## SECTION 32 — Repeated Operation / Leak Tests

Record free heap **before the series** and **after the series** for
each. A steadily-decreasing trend across the series indicates a leak;
a single-run measurement cannot show this.

| Test ID | Procedure | Watch for |
|---|---|---|
| HW-LEAK-001 | 20× `Check for Update` with the server still advertising the current build (no update) | `WiFiClientSecure`/`HTTPClient` leak per the OTA README's own required test — each check must fully close its connection |
| HW-LEAK-002 | Repeated failed manifest checks (cycle through several `ota_test_cases/0X` fixtures, `Back` between each) | Same connection-cleanup concern |
| HW-LEAK-003 | Repeated HTTP failures (404/mismatch fixtures, several times each) | `Update.abort()` / SHA context freed every time (`mbedtls_sha256_free` on every exit path — confirmed in source) |
| HW-LEAK-004 | Repeated Radio call start/stop (≥20 cycles) | Pre-buffer/jitter buffer freed every time (`resetPrivateCall()`) |
| HW-LEAK-005 | Repeated MQTT reconnect (toggle WiFi off/on ≥10 times) | No growing queue, no leaked `MQTTClient`/`WiFiClient` state |
| HW-LEAK-006 | Repeated OTA Maintenance Mode enter/exit **without** a full OTA (e.g. enter via a Check that then fails at a parser-level fixture, so Maintenance Mode is entered and left without ever downloading) — note: Maintenance Mode is only entered on *confirmed Update*, not on Check, so this specifically means repeating full attempts that fail post-confirmation (e.g. `02_wrong_sha`, `08_oversize`, `14_content_length_mismatch`) | `leaveMaintenanceMode()` correctly restores MQTT/Radio/Sleep every time, no cumulative drift |

---

## SECTION 33 — Final End-to-End Test

**HW-E2E-001** — release-candidate integration scenario, 2–3 devices:

1. Cold boot all devices (Section 3/4).
2. WiFi connects, NTP syncs (Section 9).
3. MQTT connects, presence exchanges (Section 10).
4. Exchange Text messages, including one Everyone broadcast (Section 11).
5. Exchange an Enigma message with a real key (Section 12).
6. Play a Friend Number Guessing round (Section 13).
7. Make a Private Radio call between two devices (Section 17).
8. Run a Race Mode round with all available devices (Section 20).
9. Let one device sleep and confirm it wakes only via power cycle
   (Section 8).
10. Perform OTA Build 1→2 on at least one device (Section 24).
11. Reboot, reconnect, confirm all persistent data intact (Section 31).
12. Confirm all other devices still see it correctly (presence,
    messaging) post-OTA.

PASS criteria: every step in sequence succeeds with no manual
intervention beyond what the procedure specifies, and step 11's
persistence check passes in full. This is the release-candidate gate —
document any deviation in detail rather than a bare pass/fail.

---

## SECTION 35 — Recommended Execution Order

Chosen to minimize re-flashing and to touch the live OTA server state as
few times as possible:

1. Bench electrical inspection (Section 2's boot-strap checks, HW-BOOT-000)
2. USB flash Build 1 on all boards in use (Section 3)
3. Boot/Identity (Section 4) — do this early since it gates trusting any
   later multi-device test
4. Display/Menu/Input (Section 5)
5. Settings/NVS (Section 6)
6. Power/Battery (Section 7)
7. Sleep/Wake (Section 8)
8. WiFi/NTP (Section 9)
9. MQTT/Presence (Section 10)
10. Text Messaging (Section 11)
11. Enigma (Section 12)
12. Number Guessing (Section 13)
13. Morse Practice (Section 14)
14. Notifications/Sound (Section 15)
15. Radio audio hardware (Section 16)
16. Private Radio (Section 17)
17. STUN/UDP/MQTT fallback (Section 18)
18. Broadcast Radio (Section 19)
19. Race Mode (Section 20)
20. Memory/heap baseline measurements (Section 21, scenarios A–G; save
    scenario H for after Section 24)
21. Build 1 no-update test (Section 22) — confirm this **before**
    touching the live manifest at all
22. OTA failure fixtures that don't need a real firmware binary hosted —
    i.e. every row in Section 27 except 01/02/13/14/15 (the parser-only
    subset). Even though no `.bin` is needed for these, each one is still
    a full **OTA TEST MANIFEST SWAP PROCEDURE** cycle against the one
    real live manifest URL — save known-good, publish the one fixture,
    verify online, run the one test, restore, verify restore — one
    fixture at a time, never batched
23. Publish Build 2 (Section 23 — first real write to the live OTA host)
24. OTA Build 1→2 (Section 24), including memory scenario H now
25. Build 2 persistence checks (Section 31, first half)
26. Remaining Section 27 fixtures that need Build 2 actually hosted
    (02, 13, 14) — same one-fixture-at-a-time swap procedure; the
    restore target for these is now Build 2's real manifest, not Build 1's
27. Manual rollback test (Section 29) — while still easy to get back to
    Build 2 if needed
28. Automatic rollback (Section 28) — using the `test/ota-rollback-health-fail`
    (Build 9001) induction method now documented there; run this only
    after Build 2 is confirmed working (step 24), since it needs a
    known-healthy Build 1 device and a full manifest-swap/restore cycle
    of its own, separate from any production release
29. Power-loss tests (Section 30) — last among the risky tests, once
    everything else about OTA is known-good
30. Publish Build 3 (Section 25)
31. OTA Build 2→3 (Section 26) — proves the other slot direction
32. Section 27's remaining Build-3-dependent row (15) — same swap
    procedure; the restore target is now Build 3's real manifest
33. Repeated-operation/leak tests (Section 32)
34. Final end-to-end integration (Section 33)

This order deliberately does Identity (step 3) before any multi-device
test, does the Build-1 no-update check (step 21) before any manifest
change, and defers both rollback categories (steps 27–28) and power-loss
testing (step 29) until the "happy path" OTA is already proven — so a
failure there is unambiguously about rollback/failure-recovery, not a
basic OTA defect. Steps 22, 26, and 32 each touch the one live production
manifest repeatedly — every single fixture within those steps is its own
complete OTA TEST MANIFEST SWAP PROCEDURE cycle, restored before the next
one begins.
