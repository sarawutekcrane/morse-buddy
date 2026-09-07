# Morse Buddy — Day 1 Quick Start

Purpose: the minimum sequence to get from "boards just arrived" to "first
Build 1 device confirmed alive and correctly identified," before starting
the full `MASTER_HARDWARE_TEST_CHECKLIST.md`. This is a condensed subset of
that document — it does not replace it. Do not skip to feature testing
before finishing this sequence.

Scope: this quick start assumes Build 1 (`FW_VERSION=1.0.0`,
`FW_BUILD_NUMBER=1`) is the firmware you flash first. Do not flash Build 2
or Build 3 directly — they exist only as OTA targets and must be reached by
OTA update from a running Build 1, per Sections 23–26 of the master
checklist.

## 0. Before touching any board

- [ ] Read `hardware_test/MASTER_HARDWARE_TEST_CHECKLIST.md` Section 1
      (equipment) and Section 2 (pin map) at least once.
- [ ] Confirm you have at least 2 assembled boards if you intend to test
      Text Messaging / Presence / Private Radio / Race on Day 1 (1 board is
      enough for boot/identity/UI/solo-feature checks only).
- [ ] Confirm the repository is on `claude/new-session-23k78b` and clean
      (`git status`) — do not test against a dirty working tree.

## 1. Wiring inspection (per board, before power)

- [ ] Visually check every connection against `MASTER_HARDWARE_TEST_CHECKLIST.md`
      Section 2's pin table: display (SPI + DC/CS/RST), DOT/DASH buttons on
      GPIO21 (with an external combining/selection circuit if both buttons
      share one line per your build's schematic — verify against your own
      hardware docs, this is outside firmware scope), rotary encoder
      CLK/DT/SW on GPIO16/33/25, speaker driver on GPIO5, battery divider
      enable on GPIO26 and ADC sense on GPIO34.
- [ ] Confirm nothing is wired to GPIO2, GPIO15 (strapping pins, used here
      for TFT DC/CS — HARDWARE VALIDATION REQUIRED, see Section 2) beyond
      the display itself.
- [ ] Confirm no unintended load on GPIO0 at power-on (boot mode strap).

## 2. USB detection

- [ ] Connect one board via USB.
- [ ] Confirm the host OS enumerates a serial port (e.g. `/dev/ttyUSB0` or
      `/dev/ttyACM0` on Linux, `COMx` on Windows).
- [ ] If no port appears: check cable (data-capable, not charge-only),
      check CP210x/CH340 driver installation. This is a hardware/driver
      issue, not a firmware issue — do not proceed until resolved.

## 3. First upload

- [ ] `pio run -t upload` (or your IDE's equivalent) against Build 1
      source, targeting the detected port.
- [ ] Confirm upload completes without verification errors.
- [ ] This is HW-BOOT-000 through HW-BOOT-003 in the master checklist —
      record results there, not here.

## 4. First serial monitor

- [ ] Open serial monitor at 115200 baud immediately after upload.
- [ ] Confirm boot log appears with no crash/reboot loop.
- [ ] Confirm `Identity::init()` logs a non-zero, non-`000000000000`
      device ID (this is the Phase 0–5 audit fix — verifying it here first
      is the single highest-value Day 1 check, since every multi-device
      feature depends on it).

## 5. First boot on display

- [ ] Confirm the display initializes and shows the main menu.
- [ ] Confirm all 4 inputs (DOT, DASH, encoder rotate, encoder press)
      produce visible menu movement/selection.

## 6. Minimum Day 1 test set

Once 1–5 pass, run these from the master checklist, in this order, before
moving to any networked or multi-device feature:

1. Section 3 (remaining USB flash tests, HW-BOOT-004 through HW-BOOT-007)
2. Section 4 (Identity tests, HW-ID-001 through HW-ID-003 — single-device
   persistence checks only; defer HW-ID-004+ multi-device checks until a
   second board is flashed)
3. Section 5 (Display/Menu/Input tests, HW-UI-001 through HW-UI-005)
4. Section 6 (NVS persistence — at least one settings round-trip,
   HW-NVS-001)
5. Section 7 (Power/Battery — HW-PWR-001, confirm a plausible battery
   percentage reads at all)

Stop here for Day 1 unless time remains. Continue with WiFi/MQTT, second-
board identity/multi-device tests, and all remaining sections per the
master checklist's Section 35 recommended execution order.

## Do not on Day 1

- Do not flash Build 2 or Build 3 directly.
- Do not touch the OTA server or `manifest.txt`.
- Do not attempt rollback or power-loss-during-OTA testing before the
  happy-path OTA flow (Sections 22–26) is proven on hardware.
- Do not modify firmware source to work around a failure — record it as a
  FAIL using `TEST_RESULT_TEMPLATE.md` and move on; source changes are out
  of scope for this test plan.
