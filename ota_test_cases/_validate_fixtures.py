#!/usr/bin/env python3
"""Validates ota_test_cases/*/manifest.txt fixtures by re-implementing the
exact algorithm in src/ota/ota_manifest.cpp::parse() (Phase 5) and running
it against every fixture, plus basic ASCII/size checks. This is a
line-for-line port, not an approximation -- see the source file for the
authoritative implementation this must stay in sync with.

Not part of the firmware build. Local audit tool only; not published.
"""
import re
import sys
from pathlib import Path

MAX_MANIFEST_SIZE = 2048  # OtaConfig::kManifestMaxSize
MAX_LINE_LEN = 200        # kMaxLineLen
VERSION_LEN = 32          # kVersionLen (buffer capacity; value must be < this)
HARDWARE_LEN = 48         # kHardwareLen
SHA_TMP_LEN = 16          # numeric temp buffer used for build=/size= (copyBounded target)
PATH_LEN = 160            # kPathLen

MANDATORY = {"version", "build", "hardware", "size", "sha256", "path"}


def parse_strict_u32(value: str):
    if value == "" or not value.isdigit():
        return None
    v = int(value)
    if v > 0xFFFFFFFF:
        return None
    return v


def is_hex64(value: str) -> bool:
    return len(value) == 64 and re.fullmatch(r"[0-9a-fA-F]{64}", value) is not None


def path_looks_safe(value: str) -> bool:
    if not value.startswith("/"):
        return False
    if ".." in value:
        return False
    if "://" in value:
        return False
    return True


def parse(raw: bytes):
    """Returns (ParseResult_str, manifest_dict_or_None)."""
    if len(raw) == 0:
        return "BAD_MAGIC", None
    if len(raw) > MAX_MANIFEST_SIZE:
        return "TOO_LARGE", None

    seen = set()
    out = {}
    pos = 0
    first_line = True
    n = len(raw)

    while pos < n:
        line_start = pos
        while pos < n and raw[pos:pos + 1] != b"\n":
            pos += 1
        line_len = pos - line_start
        if pos < n:
            pos += 1  # consume '\n'

        if line_len > 0 and raw[line_start + line_len - 1:line_start + line_len] == b"\r":
            line_len -= 1
        if line_len == 0:
            continue
        if line_len > MAX_LINE_LEN:
            return "FIELD_TOO_LONG", None

        line = raw[line_start:line_start + line_len].decode("ascii", errors="replace")

        if first_line:
            first_line = False
            if not line.startswith("MBOTA"):
                return "BAD_MAGIC", None
            if line != "MBOTA1":
                return "UNSUPPORTED_SCHEMA", None
            continue

        if "=" not in line:
            continue  # unrecognized line: ignored (forward compatibility)
        key, value = line.split("=", 1)

        if key == "version":
            if "version" in seen:
                return "DUPLICATE_FIELD", None
            if len(value) >= VERSION_LEN:
                return "FIELD_TOO_LONG", None
            out["version"] = value
            seen.add("version")
        elif key == "build":
            if "build" in seen:
                return "DUPLICATE_FIELD", None
            if len(value) >= SHA_TMP_LEN:
                return "MALFORMED_NUMBER", None
            v = parse_strict_u32(value)
            if v is None:
                return "MALFORMED_NUMBER", None
            out["build"] = v
            seen.add("build")
        elif key == "hardware":
            if "hardware" in seen:
                return "DUPLICATE_FIELD", None
            if len(value) >= HARDWARE_LEN:
                return "FIELD_TOO_LONG", None
            out["hardware"] = value
            seen.add("hardware")
        elif key == "size":
            if "size" in seen:
                return "DUPLICATE_FIELD", None
            if len(value) >= SHA_TMP_LEN:
                return "MALFORMED_NUMBER", None
            v = parse_strict_u32(value)
            if v is None:
                return "MALFORMED_NUMBER", None
            if v == 0:
                return "MALFORMED_NUMBER", None
            out["size"] = v
            seen.add("size")
        elif key == "sha256":
            if "sha256" in seen:
                return "DUPLICATE_FIELD", None
            if len(value) >= 65:
                return "MALFORMED_SHA256", None
            if not is_hex64(value):
                return "MALFORMED_SHA256", None
            out["sha256"] = value
            seen.add("sha256")
        elif key == "path":
            if "path" in seen:
                return "DUPLICATE_FIELD", None
            if len(value) >= PATH_LEN:
                return "BAD_PATH", None
            if not path_looks_safe(value):
                return "BAD_PATH", None
            out["path"] = value
            seen.add("path")
        # else: unknown non-critical field, ignored

    if first_line:
        return "BAD_MAGIC", None
    if seen != MANDATORY:
        return "MISSING_FIELD", None
    return "OK", out


CASES = {
    "01_valid_build2": "OK",
    "02_wrong_sha": "OK",  # parser accepts it; rejection happens later at SHA compare after download
    "03_wrong_hardware": "OK",  # parser accepts it; rejection happens later in ota_manager.cpp's hardware strcmp
    "04_missing_required_field": "MISSING_FIELD",
    "05_duplicate_required_field": "DUPLICATE_FIELD",
    "06_malformed_sha": "MALFORMED_SHA256",
    "07_zero_size": "MALFORMED_NUMBER",
    "08_oversize": "OK",  # parser accepts it; rejection happens later in downloadAndInstall's partition-size check
    "09_bad_path_parent": "BAD_PATH",
    "10_bad_path_absolute_url": "BAD_PATH",
    "11_same_build": "OK",  # parser accepts it; rejection (UP_TO_DATE) is in ota_manager.cpp's build<=current check
    "12_downgrade": "OK",   # same manifest/parse outcome as 11; differs only by device's running build
    "13_firmware_404": "OK",  # parser accepts it; the 404 only happens on the real firmware GET
    "14_content_length_mismatch": "OK",  # parser accepts it; mismatch only detected against real HTTP Content-Length
    "15_valid_build3": "OK",
    "16_unsupported_schema": "UNSUPPORTED_SCHEMA",
    "17_oversized_manifest_line": "FIELD_TOO_LONG",
}


def main():
    base = Path(__file__).parent
    all_ok = True
    for case_id, expected in CASES.items():
        path = base / case_id / "manifest.txt"
        if not path.exists():
            print(f"[MISSING FILE] {case_id}")
            all_ok = False
            continue
        raw = path.read_bytes()

        is_ascii = all(b < 128 for b in raw)
        size_ok = len(raw) <= MAX_MANIFEST_SIZE
        result, manifest = parse(raw)

        status = "PASS" if result == expected else "FAIL"
        if status == "FAIL":
            all_ok = False
        print(f"{status:5s} {case_id:32s} bytes={len(raw):4d} ascii={is_ascii!s:5s} "
              f"<=2048={size_ok!s:5s} got={result:18s} expected={expected}")

    print()
    print("ALL FIXTURES MATCH EXPECTED PARSE RESULT" if all_ok else "MISMATCH FOUND -- see FAIL lines above")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
