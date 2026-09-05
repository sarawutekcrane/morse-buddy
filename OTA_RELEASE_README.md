# Morse Buddy OTA Release Process

Phase 5 firmware updates are pulled by the device over HTTPS from a static
file host. There is no build server, no database, and no dynamic API —
publishing a release means uploading two files.

## 1. Build

```
pio run
```

Confirm it reports `SUCCESS` and check the reported firmware size against
the OTA slot (see "Build-time verification" in the Phase 5 report).

## 2. Bump the version/build number

Edit `src/ota/firmware_version.h`:

* `FW_VERSION` — human-facing semantic version (e.g. `"1.1.0"`).
* `FW_BUILD_NUMBER` — **must** be strictly greater than the previous
  release's. This is the only value devices actually compare; never reuse
  or lower it, and never rely on `FW_VERSION` alone.

Rebuild (`pio run`) after bumping these — the packaging script reads the
compiled firmware's *source* header, not `firmware.bin` itself, so the
`.bin` and the header must be in sync.

## 3. Package the release

```
python3 tools/make_ota_release.py
```

This reads `src/ota/firmware_version.h` and `partitions.csv`, validates
`.pio/build/morse_buddy/firmware.bin` against the OTA slot size, computes
its SHA-256, and writes:

```
ota_release/
  firmware-<version>-build<build>.bin
  manifest.txt
```

The script fails loudly (exit code 1) if the firmware exceeds the OTA slot,
and warns loudly if it's within 15% of the slot's capacity.

## 4. Publish — firmware first, manifest last

This order is mandatory (Phase 5 section 38): a device that fetches a
manifest before the firmware it points at is uploaded will get a
`Connection lost` / `404` failure instead of a clean "no update available."

1. Upload `ota_release/firmware-<version>-build<build>.bin` to your HTTPS
   host at the path printed by the script (matches the manifest's `path=`
   field, itself relative to `OtaConfig::kBaseUrl` in `src/ota/ota_config.h`).
2. Verify that URL is reachable over HTTPS with a normal `GET` and returns
   a correct `Content-Length` (no chunked Transfer-Encoding — the device
   firmware rejects that for the firmware binary).
3. **Only then** upload/replace `ota_release/manifest.txt` at
   `OtaConfig::kBaseUrl` + `OtaConfig::kManifestPath`.

## Host requirements (Phase 5 section 39)

* HTTPS with a valid, non-expired certificate.
* Normal static `GET` support (any standard web server / object storage
  with a CDN in front is sufficient).
* Correct `Content-Length` on the firmware response.
* No authentication required for Phase 5.
* No dynamic server-side logic of any kind is needed — this is a static
  file host, not an API.

## Root CA maintenance (Phase 5 section 40)

The trusted root CA is compiled into the firmware (`src/ota/ota_cert.h`),
not fetched at runtime. If the hosting provider ever rotates its CA
infrastructure, ship the new root through an ordinary firmware release
*before* the old CA a fielded device is relying on expires or is
distrusted — otherwise that device can never reach the OTA host again
(never work around this with `setInsecure()`).

## Before any of this works at all

`src/ota/ota_config.h`'s `kBaseUrl` and `src/ota/ota_cert.h`'s
`kOtaRootCaPem` currently hold placeholder values. Until both are replaced
with a real HTTPS origin and its matching root CA, the Firmware Update
screen on the device shows "Update Server / Not Configured" and never
attempts a network connection — this is intentional, not a bug.
