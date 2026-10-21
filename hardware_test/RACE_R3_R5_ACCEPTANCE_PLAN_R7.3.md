# Two-device hardware acceptance plan, revision 7.3 (for review)

**Status: for review only.**
- Don't start until the heap-diagnostics candidate below has been reviewed
  and, preferably, committed and pushed (section A.1), and this plan is
  approved.
- This plan supersedes combined checklist revision 6. The test items from
  revision 6 are kept unchanged in substance (sections 1–7).

**Changes from revision 7.2 (documentation only; firmware unchanged):**
- **Installation (0.5)** no longer calls `pio run -t upload`, which can
  rebuild.
  - 0.3 stages the four images (bootloader, partition table,
    `boot_app0.bin`, firmware) into `$OUT/images` with a manifest.
  - 0.5 checks every image before writing: the manifest lists exactly the
    four, the hashes match, and each fits its slot.
  - It writes them with esptool, using the offsets and flash settings
    confirmed from PlatformIO's own upload command, then runs
    `verify_flash`.
- **Rollback:** the backup-unavailable alternative is removed. Only the two
  device-bound backup restores remain, and both check the device's MAC and
  its current partition table.
- **Old-layout stop:** the explicit STOP message is now the last line
  printed, after the device's table.
- **Serial capture (0.6):** `log2file` was found to write into
  `$REPO/logs`, so `script` now records the session to `OUT` instead.
  Checked from a directory outside the project with the selected
  environment.

**Changes from revision 7.1 (documentation only; the firmware candidate is
unchanged):**
- `min_since_boot` is described correctly: it is ESP-IDF's sum of
  per-region low-water marks, which can fall at different times. H-5 no
  longer treats M1 as the exact burst minimum, or attributes a decrease to
  the burst alone.
- Every build, backup, installation and rollback command block is now a
  fail-closed Bash script:
  - `set -euo pipefail`;
  - build failures kept through `tee`;
  - stops on any checksum, size, layout, MAC or verification failure;
  - backup and rollback-image sizes checked before any `write_flash`;
  - backups bound to their device by MAC address.
- "Application-only" wording is replaced by the actual regions written
  (bootloader, partition table, `otadata`, application) and those kept
  (NVS, LittleFS).
- Section 0 is renumbered: 0.2 shell requirements, 0.3 build, 0.4 backup,
  0.5 installation, 0.6 serial capture, 0.7 after installation.

**Changes from revision 7:**
- Option A.1(a) was chosen: the build under test is the **heap-diagnostics
  candidate** (section 0.1). It is not `fb126cc` alone.
- The two stale code comments are corrected in the candidate.
- Section 8 now says exactly which serial line each heap reading comes from,
  and when it is collected.
- Sections 0 and 9 now give the exact backup, installation and rollback
  commands. The preferred rollback (9.2) restores everything below NVS from
  the device's backup and keeps saved data.

---

## A. Before hardware testing

1. **Review the uncommitted heap-diagnostics candidate.** Then commit and
   push it, so the tester builds from an exact commit (recommended).
   - If it is tested uncommitted instead, the tester must apply the
     incremental diff onto `fb126cc` and check the `race.cpp` hash given in
     section 0.1.
   - The host-test changes (room_test U20–U20d, fakes, README, mutations)
     aren't part of the firmware.
2. **Library resolution check.** `platformio.ini` uses caret ranges for
   Adafruit GFX (`^1.11.9`), ST7735/ST7789 (`^1.10.3`) and BusIO
   (`^1.16.1`). The evaluated builds resolved:

   | Library | Version |
   |---|---|
   | MQTT | 2.5.2 |
   | Adafruit GFX | 1.12.6 |
   | Adafruit ST7735 and ST7789 | 1.11.0 |
   | Adafruit BusIO | 1.17.4 |

   The toolchain was `framework-arduinoespressif32 @ 3.20017.241212`,
   toolchain-xtensa-esp32 8.4.0+2021r2-patch5, on espressif32@6.13.0
   (PlatformIO Core 6.2.0).

   - Compare the flashing machine's "Dependency Graph" with this. If it
     differs, record that; the binary isn't the evaluated build.
   - The history-browsing text-fit host checks used GFX 1.11.9 font metrics,
     so section 2 checks fit on screen.
   - Pinning exact versions would be a `platformio.ini` change. It is not
     proposed here.
3. **Firmware version is unchanged** (`FW_VERSION` "1.0.4",
   `FW_BUILD_NUMBER` 5). The device screen can't identify the build; the
   recorded `firmware.bin` SHA-256 does.
   - **During the run, don't open Firmware Update or accept any OTA
     update.** An OTA install would replace the build under test.
4. **Decide later** whether the diagnostics stay in the firmware after
   acceptance:
   - one line per minute, plus one at boot and on each Room entry;
   - removing them later makes a build that differs from the tested one.

5. **Firmware code comment (open; the firmware is unchanged in this
   revision).** The comment above the diagnostics in `race.cpp` says "the
   lowest free bytes since boot". Like the earlier plan text, that is
   imprecise: the value is the sum of per-region low-water marks (8.2).
   - The logged label `min_since_boot` is only a name, and this plan
     defines it.
   - Correcting the comment changes the `race.cpp` hash in 0.1. The
     compiled code would not change, but it would need a fresh build record.
     Your call whether to do it before the commit.

---

## 0. Identification, backup and installation (A and B)

### 0.1 Build under test: heap-diagnostics candidate

