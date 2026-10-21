#!/usr/bin/env bash
# SIMULATION SETUP (test only, not hardware). Prepares WORKDIR to run the
# acceptance plan's Bash blocks against simulated esptool/pio and devices.
# Usage: BUILD_DIR=<dir with bootloader.bin and partitions.bin from `pio run -e morse_buddy`> ./setup_sim.sh WORKDIR
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$(cd "$H/../../.." && pwd)}
BUILD_DIR=${BUILD_DIR:-$REPO/.pio/build/morse_buddy}
PLAN=${PLAN:-$REPO/hardware_test/RACE_R3_R5_ACCEPTANCE_PLAN_R7.3.md}
BOOT_APP0=~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
W=${1:?usage: setup_sim.sh WORKDIR}
[ ! -e "$W" ] || { echo "STOP: $W exists"; exit 1; }
mkdir -p "$W/bin" "$W/fw" "$W/dev"
cp "$H/fake_pio.py" "$W/bin/pio"; chmod +x "$W/bin/pio"
cp "$H/run_sim.sh" "$W/"
cp "$BUILD_DIR/bootloader.bin" "$BUILD_DIR/partitions.bin" "$BOOT_APP0" "$W/fw/"
# Three simulated 4 MB devices with random contents and the project's partition
# table: A and B, and one whose table differs (an "old layout").
python3 - "$W" <<'PY'
import sys, os
W = sys.argv[1]; pt = open(W + "/fw/partitions.bin", "rb").read()
for port, mac, old in (("pA", "24:0a:c4:00:00:0a", False), ("pB", "24:0a:c4:00:00:0b", False), ("pOld", "24:0a:c4:00:00:0c", True)):
    d = bytearray(os.urandom(0x400000)); p = bytearray(pt + b"\xff" * (4096 - len(pt)))
    if old: p[0x30] ^= 0xff
    d[0x8000:0x9000] = p
    open(f"{W}/dev/{port}.img", "wb").write(d); open(f"{W}/dev/{port}.mac", "w").write(mac)
PY
# A clone at this repository's HEAD (the commit under test) as branch simcand.
git clone -q "$REPO" "$W/repo"
git -C "$W/repo" checkout -q -b simcand "$(git -C "$REPO" rev-parse HEAD)"
# The plan's six Bash blocks, extracted unchanged.
python3 - "$PLAN" "$W" <<'PY'
import sys, re
s = open(sys.argv[1]).read(); b = re.findall(r"```bash\n(.*?)```", s, re.S)
assert len(b) == 6, len(b)
for n, x in zip(["build", "backup", "install", "monitor", "rollback92", "restore93"], b):
    open(f"{sys.argv[2]}/{n}.sh", "w").write(x)
PY
echo "ready: bash $W/run_sim.sh"
