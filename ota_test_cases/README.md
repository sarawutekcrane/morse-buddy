# Morse Buddy OTA Failure Test Fixtures

Local-only manifest fixtures for testing the Phase 5 OTA client once ESP32
hardware is available. **Nothing here is published.** These are inputs you
point a test HTTP server (or a modified local copy of the real host layout)
at — they are never uploaded to `morse-buddy-ota` / GitHub Pages.

## How the current source actually behaves (traced from `src/ota/*`, not assumed)

**`OtaManifest::parse()`** (`src/ota/ota_manifest.cpp`):
- Empty buffer -> `BAD_MAGIC`. Buffer > 2048 bytes (`OtaConfig::kManifestMaxSize`) -> `TOO_LARGE`.
- First non-blank line must be exactly `MBOTA1`. Starts with `MBOTA` but isn't exactly `MBOTA1` -> `UNSUPPORTED_SCHEMA`. Doesn't start with `MBOTA` at all -> `BAD_MAGIC`.
- Any line (any line, known field or not) longer than 200 bytes -> `FIELD_TOO_LONG`.
- A line with no `=` is silently ignored (forward-compatibility). A line whose key isn't one of the six known field names is also silently ignored.
- Mandatory fields: `version`, `build`, `hardware`, `size`, `sha256`, `path`. A **second** occurrence of any mandatory field's key -> `DUPLICATE_FIELD`, checked the instant that second line is parsed (fields after it in the file are never reached).
- `build=`/`size=`: strict base-10 only (digits, no sign, no whitespace) via `parseStrictU32`; malformed -> `MALFORMED_NUMBER`. **`size=0` is explicitly rejected as `MALFORMED_NUMBER`** (there is no separate "zero size" result code).
- `sha256=`: must be **exactly** 64 hex characters (upper or lower case) -> otherwise `MALFORMED_SHA256`.
- `path=`: must start with `/`, must not contain `..`, must not contain `://` -> otherwise `BAD_PATH`. An absolute URL like `https://...` fails the first check anyway (doesn't start with `/`), and would also fail the `://` check independently.
- If any mandatory field was never seen by end-of-file -> `MISSING_FIELD`.
- **A parser failure of any kind never reaches the on-device UI with its specific reason.** `OtaHttps::fetchManifest()` maps *every* `ParseResult != OK` to the single `Result::MANIFEST_INVALID`, shown on-screen as the generic `Update server error`. The exact `ParseResult` (e.g. "manifest has duplicate field") is only ever visible in the Serial log line `[ota] manifest parse failed: <reason>`.

**`ota_manager.cpp::runCheckForUpdateBlocking()`** (runs when the user selects "Check for Update", well before any firmware bytes are requested):
- Hardware ID compared with `strcmp()` (case-sensitive, exact match) against `OtaConfig::kHardwareId`. Mismatch -> `Wrong hardware`, rejected here, before any firmware download is ever attempted.
- Build comparison is exactly `if (manifest.build <= FW_BUILD_NUMBER) -> "up to date"`. **This one branch covers both "exactly the same build" and "an older build" (downgrade) — there is no separate on-screen or logged "downgrade rejected" message; both show `Firmware is up to date`.** Functionally a downgrade is still refused (no download happens either way), but the two scenarios are not distinguishable from the UI or Serial alone.
- **Manifest `size=` is never checked against the OTA slot size at this stage.** An oversize manifest is offered normally as "New Firmware Available" here; the real-slot-size check only happens later.

**`OtaHttps::downloadAndInstall()`** (`src/ota/ota_https.cpp`, only reached after the user confirms "Update" and passes the battery/heap gates in `beginInstallFlow()`):
1. `manifest.size == 0 || manifest.size > updatePartition->size` -> `IMAGE_TOO_LARGE`, **before opening any HTTP connection for the firmware at all.** (`size == 0` can never actually reach this line in practice — the parser already rejects it earlier — this is defense-in-depth, not a reachable path via a real manifest.)
2. HTTP GET on `manifest.path` (built as `kBaseUrl + manifest.path`, e.g. `https://sarawutekcrane.github.io` + `/morse-buddy-ota/releases/1.0.1/firmware-1.0.1-build2.bin`). Any non-200 response (**including 404**) -> `HTTPS_CONNECT_FAILED`, shown as `Secure connection failed` — there is no distinct "not found" message on-screen even though the Serial log does show the real HTTP status code.
3. `Content-Length != manifest.size` -> `CONTENT_LENGTH_MISMATCH` (`Connection lost` on-screen), **checked immediately after the HTTP 200 and strictly before `Update.begin()` is ever called.**
4. **`Update.begin()` is only entered once (1) the partition-size check passed, (2) HTTP 200 was received, and (3) Content-Length exactly matches `manifest.size`.**
5. Firmware writing (`Update.write()`) starts inside the streaming read loop immediately after `Update.begin()` succeeds -- bytes are written to the inactive OTA partition chunk-by-chunk **while** SHA-256 is computed incrementally over the same chunks. **A wrong-SHA manifest therefore does cause a full download and a full flash write of the inactive slot** — the mismatch is only detected after every byte has been received and hashed.
6. On SHA mismatch: `Update.abort()` is called and `Update.end()` is **never reached**, so `esp_ota_set_boot_partition()` never runs. The currently-running partition stays selected; the device does not reboot into the new image.
7. Only if the SHA matches: `Update.end(true)` performs ESP32 image/header validation and calls `esp_ota_set_boot_partition()`, then the device reboots (`ESP.restart()`) into the new build's `PENDING_VERIFY` state, resolved by `runPostOtaValidation()` on the next boot.

## Files

```
ota_test_cases/
  01_valid_build2/manifest.txt
  02_wrong_sha/manifest.txt
  03_wrong_hardware/manifest.txt
  04_missing_required_field/manifest.txt
  05_duplicate_required_field/manifest.txt
  06_malformed_sha/manifest.txt
  07_zero_size/manifest.txt
  08_oversize/manifest.txt
  09_bad_path_parent/manifest.txt
  10_bad_path_absolute_url/manifest.txt
  11_same_build/manifest.txt
  12_downgrade/manifest.txt
  13_firmware_404/manifest.txt
  14_content_length_mismatch/manifest.txt
  15_valid_build3/manifest.txt
  16_unsupported_schema/manifest.txt      (additional case, see below)
  17_oversized_manifest_line/manifest.txt (additional case, see below)
  _validate_fixtures.py                   (local audit tool; re-implements
                                            the exact C++ parser algorithm
                                            and checks every fixture against
                                            its expected ParseResult)
  README.md                               (this file)
```

No firmware `.bin` is duplicated anywhere in this tree. Cases that need real
firmware bytes reference the already-prepared local release paths:
`ota_test_packages/build2/firmware-1.0.1-build2.bin` and
`ota_test_packages/build3/firmware-1.0.2-build3.bin`.

## Test matrix

| ID | Name | Starting build | Manifest `build=` | Intentionally wrong | Download? | Flash write? | Reboot? | Running build after | Expected Serial/log | PASS criteria |
|---|---|---|---|---|---|---|---|---|---|---|
| 01 | valid_build2 | 1.0.0 / 1 | 2 | nothing | YES | YES | YES | 1.0.1 / 2 | `manifest OK...` -> download progress -> `calculated sha256=... expected=...` (match) -> `Update.end() succeeded` -> next boot `post-update health check passed` | Device boots Build 2; Firmware Update screen shows `Current: v1.0.1 Build: 2` |
| 02 | wrong_sha | 1.0.0 / 1 | 2 | `sha256=` valid hex, wrong value | YES | YES (full download+write happens before the check) | **NO** | 1.0.0 / 1 (unchanged) | `calculated sha256=<real> expected=deadbeef...` -> `SHA-256 mismatch, image will not be activated` | Screen shows `Update Failed / Firmware verification failed`; Build 1 still running/bootable; no reboot occurred |
| 03 | wrong_hardware | 1.0.0 / 1 | 2 | `hardware=...V2` | NO | NO | NO | 1.0.0 / 1 | `manifest OK: ... hardware=MORSE_BUDDY_ESP32_114_V2 ...` (parses fine; rejected one layer up) | Screen shows `Wrong hardware`; no firmware HTTP request ever made |
| 04 | missing_required_field | 1.0.0 / 1 | 2 | `hardware=` line removed entirely | NO | NO | NO | 1.0.0 / 1 | `manifest parse failed: manifest missing mandatory field` | Screen shows `Update server error`; never reaches the "New Firmware Available" screen |
| 05 | duplicate_required_field | 1.0.0 / 1 | 2 | `build=2` line appears twice | NO | NO | NO | 1.0.0 / 1 | `manifest parse failed: manifest has duplicate field` | Screen shows `Update server error` |
| 06 | malformed_sha | 1.0.0 / 1 | 2 | last hex digit replaced with `g` | NO | NO | NO | 1.0.0 / 1 | `manifest parse failed: manifest has malformed sha256` | Screen shows `Update server error` |
| 07 | zero_size | 1.0.0 / 1 | 2 | `size=0` | NO | NO | NO | 1.0.0 / 1 | `manifest parse failed: manifest has malformed numeric field` | Screen shows `Update server error` (not a distinct "zero size" message) |
| 08 | oversize | 1.0.0 / 1 | 2 | `size=1500000` (> 1,441,792-byte OTA slot) | Offered at Check stage; **firmware GET never opened** | NO | NO | 1.0.0 / 1 | after confirming Update: `download aborted: manifest size=1500000 exceeds inactive partition '...' size=1441792` | "New Firmware Available" is shown, but confirming Update immediately produces `Update Failed / Firmware too large` with no network attempt for the firmware |
| 09 | bad_path_parent | 1.0.0 / 1 | 2 | `path=` contains `..` | NO | NO | NO | 1.0.0 / 1 | `manifest parse failed: manifest has unsafe path` | Screen shows `Update server error` |
| 10 | bad_path_absolute_url | 1.0.0 / 1 | 2 | `path=` is a full `https://` URL to a different origin | NO | NO | NO | 1.0.0 / 1 | `manifest parse failed: manifest has unsafe path` | Screen shows `Update server error`; confirms no cross-origin firmware fetch is possible |
| 11 | same_build | **1.0.0 / 1** | 1 | nothing (this is Build 1's own real manifest) | NO | NO | NO | 1.0.0 / 1 | `manifest OK: version=1.0.0 build=1 ...` | Screen shows `Firmware is up to date` / `Current: v1.0.0` |
| 12 | downgrade | **1.0.1 / 2** | 1 | nothing (identical manifest content to #11 — the distinguishing factor is that the *device* must already be flashed to Build 2) | NO | NO | NO | 1.0.1 / 2 (unchanged) | same `manifest OK...` log | Screen shows `Firmware is up to date` / `Current: v1.0.1` — confirms build 1 is never offered as an update to a build-2 device |
| 13 | firmware_404 | 1.0.0 / 1 | 2 | `path=` points at a filename that must not exist on the live host | Offered at Check stage; firmware GET returns 404 | NO | NO | 1.0.0 / 1 | `downloading firmware: .../firmware-1.0.1-build2-MISSING.bin -> ...` -> `download aborted: HTTP GET returned 404` | Screen shows `Update Failed / Secure connection failed` (current code does not special-case 404 into a distinct message) |
| 14 | content_length_mismatch | 1.0.0 / 1 | 2 | `size=1130000`, but the real hosted Build 2 file is 1,128,816 bytes | Offered at Check stage; firmware GET succeeds (200) | **NO** (rejected before `Update.begin()`) | NO | 1.0.0 / 1 | `download aborted: Content-Length=1128816 does not match manifest size=1130000` | Screen shows `Update Failed / Connection lost`; confirm via Serial that this fires before any `Update.write()` log line |
| 15 | valid_build3 | **1.0.1 / 2** | 3 | nothing | YES | YES | YES | 1.0.2 / 3 | same shape as #01 | Device boots Build 3; screen shows `Current: v1.0.2 Build: 3` |
| 16 | unsupported_schema *(extra)* | 1.0.0 / 1 | 2 | magic line is `MBOTA2` instead of `MBOTA1` | NO | NO | NO | 1.0.0 / 1 | `manifest parse failed: unsupported manifest schema` | Screen shows `Update server error`; confirms forward-incompatible schemas fail safe rather than being guessed at |
| 17 | oversized_manifest_line *(extra)* | 1.0.0 / 1 | 2 | one extra unknown `note=` line, 225 bytes (> 200-byte line cap) | NO | NO | NO | 1.0.0 / 1 | `manifest parse failed: manifest field too long` | Screen shows `Update server error`; confirms the parser's bounded-line guarantee holds even for a line it would otherwise ignore |

**Tests 13 and 14 genuinely require hardware, and a live host serving the real file layout** — a manifest fixture alone fully specifies the *input*, but exercising the actual behavior needs a real HTTP round trip (a real 404, and a real Content-Length that disagrees with the manifest) against an actual server. They cannot be executed against a manifest file in isolation. Test 02 also genuinely needs hardware to observe the "full download completes, then is rejected and never activated" behavior, since a static analysis can only confirm what the code *would* do, not that a real download-then-reject cycle leaves Build 1 bootable. Every other case (03-12, 16, 17) is a pure parser/comparison decision and is fully exercised by local static analysis alone (see `_validate_fixtures.py`) — hardware is only needed there to confirm the on-screen text and Serial output match, not the accept/reject decision itself.

## Additional case discovered from the implementation

Two cases beyond the requested 15 were added (`16`, `17`) because they exercise `ParseResult` values (`UNSUPPORTED_SCHEMA`, `FIELD_TOO_LONG`) that no other required case reaches. Both are real, distinct code paths in `ota_manifest.cpp`, not invented behavior.