| Field | Value |
|---|---|
| Repository and branch | sarawutekcrane/morse-buddy, `test/hw-functional-ui-fix4` |
| Base commit | `fb126cce6230c0a43116cf0c537516c0d4ca7edf` (pushed; includes R3–R5 production commit `032b592e332c3c8cec27add16673ffcce7abf598`) |
| Candidate | `fb126cc` plus the uncommitted heap-diagnostics diff. Only `src/core/race.cpp` changes in the firmware |
| Incremental diff (`git diff HEAD -- src`) | SHA-256 `4f5a305e75dc466c0e096ec680d402b6e90acb22583eb48f922f749dc4d7d674` |
| Candidate `src/core/race.cpp` file | SHA-256 `23fdcc5148dccb38b8bac2b1e7498de3e1824457414a595736e62f7df5fe2b4b` (fb126cc's is `d979e7b8…`) |
| Full production diff from `73884d9` (same method as the reviewed `f8ee888e…`) | SHA-256 `7fd9e8032cbc05a88b3b98a3a6f5fe08b9158797662aa5315b8c5878cea70987` |
| If committed | Record the new commit hash here: ______ |
| Batch contents | Friend F1–F3 (`89bc226` and the earlier 2C-2 commits), history browsing (`d6aa7b7`), R1/R2 (`73884d9`), R3–R5 (`032b592`), heap diagnostics (candidate) |
| Evaluated build | `pio run -e morse_buddy`: exit 0, no warnings; RAM 124,220 B (37.9%); flash 1,191,437 B (82.6%) |
| Static DRAM | 124,228 of 124,580 B used in `dram0_0_seg`: **352 B headroom** (`fb126cc`: 360 B). The diagnostics add 8 B of `.bss`; their text is in flash |
| Engine size on target | `sizeof(RaceProtocol::Engine)` = 2,108 B (heap, allocated once at Race service init) |
| `firmware.bin` from the evaluated build | SHA-256 `dfc1df6badded1a601cc6063222c1e0fa381a497d2796929678e2b76beb4a5fd`. This is environment-specific; the flashing machine's hash is the one recorded for the run |

### 0.2 Shell requirements and per-device settings

Every block below is a **Bash** script that fails closed:
- `set -euo pipefail`: any failing command, unset variable or failing stage
  of a pipe (including a build piped through `tee`) stops the script.
- Every check is a command that stops it on failure: `sha256sum -c`, `cmp`,
  size tests, `grep -q`, and `esptool.py verify_flash`.
- **If a script stops, don't continue by hand.** Record the output and stop
  the run.
- **Requirements:** Linux (or WSL2 with the USB serial port passed through)
  with GNU coreutils (`sha256sum`, `wc -c`, `dd`, `cmp`), util-linux
  `script` (for 0.6), and a PlatformIO installation that has built this
  project once, so `tool-esptoolpy` is installed.
- **Pipes:** no stage stops reading early (no `head` or `awk … exit`
  after esptool or pio), so a successful command can't be cut off by
  SIGPIPE and stop a script.
- **esptool exit status:** I checked that `pio pkg exec -p tool-esptoolpy --
  esptool.py …` returns esptool's status (an unreachable port gave status 2).
- **Simulation check:** I ran these scripts against a simulated esptool and
  pio (`host_tests/race_r3_r5/plan_sim/`; 29 cases, each passing or stopping as
  intended; results in `host_tests/race_r3_r5/evidence/plan_r7.3_simulation_results.txt`). It is not a hardware run, and it
  doesn't model esptool's bootloader-header update. The cases covered:
  - every refusal before installation or rollback: wrong device, wrong
    `DEV`, truncated or tampered backup, a backup rejected for its layout,
    each of the four images altered, a manifest missing `boot_app0.bin`, an
    oversize firmware with a regenerated manifest, and a changed partition
    table. Each stopped with no `write_flash` call;
  - installation with no `.pio` in the clone and `pio run` forbidden;
  - a write failure through `tee`, and silent corruption caught by
    `verify_flash`;
  - NVS, LittleFS and `ota_1` unchanged by installation, and NVS and
    LittleFS by 9.2.
- **Only 0.3 builds.** Installation (0.5) and rollback (9.2, 9.3) call
  esptool directly on files that have already been checked. They never call
  `pio run`, so nothing can be rebuilt or substituted at write time.

Each script takes its inputs from the environment and resolves `OUT` to an
absolute path. Set the variables once per shell with `export`:

| Variable | Meaning |
|---|---|
| `REPO` | A clean clone |
| `OUT` | An absolute path to a directory **outside** the clone, for example `$HOME/mb-acceptance`. Every image, log, hash, record and backup goes here, so the clone stays clean |
| `DEV` | `A` or `B` |
| `PORT` | That device's serial port, for example `/dev/ttyUSB0` |
| `BK` | That device's backup file from 0.4 (used in 0.5 and section 9) |

**Device binding:**
- Each backup's name and its `.meta` file carry the device's factory MAC
  address, read with `esptool.py read_mac`.
- Every script that writes to a device first reads the MAC from the device
  on `PORT`, and refuses unless it matches the backup's `.meta`.
- It also reads the device's current partition table and refuses unless it
  matches.
- So a backup can't be written to, or count as the backup of, the other
  device.

### 0.3 Build and stage the images (once, for both devices)

This is the only step that builds. It copies the four images the upload
writes, including `boot_app0.bin`, into `$OUT/images` and records them in
`manifest.sha256`. Installation writes exactly those files.

```bash
#!/usr/bin/env bash
set -euo pipefail
: "${REPO:?set REPO}"
OUT=$(mkdir -p "${OUT:?set OUT}" && cd "$OUT" && pwd)
cd "$REPO"
EXPECT_RACE_CPP=23fdcc5148dccb38b8bac2b1e7498de3e1824457414a595736e62f7df5fe2b4b
BOOT_APP0=~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
IMG="$OUT/images"
[ ! -e "$IMG" ] || { echo "STOP: $IMG exists (images are staged once; use a new OUT)"; exit 1; }

git fetch origin test/hw-functional-ui-fix4
# Committed candidate (recommended): set COMMIT to the reviewed commit.
git checkout --detach "${COMMIT:?set COMMIT to the commit under test}"
[ -z "$(git status --porcelain)" ] || { echo "STOP: working tree not clean"; exit 1; }
# --- Uncommitted candidate instead (only if not committed): replace the two
#     commands above (checkout and clean-tree check) with:
# git checkout --detach fb126cce6230c0a43116cf0c537516c0d4ca7edf
# [ -z "$(git status --porcelain)" ] || { echo "STOP: working tree not clean"; exit 1; }
# echo "4f5a305e75dc466c0e096ec680d402b6e90acb22583eb48f922f749dc4d7d674  heap-diag-incremental.diff" | sha256sum -c -
# git apply --check heap-diag-incremental.diff
# git apply heap-diag-incremental.diff
# [ "$(git status --porcelain)" = " M src/core/race.cpp" ] || { echo "STOP: unexpected changes"; exit 1; }
git rev-parse HEAD | tee "$OUT/build-commit.txt"
echo "$EXPECT_RACE_CPP  src/core/race.cpp" | sha256sum -c -

LOG="$OUT/build-$(date +%Y%m%d-%H%M%S).log"
pio run -e morse_buddy -t clean
pio run -e morse_buddy 2>&1 | tee "$LOG"        # pipefail: a build failure stops here
grep -q '\[SUCCESS\]' "$LOG"
grep -E '^(RAM|Flash):' "$LOG"
sed -n '/Dependency Graph/,/^Building/p' "$LOG"

B=.pio/build/morse_buddy
mkdir "$IMG"
cp "$B/bootloader.bin" "$B/partitions.bin" "$B/firmware.bin" "$BOOT_APP0" "$IMG/"
cd "$IMG"
# Sizes: each image must fit before the next offset of the upload layout (0.5).
for f in bootloader.bin partitions.bin boot_app0.bin firmware.bin; do [ -s "$f" ] || { echo "STOP: empty $f"; exit 1; }; done
[ "$(wc -c < bootloader.bin)" -le $((0x8000 - 0x1000)) ] || { echo "STOP: bootloader too large"; exit 1; }
[ "$(wc -c < partitions.bin)" -eq 3072 ]               || { echo "STOP: partitions.bin size"; exit 1; }
[ "$(wc -c < boot_app0.bin)" -eq $((0x2000)) ]         || { echo "STOP: boot_app0.bin size"; exit 1; }
[ "$(wc -c < firmware.bin)" -le $((0x160000)) ]        || { echo "STOP: firmware larger than ota_0"; exit 1; }
sha256sum bootloader.bin partitions.bin boot_app0.bin firmware.bin | tee manifest.sha256
chmod a-w bootloader.bin partitions.bin boot_app0.bin firmware.bin manifest.sha256
echo "IMAGES STAGED in $IMG"
```

- Compare the RAM/Flash lines and the Dependency Graph with 0.1 and A.2.
- Record `$OUT/build-commit.txt` and `$OUT/images/manifest.sha256` in the
  results header.
- Don't rebuild between the two devices. Both get the files in
  `$OUT/images`.

### 0.4 Before flashing: device-bound backup and layout check (read-only, per device)

```bash
#!/usr/bin/env bash
set -euo pipefail
: "${DEV:?A or B}" "${PORT:?}"
OUT=$(cd "${OUT:?set OUT}" && pwd)
IMG="$OUT/images"
ESPTOOL=(pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 --port "$PORT")
FLASH_BYTES=$((0x400000))
(cd "$IMG" && sha256sum -c manifest.sha256)

"${ESPTOOL[@]}" flash_id 2>&1 | tee "$OUT/flash_id-$DEV.txt"
grep -q 'Detected flash size: 4MB' "$OUT/flash_id-$DEV.txt" || { echo "STOP: flash is not 4MB"; exit 1; }
MAC=$("${ESPTOOL[@]}" read_mac | awk '/^MAC:/ && !m {print $2; m=1}')
[[ "$MAC" =~ ^([0-9a-f]{2}:){5}[0-9a-f]{2}$ ]] || { echo "STOP: could not read MAC"; exit 1; }

BK="$OUT/backup-$DEV-${MAC//:/}-$(date +%Y%m%d-%H%M%S).bin"
[ ! -e "$BK" ] || { echo "STOP: $BK exists"; exit 1; }
"${ESPTOOL[@]}" --baud 460800 read_flash 0 "$FLASH_BYTES" "$BK"
[ "$(wc -c < "$BK")" -eq "$FLASH_BYTES" ] || { echo "STOP: backup size"; exit 1; }
"${ESPTOOL[@]}" --baud 460800 verify_flash 0 "$BK"   # read-only: compares the device with the file
sha256sum "$BK" > "$BK.sha256"

# Layout check: the device's partition table must equal the staged partitions.bin.
dd if="$BK" of="$BK.ptable" bs=4096 skip=8 count=1 status=none     # 0x8000-0x8FFF
[ "$(wc -c < "$BK.ptable")" -eq 4096 ] || { echo "STOP: ptable extract size"; exit 1; }
sha256sum "$BK.ptable" >> "$BK.sha256"
GEN=~/.platformio/packages/framework-arduinoespressif32/tools/gen_esp32part.py
if ! cmp -s -n 3072 "$BK.ptable" "$IMG/partitions.bin"; then
  echo "Device's partition table (if readable):"
  python3 "$GEN" "$BK.ptable" || true
  echo "STOP: device partition layout differs from partitions.csv"
  exit 1
fi
python3 "$GEN" "$BK.ptable"
# Written last: only a backup that passed every check above gets a .meta,
# and installation and rollback refuse a backup without one.
printf 'device=%s\nmac=%s\nflash_bytes=%s\n' "$DEV" "$MAC" "$FLASH_BYTES" > "$BK.meta"
echo "BACKUP OK for device $DEV ($MAC): $BK"
echo "export BK=$BK"
```

- **If the layout check stops the script:** compare the printed table with
  `partitions.csv` (`nvs` at 0x2D0000, `littlefs` at 0x2E0000). On an older
  layout, flashing would make saved NVS and LittleFS data inaccessible (see
  `OTA_RELEASE_README.md`). Don't flash.
- Keep `$BK`, `$BK.sha256`, `$BK.meta` and `$BK.ptable` together, per
  device. They are the only way back to the exact previous firmware (9.2)
  and data (9.3).
- **Note each device's existing data:**
  - which groups it belongs to;
  - a few chat conversations, with their last message;
  - the Solo record;
  - an existing Friend game, if any.

  0.7 checks these survive.

### 0.5 Installation (USB write of the staged images: bootloader, partition table, otadata and application; NVS and LittleFS not written)

The command and its settings are exactly the ones PlatformIO's upload uses
for this project. I confirmed them with `pio run -t upload -v` against a
port that doesn't exist, so nothing was written. The only difference is that
the files are the staged copies.

```bash
#!/usr/bin/env bash
set -euo pipefail
: "${DEV:?}" "${PORT:?}" "${BK:?the backup of this device, from 0.4}"
OUT=$(cd "${OUT:?set OUT}" && pwd)
IMG="$OUT/images"
ESPTOOL=(pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 --port "$PORT")
FLASH_ARGS=(--flash_mode dio --flash_freq 40m --flash_size 4MB)
PAIRS=(0x1000 "$IMG/bootloader.bin" 0x8000 "$IMG/partitions.bin" 0xe000 "$IMG/boot_app0.bin" 0x10000 "$IMG/firmware.bin")

# Gate 1: the staged images are exactly the four recorded in 0.3, unaltered and within size.
[ "$(sed 's/^[0-9a-f]\{64\}  //' "$IMG/manifest.sha256" | LC_ALL=C sort | tr '\n' ' ')" = "boot_app0.bin bootloader.bin firmware.bin partitions.bin " ] \
  || { echo "STOP: manifest does not list exactly the four images"; exit 1; }
(cd "$IMG" && sha256sum -c manifest.sha256)
[ "$(wc -c < "$IMG/bootloader.bin")" -le $((0x8000 - 0x1000)) ] || { echo "STOP: bootloader size"; exit 1; }
[ "$(wc -c < "$IMG/partitions.bin")" -eq 3072 ]                 || { echo "STOP: partitions.bin size"; exit 1; }
[ "$(wc -c < "$IMG/boot_app0.bin")" -eq $((0x2000)) ]           || { echo "STOP: boot_app0.bin size"; exit 1; }
[ "$(wc -c < "$IMG/firmware.bin")" -le $((0x160000)) ]          || { echo "STOP: firmware size"; exit 1; }
# Gate 2: a verified, intact backup of THIS device exists.
[ -f "$BK.meta" ] || { echo "STOP: $BK has no .meta (not accepted by 0.4)"; exit 1; }
sha256sum -c "$BK.sha256"
[ "$(wc -c < "$BK")" -eq $((0x400000)) ] || { echo "STOP: backup size"; exit 1; }
grep -qx "device=$DEV" "$BK.meta" || { echo "STOP: backup is not for device $DEV"; exit 1; }
# Gate 3: the device on $PORT is the backed-up device, and its partition table is still the staged one.
MAC=$("${ESPTOOL[@]}" read_mac | awk '/^MAC:/ && !m {print $2; m=1}')
grep -qx "mac=$MAC" "$BK.meta" || { echo "STOP: device on $PORT ($MAC) is not the backed-up device"; exit 1; }
CUR="$OUT/current-ptable-$DEV-${MAC//:/}-$(date +%Y%m%d-%H%M%S).bin"
"${ESPTOOL[@]}" --baud 460800 read_flash 0x8000 0x1000 "$CUR"
[ "$(wc -c < "$CUR")" -eq 4096 ] || { echo "STOP: ptable read size"; exit 1; }
cmp -s -n 3072 "$CUR" "$IMG/partitions.bin" || { echo "STOP: device partition table differs from the staged one"; exit 1; }

ULOG="$OUT/install-$DEV-$(date +%Y%m%d-%H%M%S).log"
"${ESPTOOL[@]}" --baud 460800 --before default_reset --after hard_reset \
  write_flash -z "${FLASH_ARGS[@]}" "${PAIRS[@]}" 2>&1 | tee "$ULOG"
[ "$(grep -c 'Hash of data verified' "$ULOG")" -eq 4 ] || { echo "STOP: not every image was hash-verified"; exit 1; }
"${ESPTOOL[@]}" --baud 460800 verify_flash "${FLASH_ARGS[@]}" "${PAIRS[@]}" 2>&1 | tee -a "$ULOG"
(cd "$IMG" && sha256sum -c manifest.sha256)   # the files written are still the recorded ones
echo "INSTALLED on device $DEV ($MAC)"
```

**What is written:**

| Address | Image (from `$OUT/images`) |
|---|---|
| 0x1000 | `bootloader.bin` (esptool sets its header to dio / 40m / 4MB, as PlatformIO's upload does; `verify_flash` with the same settings compares the same bytes) |
| 0x8000 | `partitions.bin` (identical to the device's, checked by gate 3) |
| 0xE000 | `boot_app0.bin` (`otadata`: resets the OTA selection, so the device boots `ota_0`) |
| 0x10000 | `firmware.bin`, into `ota_0` |

- **Not written:** `ota_1` (0x170000), `nvs` (0x2D0000) and `littlefs`
  (0x2E0000). Chats, settings, Wi-Fi credentials, Solo and Friend records
  are kept.
- **Never run:** `pio run -t upload`, `pio run -t erase`,
  `esptool.py erase_flash` or `erase_region`, `pio run -t uploadfs`, or any
  `write_flash` at 0x2D0000 or above.
- If a device was running from `ota_1` after an earlier OTA, it now runs
  from `ota_0`. That's expected; `ota_1` is left as it was.

**Data changes made by the new firmware itself:**
- It creates the NVS namespace `mb_race`, holding the room records
  (`rm_member`, `rc_commit`, room slots).
- It adds `raceSeq` to `mb_core`, copied once from `idCounter`.
- Nothing existing is removed or rewritten.

**Upgrade effect:** rooms from the old firmware aren't carried over. The old
firmware kept no room data in NVS, and old and new firmware use different
Race topics. Both devices must run the candidate (coordinated upgrade, B9).

### 0.6 Serial capture (whole run, one terminal per device)

```bash
#!/usr/bin/env bash
set -euo pipefail
: "${REPO:?}" "${DEV:?}" "${PORT:?}"
OUT=$(cd "${OUT:?set OUT}" && pwd)
cd "$OUT"
LOGF="$OUT/monitor-$DEV-$(date +%Y%m%d-%H%M%S).log"
script -q -f -e -c "pio device monitor -d \"$REPO\" -e morse_buddy -p \"$PORT\" --filter time" "$LOGF"
```

- `-d "$REPO" -e morse_buddy` selects the project's environment, so its
  `monitor_speed` (115200) is used. The monitor's banner shows it
  ("Terminal on … | 115200 8-N-1").
- **Don't use the `log2file` filter.** It writes to `logs/` inside the
  project (`$REPO/logs`), not to `OUT`. `script` records the session to
  `$LOGF` in `OUT` instead.
- Start the monitor **before** the boot you want to capture. Opening it may
  reset the board; to capture a boot, press the board's reset button with
  the monitor open.
- Quit with Ctrl+C. Start it again for each new session; each start gives a
  new `$LOGF`.
- **Heap lines:** `tr -d '\r' < "$LOGF" | grep '\[RACE\]\[HEAP\]'`. The
  capture has CR line endings.

### 0.7 After installation (A and B)


1. **U-1.** Both devices show "Invite to Play" with no room. The first boot
   shows no "Room data damaged" notice.
2. **U-2.** The Race Room action is **not** "Race unavailable". The serial
   log shows `engine-alloc-ok size=2108` (H-1).
3. **U-3.** The data noted in 0.4 is intact: groups, chat history, Solo
   record and Friend game.
4. **U-4.** Text, Enigma and Friend chat still send and receive (smoke test;
   section 7 repeats it at the end).

---

## What the logs can and can't show (unchanged from revision 6)

A QoS 1 publish is logged (`[PERF][MQTT] publish group=… bytes=… result=…`)
**only when it fails or takes at least 20 ms**. The line has **no topic**,
and a publish attempted while disconnected isn't logged at all. So:
- publish counts **can't** be verified from logs;
- whether a packet was sent **can't** be established on hardware either.

No visible effect on the other device shows only that **no effect was
observed**, not that nothing was sent. Items below record what the peer
showed; "not sent" is a host-test claim only.

**Devices:** two, called A and B. Section 6 is optional coverage for three
or more devices. Nothing here requires buying another device.

**Before starting:**
- A and B in the same family group. One device also has a second group.
- Serial logs captured on both devices for the whole run (section 0.6),
  with each device's id and display name noted.

**Deferred:** Room push-to-talk audio waits until the microphone and speaker
hardware is ready. Only check that pressing DOT on the Room doesn't break
anything else (item 4.6). Audio-path heap measurement (master checklist
HW-MEM-005 to -007) is deferred with it.

---

## 1. Friend F1–F3 (A and B)

1. **F1.** On A, choose friend B:
   - **Set Manually / Randomize / Open Chat** appear directly (no
     Play/Cancel);
   - Open Chat opens the existing conversation with B;
   - Back from the choice returns to the friend list.
2. **F1.** From an existing Friend, Text and Enigma chat with B, start a
   challenge:
   - the choice has **no Open Chat** row;
   - Back returns to that same chat;
   - after sending, Back from Quick Switch returns to where the flow
     started.
3. **F1.** Double-press DOT quickly on the choice screen: only one flow
   starts.
4. **F2.** Send a challenge and a result normally: **no status row** appears
   on success.
5. **F2** (opportunistic). A failure text only shows if a save or publish
   actually fails. If one appears, record its exact wording. Don't try to
   force it with Wi-Fi-off; that isn't reliable.
6. **F3.** With A's chat with B open, B completes A's challenge: A's chat
   refreshes to the solved state without leaving and re-entering. Repeat
   with a Text chat and an Enigma chat with B open.
7. **F3, draft preservation.** In A's Text and Enigma chats with B, type a
   partial draft and leave it unsent. When the refresh happens, the draft is
   still there, unchanged.
8. **F3, selected-history preservation.** In A's chat with B, scroll up and
   select an older message. When the refresh happens, the same message
   stays selected.

## 2. History browsing (A and B)

1. **Solo**, with at least 6 guesses and two digits entered:
   - turn down past the lowest selectable digit: "Guess:" turns yellow;
   - turn further: newest row, scrolling to the oldest;
   - turn back: the same digits and preview return.
   - Repeat with 0 already used, and with 4 digits entered.
   - Check that text fits (see A.4).
2. **Short while browsing** (Solo, Friend, Race): returns to editing; no
   submit, no Exit prompt. In Race, record that B showed no change (not
   proof that nothing was sent).
3. **Quick double Short while browsing:** the second Short submits (complete
   draft) or opens Exit (incomplete). This is expected; nothing suppresses
   it.
4. **DOT** held at the boundary and turned down: stays on the digit; the
   release confirms it. DOT pressed while browsing and released after
   returning (also held over 0.5 s): nothing changes.
5. **Friend:** browse a long game; leave and reopen. ("Last guess not
   saved" is opportunistic only.)
6. **Race:**
   - more than 8 guesses: the oldest 2 are unreachable;
   - B wins while A is browsing: A shows "Solved by B: <secret>", then the
     Room; browsing ends cleanly;
   - Back to the Room and re-enter Guess.

## 3. R1: participant list (A and B)

1. **With A and B in a room:**
   - both rows visible, each with its score at the right edge;
   - the title shows no range (2 participants);
   - turning right moves the ">" from the action to A's row, then B's row,
     then "Leave room", and stops there (the list scrolls to show it);
   - turning left returns to the action.
2. **Short with a participant focused:** only returns focus to the action.
   Record that B showed no round start.
3. **B joins while A has a participant focused:** the focus stays on the
   same person.
4. **Long display names and "(You)":** truncated cleanly, never overlapping
   the score.
5. **Scrolling past 2 rows** needs three or more participants: see
   section 6.

## 4. R2: Start Round (A and B)

1. **A (holding the start right) presses Start Round:**
   - A goes straight to Guess, once;
   - B auto-enters Guess when the round arrives (if B is on the Room).
2. **Press Start Round twice quickly:** one Guess screen, no Exit prompt.
3. **Back from Guess:** the Room shows "Continue"; Continue works.
4. **Start Round while not Ready** (immediately after power-up or
   reconnect): "Connecting, try again"; no navigation.
5. **A lost publish** is host-tested only (the change is committed and
   republished until A sees its own copy). On hardware, record only whether
   B eventually showed the round after any connection hiccup.
6. **DOT on the Room** (PTT path): no crash, no focus change, no action.
   Audio quality is deferred.

## 5. R3–R5, approved behaviour (design revision 8) (A and B)

0. **Create and join:**
   - A presses "Invite to Play": A's Room shows A with score 0, "> Start
     Round" and a "Leave room" row;
   - B gets the Race notification (Training Game badge) and B's Room lists
     "Join A (1)"; turn to it and press: B shows "Joining...", then
     "Waiting for A"; A lists B.
1. **Back:** from the Room to the main menu and return. Same room, same
   members, same scores, same action. No Leave occurred (B still lists A).
2. **Sleep, reboot, temporary Wi-Fi loss:** not a Leave.
   - After A sleeps, wakes or reboots, A is back in the same room.
   - B never shows A removed.
3. **Non-owner Leave:** on B (member), turn to "Leave room" and press: the
   prompt reads "Leave room?"; "No" changes nothing; "Yes":
   - B shows "Leaving...", then "Invite to Play";
   - A (coordinator) shows B removed after the update arrives;
   - A's room stays open.
4. **Owner Leave:** on A (coordinator), "Leave room": the prompt reads
   "Leave room?" / "Closes it for everyone"; "Yes":
   - A shows "Room closed".
   - B shows "Room closed" with the final scores listed once the update
     reaches it. If B was disconnected, check after B reconnects.
5. **Winner start right:**
   - after B wins, only B can start the next round: B shows "> Start
     Round", A shows "Waiting for B";
   - turn B off: A still waits (presence offline doesn't grant the right);
   - B turns on and explicitly Leaves: A can start.
6. **Coordinator unavailable:**
   - turn A off during a round; B solves: "Solved! 4A0B" and
     "Confirming...";
   - about 15 s later the status row shows "Waiting for A"; nothing else
     changes, however long A stays off;
   - A returns: B's result is confirmed (B's score +1 on both devices);
   - B can explicitly Leave: "Leaving..."; with A still off, "Leave anyway"
     (prompt "You may stay listed") returns B to "Invite to Play".
6a. **Reboot of a member:** B reboots during a room: "Reconnecting..." with
   only Leave available, then the room as before.
7. **Scores beside names:** identical on A and B after several rounds, after
   B reboots, and after B reconnects between rounds and during a round.
8. **Near-simultaneous solve:** one winner, the same on both devices.
9. **Second group:** while in a room, choosing the other group shows "In a
   room in another group" and doesn't mix rooms.
10. **Owner closes the room, then the other device creates a new room:**
    - A (coordinator) confirms Leave, and B shows "Room closed" with the
      final scores;
    - B creates a new room and A joins it;
    - both devices show every score as **0**;
    - play one round in the new room;
    - then, to give delayed old-room messages a chance, turn A's Wi-Fi off
      and on (or reboot A);
    - the new room's members, scores and round are unchanged on both
      devices, and no "Room closed" or old score reappears.

    This records an observed effect only. Whether any old message was
    actually delivered can't be seen on hardware; the host tests (E9) cover
    it.

**Host-tested only (not checkable on hardware; record as N/A):**
- an exhausted Race counter ("Storage error" / "Can't create a room");
- NVS write failures ("Storage error - room paused");
- damaged or inconsistent room records ("Room data damaged", "Room data
  inconsistent");
- the unchecked commit-record rewrite in FC-B1 adoption.

If any of these texts appears during the run, record it with the serial
log. It is a finding.

## 6. Optional: three or more devices (only if available)

1. **R1:** three or more participants. The title range ("1-2/3") scrolls;
   focus clamps at the last.
2. **R3–R5:**
   - late joiner between rounds and during a round;
   - a member Leave while others play on;
   - a three-way simultaneous solve.

## 7. Regressions (A and B)

1. Join, Continue, Back from Room and Guess, the Exit prompt, and protection
   against repeated Invite/Start/Join/Continue presses.
2. Text, Enigma and Friend chat send/receive smoke test.

---

## 8. Runtime heap and allocation (A and B)

### 8.1 Static DRAM headroom vs runtime heap

These are two different things, not to be confused:

| | Static DRAM headroom | Runtime heap |
|---|---|---|
| What it is | Space left in `dram0_0_seg` after the linker places all static data (`.data`, `.bss`) | Memory available to `new`/`malloc` while the firmware runs |
| Known? | **Yes, exactly, at build time:** 352 B free (124,228 of 124,580 B) for the candidate | **No.** Not measured on any device. The `[RACE][HEAP]` lines are how this plan measures it |
| What it decides | Whether the firmware links at all. Any static addition can overflow it again | Whether the 2,108 B Race engine (and later allocations: MQTT queue payloads, TLS, Radio buffers) can be allocated at run time |
| Checked by | The ESP32 build (done) | This section, on hardware |

- The engine moved from static memory to the heap in this batch. That is
  why the build fits; it also means about 2.1 KB more of the runtime heap is
  in use from boot, for the whole session.
- The host tests and build figures don't establish anything about the
  runtime heap on hardware.

### 8.2 The diagnostic line

```
[RACE][HEAP] <event> uptime_s=<s> free=<bytes> min_since_boot=<bytes> largest_block=<bytes> (bytes, internal 8-bit)
```

**Fields:**

| Field | Source | Meaning |
|---|---|---|
| `uptime_s` | `millis() / 1000` | Seconds since boot, for matching readings to checklist steps; also use the monitor's `time` filter stamp |
| `free` | `heap_caps_get_free_size(MALLOC_CAP_INTERNAL \| MALLOC_CAP_8BIT)` | Free bytes now |
| `min_since_boot` | `heap_caps_get_minimum_free_size(same)` | The **sum of each matching heap region's own low-water mark since boot** (ESP-IDF `esp_heap_caps.h`). Regions can reach their lows at different times, so this can be lower than the free total ever actually was at any one moment: a conservative ("worst case") indication, not a timestamped minimum. It never rises, and it can fall whenever any region sets a new low, whatever the cause |
| `largest_block` | `heap_caps_get_largest_free_block(same)` | The largest single allocation possible now (a fragmentation indicator) |

- **Capability:** byte-addressable internal RAM, the same for all three
  figures. The build doesn't enable PSRAM, so `new` draws from this heap.
- **Events:**

  | Event | When it is logged |
  |---|---|
  | `pre-alloc` | At Race service init (early boot, before Wi-Fi is connected), immediately before the engine allocation |
  | `engine-alloc-ok size=2108` or `engine-alloc-FAILED size=2108` | Immediately after the allocation; `size` is `sizeof(Engine)` in bytes |
  | `room` | On entering the Race Room, unless another reading was logged in the last 5 s |
  | `periodic` | From the Race service tick, when 60 s have passed since the last reading of any kind |

- **What's in the line:** nothing about rooms, guesses, secrets, names,
  credentials or messages. The line is formatted into a stack buffer: the
  diagnostic code makes no heap allocation of its own. Nothing is logged per
  loop or per packet.

### 8.3 Items, and when each reading is collected

1. **H-1. Allocation result: every boot, both devices.**
   - **Collected:** the `engine-alloc-…` line. Capture it by pressing reset
     with the monitor open (0.6).
   - **Expected:** `engine-alloc-ok size=2108`, and the Room never shows
     "Race unavailable".
   - Record every boot in the run, including the reboots in sections 5.2,
     5.6a and 7.
2. **H-2. Allocation cost: first boot after installation.**
   - **Collected:** the `pre-alloc` line and the `engine-alloc-ok` line that
     follows it.
   - **Expected:** `free` falls by about 2,108 B plus allocator overhead.
   - Other tasks can allocate between the two readings. A different drop is
     recorded and explained, not automatically a failure.
   - `largest_block` may or may not change, depending on which free region
     served the allocation.
3. **H-3. Idle connected baseline.**
   - **Setup:** after Wi-Fi and MQTT are connected (the Room is Ready), leave
     the device idle on the Race Room for at least 5 minutes, which gives at
     least 4 `periodic` lines.
   - **Collected:** the last `periodic` line (`free`, `min_since_boot`,
     `largest_block`). Cross-reference master checklist HW-MEM-001.
4. **H-4. Long session.** At least 10 rounds, joins, Leaves, and reboots of
   the other device (sections 4–5).
   - **Collected:** every `periodic` line during the session, plus the
     `room` line at each Room entry. At the end, idle on the Room for at
     least 3 minutes and take the last `periodic` line.
   - **Expected:** no steady downward trend in `free` across the idle
     readings. The engine is allocated once and never freed or re-allocated.
   - **Flag for review** if the end idle `free` is more than 1 KB below the
     H-3 baseline, or if it falls across consecutive idle readings. This is
     a review trigger, not an established limit.
5. **H-5. Text burst while in a room** (master checklist HW-MQTT-010:
   16 or more messages quickly from A to B).
   1. Reboot **B** with the monitor open, so the low-water marks restart.
   2. Wait until B is back in the room and Ready, and at least one
      `periodic` line has appeared. Its `min_since_boot` is **M0**.
   3. On B, leave the Room and re-enter it at least 5 s after the last
      reading. The `room` line is the **before** reading.
   4. Send the burst from A. Wait until B has received every message.
   5. At least 5 s later, leave the Room on B and re-enter it. The `room`
      line is the **after** reading; its `min_since_boot` is **M1**.
   - **What M0 and M1 can show:**
     - Both are sums of per-region low-water marks (8.2). Neither is the
       exact minimum free heap at any moment, and M1 is not the exact burst
       minimum.
     - If M1 < M0, at least one region set a new low-water mark between the
       two readings. That window includes the burst, but also the Room exit
       and entry, MQTT and Wi-Fi activity, and anything else running, so
       the decrease can't be attributed to the burst alone.
     - If M1 = M0, no region set a new low in that window.
   - **Record:** M0, M1 and their difference, plus the before and after
     `free` and `largest_block`, as observations.
   - **Expected:** all messages arrive; no crash or reboot; the after-burst
     `free` returns to near the before reading.
6. **H-6. Allocation failure path.** Host-tested only (room_test U18/U18b,
   and U20b for the FAILED line). Don't try to force it on hardware. Record
   N/A.

**What these readings can't show:**
- per-task stack use;
- the actual minimum free heap at any moment, or when it occurred:
  `min_since_boot` sums per-region lows that may come from different times,
  and the readings are point samples;
- which activity caused a new low-water mark;
- Wi-Fi and driver memory outside this capability;
- fragmentation beyond the largest block.

**Not covered here (needs separate approval):**
- The OTA free-heap guard: 80 KB after quiescing; the about 2.1 KB less heap
  reduces its margin. Checking it means running the OTA install flow.
- Private and Broadcast Radio buffers (about 64 KB pre-buffer), which are
  deferred with audio.

These remain unverified runtime-heap risks.

---

## 9. Rollback (only if needed)

### 9.1 Before rolling back (if the devices are working well enough)

- **Leave all rooms on both devices first:** members leave; the coordinator
  closes the room.
- **Why:** each room's last retained state stays on the broker under the
  new Race topics. The old firmware neither sees nor clears it. A room left
  open is resumed from `mb_race` if a device is upgraded again later.

### 9.2 Preferred: restore everything below NVS from this device's backup (bootloader, partition table, otadata and both application slots; NVS and LittleFS not written)

This writes back exactly bytes 0x000000–0x2CFFFF from the device's own
backup (0.4):
- bootloader;
- partition table;
- `otadata`;
- `ota_0` and `ota_1`;
- the unused gaps in between.

`nvs` (0x2D0000) and `littlefs` (0x2E0000) aren't written.

```bash
#!/usr/bin/env bash
set -euo pipefail
: "${DEV:?}" "${PORT:?}" "${BK:?the backup of this device, from 0.4}"
OUT=$(cd "${OUT:?set OUT}" && pwd)
ESPTOOL=(pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 --port "$PORT")
NVS_OFFSET=$((0x2D0000))

# Gate 1: the backup is intact, complete, and belongs to the device on $PORT.
[ -f "$BK.meta" ] || { echo "STOP: $BK has no .meta (not accepted by 0.4)"; exit 1; }
sha256sum -c "$BK.sha256"
[ "$(wc -c < "$BK")" -eq $((0x400000)) ] || { echo "STOP: backup size"; exit 1; }
grep -qx "device=$DEV" "$BK.meta" || { echo "STOP: backup is not for device $DEV"; exit 1; }
MAC=$("${ESPTOOL[@]}" read_mac | awk '/^MAC:/ && !m {print $2; m=1}')
grep -qx "mac=$MAC" "$BK.meta" || { echo "STOP: device on $PORT ($MAC) is not the backed-up device"; exit 1; }
# Gate 2: the device's current partition table equals the backup's (nvs still at 0x2D0000).
CUR="$OUT/current-ptable-$DEV-${MAC//:/}-$(date +%Y%m%d-%H%M%S).bin"
"${ESPTOOL[@]}" --baud 460800 read_flash 0x8000 0x1000 "$CUR"
[ "$(wc -c < "$CUR")" -eq 4096 ] || { echo "STOP: ptable read size"; exit 1; }
cmp "$CUR" "$BK.ptable" || { echo "STOP: current partition table differs from the backup's"; exit 1; }

# Extract the region below NVS and check it before any write.
IMG="$BK.below-nvs.bin"
dd if="$BK" of="$IMG" bs=4096 count=$((NVS_OFFSET / 4096)) status=none
[ "$(wc -c < "$IMG")" -eq "$NVS_OFFSET" ] || { echo "STOP: extracted image size"; exit 1; }
cmp -n "$NVS_OFFSET" "$IMG" "$BK" || { echo "STOP: extracted image differs from the backup"; exit 1; }

"${ESPTOOL[@]}" --baud 460800 write_flash 0 "$IMG"
"${ESPTOOL[@]}" --baud 460800 verify_flash 0 "$IMG"
echo "RESTORED below-NVS region on device $DEV ($MAC)"
```

- **Result:** the device runs exactly the firmware it ran before
  installation, from the same OTA slot.
- **Data effects:**
  - Chats, settings, Solo and Friend data are kept.
  - The old firmware ignores `mb_race` and `raceSeq`. They stay in NVS,
    using a few hundred bytes, and are harmless.
  - Rooms from the new firmware aren't visible to the old firmware.
- **Upgrading again later:**
  - `raceSeq` already exists, so it isn't copied from `idCounter` again.
    It is still at least as large as every number the new firmware handed
    out, so Race numbers stay unique.
  - Any room still recorded in `mb_race` is resumed. That's another reason
    to leave rooms first.

**No backup, no rollback in this run.** Section 0.4 must succeed for a
device before anything is written to it. If a device's backup is missing or
fails its checks, stop and report. This plan has no write path without a
device-bound backup.

### 9.3 Last resort: restore this device's full backup

```bash
#!/usr/bin/env bash
set -euo pipefail
: "${DEV:?}" "${PORT:?}" "${BK:?the backup of this device, from 0.4}"
OUT=$(cd "${OUT:?set OUT}" && pwd)
ESPTOOL=(pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 --port "$PORT")
[ -f "$BK.meta" ] || { echo "STOP: $BK has no .meta (not accepted by 0.4)"; exit 1; }
sha256sum -c "$BK.sha256"
[ "$(wc -c < "$BK")" -eq $((0x400000)) ] || { echo "STOP: backup size"; exit 1; }
grep -qx "device=$DEV" "$BK.meta" || { echo "STOP: backup is not for device $DEV"; exit 1; }
MAC=$("${ESPTOOL[@]}" read_mac | awk '/^MAC:/ && !m {print $2; m=1}')
grep -qx "mac=$MAC" "$BK.meta" || { echo "STOP: device on $PORT ($MAC) is not the backed-up device"; exit 1; }
"${ESPTOOL[@]}" --baud 460800 write_flash 0 "$BK"
"${ESPTOOL[@]}" --baud 460800 verify_flash 0 "$BK"
echo "RESTORED full backup on device $DEV ($MAC)"
```

- This restores the device exactly as it was before installation:
  bootloader, partition table, `otadata`, both application slots, NVS
  **and LittleFS**.
- **Everything saved after the backup is lost** (messages received, chat
  history, game records, settings changes). Use it only if 9.2 doesn't
  recover the device.
- Every rollback is a flash write. It needs your explicit go-ahead at that
  time.

---

## 10. Results

**Header (once per run):**

| Field | Value |
|---|---|
| Date | |
| Tester | |
| Commit under test (or `fb126cc` + diff `4f5a305e…`) | |
| `race.cpp` SHA-256 (expect `23fdcc51…2b4b`) | |
| `$OUT/images/manifest.sha256` (bootloader, partitions, boot_app0, firmware) | |
| RAM/Flash summary from the build | |
| Library versions match A.2? | yes / no (list the differences) |
| Device A: id, display name, port, backup file and SHA-256, layout matches? | |
| Device B: id, display name, port, backup file and SHA-256, layout matches? | |
| Serial capture files `$OUT/monitor-*.log` (A, B) | |
| Broker or network notes | |

**Result values:**
- **PASS:** observed exactly as expected.
- **FAIL:** observed something else; describe it.
- **BLOCKED:** couldn't be run; say why.
- **N/A:** host-tested only, or optional and not run.

Never record PASS without an actual observation. For sending items, record
what the other device showed; that is evidence of an effect, not proof of
transmission or non-transmission.

| ID | Item | Devices | Result | Time / uptime_s | Notes, observed texts, log lines |
|---|---|---|---|---|---|
| U-1 | No room after upgrade; no "Room data damaged" | A, B | | | |
| U-2 | Not "Race unavailable"; `engine-alloc-ok` | A, B | | | |
| U-3 | Saved data intact | A, B | | | |
| U-4 | Chat smoke test after upgrade | A, B | | | |
| 1.1 | F1 friend choice direct | A, B | | | |
| 1.2 | F1 from chats; Back paths | A, B | | | |
| 1.3 | F1 double DOT | A | | | |
| 1.4 | F2 no status row on success | A, B | | | |
| 1.5 | F2 failure text (opportunistic) | A, B | | | |
| 1.6 | F3 open chat refresh (Friend, Text, Enigma) | A, B | | | |
| 1.7 | F3 draft kept | A, B | | | |
| 1.8 | F3 selected history kept | A, B | | | |
| 2.1 | Solo browsing (incl. 0 used, 4 digits, fit) | A | | | |
| 2.2 | Short while browsing (Solo, Friend, Race) | A, B | | | |
| 2.3 | Quick double Short | A | | | |
| 2.4 | DOT at the boundary and while browsing | A | | | |
| 2.5 | Friend long game browse and reopen | A, B | | | |
| 2.6 | Race browsing: 8-row limit, win while browsing, re-enter | A, B | | | |
| 3.1 | Participant rows, scores, focus path | A, B | | | |
| 3.2 | Short on a participant | A, B | | | |
| 3.3 | Focus kept on join | A, B | | | |
| 3.4 | Long names and "(You)" | A, B | | | |
| 3.5 | Scrolling past 2 rows | 3+ | | | |
| 4.1 | Start Round enters Guess on A and B | A, B | | | |
| 4.2 | Double Start Round | A | | | |
| 4.3 | Back and Continue | A | | | |
| 4.4 | Not Ready: "Connecting, try again" | A | | | |
| 4.5 | Round eventually shown after a hiccup | A, B | | | |
| 4.6 | DOT on the Room | A | | | |
| 5.0 | Create and join | A, B | | | |
| 5.1 | Back is not Leave | A, B | | | |
| 5.2 | Sleep, reboot, Wi-Fi loss are not Leave | A, B | | | |
| 5.3 | Non-owner Leave | A, B | | | |
| 5.4 | Owner Leave closes the room | A, B | | | |
| 5.5 | Winner start right | A, B | | | |
| 5.6 | Coordinator unavailable; Leave anyway | A, B | | | |
| 5.6a | Member reboot | A, B | | | |
| 5.7 | Scores identical | A, B | | | |
| 5.8 | Near-simultaneous solve | A, B | | | |
| 5.9 | Second group | A | | | |
| 5.10 | Closed room, then new room; no old state returns | A, B | | | |
| 6.1 | 3+ devices: R1 range | 3+ | | | |
| 6.2 | 3+ devices: R3–R5 | 3+ | | | |
| 7.1 | Race regressions | A, B | | | |
| 7.2 | Chat smoke test at the end | A, B | | | |
| H-1 | `engine-alloc-ok size=2108` at every boot | A, B | | | boots logged: |
| H-2 | Allocation cost at first boot | A, B | | | pre free / largest; post free / largest; drop: |
| H-3 | Idle connected baseline | A, B | | | free / min_since_boot / largest_block: |
| H-4 | Long session, no downward trend | A, B | | | H-3 free; end idle free; lowest periodic free; trend: |
| H-5 | Text burst while in a room | B (A sends) | | | M0; M1; M0−M1 (not attributed); before / after free and largest_block: |
| H-6 | Allocation failure path | — | N/A | | Host-tested (U18/U18b/U20b) |
| R-1 | Rollback performed? (9.2 / 9.3 / not needed) | A, B | | | |

**Acceptance:**
- every item in sections U, 1–5 and 7 is PASS, and so are H-1 to H-5, with
  their figures recorded;
- any H-4 review flag is resolved;
- every FAIL is reported with its serial log before any fix;
- audio, the OTA heap guard and Radio buffers are explicitly out of scope
  and remain unverified.
