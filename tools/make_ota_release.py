#!/usr/bin/env python3
"""Package a PlatformIO build into a Morse Buddy OTA release (Phase 5
section 37).

Reads firmware version/build/hardware straight from
src/ota/firmware_version.h (single source of truth -- never re-type these
by hand and risk drift from what was actually compiled), locates the
PlatformIO build output, validates its size against the OTA slot size in
partitions.csv, computes its SHA-256, and writes:

    ota_release/firmware-<version>-build<build>.bin
    ota_release/manifest.txt

in the exact strict-ASCII MBOTA1 format ota_manifest.cpp parses.

Usage:
    python3 tools/make_ota_release.py

No network access and no PlatformIO invocation happen here -- run
`pio run` yourself first; this script only packages an existing build.
"""

import hashlib
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_VERSION_H = REPO_ROOT / "src" / "ota" / "firmware_version.h"
PARTITIONS_CSV = REPO_ROOT / "partitions.csv"
BUILD_BIN = REPO_ROOT / ".pio" / "build" / "morse_buddy" / "firmware.bin"
OUTPUT_DIR = REPO_ROOT / "ota_release"

# Manifest path prefix served under OtaConfig::kBaseUrl (ota_config.h).
# Kept as one constant here so a real deployment only needs to edit it in
# one place if the hosting layout ever changes. Matches the real host:
# https://sarawutekcrane.github.io/morse-buddy-ota/releases/<version>/...
RELEASE_PATH_PREFIX = "/morse-buddy-ota/releases"

# Phase 5 section 6: warn strongly above this fraction of the OTA slot.
WARN_THRESHOLD_FRACTION = 0.85


def die(message: str) -> None:
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def parse_firmware_version_h() -> tuple[str, int, str]:
    if not FIRMWARE_VERSION_H.exists():
        die(f"{FIRMWARE_VERSION_H} not found")
    text = FIRMWARE_VERSION_H.read_text(encoding="utf-8")

    version_match = re.search(r'FW_VERSION\s*=\s*"([^"]+)"', text)
    build_match = re.search(r"FW_BUILD_NUMBER\s*=\s*(\d+)", text)
    hardware_match = re.search(r'FW_HARDWARE_ID\s*=\s*"([^"]+)"', text)
    if not (version_match and build_match and hardware_match):
        die(f"could not parse FW_VERSION/FW_BUILD_NUMBER/FW_HARDWARE_ID out of {FIRMWARE_VERSION_H}")

    return version_match.group(1), int(build_match.group(1)), hardware_match.group(1)


def parse_ota_slot_size() -> int:
    if not PARTITIONS_CSV.exists():
        die(f"{PARTITIONS_CSV} not found")
    for line in PARTITIONS_CSV.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) >= 5 and fields[0] == "ota_0":
            size_field = fields[4]
            return int(size_field, 16) if size_field.lower().startswith("0x") else int(size_field)
    die(f"no ota_0 row found in {PARTITIONS_CSV}")
    return 0  # unreachable, keeps type checkers happy


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> None:
    version, build, hardware = parse_firmware_version_h()
    ota_slot_size = parse_ota_slot_size()

    if not BUILD_BIN.exists():
        die(f"{BUILD_BIN} not found -- run `pio run` first")

    firmware_size = BUILD_BIN.stat().st_size

    if firmware_size > ota_slot_size:
        die(
            f"firmware.bin is {firmware_size} bytes, which exceeds the OTA slot size "
            f"({ota_slot_size} bytes / 0x{ota_slot_size:X}). This release cannot be packaged."
        )

    usage_fraction = firmware_size / ota_slot_size
    if usage_fraction >= WARN_THRESHOLD_FRACTION:
        print(
            f"WARNING: firmware.bin uses {usage_fraction * 100:.1f}% of the OTA slot "
            f"({firmware_size}/{ota_slot_size} bytes) -- at or above the {WARN_THRESHOLD_FRACTION * 100:.0f}% headroom threshold.",
            file=sys.stderr,
        )

    digest = sha256_of(BUILD_BIN)

    OUTPUT_DIR.mkdir(exist_ok=True)
    release_filename = f"firmware-{version}-build{build}.bin"
    release_bin_path = OUTPUT_DIR / release_filename
    release_bin_path.write_bytes(BUILD_BIN.read_bytes())

    manifest_path_field = f"{RELEASE_PATH_PREFIX}/{version}/{release_filename}"
    manifest_text = (
        "MBOTA1\n"
        f"version={version}\n"
        f"build={build}\n"
        f"hardware={hardware}\n"
        f"size={firmware_size}\n"
        f"sha256={digest}\n"
        f"path={manifest_path_field}\n"
    )
    manifest_out_path = OUTPUT_DIR / "manifest.txt"
    manifest_out_path.write_text(manifest_text, encoding="utf-8")

    print("Version:            ", version)
    print("Build:              ", build)
    print("Hardware:           ", hardware)
    print("Size:               ", f"{firmware_size} bytes ({usage_fraction * 100:.1f}% of {ota_slot_size}-byte OTA slot)")
    print("SHA-256:            ", digest)
    print("Firmware filename:  ", release_bin_path)
    print("Manifest filename:  ", manifest_out_path)
    print()
    print("Publish order (Phase 5 section 38 -- never publish the manifest first):")
    print(f"  1. Upload {release_filename} to your HTTPS host under {manifest_path_field}")
    print(f"  2. Verify that URL is reachable")
    print(f"  3. Only then upload/replace manifest.txt at OtaConfig::kManifestPath")


if __name__ == "__main__":
    main()
